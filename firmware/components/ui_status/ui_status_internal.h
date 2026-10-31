#pragma once

/*
 * Nur fuer Dateien dieser Komponente (ui_cards.c): dieselbe Sperre, derselbe
 * Schirm, dieselbe Textbereinigung wie ui_status.c. Kein oeffentlicher
 * Header -- wer ausserhalb zeichnen will, geht ueber ui_status.h.
 */

#include <stddef.h>

#include "lvgl.h"

void ui_status_lock(void);
void ui_status_unlock(void);
lv_obj_t *ui_status_screen(void);
/* Latin-1-taugliche Kopie: Emoji raus, Typografie zu ASCII (wie alle Anzeigetexte). */
void ui_status_copy_display_text(char *dst, const char *src, size_t size);
