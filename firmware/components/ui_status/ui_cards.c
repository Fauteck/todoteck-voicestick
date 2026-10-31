#include "ui_cards.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "ui_status_internal.h"
#include "voice_board.h"

static const char *TAG = "ui_cards";

typedef struct {
    char text[UI_CARD_TEXT_LEN];
    char aux[UI_CARD_AUX_LEN];
    bool done;      /* angetippt; die Bruecke hat es noch nicht bestaetigt */
} card_item_t;

typedef struct {
    uint16_t rev;
    uint8_t count;
    card_item_t items[UI_CARD_ITEMS_MAX];
} card_list_t;

static const char *const KIND_NAMES[UI_CARD_COUNT] = { "today", "shopping", "switches" };

/*
 * Die Listen liegen im PSRAM-faehigen Heap nicht -- es sind gut 2,4 KB je
 * Liste, statisch im internen RAM. Bei drei Listen vertretbar; mehr
 * Eintraege zeigt eine Uhr ohnehin nicht sinnvoll an.
 */
static card_list_t s_lists[UI_CARD_COUNT];

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"

static const char *const TITLES[UI_CARD_COUNT] = { "Heute", "Einkauf", "Zuhause" };
static const char *const EMPTY_HINTS[UI_CARD_COUNT] = {
    "Heute ist nichts mehr offen.",
    "Die Einkaufsliste ist leer.",
    "Keine Schalter eingerichtet.",
};

/* Zeilen je Seite und ihre Hoehe -- passt unter die Ueberschrift. */
#define ROWS_PER_PAGE 6
#define ROW_H 62
#define TITLE_H 50

static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_page;
static lv_obj_t *s_empty;
static lv_obj_t *s_row[ROWS_PER_PAGE];
static lv_obj_t *s_row_text[ROWS_PER_PAGE];
static lv_obj_t *s_row_aux[ROWS_PER_PAGE];
static bool s_visible;
static ui_card_kind_t s_kind;
static uint8_t s_page_index;

static void create_locked(void)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, CONTENT_W, CONTENT_H);
    lv_obj_align(s_root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, UI_FONT_STATUS, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x2ecc71), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, UI_HEADER_INSET_X, HEADER_H - 20);

    s_page = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_page, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_page, lv_color_hex(0x9aa3ad), 0);
    lv_obj_align(s_page, LV_ALIGN_TOP_RIGHT, -UI_HEADER_INSET_X, HEADER_H - 12);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_empty, UI_FONT_QUESTION, 0);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(0x9aa3ad), 0);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, CONTENT_W);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 0);

    for (int i = 0; i < ROWS_PER_PAGE; i++) {
        s_row[i] = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_row[i]);
        lv_obj_set_size(s_row[i], CONTENT_W, ROW_H - 6);
        lv_obj_align(s_row[i], LV_ALIGN_TOP_LEFT, 0, HEADER_H + TITLE_H - 20 + i * ROW_H);
        lv_obj_set_style_radius(s_row[i], 16, 0);
        lv_obj_set_style_bg_opa(s_row[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(0x1b2430), 0);

        s_row_text[i] = lv_label_create(s_row[i]);
        lv_obj_set_style_text_font(s_row_text[i], &todoteck_watch_font_23, 0);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_row_text[i], CONTENT_W - 110);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, 16, 0);

        s_row_aux[i] = lv_label_create(s_row[i]);
        lv_obj_set_style_text_font(s_row_aux[i], UI_FONT_SMALL, 0);
        lv_obj_align(s_row_aux[i], LV_ALIGN_RIGHT_MID, -16, 0);
    }
}

static uint8_t page_count(const card_list_t *list)
{
    return list->count == 0 ? 1 : (uint8_t)((list->count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
}

static void render_locked(void)
{
    if (!s_root) {
        return;
    }
    const card_list_t *list = &s_lists[s_kind];
    const uint8_t pages = page_count(list);
    if (s_page_index >= pages) {
        s_page_index = pages - 1;
    }
    lv_label_set_text(s_title, TITLES[s_kind]);
    /* Wo man steht: Karte und, wenn es mehr als eine gibt, Seite. */
    char where[24];
    if (pages > 1) {
        snprintf(where, sizeof(where), "%u/%u  ·  %u/%u", (unsigned)(s_kind + 1),
                 (unsigned)UI_CARD_COUNT, (unsigned)(s_page_index + 1), (unsigned)pages);
    } else {
        snprintf(where, sizeof(where), "%u/%u", (unsigned)(s_kind + 1), (unsigned)UI_CARD_COUNT);
    }
    lv_label_set_text(s_page, where);

    if (list->count == 0) {
        lv_label_set_text(s_empty, EMPTY_HINTS[s_kind]);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < ROWS_PER_PAGE; i++) {
        const int index = s_page_index * ROWS_PER_PAGE + i;
        if (index >= list->count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const card_item_t *item = &list->items[index];
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
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
    }
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
    s_lists[kind].rev = rev;
    ui_status_unlock();
}

void ui_cards_add_item(ui_card_kind_t kind, const char *text, const char *aux)
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
    s_kind = kind;
    s_page_index = 0;
    s_visible = true;
    render_locked();
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
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
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
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
    switch ((voice_board_gesture_t)gesture) {
    case VOICE_BOARD_SWIPE_LEFT:
        s_kind = (ui_card_kind_t)((s_kind + 1) % UI_CARD_COUNT);
        s_page_index = 0;
        render_locked();
        break;
    case VOICE_BOARD_SWIPE_RIGHT:
        s_kind = (ui_card_kind_t)((s_kind + UI_CARD_COUNT - 1) % UI_CARD_COUNT);
        s_page_index = 0;
        render_locked();
        break;
    case VOICE_BOARD_SWIPE_UP:
        /* Naechste Seite; nach der letzten wieder die erste. */
        s_page_index = (uint8_t)((s_page_index + 1) % page_count(list));
        render_locked();
        break;
    case VOICE_BOARD_SWIPE_DOWN:
        if (s_page_index > 0) {
            s_page_index--;
            render_locked();
        } else {
            action = UI_CARDS_CLOSE;
        }
        break;
    case VOICE_BOARD_TAP:
        for (int i = 0; i < ROWS_PER_PAGE; i++) {
            if (lv_obj_has_flag(s_row[i], LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_area_t area;
            lv_obj_get_coords(s_row[i], &area);
            if (x < area.x1 || x > area.x2 || y < area.y1 - 3 || y > area.y2 + 3) {
                continue;
            }
            const int index = s_page_index * ROWS_PER_PAGE + i;
            card_item_t *item = &list->items[index];
            /*
             * Abhaken geht nur einmal -- ein zweites Tippen auf dieselbe
             * Zeile schickte sonst zweimal "erledigt". Schalter duerfen hin
             * und her.
             */
            if (item->done && s_kind != UI_CARD_SWITCHES) {
                break;
            }
            item->done = s_kind == UI_CARD_SWITCHES ? !item->done : true;
            if (hit) {
                hit->kind = s_kind;
                hit->rev = list->rev;
                hit->index = (uint8_t)index;
            }
            action = UI_CARDS_ITEM;
            render_locked();
            break;
        }
        break;
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
