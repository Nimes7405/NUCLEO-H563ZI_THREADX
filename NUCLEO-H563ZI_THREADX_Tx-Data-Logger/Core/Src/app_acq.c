#include "app_acq.h"
#include "app_config.h"
#include "app_fxdisk.h"
#include "app_fxsd.h"
#include "app_log.h"
#include "app_sdspi.h"
#include "app_watchdog.h"

#include <stdio.h>
#include <string.h>

/*
 * The ring stores *interleaved* samples (ch0, ch1, ... per frame), so one
 * window costs CH_MAX * WINDOW_FRAMES samples. Must stay a power of two: the
 * head/tail bookkeeping masks indices. 8192 holds four 4-channel windows.
 */
#define ACQ_RING_SAMPLES    8192U

typedef char acq_ring_pow2_check[(ACQ_RING_SAMPLES & (ACQ_RING_SAMPLES - 1U)) == 0U ? 1 : -1];
typedef char acq_ring_fit_check[(ACQ_RING_SAMPLES >=
                                 (2U * APP_ACQ_CH_MAX * APP_ACQ_WINDOW_FRAMES)) ? 1 : -1];
#define ACQ_NAME_LEN        16U
#define ACQ_LINE_MAX        160U

/* TIM3 runs from 250 MHz / (PSC + 1). */
#define ACQ_TIM_PSC         24U
#define ACQ_TIM_INPUT_HZ    10000000U
#define ACQ_RATE_MIN_HZ     1000U
#define ACQ_RATE_MAX_HZ     100000U
#define ACQ_RATE_DEFAULT_HZ 10000U

/*
 * GPDMA writes half-words into a circular buffer; one half is handed to the
 * sampler thread while the other half fills.  The half is sized in *frames*
 * (one sample per channel) so it always spans >= ~5 ms regardless of the
 * channel count, otherwise a 4-channel run fills both halves inside one tick.
 */
#define ACQ_DMA_FRAMES_MIN  64U
#define ACQ_DMA_FRAMES_MAX  1024U

/* A medium that keeps timing out must not stall the writer forever: after this
   many consecutive failures the writer stops touching it and drops records, so
   the console and the watchdog stay alive. */
#define ACQ_MEDIA_ERROR_LIMIT 8U

/* How often the writer pushes the FileX cache to the card. */
#define ACQ_FLUSH_INTERVAL_MS 100U

#define ACQ_MODE_IRQ        0U
#define ACQ_MODE_DMA        1U

/* The sampler only produces window records; a separate thread owns the log
   file. Batching 50 ms of records costs one `fx_media_flush()` per batch
   instead of one per line, which is what caps the writing throughput. */
#define ACQ_WRITE_PERIOD_TICKS  5U
#define ACQ_QUEUE_DEPTH         32U

static ADC_HandleTypeDef hadc1;
static TIM_HandleTypeDef htim3;
static DMA_HandleTypeDef hdma_adc1;

static uint16_t acq_dma_buf[2U * ACQ_DMA_FRAMES_MAX * APP_ACQ_CH_MAX] __attribute__((aligned(32)));
static uint32_t acq_dma_half;      /* samples per half = frames * channels */
static DMA_NodeTypeDef acq_dma_node;
static DMA_QListTypeDef acq_dma_list;
static volatile uint32_t acq_dma_ready;
static volatile uint32_t acq_dma_overruns;
static volatile uint32_t acq_mode;
static uint32_t acq_rate_hz = ACQ_RATE_DEFAULT_HZ;

static uint32_t acq_channels = 1U;        /* configured before start */
static volatile uint32_t acq_eval_channels = 1U; /* channel count of the open log */

static volatile uint16_t acq_ring[ACQ_RING_SAMPLES];
static volatile uint32_t acq_ring_head;
static volatile uint32_t acq_ring_tail;
static volatile uint32_t acq_ring_drops;

static volatile uint32_t acq_running;
static volatile uint32_t acq_isr_samples;   /* every EOC/DMA-frame the ISR saw */
static uint32_t acq_windows_done;
static uint32_t acq_samples_done;
static uint32_t acq_files_created;
static uint32_t acq_file_index;
static uint32_t acq_min;
static uint32_t acq_max;
static uint64_t acq_sum;
static char acq_current[ACQ_NAME_LEN];

/* ADC ranks this project supports, in channel order.  All four sit on GPIOC;
   only PC0 (Arduino A1) is verified against a signal, the rest are wired on
   the CN9/CN10 header and read whatever is connected there. */
typedef struct
{
  uint32_t adc_channel;
  uint16_t pin;
  const char *name;
} acq_chan_desc_t;

static const acq_chan_desc_t acq_chan_table[APP_ACQ_CH_MAX] = {
    {ADC_CHANNEL_10, GPIO_PIN_0, "PC0"},
    {ADC_CHANNEL_11, GPIO_PIN_1, "PC1"},
    {ADC_CHANNEL_12, GPIO_PIN_2, "PC2"},
    {ADC_CHANNEL_13, GPIO_PIN_3, "PC3"},
};

typedef struct
{
  uint32_t index;
  uint32_t count;      /* frames (samples per channel) in this window */
  uint32_t channels;
  uint32_t min[APP_ACQ_CH_MAX];
  uint32_t max[APP_ACQ_CH_MAX];
  uint32_t avg[APP_ACQ_CH_MAX];
} acq_record_t;

/*
 * The window writer is the same either way; only the medium differs, so the
 * two FileX volumes are hidden behind this small operation table. The simulated
 * NOR disk stays the default because it needs no card.
 */
typedef int (*acq_chunk_fn)(const char *data, uint32_t len, void *ctx);

typedef struct
{
  const char *name;
  int (*ready)(void);
  int (*lock)(void);
  int (*unlock)(void);
  int (*log_open)(const char *name);
  int (*log_write)(const void *data, uint32_t len);
  int (*log_close)(void);
  int (*log_flush)(void);
  int (*log_size)(uint32_t *size);
  int (*read_stream)(const char *name, acq_chunk_fn fn, void *ctx, uint32_t *total);
} acq_media_ops_t;

static const acq_media_ops_t acq_media_sim = {
    "sim",
    app_fxdisk_ready,
    app_fxdisk_lock,
    app_fxdisk_unlock,
    app_fxdisk_log_open,
    app_fxdisk_log_write,
    app_fxdisk_log_close,
    app_fxdisk_log_flush,
    app_fxdisk_log_size,
    (int (*)(const char *, acq_chunk_fn, void *, uint32_t *))app_fxdisk_read_stream,
};

static const acq_media_ops_t acq_media_sd = {
    "sd",
    app_fxsd_mounted_ready,
    app_fxsd_lock,
    app_fxsd_unlock,
    app_fxsd_log_open,
    app_fxsd_log_write,
    app_fxsd_log_close,
    app_fxsd_log_flush,
    app_fxsd_log_size,
    (int (*)(const char *, acq_chunk_fn, void *, uint32_t *))app_fxsd_read_stream,
};

static const acq_media_ops_t *acq_media = &acq_media_sim;
static uint32_t acq_file_max_bytes = APP_ACQ_FILE_MAX_BYTES;

static TX_QUEUE acq_queue;
static ULONG acq_queue_mem[ACQ_QUEUE_DEPTH * (sizeof(acq_record_t) / sizeof(ULONG))];
static volatile uint32_t acq_windows_dropped;
static volatile uint32_t acq_flush_batches;
static volatile uint32_t acq_media_errors;
static volatile uint32_t acq_media_gave_up;
static uint32_t acq_last_flush_ms;

typedef struct
{
  char line[ACQ_LINE_MAX];
  uint32_t used;
  uint32_t lines;
  uint32_t samples;
  uint32_t channels;
  uint32_t bad_line;
  int failed;
} acq_parse_t;

static int acq_open_file(void);
static void acq_process_window(void);
static int acq_tim_config(uint32_t hz);
static int acq_dma_setup(void);
static void acq_dma_feed_ring(void);
static int acq_write_record(const acq_record_t *rec);
static void acq_drain_and_close(void);
static void acq_adc_config(void);
static uint32_t acq_window_stats(uint32_t nch, uint32_t *min,
                                 uint32_t *max, uint32_t *sum);

/* Kernel-context init: ThreadX objects cannot be created before tx_kernel_enter. */
void app_acq_system_init(void)
{
  if (tx_queue_create(&acq_queue, "acq-q", sizeof(acq_record_t) / sizeof(ULONG),
                      acq_queue_mem, sizeof(acq_queue_mem)) != TX_SUCCESS)
  {
    Error_Handler();
  }
}

/* (Re)build ADC1 for the configured channel count; one trigger converts the
   whole rank sequence, so the per-channel rate equals the TIM3 rate. */
static void acq_adc_config(void)
{
  ADC_ChannelConfTypeDef channel = {0};

  /* HAL_ADC_DeInit() dereferences Instance, so only call it once the handle
     has been initialised (this function also runs from app_acq_init()). */
  if (hadc1.Instance != NULL)
  {
    (void)HAL_ADC_DeInit(&hadc1);
  }

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = (acq_channels > 1U) ? ADC_SCAN_ENABLE : ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = acq_channels;
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

  /* Rank constants are (SQR1 offset | field offset), not 1..4: they must be
     used verbatim, never incremented. */
  static const uint32_t ranks[APP_ACQ_CH_MAX] = {
      ADC_REGULAR_RANK_1, ADC_REGULAR_RANK_2, ADC_REGULAR_RANK_3, ADC_REGULAR_RANK_4};

  for (uint32_t i = 0U; i < acq_channels; i++)
  {
    channel.Channel = acq_chan_table[i].adc_channel;
    channel.Rank = ranks[i];
    channel.SamplingTime = ADC_SAMPLETIME_24CYCLES_5;
    channel.SingleDiff = ADC_SINGLE_ENDED;
    channel.OffsetNumber = ADC_OFFSET_NONE;
    channel.Offset = 0U;
    if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK)
    {
      Error_Handler();
    }
  }

  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
}

void app_acq_init(void)
{
  if (acq_tim_config(ACQ_RATE_DEFAULT_HZ) != 0)
  {
    Error_Handler();
  }

  acq_adc_config();

  if (acq_dma_setup() != 0)
  {
    Error_Handler();
  }
}

int app_acq_set_channels(uint32_t nch)
{
  if ((nch < 1U) || (nch > APP_ACQ_CH_MAX))
  {
    return -1;
  }
  if (acq_running != 0U)
  {
    return -2;
  }

  acq_channels = nch;
  acq_adc_config();
  return 0;
}

uint32_t app_acq_channels(void)
{
  return acq_channels;
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
    acq_isr_samples++;
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
    acq_isr_samples++;
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
    for (uint32_t i = 0U; i < acq_dma_half; i++)
    {
      acq_ring_push(acq_dma_buf[(half * acq_dma_half) + i]);
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

  rc = acq_media->log_open(acq_current);
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
  if (acq_media->ready() != 0)
  {
    return -2;
  }
  /*
   * One trigger converts the whole rank sequence back-to-back (~0.6 us per
   * channel), which is faster than the EOC interrupt can read DR.  In a scan
   * sequence the ISR therefore only ever sees the last conversion of each
   * trigger (OVR gets set), so multi-channel is DMA-only.
   */
  if ((use_dma == 0) && (acq_channels > 1U))
  {
    return -8;
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
  acq_isr_samples = 0U;
  acq_windows_dropped = 0U;
  acq_flush_batches = 0U;
  acq_media_errors = 0U;
  acq_media_gave_up = 0U;
  acq_last_flush_ms = HAL_GetTick();
  acq_eval_channels = acq_channels;

  if ((hz != 0U) && (hz != acq_rate_hz))
  {
    if (acq_tim_config(hz) != 0)
    {
      return -6;
    }
  }

  {
    /* Drop anything left over from a previous session. */
    acq_record_t stale;
    while (tx_queue_receive(&acq_queue, &stale, TX_NO_WAIT) == TX_SUCCESS)
    {
    }
  }

  if (acq_media->lock() != 0)
  {
    return -3;
  }
  if (acq_open_file() != 0)
  {
    (void)acq_media->unlock();
    return -3;
  }
  (void)acq_media->unlock();

  acq_mode = (use_dma != 0) ? ACQ_MODE_DMA : ACQ_MODE_IRQ;

  if (acq_mode == ACQ_MODE_DMA)
  {
    /* Re-arm the whole chain on every start so an earlier GPDMA experiment
       cannot leave the channel without its security attributes. */
    if (acq_dma_setup() != 0)
    {
      acq_mode = ACQ_MODE_IRQ;
      (void)acq_media->lock();
      (void)acq_media->log_close();
      (void)acq_media->unlock();
      return -7;
    }

    {
      /* Aim for one half per 10 ms thread tick: two halves per wake leaves no
         margin, and a late tick costs a whole half. */
      uint32_t frames = acq_rate_hz / 100U;

      if (frames < ACQ_DMA_FRAMES_MIN)
      {
        frames = ACQ_DMA_FRAMES_MIN;
      }
      if (frames > ACQ_DMA_FRAMES_MAX)
      {
        frames = ACQ_DMA_FRAMES_MAX;
      }
      acq_dma_half = frames * acq_eval_channels;
    }

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)(uintptr_t)acq_dma_buf,
                          2U * acq_dma_half) != HAL_OK)
    {
      acq_mode = ACQ_MODE_IRQ;
      (void)acq_media->lock();
      (void)acq_media->log_close();
      (void)acq_media->unlock();
      return -4;
    }
  }
  else if (HAL_ADC_Start_IT(&hadc1) != HAL_OK)
  {
    (void)acq_media->lock();
    (void)acq_media->log_close();
    (void)acq_media->unlock();
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
    (void)acq_media->lock();
    (void)acq_media->log_close();
    (void)acq_media->unlock();
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

  acq_running = 0U;
  (void)HAL_TIM_Base_Stop(&htim3);
  if (acq_mode == ACQ_MODE_DMA)
  {
    (void)HAL_ADC_Stop_DMA(&hadc1);
  }
  else
  {
    (void)HAL_ADC_Stop_IT(&hadc1);
  }

  /* Let a window that was already being summarised reach the queue. */
  tx_thread_sleep(1);
  acq_drain_and_close();
}

int app_acq_running(void)
{
  return (acq_running != 0U) ? 1 : 0;
}

/* Consumer side: format one window record, append, and rotate when full.
   Line layout: idx,count,nch,min0,max0,avg0[,min1,max1,avg1...]
   Caller must hold the media lock. */
static int acq_write_record(const acq_record_t *rec)
{
  char line[ACQ_LINE_MAX];
  uint32_t size = 0U;
  int len;
  int pos;

  len = snprintf(line, sizeof(line), "%lu,%lu,%lu",
                 (unsigned long)rec->index,
                 (unsigned long)rec->count,
                 (unsigned long)rec->channels);
  pos = (len > 0) ? len : 0;

  for (uint32_t i = 0U; (i < rec->channels) && (pos > 0) &&
                       ((size_t)pos < sizeof(line)); i++)
  {
    len = snprintf(&line[pos], sizeof(line) - (size_t)pos, ",%lu,%lu,%lu",
                   (unsigned long)rec->min[i],
                   (unsigned long)rec->max[i],
                   (unsigned long)rec->avg[i]);
    pos += (len > 0) ? len : 0;
  }

  if ((pos <= 0) || ((size_t)pos >= sizeof(line)))
  {
    return -1;
  }
  len = snprintf(&line[pos], sizeof(line) - (size_t)pos, "\r\n");
  pos += (len > 0) ? len : 0;

  if (acq_media->log_write(line, (uint32_t)pos) != 0)
  {
    acq_media_errors++;
    if (acq_media_errors <= 3U)
    {
      uint32_t op = 0U, code = 0U, lba = 0U;

      app_sdspi_last_error(&op, &code, &lba);
      app_log_printf("ACQ media-fail n=%lu op=%lu code=%02lx lba=%lu\r\n",
                     (unsigned long)acq_media_errors, (unsigned long)op,
                     (unsigned long)code, (unsigned long)lba);
    }
    if (acq_media_errors >= ACQ_MEDIA_ERROR_LIMIT)
    {
      acq_media_gave_up = 1U;
    }
    return -1;
  }
  acq_media_errors = 0U;

  if ((acq_media->log_size(&size) == 0) && (size >= acq_file_max_bytes))
  {
    if (acq_open_file() != 0)
    {
      return -2;
    }
  }
  return 0;
}

/* Pull one window of interleaved frames out of the ring and reduce it per
   channel. Returns the number of frames consumed (0 when the ring holds less
   than a full window). */
static uint32_t acq_window_stats(uint32_t nch, uint32_t *min,
                                 uint32_t *max, uint32_t *sum)
{
  uint32_t need = nch * APP_ACQ_WINDOW_FRAMES;
  uint32_t pending = (acq_ring_head - acq_ring_tail) & (ACQ_RING_SAMPLES - 1U);

  if ((nch == 0U) || (nch > APP_ACQ_CH_MAX) || (pending < need))
  {
    return 0U;
  }

  for (uint32_t i = 0U; i < nch; i++)
  {
    min[i] = 0xFFFFU;
    max[i] = 0U;
    sum[i] = 0U;
  }

  for (uint32_t frame = 0U; frame < APP_ACQ_WINDOW_FRAMES; frame++)
  {
    for (uint32_t ch = 0U; ch < nch; ch++)
    {
      uint32_t value = acq_ring[acq_ring_tail];
      __DMB();
      acq_ring_tail = (acq_ring_tail + 1U) & (ACQ_RING_SAMPLES - 1U);

      if (value < min[ch])
      {
        min[ch] = value;
      }
      if (value > max[ch])
      {
        max[ch] = value;
      }
      sum[ch] += value;
    }
  }

  return APP_ACQ_WINDOW_FRAMES;
}

/* Producer side: statistics only, never touches the file system. */
static void acq_process_window(void)
{
  const uint32_t nch = acq_eval_channels;
  acq_record_t rec;
  uint32_t min[APP_ACQ_CH_MAX];
  uint32_t max[APP_ACQ_CH_MAX];
  uint32_t sum[APP_ACQ_CH_MAX];
  uint32_t frames;
  uint32_t ch;

  frames = acq_window_stats(nch, min, max, sum);
  if (frames == 0U)
  {
    return;
  }

  for (ch = 0U; ch < nch; ch++)
  {
    if (min[ch] < acq_min)
    {
      acq_min = min[ch];
    }
    if (max[ch] > acq_max)
    {
      acq_max = max[ch];
    }
    acq_sum += sum[ch];
  }
  acq_samples_done += frames * nch;

  rec.index = acq_windows_done;
  rec.count = frames;
  rec.channels = nch;
  for (ch = 0U; ch < nch; ch++)
  {
    rec.min[ch] = min[ch];
    rec.max[ch] = max[ch];
    rec.avg[ch] = sum[ch] / frames;
  }

  if (tx_queue_send(&acq_queue, &rec, TX_NO_WAIT) == TX_SUCCESS)
  {
    acq_windows_done++;
  }
  else
  {
    acq_windows_dropped++;
  }
}

/* Write everything still queued, then close; used when a capture stops so the
   console thread can hand the file over in a defined state. */
static void acq_drain_and_close(void)
{
  acq_record_t rec;

  if (acq_media->lock() != 0)
  {
    return;
  }
  while (tx_queue_receive(&acq_queue, &rec, TX_NO_WAIT) == TX_SUCCESS)
  {
    /* Once the medium has failed, retrying every queued window is what made
       `acq stop` hang for minutes; drop them instead. */
    if (acq_media_gave_up != 0U)
    {
      acq_windows_dropped++;
    }
    else
    {
      (void)acq_write_record(&rec);
    }
  }
  (void)acq_media->log_close();
  (void)acq_media->unlock();
}

/* Writer thread: drains the queue in batches and flushes once per batch. */
void app_acq_writer_entry(unsigned long thread_input)
{
  (void)thread_input;

  for (;;)
  {
    app_hb_mark(APP_HB_WRITER);

    acq_record_t rec;
    uint32_t lines = 0U;
    int locked = 0;

    if (acq_media_gave_up != 0U)
    {
      /* Drain without logging so the queue cannot grow unbounded. */
      while (tx_queue_receive(&acq_queue, &rec, TX_NO_WAIT) == TX_SUCCESS)
      {
        acq_windows_dropped++;
      }
      tx_thread_sleep(APP_MS_TO_TICKS(10));
      continue;
    }

    locked = (acq_media->lock() == 0);

    if (locked)
    {
      while (tx_queue_receive(&acq_queue, &rec, TX_NO_WAIT) == TX_SUCCESS)
      {
        if (acq_write_record(&rec) != 0)
        {
          acq_windows_dropped++;
        }
        lines++;
        app_hb_mark(APP_HB_WRITER);
      }
      /*
       * Rate-limit the flush. At 50 kHz a window arrives every 10 ms and the
       * queue normally holds just one, so "flush per batch" degenerated into
       * "flush per record" and the write amplification outpaced the link.
       */
      if ((lines > 0U) && ((HAL_GetTick() - acq_last_flush_ms) >= ACQ_FLUSH_INTERVAL_MS))
      {
        (void)acq_media->log_flush();
        acq_flush_batches++;
        acq_last_flush_ms = HAL_GetTick();
      }
      (void)acq_media->unlock();
    }

    tx_thread_sleep(ACQ_WRITE_PERIOD_TICKS);
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

      /* Drain every complete window: above ~50 kHz several of them pile up
         between two 10 ms ticks. */
      pending = (acq_ring_head - acq_ring_tail) & (ACQ_RING_SAMPLES - 1U);
      while (pending >= (acq_eval_channels * APP_ACQ_WINDOW_FRAMES))
      {
        acq_process_window();
        pending = (acq_ring_head - acq_ring_tail) & (ACQ_RING_SAMPLES - 1U);
      }
    }

    tx_thread_sleep(1);
  }
}

/*
 * Deterministic de-interleave check: fill the ring with N interleaved ramps and
 * run the very path the capture uses.  Channel c must see [1000c, 1000c+99].
 */
int app_acq_demux_selftest(void)
{
  const uint32_t nch = APP_ACQ_CH_MAX;
  uint32_t min[APP_ACQ_CH_MAX];
  uint32_t max[APP_ACQ_CH_MAX];
  uint32_t sum[APP_ACQ_CH_MAX];
  uint32_t frames;
  int failures = 0;

  if (acq_running != 0U)
  {
    app_log_printf("ACQ DEMUX SKIP reason=acq-running\r\n");
    return 1;
  }

  acq_ring_head = 0U;
  acq_ring_tail = 0U;

  for (uint32_t frame = 0U; frame < APP_ACQ_WINDOW_FRAMES; frame++)
  {
    for (uint32_t ch = 0U; ch < nch; ch++)
    {
      acq_ring_push((uint16_t)((1000U * ch) + (frame % 100U)));
    }
  }

  frames = acq_window_stats(nch, min, max, sum);
  if (frames != APP_ACQ_WINDOW_FRAMES)
  {
    app_log_printf("ACQ DEMUX FAIL reason=frames got=%lu\r\n", (unsigned long)frames);
    return 1;
  }

  for (uint32_t ch = 0U; ch < nch; ch++)
  {
    uint32_t expect_min = 1000U * ch;
    uint32_t expect_max = expect_min + 99U;
    /* Five passes over 0..99 -> average 49.5, truncated to 49. */
    uint32_t expect_avg = expect_min + 49U;
    uint32_t got_avg = sum[ch] / frames;
    int ok = (min[ch] == expect_min) && (max[ch] == expect_max) && (got_avg == expect_avg);

    app_log_printf("ACQ DEMUX ch=%lu min=%s max=%s avg=%s (%lu/%lu/%lu)\r\n",
                   (unsigned long)ch, (min[ch] == expect_min) ? "ok" : "bad",
                   (max[ch] == expect_max) ? "ok" : "bad",
                   (got_avg == expect_avg) ? "ok" : "bad",
                   (unsigned long)min[ch], (unsigned long)max[ch],
                   (unsigned long)got_avg);
    if (!ok)
    {
      failures++;
    }
  }

  acq_ring_head = 0U;
  acq_ring_tail = 0U;

  app_log_printf("ACQ DEMUX %s channels=%lu failures=%d\r\n",
                 (failures == 0) ? "PASS" : "FAIL", (unsigned long)nch, failures);
  return failures;
}

void app_acq_report(void)
{
  app_log_printf("ACQ running=%d mode=%s target=%s rate=%lu ch=%lu file=%s windows=%lu samples=%lu files=%lu min=%lu max=%lu drop=%lu dma_ovr=%lu qdrop=%lu flush=%lu media_err=%lu gave_up=%lu\r\n",
                 (int)acq_running,
                 (acq_mode == ACQ_MODE_DMA) ? "dma" : "irq",
                 acq_media->name,
                 (unsigned long)acq_rate_hz,
                 (unsigned long)acq_eval_channels,
                 (acq_current[0] != '\0') ? acq_current : "-",
                 (unsigned long)acq_windows_done,
                 (unsigned long)acq_samples_done,
                 (unsigned long)acq_files_created,
                 (unsigned long)((acq_min == 0xFFFFFFFFU) ? 0U : acq_min),
                 (unsigned long)acq_max,
                 (unsigned long)acq_ring_drops,
                 (unsigned long)acq_dma_overruns,
                 (unsigned long)acq_windows_dropped,
                 (unsigned long)acq_flush_batches,
                 (unsigned long)acq_media_errors,
                 (unsigned long)acq_media_gave_up);

  app_log_printf("ACQ adc isr=%lu sqr1=%08lx cfgr=%08lx isr_reg=%08lx\r\n",
                 (unsigned long)acq_isr_samples,
                 (unsigned long)ADC1->SQR1, (unsigned long)ADC1->CFGR,
                 (unsigned long)ADC1->ISR);

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

/* Split a comma separated line of unsigned numbers. Returns the field count,
   or 0 if a field is empty or not fully numeric. */
static uint32_t acq_split_fields(char *line, unsigned long *out, uint32_t max)
{
  char *cursor = line;
  uint32_t fields = 0U;

  while ((*cursor != '\0') && (*cursor != '\r') && (fields < max))
  {
    char *end = NULL;

    out[fields] = strtoul(cursor, &end, 10);
    if ((end == cursor) || ((*end != ',') && (*end != '\0') && (*end != '\r')))
    {
      return 0U;
    }
    fields++;
    if (*end != ',')
    {
      break;
    }
    cursor = end + 1;
  }

  return fields;
}

/* Validate one CSV line: idx,count,nch,min0,max0,avg0[,min1,max1,avg1...] */
static void acq_feed_line(acq_parse_t *ctx)
{
  unsigned long value[3U + (3U * APP_ACQ_CH_MAX)];
  uint32_t fields;
  unsigned long count;
  unsigned long nch;

  ctx->line[ctx->used] = '\0';
  ctx->used = 0U;

  fields = acq_split_fields(ctx->line, value, (uint32_t)(sizeof(value) / sizeof(value[0])));

  count = (fields > 1U) ? value[1] : 0UL;
  nch = (fields > 2U) ? value[2] : 0UL;

  if ((fields < 6U) || (nch < 1UL) || (nch > APP_ACQ_CH_MAX) ||
      (fields != (3U + (3U * nch))) || (count != APP_ACQ_WINDOW_FRAMES) ||
      ((ctx->channels != 0U) && (nch != ctx->channels)))
  {
    if (ctx->failed == 0)
    {
      ctx->bad_line = ctx->lines + 1U;
      ctx->failed = 1;
    }
    return;
  }

  for (uint32_t ch = 0U; ch < (uint32_t)nch; ch++)
  {
    unsigned long minimum = value[3U + (3U * ch)];
    unsigned long maximum = value[4U + (3U * ch)];
    unsigned long average = value[5U + (3U * ch)];

    if ((minimum > maximum) || (average < minimum) || (average > maximum) ||
        (maximum > 4095UL))
    {
      if (ctx->failed == 0)
      {
        ctx->bad_line = ctx->lines + 1U;
        ctx->failed = 1;
      }
      return;
    }
  }

  ctx->channels = (uint32_t)nch;
  ctx->samples += (uint32_t)(count * nch);
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
  int result;

  memset(&ctx, 0, sizeof(ctx));

  if (acq_media->lock() != 0)
  {
    return -9;
  }

  if (acq_current[0] == '\0')
  {
    app_log_printf("ACQ CHECK FAIL reason=no-file\r\n");
    (void)acq_media->unlock();
    return -1;
  }

  /* Flush first: the writer batches, so the tail of the file may still be in
     the media cache. */
  (void)acq_media->log_flush();

  rc = acq_media->read_stream(acq_current, (acq_chunk_fn)acq_parse_chunk, &ctx, &total);
  if ((ctx.used > 0U) && (rc == 0))
  {
    acq_feed_line(&ctx);
  }

  if ((rc != 0) || (ctx.failed != 0) || (ctx.lines == 0U))
  {
    app_log_printf("ACQ CHECK FAIL file=%s rc=%d line=%lu lines=%lu bytes=%lu\r\n",
                   acq_current, rc, (unsigned long)ctx.bad_line,
                   (unsigned long)ctx.lines, (unsigned long)total);
    result = -2;
  }
  else
  {
    app_log_printf("ACQ CHECK PASS file=%s lines=%lu channels=%lu samples=%lu bytes=%lu\r\n",
                   acq_current, (unsigned long)ctx.lines, (unsigned long)ctx.channels,
                   (unsigned long)ctx.samples, (unsigned long)total);
    result = 0;
  }

  (void)acq_media->unlock();
  return result;
}


/* HAL channel handles for the plain 0..19 indices, so a diagnostic can address
   a channel by number instead of by macro name. */
static const uint32_t acq_adc_channel_num[20] = {
    ADC_CHANNEL_0,  ADC_CHANNEL_1,  ADC_CHANNEL_2,  ADC_CHANNEL_3,
    ADC_CHANNEL_4,  ADC_CHANNEL_5,  ADC_CHANNEL_6,  ADC_CHANNEL_7,
    ADC_CHANNEL_8,  ADC_CHANNEL_9,  ADC_CHANNEL_10, ADC_CHANNEL_11,
    ADC_CHANNEL_12, ADC_CHANNEL_13, ADC_CHANNEL_14, ADC_CHANNEL_15,
    ADC_CHANNEL_16, ADC_CHANNEL_17, ADC_CHANNEL_18, ADC_CHANNEL_19};

/*
 * Software-triggered single conversion.  The whole ADC1 configuration is
 * rebuilt, so the regular triggered sequence is restored before returning;
 * that makes this usable from the console between captures.
 */
int app_acq_one_shot(uint32_t adc_channel, uint32_t samples, uint32_t *raw)
{
  ADC_ChannelConfTypeDef channel = {0};
  uint32_t ccr_saved;
  uint32_t sum = 0U;
  uint32_t got = 0U;

  if (raw == NULL)
  {
    return -1;
  }
  if (acq_running != 0U)
  {
    return -2;
  }
  if (samples == 0U)
  {
    samples = 1U;
  }
  *raw = 0U;

  if (hadc1.Instance != NULL)
  {
    (void)HAL_ADC_DeInit(&hadc1);
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
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.SamplingMode = ADC_SAMPLING_MODE_NORMAL;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    acq_adc_config();
    return -3;
  }

  /* Internal channels need their analog buffer switched on explicitly, and
     leaving an unused buffer on would bias the external channels. */
  ccr_saved = ADC12_COMMON->CCR;
  {
    uint32_t add = 0U;

    /* The channel number lives in the AWD1CH bit positions, not in the low
       bits, so it has to be shifted out before comparing. */
    switch ((adc_channel & ADC_CHANNEL_ID_NUMBER_MASK) >>
            ADC_CHANNEL_ID_NUMBER_BITOFFSET_POS)
    {
      case 16U: add = ADC_CCR_TSEN; break;
      case 17U: add = ADC_CCR_VREFEN; break;
      default: break;
    }
    if ((adc_channel & ADC_CHANNEL_ID_INTERNAL_CH_2) != 0U)
    {
      add |= ADC_CCR_VBATEN;
    }
    ADC12_COMMON->CCR = ccr_saved | add;
  }

  channel.Channel = adc_channel;
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  channel.SingleDiff = ADC_SINGLE_ENDED;
  channel.OffsetNumber = ADC_OFFSET_NONE;
  channel.Offset = 0U;
  if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK)
  {
    ADC12_COMMON->CCR = ccr_saved;
    acq_adc_config();
    return -4;
  }

  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    ADC12_COMMON->CCR = ccr_saved;
    acq_adc_config();
    return -5;
  }

  for (uint32_t i = 0U; i < samples; i++)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      break;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 20U) == HAL_OK)
    {
      sum += HAL_ADC_GetValue(&hadc1);
      got++;
    }
    (void)HAL_ADC_Stop(&hadc1);
  }

  if (got != 0U)
  {
    *raw = sum / got;
  }
  ADC12_COMMON->CCR = ccr_saved;
  acq_adc_config();
  return (got != 0U) ? 0 : -6;
}

int app_acq_sample_index(uint32_t adc_channel_number, uint32_t samples,
                         uint32_t *raw)
{
  if (adc_channel_number >= 20U)
  {
    return -7;
  }
  return app_acq_one_shot(acq_adc_channel_num[adc_channel_number], samples, raw);
}

/* 12-bit codes as a band: rails are <=5 % and >=95 % of full scale. */
static const char *acq_level_band(uint32_t raw)
{
  if (raw <= 205U)
  {
    return "low";
  }
  if (raw >= 3890U)
  {
    return "high";
  }
  return "mid";
}

static uint32_t acq_level_mv(uint32_t raw, uint32_t vdd_mv)
{
  if (vdd_mv == 0U)
  {
    return 0U;
  }
  return (uint32_t)(((uint64_t)raw * (uint64_t)vdd_mv) / 4095ULL);
}

void app_acq_level_report(uint32_t adc_channel_number, int have_channel)
{
  uint32_t vdd_mv = 0U;

  if (have_channel != 0)
  {
    uint32_t raw = 0U;
    int rc = app_acq_sample_index(adc_channel_number, 8U, &raw);

    if (rc == 0)
    {
      app_log_printf("ACQ LEVEL adc=%lu raw=%lu band=%s\r\n",
                     (unsigned long)adc_channel_number, (unsigned long)raw,
                     acq_level_band(raw));
    }
    else
    {
      app_log_printf("ACQ LEVEL adc=%lu rc=%d\r\n",
                     (unsigned long)adc_channel_number, rc);
    }
    return;
  }

  /* Sample the pins first: an internal channel leaves its voltage on the
     sample-and-hold capacitor, which biases whatever follows. */
  for (uint32_t i = 0U; i < acq_channels; i++)
  {
    uint32_t raw = 0U;
    int rc = app_acq_one_shot(acq_chan_table[i].adc_channel, 8U, &raw);

    if (rc != 0)
    {
      app_log_printf("ACQ LEVEL ch=%lu pad=%s rc=%d\r\n",
                     (unsigned long)i, acq_chan_table[i].name, rc);
      continue;
    }
    app_log_printf("ACQ LEVEL ch=%lu adc=%lu pad=%s raw=%lu band=%s\r\n",
                   (unsigned long)i,
                   (unsigned long)((acq_chan_table[i].adc_channel &
                                    ADC_CHANNEL_ID_NUMBER_MASK) >>
                                   ADC_CHANNEL_ID_NUMBER_BITOFFSET_POS),
                   acq_chan_table[i].name, (unsigned long)raw,
                   acq_level_band(raw));
  }

  {
    uint32_t vref = 0U;
    int rc = app_acq_one_shot(ADC_CHANNEL_VREFINT, 8U, &vref);

    if ((rc == 0) && (vref != 0U))
    {
      /* VREFINT is 1.21 V nominal on this family. */
      vdd_mv = (uint32_t)((1210ULL * 4095ULL) / (uint64_t)vref);
      app_log_printf("ACQ LEVEL vrefint raw=%lu vdd_mv=%lu\r\n",
                     (unsigned long)vref, (unsigned long)vdd_mv);
    }
    else
    {
      app_log_printf("ACQ LEVEL vrefint rc=%d\r\n", rc);
    }

    /* The internal channels are the ones that do not depend on the pads, so
       they tell a dead analog front end apart from a wiring problem. */
    {
      uint32_t ts = 0U;
      uint32_t vbat = 0U;

      rc = app_acq_one_shot(ADC_CHANNEL_TEMPSENSOR, 8U, &ts);
      if (rc == 0)
      {
        app_log_printf("ACQ LEVEL tempsensor raw=%lu mv=%lu\r\n",
                       (unsigned long)ts,
                       (unsigned long)acq_level_mv(ts, vdd_mv));
      }
      else
      {
        app_log_printf("ACQ LEVEL tempsensor rc=%d\r\n", rc);
      }

      rc = app_acq_one_shot(ADC_CHANNEL_VBAT, 8U, &vbat);
      if (rc == 0)
      {
        /* The VBAT channel sees VBAT through a 1/4 divider ladder. */
        app_log_printf("ACQ LEVEL vbat raw=%lu mv=%lu\r\n",
                       (unsigned long)vbat,
                       (unsigned long)(acq_level_mv(vbat, vdd_mv) * 4ULL));
      }
      else
      {
        app_log_printf("ACQ LEVEL vbat rc=%d\r\n", rc);
      }
    }
  }

  if (vdd_mv != 0U)
  {
    for (uint32_t i = 0U; i < acq_channels; i++)
    {
      uint32_t raw = 0U;

      if (app_acq_one_shot(acq_chan_table[i].adc_channel, 8U, &raw) != 0)
      {
        continue;
      }
      app_log_printf("ACQ LEVEL ch=%lu pad=%s mv=%lu\r\n",
                     (unsigned long)i, acq_chan_table[i].name,
                     (unsigned long)acq_level_mv(raw, vdd_mv));
    }
  }
}

/*
 * Diagnostic: drive DAC1 channel 1 (PA4) and read it back on ADC1_INP18, which
 * is the same pad.  The DAC and the ADC are both internal, so this shows
 * whether an external channel can see a pad that the chip itself drives.
 */
int app_acq_dac_loopback(uint32_t code, uint32_t *raw)
{
  static DAC_HandleTypeDef hdac1;
  DAC_ChannelConfTypeDef cfg = {0};
  int rc;

  if ((raw == NULL) || (code > 4095U))
  {
    return -1;
  }
  if (acq_running != 0U)
  {
    return -2;
  }

  __HAL_RCC_DAC1_CLK_ENABLE();

  hdac1.Instance = DAC1;
  if (HAL_DAC_Init(&hdac1) != HAL_OK)
  {
    return -3;
  }

  cfg.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_DISABLE;
  cfg.DAC_DMADoubleDataMode = DISABLE;
  cfg.DAC_SignedFormat = DISABLE;
  cfg.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
  cfg.DAC_Trigger = DAC_TRIGGER_NONE;
  cfg.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  cfg.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
  cfg.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
  cfg.DAC_TrimmingValue = 0U;
  if (HAL_DAC_ConfigChannel(&hdac1, &cfg, DAC_CHANNEL_1) != HAL_OK)
  {
    (void)HAL_DAC_DeInit(&hdac1);
    return -4;
  }
  if (HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, code) != HAL_OK)
  {
    (void)HAL_DAC_DeInit(&hdac1);
    return -5;
  }
  if (HAL_DAC_Start(&hdac1, DAC_CHANNEL_1) != HAL_OK)
  {
    (void)HAL_DAC_DeInit(&hdac1);
    return -6;
  }

  tx_thread_sleep(APP_MS_TO_TICKS(2));
  rc = app_acq_one_shot(ADC_CHANNEL_18, 8U, raw);

  (void)HAL_DAC_Stop(&hdac1, DAC_CHANNEL_1);
  (void)HAL_DAC_DeInit(&hdac1);
  __HAL_RCC_DAC1_CLK_DISABLE();
  return rc;
}

/* Pick the medium the window writer logs to. Refused while a capture runs. */
int app_acq_set_target(int target)
{
  if (acq_running != 0U)
  {
    return -2;
  }

  if (target == APP_ACQ_TARGET_SD)
  {
    int rc;

    if (app_fxsd_lock() != 0)
    {
      return -1;
    }
    rc = app_fxsd_mount();
    (void)app_fxsd_unlock();

    if (rc != 0)
    {
      return rc;
    }
    acq_media = &acq_media_sd;
    /* A card has room for a lot more than the simulated disk, and rotating
       every 4 KB would cost a file create per second at high rates. */
    acq_file_max_bytes = APP_ACQ_SD_FILE_MAX_BYTES;
  }
  else
  {
    acq_media = &acq_media_sim;
    acq_file_max_bytes = APP_ACQ_FILE_MAX_BYTES;
  }
  return 0;
}

const char *app_acq_target_name(void)
{
  return acq_media->name;
}

/* 1 once the writer stopped touching a failing medium. */
int app_acq_media_gave_up(void)
{
  return (acq_media_gave_up != 0U) ? 1 : 0;
}
