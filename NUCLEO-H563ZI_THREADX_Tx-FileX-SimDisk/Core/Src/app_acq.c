#include "app_acq.h"
#include "app_config.h"
#include "app_fxdisk.h"
#include "app_log.h"
#include "app_watchdog.h"

#include <stdio.h>
#include <string.h>

/* Holds four DMA halves so a stalled log write (up to ~40 ms at 50 kHz) does
   not cost samples: one feed pass can enqueue 2 x ACQ_DMA_HALF at once. */
#define ACQ_RING_SAMPLES    2048U
#define ACQ_NAME_LEN        16U
#define ACQ_LINE_MAX        64U

/* TIM3 runs from 250 MHz / (PSC + 1). */
#define ACQ_TIM_PSC         24U
#define ACQ_TIM_INPUT_HZ    10000000U
#define ACQ_RATE_MIN_HZ     1000U
#define ACQ_RATE_MAX_HZ     100000U
#define ACQ_RATE_DEFAULT_HZ 10000U

/* GPDMA writes half-words into a circular buffer; one half is handed to the
   sampler thread while the other half fills. */
#define ACQ_DMA_HALF        512U

#define ACQ_MODE_IRQ        0U
#define ACQ_MODE_DMA        1U

static ADC_HandleTypeDef hadc1;
static TIM_HandleTypeDef htim3;
static DMA_HandleTypeDef hdma_adc1;

static uint16_t acq_dma_buf[2U * ACQ_DMA_HALF] __attribute__((aligned(32)));
static DMA_NodeTypeDef acq_dma_node;
static DMA_QListTypeDef acq_dma_list;
static volatile uint32_t acq_dma_ready;
static volatile uint32_t acq_dma_overruns;
static volatile uint32_t acq_mode;
static uint32_t acq_rate_hz = ACQ_RATE_DEFAULT_HZ;

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
static int acq_tim_config(uint32_t hz);
static int acq_dma_setup(void);
static void acq_dma_feed_ring(void);

void app_acq_init(void)
{
  ADC_ChannelConfTypeDef channel = {0};

  if (acq_tim_config(ACQ_RATE_DEFAULT_HZ) != 0)
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
  hadc1.Init.DMAContinuousRequests = ENABLE;
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

  if (acq_dma_setup() != 0)
  {
    Error_Handler();
  }
}

/* TIM3 update event drives ADC1 start-of-conversion at `hz`. */
static int acq_tim_config(uint32_t hz)
{
  TIM_MasterConfigTypeDef master = {0};
  uint32_t period;

  if ((hz < ACQ_RATE_MIN_HZ) || (hz > ACQ_RATE_MAX_HZ))
  {
    return -1;
  }

  period = ACQ_TIM_INPUT_HZ / hz;
  if (period < 2U)
  {
    return -1;
  }

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = ACQ_TIM_PSC;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = period - 1U;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    return -2;
  }

  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &master) != HAL_OK)
  {
    return -3;
  }

  acq_rate_hz = ACQ_TIM_INPUT_HZ / period;
  return 0;
}

/*
 * The SRAM blocks are attributed secure + privileged-only in the GTZC MPCBB,
 * so the DMA channel and the source/destination *beats* all have to be marked
 * secure (see `dma test`).  Without DMA_CHANNEL_SRC_SEC/DEST_SEC the channel
 * still counts the transfer down but the TZSC drops every beat.
 */
static int acq_dma_setup(void)
{
  DMA_NodeConfTypeDef node = {0};

  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* GPDMA has no plain "circular" bit: circulation comes from a one-node
     linked list whose tail points back at itself. */
  node.NodeType = DMA_GPDMA_LINEAR_NODE;
  node.Init.Request = GPDMA1_REQUEST_ADC1;
  node.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
  node.Init.Direction = DMA_PERIPH_TO_MEMORY;
  node.Init.SrcInc = DMA_SINC_FIXED;
  node.Init.DestInc = DMA_DINC_INCREMENTED;
  node.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_HALFWORD;
  node.Init.DestDataWidth = DMA_DEST_DATAWIDTH_HALFWORD;
  node.Init.SrcBurstLength = 1U;
  node.Init.DestBurstLength = 1U;
  node.Init.Priority = DMA_LOW_PRIORITY_HIGH_WEIGHT;
  node.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0;
  node.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
  node.Init.Mode = DMA_NORMAL;
  node.TriggerConfig.TriggerPolarity = DMA_TRIG_POLARITY_MASKED;
  node.DataHandlingConfig.DataExchange = DMA_EXCHANGE_NONE;
  node.DataHandlingConfig.DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED;
  /* Beat security lives in the node; the channel-level attributes below only
     describe the channel itself.  Both are needed on this board. */
  node.SrcSecure = DMA_CHANNEL_SRC_SEC;
  node.DestSecure = DMA_CHANNEL_DEST_SEC;

  if (HAL_DMAEx_List_BuildNode(&node, &acq_dma_node) != HAL_OK)
  {
    return -1;
  }
  if (HAL_DMAEx_List_InsertNode(&acq_dma_list, NULL, &acq_dma_node) != HAL_OK)
  {
    return -1;
  }
  if (HAL_DMAEx_List_SetCircularMode(&acq_dma_list) != HAL_OK)
  {
    return -1;
  }

  hdma_adc1.Instance = GPDMA1_Channel1;
  hdma_adc1.InitLinkedList.Priority = DMA_LOW_PRIORITY_HIGH_WEIGHT;
  hdma_adc1.InitLinkedList.LinkStepMode = DMA_LSM_FULL_EXECUTION;
  hdma_adc1.InitLinkedList.LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0;
  hdma_adc1.InitLinkedList.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
  hdma_adc1.InitLinkedList.LinkedListMode = DMA_LINKEDLIST_CIRCULAR;
  if (HAL_DMAEx_List_Init(&hdma_adc1) != HAL_OK)
  {
    return -1;
  }
  if (HAL_DMAEx_List_LinkQ(&hdma_adc1, &acq_dma_list) != HAL_OK)
  {
    return -1;
  }
  if (HAL_DMA_ConfigChannelAttributes(&hdma_adc1, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC) != HAL_OK)
  {
    return -1;
  }

  __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1);

  HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, 5U, 0U);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
  return 0;
}

void GPDMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_adc1);
}

/* One conversion is produced per TIM3 update, so the ISR only moves a sample. */
void ADC1_IRQHandler(void)
{
  HAL_ADC_IRQHandler(&hadc1);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance != ADC1)
  {
    return;
  }

  if (acq_mode == ACQ_MODE_DMA)
  {
    uint32_t bit = 1UL << 1U;

    if ((acq_dma_ready & bit) != 0U)
    {
      acq_dma_overruns++;
      return;
    }
    acq_dma_ready |= bit;
    return;
  }

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

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc->Instance == ADC1) && (acq_mode == ACQ_MODE_DMA))
  {
    uint32_t bit = 1UL << 0U;

    if ((acq_dma_ready & bit) != 0U)
    {
      acq_dma_overruns++;
      return;
    }
    acq_dma_ready |= bit;
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    acq_ring_drops++;
  }
}

static void acq_ring_push(uint16_t value)
{
  uint32_t next = (acq_ring_head + 1U) & (ACQ_RING_SAMPLES - 1U);

  if (next == acq_ring_tail)
  {
    acq_ring_drops++;
    return;
  }
  acq_ring[acq_ring_head] = value;
  __DMB();
  acq_ring_head = next;
}

/* Move every DMA half that finished since the last pass into the ring. */
static void acq_dma_feed_ring(void)
{
  for (uint32_t half = 0U; half < 2U; half++)
  {
    uint32_t bit = 1UL << half;

    if ((acq_dma_ready & bit) == 0U)
    {
      continue;
    }
    for (uint32_t i = 0U; i < ACQ_DMA_HALF; i++)
    {
      acq_ring_push(acq_dma_buf[(half * ACQ_DMA_HALF) + i]);
    }
    __DMB();
    acq_dma_ready &= ~bit;
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

int app_acq_start(int use_dma, uint32_t hz)
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
  acq_dma_ready = 0U;
  acq_dma_overruns = 0U;

  if ((hz != 0U) && (hz != acq_rate_hz))
  {
    if (acq_tim_config(hz) != 0)
    {
      return -6;
    }
  }

  if (acq_open_file() != 0)
  {
    return -3;
  }

  acq_mode = (use_dma != 0) ? ACQ_MODE_DMA : ACQ_MODE_IRQ;

  if (acq_mode == ACQ_MODE_DMA)
  {
    /* Re-arm the whole chain on every start so an earlier GPDMA experiment
       cannot leave the channel without its security attributes. */
    if (acq_dma_setup() != 0)
    {
      acq_mode = ACQ_MODE_IRQ;
      (void)app_fxdisk_log_close();
      return -7;
    }

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)(uintptr_t)acq_dma_buf,
                          2U * ACQ_DMA_HALF) != HAL_OK)
    {
      acq_mode = ACQ_MODE_IRQ;
      (void)app_fxdisk_log_close();
      return -4;
    }
  }
  else if (HAL_ADC_Start_IT(&hadc1) != HAL_OK)
  {
    (void)app_fxdisk_log_close();
    return -4;
  }

  if (HAL_TIM_Base_Start(&htim3) != HAL_OK)
  {
    if (acq_mode == ACQ_MODE_DMA)
    {
      (void)HAL_ADC_Stop_DMA(&hadc1);
    }
    else
    {
      (void)HAL_ADC_Stop_IT(&hadc1);
    }
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
  if (acq_mode == ACQ_MODE_DMA)
  {
    (void)HAL_ADC_Stop_DMA(&hadc1);
  }
  else
  {
    (void)HAL_ADC_Stop_IT(&hadc1);
  }
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
      uint32_t pending;

      if (acq_mode == ACQ_MODE_DMA)
      {
        acq_dma_feed_ring();
      }

      pending = (acq_ring_head - acq_ring_tail) & (ACQ_RING_SAMPLES - 1U);
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
  app_log_printf("ACQ running=%d mode=%s rate=%lu file=%s windows=%lu samples=%lu files=%lu min=%lu max=%lu drop=%lu dma_ovr=%lu\r\n",
                 (int)acq_running,
                 (acq_mode == ACQ_MODE_DMA) ? "dma" : "irq",
                 (unsigned long)acq_rate_hz,
                 (acq_current[0] != '\0') ? acq_current : "-",
                 (unsigned long)acq_windows_done,
                 (unsigned long)acq_samples_done,
                 (unsigned long)acq_files_created,
                 (unsigned long)((acq_min == 0xFFFFFFFFU) ? 0U : acq_min),
                 (unsigned long)acq_max,
                 (unsigned long)acq_ring_drops,
                 (unsigned long)acq_dma_overruns);

  if (acq_mode == ACQ_MODE_DMA)
  {
    app_log_printf("ACQ dma ch1 ccr=%08lx csr=%08lx cb1=%08lx sar=%08lx dar=%08lx\r\n",
                   (unsigned long)GPDMA1_Channel1->CCR,
                   (unsigned long)GPDMA1_Channel1->CSR,
                   (unsigned long)GPDMA1_Channel1->CBR1,
                   (unsigned long)GPDMA1_Channel1->CSAR,
                   (unsigned long)GPDMA1_Channel1->CDAR);
  }
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
