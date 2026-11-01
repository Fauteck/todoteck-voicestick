#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* Panel, LVGL-Anzeige und Zeichenpuffer der Uhr; lv_init() muss vorher gelaufen sein. */
esp_err_t ui_display_watch206_create(lv_display_t **out);
esp_err_t ui_display_watch206_set_brightness(uint8_t brightness);
void ui_display_watch206_sleep(void);
/* Zurueck aus Sleep-In: Sleep-Out, 120 ms warten, Display an. Dauert deshalb 120 ms. */
void ui_display_watch206_wake(void);
