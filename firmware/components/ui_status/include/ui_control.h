#pragma once

/*
 * Kontrollzentrum der Uhr: mit einem Wischen vom oberen Rand nach unten.
 * Sechs Kacheln -- Helligkeit, Lautlos, Kino, Sparen, Lampe, Akku. Was die
 * Kacheln bewirken, entscheidet main.c; hier wird nur gezeichnet und
 * getroffen. Dazu die Taschenlampe: der ganze Schirm weiss, bis zum
 * naechsten Antippen. Auf dem StickS3 tun die Funktionen nichts.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_CONTROL_BRIGHTNESS,
    UI_CONTROL_SILENT,
    UI_CONTROL_CINEMA,
    UI_CONTROL_SAVER,
    UI_CONTROL_TORCH,
    UI_CONTROL_BATTERY,
    UI_CONTROL_COUNT,
} ui_control_tile_t;

typedef struct {
    const char *brightness;   /* "Hell", "Mittel", "Dunkel" */
    bool silent;
    bool cinema;
    bool saver;
    int battery_percent;      /* -1: unbekannt */
    bool charging;
} ui_control_state_t;

void ui_control_show(const ui_control_state_t *state);
void ui_control_update(const ui_control_state_t *state);
void ui_control_hide(void);
bool ui_control_visible(void);
/* Kachel unter dem Finger, -1: keine. */
int ui_control_tile_at(int16_t x, int16_t y);

void ui_torch_set(bool on);
bool ui_torch_on(void);
