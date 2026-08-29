#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define STICK_S3_PIN_BUTTON_FRONT 11
#define STICK_S3_PIN_BUTTON_SIDE  12

/*
 * Welche Taste welche Rolle hat -- und warum nicht die naheliegende.
 *
 * Die vordere ist die grosse blaue auf der Frontplatte, die seitliche sitzt
 * am Rand (im Querformat unten). Bis 08/2026 sprach die vordere und die
 * seitliche brach ab. Am Handgelenk hat sich das umgedreht: Die grosse
 * Flaeche liegt genau dort, wo der Arm an Tischkante, Tuerrahmen oder
 * Jackenaermel stoesst, und startete dabei Aufnahmen, die niemand wollte.
 * Die kleinere Taste an der Kante trifft man nur mit dem Finger.
 *
 * Deshalb sprechen ab hier Rollen, nicht Lagen. Nach aussen aendert sich
 * nichts: Das Protokoll kennt ohnehin nur `primary` (sprechen) und
 * `secondary` (blaettern) -- welcher Pin daran haengt, ist Sache des Geraets.
 */
#define STICK_S3_PIN_BUTTON_TALK   STICK_S3_PIN_BUTTON_SIDE
#define STICK_S3_PIN_BUTTON_BROWSE STICK_S3_PIN_BUTTON_FRONT
#define STICK_S3_PIN_PMIC_IRQ     13

#define STICK_S3_PIN_I2C_SCL 48
#define STICK_S3_PIN_I2C_SDA 47

#define STICK_S3_PIN_ES8311_MCLK 18
// Pin names follow the codec's perspective:
//   ES8311_DIN  = codec serial data input  (DSDIN, MCU -> codec, speaker path) = GPIO14
//   ES8311_DOUT = codec serial data output (ASDOUT, codec -> MCU, mic path)   = GPIO16
#define STICK_S3_PIN_ES8311_BCLK 17
#define STICK_S3_PIN_ES8311_LRCK 15
#define STICK_S3_PIN_ES8311_DIN  14
#define STICK_S3_PIN_ES8311_DOUT 16

#define STICK_S3_PIN_LCD_MOSI 39
#define STICK_S3_PIN_LCD_SCK  40
#define STICK_S3_PIN_LCD_DC   45
#define STICK_S3_PIN_LCD_CS   41
#define STICK_S3_PIN_LCD_RST  21
#define STICK_S3_PIN_LCD_BL   38

esp_err_t stick_s3_board_init(void);
i2c_master_bus_handle_t stick_s3_board_i2c_bus(void);
esp_err_t stick_s3_board_battery_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_vbus_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_battery_level(int *level_percent);
esp_err_t stick_s3_board_battery_charging(bool *charging);
esp_err_t stick_s3_board_usb_powered(bool *usb_powered);
esp_err_t stick_s3_board_clear_power_irqs(uint8_t *sys_status);
void stick_s3_board_prepare_deep_sleep(void);
/* Liest den Pin direkt -- der Abgleich im Aufnahme-Takt braucht das. */
bool stick_s3_talk_button_pressed(void);
bool stick_s3_browse_button_pressed(void);
