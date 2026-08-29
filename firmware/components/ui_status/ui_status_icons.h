#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef enum {
    UI_STATUS_ICON_BOOT,
    UI_STATUS_ICON_PAIRING,
    UI_STATUS_ICON_IDLE,
    UI_STATUS_ICON_RESTING,
    UI_STATUS_ICON_RECORDING,
    UI_STATUS_ICON_TRANSCRIBING,
    UI_STATUS_ICON_ERROR,
} ui_status_icon_scene_t;

#define UI_TECKI_THINK_DOTS 3

/*
 * Die zwei Flaechen, in denen Tecki auftritt. Die grosse ist seine Spalte
 * neben dem Text, die kleine seine Ecke auf dem Ziffernblatt — dort gehoert
 * der Schirm der Uhrzeit, und ein Maskottchen in Originalgroesse waere
 * daneben ein Mitbewerber statt einer Randnotiz.
 */
#define UI_TECKI_BOX 64
#define UI_TECKI_BOX_SMALL 28

/*
 * Tecki besteht aus Grundformen, nicht aus einem Bild: eine Linie mit zwei
 * Knicken als Koerper, dazu Kreise und Striche fuers Gesicht. Alle Teile
 * werden einmal angelegt und je Szene nur umgefaerbt, verschoben oder
 * versteckt — die Begruendung steht in ui_status_icons.c.
 */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *eye[2];
    lv_obj_t *pupil[2];
    lv_obj_t *lid[2];
    lv_obj_t *cross[4];
    lv_obj_t *dot[UI_TECKI_THINK_DOTS];
} ui_status_icons_t;

void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen);
void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene);
/* Flaeche und obere linke Ecke der Figur — vor ui_status_icons_apply() setzen. */
void ui_status_icons_set_box(ui_status_icons_t *icons, int32_t box, int32_t x, int32_t y);
/* Platz machen: bei langem Text weicht die Figur dem Text. */
void ui_status_icons_show(ui_status_icons_t *icons, bool visible);
void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene);
void ui_status_icons_stop_anim(ui_status_icons_t *icons);
