#pragma once

#include "esp_err.h"

/*
 * Kurze Quittungstoene ueber den eingebauten Lautsprecher.
 *
 * Warum ueberhaupt: Am Handgelenk sieht man das Display beim Sprechen nicht.
 * Ohne Ton weiss man erst hinterher, ob die Aufnahme lief — und merkt einen
 * Fehlgriff erst, wenn nichts passiert.
 *
 * Die Toene laufen ueber denselben ES8311 wie das Mikrofon, nur in die andere
 * Richtung. Weil sich Aufnahme- und Wiedergabepfad die I2S-Leitungen teilen,
 * spielt `audio_tone_play()` **nichts**, solange die Aufnahme laeuft, und
 * kehrt dann sofort zurueck. Der Startton gehoert deshalb *vor*
 * `audio_pipeline_start()`.
 */

typedef enum {
    AUDIO_TONE_START,   /* ein Ton: Aufnahme laeuft */
    AUDIO_TONE_DONE,    /* zwei aufsteigende Toene: Antwort da */
    AUDIO_TONE_ERROR,   /* zwei absteigende Toene: hat nicht geklappt */
    AUDIO_TONE_CANCEL,  /* ein kurzer tiefer Ton: verworfen */
} audio_tone_t;

esp_err_t audio_tone_init(void);

/*
 * Spielt den Ton und kehrt erst danach zurueck (rund 100 bis 250 ms).
 * Blockierend mit Absicht: der Wiedergabepfad muss abgebaut sein, bevor die
 * Aufnahme die I2S-Leitungen uebernimmt. Fehler werden protokolliert, nicht
 * gemeldet — ein stummer Stick ist aergerlich, ein abgebrochener Turn nicht
 * hinnehmbar.
 */
void audio_tone_play(audio_tone_t tone);
