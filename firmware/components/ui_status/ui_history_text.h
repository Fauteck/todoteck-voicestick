#pragma once

/*
 * Texte des Verlaufs ohne LVGL: Zeitstempel ("vor 12 min", "gestern 18:40"),
 * Ergebnis-Marker (Wort und Farbe) und die Tagesbilanz. Reines C, damit
 * die Logik auf dem Rechner mit gcc gegen Beispiele laeuft -- ein Build
 * fuer das Geraet dauert Minuten, ein Test hier Sekunden.
 */

#include <stddef.h>
#include <stdint.h>

#include "ui_status.h"

/* "created", "done", "answer", "error" -> Marker; alles andere NONE. */
ui_outcome_t ui_outcome_from_name(const char *name);
/* "angelegt", "erledigt", "Antwort", "Fehler"; leer bei NONE. */
const char *ui_outcome_word(ui_outcome_t outcome);
/* 0xRRGGBB des Punkts; angelegt und erledigt gruen, Antwort grau, Fehler rot. */
uint32_t ui_outcome_colour(ui_outcome_t outcome);

/*
 * Zeitstempel relativ zur Ortszeit: unter einer Minute "gerade eben", unter
 * einer Stunde "vor 12 min", am selben Tag "11:46", am Vortag "gestern
 * 18:40", in der Woche "Mo 18:40", davor "29.10. 18:40". Leer, wenn `at`
 * 0 ist (die Uhr stand nicht, als der Eintrag entstand).
 */
void ui_history_format_when(char *out, size_t size, int64_t at_utc, int64_t now_utc,
                            int32_t tz_offset_min);

/* Ortstag als fortlaufende Zahl (Tage seit 1970 in Ortszeit). */
int64_t ui_history_local_day(int64_t epoch_utc, int32_t tz_offset_min);

/* "4 Befehle · 3 erledigt · 1 Frage · 0 Fehler" -- mit Einzahl, wo sie hingehoert. */
void ui_history_format_balance(char *out, size_t size, unsigned commands, unsigned done,
                               unsigned answers, unsigned errors);
