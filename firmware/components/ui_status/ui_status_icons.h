#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "sdkconfig.h"

typedef enum {
    UI_STATUS_ICON_BOOT,
    UI_STATUS_ICON_PAIRING,
    UI_STATUS_ICON_IDLE,
    UI_STATUS_ICON_RESTING,
    UI_STATUS_ICON_RECORDING,
    UI_STATUS_ICON_TRANSCRIBING,
    UI_STATUS_ICON_ERROR,
    /*
     * Stimmungen ueber einer Szene (ui_status_set_mood). Seit den leeren
     * Zustaenden auch als Pose fuer sich: die leeren Karten und der leere
     * Verlauf (Tecki schaut zur Seite).
     */
    UI_STATUS_ICON_HAPPY,
    UI_STATUS_ICON_LOOK_LEFT,
    UI_STATUS_ICON_LOOK_RIGHT,
    /*
     * Statusposen (nur die Uhr, ui_status.c: icon_scene_for): "Update
     * laeuft" (Augen zu, die Punkte des Nachdenkens) und "Akku niedrig"
     * (kleine Augen, Blick nach unten, leicht gekippt). "Keine Verbindung"
     * braucht keine eigene -- das ist der suchende Blick von "Koppeln".
     */
    UI_STATUS_ICON_UPDATING,
    UI_STATUS_ICON_TIRED,
    UI_STATUS_ICON_COUNT,
} ui_status_icon_scene_t;

#define UI_TECKI_THINK_DOTS 3

/*
 * Die zwei Flaechen, in denen Tecki auftritt. Die grosse ist seine Spalte
 * neben dem Text, die kleine seine Ecke auf dem Ziffernblatt — dort gehoert
 * der Schirm der Uhrzeit, und ein Maskottchen in Originalgroesse waere
 * daneben ein Mitbewerber statt einer Randnotiz.
 */
#if CONFIG_VOICE_BOARD_WATCH206
/*
 * Die Uhr: 150 px mittig ueber dem Text, 64 px auf dem Ziffernblatt. Die
 * Geometrie faellt wie beim StickS3 vollstaendig aus der Flaeche.
 */
#define UI_TECKI_BOX 150
#define UI_TECKI_BOX_SMALL 64
#else
#define UI_TECKI_BOX 64
#define UI_TECKI_BOX_SMALL 28
#endif

/*
 * Tecki besteht aus Grundformen, nicht aus einem Bild: eine Linie mit zwei
 * Knicken als Koerper, dazu Kreise und Striche fuers Gesicht. Alle Teile
 * werden einmal angelegt und je Szene nur umgefaerbt, verschoben oder
 * versteckt — die Begruendung steht in ui_status_icons.c.
 */
/*
 * Jede Figur traegt ihre eigene Geometrie. Bis 10/2026 lag sie einmal fuer
 * alle in ui_status_icons.c -- es gab ja nur eine Figur. Seit die leeren
 * Karten einen zweiten, kleinen Tecki zeigen, stehen zwei Figuren mit
 * verschiedener Flaeche zugleich auf dem Schirm, und `lv_line` behaelt einen
 * Zeiger auf seine Punkte: Mit geteilten Punkten zoege die eine die andere
 * mit. Gespeichert werden nur die Punkte der gerade gezeigten Pose (gut
 * 90 Byte je Figur); die Drehung rechnet ui_status_icons_apply() bei jedem
 * Wechsel neu, das sind fuenf Punkte.
 */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *eye[2];
    lv_obj_t *pupil[2];
    lv_obj_t *lid[2];
    lv_obj_t *cross[4];
    lv_obj_t *dot[UI_TECKI_THINK_DOTS];
    int32_t box;
    int32_t x;
    int32_t y;
    lv_point_precise_t body_pts[3];
    lv_point_precise_t cross_pts[4][2];
} ui_status_icons_t;

/* Legt die Figur in `parent` an, in der grossen Flaeche (UI_TECKI_BOX). */
void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen);
void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene);
/* Flaeche und obere linke Ecke der Figur — vor ui_status_icons_apply() setzen. */
void ui_status_icons_set_box(ui_status_icons_t *icons, int32_t box, int32_t x, int32_t y);
/* Platz machen: bei langem Text weicht die Figur dem Text. */
void ui_status_icons_show(ui_status_icons_t *icons, bool visible);
/*
 * Nach ui_status_icons_apply(): Koerper und Punkte in `mark`, Augen, Lider
 * und Kreuze in `eye` -- fuer das rote Zifferblatt im Nachtmodus (0.4.4).
 * Das naechste apply() setzt die Farben der Pose wieder.
 */
void ui_status_icons_tint(ui_status_icons_t *icons, uint32_t mark, uint32_t eye);
void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene);
void ui_status_icons_stop_anim(ui_status_icons_t *icons);
