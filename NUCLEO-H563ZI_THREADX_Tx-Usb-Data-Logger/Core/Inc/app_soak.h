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
/* Called from the monitor thread; cheap when no soak is running. */
void app_soak_poll(void);

/* hours 1..APP_SOAK_HOURS_MAX; sets channels and the rotation budget, arms the
   ledger, writes the header and starts the capture. */
int app_soak_start(uint32_t hours, uint32_t hz, uint32_t channels);
void app_soak_stop(void);
void app_soak_report(void);

int app_soak_active(void);
/* 1 while a soak runs: periodic console output is suppressed. */
int app_soak_quiet(void);

#endif
