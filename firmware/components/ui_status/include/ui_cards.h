#pragma once

/*
 * Die Karten der Uhr: Heute, Einkauf, Zuhause.
 *
 * Erreichbar vom Ziffernblatt und aus dem Verlauf mit einem Wischen nach
 * oben; links/rechts wechselt die Karte, nach oben die naechste Seite, nach
 * unten eine Ebene zurueck. Ein Tippen auf eine Zeile hakt sie ab (Heute,
 * Einkauf) oder schaltet sie um (Zuhause) -- ausgefuehrt von der Bruecke,
 * die Karte zeigt es sofort. Danach steht fuenf Sekunden "Rueckgaengig".
 *
 * Die Inhalte schickt die Bruecke als `list`-Ereignis (docs/protocol.md).
 * Die Uhr kennt nur Texte und Positionen, keine IDs: Ein Tippen meldet
 * Art, Stand (`rev`) und Index, und die Bruecke weiss, was dahinter steht.
 * Auf dem StickS3 gibt es die Karten nicht; die Funktionen tun dort nichts.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_CARD_TODAY,
    UI_CARD_SHOPPING,
    UI_CARD_SWITCHES,
    UI_CARD_COUNT,
} ui_card_kind_t;

#define UI_CARD_ITEMS_MAX 15
#define UI_CARD_TEXT_LEN 41
#define UI_CARD_AUX_LEN 8

typedef enum {
    UI_CARDS_NONE,   /* nichts zu tun (oder intern erledigt) */
    UI_CARDS_ITEM,   /* Zeile angetippt -- `hit` sagt welche */
    UI_CARDS_CLOSE,  /* eine Ebene zurueck (die Karten sind schon zu) */
    UI_CARDS_UNDO,   /* "Rueckgaengig" angetippt -- `hit` sagt, welche Zeile */
} ui_cards_action_t;

typedef struct {
    ui_card_kind_t kind;
    uint16_t rev;
    uint8_t index;
} ui_cards_hit_t;

/* "today", "shopping", "switches" -> Art; -1 bei Unbekanntem. */
int ui_cards_kind_from_name(const char *name);
const char *ui_cards_kind_name(ui_card_kind_t kind);

/* Eine Liste kommt in Teilen: Teil 0 leert sie, jeder Teil haengt an. */
void ui_cards_begin_part(ui_card_kind_t kind, uint16_t rev, uint8_t part);
/* `colour`: "#RRGGBB" (Projektfarbe, nur `today`) oder ""/NULL -- dann kein Streifen. */
void ui_cards_add_item(ui_card_kind_t kind, const char *text, const char *aux,
                       const char *colour);
/* Nach dem letzten Teil: neu zeichnen, falls die Karte gerade zu sehen ist. */
void ui_cards_commit(ui_card_kind_t kind);

void ui_cards_show(ui_card_kind_t kind);
/* Neu zeichnen, falls sichtbar -- nach einer neuen Schriftgroesse. */
void ui_cards_refresh(void);
void ui_cards_hide(void);
bool ui_cards_visible(void);

/* Gesten (voice_board_gesture_t als int) und Lage des Fingers. */
ui_cards_action_t ui_cards_touch(int gesture, int16_t x, int16_t y, ui_cards_hit_t *hit);
