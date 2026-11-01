#pragma once

/*
 * Akku-Protokoll (Paket H, Punkt 1) -- reines C ohne ESP-IDF, damit Ring und
 * JSON-Teile auf dem Rechner mit gcc gegen Beispiele laufen.
 *
 * Zuerst messen, dann sparen: Alle zehn Minuten eine Stichprobe aus Zeit
 * (Unix-Sekunden), Prozent und einer Flags-Bitmaske, die sagt, in welchem
 * Zustand die Uhr dabei war. Die Bruecke holt sie ab und laedt sie zum Server,
 * der daraus die Entladerate je Firmware-Fassung rechnet -- damit sieht man je
 * Aenderung, was sie bringt.
 *
 * Der Ring fasst 288 Proben, also 48 Stunden, und liegt in main.c im
 * RTC-Speicher: Er ueberlebt den Tiefschlaf, nicht den Stromausfall. Die
 * letzten sechs wandern stuendlich in den NVS (main.c), damit nach einem
 * Neustart wenigstens die juengste Stunde bleibt.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BATTERY_LOG_SAMPLES 288
#define BATTERY_LOG_INTERVAL_S (10 * 60)
/* So viele Proben gehen stuendlich in den NVS. */
#define BATTERY_LOG_NVS_SAMPLES 6
/* So viele Proben je Teil, damit ein Teil unter 240 Byte bleibt (siehe battery_log_part_json). */
#define BATTERY_LOG_PART_SAMPLES 8
/* Was eine Benachrichtigung tragen darf: MTU 247 abzueglich ATT-Kopf und 4 Byte Rahmen. */
#define BATTERY_LOG_PART_MAX_BYTES 240

/* Flags-Bitmaske einer Probe (docs/protocol.md, "battery log"). */
#define BATTERY_LOG_FLAG_CHARGING    0x01
#define BATTERY_LOG_FLAG_USB         0x02
#define BATTERY_LOG_FLAG_DISPLAY_ON  0x04
#define BATTERY_LOG_FLAG_BLE         0x08
#define BATTERY_LOG_FLAG_LIGHT_SLEEP 0x10
#define BATTERY_LOG_FLAG_RECORDED    0x20

typedef struct {
    uint32_t t;
    uint8_t p;
    uint8_t f;
} battery_log_sample_t;

typedef struct {
    battery_log_sample_t s[BATTERY_LOG_SAMPLES];
    uint16_t count;
    uint16_t next;
} battery_log_t;

static inline void battery_log_clear(battery_log_t *log)
{
    log->count = 0;
    log->next = 0;
}

/* Die i-te Probe von der aeltesten (0) aus gezaehlt; NULL ausserhalb. */
static inline const battery_log_sample_t *battery_log_at(const battery_log_t *log, size_t i)
{
    if (i >= log->count) {
        return NULL;
    }
    return &log->s[(log->next + BATTERY_LOG_SAMPLES - log->count + i) % BATTERY_LOG_SAMPLES];
}

static inline const battery_log_sample_t *battery_log_newest(const battery_log_t *log)
{
    return log->count ? battery_log_at(log, log->count - 1) : NULL;
}

static inline void battery_log_add(battery_log_t *log, uint32_t t, int pct, uint8_t flags)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    log->s[log->next].t = t;
    log->s[log->next].p = (uint8_t)pct;
    log->s[log->next].f = flags;
    log->next = (uint16_t)((log->next + 1) % BATTERY_LOG_SAMPLES);
    if (log->count < BATTERY_LOG_SAMPLES) {
        log->count++;
    }
}

/* Faellig, wenn seit der juengsten Probe BATTERY_LOG_INTERVAL_S vergangen sind (oder keine da ist). */
static inline bool battery_log_due(const battery_log_t *log, uint32_t now)
{
    const battery_log_sample_t *newest = battery_log_newest(log);
    return !newest || now - newest->t >= BATTERY_LOG_INTERVAL_S || now < newest->t;
}

/* Zahl der Proben mit t > since. Der Ring ist nach Zeit sortiert, also von hinten zaehlen. */
static inline size_t battery_log_count_since(const battery_log_t *log, uint32_t since)
{
    size_t n = 0;
    while (n < log->count && battery_log_at(log, log->count - 1 - n)->t > since) {
        n++;
    }
    return n;
}

/* Zahl der Teile fuer eine Antwort; eine leere Antwort ist ein Teil. */
static inline unsigned battery_log_part_count(size_t samples)
{
    return samples ? (unsigned)((samples + BATTERY_LOG_PART_SAMPLES - 1) / BATTERY_LOG_PART_SAMPLES)
                   : 1u;
}

/*
 * Teil `part` der Antwort auf `battery_log_request` nach `out`:
 *   {"event":"battery_log","part":0,"total":3,"samples":[[t,p,f],…]}
 * Rueckgabe: Zahl der Proben im Teil. Die Rechnung hinter der Grenze von acht
 * Proben: 58 Byte Kopf bei zweistelligen Teilnummern plus 8 * 20 Byte je
 * Probe ("[1762000000,100,63],") plus "]}" sind 218 Byte -- unter den 240,
 * die eine Benachrichtigung traegt.
 */
static inline size_t battery_log_part_json(const battery_log_t *log, uint32_t since, unsigned part,
                                           char *out, size_t size)
{
    const size_t total_samples = battery_log_count_since(log, since);
    const unsigned total = battery_log_part_count(total_samples);
    const size_t first = (size_t)part * BATTERY_LOG_PART_SAMPLES;
    size_t n = 0;
    int len = snprintf(out, size, "{\"event\":\"battery_log\",\"part\":%u,\"total\":%u,\"samples\":[",
                       part, total);
    if (len < 0 || (size_t)len >= size) {
        return 0;
    }
    for (size_t i = first; i < total_samples && n < BATTERY_LOG_PART_SAMPLES; i++, n++) {
        const battery_log_sample_t *s = battery_log_at(log, log->count - total_samples + i);
        const int w = snprintf(out + len, size - (size_t)len, "%s[%lu,%u,%u]", n ? "," : "",
                               (unsigned long)s->t, (unsigned)s->p, (unsigned)s->f);
        if (w < 0 || (size_t)(len + w) >= size) {
            break;
        }
        len += w;
    }
    if ((size_t)len + 2 < size) {
        out[len++] = ']';
        out[len++] = '}';
        out[len] = '\0';
    }
    return n;
}

/*
 * Die letzten BATTERY_LOG_NVS_SAMPLES Proben als Blob (fuer den NVS), und
 * zurueck: der Blob wird nur in einen leeren Ring uebernommen -- nach dem
 * Tiefschlaf ist der RTC-Ring ohnehin vollstaendiger als der Blob.
 */
static inline size_t battery_log_export_tail(const battery_log_t *log, battery_log_sample_t *out,
                                             size_t max)
{
    const size_t n = log->count < max ? log->count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = *battery_log_at(log, log->count - n + i);
    }
    return n;
}

static inline void battery_log_import(battery_log_t *log, const battery_log_sample_t *in, size_t n)
{
    if (log->count != 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        battery_log_add(log, in[i].t, in[i].p, in[i].f);
    }
}
