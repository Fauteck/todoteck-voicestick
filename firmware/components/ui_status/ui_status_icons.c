#include "ui_status_icons.h"

#include <math.h>
#include <string.h>

/*
 * Tecki — das Todoteck-Maskottchen als Zustandsanzeige.
 *
 * Vorher lagen hier sechs Katzenbilder aus dem Upstream als ARGB8888-Rohdaten,
 * 112x112 zu je 50.176 Byte, also rund 294 KB Flash. Tecki braucht davon
 * nichts: Er IST die Marke aus dem Todoteck-Logo, und die besteht aus einer
 * Linie mit zwei Knicken und ein paar Kreisen. Gezeichnet wird er deshalb aus
 * LVGL-Grundformen. Das spart nicht nur den Flash — es macht die Figur auch
 * aenderbar, ohne dass jemand Bilder nachrendern und einchecken muss.
 *
 * ── Die Regel ───────────────────────────────────────────────────────────────
 * Die Geometrie ist im Todoteck-Repo festgeschrieben (docs/maskottchen.md).
 * Im 512er Raster: Knick bei (210,318), Enden bei (126,234) und (386,195),
 * Strichstaerke 76. Das Gesicht haengt daran statt an festen Pixelwerten:
 *
 *   Augenanker   46 % entlang V->B         Augenradius  0,25 * Strichstaerke
 *   Augenabstand +/- 0,29 * Strichstaerke  Pupille      0,12 * Strichstaerke
 *
 * Hier unten steht deshalb nur MARK_WIDTH; alles andere faellt daraus. Wer die
 * Figur groesser oder kleiner will, aendert genau diese eine Zahl.
 *
 * ── Warum 101,5 und nicht 112 ───────────────────────────────────────────────
 * Drei Szenen kippen die Figur um den Knick (Ruht +8, Hoert zu -7, Fehler +9).
 * Beim Drehen um einen Punkt, der nicht die Mitte ist, wandert der Rahmen mit.
 * 101,5 ist die groesste Breite, bei der alle sechs Posen in der 112er Flaeche
 * bleiben — nachgerechnet, nicht geschaetzt. Bei mehr ragt die Spitze in
 * "Fehler" rechts heraus.
 *
 * ── Warum zwei Gruentoene ───────────────────────────────────────────────────
 * ui_status.c faerbt den Grund je Szene: cremeweiss (0xfff7ed), im Ruhezustand
 * dunkel (0x1b2430). Das Marken-Gruen 0x2ecc71 erreicht auf dem hellen Grund
 * nur 1,98:1 — WCAG verlangt 3:1 fuer grafische Objekte, und am Handgelenk in
 * der Sonne merkt man den Unterschied sofort. Auf hellem Grund gilt deshalb
 * 0x1e9e56 (3,25:1), im Ruhezustand darf das hellere 0x2ecc71 stehen (7,45:1).
 * Beides sind die im Design-System festgelegten Logo-Toene, keine Erfindung.
 *
 * ── Was bewusst fehlt ───────────────────────────────────────────────────────
 * Kein Blinzeln (die 440 mAh sind der Grund), kein Mund ausser beim Antworten
 * — und einen Sprech-Zustand kennt diese Firmware noch gar nicht, der kommt
 * erst mit der Sprachausgabe. Keine Schallwellen beim Aufnehmen, weil daneben
 * schon Pegel und Laufzeit stehen; kein Warnzeichen beim Fehler, weil der
 * Koerper rot wird und die Statuszeile es sagt. Ein drittes Signal fuer
 * dieselbe Sache ist Laerm, kein Hinweis.
 */

/* ── Flaeche und Lage ─────────────────────────────────────────────────────── */
#define ICON_BOX   112
#define ICON_TOP_Y 42

/* ── Geometrie, alles abgeleitet ──────────────────────────────────────────── */
#define MARK_WIDTH 101.5f
#define MARK_K     (MARK_WIDTH / 336.0f)
#define STROKE_W   (76.0f * MARK_K)
#define EYE_R      (0.25f * STROKE_W)
#define PUPIL_R    (0.12f * STROKE_W)
#define LID_H      (0.07f * STROKE_W)
#define CROSS_W    (0.10f * STROKE_W)

/*
 * Punkte im 512er Raster, verschoben auf den Ursprung der Bounding-Box
 * (88,157) — dort faengt die Tinte an, die runden Enden eingerechnet.
 */
static const float MARK_PTS[3][2] = {
    { 38.0f, 77.0f },   /* A, Ende des kurzen Schenkels */
    { 122.0f, 161.0f }, /* V, Knick und Drehpunkt */
    { 298.0f, 38.0f },  /* B, Spitze */
};
static const float MARK_EYES[2][2] = {
    { 181.0f, 104.0f },
    { 225.0f, 104.0f },
};

/* ── Farben ───────────────────────────────────────────────────────────────── */
#define COL_MARK_LIGHT 0x1e9e56 /* auf cremeweissem Grund */
#define COL_MARK_DARK  0x2ecc71 /* auf dunklem Grund im Ruhezustand */
#define COL_MARK_ERROR 0xe74c3c /* 3,60:1 auf Creme, reicht */
#define COL_EYE_LIGHT  0xffffff
#define COL_EYE_DARK   0xe8eef7
#define COL_PUPIL      0x1b2a38

typedef enum {
    EYES_OPEN,
    EYES_CLOSED,
    EYES_CROSSED,
} eye_style_t;

typedef struct {
    float angle_deg;    /* Drehung um den Knick */
    uint32_t mark;      /* Koerperfarbe */
    bool dark_ground;   /* faerbt die Augen */
    eye_style_t eyes;
    float eye_scale;    /* 1,0 = Ruhemass */
    float gaze_x;       /* Blickrichtung, Vielfache der Strichstaerke */
    float gaze_y;
    bool dots;          /* die drei Punkte beim Nachdenken */
    lv_opa_t opa;
} scene_spec_t;

/*
 * "Koppeln" und "Bereit" unterscheiden sich nur im Blick: beim Koppeln schaut
 * Tecki mit grossen Augen zur Seite, als suche er etwas — was er ja auch tut.
 * Mehr Unterschied braucht es nicht, die Statuszeile sagt das Uebrige.
 */
static const scene_spec_t SPEC[] = {
    [UI_STATUS_ICON_BOOT] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.14f, 0.07f, 0.0f, false, LV_OPA_COVER },
    [UI_STATUS_ICON_PAIRING] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.14f, 0.07f, 0.0f, false, LV_OPA_COVER },
    [UI_STATUS_ICON_IDLE] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.0f, 0.0f, 0.0f, false, LV_OPA_COVER },
    [UI_STATUS_ICON_RESTING] =
        { 8.0f, COL_MARK_DARK, true, EYES_CLOSED, 1.0f, 0.0f, 0.0f, false, LV_OPA_50 },
    [UI_STATUS_ICON_RECORDING] =
        { -7.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.14f, -0.07f, -0.02f, false, LV_OPA_COVER },
    [UI_STATUS_ICON_TRANSCRIBING] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.0f, -0.05f, -0.08f, true, LV_OPA_COVER },
    [UI_STATUS_ICON_ERROR] =
        { 9.0f, COL_MARK_ERROR, false, EYES_CROSSED, 1.0f, 0.0f, 0.0f, false, LV_OPA_COVER },
};

#define SCENE_COUNT ((int)(sizeof(SPEC) / sizeof(SPEC[0])))

/*
 * Einmal beim Anlegen ausgerechnet. lv_line behaelt den Zeiger auf die Punkte,
 * also muessen sie leben, solange das Objekt lebt — deshalb statisch und je
 * Szene getrennt, statt ein Feld bei jedem Wechsel zu ueberschreiben.
 */
static lv_point_precise_t s_body[SCENE_COUNT][3];
static lv_point_precise_t s_cross[4][2];
static float s_eye_xy[SCENE_COUNT][2][2];

/* Eigene Konstante statt M_PI: das haengt an Feature-Test-Makros und ist
 * nicht ueberall ohne Weiteres da. */
#define TECKI_PI 3.14159265358979323846f

static void rotate_about(float px, float py, float ox, float oy, float rad, float *rx, float *ry)
{
    const float c = cosf(rad);
    const float s = sinf(rad);
    const float dx = px - ox;
    const float dy = py - oy;
    *rx = ox + dx * c - dy * s;
    *ry = oy + dx * s + dy * c;
}

static void compute_geometry(void)
{
    const float x0 = (ICON_BOX - MARK_WIDTH) / 2.0f;
    const float y0 = (ICON_BOX - 199.0f * MARK_K) / 2.0f;
    const float vx = MARK_PTS[1][0] * MARK_K + x0;
    const float vy = MARK_PTS[1][1] * MARK_K + y0;

    for (int scene = 0; scene < SCENE_COUNT; scene++) {
        const float rad = SPEC[scene].angle_deg * TECKI_PI / 180.0f;
        for (int i = 0; i < 3; i++) {
            float rx, ry;
            rotate_about(MARK_PTS[i][0] * MARK_K + x0, MARK_PTS[i][1] * MARK_K + y0,
                         vx, vy, rad, &rx, &ry);
            s_body[scene][i].x = lroundf(rx);
            s_body[scene][i].y = lroundf(ry);
        }
        for (int i = 0; i < 2; i++) {
            float rx, ry;
            rotate_about(MARK_EYES[i][0] * MARK_K + x0, MARK_EYES[i][1] * MARK_K + y0,
                         vx, vy, rad, &rx, &ry);
            s_eye_xy[scene][i][0] = rx;
            s_eye_xy[scene][i][1] = ry;
        }
    }

    /* Die Kreuze im Fehlerfall sitzen auf den dortigen Augenmitten. */
    const float arm = 0.85f * EYE_R;
    for (int i = 0; i < 2; i++) {
        const float cx = s_eye_xy[UI_STATUS_ICON_ERROR][i][0];
        const float cy = s_eye_xy[UI_STATUS_ICON_ERROR][i][1];
        s_cross[i * 2][0].x = lroundf(cx - arm);
        s_cross[i * 2][0].y = lroundf(cy - arm);
        s_cross[i * 2][1].x = lroundf(cx + arm);
        s_cross[i * 2][1].y = lroundf(cy + arm);
        s_cross[i * 2 + 1][0].x = lroundf(cx + arm);
        s_cross[i * 2 + 1][0].y = lroundf(cy - arm);
        s_cross[i * 2 + 1][1].x = lroundf(cx - arm);
        s_cross[i * 2 + 1][1].y = lroundf(cy + arm);
    }
}

static lv_obj_t *make_blob(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}

static lv_obj_t *make_stroke(lv_obj_t *parent)
{
    lv_obj_t *o = lv_line_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, ICON_BOX, ICON_BOX);
    lv_obj_set_style_line_rounded(o, true, 0);
    return o;
}

static void place_circle(lv_obj_t *o, float cx, float cy, float r)
{
    const int32_t d = (int32_t)lroundf(r * 2.0f);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, (int32_t)lroundf(cx - r), (int32_t)lroundf(cy - r));
}

static void show(lv_obj_t *o, bool visible)
{
    if (visible) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen)
{
    memset(icons, 0, sizeof(*icons));
    compute_geometry();

    icons->root = lv_obj_create(screen);
    lv_obj_remove_style_all(icons->root);
    lv_obj_remove_flag(icons->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(icons->root, ICON_BOX, ICON_BOX);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, ICON_TOP_Y);

    icons->body = make_stroke(icons->root);
    lv_obj_set_style_line_width(icons->body, (int32_t)lroundf(STROKE_W), 0);

    for (int i = 0; i < 2; i++) {
        icons->eye[i] = make_blob(icons->root);
        icons->lid[i] = make_blob(icons->root);
    }
    /* Pupillen nach den Augen, damit sie darauf liegen. */
    for (int i = 0; i < 2; i++) {
        icons->pupil[i] = make_blob(icons->root);
        lv_obj_set_style_bg_color(icons->pupil[i], lv_color_hex(COL_PUPIL), 0);
    }
    for (int i = 0; i < 4; i++) {
        icons->cross[i] = make_stroke(icons->root);
        lv_obj_set_style_line_width(icons->cross[i], (int32_t)lroundf(CROSS_W), 0);
        lv_line_set_points(icons->cross[i], s_cross[i], 2);
    }
    for (int i = 0; i < UI_TECKI_THINK_DOTS; i++) {
        icons->dot[i] = make_blob(icons->root);
    }

    ui_status_icons_apply(icons, UI_STATUS_ICON_BOOT);
}

void ui_status_icons_stop_anim(ui_status_icons_t *icons)
{
    /*
     * Es gibt nichts zu stoppen — Tecki bewegt sich nicht. Die Funktion
     * bleibt, weil ui_status.c sie bei jedem Szenenwechsel ruft und ein
     * kuenftiger Effekt hier wieder abzuraeumen waere.
     */
    if (icons->root != NULL) {
        lv_anim_delete(icons->root, NULL);
    }
}

void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    if (icons->root == NULL) {
        return;
    }
    if ((int)scene < 0 || (int)scene >= SCENE_COUNT) {
        scene = UI_STATUS_ICON_IDLE;
    }
    ui_status_icons_stop_anim(icons);

    const scene_spec_t *s = &SPEC[scene];
    const uint32_t eye_colour = s->dark_ground ? COL_EYE_DARK : COL_EYE_LIGHT;
    const float eye_r = EYE_R * s->eye_scale;

    lv_line_set_points(icons->body, s_body[scene], 3);
    lv_obj_set_style_line_color(icons->body, lv_color_hex(s->mark), 0);

    for (int i = 0; i < 2; i++) {
        const float cx = s_eye_xy[scene][i][0];
        const float cy = s_eye_xy[scene][i][1];

        lv_obj_set_style_bg_color(icons->eye[i], lv_color_hex(eye_colour), 0);
        place_circle(icons->eye[i], cx, cy, eye_r);
        place_circle(icons->pupil[i], cx + s->gaze_x * STROKE_W, cy + s->gaze_y * STROKE_W,
                     PUPIL_R);

        /* Geschlossenes Auge: ein liegender Balken statt der Scheibe. */
        lv_obj_set_style_bg_color(icons->lid[i], lv_color_hex(eye_colour), 0);
        const int32_t lid_w = (int32_t)lroundf(eye_r * 2.0f);
        int32_t lid_h = (int32_t)lroundf(LID_H * 2.0f);
        if (lid_h < 2) {
            lid_h = 2; /* darunter verschwindet der Strich beim Zeichnen */
        }
        lv_obj_set_size(icons->lid[i], lid_w, lid_h);
        lv_obj_set_pos(icons->lid[i], (int32_t)lroundf(cx - eye_r),
                       (int32_t)lroundf(cy) - lid_h / 2);

        show(icons->eye[i], s->eyes == EYES_OPEN);
        show(icons->pupil[i], s->eyes == EYES_OPEN);
        show(icons->lid[i], s->eyes == EYES_CLOSED);
    }

    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_line_color(icons->cross[i], lv_color_hex(eye_colour), 0);
        show(icons->cross[i], s->eyes == EYES_CROSSED);
    }

    /*
     * Die drei Punkte steigen ueber der Spitze auf. Sie drehen nicht mit —
     * sie gehoeren zum Warten, nicht zum Koerper.
     */
    static const float DOT_POS[UI_TECKI_THINK_DOTS][3] = {
        { 0.44f, 0.16f, 0.090f },
        { 0.56f, 0.10f, 0.105f },
        { 0.68f, 0.04f, 0.120f },
    };
    for (int i = 0; i < UI_TECKI_THINK_DOTS; i++) {
        lv_obj_set_style_bg_color(icons->dot[i], lv_color_hex(s->mark), 0);
        place_circle(icons->dot[i], DOT_POS[i][0] * ICON_BOX, DOT_POS[i][1] * ICON_BOX,
                     DOT_POS[i][2] * STROKE_W);
        show(icons->dot[i], s->dots);
    }

    lv_obj_set_style_opa(icons->root, s->opa, 0);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, ICON_TOP_Y);
}

void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    (void)icons;
    (void)scene;
}
