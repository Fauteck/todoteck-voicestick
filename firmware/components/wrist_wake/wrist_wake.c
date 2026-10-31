#include "wrist_wake.h"

#include <math.h>
#include <stdbool.h>

#include "bmi270.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "stick_s3_board.h"
#include "voice_board.h"

static const char *TAG = "wrist_wake";

/*
 * ── Was hier gemessen wird, und warum so ────────────────────────────────────
 *
 * Eine Uhr wird nicht "bewegt", sie wird **angehoben und dann stillgehalten**.
 * Genau das ist die Erkennung: erst eine Bewegung, dann Ruhe, und zwischen der
 * Lage davor und der Lage danach ein deutlicher Winkelunterschied. Alle drei
 * Bedingungen zusammen — sonst weckt jeder Schritt beim Gehen den Schirm.
 *
 * Gemessen wird nur die Beschleunigung, also die Richtung der Schwerkraft im
 * Geraet. Das reicht fuer "die Lage hat sich geaendert" und braucht keine
 * Annahme darueber, welche Achse aus dem Display zeigt — die stuende hier
 * sonst als geratene Zahl, die niemand nachpruefen kann, solange die Firmware
 * auf keinem echten Stick lief.
 *
 * ── Warum abfragen und nicht wecken lassen ──────────────────────────────────
 *
 * Der BMI270 koennte den Anhebe-Moment selbst erkennen (er bringt
 * "wrist wear wake-up" als Funktion mit) und ueber seine Interrupt-Leitung
 * melden. Die M5-Dokumentation fuehrt die Leitung als "G4 (PYG4_IMU_INT ueber
 * M5PM1)" — ob sie damit an einem Pin haengt, der aus dem Tiefschlaf weckt,
 * oder am Power-Management-Baustein endet, entscheidet sich am Geraet und
 * nicht hier. Bis das jemand gemessen hat, waere ein zweiter Weckgrund im
 * Tiefschlaf eine Behauptung. Also: abfragen, solange das Geraet ohnehin wach
 * ist (zwischen Abdunkeln und Tiefschlaf), und aus dem Tiefschlaf weiter nur
 * per Taste.
 */

/* Alle 100 ms ein Wert: fein genug fuer eine Handbewegung, grob genug fuers Budget. */
#define SAMPLE_PERIOD_MS 100
/* Ab hier gilt es als Bewegung (in g, ueber die Differenz zweier Messungen). */
#define MOVE_THRESHOLD_G 0.12f
/* Darunter gilt es als still. */
#define QUIET_THRESHOLD_G 0.05f
/* So viele stille Messungen in Folge beenden die Bewegung (also 300 ms). */
#define QUIET_SAMPLES 3
/* So viel Winkel muss zwischen der Lage davor und danach liegen. */
#define TILT_MIN_DEG 20.0f
/*
 * Obergrenze fuer die Dauer einer Bewegung. Wer laeuft, erzeugt eine
 * Bewegung, die nie endet; nach zwei Sekunden ohne Ruhe wird die
 * Ausgangslage neu genommen und von vorn gemessen.
 */
#define MOVE_TIMEOUT_SAMPLES 20

typedef struct {
    float x, y, z;
} vec3_t;

/*
 * Der Sensor hinter der Geste. Der Algorithmus unten ist fuer beide Geraete
 * derselbe; was wechselt, sind vier Handgriffe: anlegen, Messung an und aus,
 * Beschleunigung in g lesen.
 *
 *   StickS3   BMI270 (0x68) ueber den Espressif-Treiber.
 *   Uhr       QMI8658 (0x6B, WHO_AM_I 0x05), direkt ueber vier Register --
 *             fuer die Beschleunigung allein braucht es keinen Treiber.
 */
#if CONFIG_VOICE_BOARD_WATCH206
#define QMI_ADDR 0x6B
#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1 0x02
#define QMI_CTRL2 0x03
#define QMI_CTRL7 0x08
#define QMI_AX_L 0x35
/* +/-2 g, Low-Power-Modus mit 21 Hz: reicht fuer eine Abfrage alle 100 ms. */
#define QMI_CTRL2_2G_LP21 0x0D
#define QMI_LSB_PER_G 16384.0f

static i2c_master_dev_handle_t s_qmi;
/* Nur ein Zeiger als "ist da"-Marke, damit der Rest der Datei gleich bleibt. */
static void *s_sensor;

static esp_err_t qmi_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_qmi, buf, sizeof(buf), 100);
}

static esp_err_t qmi_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_qmi, &reg, 1, data, len, 100);
}
#else
static bmi270_handle_t *s_sensor;
#endif
static wrist_wake_cb_t s_on_raise;
static volatile bool s_watching;
static volatile bool s_measuring;

static float vec_len(vec3_t v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static float vec_diff(vec3_t a, vec3_t b)
{
    const vec3_t d = { a.x - b.x, a.y - b.y, a.z - b.z };
    return vec_len(d);
}

/* Winkel zwischen zwei Richtungen in Grad. 0, wenn eine davon keine ist. */
static float angle_between_deg(vec3_t a, vec3_t b)
{
    const float la = vec_len(a);
    const float lb = vec_len(b);
    if (la < 0.01f || lb < 0.01f) {
        return 0.0f;
    }
    float cosine = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    if (cosine > 1.0f) {
        cosine = 1.0f;
    } else if (cosine < -1.0f) {
        cosine = -1.0f;
    }
    return acosf(cosine) * 57.29578f; /* 180/pi */
}

/*
 * Messung an oder aus.
 *
 * Der Treiber schaltet mit `bmi270_start` immer Beschleunigung *und*
 * Drehrate ein — die Drehrate zieht den Loewenanteil des Stroms und wird hier
 * nie gelesen. Getrennt schalten kann seine Schnittstelle nicht, deshalb
 * laeuft die Messung nur, solange wirklich beobachtet wird, und nicht
 * dauerhaft.
 */
static void set_measuring(bool on)
{
    if (!s_sensor || s_measuring == on) {
        return;
    }
#if CONFIG_VOICE_BOARD_WATCH206
    if (qmi_write(QMI_CTRL7, on ? 0x01 : 0x00) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 laesst sich nicht schalten");
        return;
    }
#else
    if (on) {
        const bmi270_config_t config = {
            .acce_odr = BMI270_ACC_ODR_25_HZ,
            .acce_range = BMI270_ACC_RANGE_2_G,
            .gyro_odr = BMI270_GYR_ODR_25_HZ,
            .gyro_range = BMI270_GYR_RANGE_2000_DPS,
        };
        if (bmi270_start(s_sensor, &config) != ESP_OK) {
            ESP_LOGW(TAG, "Messung liess sich nicht starten");
            return;
        }
        (void)bmi270_set_acce_filter_perf(s_sensor, BMI270_POWER_OPTIMIZED);
    } else {
        (void)bmi270_stop(s_sensor);
    }
#endif
    s_measuring = on;
}

static bool read_acce(vec3_t *out)
{
#if CONFIG_VOICE_BOARD_WATCH206
    uint8_t d[6];
    if (qmi_read(QMI_AX_L, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    out->x = (float)(int16_t)((d[1] << 8) | d[0]) / QMI_LSB_PER_G;
    out->y = (float)(int16_t)((d[3] << 8) | d[2]) / QMI_LSB_PER_G;
    out->z = (float)(int16_t)((d[5] << 8) | d[4]) / QMI_LSB_PER_G;
    return true;
#else
    return bmi270_get_acce_data(s_sensor, &out->x, &out->y, &out->z) == ESP_OK;
#endif
}

#if CONFIG_VOICE_BOARD_WATCH206
static esp_err_t sensor_create(void)
{
    i2c_master_bus_handle_t bus = voice_board_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_qmi);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t who = 0;
    err = qmi_read(QMI_WHO_AM_I, &who, 1);
    if (err == ESP_OK && who != 0x05) {
        err = ESP_ERR_NOT_FOUND;
    }
    /* Adressen fortlaufend, Little Endian; Beschleunigung einstellen, noch aus. */
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL1, 0x60);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL2, QMI_CTRL2_2G_LP21);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL7, 0x00);
    }
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_qmi);
        s_qmi = NULL;
        return err;
    }
    s_sensor = s_qmi;
    return ESP_OK;
}
#endif

static void wrist_task(void *arg)
{
    (void)arg;

    bool moving = false;
    int quiet_run = 0;
    int move_run = 0;
    vec3_t previous = { 0 };
    vec3_t before_move = { 0 };
    bool have_previous = false;

    while (true) {
        if (!s_watching) {
            set_measuring(false);
            have_previous = false;
            moving = false;
            /* Nichts zu tun: warten, bis der Schirm wieder dunkel ist. */
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        set_measuring(true);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));

        vec3_t now;
        if (!read_acce(&now)) {
            continue;
        }
        if (!have_previous) {
            previous = now;
            before_move = now;
            have_previous = true;
            continue;
        }

        const float delta = vec_diff(now, previous);
        previous = now;

        if (!moving) {
            if (delta > MOVE_THRESHOLD_G) {
                moving = true;
                quiet_run = 0;
                move_run = 0;
            } else {
                /* Ruhige Lage: sie ist der Vergleichspunkt fuer die naechste Bewegung. */
                before_move = now;
            }
            continue;
        }

        if (++move_run > MOVE_TIMEOUT_SAMPLES) {
            /* Dauerbewegung (Gehen, Fahrt): neu ansetzen statt irgendwann zu feuern. */
            moving = false;
            before_move = now;
            continue;
        }

        if (delta > QUIET_THRESHOLD_G) {
            quiet_run = 0;
            continue;
        }
        if (++quiet_run < QUIET_SAMPLES) {
            continue;
        }

        moving = false;
        const float tilt = angle_between_deg(before_move, now);
        before_move = now;
        if (tilt < TILT_MIN_DEG) {
            continue;
        }

        ESP_LOGI(TAG, "Handgelenk angehoben (%.0f Grad)", tilt);
        if (s_on_raise) {
            s_on_raise();
        }
    }
}

esp_err_t wrist_wake_init(wrist_wake_cb_t on_raise)
{
#if CONFIG_VOICE_BOARD_WATCH206
    esp_err_t err = sensor_create();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 nicht ansprechbar (%s) — Handgelenk-Geste entfaellt",
                 esp_err_to_name(err));
        s_sensor = NULL;
        return err;
    }
#else
    i2c_master_bus_handle_t bus = stick_s3_board_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    const bmi270_driver_config_t config = {
        .addr = BMI270_I2C_ADDRESS_L,
        .interface = BMI270_USE_I2C,
        .i2c_bus = bus,
    };
    esp_err_t err = bmi270_create(&config, &s_sensor);
    if (err != ESP_OK) {
        /*
         * Kein ESP_ERROR_CHECK: Ohne Sensor faellt die Geste weg, sonst
         * nichts. Ein Sprachgeraet, das wegen einer fehlenden Uhrenfunktion
         * nicht startet, waere die schlechtere Fassung.
         */
        ESP_LOGW(TAG, "BMI270 nicht ansprechbar (%s) — Handgelenk-Geste entfaellt",
                 esp_err_to_name(err));
        s_sensor = NULL;
        return err;
    }
#endif

    s_on_raise = on_raise;

    BaseType_t ok = xTaskCreate(wrist_task, "wrist", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Task liess sich nicht anlegen — Handgelenk-Geste entfaellt");
#if !CONFIG_VOICE_BOARD_WATCH206
        bmi270_delete(s_sensor);
#endif
        s_sensor = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Bewegungssensor bereit");
    return ESP_OK;
}

bool wrist_wake_available(void)
{
    return s_sensor != NULL;
}

void wrist_wake_watch(bool enabled)
{
    s_watching = enabled && s_sensor != NULL;
}

void wrist_wake_prepare_deep_sleep(void)
{
    s_watching = false;
    set_measuring(false);
}
