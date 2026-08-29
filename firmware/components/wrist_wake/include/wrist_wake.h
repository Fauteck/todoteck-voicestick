#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Handgelenk anheben statt Knopf druecken.
 *
 * STILLGELEGT (08/2026): Aus dem gedunkelten Schirm weckt nur noch ein
 * Tastendruck. Die Komponente bleibt vollstaendig erhalten, wird aber von
 * main.c nicht mehr aufgesetzt — siehe WRIST_WAKE_ENABLED dort. Alles
 * Folgende beschreibt, was passiert, wenn der Schalter wieder auf 1 steht.
 *
 * Warum ueberhaupt: Der Schirm dunkelt nach 15 Sekunden ab. Wer danach die
 * Uhrzeit sehen will, musste bisher eine Taste druecken — mit der
 * anderen Hand, was am Handgelenk genau die Bewegung ist, die eine Uhr
 * ueberfluessig machen soll.
 *
 * Was das Geraet dafuer hat: den BMI270, der bis hierher ungenutzt auf dem
 * I2C-Bus lag. Was es *nicht* hat: eine gesicherte Verbindung von dessen
 * Interrupt-Leitung zu einem Pin, der aus dem Tiefschlaf weckt (siehe
 * wrist_wake.c). Deshalb ist das hier eine Abfrage im wachen Zustand und
 * kein Weckgrund: Aus dem Tiefschlaf holt weiterhin nur die Taste.
 */

/* Wird aus dem Abfrage-Task gerufen — kurz halten, keine Anzeige darin. */
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
 * ist — bei hellem Schirm gibt es nichts aufzuwecken, und der Sensor kostet
 * dann Strom fuer nichts.
 */
void wrist_wake_watch(bool enabled);

/* Vor dem Tiefschlaf: Messung aus, sonst laeuft der Sensor die Nacht durch. */
void wrist_wake_prepare_deep_sleep(void);
