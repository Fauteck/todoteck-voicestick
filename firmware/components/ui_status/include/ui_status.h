#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

/* Antwortmoeglichkeiten auf eine Rueckfrage: hoechstens drei, je 15 Byte Text. */
#define UI_STATUS_CHOICES_MAX 3
#define UI_STATUS_CHOICE_LEN 16

/* Zeilen unter der Uhrzeit (nur die Uhr): hoechstens drei, je 56 Byte (bis 0.3.6: 40). */
#define UI_STATUS_FACE_LINES 3
#define UI_STATUS_FACE_LEN 57

/*
 * Ergebnis eines Turns, wie die Bruecke es mit `ui_state` meldet
 * (`outcome`, docs/protocol.md): angelegt, erledigt, Antwort, Fehler. Der
 * Verlauf zeigt je Eintrag einen Punkt in der Farbe und das Wort dazu.
 */
typedef enum {
    UI_OUTCOME_NONE,
    UI_OUTCOME_CREATED,
    UI_OUTCOME_DONE,
    UI_OUTCOME_ANSWER,
    UI_OUTCOME_ERROR,
} ui_outcome_t;

/* Was ein Chip im Verlauf ausloest -- main.c schickt das Ereignis an die Bruecke. */
typedef enum {
    UI_HISTORY_CHIP_NONE,
    UI_HISTORY_CHIP_REPEAT,     /* "Nochmal": `repeat` mit der Frage */
    UI_HISTORY_CHIP_UNDO_TURN,  /* "Rueckgaengig" eines Turns: `undo_turn` mit `ref` */
    UI_HISTORY_CHIP_UNDO_CARD,  /* "Rueckgaengig" einer Karten-Aktion: `undo` mit Koordinaten */
} ui_history_chip_t;

typedef struct {
    ui_history_chip_t chip;
    char question[64];   /* fuer `repeat` */
    uint16_t ref;        /* fuer `undo_turn` */
    uint8_t card_kind;   /* fuer `undo`: ui_card_kind_t, Stand, Position */
    uint16_t card_rev;
    uint8_t card_index;
} ui_history_chip_hit_t;

typedef enum {
    UI_STATUS_MOOD_NONE,
    UI_STATUS_MOOD_HAPPY,
    UI_STATUS_MOOD_LOOK_LEFT,
    UI_STATUS_MOOD_LOOK_RIGHT,
} ui_status_mood_t;

esp_err_t ui_status_init(void);
esp_err_t ui_status_set_brightness(uint8_t brightness);
void ui_status_prepare_deep_sleep(void);
void ui_status_set_device_name(const char *device_name);
void ui_status_set_advertising(void);
void ui_status_set_pairing(const char *device_name);
void ui_status_set_idle(void);
/*
 * Antwort der Bruecke plus die verstandene Frage; beides wandert in den
 * Verlauf, mit Ergebnis-Marker und der `ref`, unter der die Bruecke den Turn
 * 15 Minuten lang zuruecknimmt (0 = nicht ruecknehmbar).
 */
void ui_status_set_idle_text(const char *text, const char *question, ui_outcome_t outcome,
                             uint16_t ref);
/*
 * Karten-Aktion als Verlaufseintrag ("Erledigt: Muell rausstellen", "Licht
 * Kueche: An") samt Koordinaten fuer `undo` -- die Szene aendert sich nicht.
 */
void ui_status_history_add_card(const char *line, uint8_t card_kind, uint16_t card_rev,
                                uint8_t card_index);
/* Liegt der Finger auf "Nochmal" oder "Rueckgaengig" des gezeigten Eintrags? */
bool ui_status_history_chip_at(int16_t x, int16_t y, ui_history_chip_hit_t *out);
/* Nach einem gesendeten Rueckgaengig: der gezeigte Eintrag ist nicht noch einmal ruecknehmbar. */
void ui_status_history_mark_undone(void);
void ui_status_set_idle_dimmed(bool dimmed);
/* Einen Schritt zurueck durch die letzten Fragen und Antworten. */
void ui_status_browse_history(void);
/* Eine Antwort neuer -- das Gegenstueck zum Blaettern, fuer das Wischen auf der Uhr. */
void ui_status_browse_newer(void);
/* Steht gerade der Verlauf (oder sein leerer Zustand) auf dem Schirm? */
bool ui_status_in_history(void);
/* Zurueck aufs Ziffernblatt, ohne Bruecke auf "Koppeln". */
void ui_status_show_home(void);
/*
 * Antwortflaechen unter der gerade gezeigten Antwort (nur die Uhr). Die
 * naechste Szene raeumt sie wieder weg.
 */
/*
 * Was unter der Uhrzeit steht, und der naechste Termin (UTC-Sekunden, 0 =
 * keiner). Die Uhr fuehrt den Countdown selbst nach.
 */
void ui_status_set_face(const char (*lines)[UI_STATUS_FACE_LEN], uint8_t count,
                        const char *event_title, int64_t event_start);
/* Teckis Stimmung fuer `duration_ms`, danach wieder die Szene. */
void ui_status_set_mood(ui_status_mood_t mood, uint32_t duration_ms);
void ui_status_set_choices(const char (*choices)[UI_STATUS_CHOICE_LEN], uint8_t count);
/* Welche Flaeche liegt unter dem Finger? -1: keine. `out` bekommt ihren Text. */
int ui_status_choice_at(int16_t x, int16_t y, char *out, size_t out_size);
void ui_status_set_cancelled(void);
/*
 * Transkript zuerst (nur die Uhr zeichnet "Abbrechen" und den Balken):
 * "Verstanden" + Text, `cancel_ms` Frist. Endet mit der naechsten Szene.
 */
void ui_status_set_transcript(const char *text, uint32_t cancel_ms);
/* Steht die Transkript-Szene gerade da? */
bool ui_status_transcript_shown(void);
/* ... und laeuft ihre Frist noch? */
bool ui_status_transcript_cancellable(void);
/* Liegt der Finger auf "Abbrechen" (und laeuft die Frist noch)? */
bool ui_status_transcript_cancel_at(int16_t x, int16_t y);
void ui_status_set_transcript_cancelled(void);
/*
 * Kurzform <-> ganze Antwort. `true`, wenn umgeschaltet wurde; `false`, wenn
 * die Szene nichts umzuschalten hat.
 */
bool ui_status_toggle_more(void);
/* Stehen gerade Antwortflaechen einer Rueckfrage da? */
bool ui_status_choices_open(void);
/* Schriftgroesse aus `watch_settings` (0 klein, 1 normal, 2 gross); nur die Uhr wertet sie aus. */
void ui_status_set_font_level(uint8_t level);
/* Ein Firmware-Update laeuft: Tecki zeigt es auf Ziffernblatt und Bereit-Szene. */
void ui_status_set_updating(bool updating);
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
/*
 * Akku-Restzeit fuer das Zifferblatt ("noch etwa 2 h"), fertig formatiert von
 * main.c; leer = nichts zeigen. Steht rot unter den Zifferblatt-Zeilen (nur die Uhr).
 */
void ui_status_set_battery_remaining(const char *line);
/*
 * Nachtlade-Modus (nur die Uhr): am Kabel zwischen 22 und 6 Uhr zeigt das
 * Zifferblatt nur die grosse Uhrzeit in Dunkelgrau -- keine Zeilen, kein
 * Tecki, keine Kopfzeile. Andere Szenen bleiben, wie sie sind.
 */
void ui_status_set_night_mode(bool on);
void ui_status_set_partial_text(const char *text);
void ui_status_set_ota_progress(uint32_t written, uint32_t size);
void ui_status_set_ota_rebooting(void);
void ui_status_set_error(const char *message);
