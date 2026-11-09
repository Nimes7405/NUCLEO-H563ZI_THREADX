#include "app_console.h"
#include "app_config.h"
#include "app_control.h"
#include "app_fault.h"
#include "app_log.h"
#include "app_param.h"
#include "app_stacks.h"
#include "app_stacks.h"
#include "app_watchdog.h"

#include <stdlib.h>
#include <string.h>

UART_HandleTypeDef huart3;

static uint8_t rx_byte;
static volatile uint8_t rx_ring[APP_CON_RX_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;
static volatile uint32_t rx_dropped;

static char line_buffer[APP_CON_LINE_SIZE];
static uint32_t line_len;

static void console_dispatch(char *line);
static void console_fault_report(void);
static void console_help(void);
static void console_cal(const char *arg);
#if !APP_RELEASE
static void console_stall(const char *arg);
#endif

void app_console_init(void)
{
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK)
  {
    Error_Handler();
  }

  app_log_set_uart_ready();

  HAL_NVIC_SetPriority(USART3_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(USART3_IRQn);
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance != USART3)
  {
    return;
  }

  uint32_t next = (rx_head + 1U) & (APP_CON_RX_SIZE - 1U);
  if (next != rx_tail)
  {
    rx_ring[rx_head] = rx_byte;
    __DMB();
    rx_head = next;
  }
  else
  {
    rx_dropped++;
  }

  (void)HAL_UART_Receive_IT(&huart3, &rx_byte, 1U);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART3)
  {
    (void)HAL_UART_Receive_IT(&huart3, &rx_byte, 1U);
  }
}

void app_console_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  app_log_printf("READY tx-control-node %s profile=%s\r\n", APP_VERSION, APP_PROFILE_NAME);

  for (;;)
  {
    app_hb_mark(APP_HB_CONSOLE);

    while (rx_tail != rx_head)
    {
      char c = (char)rx_ring[rx_tail];
      __DMB();
      rx_tail = (rx_tail + 1U) & (APP_CON_RX_SIZE - 1U);

      if ((c == '\r') || (c == '\n'))
      {
        if (line_len > 0U)
        {
          line_buffer[line_len] = '\0';
          console_dispatch(line_buffer);
          line_len = 0U;
        }
      }
      else if ((c == '\b') || (c == 0x7F))
      {
        if (line_len > 0U)
        {
          line_len--;
        }
      }
      else if (line_len < (APP_CON_LINE_SIZE - 1U))
      {
        line_buffer[line_len++] = c;
      }
      else
      {
        line_len = 0U;
        app_log_printf("ERR line-too-long\r\n");
      }
    }

    tx_thread_sleep(1);
  }
}

void app_console_status(void)
{
  uint32_t uptime_ms = (uint32_t)((uint64_t)tx_time_get() * 10U);

  app_log_printf("STATUS ver=%s uptime_ms=%lu hal_tick=%lu reset=%s safe=%d wdg_feed=%lu hb=%lu,%lu,%lu,%lu,%lu,%lu drop=%lu rx_drop=%lu profile=%s\r\n",
                 APP_VERSION,
                 (unsigned long)uptime_ms,
                 (unsigned long)HAL_GetTick(),
                 app_boot_reset_name(),
                 app_fault_safe_mode(),
                 (unsigned long)app_watchdog_feed_count(),
                 (unsigned long)app_hb_get(APP_HB_MONITOR),
                 (unsigned long)app_hb_get(APP_HB_LOGGER),
                 (unsigned long)app_hb_get(APP_HB_CONSOLE),
                 (unsigned long)app_hb_get(APP_HB_PROD_A),
                 (unsigned long)app_hb_get(APP_HB_PROD_B),
                 (unsigned long)app_hb_get(APP_HB_CONTROL),
                 (unsigned long)app_log_dropped(),
                 (unsigned long)rx_dropped,
                 APP_PROFILE_NAME);
}

static void console_help(void)
{
#if APP_RELEASE
  app_log_printf("HELP commands: status stack fault [clear] wdg log <text> set <0..4095> plant <sim|adc> ctrl <on|off> gain <kp> <ki> cal [show|set <offset> <scale>|reset|erase] reboot\r\n");
#else
  app_log_printf("HELP commands: status stack fault [clear] wdg log <text> set <0..4095> plant <sim|adc> ctrl <on|off> gain <kp> <ki> cal [show|set <offset> <scale>|reset|erase] reboot guard stall <0|1|2|3|4> crash\r\n");
#endif
}

static void console_fault_report(void)
{
  app_fault_record_t record;

  if (app_fault_get_last(&record) == 0)
  {
    app_log_printf("FAULT none\r\n");
    return;
  }

  app_log_printf("FAULT seq=%lu cfsr=0x%08lx hfsr=0x%08lx mmfar=0x%08lx bfar=0x%08lx\r\n",
                 (unsigned long)record.sequence,
                 (unsigned long)record.cfsr,
                 (unsigned long)record.hfsr,
                 (unsigned long)record.mmfar,
                 (unsigned long)record.bfar);
  app_log_printf("FAULT pc=0x%08lx lr=0x%08lx xpsr=0x%08lx sp=0x%08lx exc=0x%08lx\r\n",
                 (unsigned long)record.pc,
                 (unsigned long)record.lr,
                 (unsigned long)record.xpsr,
                 (unsigned long)record.sp,
                 (unsigned long)record.exc_return);
  if (record.guard_id < (uint32_t)APP_STK_COUNT)
  {
    app_log_printf("FAULT guard=%s safe=%d\r\n",
                   app_stacks_name((app_stack_id_t)record.guard_id),
                   app_fault_safe_mode());
  }
  else
  {
    app_log_printf("FAULT guard=none safe=%d\r\n", app_fault_safe_mode());
  }
}

#if !APP_RELEASE
static void console_stall(const char *arg)
{
  unsigned long n = strtoul(arg, NULL, 10);

  if (n == 0UL)
  {
    for (uint32_t i = 0U; i < (uint32_t)APP_HB_COUNT; i++)
    {
      app_hb_set_stall((app_hb_id_t)i, 0);
    }
    app_watchdog_set_wwdg_stall(0);
    app_log_printf("STALL cleared\r\n");
  }
  else if (n == 1UL)
  {
    app_hb_set_stall(APP_HB_PROD_A, 1);
    app_log_printf("STALL prod_a simulated; IWDG should reset in ~4 s\r\n");
  }
  else if (n == 2UL)
  {
    app_hb_set_stall(APP_HB_PROD_B, 1);
    app_log_printf("STALL prod_b simulated; IWDG should reset in ~4 s\r\n");
  }
  else if (n == 3UL)
  {
    app_hb_set_stall(APP_HB_CONTROL, 1);
    app_log_printf("STALL control simulated; IWDG should reset in ~4 s\r\n");
  }
  else if (n == 4UL)
  {
    app_watchdog_set_wwdg_stall(1);
    app_log_printf("STALL wwdg simulated; WWDG should reset in ~134 ms\r\n");
  }
  else
  {
    app_log_printf("ERR stall index must be 0, 1, 2, 3 or 4\r\n");
  }
}
#endif

static void console_set(const char *arg)
{
  long value = strtol(arg, NULL, 10);
  (void)app_control_set_setpoint((int32_t)value);
  app_log_printf("SET sp=%ld\r\n", (long)app_control_get_setpoint());
}

static void console_plant(const char *arg)
{
  while (*arg == ' ')
  {
    arg++;
  }
  if (strncmp(arg, "sim", 3) == 0)
  {
    app_control_set_plant(APP_PLANT_SIM);
    app_log_printf("PLANT mode=sim\r\n");
  }
  else if (strncmp(arg, "adc", 3) == 0)
  {
    app_control_set_plant(APP_PLANT_ADC);
    app_log_printf("PLANT mode=adc\r\n");
  }
  else
  {
    app_log_printf("ERR plant must be sim or adc\r\n");
  }
}

static void console_ctrl(const char *arg)
{
  while (*arg == ' ')
  {
    arg++;
  }
  if (strncmp(arg, "on", 2) == 0)
  {
    (void)app_control_set_enabled(1);
    app_log_printf("CTRL on=1\r\n");
  }
  else if (strncmp(arg, "off", 3) == 0)
  {
    (void)app_control_set_enabled(0);
    app_log_printf("CTRL on=0\r\n");
  }
  else
  {
    app_log_printf("ERR ctrl must be on or off\r\n");
  }
}

static void console_gain(const char *arg)
{
  char *end = NULL;
  float kp = strtof(arg, &end);
  float ki = (end != NULL) ? strtof(end, NULL) : 0.0f;
  app_control_set_gains(kp, ki);
  uint32_t kp100 = (uint32_t)(kp * 100.0f);
  uint32_t ki1000 = (uint32_t)(ki * 1000.0f);
  app_log_printf("GAIN kp=%lu.%02lu ki=%lu.%03lu\r\n",
                 (unsigned long)(kp100 / 100U), (unsigned long)(kp100 % 100U),
                 (unsigned long)(ki1000 / 1000U), (unsigned long)(ki1000 % 1000U));
}

static void console_cal_show(void)
{
  float offset = 0.0f;
  float scale = 1.0f;
  app_control_get_calibration(&offset, &scale);

  long off100 = (long)(offset * 100.0f);
  long off_whole = off100 / 100;
  long off_frac = off100 % 100;
  if (off_frac < 0)
  {
    off_frac = -off_frac;
  }
  uint32_t scale1000 = (uint32_t)(scale * 1000.0f + 0.5f);
  app_log_printf("CAL offset=%ld.%02ld scale=%lu.%03lu\r\n",
                 off_whole,
                 off_frac,
                 (unsigned long)(scale1000 / 1000U),
                 (unsigned long)(scale1000 % 1000U));

  app_param_info_t info;
  app_param_info(&info);
  app_log_printf("CAL stored=%lu/%lu slot=%lu erases=%lu err=%d\r\n",
                 (unsigned long)info.stored,
                 (unsigned long)info.slot_count,
                 (unsigned long)info.next_slot,
                 (unsigned long)info.erases,
                 info.flash_error);
}

static void console_cal_persist(float offset, float scale)
{
  if (app_param_save(offset, scale) != 0)
  {
    app_log_printf("CAL save failed; value kept in RAM only\r\n");
  }
  /* A flash erase blocks instruction fetch for ~2 ms, which is not a control
     health problem, so restart the jitter/overrun statistics window. */
  app_control_clear_stats();
  app_log_printf("CTRL stats reset after flash op\r\n");
}

static void console_cal(const char *arg)
{
  while (*arg == ' ')
  {
    arg++;
  }

  if ((*arg == '\0') || (strncmp(arg, "show", 4) == 0))
  {
    console_cal_show();
  }
  else if (strncmp(arg, "reset", 5) == 0)
  {
    app_control_reset_calibration();
    console_cal_persist(0.0f, 1.0f);
    console_cal_show();
  }
  else if (strncmp(arg, "erase", 5) == 0)
  {
    if (app_param_erase() == 0)
    {
      app_log_printf("CAL page erased\r\n");
    }
    app_control_reset_calibration();
    app_control_clear_stats();
    app_log_printf("CTRL stats reset after flash op\r\n");
    console_cal_show();
  }
  else if (strncmp(arg, "set", 3) == 0)
  {
    char *end = NULL;
    float offset = strtof(arg + 3, &end);
    if (end == arg + 3)
    {
      app_log_printf("ERR cal set needs <offset> <scale>\r\n");
      return;
    }
    float scale = strtof(end, &end);
    if ((end == NULL) || (scale <= 0.0f))
    {
      app_log_printf("ERR cal set needs <offset> <scale>\r\n");
      return;
    }
    app_control_set_calibration(offset, scale);
    console_cal_persist(offset, scale);
    console_cal_show();
  }
  else
  {
    app_log_printf("ERR cal must be show, set <offset> <scale> or reset\r\n");
  }
}

static void console_dispatch(char *line)
{
  while (*line == ' ')
  {
    line++;
  }
  if (*line == '\0')
  {
    return;
  }

  if (strcmp(line, "help") == 0)
  {
    console_help();
  }
  else if (strcmp(line, "status") == 0)
  {
    app_console_status();
  }
  else if (strcmp(line, "stack") == 0)
  {
    app_stacks_report();
  }
  else if (strncmp(line, "fault", 5) == 0)
  {
    const char *arg = line + 5;
    while (*arg == ' ')
    {
      arg++;
    }
    if (strncmp(arg, "clear", 5) == 0)
    {
      app_fault_clear();
    }
    else
    {
      console_fault_report();
    }
  }
  else if (strcmp(line, "wdg") == 0)
  {
    app_log_printf("WDG timeout_ms=%lu feed_ms=%lu feeds=%lu wwdg=on\r\n",
                   (unsigned long)APP_WDG_TIMEOUT_MS,
                   (unsigned long)APP_WDG_FEED_MS,
                   (unsigned long)app_watchdog_feed_count());
  }
  else if (strncmp(line, "set ", 4) == 0)
  {
    console_set(line + 4);
  }
  else if (strncmp(line, "plant", 5) == 0)
  {
    console_plant(line + 5);
  }
  else if (strncmp(line, "ctrl", 4) == 0)
  {
    console_ctrl(line + 4);
  }
  else if (strncmp(line, "gain", 4) == 0)
  {
    console_gain(line + 4);
  }
  else if (strncmp(line, "cal", 3) == 0)
  {
    console_cal(line + 3);
  }
  else if (strncmp(line, "log ", 4) == 0)
  {
    app_log_printf("LOG %s\r\n", line + 4);
  }
#if !APP_RELEASE
  else if (strcmp(line, "guard") == 0)
  {
    uint32_t address = app_stacks_guard_addr(APP_STK_PROD_A);
    if (address == 0U)
    {
      app_log_printf("ERR guard not configured\r\n");
    }
    else
    {
      app_log_printf("GUARD triggering write to 0x%08lx\r\n", (unsigned long)address);
      tx_thread_sleep(20);
      *(volatile uint32_t *)address = 0xDEADBEEFUL;
    }
  }
  else if (strncmp(line, "stall", 5) == 0)
  {
    console_stall(line + 5);
  }
  else if (strcmp(line, "crash") == 0)
  {
    app_log_printf("CRASH triggering bus fault\r\n");
    tx_thread_sleep(20);
    *(volatile uint32_t *)0xFFFFFFF0UL = 0xDEADBEEFUL;
  }
#endif
  else if (strcmp(line, "reboot") == 0)
  {
    app_log_printf("REBOOT requested\r\n");
    tx_thread_sleep(20);
    NVIC_SystemReset();
  }
  else
  {
    app_log_printf("ERR unknown-cmd=%s len=%lu\r\n",
                   line, (unsigned long)strlen(line));
  }
}
