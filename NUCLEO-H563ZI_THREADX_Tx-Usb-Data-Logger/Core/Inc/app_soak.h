#ifndef APP_SOAK_H
#define APP_SOAK_H

#include <stdint.h>

/*
 * Unattended long run.
 *
 * The point of a soak is to prove that neither samples nor wall-clock time went
 * missing while nobody was watching, so nothing here depends on a host being
 * connected:
 *
 *   - a ledger in the backup domain counts boots and resets, so a reset in the
 *     middle of the run is visible afterwards even though RAM was wiped
 *   - SOAK.LOG on the same medium as the capture holds periodic evidence of
 *     counter and stack-watermark trends, which is the only way to spot a slow
 *     leak
 *   - console chatter is muted so an absent serial consumer cannot slow the
 *     logger thread down
 */

void app_soak_system_init(void);

/* Two halves on purpose. poll() runs in the monitor thread every tick and only
   does arithmetic: putting the snprintf + FileX write path there overflowed its
   2 KB stack (CFSR STKOF, caught by hardware). service() runs in the console
   thread, which already owns the 4 KB stack that file commands use. */
void app_soak_poll(void);
void app_soak_service(void);

/* duration_s is the whole run, not just whole hours: a five minute trial has to
   exercise the same path as an eight hour one. Sets channels and the rotation
   budget, arms the ledger, writes the header and starts the capture. */
#define APP_SOAK_MIN_SECONDS    30U
/* resume_index is the file ring position to continue from: 0 for a fresh run,
   or the value SOAK.ARM carried for a run that was interrupted. It is a
   parameter rather than hidden state because both the console and the boot
   path call this, and a stale default silently truncated a file twice. */
int app_soak_start(uint32_t duration_s, uint32_t hz, uint32_t channels,
                   uint32_t resume_index);
/* Called once at boot from the console thread, after the file systems are up.
   If SOAK.ARM is on the card the run continues with the remaining time, which
   is what makes swapping the power source safe: a power cycle clears the
   backup domain, so the arm state has to live on the media. */
void app_soak_boot_resume(void);
void app_soak_stop(void);
void app_soak_report(void);

int app_soak_active(void);
/* 1 while a soak runs: periodic console output is suppressed. */
int app_soak_quiet(void);

#endif
