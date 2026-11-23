#ifndef APP_RTC_H
#define APP_RTC_H

#include <stdint.h>

/*
 * Calendar clock on the RTC, used to timestamp the acquisition log.
 *
 * LSE (the 32.768 kHz crystal) is preferred; when it does not start the driver
 * falls back to LSI so a logger still runs on a board without the crystal. The
 * source is reported because LSI drifts by percent, not ppm.
 *
 * NUCLEO boards usually have no VBAT cell, so the calendar restarts from zero
 * after a power cycle; set it from the console once the board is powered.
 */

/* Epoch seconds since 2000-01-01 00:00:00 (not the Unix epoch). */
#define APP_RTC_EPOCH_BASE_YEAR  2000U

/* 0 on success. Safe to call once during start-up; a warm reset keeps the
   running calendar instead of reinitialising it. */
int app_rtc_init(void);

int app_rtc_ready(void);

/* Seconds since 2000-01-01. Returns 0 when the driver is not running. */
uint32_t app_rtc_epoch(void);

/* Set from a civil date/time. Returns 0 on success, negative when a field is
   out of range. */
int app_rtc_set(uint32_t year, uint32_t month, uint32_t day,
                uint32_t hour, uint32_t minute, uint32_t second);

/* Parse "YYYY-MM-DD HH:MM:SS" (also accepts 'T' as the separator). */
int app_rtc_set_from_string(const char *text);

/* "YYYY-MM-DD HH:MM:SS" for the current epoch. Needs >= 20 bytes. */
void app_rtc_format(char *out, uint32_t size);
/* Same, for an arbitrary epoch value. */
void app_rtc_format_epoch(uint32_t epoch, char *out, uint32_t size);

void app_rtc_report(void);

#endif
