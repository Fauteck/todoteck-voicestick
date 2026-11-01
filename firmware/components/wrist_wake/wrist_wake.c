#include "wrist_wake.h"

#include <math.h>
#include <stdbool.h>

#include "bmi270.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "stick_s3_board.h"
#include "voice_board.h"

#if CONFIG_VOICE_BOARD_WATCH206
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#endif

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
 * ── Zwei Wege zum selben Ergebnis ───────────────────────────────────────────
 *
 * StickS3 (BMI270): abfragen, alle 100 ms, solange der Schirm dunkel ist. Der
 * BMI270 koennte den Anhebe-Moment selbst erkennen und ueber seine
 * Interrupt-Leitung melden; die M5-Dokumentation fuehrt die Leitung als "G4
 * (PYG4_IMU_INT ueber M5PM1)" — ob sie an einem Pin des Chips endet, hat
 * niemand gemessen. Also: abfragen, und aus dem Tiefschlaf weiter nur per
 * Taste. Auf dem StickS3 ist die Geste ohnehin stillgelegt (main.c).
 *
 * Uhr (QMI8658, Paket H, Punkt 2): nicht mehr abfragen, sondern wecken
 * lassen. Der Sensor laeuft mit "Wake on Motion" (WoM): Beschleunigung im
 * Low-Power-Modus mit 21 Hz, Gyroskop aus, und sobald sich die Beschleunigung
 * um mehr als die Schwelle aendert, zieht er INT1 (GPIO21) nach unten. Erst
 * dann wacht die Task auf, misst mit ~25 Hz (31 Hz Low-Power, alle 40 ms ein
 * Wert) bis zur Ruhe und entscheidet wie bisher: Winkel zwischen der Ruhelage
 * davor und der danach. Dazwischen schlaeft der Chip, statt alle 100 ms fuer
 * eine I2C-Abfrage aufzuwachen — die Leitung weckt ihn auch aus dem Light
 * Sleep (gpio_wakeup_enable).
 *
 * Aus dem Tiefschlaf weckt der Sensor **nicht** (Entscheidung Paket H): GPIO21
 * ist zwar ein RTC-Pin und koennte in die ext1-Maske, aber ein Neustart kann
 * ein Anheben nicht von einem Schritt unterscheiden, ohne erst 300 ms zu
 * messen -- jede Bewegung im Schlaf wuerde einen vollen Start samt Schirm und
 * neuer Bluetooth-Verbindung kosten. Vor dem Tiefschlaf geht der Sensor aus;
 * aus ihm holen weiter nur die Tasten.
 */

/* Ab hier gilt es als Bewegung (in g, ueber die Differenz zweier Messungen). */
#define MOVE_THRESHOLD_G 0.12f
/* Darunter gilt es als still. */
#define QUIET_THRESHOLD_G 0.05f
/* So viel Ruhe in Folge beendet die Bewegung. */
#define QUIET_MS 300
/*
 * Obergrenze fuer die Dauer einer Bewegung. Wer laeuft, erzeugt eine
 * Bewegung, die nie endet; nach zwei Sekunden ohne Ruhe wird die
 * Ausgangslage neu genommen und von vorn gemessen.
 */
#define MOVE_TIMEOUT_MS 2000
/* So viel Winkel muss zwischen der Lage davor und danach liegen. */
#define TILT_MIN_DEG 20.0f

typedef struct {
    float x, y, z;
} vec3_t;

static wrist_wake_cb_t s_on_raise;
static volatile bool s_watching;

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

static void report_raise(float tilt)
{
    if (tilt < TILT_MIN_DEG) {
        return;
    }
    ESP_LOGI(TAG, "Handgelenk angehoben (%.0f Grad)", tilt);
    if (s_on_raise) {
        s_on_raise();
    }
}

#if CONFIG_VOICE_BOARD_WATCH206
/* ── Uhr: QMI8658 mit Wake on Motion ─────────────────────────────────────── */

#define QMI_ADDR 0x6B
#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1 0x02
#define QMI_CTRL2 0x03
#define QMI_CTRL7 0x08
#define QMI_CTRL8 0x09
#define QMI_CTRL9 0x0A
#define QMI_CAL1_L 0x0B
#define QMI_CAL1_H 0x0C
#define QMI_STATUSINT 0x2D
#define QMI_STATUS1 0x2F
#define QMI_AX_L 0x35

/*
 * CTRL1 (0x02) laut Registerbeschreibung des QMI8658A:
 *   Bit 7  SIM            SPI 3-Draht (hier I2C: 0)
 *   Bit 6  ADDR_AI        Adresse zaehlt beim Lesen mehrerer Bytes weiter
 *   Bit 5  BE             Big Endian in den Datenregistern
 *   Bit 4  INT2_EN        INT2 als Ausgang
 *   Bit 3  INT1_EN        INT1 als Ausgang
 *   Bit 2  FIFO_INT_SEL   FIFO-Interrupt auf INT1 statt INT2
 *   Bit 0  SensorDisable  interne 2-MHz-Versorgung aus
 * Bis 0.4.1 stand hier 0x60 | 0x10: Bit 5 schaltete Big Endian, read_acce
 * setzt die Bytes aber Little Endian zusammen (Low zuerst), und 0x10 machte
 * INT2 zum Ausgang -- INT1, an dem GPIO21 haengt, blieb hochohmig. Richtig:
 * nur ADDR_AI (0x40) und INT1_EN (0x08).
 */
#define QMI_CTRL1_BASE 0x40
#define QMI_CTRL1_INT1_EN 0x08
/* CTRL8 Bit 7: CTRL9-Handshake ueber STATUSINT.7 statt ueber die INT1-Leitung. */
#define QMI_CTRL8_HANDSHAKE_STATUSINT 0x80
/*
 * CTRL2: +/-2 g (Bits 6..4 = 000) und der aODR-Code in Bits 3..0. Die
 * Low-Power-Codes laut Tabelle des QMI8658A: 1100 = 128 Hz, 1101 = 58 Hz,
 * 1110 = 31 Hz, 1111 = 21 Hz. (Bis 0.4.0 stand hier 0x0D mit dem Kommentar
 * "21 Hz"; das waren 58.)
 */
#define QMI_CTRL2_2G_LP31 0x0E
#define QMI_CTRL2_2G_LP21 0x0F
#define QMI_CTRL7_ACC_ON 0x01
#define QMI_CTRL7_OFF 0x00
#define QMI_CTRL9_CMD_ACK 0x00
#define QMI_CTRL9_CMD_WRITE_WOM 0x08
#define QMI_STATUSINT_CMD_DONE 0x80
#define QMI_LSB_PER_G 16384.0f

/* WoM-Schwelle in mg: dieselben 0,12 g wie MOVE_THRESHOLD_G. */
#define WOM_THRESHOLD_MG 120
/*
 * CAL1_H: Bits 7..6 waehlen die Leitung und ihren Ruhepegel -- 11 heisst
 * INT1 mit Ruhepegel 1, bei Bewegung geht sie auf 0. So weckt sie als
 * fallende Flanke beziehungsweise als Low-Pegel im Light Sleep. Bits 5..0
 * sind die Ausblendzeit nach dem Einschalten, in Proben.
 */
#define WOM_CAL1_H (0xC0 | 0x04)
/* Der Interrupt-Pin laut Schaltplan (README, Hardware der Uhr). */
#define WOM_INT_GPIO GPIO_NUM_21

/* Messtakt beim Entscheiden: 40 ms je Wert bei 31 Hz Low-Power. */
#define MEASURE_PERIOD_MS 40
#define QUIET_SAMPLES (QUIET_MS / MEASURE_PERIOD_MS)
#define MOVE_TIMEOUT_SAMPLES (MOVE_TIMEOUT_MS / MEASURE_PERIOD_MS)

static i2c_master_dev_handle_t s_qmi;
static SemaphoreHandle_t s_kick;
static volatile bool s_int_fired;
static bool s_armed;
/* Die Ruhelage, gegen die das naechste Anheben gemessen wird. */
static vec3_t s_rest;
static bool s_have_rest;

static esp_err_t qmi_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_qmi, buf, sizeof(buf), 100);
}

static esp_err_t qmi_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_qmi, &reg, 1, data, len, 100);
}

static bool read_acce(vec3_t *out)
{
    uint8_t d[6];
    if (qmi_read(QMI_AX_L, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    out->x = (float)(int16_t)((d[1] << 8) | d[0]) / QMI_LSB_PER_G;
    out->y = (float)(int16_t)((d[3] << 8) | d[2]) / QMI_LSB_PER_G;
    out->z = (float)(int16_t)((d[5] << 8) | d[4]) / QMI_LSB_PER_G;
    return true;
}

/*
 * CTRL9-Befehl mit Handshake: Befehl schreiben, auf CmdDone warten,
 * quittieren, warten, bis CmdDone wieder weg ist. Laeuft der Handshake nicht
 * durch, wird das gemeldet und trotzdem weitergemacht -- der Sensor nimmt
 * den Befehl in der Regel auch dann.
 */
static esp_err_t qmi_ctrl9(uint8_t cmd)
{
    esp_err_t err = qmi_write(QMI_CTRL9, cmd);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t st = 0;
    int tries = 0;
    for (; tries < 20; tries++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (qmi_read(QMI_STATUSINT, &st, 1) == ESP_OK && (st & QMI_STATUSINT_CMD_DONE)) {
            break;
        }
    }
    if (tries == 20) {
        ESP_LOGW(TAG, "QMI8658: CTRL9 0x%02x ohne CmdDone", cmd);
    }
    err = qmi_write(QMI_CTRL9, QMI_CTRL9_CMD_ACK);
    for (tries = 0; tries < 20; tries++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (qmi_read(QMI_STATUSINT, &st, 1) == ESP_OK && !(st & QMI_STATUSINT_CMD_DONE)) {
            break;
        }
    }
    return err;
}

/* WoM scharf: Beschleunigung 21 Hz Low-Power, Schwelle gesetzt, Leitung auf Ruhepegel. */
static esp_err_t wom_arm(void)
{
    esp_err_t err = qmi_write(QMI_CTRL7, QMI_CTRL7_OFF);
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL2, QMI_CTRL2_2G_LP21);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CAL1_L, WOM_THRESHOLD_MG);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CAL1_H, WOM_CAL1_H);
    }
    if (err == ESP_OK) {
        err = qmi_ctrl9(QMI_CTRL9_CMD_WRITE_WOM);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL7, QMI_CTRL7_ACC_ON);
    }
    return err;
}

/* WoM aus (Schwelle 0), Sensor je nach `measure` auf 31 Hz messen oder ganz aus. */
static esp_err_t wom_disarm(bool measure)
{
    esp_err_t err = qmi_write(QMI_CTRL7, QMI_CTRL7_OFF);
    if (err == ESP_OK) {
        err = qmi_write(QMI_CAL1_L, 0);
    }
    if (err == ESP_OK) {
        err = qmi_ctrl9(QMI_CTRL9_CMD_WRITE_WOM);
    }
    if (err == ESP_OK && measure) {
        err = qmi_write(QMI_CTRL2, QMI_CTRL2_2G_LP31);
        if (err == ESP_OK) {
            err = qmi_write(QMI_CTRL7, QMI_CTRL7_ACC_ON);
        }
    }
    return err;
}

/* Das Ereignisbit des Sensors lesen; damit geht INT1 auf den Ruhepegel zurueck. */
static void wom_clear(void)
{
    uint8_t st = 0;
    (void)qmi_read(QMI_STATUS1, &st, 1);
}

static void IRAM_ATTR wom_isr(void *arg)
{
    (void)arg;
    /* Pegel-Interrupt (Light-Sleep-Weckquelle): aus, bis die Task ihn geloescht hat. */
    gpio_intr_disable(WOM_INT_GPIO);
    s_int_fired = true;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_kick, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

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
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL1, QMI_CTRL1_BASE | QMI_CTRL1_INT1_EN);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL8, QMI_CTRL8_HANDSHAKE_STATUSINT);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL2, QMI_CTRL2_2G_LP21);
    }
    if (err == ESP_OK) {
        err = qmi_write(QMI_CTRL7, QMI_CTRL7_OFF);
    }
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_qmi);
        s_qmi = NULL;
    }
    return err;
}

static esp_err_t int_pin_create(void)
{
    /*
     * Der Interrupt wird hier noch NICHT konfiguriert (GPIO_INTR_DISABLE):
     * gpio_config() ruft in IDF 5.5.1 am Ende gpio_intr_enable() -- mit
     * .intr_type = GPIO_INTR_LOW_LEVEL waere der Pegel-Interrupt scharf,
     * bevor gpio_isr_handler_add() den Handler einhaengt. Liegt GPIO21 in
     * dem Moment auf 0 (der Sensor treibt INT1 erst mit scharfem WoM auf
     * Ruhepegel 1, davor floatet die Leitung), feuert der Interrupt nach
     * gpio_install_isr_service() endlos: gpio_isr_loop findet keinen
     * Handler, loescht das Status-Bit, der Pegel bleibt 0, sofort wieder --
     * Interrupt-Watchdog nach 300 ms, Panic, OTA-Rueckfall. So stuerzte
     * 0.4.1 beim Start ab. Die Art des Interrupts setzt gpio_set_intr_type()
     * unten, nachdem der Handler steht und der Interrupt aus ist -- wie
     * init_pmic_irq() in main.c fuer den StickS3.
     *
     * Pull-up, obwohl der Sensor Push-Pull treibt: Der Ruhepegel der
     * WoM-Leitung ist 1, und solange der Sensor sie nicht treibt (vor dem
     * Scharfschalten, im Tiefschlaf), haelt der Pull-up sie oben statt sie
     * schweben zu lassen.
     */
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WOM_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /*
     * Flags 0, nicht ESP_INTR_FLAG_IRAM: wom_isr ruft Code im Flash
     * (gpio_intr_disable, xSemaphoreGiveFromISR). Auf der Uhr ist das der
     * erste Aufruf von gpio_install_isr_service -- Touch und Tasten kommen
     * spaeter und nehmen den Dienst, wie er ist (ESP_ERR_INVALID_STATE).
     */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_isr_handler_add(WOM_INT_GPIO, wom_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    /* Handler steht, Interrupt aus; jetzt die Art setzen. Scharf erst mit watch(true). */
    gpio_intr_disable(WOM_INT_GPIO);
    err = gpio_set_intr_type(WOM_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        return err;
    }
    /*
     * Dieselbe Leitung weckt aus dem Light Sleep: Ohne das wuerde der Chip
     * den Interrupt erst beim naechsten ohnehin faelligen Aufwachen sehen --
     * mit dem langsamen Verbindungsintervall bis zu zwei Sekunden spaeter.
     * Die Tasten (espressif/button, enable_power_save) machen es genauso.
     *
     * Hier nur die Weckquelle "GPIO" freigeben; welcher Pin weckt, setzt
     * arm() mit gpio_wakeup_enable und disarm() nimmt es zurueck. Bis 0.4.1
     * stand gpio_wakeup_enable dauerhaft hier: Solange WoM nicht scharf ist,
     * treibt der Sensor die Leitung nicht und der Pin floatet ohne Pull --
     * ein schwebender Low-Pegel weckt den Chip dann aus jedem Light Sleep
     * sofort wieder auf.
     */
    return esp_sleep_enable_gpio_wakeup();
}

/*
 * Nach dem Wecken: messen, bis die Bewegung zur Ruhe kommt, dann den Winkel
 * gegen die Ruhelage davor nehmen. Die neue Ruhelage ist danach der
 * Vergleichspunkt fuer das naechste Mal. Bricht ab, sobald niemand mehr
 * beobachtet (heller Schirm, Aufnahme, Tiefschlaf).
 */
static void measure_after_wake(void)
{
    vec3_t previous = { 0 };
    bool have_previous = false;
    int quiet_run = 0;
    for (int n = 0; n < MOVE_TIMEOUT_SAMPLES && s_watching; n++) {
        vTaskDelay(pdMS_TO_TICKS(MEASURE_PERIOD_MS));
        vec3_t now;
        if (!read_acce(&now)) {
            continue;
        }
        if (!have_previous) {
            previous = now;
            have_previous = true;
            continue;
        }
        const float delta = vec_diff(now, previous);
        previous = now;
        if (delta > QUIET_THRESHOLD_G) {
            quiet_run = 0;
            continue;
        }
        if (++quiet_run < QUIET_SAMPLES) {
            continue;
        }
        /* Ruhe: entscheiden, und die Lage als neue Ausgangslage merken. */
        const float tilt = s_have_rest ? angle_between_deg(s_rest, now) : 0.0f;
        s_rest = now;
        s_have_rest = true;
        if (s_watching) {
            report_raise(tilt);
        }
        return;
    }
    /* Dauerbewegung (Gehen, Fahrt) oder abgebrochen: die letzte Lage ist die neue Ausgangslage. */
    if (have_previous) {
        s_rest = previous;
        s_have_rest = true;
    }
}

static void arm(void)
{
    if (!s_have_rest) {
        /* Erste Ruhelage: einmal kurz messen, bevor der Sensor nur noch weckt. */
        if (wom_disarm(true) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2 * MEASURE_PERIOD_MS));
            s_have_rest = read_acce(&s_rest);
        }
    }
    if (wom_arm() != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658: WoM laesst sich nicht scharf schalten");
        return;
    }
    wom_clear();
    s_int_fired = false;
    /* Erst jetzt treibt der Sensor die Leitung auf Ruhepegel 1: Weckquelle und Interrupt scharf. */
    (void)gpio_wakeup_enable(WOM_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    gpio_intr_enable(WOM_INT_GPIO);
    s_armed = true;
}

static void disarm(void)
{
    gpio_intr_disable(WOM_INT_GPIO);
    (void)gpio_wakeup_disable(WOM_INT_GPIO);
    (void)wom_disarm(false);
    s_armed = false;
}

static void wrist_task(void *arg)
{
    (void)arg;
    while (true) {
        xSemaphoreTake(s_kick, portMAX_DELAY);
        if (s_watching && !s_armed) {
            arm();
        } else if (!s_watching && s_armed) {
            disarm();
        }
        if (s_armed && s_int_fired) {
            s_int_fired = false;
            if (wom_disarm(true) == ESP_OK) {
                measure_after_wake();
            }
            if (s_watching) {
                arm();
            } else {
                disarm();
            }
        }
    }
}

esp_err_t wrist_wake_init(wrist_wake_cb_t on_raise)
{
    esp_err_t err = sensor_create();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 nicht ansprechbar (%s) — Handgelenk-Geste entfaellt",
                 esp_err_to_name(err));
        return err;
    }
    s_kick = xSemaphoreCreateBinary();
    if (!s_kick) {
        return ESP_ERR_NO_MEM;
    }
    err = int_pin_create();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d als Weckleitung nicht einzurichten (%s) — Handgelenk-Geste entfaellt",
                 (int)WOM_INT_GPIO, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_qmi);
        s_qmi = NULL;
        return err;
    }
    /*
     * Diagnose fuer den Monitor: INT1 ist jetzt Ausgang (CTRL1), WoM noch
     * nicht scharf. Treibt der Sensor die Leitung, steht hier stabil 0 oder 1;
     * floatet sie (falsches CTRL1-Bit, andere Leitung), wechselt der Wert von
     * Start zu Start.
     */
    ESP_LOGI(TAG, "QMI8658 INT1 an GPIO%d: Pegel %d", (int)WOM_INT_GPIO,
             gpio_get_level(WOM_INT_GPIO));
    s_on_raise = on_raise;

    BaseType_t ok = xTaskCreate(wrist_task, "wrist", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Task liess sich nicht anlegen — Handgelenk-Geste entfaellt");
        i2c_master_bus_rm_device(s_qmi);
        s_qmi = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Bewegungssensor bereit (WoM auf GPIO%d)", (int)WOM_INT_GPIO);
    return ESP_OK;
}

bool wrist_wake_available(void)
{
    return s_qmi != NULL;
}

void wrist_wake_watch(bool enabled)
{
    const bool want = enabled && s_qmi != NULL;
    if (want == s_watching) {
        return;
    }
    s_watching = want;
    if (s_kick) {
        xSemaphoreGive(s_kick);
    }
}

void wrist_wake_prepare_deep_sleep(void)
{
    s_watching = false;
    if (!s_qmi) {
        return;
    }
    /*
     * Sofort und nicht ueber die Task: Gleich schlaeft das Geraet. Der
     * I2C-Treiber sperrt den Bus je Uebertragung, ein gleichzeitiger Zugriff
     * der Task ist deshalb unschaedlich.
     */
    gpio_intr_disable(WOM_INT_GPIO);
    (void)gpio_wakeup_disable(WOM_INT_GPIO);
    (void)wom_disarm(false);
    s_armed = false;
}

#else
/* ── StickS3: BMI270, abgefragt ──────────────────────────────────────────── */

/* Alle 100 ms ein Wert: fein genug fuer eine Handbewegung, grob genug fuers Budget. */
#define SAMPLE_PERIOD_MS 100
#define QUIET_SAMPLES (QUIET_MS / SAMPLE_PERIOD_MS)
#define MOVE_TIMEOUT_SAMPLES (MOVE_TIMEOUT_MS / SAMPLE_PERIOD_MS)

static bmi270_handle_t *s_sensor;
static volatile bool s_measuring;

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
    s_measuring = on;
}

static bool read_acce(vec3_t *out)
{
    return bmi270_get_acce_data(s_sensor, &out->x, &out->y, &out->z) == ESP_OK;
}

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
        report_raise(tilt);
    }
}

esp_err_t wrist_wake_init(wrist_wake_cb_t on_raise)
{
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

    s_on_raise = on_raise;

    BaseType_t ok = xTaskCreate(wrist_task, "wrist", 3072, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Task liess sich nicht anlegen — Handgelenk-Geste entfaellt");
        bmi270_delete(s_sensor);
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
#endif
