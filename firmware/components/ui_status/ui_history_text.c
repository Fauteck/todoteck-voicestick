#include "ui_history_text.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

ui_outcome_t ui_outcome_from_name(const char *name)
{
    if (!name) {
        return UI_OUTCOME_NONE;
    }
    if (strcmp(name, "created") == 0) {
        return UI_OUTCOME_CREATED;
    }
    if (strcmp(name, "done") == 0) {
        return UI_OUTCOME_DONE;
    }
    if (strcmp(name, "answer") == 0) {
        return UI_OUTCOME_ANSWER;
    }
    if (strcmp(name, "error") == 0) {
        return UI_OUTCOME_ERROR;
    }
    return UI_OUTCOME_NONE;
}

const char *ui_outcome_word(ui_outcome_t outcome)
{
    switch (outcome) {
    case UI_OUTCOME_CREATED:
        return "angelegt";
    case UI_OUTCOME_DONE:
        return "erledigt";
    case UI_OUTCOME_ANSWER:
        return "Antwort";
    case UI_OUTCOME_ERROR:
        return "Fehler";
    default:
        return "";
    }
}

uint32_t ui_outcome_colour(ui_outcome_t outcome)
{
    switch (outcome) {
    case UI_OUTCOME_CREATED:
    case UI_OUTCOME_DONE:
        return 0x2ecc71;
    case UI_OUTCOME_ERROR:
        return 0xe74c3c;
    default:
        return 0x9aa3ad;
    }
}

int64_t ui_history_local_day(int64_t epoch_utc, int32_t tz_offset_min)
{
    const int64_t local = epoch_utc + (int64_t)tz_offset_min * 60;
    return local >= 0 ? local / 86400 : (local - 86399) / 86400;
}

void ui_history_format_when(char *out, size_t size, int64_t at_utc, int64_t now_utc,
                            int32_t tz_offset_min)
{
    static const char *const WEEKDAYS[7] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" };
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (at_utc <= 0) {
        return;
    }
    const int64_t delta_s = now_utc - at_utc;
    if (delta_s < 60) {
        snprintf(out, size, "gerade eben");
        return;
    }
    if (delta_s < 3600) {
        snprintf(out, size, "vor %u min", (unsigned)(delta_s / 60) % 60u);
        return;
    }
    const time_t local = (time_t)(at_utc + (int64_t)tz_offset_min * 60);
    struct tm parts;
    gmtime_r(&local, &parts);
    char clock[8];
    snprintf(clock, sizeof(clock), "%02u:%02u", (unsigned)parts.tm_hour % 24u,
             (unsigned)parts.tm_min % 60u);
    const int64_t days = ui_history_local_day(now_utc, tz_offset_min) -
                         ui_history_local_day(at_utc, tz_offset_min);
    if (days <= 0) {
        snprintf(out, size, "%s", clock);
    } else if (days == 1) {
        snprintf(out, size, "gestern %s", clock);
    } else if (days < 7) {
        snprintf(out, size, "%s %s", WEEKDAYS[parts.tm_wday % 7], clock);
    } else {
        snprintf(out, size, "%02u.%02u. %s", (unsigned)parts.tm_mday % 32u,
                 (unsigned)(parts.tm_mon + 1) % 13u, clock);
    }
}

void ui_history_format_balance(char *out, size_t size, unsigned commands, unsigned done,
                               unsigned answers, unsigned errors)
{
    snprintf(out, size, "%u %s · %u erledigt · %u %s · %u Fehler",
             commands % 1000u, commands == 1 ? "Befehl" : "Befehle",
             done % 1000u,
             answers % 1000u, answers == 1 ? "Frage" : "Fragen",
             errors % 1000u);
}
