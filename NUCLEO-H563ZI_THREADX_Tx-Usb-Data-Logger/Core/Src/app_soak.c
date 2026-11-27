#include "app_soak.h"
#include "app_acq.h"
#include "app_config.h"
#include "app_fault.h"
#include "app_fxsd.h"
#include "app_log.h"
#include "app_stacks.h"
#include "app_watchdog.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

#define APP_SOAK_HOURS_MAX      48U
/* One record per minute. Five was too coarse: a power cycle loses everything
   since the last record, and on a run whose whole point is endurance, being
   unable to say "how far did it get" is worse than 60 extra bytes a minute. */
#define APP_SOAK_RECORD_S       60U

/* Backup registers: only BKP0R is taken (the RTC warm-start magic), and the
   block is contiguous so the whole ledger can be cleared in one loop. A power
   cycle clears them, which is exactly the semantics wanted: a new run. */
#define SOAK_BKP_MAGIC_R        TAMP->BKP8R
#define SOAK_BKP_BOOTS_R        TAMP->BKP9R
#define SOAK_BKP_SECONDS_R      TAMP->BKP10R
#define SOAK_BKP_LAST_IDX_R     TAMP->BKP11R
#define SOAK_BKP_TARGET_R       TAMP->BKP12R
#define SOAK_BKP_RESETS_R       TAMP->BKP13R
#define SOAK_BKP_FILES_R        TAMP->BKP14R

#define SOAK_MAGIC              0x534F414BU   /* "SOAK" */

static uint32_t soak_target_s;      /* 0 when no soak is armed */
static uint32_t soak_elapsed_s;
static uint32_t soak_boots;
static uint32_t soak_resets;
static uint32_t soak_hz;
static uint32_t soak_channels;
static uint32_t soak_last_idx;
static uint32_t soak_start_tick;   /* elapsed is derived from this, not a counter */
static uint32_t soak_next_record_s;
static int soak_active;
static uint32_t soak_records;
static volatile int soak_record_pending;
static volatile int soak_stop_pending;

static void soak_arm_write(uint32_t total_s, uint32_t hz, uint32_t channels,
                           uint32_t next_file, uint32_t elapsed);

static void soak_ledger_clear(void)
{
  /* The first boot of a fresh ledger still counts as one. */
  SOAK_BKP_BOOTS_R = 1U;
  SOAK_BKP_SECONDS_R = 0U;
  SOAK_BKP_LAST_IDX_R = 0U;
  SOAK_BKP_TARGET_R = 0U;
  SOAK_BKP_RESETS_R = 0U;
  SOAK_BKP_FILES_R = 0U;
  SOAK_BKP_MAGIC_R = SOAK_MAGIC;
}

/* Rotation budget from the run length: files x max_bytes has to hold the whole
   capture, otherwise the ring starts overwriting the evidence it exists to
   preserve. 4 MB per file keeps the file count small. */
static int soak_apply_rotation(uint32_t duration_s, uint32_t hz, uint32_t channels)
{
  /* One record per APP_ACQ_WINDOW_FRAMES frames; 61 bytes measured at 4
     channels including the CRLF, 72 here to leave room. */
  uint32_t records = (hz * duration_s) / APP_ACQ_WINDOW_FRAMES;
  uint32_t bytes = records * (32U + (16U * channels));
  const uint32_t per_file = 4U * 1024U * 1024U;
  uint32_t need = (bytes / per_file) + 2U;

  if (need < 2U)
  {
    need = 2U;
  }
  if (need > 24U)
  {
    need = 24U;
  }
  return app_acq_set_files(per_file, need);
}

static void soak_write(const char *text)
{
  /* SOAK.LOG shares the card with the capture, so it goes through the same
     lock the acquisition writer uses. */
  /* app_fxsd_mounted_ready() returns 0 when mounted; is_mounted() returns 1
     and is the less surprising of the two to read here. */
  if (app_fxsd_is_mounted() != 0)
  {
    if (app_fxsd_lock() == 0)
    {
      (void)app_fxsd_append_lazy("SOAK.LOG", text);
      (void)app_fxsd_unlock();
    }
  }
}

static void soak_record(char kind)
{
  app_acq_stats_t stats;
  char line[224];
  int len;

  app_acq_get_stats(&stats);

  len = snprintf(line, sizeof(line),
                 "%c uptime_s=%lu elapsed_s=%lu boots=%lu resets=%lu "
                 "windows=%lu samples=%lu drop=%lu dma_ovr=%lu qdrop=%lu "
                 "last_idx=%lu log_drop=%lu "
                 "stk_m=%lu stk_c=%lu stk_w=%lu\r\n",
                 kind,
                 (unsigned long)((uint64_t)tx_time_get() / APP_TICK_HZ),
                 (unsigned long)soak_elapsed_s,
                 (unsigned long)soak_boots,
                 (unsigned long)soak_resets,
                 (unsigned long)stats.windows,
                 (unsigned long)stats.samples,
                 (unsigned long)stats.drops,
                 (unsigned long)stats.dma_overruns,
                 (unsigned long)stats.queue_drops,
                 (unsigned long)soak_last_idx,
                 (unsigned long)app_log_dropped(),
                 (unsigned long)app_stacks_used(APP_STK_MONITOR),
                 (unsigned long)app_stacks_used(APP_STK_CONSOLE),
                 (unsigned long)app_stacks_used(APP_STK_WRITER));

  if (len > 0)
  {
    soak_write(line);
  }
}

void app_soak_system_init(void)
{
  if (SOAK_BKP_MAGIC_R != SOAK_MAGIC)
  {
    soak_ledger_clear();
    return;
  }

  /* A live magic means the backup domain kept running: this boot is a reset,
     not a power cycle. That is the only way to notice a mid-run reset. */
  soak_boots = SOAK_BKP_BOOTS_R + 1U;
  soak_elapsed_s = SOAK_BKP_SECONDS_R;
  soak_last_idx = SOAK_BKP_LAST_IDX_R;
  soak_target_s = SOAK_BKP_TARGET_R;
  soak_resets = SOAK_BKP_RESETS_R;

  if (soak_target_s != 0U)
  {
    soak_resets++;
  }

  SOAK_BKP_BOOTS_R = soak_boots;
  SOAK_BKP_RESETS_R = soak_resets;
}

void app_soak_poll(void)
{
  uint32_t elapsed;

  if (soak_active == 0)
  {
    return;
  }

  {
    app_acq_stats_t stats;

    app_acq_get_stats(&stats);
    soak_last_idx = stats.last_index;
    SOAK_BKP_LAST_IDX_R = soak_last_idx;
    SOAK_BKP_FILES_R = stats.files;
  }

  /* Derive elapsed from the kernel tick instead of counting iterations: the
     monitor thread does not run at an exact 100 ms, and a counter drifts
     against wall clock - an 8 hour target would finish early. */
  elapsed = ((uint32_t)tx_time_get() - soak_start_tick) / APP_TICK_HZ;
  if (elapsed == soak_elapsed_s)
  {
    return;
  }
  soak_elapsed_s = elapsed;
  SOAK_BKP_SECONDS_R = soak_elapsed_s;

  if (soak_target_s == 0U)
  {
    return;
  }

  if (soak_elapsed_s >= soak_next_record_s)
  {
    soak_next_record_s = soak_elapsed_s + APP_SOAK_RECORD_S;
    soak_record_pending = 1;
  }
  if (soak_elapsed_s >= soak_target_s)
  {
    soak_stop_pending = 1;
  }
}

/* Console thread: the only place that touches the card on the soak's behalf. */
void app_soak_service(void)
{
  if (soak_stop_pending != 0)
  {
    soak_stop_pending = 0;
    soak_record_pending = 0;
    app_soak_stop();
    return;
  }
  if (soak_record_pending != 0)
  {
    soak_record_pending = 0;
    soak_records++;
    soak_record('R');
    soak_arm_write(soak_target_s, soak_hz, soak_channels,
                   app_acq_file_index(), soak_elapsed_s);
  }
}

/* SOAK.ARM: "<total_s> <hz> <ch>". It lives on the card because the backup
   domain dies with the power, and the whole point is to survive a power swap. */
/* The arm file carries progress as well as parameters. The backup domain dies
   with the power, so without this a resumed run restarts the name ring at
   LOG0000 and truncates the file the previous segment wrote - losing exactly
   the evidence the soak exists to collect. Rewritten every record and at
   truncating (append=0), so it never grows. */
static void soak_arm_write(uint32_t total_s, uint32_t hz, uint32_t channels,
                           uint32_t next_file, uint32_t elapsed)
{
  char text[80];

  if (app_fxsd_is_mounted() == 0)
  {
    return;
  }
  snprintf(text, sizeof(text), "%lu %lu %lu %lu %lu\n",
           (unsigned long)total_s, (unsigned long)hz, (unsigned long)channels,
           (unsigned long)next_file, (unsigned long)elapsed);
  if (app_fxsd_lock() == 0)
  {
    (void)app_fxsd_write_file("SOAK.ARM", text, 0);
    (void)app_fxsd_unlock();
  }
}

static void soak_arm_clear(void)
{
  if (app_fxsd_is_mounted() == 0)
  {
    return;
  }
  if (app_fxsd_lock() == 0)
  {
    (void)app_fxsd_delete("SOAK.ARM");
    (void)app_fxsd_unlock();
  }
}

/*
 * A power cycle wipes the backup domain, so the ledger alone cannot say how far
 * the run got. SOAK.LOG holds "elapsed_s=<n>" every 5 minutes; recover the last
 * one so a power blip resumes with the remaining time instead of replaying the
 * whole duration.
 */
typedef struct
{
  char line[176];
  uint32_t used;
  uint32_t best;
} soak_scan_t;

static void soak_scan_line(soak_scan_t *scan)
{
  const char *hit = strstr(scan->line, "elapsed_s=");

  if (hit != NULL)
  {
    uint32_t value = (uint32_t)strtoul(hit + 10, NULL, 10);

    if (value > scan->best)
    {
      scan->best = value;
    }
  }
  scan->used = 0U;
  scan->line[0] = '\0';
}

static int soak_log_scan_chunk(const char *data, uint32_t len, void *ctx)
{
  soak_scan_t *scan = (soak_scan_t *)ctx;

  for (uint32_t i = 0U; i < len; i++)
  {
    char c = data[i];

    if ((c == '\n') || (c == '\r'))
    {
      if (scan->used > 0U)
      {
        soak_scan_line(scan);
      }
      continue;
    }
    if (scan->used < (sizeof(scan->line) - 1U))
    {
      scan->line[scan->used++] = c;
      scan->line[scan->used] = '\0';
    }
  }
  return 0;
}

static uint32_t soak_recover_elapsed(void)
{
  soak_scan_t scan;
  uint32_t total = 0U;

  memset(&scan, 0, sizeof(scan));

  if (app_fxsd_is_mounted() == 0)
  {
    return 0U;
  }
  if (app_fxsd_lock() != 0)
  {
    return 0U;
  }
  (void)app_fxsd_read_stream("SOAK.LOG", soak_log_scan_chunk, &scan, &total);
  (void)app_fxsd_unlock();

  return scan.best;
}

void app_soak_boot_resume(void)
{
  char text[48];
  uint32_t len = 0U;
  unsigned long total_s = 0UL;
  unsigned long hz = 0UL;
  unsigned long channels = 0UL;
  unsigned long next_file = 0UL;
  unsigned long arm_elapsed = 0UL;
  uint32_t remaining;
  uint32_t recovered;

  /*
   * This runs in the console thread, which cannot mark its own heartbeat while
   * blocked here. Mounting a failing card costs several timeout periods, and
   * the watchdog stops feeding IWDG after 1.5 s - without these marks a dead
   * card turned boot into an IWDG reset loop, which is how this was found.
   */
  app_hb_mark(APP_HB_CONSOLE);

  /* The acquisition target is not remembered across a power cycle, so a run
     that was pointed at the card has to re-mount it to find its own arm file. */
  if (app_fxsd_is_mounted() == 0)
  {
    if (app_acq_set_target(APP_ACQ_TARGET_SD) != 0)
    {
      /* Leave SOAK.ARM alone: the next boot with a working card resumes. */
      return;
    }
    app_hb_mark(APP_HB_CONSOLE);
  }

  if (app_fxsd_read_file("SOAK.ARM", text, sizeof(text) - 1U, &len) != 0)
  {
    return;
  }
  app_hb_mark(APP_HB_CONSOLE);
  text[len] = '\0';

  /* strtoul rather than sscanf: the scanf family drags in enough newlib to
     cost ~45 KB of flash for three integers. */
  {
    char *end = text;

    total_s = strtoul(end, &end, 10);
    hz = strtoul(end, &end, 10);
    channels = strtoul(end, &end, 10);
    next_file = strtoul(end, &end, 10);
    arm_elapsed = strtoul(end, &end, 10);
  }
  if ((total_s == 0UL) || (hz == 0UL) || (channels == 0UL))
  {
    soak_arm_clear();
    return;
  }

  /* Three sources, best wins: the backup domain is exact when it survived a
     reset, the arm file is rewritten every minute, and the log is the
     last-resort fallback if the arm file was lost. */
  {
    uint32_t from_log = soak_recover_elapsed();

    if (from_log > soak_elapsed_s)
    {
      soak_elapsed_s = from_log;
    }
    if ((uint32_t)arm_elapsed > soak_elapsed_s)
    {
      soak_elapsed_s = (uint32_t)arm_elapsed;
    }
  }

  if (total_s < soak_elapsed_s)
  {
    soak_elapsed_s = (uint32_t)total_s;
  }
  remaining = (uint32_t)total_s - soak_elapsed_s;

  /* start() clears the counters, so keep what was recovered here. */
  recovered = soak_elapsed_s;

  if (remaining < APP_SOAK_MIN_SECONDS)
  {
    soak_arm_clear();
    return;
  }

  if (app_soak_start(remaining, (uint32_t)hz, (uint32_t)channels) != 0)
  {
    return;
  }

  /*
   * start() re-initialises everything it owns, so the resumed state has to be
   * restored after it, not before:
   *   - set_files() inside start() resets the file index, which would undo the
   *     resume point and truncate the file the previous segment wrote
   *   - soak_elapsed_s is zeroed, which would restart the run from zero
   *   - the arm file was rewritten with the remaining duration, so a second
   *     power cycle would shorten the run again
   */
  (void)app_acq_resume_index((uint32_t)next_file);

  soak_elapsed_s = recovered;
  soak_target_s = (uint32_t)total_s;
  soak_start_tick -= recovered * APP_TICK_HZ;
  soak_next_record_s = recovered + APP_SOAK_RECORD_S;

  SOAK_BKP_TARGET_R = soak_target_s;
  SOAK_BKP_SECONDS_R = recovered;
  soak_arm_write(soak_target_s, (uint32_t)hz, (uint32_t)channels,
                 (uint32_t)next_file, recovered);
}

int app_soak_start(uint32_t duration_s, uint32_t hz, uint32_t channels)
{
  char header[192];
  int rc;

  if ((duration_s < APP_SOAK_MIN_SECONDS) || (duration_s > (APP_SOAK_HOURS_MAX * 3600U)))
  {
    return -1;
  }
  if ((hz < 1000U) || (hz > 100000U))
  {
    return -2;
  }
  if ((channels < 1U) || (channels > APP_ACQ_CH_MAX))
  {
    return -3;
  }
  if (app_acq_running() != 0)
  {
    return -4;
  }
  if (app_fxsd_is_mounted() == 0)
  {
    return -5;
  }

  rc = app_acq_set_channels(channels);
  if (rc != 0)
  {
    return -6;
  }
  rc = soak_apply_rotation(duration_s, hz, channels);
  if (rc != 0)
  {
    return -7;
  }

  soak_hz = hz;
  soak_channels = channels;
  soak_target_s = duration_s;
  soak_elapsed_s = 0U;
  soak_last_idx = 0U;
  soak_records = 0U;
  soak_record_pending = 0;
  soak_stop_pending = 0;
  soak_start_tick = (uint32_t)tx_time_get();
  soak_next_record_s = APP_SOAK_RECORD_S;

  /* Re-arming must not inherit the previous run's reset count. */
  {
    uint32_t boots = (soak_boots == 0U) ? 1U : soak_boots;

    SOAK_BKP_MAGIC_R = SOAK_MAGIC;
    SOAK_BKP_BOOTS_R = boots;
    SOAK_BKP_RESETS_R = 0U;
    SOAK_BKP_SECONDS_R = 0U;
    SOAK_BKP_LAST_IDX_R = 0U;
    SOAK_BKP_TARGET_R = soak_target_s;
    soak_resets = 0U;
  }

  snprintf(header, sizeof(header),
           "#SOAK1 duration_s=%lu rate=%lu ch=%lu file_bytes=%lu keep=%lu "
           "footprint=%lu boot_reset=%s boot=%lu\r\n",
           (unsigned long)duration_s,
           (unsigned long)hz,
           (unsigned long)channels,
           (unsigned long)app_acq_file_max_bytes(),
           (unsigned long)app_acq_file_keep(),
           (unsigned long)(app_acq_file_max_bytes() * app_acq_file_keep()),
           app_boot_reset_name(),
           (unsigned long)soak_boots);
  soak_write(header);
  soak_arm_write(duration_s, hz, channels, 0U, 0U);

  rc = app_acq_start(1, hz);
  if (rc != 0)
  {
    soak_target_s = 0U;
    SOAK_BKP_TARGET_R = 0U;
    soak_arm_clear();
    return -8;
  }

  soak_active = 1;
  return 0;
}

void app_soak_stop(void)
{
  if (soak_active != 0)
  {
    soak_record('E');
    app_acq_stop();
  }
  soak_active = 0;
  soak_target_s = 0U;
  soak_record_pending = 0;
  soak_stop_pending = 0;
  SOAK_BKP_TARGET_R = 0U;
  SOAK_BKP_FILES_R = 0U;
  soak_arm_clear();
}

void app_soak_report(void)
{
  app_acq_stats_t stats;
  uint32_t seconds_left = 0U;

  app_acq_get_stats(&stats);
  if ((soak_target_s != 0U) && (soak_elapsed_s < soak_target_s))
  {
    seconds_left = soak_target_s - soak_elapsed_s;
  }

  app_log_printf("SOAK active=%d target=%s duration_s=%lu elapsed_s=%lu left_s=%lu "
                 "records=%lu\r\n",
                 soak_active,
                 (app_acq_target_name() != NULL) ? app_acq_target_name() : "-",
                 (unsigned long)soak_target_s,
                 (unsigned long)soak_elapsed_s,
                 (unsigned long)seconds_left,
                 (unsigned long)soak_records);

  app_log_printf("SOAK ledger boots=%lu resets=%lu last_idx=%lu "
                 "boot_reset=%s\r\n",
                 (unsigned long)soak_boots,
                 (unsigned long)soak_resets,
                 (unsigned long)soak_last_idx,
                 app_boot_reset_name());

  app_log_printf("SOAK files max_bytes=%lu keep=%lu footprint=%lu next_file=%lu\r\n",
                 (unsigned long)app_acq_file_max_bytes(),
                 (unsigned long)app_acq_file_keep(),
                 (unsigned long)(app_acq_file_max_bytes() * app_acq_file_keep()),
                 (unsigned long)app_acq_file_index());

  app_log_printf("SOAK acq running=%lu mode=%s rate=%lu ch=%lu windows=%lu "
                 "samples=%lu files=%lu drop=%lu dma_ovr=%lu qdrop=%lu\r\n",
                 (unsigned long)stats.running,
                 (stats.dma_mode != 0U) ? "dma" : "irq",
                 (unsigned long)stats.rate_hz,
                 (unsigned long)stats.channels,
                 (unsigned long)stats.windows,
                 (unsigned long)stats.samples,
                 (unsigned long)stats.files,
                 (unsigned long)stats.drops,
                 (unsigned long)stats.dma_overruns,
                 (unsigned long)stats.queue_drops);

  if (soak_resets != 0U)
  {
    app_log_printf("SOAK WARN resets-during-soak=%lu (a reset restarts the "
                   "window index; cross-check SOAK.LOG)\r\n",
                   (unsigned long)soak_resets);
  }
}

int app_soak_active(void)
{
  return soak_active;
}

int app_soak_quiet(void)
{
  return soak_active;
}
