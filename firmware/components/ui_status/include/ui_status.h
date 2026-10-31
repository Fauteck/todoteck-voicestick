#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/* Antwortmoeglichkeiten auf eine Rueckfrage: hoechstens drei, je 15 Byte Text. */
#define UI_STATUS_CHOICES_MAX 3
#define UI_STATUS_CHOICE_LEN 16

esp_err_t ui_status_init(void);
esp_err_t ui_status_set_brightness(uint8_t brightness);
void ui_status_prepare_deep_sleep(void);
void ui_status_set_device_name(const char *device_name);
void ui_status_set_advertising(void);
void ui_status_set_pairing(const char *device_name);
void ui_status_set_idle(void);
/* Antwort der Bruecke plus die verstandene Frage; beides wandert in den Verlauf. */
void ui_status_set_idle_text(const char *text, const char *question);
void ui_status_set_idle_dimmed(bool dimmed);
/* Einen Schritt zurueck durch die letzten Fragen und Antworten. */
void ui_status_browse_history(void);
/* Eine Antwort neuer -- das Gegenstueck zum Blaettern, fuer das Wischen auf der Uhr. */
void ui_status_browse_newer(void);
/* Zurueck aufs Ziffernblatt, ohne Bruecke auf "Koppeln". */
void ui_status_show_home(void);
/*
 * Antwortflaechen unter der gerade gezeigten Antwort (nur die Uhr). Die
 * naechste Szene raeumt sie wieder weg.
 */
void ui_status_set_choices(const char (*choices)[UI_STATUS_CHOICE_LEN], uint8_t count);
/* Welche Flaeche liegt unter dem Finger? -1: keine. `out` bekommt ihren Text. */
int ui_status_choice_at(int16_t x, int16_t y, char *out, size_t out_size);
void ui_status_set_cancelled(void);
void ui_status_set_press_too_short(void);
/* Sprechtaste gedrueckt, aber keine Bruecke verbunden -- vorher geschah dann sichtbar nichts. */
void ui_status_set_not_connected(void);
void ui_status_set_no_speech(const char *hint);
void ui_status_set_link(bool connected);
/* Uhrzeit von der Bruecke: UTC-Sekunden plus Abstand der Ortszeit in Minuten. */
void ui_status_set_clock(int64_t epoch_utc, int32_t tz_offset_min);
/* Nach dem Tiefschlaf: nur der Abstand kommt zurueck, die Systemuhr lief weiter. */
void ui_status_restore_clock(int32_t tz_offset_min);
void ui_status_set_recording_meter(uint8_t level_percent, uint8_t remaining_percent);
void ui_status_set_recording(uint32_t session_id);
void ui_status_set_battery(int level_percent, bool charging, bool usb_powered);
void ui_status_set_partial_text(const char *text);
void ui_status_set_ota_progress(uint32_t written, uint32_t size);
void ui_status_set_ota_rebooting(void);
void ui_status_set_error(const char *message);
