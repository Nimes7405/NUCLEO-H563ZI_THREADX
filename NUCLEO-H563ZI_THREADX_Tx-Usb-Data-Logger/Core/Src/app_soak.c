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
#define APP_SOAK_RECORD_MS      (5U * 60U * 1000U)   /* SOAK.LOG cadence */
#define APP_SOAK_RECORD_TICKS   (APP_SOAK_RECORD_MS / 1000U * APP_TICK_HZ)

/* Backup registers: only BKP0R is taken (the RTC warm-start magic), and the
   block is contiguous so the whole ledger can be cleared in one loop. A power
   cycle clears them, which is exactly the semantics wanted: a new run. */
#define SOAK_BKP_MAGIC_R        TAMP->BKP8R
#define SOAK_BKP_BOOTS_R        TAMP->BKP9R
#define SOAK_BKP_SECONDS_R      TAMP->BKP10R
#define SOAK_BKP_LAST_IDX_R     TAMP->BKP11R
#define SOAK_BKP_TARGET_R       TAMP->BKP12R
#define SOAK_BKP_RESETS_R       TAMP->BKP13R

#define SOAK_MAGIC              0x534F414BU   /* "SOAK" */

static uint32_t soak_target_s;      /* 0 when no soak is armed */
static uint32_t soak_elapsed_s;
static uint32_t soak_boots;
static uint32_t soak_resets;
static uint32_t soak_hz;
static uint32_t soak_channels;
static uint32_t soak_last_idx;
static uint32_t soak_tick_acc;
static int soak_active;
static uint32_t soak_records;

static void soak_ledger_clear(void)
{
  /* The first boot of a fresh ledger still counts as one. */
  SOAK_BKP_BOOTS_R = 1U;
  SOAK_BKP_SECONDS_R = 0U;
  SOAK_BKP_LAST_IDX_R = 0U;
  SOAK_BKP_TARGET_R = 0U;
  SOAK_BKP_RESETS_R = 0U;
  SOAK_BKP_MAGIC_R = SOAK_MAGIC;
}

/* Rotation budget from the run length: files x max_bytes has to hold the whole
   capture, otherwise the ring starts overwriting the evidence it exists to
   preserve. 4 MB per file keeps the file count small. */
static int soak_apply_rotation(uint32_t hours, uint32_t hz, uint32_t channels)
{
  /* One record per APP_ACQ_WINDOW_FRAMES frames; ~72 bytes per line at 4
     channels including the CRLF, rounded up to be safe. */
  uint32_t records_per_hour = (hz * 3600U) / APP_ACQ_WINDOW_FRAMES;
  uint32_t bytes_per_hour = records_per_hour * (32U + (16U * channels));
  const uint32_t per_file = 4U * 1024U * 1024U;
  uint32_t need = (bytes_per_hour * hours) / per_file + 2U;

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
  if (app_fxsd_mounted_ready() != 0)
  {
    if (app_fxsd_lock() == 0)
    {
      (void)app_fxsd_write_file("SOAK.LOG", text, 1);
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
  if (soak_active == 0)
  {
    return;
  }

  {
    app_acq_stats_t stats;

    app_acq_get_stats(&stats);
    soak_last_idx = stats.last_index;
    SOAK_BKP_LAST_IDX_R = soak_last_idx;
  }

  soak_tick_acc += APP_MONITOR_PERIOD_MS;
  if (soak_tick_acc < 1000U)
  {
    return;
  }
  soak_tick_acc -= 1000U;
  soak_elapsed_s++;
  SOAK_BKP_SECONDS_R = soak_elapsed_s;

  if (soak_target_s != 0U)
  {
    static uint32_t since_record;

    since_record++;
    if (since_record >= (APP_SOAK_RECORD_MS / 1000U))
    {
      since_record = 0U;
      soak_records++;
      soak_record('R');
    }
    if (soak_elapsed_s >= soak_target_s)
    {
      app_soak_stop();
    }
  }
}

int app_soak_start(uint32_t hours, uint32_t hz, uint32_t channels)
{
  char header[192];
  int rc;

  if ((hours < 1U) || (hours > APP_SOAK_HOURS_MAX))
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
  if (app_fxsd_mounted_ready() == 0)
  {
    return -5;
  }

  rc = app_acq_set_channels(channels);
  if (rc != 0)
  {
    return -6;
  }
  rc = soak_apply_rotation(hours, hz, channels);
  if (rc != 0)
  {
    return -7;
  }

  soak_hz = hz;
  soak_channels = channels;
  soak_target_s = hours * 3600U;
  soak_elapsed_s = 0U;
  soak_last_idx = 0U;
  soak_tick_acc = 0U;
  soak_records = 0U;

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
           "#SOAK1 hours=%lu rate=%lu ch=%lu file_bytes=%lu keep=%lu "
           "footprint=%lu boot_reset=%s boot=%lu\r\n",
           (unsigned long)hours,
           (unsigned long)hz,
           (unsigned long)channels,
           (unsigned long)app_acq_file_max_bytes(),
           (unsigned long)app_acq_file_keep(),
           (unsigned long)(app_acq_file_max_bytes() * app_acq_file_keep()),
           app_boot_reset_name(),
           (unsigned long)soak_boots);
  soak_write(header);

  rc = app_acq_start(1, hz);
  if (rc != 0)
  {
    soak_target_s = 0U;
    SOAK_BKP_TARGET_R = 0U;
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
  SOAK_BKP_TARGET_R = 0U;
}

void app_soak_report(void)
{
  app_acq_stats_t stats;
  uint32_t hours_left = 0U;

  app_acq_get_stats(&stats);
  if ((soak_target_s != 0U) && (soak_elapsed_s < soak_target_s))
  {
    hours_left = (soak_target_s - soak_elapsed_s) / 3600U;
  }

  app_log_printf("SOAK active=%d target=%s hours=%lu elapsed_s=%lu left_h=%lu "
                 "records=%lu\r\n",
                 soak_active,
                 (app_acq_target_name() != NULL) ? app_acq_target_name() : "-",
                 (unsigned long)(soak_target_s / 3600U),
                 (unsigned long)soak_elapsed_s,
                 (unsigned long)hours_left,
                 (unsigned long)soak_records);

  app_log_printf("SOAK ledger boots=%lu resets=%lu last_idx=%lu "
                 "boot_reset=%s\r\n",
                 (unsigned long)soak_boots,
                 (unsigned long)soak_resets,
                 (unsigned long)soak_last_idx,
                 app_boot_reset_name());

  app_log_printf("SOAK files max_bytes=%lu keep=%lu footprint=%lu\r\n",
                 (unsigned long)app_acq_file_max_bytes(),
                 (unsigned long)app_acq_file_keep(),
                 (unsigned long)(app_acq_file_max_bytes() * app_acq_file_keep()));

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
