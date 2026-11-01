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
 * Hier unten steht deshalb nur die Flaeche; alles andere faellt daraus. Wer
 * die Figur groesser oder kleiner will, aendert genau diese eine Zahl.
 *
 * ── Warum die Figur schmaler ist als ihre Flaeche ───────────────────────────
 * Drei Szenen kippen die Figur um den Knick (Ruht +8, Hoert zu -7, Fehler +9).
 * Beim Drehen um einen Punkt, der nicht die Mitte ist, wandert der Rahmen mit.
 * 90,625 Prozent der Flaeche ist die groesste Breite, bei der alle sechs Posen
 * darin bleiben — nachgerechnet, nicht geschaetzt (hochkant: 101,5 von 112).
 * Bei mehr ragt die Spitze in "Fehler" rechts heraus.
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
 * Kein Blinzeln (der kleine Akku ist der Grund), kein Mund ausser beim Antworten
 * — und einen Sprech-Zustand kennt diese Firmware noch gar nicht, der kommt
 * erst mit der Sprachausgabe. Keine Schallwellen beim Aufnehmen, weil daneben
 * schon Pegel und Laufzeit stehen; kein Warnzeichen beim Fehler, weil der
 * Koerper rot wird und die Statuszeile es sagt. Ein drittes Signal fuer
 * dieselbe Sache ist Laerm, kein Hinweis.
 */

/* ── Flaeche und Lage ─────────────────────────────────────────────────────── */
/*
 * Im Querformat steht Tecki links, der Text rechts daneben — UI_TECKI_BOX
 * und TECKI_BOX in ui_status.c sind dieselbe Spalte und muessen
 * zusammenpassen. Hochkant war die Flaeche 112 breit; auf 119 Pixeln Hoehe
 * waere das die ganze Anzeige gewesen, und der Text haette nichts mehr uebrig
 * gehabt.
 *
 * Seit dem Ziffernblatt gibt es zwei Flaechen statt einer: Auf dem Zifferblatt
 * (Bereit und Ruht) sitzt Tecki klein in der Ecke, damit die Uhrzeit den
 * Schirm bekommt. Ueberall sonst bleibt er in seiner Spalte. Die Flaeche ist
 * deshalb eine Laufzeitgroesse, kein Makro mehr — die Geometrie faellt
 * weiterhin vollstaendig aus ihr heraus. Seit 10/2026 traegt sie jede Figur
 * selbst (ui_status_icons_t: box, x, y), weil auf der Uhr zwei zugleich
 * stehen koennen.
 */

/* ── Geometrie, alles abgeleitet ──────────────────────────────────────────── */
/*
 * 90,625 Prozent der Flaeche — frueher stand hier 101,5 zu 112, dieselbe
 * Zahl. Als Verhaeltnis statt als Absolutwert, damit die Begruendung darunter
 * auch bei einer anderen Flaeche noch stimmt: Wer die Figur groesser oder
 * kleiner will, aendert die Flaeche, und der Rand waechst mit.
 */
#define MARK_WIDTH(b) (0.90625f * (float)(b))
#define MARK_K(b)     (MARK_WIDTH(b) / 336.0f)
#define STROKE_W(b)   (76.0f * MARK_K(b))
#define EYE_R(b)      (0.25f * STROKE_W(b))
#define PUPIL_R(b)    (0.12f * STROKE_W(b))
#define LID_H(b)      (0.07f * STROKE_W(b))
#define CROSS_W(b)    (0.10f * STROKE_W(b))

/*
 * Die duennste Linie, die der Zeichner noch sichtbar ausgibt. Bei der kleinen
 * Flaeche faellt die Strichstaerke der Kreuze sonst unter einen Pixel und die
 * Fehler-Augen verschwaenden ganz.
 */
static int32_t line_width(float w)
{
    const int32_t px = (int32_t)lroundf(w);
    return px < 1 ? 1 : px;
}

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
#if CONFIG_VOICE_BOARD_WATCH206
/*
 * Die Uhr zeigt jede Szene auf schwarzem Grund (AMOLED, siehe
 * ui_layout_watch206.h). Das dunklere Gruen ist nur fuer Creme da; auf
 * Schwarz gilt ueberall das Marken-Gruen.
 */
#define COL_MARK_LIGHT 0x2ecc71
#else
#define COL_MARK_LIGHT 0x1e9e56 /* auf cremeweissem Grund */
#endif
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
    /*
     * Freut sich: leicht zurueckgelehnt, grosse Augen, Blick nach oben --
     * nach einer Antwort oder einem abgehakten Eintrag, fuer drei Sekunden.
     * Schaut zur Seite: beim Wischen in die Richtung, in die es geht.
     */
    [UI_STATUS_ICON_HAPPY] =
        { -6.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.25f, 0.0f, -0.09f, false, LV_OPA_COVER },
    [UI_STATUS_ICON_LOOK_LEFT] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.0f, -0.11f, 0.0f, false, LV_OPA_COVER },
    [UI_STATUS_ICON_LOOK_RIGHT] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_OPEN, 1.0f, 0.11f, 0.0f, false, LV_OPA_COVER },
    /*
     * Die Statusposen. "Update laeuft": Augen zu und die drei Punkte -- er
     * ist beschaeftigt, nicht muede. "Akku niedrig": gekippt wie beim Ruhen,
     * kleine Augen mit Blick nach unten, etwas blasser. Der Winkel bleibt
     * bei 8 Grad, innerhalb dessen, wofuer der Rand unten gerechnet ist.
     */
    [UI_STATUS_ICON_UPDATING] =
        { 0.0f, COL_MARK_LIGHT, false, EYES_CLOSED, 1.0f, 0.0f, 0.0f, true, LV_OPA_COVER },
    [UI_STATUS_ICON_TIRED] =
        { 8.0f, COL_MARK_LIGHT, false, EYES_OPEN, 0.75f, 0.0f, 0.09f, false, LV_OPA_70 },
};

#define SCENE_COUNT ((int)(sizeof(SPEC) / sizeof(SPEC[0])))
_Static_assert(sizeof(SPEC) / sizeof(SPEC[0]) == UI_STATUS_ICON_COUNT,
               "jede Pose braucht eine Zeile in SPEC");

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

/* Ursprung der Figur in ihrer Flaeche und der Knick als Drehpunkt. */
typedef struct {
    float k;
    float x0;
    float y0;
    float vx;
    float vy;
} mark_frame_t;

static mark_frame_t mark_frame(int32_t box)
{
    mark_frame_t f;
    f.k = MARK_K(box);
    f.x0 = ((float)box - MARK_WIDTH(box)) / 2.0f;
    f.y0 = ((float)box - 199.0f * f.k) / 2.0f;
    f.vx = MARK_PTS[1][0] * f.k + f.x0;
    f.vy = MARK_PTS[1][1] * f.k + f.y0;
    return f;
}

/* Augenmitten einer Pose, gedreht wie der Koerper. */
static void eye_centres(const mark_frame_t *f, ui_status_icon_scene_t scene, float out[2][2])
{
    const float rad = SPEC[scene].angle_deg * TECKI_PI / 180.0f;
    for (int i = 0; i < 2; i++) {
        rotate_about(MARK_EYES[i][0] * f->k + f->x0, MARK_EYES[i][1] * f->k + f->y0,
                     f->vx, f->vy, rad, &out[i][0], &out[i][1]);
    }
}

/* Die Kreuze im Fehlerfall sitzen auf den dortigen Augenmitten. */
static void compute_cross(ui_status_icons_t *icons)
{
    const mark_frame_t f = mark_frame(icons->box);
    float eyes[2][2];
    eye_centres(&f, UI_STATUS_ICON_ERROR, eyes);
    const float arm = 0.85f * EYE_R(icons->box);
    for (int i = 0; i < 2; i++) {
        const float cx = eyes[i][0];
        const float cy = eyes[i][1];
        icons->cross_pts[i * 2][0].x = lroundf(cx - arm);
        icons->cross_pts[i * 2][0].y = lroundf(cy - arm);
        icons->cross_pts[i * 2][1].x = lroundf(cx + arm);
        icons->cross_pts[i * 2][1].y = lroundf(cy + arm);
        icons->cross_pts[i * 2 + 1][0].x = lroundf(cx + arm);
        icons->cross_pts[i * 2 + 1][0].y = lroundf(cy - arm);
        icons->cross_pts[i * 2 + 1][1].x = lroundf(cx - arm);
        icons->cross_pts[i * 2 + 1][1].y = lroundf(cy + arm);
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

static lv_obj_t *make_stroke(lv_obj_t *parent, int32_t box)
{
    lv_obj_t *o = lv_line_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, box, box);
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
    icons->box = UI_TECKI_BOX;
    icons->y = 35;
    compute_cross(icons);

    icons->root = lv_obj_create(screen);
    lv_obj_remove_style_all(icons->root);
    lv_obj_remove_flag(icons->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(icons->root, icons->box, icons->box);
    lv_obj_align(icons->root, LV_ALIGN_TOP_LEFT, icons->x, icons->y);

    icons->body = make_stroke(icons->root, icons->box);
    lv_obj_set_style_line_width(icons->body, line_width(STROKE_W(icons->box)), 0);

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
        icons->cross[i] = make_stroke(icons->root, icons->box);
        lv_obj_set_style_line_width(icons->cross[i], line_width(CROSS_W(icons->box)), 0);
        lv_line_set_points(icons->cross[i], icons->cross_pts[i], 2);
    }
    for (int i = 0; i < UI_TECKI_THINK_DOTS; i++) {
        icons->dot[i] = make_blob(icons->root);
    }

    ui_status_icons_apply(icons, UI_STATUS_ICON_BOOT);
}

/*
 * Tecki tritt beiseite, wenn eine Antwort mehr Platz braucht, als neben ihm
 * frei ist — die Entscheidung faellt in ui_status.c am gemessenen Text.
 * Versteckt wird die ganze Gruppe: Koerper, Augen und Punkte haengen an
 * `root`, ein Flag genuegt also fuer alle.
 */
void ui_status_icons_show(ui_status_icons_t *icons, bool visible)
{
    if (icons->root == NULL) {
        return;
    }
    show(icons->root, visible);
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

    const int32_t box = icons->box;
    const scene_spec_t *s = &SPEC[scene];
    const uint32_t eye_colour = s->dark_ground ? COL_EYE_DARK : COL_EYE_LIGHT;
    const float eye_r = EYE_R(box) * s->eye_scale;
    const float stroke = STROKE_W(box);

    /*
     * Die Punkte dieser einen Pose, gedreht um den Knick. lv_line behaelt den
     * Zeiger auf body_pts; set_points meldet die Flaeche danach neu an.
     */
    const mark_frame_t f = mark_frame(box);
    const float rad = s->angle_deg * TECKI_PI / 180.0f;
    for (int i = 0; i < 3; i++) {
        float rx, ry;
        rotate_about(MARK_PTS[i][0] * f.k + f.x0, MARK_PTS[i][1] * f.k + f.y0,
                     f.vx, f.vy, rad, &rx, &ry);
        icons->body_pts[i].x = lroundf(rx);
        icons->body_pts[i].y = lroundf(ry);
    }
    float eyes[2][2];
    eye_centres(&f, scene, eyes);

    lv_line_set_points(icons->body, icons->body_pts, 3);
    lv_obj_set_style_line_color(icons->body, lv_color_hex(s->mark), 0);

    for (int i = 0; i < 2; i++) {
        const float cx = eyes[i][0];
        const float cy = eyes[i][1];

        lv_obj_set_style_bg_color(icons->eye[i], lv_color_hex(eye_colour), 0);
        place_circle(icons->eye[i], cx, cy, eye_r);
        place_circle(icons->pupil[i], cx + s->gaze_x * stroke, cy + s->gaze_y * stroke,
                     PUPIL_R(box));

        /* Geschlossenes Auge: ein liegender Balken statt der Scheibe. */
        lv_obj_set_style_bg_color(icons->lid[i], lv_color_hex(eye_colour), 0);
        const int32_t lid_w = (int32_t)lroundf(eye_r * 2.0f);
        int32_t lid_h = (int32_t)lroundf(LID_H(box) * 2.0f);
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
        place_circle(icons->dot[i], DOT_POS[i][0] * (float)box,
                     DOT_POS[i][1] * (float)box, DOT_POS[i][2] * stroke);
        show(icons->dot[i], s->dots);
    }

    lv_obj_set_style_opa(icons->root, s->opa, 0);
    lv_obj_set_size(icons->root, box, box);
    lv_obj_align(icons->root, LV_ALIGN_TOP_LEFT, icons->x, icons->y);
}

/*
 * Flaeche und Ecke fuer die naechste Pose dieser Figur.
 *
 * Neu gerechnet und neu bemessen wird nur, wenn sich die Flaeche wirklich
 * geaendert hat -- sonst meldete jedes Nachziehen der Minute Flaechen als
 * ungueltig, die genauso aussehen wie vorher. Den Koerper setzt der
 * anschliessende ui_status_icons_apply() ohnehin neu.
 */
void ui_status_icons_set_box(ui_status_icons_t *icons, int32_t box, int32_t x, int32_t y)
{
    icons->x = x;
    icons->y = y;
    if (box == icons->box) {
        return;
    }
    icons->box = box;
    compute_cross(icons);
    if (icons->root == NULL) {
        return;
    }
    lv_obj_set_size(icons->root, box, box);
    lv_obj_set_size(icons->body, box, box);
    lv_obj_set_style_line_width(icons->body, line_width(STROKE_W(box)), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_size(icons->cross[i], box, box);
        lv_obj_set_style_line_width(icons->cross[i], line_width(CROSS_W(box)), 0);
        lv_line_set_points(icons->cross[i], icons->cross_pts[i], 2);
    }
}

void ui_status_icons_tint(ui_status_icons_t *icons, uint32_t mark, uint32_t eye)
{
    if (icons->root == NULL) {
        return;
    }
    lv_obj_set_style_line_color(icons->body, lv_color_hex(mark), 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(icons->eye[i], lv_color_hex(eye), 0);
        lv_obj_set_style_bg_color(icons->lid[i], lv_color_hex(eye), 0);
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_line_color(icons->cross[i], lv_color_hex(eye), 0);
    }
    for (int i = 0; i < UI_TECKI_THINK_DOTS; i++) {
        lv_obj_set_style_bg_color(icons->dot[i], lv_color_hex(mark), 0);
    }
}

void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    (void)icons;
    (void)scene;
}
