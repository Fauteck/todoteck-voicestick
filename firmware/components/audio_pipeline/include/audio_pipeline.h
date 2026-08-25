#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(uint32_t session_id);
esp_err_t audio_pipeline_stop(void);
uint32_t audio_pipeline_session_id(void);

/*
 * Aussteuerung des Mikrofons, 0 bis 100, mit kurzem Nachlauf.
 *
 * Nicht linear zum Ausschlag, sondern wurzelfoermig: normale Sprache liegt
 * roh bei knapp zehn Prozent des Vollausschlags und waere als Balken kaum zu
 * sehen — genau die Auskunft, um die es geht.
 */
uint8_t audio_pipeline_level(void);

/*
 * Belegt der Aufnahmepfad gerade die I2S-Leitungen? Auch nach dem Stoppen
 * noch wahr, solange die Abbautasks laufen — der Tonausgabe-Pfad teilt sich
 * die Pins und darf erst danach oeffnen.
 */
bool audio_pipeline_is_busy(void);
