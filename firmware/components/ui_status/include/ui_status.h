#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ui_status_init(void);
esp_err_t ui_status_set_brightness(uint8_t brightness);
void ui_status_prepare_deep_sleep(void);
void ui_status_set_device_name(const char *device_name);
void ui_status_set_advertising(void);
void ui_status_set_pairing(const char *device_name);
void ui_status_set_idle_hint(const char *hint);
void ui_status_set_idle(void);
void ui_status_set_idle_text(const char *text);
void ui_status_set_idle_dimmed(bool dimmed);
void ui_status_show_last_answer(void);
void ui_status_set_cancelled(void);
void ui_status_set_link(bool connected);
void ui_status_set_recording_meter(uint8_t level_percent, uint8_t remaining_percent);
void ui_status_set_recording(uint32_t session_id);
void ui_status_set_battery(int level_percent, bool charging, bool usb_powered);
void ui_status_set_partial_text(const char *text);
void ui_status_set_ota_progress(uint32_t written, uint32_t size);
void ui_status_set_ota_rebooting(void);
void ui_status_set_error(const char *message);
