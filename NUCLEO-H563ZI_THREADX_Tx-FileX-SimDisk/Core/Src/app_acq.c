#include "app_acq.h"
#include "app_config.h"
#include "app_fxdisk.h"
#include "app_log.h"
#include "app_watchdog.h"

#include <stdio.h>
#include <string.h>

#define ACQ_RING_SAMPLES    1024U
#define ACQ_NAME_LEN        16U
#define ACQ_LINE_MAX        64U

static ADC_HandleTypeDef hadc1;
static TIM_HandleTypeDef htim3;

static volatile uint16_t acq_ring[ACQ_RING_SAMPLES];
static volatile uint32_t acq_ring_head;
static volatile uint32_t acq_ring_tail;
static volatile uint32_t acq_ring_drops;

static volatile uint32_t acq_running;
static uint32_t acq_windows_done;
static uint32_t acq_samples_done;
static uint32_t acq_files_created;
static uint32_t acq_file_index;
static uint32_t acq_min;
static uint32_t acq_max;
static uint64_t acq_sum;
static char acq_current[ACQ_NAME_LEN];

typedef struct
{
  char line[ACQ_LINE_MAX];
  uint32_t used;
  uint32_t lines;
  uint32_t samples;
  uint32_t min;
  uint32_t max;
  uint32_t bad_line;
  int failed;
} acq_parse_t;

static int acq_open_file(void);
static void acq_process_window(void);

void app_acq_init(void)
{
  TIM_MasterConfigTypeDef master = {0};
  ADC_ChannelConfTypeDef channel = {0};

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 24U;      /* 250 MHz / 25 = 10 MHz */
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999U;        /* 10 MHz / 1000 = 10 kHz */
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &master) != HAL_OK)
  {
    Error_Handler();
  }

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1U;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T3_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.SamplingMode = ADC_SAMPLING_MODE_NORMAL;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  channel.Channel = ADC_CHANNEL_10; /* PC0 / Arduino A1 */
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_24CYCLES_5;
  channel.SingleDiff = ADC_SINGLE_ENDED;
  channel.OffsetNumber = ADC_OFFSET_NONE;
  channel.Offset = 0U;
  if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
}

/* One conversion is produced per TIM3 update, so the ISR only moves a sample. */
void ADC1_IRQHandler(void)
{
  HAL_ADC_IRQHandler(&hadc1);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    uint32_t next = (acq_ring_head + 1U) & (ACQ_RING_SAMPLES - 1U);

    if (next == acq_ring_tail)
    {
      acq_ring_drops++;
      return;
    }
    acq_ring[acq_ring_head] = (uint16_t)HAL_ADC_GetValue(hadc);
    __DMB();
    acq_ring_head = next;
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    acq_ring_drops++;
  }
}

static int acq_open_file(void)
{
  int rc;

  snprintf(acq_current, sizeof(acq_current), "LOG%04lu.CSV",
           (unsigned long)(acq_file_index % APP_ACQ_FILE_KEEP));

  rc = app_fxdisk_log_open(acq_current);
  if (rc != 0)
  {
    return rc;
  }

  acq_file_index++;
  if (acq_files_created < APP_ACQ_FILE_KEEP)
  {
    acq_files_created++;
  }
  return 0;
}

int app_acq_start(void)
{
  if (acq_running != 0U)
  {
    return -1;
  }
  if (app_fxdisk_ready() != 0)
  {
    return -2;
  }

  acq_ring_head = 0U;
  acq_ring_tail = 0U;
  acq_ring_drops = 0U;
  acq_windows_done = 0U;
  acq_samples_done = 0U;
  acq_file_index = 0U;
  acq_files_created = 0U;
  acq_min = 0xFFFFFFFFU;
  acq_max = 0U;
  acq_sum = 0U;

  if (acq_open_file() != 0)
  {
    return -3;
  }

  if (HAL_ADC_Start_IT(&hadc1) != HAL_OK)
  {
    (void)app_fxdisk_log_close();
    return -4;
  }

  if (HAL_TIM_Base_Start(&htim3) != HAL_OK)
  {
    (void)HAL_ADC_Stop_IT(&hadc1);
    (void)app_fxdisk_log_close();
    return -5;
  }

  acq_running = 1U;
  return 0;
}

void app_acq_stop(void)
{
  if (acq_running == 0U)
  {
    return;
  }

  (void)HAL_TIM_Base_Stop(&htim3);
  (void)HAL_ADC_Stop_IT(&hadc1);
  (void)app_fxdisk_log_close();
  acq_running = 0U;
}

int app_acq_running(void)
{
  return (acq_running != 0U) ? 1 : 0;
}

static void acq_process_window(void)
{
  uint32_t minimum = 0xFFFFU;
  uint32_t maximum = 0U;
  uint32_t sum = 0U;
  uint32_t size = 0UL;
  char line[ACQ_LINE_MAX];
  int len;

  for (uint32_t i = 0U; i < APP_ACQ_WINDOW_SAMPLES; i++)
  {
    uint32_t next = (acq_ring_tail + 1U) & (ACQ_RING_SAMPLES - 1U);
    uint32_t value;

    if (next == acq_ring_head)
    {
      break;
    }
    value = acq_ring[acq_ring_tail];
    __DMB();
    acq_ring_tail = next;

    if (value < minimum)
    {
      minimum = value;
    }
    if (value > maximum)
    {
      maximum = value;
    }
    sum += value;
  }

  if (sum == 0U && maximum == 0U && minimum == 0xFFFFU)
  {
    return;
  }

  if (minimum < acq_min)
  {
    acq_min = minimum;
  }
  if (maximum > acq_max)
  {
    acq_max = maximum;
  }
  acq_sum += sum;
  acq_samples_done += APP_ACQ_WINDOW_SAMPLES;

  len = snprintf(line, sizeof(line), "%lu,%lu,%lu,%lu,%lu\r\n",
                 (unsigned long)acq_windows_done,
                 (unsigned long)minimum,
                 (unsigned long)maximum,
                 (unsigned long)(sum / APP_ACQ_WINDOW_SAMPLES),
                 (unsigned long)APP_ACQ_WINDOW_SAMPLES);
  if ((len > 0) && (app_fxdisk_log_write(line, (uint32_t)len) == 0))
  {
    acq_windows_done++;
  }

  if ((app_fxdisk_log_size(&size) == 0) && (size >= APP_ACQ_FILE_MAX_BYTES))
  {
    (void)app_fxdisk_log_close();
    (void)acq_open_file();
  }
}

void app_acq_thread_entry(unsigned long thread_input)
{
  (void)thread_input;

  for (;;)
  {
    app_hb_mark(APP_HB_SAMPLER);

    if (acq_running != 0U)
    {
      uint32_t pending = (acq_ring_head - acq_ring_tail) & (ACQ_RING_SAMPLES - 1U);
      if (pending >= APP_ACQ_WINDOW_SAMPLES)
      {
        acq_process_window();
      }
    }

    tx_thread_sleep(1);
  }
}

void app_acq_report(void)
{
  app_log_printf("ACQ running=%d file=%s windows=%lu samples=%lu files=%lu min=%lu max=%lu drop=%lu\r\n",
                 (int)acq_running,
                 (acq_current[0] != '\0') ? acq_current : "-",
                 (unsigned long)acq_windows_done,
                 (unsigned long)acq_samples_done,
                 (unsigned long)acq_files_created,
                 (unsigned long)((acq_min == 0xFFFFFFFFU) ? 0U : acq_min),
                 (unsigned long)acq_max,
                 (unsigned long)acq_ring_drops);
}

static void acq_feed_line(acq_parse_t *ctx)
{
  unsigned long index;
  unsigned long minimum;
  unsigned long maximum;
  unsigned long average;
  unsigned long count;

  ctx->line[ctx->used] = '\0';
  ctx->used = 0U;

  if (sscanf(ctx->line, "%lu,%lu,%lu,%lu,%lu",
             &index, &minimum, &maximum, &average, &count) != 5)
  {
    if (ctx->failed == 0)
    {
      ctx->bad_line = ctx->lines + 1U;
      ctx->failed = 1;
    }
    return;
  }

  if ((minimum > maximum) || (average < minimum) || (average > maximum) ||
      (maximum > 4095UL) || (count != APP_ACQ_WINDOW_SAMPLES))
  {
    if (ctx->failed == 0)
    {
      ctx->bad_line = ctx->lines + 1U;
      ctx->failed = 1;
    }
    return;
  }

  if (minimum < ctx->min)
  {
    ctx->min = (uint32_t)minimum;
  }
  if (maximum > ctx->max)
  {
    ctx->max = (uint32_t)maximum;
  }
  ctx->samples += (uint32_t)count;
  ctx->lines++;
}

static int acq_parse_chunk(const char *data, uint32_t len, void *arg)
{
  acq_parse_t *ctx = (acq_parse_t *)arg;

  for (uint32_t i = 0U; i < len; i++)
  {
    char c = data[i];

    if (c == '\n')
    {
      if (ctx->used > 0U)
      {
        acq_feed_line(ctx);
      }
      continue;
    }

    if (c == '\r')
    {
      continue;
    }

    if (ctx->used < (ACQ_LINE_MAX - 1U))
    {
      ctx->line[ctx->used++] = c;
    }
    else if (ctx->failed == 0)
    {
      ctx->bad_line = ctx->lines + 1U;
      ctx->failed = 1;
      ctx->used = 0U;
    }
  }

  return 0;
}

int app_acq_check(void)
{
  acq_parse_t ctx;
  uint32_t total = 0U;
  int rc;

  memset(&ctx, 0, sizeof(ctx));
  ctx.min = 0xFFFFFFFFU;

  if (acq_current[0] == '\0')
  {
    app_log_printf("ACQ CHECK FAIL reason=no-file\r\n");
    return -1;
  }

  rc = app_fxdisk_read_stream(acq_current, acq_parse_chunk, &ctx, &total);
  if ((ctx.used > 0U) && (rc == 0))
  {
    acq_feed_line(&ctx);
  }

  if ((rc != 0) || (ctx.failed != 0) || (ctx.lines == 0U))
  {
    app_log_printf("ACQ CHECK FAIL file=%s rc=%d line=%lu lines=%lu bytes=%lu\r\n",
                   acq_current, rc, (unsigned long)ctx.bad_line,
                   (unsigned long)ctx.lines, (unsigned long)total);
    return -2;
  }

  app_log_printf("ACQ CHECK PASS file=%s lines=%lu samples=%lu min=%lu max=%lu bytes=%lu\r\n",
                 acq_current, (unsigned long)ctx.lines, (unsigned long)ctx.samples,
                 (unsigned long)ctx.min, (unsigned long)ctx.max, (unsigned long)total);
  return 0;
}
