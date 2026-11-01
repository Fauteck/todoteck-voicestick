#pragma once

/*
 * Akku-Restzeit aus Stichproben (Paket G, Punkt 16) -- reines C ohne
 * ESP-IDF, damit die Rechnung auf dem Rechner mit gcc gegen Beispiele laeuft.
 *
 * Alle zehn Minuten eine Stichprobe (Zeit in Sekunden, Prozent), nur ohne
 * Ladekabel, in einem Ring von zwoelf Werten (also zwei Stunden). Die
 * Entladerate ist die Differenz zwischen aeltester und juengster Probe; ab
 * drei Proben und einer halben Stunde Spanne gilt sie als belastbar, und
 * die Restzeit ist Prozent geteilt durch Rate. Unsicher heisst -1, und dann
 * steht nichts auf dem Schirm -- eine geratene Stundenzahl waere schlechter
 * als keine.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BATTERY_SAMPLES 12
#define BATTERY_SAMPLE_INTERVAL_S (10 * 60)
#define BATTERY_MIN_SAMPLES 3
#define BATTERY_MIN_SPAN_S (30 * 60)

typedef struct {
    int64_t t_s;
    uint8_t pct;
} battery_sample_t;

typedef struct {
    battery_sample_t s[BATTERY_SAMPLES];
    uint8_t count;
    uint8_t next;
} battery_ring_t;

static inline void battery_ring_clear(battery_ring_t *ring)
{
    ring->count = 0;
    ring->next = 0;
}

/* Die juengste Probe, oder NULL. */
static inline const battery_sample_t *battery_ring_newest(const battery_ring_t *ring)
{
    if (ring->count == 0) {
        return NULL;
    }
    return &ring->s[(ring->next + BATTERY_SAMPLES - 1) % BATTERY_SAMPLES];
}

static inline void battery_ring_add(battery_ring_t *ring, int64_t t_s, int pct)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    ring->s[ring->next].t_s = t_s;
    ring->s[ring->next].pct = (uint8_t)pct;
    ring->next = (uint8_t)((ring->next + 1) % BATTERY_SAMPLES);
    if (ring->count < BATTERY_SAMPLES) {
        ring->count++;
    }
}

/* Faellig, wenn seit der juengsten Probe BATTERY_SAMPLE_INTERVAL_S vergangen sind (oder keine da ist). */
static inline bool battery_ring_due(const battery_ring_t *ring, int64_t now_s)
{
    const battery_sample_t *newest = battery_ring_newest(ring);
    return !newest || now_s - newest->t_s >= BATTERY_SAMPLE_INTERVAL_S ||
           now_s < newest->t_s; /* Uhr zurueckgestellt: lieber neu messen */
}

/* Restzeit in Minuten bei `pct_now` Prozent; -1, wenn die Rate noch nicht belastbar ist. */
static inline int32_t battery_ring_remaining_min(const battery_ring_t *ring, int pct_now)
{
    if (ring->count < BATTERY_MIN_SAMPLES || pct_now <= 0) {
        return -1;
    }
    const battery_sample_t *newest = battery_ring_newest(ring);
    const battery_sample_t *oldest = &ring->s[(ring->next + BATTERY_SAMPLES - ring->count) %
                                             BATTERY_SAMPLES];
    const int64_t span_s = newest->t_s - oldest->t_s;
    const int drop = (int)oldest->pct - (int)newest->pct;
    if (span_s < BATTERY_MIN_SPAN_S || drop <= 0) {
        return -1;
    }
    /* Prozent / (Prozent je Sekunde) = Sekunden; Minuten daraus, aufgerundet auf volle. */
    const int64_t remaining_s = (int64_t)pct_now * span_s / drop;
    const int64_t minutes = (remaining_s + 59) / 60;
    return minutes > 99 * 60 ? 99 * 60 : (int32_t)minutes;
}

/* "noch 9 h", "noch 40 min"; leer bei -1. `about` setzt "etwa" davor (Zifferblatt). */
static inline void battery_format_remaining(char *out, size_t size, int32_t minutes, bool about)
{
    if (size == 0) {
        return;
    }
    if (minutes < 0) {
        out[0] = '\0';
        return;
    }
    const char *etwa = about ? "etwa " : "";
    if (minutes >= 60) {
        /* Auf ganze Stunden gerundet: 9 h 40 min sind "noch 10 h", nicht "noch 9 h". */
        snprintf(out, size, "noch %s%u h", etwa, (unsigned)((minutes + 30) / 60) % 100u);
    } else {
        snprintf(out, size, "noch %s%u min", etwa, (unsigned)minutes % 60u);
    }
}
