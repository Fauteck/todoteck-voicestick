#include "ui_cards.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "ui_status_internal.h"
#include "voice_board.h"

static const char *TAG = "ui_cards";

/* Farbe einer Zeile: 0xRRGGBB plus dieses Bit, wenn eine mitkam. */
#define CARD_COLOUR_SET 0x01000000u

typedef struct {
    char text[UI_CARD_TEXT_LEN];
    char aux[UI_CARD_AUX_LEN];
    uint32_t colour;  /* Projektfarbe (nur `today`), CARD_COLOUR_SET wenn gesetzt */
    bool done;        /* angetippt; die Bruecke hat es noch nicht bestaetigt */
} card_item_t;

typedef struct {
    uint16_t rev;
    uint8_t count;
    /*
     * Kam diese Liste je von der Bruecke? Ohne die Unterscheidung zeigte
     * eine Uhr ohne Daten -- Bruecke nicht verbunden, Glance gescheitert --
     * "Heute ist alles erledigt.", obwohl Aufgaben offen waren (11/2026).
     */
    bool received;
    card_item_t items[UI_CARD_ITEMS_MAX];
} card_list_t;

static const char *const KIND_NAMES[UI_CARD_COUNT] = { "today", "shopping", "switches" };

/*
 * Die Listen liegen im PSRAM-faehigen Heap nicht -- es sind gut 2,5 KB je
 * Liste, statisch im internen RAM. Bei drei Listen vertretbar; mehr
 * Eintraege zeigt eine Uhr ohnehin nicht sinnvoll an.
 */
static card_list_t s_lists[UI_CARD_COUNT];

/*
 * "#RRGGBB" -> Farbe mit CARD_COLOUR_SET; alles andere (leer, "#abc",
 * fehlend, kaputt) -> 0, also kein Streifen. Aeltere Bruecken schicken das
 * dritte Element gar nicht.
 */
static uint32_t parse_colour(const char *text)
{
    if (!text || text[0] != '#' || strlen(text) != 7) {
        return 0;
    }
    uint32_t value = 0;
    for (int i = 1; i < 7; i++) {
        const char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    return value | CARD_COLOUR_SET;
}

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"
#include "ui_status_icons.h"

static const char *const TITLES[UI_CARD_COUNT] = { "Heute", "Einkauf", "Zuhause" };
/*
 * Leere Karten sagen etwas Freundliches, und Tecki sagt es mit: Heute
 * erledigt -- er freut sich; leere Einkaufsliste -- neutral; keine Schalter
 * -- er schaut zur Seite, wo man sie einrichtet.
 */
static const char *const EMPTY_HINTS[UI_CARD_COUNT] = {
    "Heute ist alles erledigt.",
    "Nichts auf der Einkaufsliste.",
    "Noch keine Schalter. In den Einstellungen unter Sprache einrichten.",
};
/* Noch nie eine Liste bekommen: ehrlich sagen, dass Daten fehlen. */
static const char *const NO_DATA_HINT = "Noch keine Daten vom Handy. Ist die Todoteck-App verbunden?";

static const ui_status_icon_scene_t EMPTY_POSES[UI_CARD_COUNT] = {
    UI_STATUS_ICON_HAPPY,
    UI_STATUS_ICON_IDLE,
    UI_STATUS_ICON_LOOK_RIGHT,
};

/*
 * Zeilen je Seite und Schrift nach der Schriftgroesse aus `watch_settings`
 * (ui_status_font_level): klein 7 Zeilen in 20 px, normal 6 in 23, gross 5
 * in 26. Die Zeilen teilen sich die Hoehe zwischen Ueberschrift und den
 * Seitenpunkten unten; jede ist mindestens 48 px hoch (Tabelle in
 * ui_layout_watch206.h).
 */
#define ROWS_MAX 7
#define TITLE_H 50
#define ROWS_TOP (HEADER_H + TITLE_H - 20)
#define ROWS_BOTTOM (CONTENT_H - 24)
#define ROW_GAP 6
#define STRIPE_W 6
static const uint8_t LEVEL_ROWS[3] = { 7, 6, 5 };
static const lv_font_t *const LEVEL_ROW_FONT[3] = {
    &todoteck_watch_font_20, &todoteck_watch_font_23, &todoteck_watch_font_26,
};

/* Rueckgaengig: 5 s nach einem Tippen auf eine Zeile, unten in der Karte. */
#define UNDO_W 240
#define UNDO_H 56
#define UNDO_MS 5000
/*
 * Bestaetigung auf der Karte Heute: Das erste Tippen markiert die Zeile,
 * erst "OK - erledigt" hakt ab. Der Chip sitzt rechts **in** der markierten
 * Zeile, nicht unten wie "Rueckgaengig": Unten laege er ueber der letzten
 * Zeile, und wer nach der Markierung eine untere Zeile antippen wollte,
 * bestaetigte stattdessen die markierte -- genau der Fehlgriff, den die
 * Bestaetigung verhindern soll. In der Zeile verdeckt er nur die eigene
 * Zusatzangabe (Uhrzeit), ist so hoch wie die Zeile (also >= 48 px), und
 * seine Mitte liegt bei x 319 weit weg von den Eckrundungen. 150 px fassen
 * "OK - erledigt" in der 20er (rund 116 px) mit Rand.
 */
#define OK_W 150
#define OK_MS 5000
/* Teckis Flaeche auf der leeren Karte. */
#define EMPTY_TECKI_BOX 96
#define EMPTY_TECKI_Y 110

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static ui_dots_t s_card_dots;
static ui_dots_t s_page_dots;
static lv_obj_t *s_empty;
static ui_status_icons_t s_tecki;
static lv_obj_t *s_row[ROWS_MAX];
static lv_obj_t *s_row_stripe[ROWS_MAX];
static lv_obj_t *s_row_text[ROWS_MAX];
static lv_obj_t *s_row_aux[ROWS_MAX];
static lv_obj_t *s_undo;
static lv_timer_t *s_undo_timer;
static lv_obj_t *s_ok;
static lv_timer_t *s_sel_timer;
static bool s_visible;
static ui_card_kind_t s_kind;
static uint8_t s_page_index;

/* Was der Chip rueckgaengig machen wuerde. */
static struct {
    bool armed;
    ui_card_kind_t kind;
    uint16_t rev;
    uint8_t index;
    bool prev_done;
} s_last;

/* Markierte Zeile der Karte Heute, die auf "OK" wartet (Index in der Liste). */
static struct {
    bool active;
    uint16_t rev;
    uint8_t index;
} s_sel;

static uint8_t rows_per_page(void)
{
    return LEVEL_ROWS[ui_status_font_level() % 3];
}

static void create_locked(void)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_root, CONTENT_W, CONTENT_H);
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /*
     * 26 statt 30 px: Seit die Uhrzeit mittig in der Kopfzeile steht, muss
     * "Zuhause" links davor enden (rund 115 statt 135 px breit).
     */
    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, &todoteck_watch_font_26, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, UI_HEADER_INSET_X, HEADER_H - 20);

    /* Welche der drei Karten: Punkte oben rechts (statt "1/3"). */
    ui_dots_create(&s_card_dots, s_root);
    lv_obj_align(s_card_dots.box, LV_ALIGN_TOP_RIGHT, -UI_HEADER_INSET_X, HEADER_H - 6);
    /* Welche Seite der Karte: Punkte unten mittig, nur bei mehr als einer. */
    ui_dots_create(&s_page_dots, s_root);
    lv_obj_align(s_page_dots.box, LV_ALIGN_TOP_MID, 0, CONTENT_H - 16);

    ui_status_icons_create(&s_tecki, s_root);
    ui_status_icons_show(&s_tecki, false);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_empty, UI_FONT_QUESTION, 0);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, CONTENT_W - 2 * UI_HEADER_INSET_X);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, EMPTY_TECKI_Y + EMPTY_TECKI_BOX + 16);

    for (int i = 0; i < ROWS_MAX; i++) {
        s_row[i] = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_row[i]);
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(s_row[i], 16, 0);
        lv_obj_set_style_bg_opa(s_row[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(0x1b2430), 0);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

        /* Projektfarbe als Streifen links; ohne Farbe versteckt. */
        s_row_stripe[i] = lv_obj_create(s_row[i]);
        lv_obj_remove_style_all(s_row_stripe[i]);
        lv_obj_set_style_radius(s_row_stripe[i], STRIPE_W / 2, 0);
        lv_obj_set_style_bg_opa(s_row_stripe[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_row_stripe[i], LV_OBJ_FLAG_HIDDEN);

        s_row_text[i] = lv_label_create(s_row[i]);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);

        s_row_aux[i] = lv_label_create(s_row[i]);
        lv_obj_set_style_text_font(s_row_aux[i], UI_FONT_SMALL, 0);
        lv_obj_align(s_row_aux[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }

    /* Rueckgaengig-Chip: hell gefuellt, damit er ueber den dunklen Zeilen steht. */
    s_undo = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_undo);
    lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_undo, UNDO_W, UNDO_H);
    lv_obj_set_style_radius(s_undo, UNDO_H / 2, 0);
    lv_obj_set_style_bg_opa(s_undo, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_undo, lv_color_hex(0xf2f2f2), 0);
    lv_obj_align(s_undo, LV_ALIGN_TOP_MID, 0, ROWS_BOTTOM - UNDO_H);
    lv_obj_t *undo_label = lv_label_create(s_undo);
    lv_label_set_text(undo_label, "Rückgängig");
    lv_obj_set_style_text_font(undo_label, &todoteck_watch_font_23, 0);
    lv_obj_set_style_text_color(undo_label, lv_color_hex(0x000000), 0);
    lv_obj_center(undo_label);
    lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);

    /* OK-Chip: gruen mit schwarzer Schrift, Lage setzt render_locked(). */
    s_ok = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_ok);
    lv_obj_remove_flag(s_ok, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_ok, 16, 0);
    lv_obj_set_style_bg_opa(s_ok, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ok, lv_color_hex(0x2ecc71), 0);
    lv_obj_t *ok_label = lv_label_create(s_ok);
    char ok_text[24];
    /* Die Schriften haben keinen Halbgeviertstrich; die Kopie macht "-" daraus. */
    ui_status_copy_display_text(ok_text, "OK – erledigt", sizeof(ok_text));
    lv_label_set_text(ok_label, ok_text);
    lv_obj_set_style_text_font(ok_label, &todoteck_watch_font_20, 0);
    lv_obj_set_style_text_color(ok_label, lv_color_hex(0x000000), 0);
    lv_obj_center(ok_label);
    lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
}

static uint8_t page_count(const card_list_t *list)
{
    const uint8_t rows = rows_per_page();
    return list->count == 0 ? 1 : (uint8_t)((list->count + rows - 1) / rows);
}

static void hide_undo_locked(void)
{
    s_last.armed = false;
    if (s_undo_timer) {
        lv_timer_delete(s_undo_timer);
        s_undo_timer = NULL;
    }
    if (s_undo) {
        lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Markierung verwerfen; nichts wird gesendet. Zeichnet nicht neu. */
static bool clear_selection_locked(void)
{
    const bool was_active = s_sel.active;
    s_sel.active = false;
    if (s_sel_timer) {
        lv_timer_delete(s_sel_timer);
        s_sel_timer = NULL;
    }
    if (s_ok) {
        lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
    }
    return was_active;
}

static void render_locked(void);

static void sel_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_sel_timer = NULL;
    clear_selection_locked();
    if (s_visible) {
        render_locked();
    }
}

static void undo_expired_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Einmaliger Zeitgeber: LVGL loescht ihn nach diesem Aufruf selbst. */
    s_undo_timer = NULL;
    hide_undo_locked();
}

static void render_locked(void)
{
    if (!s_root) {
        return;
    }
    const card_list_t *list = &s_lists[s_kind];
    const uint8_t rows = rows_per_page();
    const uint8_t pages = page_count(list);
    const lv_font_t *row_font = LEVEL_ROW_FONT[ui_status_font_level() % 3];
    if (s_page_index >= pages) {
        s_page_index = pages - 1;
    }
    lv_label_set_text(s_title, TITLES[s_kind]);
    ui_dots_set(&s_card_dots, UI_CARD_COUNT, (uint8_t)s_kind);
    ui_dots_set(&s_page_dots, pages > 1 ? pages : 0, s_page_index);

    if (list->count == 0) {
        lv_label_set_text(s_empty, list->received ? EMPTY_HINTS[s_kind] : NO_DATA_HINT);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        ui_status_icons_set_box(&s_tecki, EMPTY_TECKI_BOX, (CONTENT_W - EMPTY_TECKI_BOX) / 2,
                                EMPTY_TECKI_Y);
        ui_status_icons_apply(&s_tecki, list->received ? EMPTY_POSES[s_kind] : UI_STATUS_ICON_LOOK_LEFT);
        ui_status_icons_show(&s_tecki, true);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        ui_status_icons_show(&s_tecki, false);
    }

    const int32_t pitch = (ROWS_BOTTOM - ROWS_TOP) / rows;
    bool ok_shown = false;
    for (int i = 0; i < ROWS_MAX; i++) {
        const int index = s_page_index * rows + i;
        if (i >= rows || index >= list->count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const card_item_t *item = &list->items[index];
        const bool selected = s_sel.active && s_kind == UI_CARD_TODAY && s_sel.index == index;
        lv_obj_set_size(s_row[i], CONTENT_W, pitch - ROW_GAP);
        lv_obj_align(s_row[i], LV_ALIGN_TOP_LEFT, 0, ROWS_TOP + i * pitch);
        /* Markiert: hellere Flaeche und gruener Rand, passend zum OK-Chip. */
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(selected ? 0x2a3b4f : 0x1b2430), 0);
        lv_obj_set_style_border_width(s_row[i], selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_row[i], lv_color_hex(0x2ecc71), 0);
        if (selected) {
            lv_obj_set_size(s_ok, OK_W, pitch - ROW_GAP);
            lv_obj_align(s_ok, LV_ALIGN_TOP_LEFT, CONTENT_W - OK_W, ROWS_TOP + i * pitch);
            lv_obj_remove_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
            ok_shown = true;
        }

        const bool striped = (item->colour & CARD_COLOUR_SET) != 0;
        const int32_t text_x = striped ? 16 + STRIPE_W + 8 : 16;
        if (striped) {
            lv_obj_set_size(s_row_stripe[i], STRIPE_W, pitch - ROW_GAP - 16);
            lv_obj_align(s_row_stripe[i], LV_ALIGN_LEFT_MID, 12, 0);
            lv_obj_set_style_bg_color(s_row_stripe[i],
                                      lv_color_hex(item->colour & 0xFFFFFFu), 0);
            lv_obj_set_style_bg_opa(s_row_stripe[i], item->done ? LV_OPA_40 : LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_row_stripe[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_row_stripe[i], LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_set_style_text_font(s_row_text[i], row_font, 0);
        lv_obj_set_width(s_row_text[i], (selected ? CONTENT_W - OK_W - 8 : CONTENT_W - 94) - text_x);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, text_x, 0);
        lv_label_set_text(s_row_text[i], item->text);
        lv_obj_set_style_text_decor(s_row_text[i],
                                    item->done && s_kind != UI_CARD_SWITCHES ?
                                    LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_text_color(s_row_text[i],
                                    item->done ? lv_color_hex(0x6f7f95) : lv_color_hex(0xf2f2f2),
                                    0);
        if (s_kind == UI_CARD_SWITCHES) {
            /* Schalter: An/Aus als farbiges Wort; angetippt zeigt es den Zielzustand. */
            bool on = strcmp(item->aux, "1") == 0;
            if (item->done) {
                on = !on;
            }
            lv_label_set_text(s_row_aux[i], on ? "An" : "Aus");
            lv_obj_set_style_text_color(s_row_aux[i],
                                        on ? lv_color_hex(0x2ecc71) : lv_color_hex(0x9aa3ad), 0);
        } else {
            lv_label_set_text(s_row_aux[i], item->aux);
            lv_obj_set_style_text_color(s_row_aux[i], lv_color_hex(0x9aa3ad), 0);
        }
        /* Unter dem OK-Chip laege die Zusatzangabe nur verdeckt. */
        if (selected) {
            lv_obj_add_flag(s_row_aux[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_row_aux[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!ok_shown) {
        lv_obj_add_flag(s_ok, LV_OBJ_FLAG_HIDDEN);
    }
    /* Die Chips liegen ueber den Zeilen, also nach ihnen nach vorn. */
    if (s_last.armed) {
        lv_obj_move_foreground(s_undo);
    }
    if (ok_shown) {
        lv_obj_move_foreground(s_ok);
    }
}

/* Abstand des Punkts zur Mitte einer Flaeche, wenn er in ihr samt Toleranz liegt; sonst -1. */
static int32_t hit_distance(lv_obj_t *obj, int16_t x, int16_t y)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return -1;
    }
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    if (x < area.x1 - UI_HIT_TOLERANCE || x > area.x2 + UI_HIT_TOLERANCE ||
        y < area.y1 - UI_HIT_TOLERANCE || y > area.y2 + UI_HIT_TOLERANCE) {
        return -1;
    }
    const int32_t cy = (area.y1 + area.y2) / 2;
    return y > cy ? y - cy : cy - y;
}

/*
 * Zeile `index` der sichtbaren Karte abhaken bzw. umschalten -- so, wie es
 * vor der Bestaetigung jedes Tippen tat: Die Zeile zeigt es sofort, und
 * fuenf Sekunden steht "Rueckgaengig" (ein neues Abhaken startet sie neu).
 */
static ui_cards_action_t check_off_locked(card_list_t *list, int index, ui_cards_hit_t *hit)
{
    card_item_t *item = &list->items[index];
    s_last.kind = s_kind;
    s_last.rev = list->rev;
    s_last.index = (uint8_t)index;
    s_last.prev_done = item->done;
    item->done = s_kind == UI_CARD_SWITCHES ? !item->done : true;
    if (hit) {
        hit->kind = s_kind;
        hit->rev = list->rev;
        hit->index = (uint8_t)index;
    }
    hide_undo_locked();
    s_last.armed = true;
    s_undo_timer = lv_timer_create(undo_expired_cb, UNDO_MS, NULL);
    lv_timer_set_repeat_count(s_undo_timer, 1);
    lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
    render_locked();
    return UI_CARDS_ITEM;
}
#endif /* CONFIG_VOICE_BOARD_WATCH206 */

int ui_cards_kind_from_name(const char *name)
{
    for (int i = 0; i < UI_CARD_COUNT; i++) {
        if (name && strcmp(name, KIND_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *ui_cards_kind_name(ui_card_kind_t kind)
{
    return kind < UI_CARD_COUNT ? KIND_NAMES[kind] : "";
}

void ui_cards_begin_part(ui_card_kind_t kind, uint16_t rev, uint8_t part)
{
    if (kind >= UI_CARD_COUNT) {
        return;
    }
    ui_status_lock();
    if (part == 0) {
        memset(&s_lists[kind], 0, sizeof(s_lists[kind]));
    }
#if CONFIG_VOICE_BOARD_WATCH206
    /*
     * Neue Liste waehrend einer Markierung: Hinter dem Index steht jetzt
     * vielleicht eine andere Aufgabe -- ein "OK" bestaetigte die falsche.
     * Das Neuzeichnen kommt mit ui_cards_commit().
     */
    if (kind == UI_CARD_TODAY && s_sel.active && s_sel.rev != rev) {
        clear_selection_locked();
    }
#endif
    s_lists[kind].rev = rev;
    s_lists[kind].received = true;
    ui_status_unlock();
}

void ui_cards_add_item(ui_card_kind_t kind, const char *text, const char *aux,
                       const char *colour)
{
    if (kind >= UI_CARD_COUNT) {
        return;
    }
    ui_status_lock();
    card_list_t *list = &s_lists[kind];
    if (list->count < UI_CARD_ITEMS_MAX) {
        card_item_t *item = &list->items[list->count++];
        ui_status_copy_display_text(item->text, text ? text : "", sizeof(item->text));
        ui_status_copy_display_text(item->aux, aux ? aux : "", sizeof(item->aux));
        item->colour = parse_colour(colour);
        item->done = false;
    }
    ui_status_unlock();
}

void ui_cards_commit(ui_card_kind_t kind)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    if (s_visible && s_kind == kind) {
        render_locked();
    }
    ui_status_unlock();
#else
    (void)kind;
#endif
}

void ui_cards_show(ui_card_kind_t kind)
{
#if CONFIG_VOICE_BOARD_WATCH206
    if (kind >= UI_CARD_COUNT) {
        kind = UI_CARD_TODAY;
    }
    ui_status_lock();
    create_locked();
    hide_undo_locked();
    clear_selection_locked();
    s_kind = kind;
    s_page_index = 0;
    s_visible = true;
    render_locked();
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    ui_status_header_refresh_locked();
    ui_status_unlock();
#else
    (void)kind;
#endif
}

void ui_cards_hide(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    s_visible = false;
    hide_undo_locked();
    clear_selection_locked();
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_header_refresh_locked();
    ui_status_unlock();
#endif
}

bool ui_cards_visible(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_visible;
#else
    return false;
#endif
}

/*
 * Gesten auf den Karten -- Teil der einen Gestensprache der Uhr (Tabelle in
 * main.c, handle_touch):
 *
 *   links / rechts   naechste / vorige Karte (Rueckgaengig-Chip verschwindet)
 *   nach oben        naechste Seite, nach der letzten wieder die erste
 *   nach unten       eine Ebene zurueck: Karten zu
 *   Tippen           Chip "Rueckgaengig" vor Zeile; Zeile abhaken/schalten --
 *                    auf Heute erst markieren, dann "OK - erledigt" (11/2026)
 *
 * Heute fragt nach: Ein Tippen auf eine Zeile markiert sie nur, erst der
 * OK-Chip in der Zeile hakt ab. Ein Fehltipp am Handgelenk -- beim Wischen,
 * am Aermel -- hakte vorher sofort eine Aufgabe ab (Wunsch des Nutzers).
 * Eine andere Zeile antippen markiert diese; daneben tippen, wischen, die
 * Karte wechseln oder schliessen, fuenf Sekunden warten oder eine neue Liste
 * der Bruecke verwerfen die Markierung, ohne etwas zu senden. Einkauf und
 * Zuhause bleiben beim direkten Tippen: Dort ist der Fehlgriff harmlos und
 * mit "Rueckgaengig" schnell zurueck.
 *
 * Bis 10/2026 blaetterte "nach unten" erst zurueck und schloss erst auf der
 * ersten Seite. Ueberall sonst heisst nach unten "eine Ebene zurueck", und
 * eine Geste, die je nach Seite Verschiedenes tut, merkt sich niemand. Wer
 * eine Seite zurueck will, wischt weiter nach oben -- die Seiten laufen im
 * Kreis, und mehr als drei sind es nie.
 */
ui_cards_action_t ui_cards_touch(int gesture, int16_t x, int16_t y, ui_cards_hit_t *hit)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_cards_action_t action = UI_CARDS_NONE;
    ui_status_lock();
    if (!s_visible) {
        ui_status_unlock();
        return UI_CARDS_NONE;
    }
    card_list_t *list = &s_lists[s_kind];
    /* Jede Geste ausser Tippen verwirft die Markierung. */
    if ((voice_board_gesture_t)gesture != VOICE_BOARD_TAP && clear_selection_locked()) {
        render_locked();
    }
    switch ((voice_board_gesture_t)gesture) {
    /*
     * Die Karten liegen rechts neben dem Zifferblatt in einer Reihe:
     * Zifferblatt | Heute | Einkauf | Zuhause. Nach links wischen holt die
     * naechste Karte von rechts; am Ende der Reihe passiert nichts (kein
     * Ringlauf, sonst verliert man die Orientierung). Nach rechts geht es
     * zurueck, von Heute aus aufs Zifferblatt.
     */
    case VOICE_BOARD_SWIPE_LEFT:
        if (s_kind + 1 < UI_CARD_COUNT) {
            hide_undo_locked();
            s_kind = (ui_card_kind_t)(s_kind + 1);
            s_page_index = 0;
            render_locked();
        } else {
            action = UI_CARDS_END;
        }
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        if (s_kind == 0) {
            action = UI_CARDS_CLOSE;
        } else {
            hide_undo_locked();
            s_kind = (ui_card_kind_t)(s_kind - 1);
            s_page_index = 0;
            render_locked();
        }
        break;
    case VOICE_BOARD_SWIPE_UP:
        s_page_index = (uint8_t)((s_page_index + 1) % page_count(list));
        render_locked();
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        action = UI_CARDS_CLOSE;
        break;
    case VOICE_BOARD_TAP: {
        /*
         * Mit Toleranz trifft ein Finger zwischen zwei Zeilen beide; es
         * zaehlt die, deren Mitte naeher liegt.
         */
        const uint8_t rows = rows_per_page();
        int best = -1;
        int32_t best_distance = 0;
        for (int i = 0; i < rows && i < ROWS_MAX; i++) {
            const int32_t distance = hit_distance(s_row[i], x, y);
            if (distance >= 0 && (best < 0 || distance < best_distance)) {
                best = i;
                best_distance = distance;
            }
        }
        if (s_sel.active) {
            /*
             * OK getroffen: Der Chip liegt in der markierten Zeile und hat
             * deren Mitte; eine Nachbarzeile gewinnt nur, wenn ihre Mitte
             * naeher liegt (Tippen im Zwischenraum).
             */
            const int32_t ok_distance = hit_distance(s_ok, x, y);
            if (ok_distance >= 0 && (best < 0 || ok_distance <= best_distance)) {
                const int index = s_sel.index;
                const bool valid = s_kind == UI_CARD_TODAY && list->rev == s_sel.rev &&
                                   index < list->count && !list->items[index].done;
                clear_selection_locked();
                if (valid) {
                    action = check_off_locked(list, index, hit);
                } else {
                    render_locked();
                }
                break;
            }
        }
        if (s_last.armed && hit_distance(s_undo, x, y) >= 0) {
            /*
             * Rueckgaengig: Die Zeile zeigt sofort wieder den Stand vorher --
             * sofern die Liste noch dieselbe ist. Hat die Bruecke inzwischen
             * eine neue geschickt (`rev` anders), zieht der naechste Abgleich
             * die Karte ohnehin nach.
             */
            card_list_t *undo_list = &s_lists[s_last.kind];
            if (undo_list->rev == s_last.rev && s_last.index < undo_list->count) {
                undo_list->items[s_last.index].done = s_last.prev_done;
            }
            if (hit) {
                hit->kind = s_last.kind;
                hit->rev = s_last.rev;
                hit->index = s_last.index;
            }
            hide_undo_locked();
            action = UI_CARDS_UNDO;
            render_locked();
            break;
        }
        const int index = best < 0 ? -1 : s_page_index * rows + best;
        /*
         * Abhaken geht nur einmal -- ein zweites Tippen auf dieselbe
         * Zeile schickte sonst zweimal "erledigt". Schalter duerfen hin
         * und her.
         */
        if (index < 0 || index >= list->count ||
            (list->items[index].done && s_kind != UI_CARD_SWITCHES)) {
            /* Daneben getippt: eine Markierung verfaellt, gesendet wird nichts. */
            if (clear_selection_locked()) {
                render_locked();
            }
            break;
        }
        if (s_kind == UI_CARD_TODAY) {
            /*
             * Heute: nur markieren (oder die Markierung auf diese Zeile
             * legen). Fuenf Sekunden wartet der OK-Chip, jedes neue Tippen
             * auf eine Zeile startet sie neu. Ein noch stehendes
             * "Rueckgaengig" weicht -- sonst laegen zwei Chips uebereinander.
             */
            clear_selection_locked();
            hide_undo_locked();
            s_sel.active = true;
            s_sel.rev = list->rev;
            s_sel.index = (uint8_t)index;
            s_sel_timer = lv_timer_create(sel_expired_cb, OK_MS, NULL);
            lv_timer_set_repeat_count(s_sel_timer, 1);
            if (hit) {
                hit->kind = s_kind;
                hit->rev = list->rev;
                hit->index = (uint8_t)index;
            }
            action = UI_CARDS_SELECT;
            render_locked();
            break;
        }
        action = check_off_locked(list, index, hit);
        break;
    }
    default:
        break;
    }
    ui_status_unlock();
    if (action == UI_CARDS_CLOSE) {
        ui_cards_hide();
    }
    ESP_LOGD(TAG, "touch %d -> %d", gesture, (int)action);
    return action;
#else
    (void)gesture;
    (void)x;
    (void)y;
    (void)hit;
    return UI_CARDS_NONE;
#endif
}

/* Neue Schriftgroesse: Zeilen je Seite und Schrift aendern sich mit. */
void ui_cards_refresh(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    if (s_visible) {
        render_locked();
    }
    ui_status_unlock();
#endif
}
