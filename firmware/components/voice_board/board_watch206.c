#include "voice_board.h"

#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Pins und Adressen stammen aus dem Schaltplan (Revision V1.0) und dem
 * Board-Paket von Waveshare (waveshare/esp32_s3_touch_amoled_2_06 2.0.0), nicht
 * aus einer Messung. Was am Geraet anders ist, wird hier korrigiert -- und
 * nirgends sonst.
 */

static const char *TAG = "watch206_board";

#define I2C_FREQ_HZ 100000
#define I2C_TIMEOUT_MS 100

/* ── AXP2101 ──────────────────────────────────────────────────────────────── */
#define AXP_ADDR 0x34
#define AXP_REG_STATUS1 0x00          /* Bit 5: VBUS gut */
#define AXP_REG_STATUS2 0x01          /* Bit 6..5: 01 laedt, 10 entlaedt */
#define AXP_REG_GAUGE_CTRL 0x18       /* Bit 3: Ladestandsmesser an */
#define AXP_REG_ADC_CTRL 0x30         /* Bit 0..3: Akku, TS, VBUS, System */
#define AXP_REG_BAT_DET_CTRL 0x68     /* Bit 0: Akku erkennen */
#define AXP_REG_BAT_PERCENT 0xA4

/* ── PCF85063 ─────────────────────────────────────────────────────────────── */
#define RTC_ADDR 0x51
#define RTC_REG_CTRL1 0x00
#define RTC_REG_RAM 0x03
#define RTC_REG_SECONDS 0x04          /* Bit 7: Oszillator stand, Zeit ungueltig */
/*
 * Das RAM-Byte traegt den Abstand zur UTC in Viertelstunden (-48..+56 deckt
 * jede Zeitzone der Welt). 0x80 ist "nie gesetzt": So weiss das Geraet nach
 * einem Akkuwechsel, dass es zwar eine laufende Uhr, aber keine Ortszeit hat.
 */
#define RTC_OFFSET_UNSET ((int8_t)0x80)

/* ── FT3168 ───────────────────────────────────────────────────────────────── */
#define TOUCH_ADDR 0x38
#define TOUCH_REG_TD_STATUS 0x02      /* Bit 3..0: Zahl der Finger */
/*
 * Wie lange nach einem Antippen das naechste nicht zaehlt. Ein Finger, der
 * beim Abheben noch einmal kurz aufsetzt, wuerde sonst zwei Seiten im Verlauf
 * weiterblaettern.
 */
#define TOUCH_DEBOUNCE_US (250 * 1000)
#define TOUCH_POLL_MS 20
/* Laenger als das liegt kein Finger auf einem Antippen -- dann ist es der Aermel. */
#define TOUCH_MAX_TAP_MS 800

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_axp_dev;
static i2c_master_dev_handle_t s_rtc_dev;
static i2c_master_dev_handle_t s_touch_dev;
static esp_timer_handle_t s_motor_timer;
static SemaphoreHandle_t s_touch_sem;
static voice_board_tap_cb_t s_tap_cb;

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *data, size_t len)
{
    if (!dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(dev, buf, len + 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_set_bits(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(reg_read(dev, reg, &value, 1), TAG, "read 0x%02x", reg);
    value |= mask;
    return reg_write(dev, reg, &value, 1);
}

static esp_err_t add_device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, dev);
}

/*
 * Der AXP2101 bekommt nur, was zum Messen noetig ist. Die Spannungsschienen
 * bleiben, wie sie ab Werk eingestellt sind -- das Board-Paket von Waveshare
 * fasst sie ebenfalls nicht an, und eine falsch abgeschaltete Schiene nimmt
 * im schlimmsten Fall dem Display oder dem Codec den Strom.
 */
static esp_err_t init_pmic(void)
{
    ESP_RETURN_ON_ERROR(add_device(AXP_ADDR, &s_axp_dev), TAG, "add axp2101");
    ESP_RETURN_ON_ERROR(reg_set_bits(s_axp_dev, AXP_REG_BAT_DET_CTRL, 0x01), TAG,
                        "battery detect");
    ESP_RETURN_ON_ERROR(reg_set_bits(s_axp_dev, AXP_REG_ADC_CTRL, 0x0D), TAG, "adc");
    ESP_RETURN_ON_ERROR(reg_set_bits(s_axp_dev, AXP_REG_GAUGE_CTRL, 0x08), TAG, "gauge");
    return ESP_OK;
}

static esp_err_t init_rtc(void)
{
    ESP_RETURN_ON_ERROR(add_device(RTC_ADDR, &s_rtc_dev), TAG, "add pcf85063");
    uint8_t ctrl1 = 0;
    /* Nur pruefen, ob er antwortet -- die Zeit selbst liest main.c. */
    return reg_read(s_rtc_dev, RTC_REG_CTRL1, &ctrl1, 1);
}

static void motor_off_cb(void *arg)
{
    (void)arg;
    gpio_set_level(VOICE_BOARD_PIN_MOTOR, 0);
}

static esp_err_t init_motor(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VOICE_BOARD_PIN_MOTOR,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "motor gpio");
    gpio_set_level(VOICE_BOARD_PIN_MOTOR, 0);
    const esp_timer_create_args_t timer_args = {
        .callback = motor_off_cb,
        .name = "motor_off",
    };
    return esp_timer_create(&timer_args, &s_motor_timer);
}

esp_err_t voice_board_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = VOICE_BOARD_PIN_I2C_SDA,
        .scl_io_num = VOICE_BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    /* Die Tasten haben eigene Widerstaende auf der Platine; hier nur lesen. */
    const gpio_config_t buttons = {
        .pin_bit_mask = (1ULL << VOICE_BOARD_PIN_BUTTON_TALK) |
                        (1ULL << VOICE_BOARD_PIN_BUTTON_BROWSE),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&buttons), TAG, "buttons");

    /*
     * Ohne Leistungsbaustein laeuft das Geraet trotzdem -- nur ohne
     * Akkustand. Ohne Uhr auch, nur mit "--:--" bis zur ersten Bruecke. Beides
     * ist kein Grund, gar nicht zu starten.
     */
    esp_err_t err = init_pmic();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 nicht bereit: %s", esp_err_to_name(err));
    }
    err = init_rtc();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 nicht bereit: %s", esp_err_to_name(err));
    }
    err = init_motor();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Motor nicht bereit: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

i2c_master_bus_handle_t voice_board_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t voice_board_battery_level(int *level_percent)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_axp_dev, AXP_REG_BAT_PERCENT, &value, 1), TAG, "percent");
    *level_percent = value > 100 ? 100 : value;
    return ESP_OK;
}

esp_err_t voice_board_battery_charging(bool *charging)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_axp_dev, AXP_REG_STATUS2, &value, 1), TAG, "status2");
    *charging = ((value >> 5) & 0x03) == 0x01;
    return ESP_OK;
}

esp_err_t voice_board_usb_powered(bool *usb_powered)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_axp_dev, AXP_REG_STATUS1, &value, 1), TAG, "status1");
    *usb_powered = (value & (1 << 5)) != 0;
    return ESP_OK;
}

esp_err_t voice_board_clear_power_irqs(uint8_t *sys_status)
{
    /* Keine IRQ-Leitung, also nichts zu quittieren; der Akku wird gepollt. */
    *sys_status = 0;
    return ESP_OK;
}

void voice_board_prepare_deep_sleep(void)
{
    if (s_motor_timer) {
        (void)esp_timer_stop(s_motor_timer);
    }
    gpio_set_level(VOICE_BOARD_PIN_MOTOR, 0);
    /*
     * Der Touch-Baustein geht in den Ruhemodus (Register 0xA5 = 3). Wecken
     * kann er im Tiefschlaf ohnehin nicht: GPIO38 ist kein RTC-Pin. Beim
     * Aufwachen holt ihn der Reset-Pin in voice_board_touch_init() zurueck.
     */
    if (s_touch_dev) {
        const uint8_t hibernate = 0x03;
        (void)reg_write(s_touch_dev, 0xA5, &hibernate, 1);
    }
}

bool voice_board_talk_button_pressed(void)
{
    return gpio_get_level(VOICE_BOARD_PIN_BUTTON_TALK) == VOICE_BOARD_BUTTON_TALK_ACTIVE_LEVEL;
}

bool voice_board_browse_button_pressed(void)
{
    return gpio_get_level(VOICE_BOARD_PIN_BUTTON_BROWSE) == VOICE_BOARD_BUTTON_BROWSE_ACTIVE_LEVEL;
}

/* ── Uhr ──────────────────────────────────────────────────────────────────── */

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Tage seit 1970-01-01 fuer ein Datum im gregorianischen Kalender. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

esp_err_t voice_board_rtc_read(int64_t *epoch, int32_t *tz_offset_min)
{
    uint8_t ram = 0;
    uint8_t t[7] = {0};
    ESP_RETURN_ON_ERROR(reg_read(s_rtc_dev, RTC_REG_RAM, &ram, 1), TAG, "rtc ram");
    ESP_RETURN_ON_ERROR(reg_read(s_rtc_dev, RTC_REG_SECONDS, t, sizeof(t)), TAG, "rtc time");
    if ((t[0] & 0x80) || (int8_t)ram == RTC_OFFSET_UNSET) {
        return ESP_ERR_INVALID_STATE;
    }
    const int sec = bcd_to_bin(t[0] & 0x7F);
    const int min = bcd_to_bin(t[1] & 0x7F);
    const int hour = bcd_to_bin(t[2] & 0x3F);
    const int day = bcd_to_bin(t[3] & 0x3F);
    const int month = bcd_to_bin(t[5] & 0x1F);
    const int year = 2000 + bcd_to_bin(t[6]);
    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 2024) {
        return ESP_ERR_INVALID_STATE;
    }
    *epoch = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
             hour * 3600 + min * 60 + sec;
    *tz_offset_min = (int8_t)ram * 15;
    return ESP_OK;
}

esp_err_t voice_board_rtc_write(int64_t epoch, int32_t tz_offset_min)
{
    const time_t now = (time_t)epoch;
    struct tm parts;
    gmtime_r(&now, &parts);
    if (parts.tm_year < 100 || parts.tm_year > 199) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t t[7] = {
        bin_to_bcd((uint8_t)parts.tm_sec),     /* Bit 7 = 0: Oszillator laeuft */
        bin_to_bcd((uint8_t)parts.tm_min),
        bin_to_bcd((uint8_t)parts.tm_hour),
        bin_to_bcd((uint8_t)parts.tm_mday),
        (uint8_t)parts.tm_wday,
        bin_to_bcd((uint8_t)(parts.tm_mon + 1)),
        bin_to_bcd((uint8_t)(parts.tm_year - 100)),
    };
    ESP_RETURN_ON_ERROR(reg_write(s_rtc_dev, RTC_REG_SECONDS, t, sizeof(t)), TAG, "rtc time");
    const int8_t quarters = (int8_t)(tz_offset_min / 15);
    const uint8_t ram = (uint8_t)quarters;
    return reg_write(s_rtc_dev, RTC_REG_RAM, &ram, 1);
}

/* ── Motor ────────────────────────────────────────────────────────────────── */

void voice_board_haptic(uint32_t duration_ms)
{
    if (!s_motor_timer || duration_ms == 0) {
        return;
    }
    (void)esp_timer_stop(s_motor_timer);
    gpio_set_level(VOICE_BOARD_PIN_MOTOR, 1);
    (void)esp_timer_start_once(s_motor_timer, (uint64_t)duration_ms * 1000ULL);
}

/* ── Touch ────────────────────────────────────────────────────────────────── */

static void IRAM_ATTR touch_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(VOICE_BOARD_PIN_TOUCH_INT);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_touch_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static int touch_points(void)
{
    uint8_t status = 0;
    if (reg_read(s_touch_dev, TOUCH_REG_TD_STATUS, &status, 1) != ESP_OK) {
        return -1;
    }
    return status & 0x0F;
}

/*
 * Ein Antippen ist: ein Finger setzt auf und hebt innerhalb von
 * TOUCH_MAX_TAP_MS wieder ab. Gezaehlt wird beim Abheben, nicht beim
 * Aufsetzen -- so loest ein Aermel, der ueber den Schirm streift und liegen
 * bleibt, nichts aus.
 *
 * Gelesen wird ueber I2C erst nach der Unterbrechung; ohne Beruehrung
 * schlaeft die Task also. Ausgewertet wird nur die Fingerzahl, nicht die
 * Lage: Phase 1 kennt auf dem Schirm genau eine Geste.
 */
static void touch_task(void *arg)
{
    (void)arg;
    int64_t last_tap_us = 0;
    while (true) {
        xSemaphoreTake(s_touch_sem, portMAX_DELAY);
        const int64_t down_us = esp_timer_get_time();
        bool seen = false;
        while (true) {
            const int points = touch_points();
            if (points > 0) {
                seen = true;
            } else {
                break;  /* abgehoben -- oder der Baustein antwortet nicht */
            }
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        }
        const int64_t up_us = esp_timer_get_time();
        if (seen && up_us - down_us <= TOUCH_MAX_TAP_MS * 1000LL &&
            up_us - last_tap_us >= TOUCH_DEBOUNCE_US) {
            last_tap_us = up_us;
            if (s_tap_cb) {
                s_tap_cb();
            }
        }
        gpio_intr_enable(VOICE_BOARD_PIN_TOUCH_INT);
    }
}

esp_err_t voice_board_touch_init(voice_board_tap_cb_t cb)
{
    s_tap_cb = cb;

    const gpio_config_t rst = {
        .pin_bit_mask = 1ULL << VOICE_BOARD_PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "touch rst");
    gpio_set_level(VOICE_BOARD_PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(VOICE_BOARD_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(add_device(TOUCH_ADDR, &s_touch_dev), TAG, "add ft3168");
    ESP_RETURN_ON_FALSE(touch_points() >= 0, ESP_ERR_NOT_FOUND, TAG, "ft3168 antwortet nicht");

    s_touch_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_touch_sem, ESP_ERR_NO_MEM, TAG, "touch sem");

    const gpio_config_t irq = {
        .pin_bit_mask = 1ULL << VOICE_BOARD_PIN_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq), TAG, "touch int");
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(VOICE_BOARD_PIN_TOUCH_INT, touch_isr, NULL),
                        TAG, "touch isr");
    BaseType_t ok = xTaskCreate(touch_task, "touch", 3072, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
