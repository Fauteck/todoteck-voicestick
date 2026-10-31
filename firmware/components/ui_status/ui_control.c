#include "ui_control.h"

#include <stdio.h>

#include "sdkconfig.h"
#include "ui_status_internal.h"

#if CONFIG_VOICE_BOARD_WATCH206
#include "ui_layout_watch206.h"

#define TILE_GAP 12
#define TILE_COLS 2
#define TILE_ROWS 3

static lv_obj_t *s_root;
static lv_obj_t *s_tile[UI_CONTROL_COUNT];
static lv_obj_t *s_tile_title[UI_CONTROL_COUNT];
static lv_obj_t *s_tile_value[UI_CONTROL_COUNT];
static lv_obj_t *s_torch;
static bool s_visible;
static bool s_torch_on;

static const char *const TITLES[UI_CONTROL_COUNT] = {
    "Helligkeit", "Lautlos", "Kino", "Sparen", "Lampe", "Akku",
};

static void create_locked(void)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, CONTENT_W, CONTENT_H);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    const int32_t top = HEADER_H + 10;
    const int32_t w = (CONTENT_W - (TILE_COLS - 1) * TILE_GAP) / TILE_COLS;
    const int32_t h = (CONTENT_H - top - (TILE_ROWS - 1) * TILE_GAP) / TILE_ROWS;
    for (int i = 0; i < UI_CONTROL_COUNT; i++) {
        s_tile[i] = lv_obj_create(s_root);
        lv_obj_remove_style_all(s_tile[i]);
        lv_obj_set_size(s_tile[i], w, h);
        lv_obj_align(s_tile[i], LV_ALIGN_TOP_LEFT, (i % TILE_COLS) * (w + TILE_GAP),
                     top + (i / TILE_COLS) * (h + TILE_GAP));
        lv_obj_set_style_radius(s_tile[i], 28, 0);
        lv_obj_set_style_bg_opa(s_tile[i], LV_OPA_COVER, 0);

        s_tile_title[i] = lv_label_create(s_tile[i]);
        lv_obj_set_style_text_font(s_tile_title[i], UI_FONT_SMALL, 0);
        lv_label_set_text(s_tile_title[i], TITLES[i]);
        lv_obj_align(s_tile_title[i], LV_ALIGN_TOP_LEFT, 18, 16);

        s_tile_value[i] = lv_label_create(s_tile[i]);
        lv_obj_set_style_text_font(s_tile_value[i], &todoteck_watch_font_26, 0);
        lv_obj_align(s_tile_value[i], LV_ALIGN_BOTTOM_LEFT, 18, -16);
    }

    s_torch = lv_obj_create(ui_status_screen());
    lv_obj_remove_style_all(s_torch);
    /* Ueber den Rand des Inhaltsbereichs hinaus: der ganze Schirm leuchtet. */
    lv_obj_set_size(s_torch, LCD_H_RES, LCD_V_RES);
    lv_obj_align(s_torch, LV_ALIGN_TOP_LEFT, -UI_SCREEN_PAD, -UI_SCREEN_PAD);
    lv_obj_set_style_bg_opa(s_torch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_torch, lv_color_hex(0xffffff), 0);
    lv_obj_add_flag(s_torch, LV_OBJ_FLAG_HIDDEN);
}

static void tile_style(int i, bool on, const char *value)
{
    lv_obj_set_style_bg_color(s_tile[i], on ? lv_color_hex(0x2ecc71) : lv_color_hex(0x1b2430), 0);
    const lv_color_t text = on ? lv_color_hex(0x000000) : lv_color_hex(0xf2f2f2);
    lv_obj_set_style_text_color(s_tile_title[i], text, 0);
    lv_obj_set_style_text_color(s_tile_value[i], text, 0);
    lv_label_set_text(s_tile_value[i], value);
}

static void render_locked(const ui_control_state_t *st)
{
    if (!s_root || !st) {
        return;
    }
    tile_style(UI_CONTROL_BRIGHTNESS, false, st->brightness ? st->brightness : "");
    tile_style(UI_CONTROL_SILENT, st->silent, st->silent ? "An" : "Aus");
    tile_style(UI_CONTROL_CINEMA, st->cinema, st->cinema ? "An" : "Aus");
    tile_style(UI_CONTROL_SAVER, st->saver, st->saver ? "An" : "Aus");
    tile_style(UI_CONTROL_TORCH, false, "Tippen");
    char battery[16];
    if (st->battery_percent < 0) {
        snprintf(battery, sizeof(battery), "--%%");
    } else {
        snprintf(battery, sizeof(battery), "%d %%%s", st->battery_percent,
                 st->charging ? " +" : "");
    }
    tile_style(UI_CONTROL_BATTERY, false, battery);
}
#endif

void ui_control_show(const ui_control_state_t *state)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    create_locked();
    render_locked(state);
    s_visible = true;
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);
    ui_status_unlock();
#else
    (void)state;
#endif
}

void ui_control_update(const ui_control_state_t *state)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    if (s_visible) {
        render_locked(state);
    }
    ui_status_unlock();
#else
    (void)state;
#endif
}

void ui_control_hide(void)
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

bool ui_control_visible(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_visible;
#else
    return false;
#endif
}

int ui_control_tile_at(int16_t x, int16_t y)
{
#if CONFIG_VOICE_BOARD_WATCH206
    int hit = -1;
    ui_status_lock();
    for (int i = 0; s_visible && i < UI_CONTROL_COUNT; i++) {
        lv_area_t area;
        lv_obj_get_coords(s_tile[i], &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            hit = i;
            break;
        }
    }
    ui_status_unlock();
    return hit;
#else
    (void)x;
    (void)y;
    return -1;
#endif
}

void ui_torch_set(bool on)
{
#if CONFIG_VOICE_BOARD_WATCH206
    ui_status_lock();
    create_locked();
    s_torch_on = on;
    if (on) {
        lv_obj_remove_flag(s_torch, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_torch);
    } else {
        lv_obj_add_flag(s_torch, LV_OBJ_FLAG_HIDDEN);
    }
    ui_status_unlock();
#else
    (void)on;
#endif
}

bool ui_torch_on(void)
{
#if CONFIG_VOICE_BOARD_WATCH206
    return s_torch_on;
#else
    return false;
#endif
}
