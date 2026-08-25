#include "ui_status.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "stick_s3_board.h"
#include "ui_status_icons.h"

static const char *TAG = "ui_status";

#define LCD_HOST SPI2_HOST

#define LCD_H_RES 135
#define LCD_V_RES 240
#define LCD_X_GAP 52
#define LCD_Y_GAP 40

#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_PWM_MAX 255
#define LCD_BACKLIGHT_DEFAULT 128

#define LVGL_DRAW_BUF_LINES 24
#define LVGL_TICK_PERIOD_MS 10
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE (5 * 1024)
#define LVGL_TASK_PRIORITY 2

#define LCD_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
/*
 * Eigene Schriften statt der eingebauten LVGL-Montserrat: die bringen nur
 * ASCII mit, "Rueckspuelen" erschiene also mit Luecken. Diese hier sind mit
 * lv_font_conv aus Montserrat erzeugt und decken zusaetzlich Latin-1 ab,
 * also Umlaute und Eszett.
 */
LV_FONT_DECLARE(todoteck_font_16);
LV_FONT_DECLARE(todoteck_font_10);

/*
 * Wofuer der untere Text steht — und damit, wieviel Flaeche er bekommt.
 *
 * `UI_TEXT_HINT` ist Beiwerk ("Halten zum Sprechen", der Geraetename beim
 * Koppeln): klein, gedaempft, unter Tecki. `UI_TEXT_MESSAGE` ist die Auskunft
 * selbst — die Antwort der Bruecke, ein Zwischenstand, eine Fehlermeldung.
 * Die gibt es nur einmal, und sie muss lesbar sein.
 */
typedef enum {
    UI_TEXT_HINT,
    UI_TEXT_MESSAGE,
} ui_text_kind_t;

#define UI_STATUS_TEXT_MAX 32
#define UI_HINT_TEXT_MAX 192

static _lock_t s_lvgl_lock;
static bool s_ready;
static lv_display_t *s_display;
static lv_obj_t *s_screen;
static lv_obj_t *s_top_label;
static lv_obj_t *s_ble_dot;
static lv_obj_t *s_status_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_battery_shell;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_battery_tip;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_meter_track;
static lv_obj_t *s_meter_fill;
static lv_obj_t *s_time_track;
static lv_obj_t *s_time_fill;
static ui_status_icons_t s_icons;
static ui_status_icon_scene_t s_scene = UI_STATUS_ICON_BOOT;
/* Beiwerk oder Auskunft — entscheidet ueber Schriftgroesse und Platz. */
static ui_text_kind_t s_text_kind = UI_TEXT_HINT;
static char s_status_text[UI_STATUS_TEXT_MAX] = "Startet";
static char s_hint_text[UI_HINT_TEXT_MAX] = "einen Moment";
static char s_idle_hint_text[UI_HINT_TEXT_MAX] = "Halten zum Sprechen";
static char s_device_name[24] = "BLE";
static bool s_dimmed;
/*
 * Der Verbindungszustand kam bisher aus der Szene: "Koppeln" faerbte den
 * Punkt blau, alles andere gruen. Das log, sobald die Bruecke waehrend einer
 * Anzeige wegbrach — man sah es erst beim naechsten Sprechversuch. Jetzt
 * traegt der Punkt den echten Zustand, unabhaengig davon, was auf dem
 * Display steht.
 */
static bool s_link_connected;
/*
 * Die letzte Antwort ueberlebt den Bildschirmwechsel: nach Ruhezustand oder
 * Fehler ist sie sonst weg, obwohl man sie vielleicht nur nicht rechtzeitig
 * gelesen hat. Die Seitentaste holt sie zurueck.
 */
static char s_last_answer[UI_HINT_TEXT_MAX];

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *display = (lv_display_t *)user_ctx;
    lv_display_flush_ready(display);
    return false;
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    const int x1 = area->x1;
    const int x2 = area->x2;
    const int y1 = area->y1;
    const int y2 = area->y2;

    lv_draw_sw_rgb565_swap(px_map, (x2 - x1 + 1) * (y2 - y1 + 1));
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    while (true) {
        _lock_acquire(&s_lvgl_lock);
        uint32_t delay_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);

        delay_ms = MAX(delay_ms, LVGL_TASK_MIN_DELAY_MS);
        delay_ms = MIN(delay_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(delay_ms * 1000);
    }
}

static lv_obj_t *create_blob(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    return obj;
}

static void create_battery_ui(lv_obj_t *screen)
{
    s_battery_shell = lv_obj_create(screen);
    lv_obj_remove_style_all(s_battery_shell);
    lv_obj_set_size(s_battery_shell, 20, 10);
    lv_obj_set_style_radius(s_battery_shell, 3, 0);
    lv_obj_set_style_border_width(s_battery_shell, 1, 0);
    lv_obj_set_style_border_color(s_battery_shell, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_bg_opa(s_battery_shell, LV_OPA_TRANSP, 0);
    lv_obj_align(s_battery_shell, LV_ALIGN_TOP_RIGHT, -31, 4);

    s_battery_fill = lv_obj_create(s_battery_shell);
    lv_obj_remove_style_all(s_battery_fill);
    lv_obj_set_size(s_battery_fill, 12, 6);
    lv_obj_set_style_radius(s_battery_fill, 2, 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(0x67c59b), 0);
    lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 2, 0);

    s_battery_tip = lv_obj_create(screen);
    lv_obj_remove_style_all(s_battery_tip);
    lv_obj_set_size(s_battery_tip, 2, 5);
    lv_obj_set_style_radius(s_battery_tip, 2, 0);
    lv_obj_set_style_bg_opa(s_battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_battery_tip, lv_color_hex(0x675f71), 0);
    lv_obj_align_to(s_battery_tip, s_battery_shell, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_battery_label = lv_label_create(screen);
    lv_label_set_text(s_battery_label, "--%");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x675f71), 0);
    lv_obj_set_style_text_font(s_battery_label, &todoteck_font_10, 0);
    lv_label_set_long_mode(s_battery_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_battery_label, 28);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, 0, 4);
}

#define METER_WIDTH 100
#define METER_LEVEL_HEIGHT 6
#define METER_TIME_HEIGHT 3
#define METER_LEVEL_Y 142
#define METER_TIME_Y 152

/*
 * Zwei Balken, die nur waehrend der Aufnahme sichtbar sind.
 *
 * Oben der Pegel: er beantwortet die Frage "hoert das Ding mich ueberhaupt?",
 * die man sonst erst nach dem Absenden beantwortet bekommt — und dann mit
 * einer leeren Transkription. Darunter die Restzeit bis zur
 * 30-Sekunden-Grenze, die der Server ohnehin zieht; ein Satz, der genau an
 * der Grenze abbricht, ist teurer als ein Balken, der vorher warnt.
 */
static void create_meters(lv_obj_t *screen)
{
    s_meter_track = lv_obj_create(screen);
    lv_obj_remove_style_all(s_meter_track);
    lv_obj_set_size(s_meter_track, METER_WIDTH, METER_LEVEL_HEIGHT);
    lv_obj_set_style_radius(s_meter_track, METER_LEVEL_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_meter_track, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(s_meter_track, lv_color_hex(0x9a8fa4), 0);
    lv_obj_align(s_meter_track, LV_ALIGN_TOP_MID, 0, METER_LEVEL_Y);
    lv_obj_add_flag(s_meter_track, LV_OBJ_FLAG_HIDDEN);

    s_meter_fill = lv_obj_create(s_meter_track);
    lv_obj_remove_style_all(s_meter_fill);
    lv_obj_set_size(s_meter_fill, 2, METER_LEVEL_HEIGHT);
    lv_obj_set_style_radius(s_meter_fill, METER_LEVEL_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_meter_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_meter_fill, lv_color_hex(0x55c98a), 0);
    lv_obj_align(s_meter_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_time_track = lv_obj_create(screen);
    lv_obj_remove_style_all(s_time_track);
    lv_obj_set_size(s_time_track, METER_WIDTH, METER_TIME_HEIGHT);
    lv_obj_set_style_radius(s_time_track, METER_TIME_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_time_track, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(s_time_track, lv_color_hex(0x9a8fa4), 0);
    lv_obj_align(s_time_track, LV_ALIGN_TOP_MID, 0, METER_TIME_Y);
    lv_obj_add_flag(s_time_track, LV_OBJ_FLAG_HIDDEN);

    s_time_fill = lv_obj_create(s_time_track);
    lv_obj_remove_style_all(s_time_fill);
    lv_obj_set_size(s_time_fill, METER_WIDTH, METER_TIME_HEIGHT);
    lv_obj_set_style_radius(s_time_fill, METER_TIME_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(s_time_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_time_fill, lv_color_hex(0xf2b23c), 0);
    lv_obj_align(s_time_fill, LV_ALIGN_LEFT_MID, 0, 0);
}

/*
 * Layoutmasse im Inhaltsbereich: 135x240 abzueglich 8 Pixel Rand ringsum.
 */
#define CONTENT_W (LCD_H_RES - 16)
#define CONTENT_H (LCD_V_RES - 16)
#define TEXT_BOTTOM_Y (CONTENT_H - 10)
#define STATUS_Y_WITH_TECKI 168
#define STATUS_Y_TEXT_ONLY 18
#define TEXT_GAP 6
#define TEXT_LINE_SPACE 2

typedef struct {
    bool tecki;             /* bleibt die Figur stehen? */
    const lv_font_t *font;  /* Schrift des unteren Texts */
    int32_t status_y;       /* Oberkante der Statuszeile */
} text_layout_t;

static int32_t text_height(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text ? text : "", font, 0, TEXT_LINE_SPACE, CONTENT_W,
                     LV_TEXT_FLAG_NONE);
    return size.y;
}

/*
 * Wo Statuszeile und Text hinkommen — und ob Tecki dabei stehen bleibt.
 *
 * Der Anlass steht auf einem Foto vom Handgelenk: "Aufgabe angelegt:
 * AliExpress Bestellung · Faellig: Di., 25.08.2026 · Projekt: Home Lab"
 * braucht auf 119 Pixel Breite sieben Zeilen. Die wuchsen vom unteren Rand
 * nach oben ueber die Statuszeile und mitten durch die Figur hindurch —
 * lesbar war danach weder das eine noch das andere.
 *
 * Die Entscheidung faellt deshalb am gemessenen Text, nicht an der Szene:
 * Passt er unter Tecki, bleibt alles wie gehabt. Passt er nicht, weicht
 * Tecki. Er sagt ohnehin dasselbe wie die Statuszeile daneben; der Text sagt
 * etwas, das es nur einmal gibt.
 */
static text_layout_t plan_text_layout(const char *status, const char *text, ui_text_kind_t kind)
{
    const int32_t status_h = status && status[0] ? todoteck_font_16.line_height : 0;
    const int32_t room_under_tecki = TEXT_BOTTOM_Y - (STATUS_Y_WITH_TECKI + status_h);

    /* Eine Auskunft, die in eine Zeile passt, darf gross bleiben. */
    if (kind == UI_TEXT_MESSAGE && text_height(text, &todoteck_font_16) <= room_under_tecki) {
        return (text_layout_t){ true, &todoteck_font_16, STATUS_Y_WITH_TECKI };
    }
    if (text_height(text, &todoteck_font_10) <= room_under_tecki) {
        return (text_layout_t){ true, &todoteck_font_10, STATUS_Y_WITH_TECKI };
    }

    /*
     * Ohne Figur: Statuszeile nach oben unter die Kopfzeile, der Rest gehoert
     * dem Text. Die Zeile wird immer eingerechnet, auch wenn sie gerade leer
     * ist — sonst spraenge das Layout, sobald sie doch etwas traegt.
     */
    const int32_t top = STATUS_Y_TEXT_ONLY + todoteck_font_16.line_height + TEXT_GAP;
    const int32_t room_full = TEXT_BOTTOM_Y - top;
    const lv_font_t *font = text_height(text, &todoteck_font_16) <= room_full
                                ? &todoteck_font_16
                                : &todoteck_font_10;
    return (text_layout_t){ false, font, STATUS_Y_TEXT_ONLY };
}

static void render_scene_locked(ui_status_icon_scene_t scene, const char *status, const char *hint)
{
    if (!s_ready) {
        return;
    }

    ui_status_icons_apply(&s_icons, scene);

    const char *body_text = hint ? hint : "";
    const text_layout_t plan = plan_text_layout(status, body_text, s_text_kind);

    /*
     * Ohne Tecki traegt die Statuszeile allein, dass etwas schiefging — die
     * Fehlerszene laesst sie sonst absichtlich leer, weil die rote Figur mit
     * den Kreuzaugen es sagt. Ist die weg, muss es jemand anderes sagen.
     */
    const char *status_shown = status ? status : "";
    if (!plan.tecki && !status_shown[0] && scene == UI_STATUS_ICON_ERROR) {
        status_shown = "Fehler";
    }

    ui_status_icons_show(&s_icons, plan.tecki);
    lv_label_set_text(s_status_label, status_shown);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, plan.status_y);

    lv_label_set_text(s_hint_label, body_text);
    lv_obj_set_style_text_font(s_hint_label, plan.font, 0);
    if (plan.tecki) {
        lv_obj_set_height(s_hint_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    } else {
        /*
         * Feste Hoehe mit LV_LABEL_LONG_DOT: Was selbst in der kleinen
         * Schrift nicht mehr auf den Schirm passt, endet mit Auslassungs-
         * punkten statt mitten im Wort abgeschnitten zu sein.
         */
        const int32_t top = plan.status_y + todoteck_font_16.line_height + TEXT_GAP;
        lv_obj_set_height(s_hint_label, TEXT_BOTTOM_Y - top);
        lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(s_hint_label, LV_ALIGN_TOP_MID, 0, top);
    }

    const bool resting = scene == UI_STATUS_ICON_RESTING;
    const bool error = scene == UI_STATUS_ICON_ERROR;
    const bool recording = scene == UI_STATUS_ICON_RECORDING;

    if (recording && plan.tecki) {
        lv_obj_remove_flag(s_meter_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_time_track, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_meter_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_track, LV_OBJ_FLAG_HIDDEN);
    }

    lv_color_t bg = resting ? lv_color_hex(0x1b2430) : lv_color_hex(0xfff7ed);
    lv_color_t text = resting ? lv_color_hex(0xe8eef7) : lv_color_hex(0x3f3440);
    lv_color_t muted = resting ? lv_color_hex(0xa8bad2) : lv_color_hex(0x7f7180);
    lv_color_t hint_color = resting ? lv_color_hex(0xdfe9f8) : muted;
    /*
     * Gefuellt gruen heisst verbunden, ein leerer Ring heisst getrennt — der
     * Unterschied ist auch am Handgelenk und aus dem Augenwinkel zu sehen,
     * anders als zwei aehnlich helle Farbtoene.
     */
    lv_color_t ble = error ? lv_color_hex(0xf97373) :
                     s_link_connected ? lv_color_hex(0x55c98a) :
                     lv_color_hex(0x8fb8ff);

    lv_obj_set_style_bg_color(s_screen, bg, 0);
    lv_obj_set_style_text_color(s_screen, text, 0);
    lv_obj_set_style_text_color(s_top_label, muted, 0);
    lv_obj_set_style_bg_color(s_ble_dot, ble, 0);
    lv_obj_set_style_bg_opa(s_ble_dot, s_link_connected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ble_dot, s_link_connected ? 0 : 2, 0);
    lv_obj_set_style_border_color(s_ble_dot, ble, 0);
    lv_obj_set_style_text_color(s_status_label, text, 0);
    lv_obj_set_style_text_color(s_hint_label, plan.tecki ? hint_color : text, 0);
    lv_obj_set_style_text_color(s_battery_label, muted, 0);
    lv_obj_set_style_border_color(s_battery_shell, muted, 0);
    lv_obj_set_style_bg_color(s_battery_tip, muted, 0);

    ui_status_icons_start_anim(&s_icons, scene);
}

static void render_current_locked(void)
{
    if (s_dimmed) {
        const ui_text_kind_t kind = s_text_kind;
        s_text_kind = UI_TEXT_HINT;
        render_scene_locked(UI_STATUS_ICON_RESTING, "Ruht", "");
        s_text_kind = kind;
    } else {
        render_scene_locked(s_scene, s_status_text, s_hint_text);
    }
}

static void create_status_ui(void)
{
    s_screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xfff7ed), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(0x3f3440), 0);
    lv_obj_set_style_pad_all(s_screen, 8, 0);

    s_top_label = lv_label_create(s_screen);
    lv_label_set_text(s_top_label, s_device_name);
    lv_obj_set_style_text_font(s_top_label, &todoteck_font_10, 0);
    lv_obj_set_style_text_color(s_top_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_long_mode(s_top_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_top_label, 66);
    lv_obj_align(s_top_label, LV_ALIGN_TOP_LEFT, 12, 4);

    s_ble_dot = create_blob(s_screen, 8, 8, lv_color_hex(0x8fb8ff));
    lv_obj_align(s_ble_dot, LV_ALIGN_TOP_LEFT, 0, 6);

    create_battery_ui(s_screen);
    create_meters(s_screen);
    ui_status_icons_create(&s_icons, s_screen);

    s_status_label = lv_label_create(s_screen);
    lv_label_set_text(s_status_label, s_status_text);
    lv_obj_set_style_text_font(s_status_label, &todoteck_font_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x3f3440), 0);
    lv_obj_set_width(s_status_label, LCD_H_RES - 16);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 168);

    s_hint_label = lv_label_create(s_screen);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_hint_label, TEXT_LINE_SPACE, 0);
    lv_obj_set_width(s_hint_label, LCD_H_RES - 16);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x7f7180), 0);
    lv_label_set_text(s_hint_label, s_hint_text);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_ready = true;
    render_current_locked();
}

static size_t utf8_decode(const unsigned char *p, uint32_t *codepoint)
{
    if (p[0] < 0x80) {
        *codepoint = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                     (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                     ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    /* Kaputtes Byte: eins weiter, sonst kaeme die Schleife nicht voran. */
    *codepoint = 0xFFFD;
    return 1;
}

/*
 * Deckungsbereich der beiden Schriften: ASCII ab dem Leerzeichen und Latin-1
 * ohne dessen Steuerbereich. Steht als Bedingung hier, weil die Alternative
 * — jede Schrift nach jedem Zeichen fragen — teurer ist als die zwei
 * Vergleiche und dasselbe Ergebnis liefert.
 */
static bool font_has_codepoint(uint32_t codepoint)
{
    return (codepoint >= 32 && codepoint <= 126) ||
           (codepoint >= 160 && codepoint <= 172) ||
           (codepoint >= 174 && codepoint <= 255);
}

/*
 * Typografie, die keine der beiden Schriften enthaelt, die aber in jedem Text
 * von draussen steckt: Gedankenstrich, typografische Anfuehrungszeichen,
 * Auslassungspunkte. Ersatzlos zu streichen ergaebe "Aufgabe heute" statt
 * "Aufgabe - heute" — der ASCII-Ersatz sagt dasselbe wie das Original.
 */
static const char *ascii_substitute(uint32_t codepoint)
{
    switch (codepoint) {
    case 0x2010: /* Bindestrich */
    case 0x2011:
    case 0x2012:
    case 0x2013: /* Halbgeviertstrich */
    case 0x2014: /* Geviertstrich */
    case 0x2015:
    case 0x2212: /* Minus */
        return "-";
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
        return "'";
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
        return "\"";
    case 0x2022: /* Aufzaehlungspunkt */
    case 0x2027:
        return "·";
    case 0x2026: /* Auslassungspunkte */
        return "...";
    case 0x2192: /* Pfeil */
        return "->";
    default:
        return NULL;
    }
}

/*
 * Kopiert Text so, wie das Display ihn zeigen kann: hoechstens `size - 1`
 * Byte, nie mitten in ein UTF-8-Zeichen — und ohne Zeichen, die die Schrift
 * nicht kennt.
 *
 * Beide Regeln stammen aus der Praxis. `strlcpy` schnitte einen Umlaut an der
 * Bytegrenze mittendurch; LVGL laeuft beim Dekodieren dann ins Leere. Und die
 * Antworten der Bruecke tragen Emoji ("Aufgabe angelegt · Faellig: ..." kommt
 * mit Haken-, Kalender- und Ordnersymbol an), die keine der beiden Schriften
 * enthaelt: Auf dem Geraet standen dort leere Rechtecke mitten im Satz. Sie
 * fliegen raus, samt der Luecke, die sie hinterlassen — sonst bliebe der
 * doppelte Abstand stehen, den sie umgab.
 */
static void copy_utf8_display(char *dst, const char *src, size_t size)
{
    if (size == 0) {
        return;
    }
    size_t out = 0;
    bool pending_space = false;
    const unsigned char *p = (const unsigned char *)(src ? src : "");

    while (*p != '\0') {
        uint32_t codepoint = 0;
        const size_t len = utf8_decode(p, &codepoint);
        const unsigned char *start = p;
        p += len;

        if (codepoint == '\n') {
            if (out + 1 >= size) {
                break;
            }
            dst[out++] = '\n';
            pending_space = false;
            continue;
        }
        /* 0x00A0 ist ein Leerzeichen, auch wenn die Schrift es kennt. */
        if (codepoint == ' ' || codepoint == '\t' || codepoint == 0x00A0) {
            pending_space = out > 0;
            continue;
        }

        const char *substitute = NULL;
        if (!font_has_codepoint(codepoint)) {
            substitute = ascii_substitute(codepoint);
            if (substitute == NULL) {
                continue;
            }
        }
        const char *from = substitute ? substitute : (const char *)start;
        const size_t bytes = substitute ? strlen(substitute) : len;

        if (out + (pending_space ? 1 : 0) + bytes + 1 > size) {
            break;
        }
        if (pending_space) {
            dst[out++] = ' ';
            pending_space = false;
        }
        memcpy(dst + out, from, bytes);
        out += bytes;
    }
    dst[out] = '\0';
}

static void set_scene(ui_status_icon_scene_t scene, const char *status, const char *hint,
                      ui_text_kind_t kind)
{
    _lock_acquire(&s_lvgl_lock);
    s_scene = scene;
    s_text_kind = kind;
    copy_utf8_display(s_status_text, status ? status : "", sizeof(s_status_text));
    copy_utf8_display(s_hint_text, hint ? hint : "", sizeof(s_hint_text));
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

esp_err_t ui_status_init(void)
{
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        // RC_FAST clock remains active during light sleep, preventing backlight flicker
        .clk_cfg = LEDC_USE_RC_FAST_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "configure backlight timer");

    const ledc_channel_config_t backlight_channel = {
        .gpio_num = STICK_S3_PIN_LCD_BL,
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        .flags.output_invert = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "configure backlight channel");

    spi_bus_config_t bus_config = {
        .sclk_io_num = STICK_S3_PIN_LCD_SCK,
        .mosi_io_num = STICK_S3_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "initialize lcd spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = STICK_S3_PIN_LCD_DC,
        .cs_gpio_num = STICK_S3_PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io),
                        TAG, "create lcd panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = STICK_S3_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &panel),
                        TAG, "create st7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "invert panel colors");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), TAG, "mirror panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP), TAG, "set panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "turn display on");

    lv_init();
    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_display, ESP_ERR_NO_MEM, TAG, "create lvgl display");

    const size_t draw_buffer_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "allocate lvgl draw buffers");

    lv_display_set_buffers(s_display, buf1, buf2, draw_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_display, panel);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, s_display),
                        TAG, "register lcd callbacks");

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &tick_timer), TAG, "create lvgl tick");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000),
                        TAG, "start lvgl tick");

    _lock_acquire(&s_lvgl_lock);
    create_status_ui();
    _lock_release(&s_lvgl_lock);

    BaseType_t task_ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE,
                                     NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create lvgl task");

    ESP_RETURN_ON_ERROR(ui_status_set_brightness(LCD_BACKLIGHT_DEFAULT), TAG, "set backlight");
    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}

esp_err_t ui_status_set_brightness(uint8_t brightness)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE,
                                      LCD_BACKLIGHT_LEDC_CHANNEL,
                                      brightness),
                        TAG, "set backlight duty");
    return ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL);
}

void ui_status_prepare_deep_sleep(void)
{
    (void)ui_status_set_brightness(0);

    _lock_acquire(&s_lvgl_lock);
    if (s_display) {
        esp_lcd_panel_handle_t panel = lv_display_get_user_data(s_display);
        if (panel) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel, false));
        }
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_device_name(const char *device_name)
{
    copy_utf8_display(s_device_name, device_name && device_name[0] ? device_name : "BLE",
                      sizeof(s_device_name));

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
        lv_label_set_text(s_top_label, s_device_name);
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_advertising(void)
{
    ESP_LOGD(TAG, "advertising");
    set_scene(UI_STATUS_ICON_PAIRING, "Koppeln", "Brücke starten", UI_TEXT_HINT);
}

void ui_status_set_pairing(const char *device_name)
{
    ESP_LOGD(TAG, "pairing %s", device_name ? device_name : "");
    ui_status_set_device_name(device_name);
    set_scene(UI_STATUS_ICON_PAIRING, "Koppeln", device_name ? device_name : "VS-0000",
              UI_TEXT_HINT);
}

void ui_status_set_idle_hint(const char *hint)
{
    _lock_acquire(&s_lvgl_lock);
    copy_utf8_display(s_idle_hint_text, hint && hint[0] ? hint : "Halten zum Sprechen", sizeof(s_idle_hint_text));
    if (s_scene == UI_STATUS_ICON_IDLE) {
        copy_utf8_display(s_hint_text, s_idle_hint_text, sizeof(s_hint_text));
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_idle(void)
{
    ESP_LOGD(TAG, "idle");
    set_scene(UI_STATUS_ICON_IDLE, "Bereit", s_idle_hint_text, UI_TEXT_HINT);
}

/*
 * Wie ui_status_set_idle(), zeigt aber die Antwort der Bruecke statt des
 * Standard-Hinweises. Der Text bleibt stehen, bis der naechste Turn beginnt —
 * wer ihn verpasst hat, soll ihn noch lesen koennen.
 *
 * Ohne das bliebe die eigentliche Auskunft ("Aufgabe angelegt: Pool
 * rueckspuelen") unsichtbar: sie kam schon immer ueber die Funkstrecke, wurde
 * bei ready aber verworfen.
 */
void ui_status_set_idle_text(const char *text)
{
    if (!text || !text[0]) {
        ui_status_set_idle();
        return;
    }
    ESP_LOGD(TAG, "idle mit Text");
    _lock_acquire(&s_lvgl_lock);
    copy_utf8_display(s_last_answer, text, sizeof(s_last_answer));
    _lock_release(&s_lvgl_lock);
    set_scene(UI_STATUS_ICON_IDLE, "Bereit", text, UI_TEXT_MESSAGE);
}

/*
 * Holt die zuletzt gezeigte Antwort zurueck auf den Bildschirm.
 *
 * Der Fall dahinter: Das Display dimmt nach 30 Sekunden und schlaeft nach
 * fuenf Minuten ganz ein; wer den Stick erst danach ansieht, hat die Antwort
 * nie gelesen. Sie noch einmal zu zeigen kostet nichts — sie erneut zu
 * erfragen kostet Transkription, Intent und womoeglich eine zweite Aufgabe.
 */
void ui_status_show_last_answer(void)
{
    _lock_acquire(&s_lvgl_lock);
    const bool have = s_last_answer[0] != '\0';
    _lock_release(&s_lvgl_lock);

    if (!have) {
        set_scene(UI_STATUS_ICON_IDLE, "Bereit", "Noch keine Antwort", UI_TEXT_HINT);
        return;
    }
    ESP_LOGD(TAG, "letzte Antwort erneut");
    set_scene(UI_STATUS_ICON_IDLE, "Bereit", s_last_answer, UI_TEXT_MESSAGE);
}

/*
 * Abbruch: die Aufnahme laeuft nicht weiter und es geht nichts an Todoteck.
 * Bisher kehrte das Geraet dabei stumm in den Bereitzustand zurueck — nicht
 * zu unterscheiden von "gesendet und nichts gefunden".
 */
void ui_status_set_cancelled(void)
{
    ESP_LOGI(TAG, "abgebrochen");
    set_scene(UI_STATUS_ICON_IDLE, "Abgebrochen", "Nichts gesendet", UI_TEXT_HINT);
}

/*
 * Ein Druck, der zu kurz war, um etwas zu sagen: die Aufnahme wird verworfen,
 * nichts geht an Todoteck. Die Zeile sagt gleich, was stattdessen zu tun ist —
 * "Abgebrochen" allein liesse offen, warum.
 */
void ui_status_set_press_too_short(void)
{
    ESP_LOGI(TAG, "Druck zu kurz");
    set_scene(UI_STATUS_ICON_IDLE, "Zu kurz", "Taste halten, solange du sprichst",
              UI_TEXT_HINT);
}

/*
 * Der echte Verbindungszustand zur Bruecke. Faerbt nur den Punkt in der
 * Kopfzeile — die Szene bleibt, wie sie ist.
 */
void ui_status_set_link(bool connected)
{
    _lock_acquire(&s_lvgl_lock);
    if (s_link_connected != connected) {
        s_link_connected = connected;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

/*
 * Pegel und Restzeit waehrend der Aufnahme, beide in Prozent. Wird vom
 * Aufnahme-Takt in main.c gespeist; ausserhalb der Aufnahme sind die Balken
 * ausgeblendet, der Aufruf ist dann folgenlos.
 */
void ui_status_set_recording_meter(uint8_t level_percent, uint8_t remaining_percent)
{
    if (level_percent > 100) {
        level_percent = 100;
    }
    if (remaining_percent > 100) {
        remaining_percent = 100;
    }

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
        lv_obj_set_width(s_meter_fill, MAX(2, (METER_WIDTH * level_percent) / 100));
        lv_obj_set_width(s_time_fill, MAX(1, (METER_WIDTH * remaining_percent) / 100));
        lv_obj_set_style_bg_color(s_time_fill,
                                  remaining_percent <= 20 ? lv_color_hex(0xf97373) :
                                  lv_color_hex(0xf2b23c),
                                  0);
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_idle_dimmed(bool dimmed)
{
    ESP_LOGD(TAG, "idle dimmed=%d", dimmed);
    _lock_acquire(&s_lvgl_lock);
    if (s_dimmed != dimmed) {
        s_dimmed = dimmed;
        render_current_locked();
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_recording(uint32_t session_id)
{
    ESP_LOGD(TAG, "recording session %" PRIu32, session_id);
    (void)session_id;

    _lock_acquire(&s_lvgl_lock);
    s_scene = UI_STATUS_ICON_RECORDING;
    s_text_kind = UI_TEXT_HINT;
    strlcpy(s_status_text, "Hört zu", sizeof(s_status_text));
    strlcpy(s_hint_text, "Sprich jetzt", sizeof(s_hint_text));
    render_current_locked();
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_battery(int level_percent, bool charging, bool usb_powered)
{
    if (level_percent < 0) {
        level_percent = 0;
    } else if (level_percent > 100) {
        level_percent = 100;
    }

    _lock_acquire(&s_lvgl_lock);
    if (s_ready) {
        const int fill_width = MAX(2, (level_percent * 14) / 100);
        lv_obj_set_width(s_battery_fill, fill_width);
        lv_obj_set_style_bg_color(s_battery_fill,
                                  level_percent <= 20 ? lv_color_hex(0xf97373) :
                                  charging || usb_powered ? lv_color_hex(0x5ec4ff) :
                                  lv_color_hex(0x67c59b),
                                  0);
        lv_label_set_text_fmt(s_battery_label, "%d%%", level_percent);
    }
    _lock_release(&s_lvgl_lock);
}

void ui_status_set_partial_text(const char *text)
{
    ESP_LOGD(TAG, "partial: %s", text ? text : "");
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "Denkt nach", text ? text : "", UI_TEXT_MESSAGE);
}

void ui_status_set_ota_progress(uint32_t written, uint32_t size)
{
    char hint[32];
    uint32_t percent = 0;
    if (size > 0) {
        percent = MIN(100, (written * 100) / size);
    }
    snprintf(hint, sizeof(hint), "%" PRIu32 "%%", percent);
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "Aktualisiert", hint, UI_TEXT_HINT);
}

void ui_status_set_ota_rebooting(void)
{
    set_scene(UI_STATUS_ICON_TRANSCRIBING, "Neustart", "Firmware aktualisiert", UI_TEXT_HINT);
}

void ui_status_set_error(const char *message)
{
    ESP_LOGE(TAG, "%s", message ? message : "unknown error");
    set_scene(UI_STATUS_ICON_ERROR, "", message ? message : "Unbekannter Fehler",
              UI_TEXT_MESSAGE);
}
