#include "app_watchdog.h"
#include "app_log.h"
#include "app_stacks.h"

IWDG_HandleTypeDef hiwdg;

#define APP_WWDG_FIRST_FEED_MS 60U

static WWDG_HandleTypeDef hwwdg;
static ULONG wwdg_start_ticks;
static volatile uint8_t wwdg_sim_stall;
static volatile uint8_t wwdg_running;

static const char *const hb_names[APP_HB_COUNT] = {
    "monitor", "logger", "console", "prod_a", "prod_b", "control"};

static volatile uint32_t hb_count[APP_HB_COUNT];
static volatile uint8_t hb_sim_stall[APP_HB_COUNT];
static uint32_t hb_last[APP_HB_COUNT];
static uint32_t hb_stall_ms[APP_HB_COUNT];
static uint32_t feed_count;
static uint32_t feed_acc_ms;
static int stall_reported;

void app_hb_mark(app_hb_id_t id)
{
  if ((id >= APP_HB_COUNT) || (hb_sim_stall[id] != 0U))
  {
    return;
  }
  hb_count[id]++;
}

uint32_t app_hb_get(app_hb_id_t id)
{
  if (id >= APP_HB_COUNT)
  {
    return 0U;
  }
  return hb_count[id];
}

const char *app_hb_name(app_hb_id_t id)
{
  if (id >= APP_HB_COUNT)
  {
    return "?";
  }
  return hb_names[id];
}

void app_hb_set_stall(app_hb_id_t id, int on)
{
  if (id >= APP_HB_COUNT)
  {
    return;
  }
  hb_sim_stall[id] = (on != 0) ? 1U : 0U;
}

void app_watchdog_set_wwdg_stall(int on)
{
  wwdg_sim_stall = (on != 0) ? 1U : 0U;
}

void app_watchdog_init(void)
{
  __HAL_RCC_LSI_ENABLE();
  while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == 0U)
  {
  }

  /* Keep the watchdog running while the core is halted by a debugger. */
  DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;

  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
  hiwdg.Init.Reload = 3999U; /* 32 * 4000 / 32000 Hz = 4.0 s */
  hiwdg.Init.Window = IWDG_WINDOW_DISABLE;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }

  wwdg_sim_stall = 0U;
  wwdg_running = 0U;

  for (uint32_t i = 0U; i < (uint32_t)APP_HB_COUNT; i++)
  {
    hb_last[i] = hb_count[i];
    hb_stall_ms[i] = 0U;
  }
  feed_count = 0U;
  feed_acc_ms = 0U;
  stall_reported = 0;
}

void app_watchdog_wwdg_start(void)
{
  DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_WWDG_STOP;
  __HAL_RCC_WWDG_CLK_ENABLE();
  hwwdg.Instance = WWDG;
  hwwdg.Init.Prescaler = WWDG_PRESCALER_128;
  hwwdg.Init.Window = 0x60U;
  hwwdg.Init.Counter = 0x7FU;
  hwwdg.Init.EWIMode = WWDG_EWI_DISABLE;
  if (HAL_WWDG_Init(&hwwdg) != HAL_OK)
  {
    Error_Handler();
  }
  wwdg_start_ticks = tx_time_get();
  wwdg_running = 1U;
}

int app_watchdog_poll(void)
{
  int stalled = 0;

  ULONG now_ticks = tx_time_get();
  if ((wwdg_running != 0U) && (wwdg_sim_stall == 0U) &&
      ((now_ticks - wwdg_start_ticks) >= APP_MS_TO_TICKS(APP_WWDG_FIRST_FEED_MS)))
  {
    (void)HAL_WWDG_Refresh(&hwwdg);
  }

  for (uint32_t i = 1U; i < (uint32_t)APP_HB_COUNT; i++)
  {
    if (hb_count[i] != hb_last[i])
    {
      hb_last[i] = hb_count[i];
      hb_stall_ms[i] = 0U;
    }
    else if (hb_stall_ms[i] < APP_HB_STALL_MS)
    {
      hb_stall_ms[i] += APP_MONITOR_PERIOD_MS;
    }

    if (hb_stall_ms[i] >= APP_HB_STALL_MS)
    {
      stalled = 1;
    }
  }

  if (app_stacks_any_overflow() != 0)
  {
    stalled = 1;
  }

  if (stalled != 0)
  {
    if (stall_reported == 0)
    {
      for (uint32_t i = 1U; i < (uint32_t)APP_HB_COUNT; i++)
      {
        if (hb_stall_ms[i] >= APP_HB_STALL_MS)
        {
          app_log_printf("WATCHDOG stall=%s no-heartbeat=%lums; stop feeding IWDG\r\n",
                         hb_names[i], (unsigned long)hb_stall_ms[i]);
        }
      }
      int overflow_id = app_stacks_overflow_id();
      if (overflow_id >= 0)
      {
        app_log_printf("WATCHDOG stack-canary-overwritten name=%s first=0x%02x; stop feeding IWDG\r\n",
                       hb_names[overflow_id],
                       app_stacks_first_byte((app_stack_id_t)overflow_id));
      }
      HAL_GPIO_WritePin(APP_LED_RED_GPIO_Port, APP_LED_RED_Pin, GPIO_PIN_SET);
      stall_reported = 1;
    }
    return 0;
  }

  feed_acc_ms += APP_MONITOR_PERIOD_MS;
  if (feed_acc_ms >= APP_WDG_FEED_MS)
  {
    feed_acc_ms = 0U;
    if (HAL_IWDG_Refresh(&hiwdg) == HAL_OK)
    {
      feed_count++;
    }
  }
  return 1;
}

uint32_t app_watchdog_feed_count(void)
{
  return feed_count;
}
