#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Handgelenk anheben statt Knopf druecken.
 *
 * Warum ueberhaupt: Der Schirm dunkelt nach 15 Sekunden ab (und geht nach
 * weiteren 15 aus). Wer danach die Uhrzeit sehen will, musste sonst eine
 * Taste druecken — mit der anderen Hand, was am Handgelenk genau die
 * Bewegung ist, die eine Uhr ueberfluessig machen soll.
 *
 * Zwei Geraete, zwei Wege (wrist_wake.c):
 *
 *   StickS3   BMI270, abgefragt alle 100 ms, solange der Schirm dunkel ist.
 *             STILLGELEGT (08/2026): main.c setzt die Komponente dort nicht
 *             auf (WRIST_WAKE_ENABLED 0); aus dem gedunkelten Schirm weckt
 *             nur ein Tastendruck. Die Interrupt-Leitung des BMI270 ist nicht
 *             gesichert verdrahtet, deshalb Abfrage und kein Weckgrund.
 *   Uhr       QMI8658 mit "Wake on Motion": Der Sensor weckt die Task ueber
 *             INT1 (GPIO21), erst dann wird gemessen und entschieden. Keine
 *             Abfrage im 100-ms-Takt mehr; die Leitung weckt den Chip auch
 *             aus dem Light Sleep. Aus dem Tiefschlaf holen weiter nur die
 *             Tasten (Begruendung in wrist_wake.c).
 */

/* Wird aus der Sensor-Task gerufen — kurz halten, keine Anzeige darin. */
typedef void (*wrist_wake_cb_t)(void);

/*
 * Sensor suchen und aufsetzen. Fehlt er oder antwortet er nicht, meldet die
 * Funktion das und alles andere laeuft weiter: Das Geraet ist ein
 * Sprachgeraet mit Uhr, keine Uhr mit Mikrofon.
 */
esp_err_t wrist_wake_init(wrist_wake_cb_t on_raise);

/* Steht der Sensor? Sonst gibt es die Geste nicht, und niemand soll sie versprechen. */
bool wrist_wake_available(void);

/*
 * Beobachten an oder aus. Eingeschaltet nur, solange der Schirm gedunkelt
 * oder aus ist — bei hellem Schirm gibt es nichts aufzuwecken, und der Sensor
 * kostet dann Strom fuer nichts. Auf der Uhr heisst "an": WoM scharf.
 */
void wrist_wake_watch(bool enabled);

/* Vor dem Tiefschlaf: Sensor aus, sonst laeuft er die Nacht durch. Weckquelle bleiben die Tasten. */
void wrist_wake_prepare_deep_sleep(void);
