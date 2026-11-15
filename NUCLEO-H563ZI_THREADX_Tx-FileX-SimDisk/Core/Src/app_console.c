#include "app_console.h"
#include "app_acq.h"
#include "app_config.h"
#include "app_dma.h"
#include "app_fault.h"
#include "app_fxdisk.h"
#include "app_log.h"
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
static void console_acq(const char *arg);
static void console_dma(const char *arg);
static void console_fs(const char *arg);
static void console_stall(const char *arg);

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

  app_fxdisk_init();

  app_log_printf("READY tx-filex-simdisk %s\r\n", APP_VERSION);

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

  app_log_printf("STATUS ver=%s uptime_ms=%lu reset=%s wdg_feed=%lu hb=%lu,%lu,%lu,%lu,%lu,%lu drop=%lu rx_drop=%lu\r\n",
                 APP_VERSION,
                 (unsigned long)uptime_ms,
                 app_boot_reset_name(),
                 (unsigned long)app_watchdog_feed_count(),
                 (unsigned long)app_hb_get(APP_HB_MONITOR),
                 (unsigned long)app_hb_get(APP_HB_LOGGER),
                 (unsigned long)app_hb_get(APP_HB_CONSOLE),
                 (unsigned long)app_hb_get(APP_HB_PROD_A),
                 (unsigned long)app_hb_get(APP_HB_PROD_B),
                 (unsigned long)app_hb_get(APP_HB_SAMPLER),
                 (unsigned long)app_log_dropped(),
                 (unsigned long)rx_dropped);
}

static void console_help(void)
{
  app_log_printf("HELP commands: status stack fault wdg log <text> fs <info|ls|format|mount|umount|write|append|read|rm|test|fill|stress> acq <info|start [dma|irq] [hz]|stop|check|selftest> dma <env|test> stall <0|1|2> crash reboot\r\n");
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
}

static void console_acq(const char *arg)
{
  while (*arg == ' ')
  {
    arg++;
  }

  if ((*arg == '\0') || (strcmp(arg, "info") == 0))
  {
    app_acq_report();
    return;
  }

  if (strncmp(arg, "start", 5) == 0)
  {
    const char *opt = arg + 5;
    int use_dma = 0;
    uint32_t hz = 0U;
    int rc;

    while (*opt == ' ')
    {
      opt++;
    }
    if (strncmp(opt, "dma", 3) == 0)
    {
      use_dma = 1;
      opt += 3;
    }
    else if (strncmp(opt, "irq", 3) == 0)
    {
      opt += 3;
    }
    while (*opt == ' ')
    {
      opt++;
    }
    if (*opt != '\0')
    {
      hz = (uint32_t)strtoul(opt, NULL, 10);
    }

    rc = app_acq_start(use_dma, hz);
    app_log_printf("ACQ start=%s mode=%s rc=%d\r\n", (rc == 0) ? "ok" : "fail",
                   (use_dma != 0) ? "dma" : "irq", rc);
    return;
  }

  if (strcmp(arg, "stop") == 0)
  {
    app_acq_stop();
    app_log_printf("ACQ stop=ok\r\n");
    app_acq_report();
    return;
  }

  if (strcmp(arg, "check") == 0)
  {
    (void)app_acq_check();
    return;
  }

  if (strcmp(arg, "selftest") == 0)
  {
    int rc = app_acq_start(0, 0U);

    if (rc != 0)
    {
      app_log_printf("ACQ SELFTEST FAIL start rc=%d\r\n", rc);
      return;
    }
    tx_thread_sleep(APP_MS_TO_TICKS(1500));
    app_acq_stop();
    rc = app_acq_check();
    app_log_printf("ACQ SELFTEST %s rc=%d\r\n", (rc == 0) ? "PASS" : "FAIL", rc);
    return;
  }

  app_log_printf("ERR unknown-acq=%s\r\n", arg);
}

/* Runs with the media lock held: the acquisition writer thread logs on the
   same FileX medium. */
static void console_fs_locked(const char *arg)
{
  static char name[APP_CON_FS_NAME_MAX];
  static char data[APP_CON_FS_DATA_MAX];
  const char *cursor;
  const char *split;
  size_t name_len;

  while (*arg == ' ')
  {
    arg++;
  }

  if ((*arg == '\0') || (strcmp(arg, "info") == 0))
  {
    app_fxdisk_report();
    return;
  }

  if (strcmp(arg, "ls") == 0)
  {
    int count = app_fxdisk_list();
    app_log_printf("FS count=%d\r\n", count);
    return;
  }

  if (strcmp(arg, "format") == 0)
  {
    int rc;

    if (app_acq_running() != 0)
    {
      app_log_printf("ERR acq-running stop-capture-first\r\n");
      return;
    }
    rc = app_fxdisk_format();
    app_log_printf("FS format=%s rc=%d\r\n", (rc == 0) ? "ok" : "fail", rc);
    return;
  }

  if (strcmp(arg, "mount") == 0)
  {
    int rc = app_fxdisk_mount();
    app_log_printf("FS mount=%s rc=%d\r\n", (rc == 0) ? "ok" : "fail", rc);
    return;
  }

  if ((strcmp(arg, "umount") == 0) || (strcmp(arg, "unmount") == 0))
  {
    int rc;

    if (app_acq_running() != 0)
    {
      app_log_printf("ERR acq-running stop-capture-first\r\n");
      return;
    }
    rc = app_fxdisk_unmount();
    app_log_printf("FS umount=%s rc=%d\r\n", (rc == 0) ? "ok" : "fail", rc);
    return;
  }

  if (strcmp(arg, "test") == 0)
  {
    static const char payload[] = "filex-simdisk-selftest";
    uint32_t got = 0U;
    int ok;

    ok = (app_fxdisk_write("SELFTEST.TXT", payload, (uint32_t)(sizeof(payload) - 1U), 0) == 0);
    if (ok)
    {
      ok = (app_fxdisk_read("SELFTEST.TXT", data, sizeof(data), &got) == 0);
    }
    if (ok)
    {
      ok = (got == (sizeof(payload) - 1U)) &&
           (memcmp(data, payload, sizeof(payload) - 1U) == 0);
    }
    if (ok)
    {
      ok = (app_fxdisk_delete("SELFTEST.TXT") == 0);
    }
    if (ok)
    {
      ok = (app_fxdisk_read("SELFTEST.TXT", data, sizeof(data), &got) != 0);
    }
    app_log_printf("FS SELFTEST %s bytes=%lu\r\n", ok ? "PASS" : "FAIL",
                   (unsigned long)got);
    return;
  }

  if ((strncmp(arg, "write ", 6) == 0) || (strncmp(arg, "append ", 7) == 0))
  {
    int append = (arg[0] == 'a') ? 1 : 0;
    int rc;

    cursor = arg + (append ? 7 : 6);
    split = strchr(cursor, ' ');
    name_len = (split == NULL) ? strlen(cursor) : (size_t)(split - cursor);
    if ((name_len == 0U) || (name_len >= sizeof(name)) || (split == NULL))
    {
      app_log_printf("ERR usage: fs %s <name> <text>\r\n", append ? "append" : "write");
      return;
    }

    memcpy(name, cursor, name_len);
    name[name_len] = '\0';
    snprintf(data, sizeof(data), "%s", split + 1);

    rc = app_fxdisk_write(name, data, (uint32_t)strlen(data), append);
    app_log_printf("FS WROTE name=%s bytes=%lu rc=%d\r\n", name,
                   (unsigned long)strlen(data), rc);
    return;
  }

  if (strncmp(arg, "read ", 5) == 0)
  {
    uint32_t got = 0U;
    int rc;

    cursor = arg + 5;
    while (*cursor == ' ')
    {
      cursor++;
    }
    if ((*cursor == '\0') || (strlen(cursor) >= sizeof(name)))
    {
      app_log_printf("ERR usage: fs read <name>\r\n");
      return;
    }
    snprintf(name, sizeof(name), "%s", cursor);

    rc = app_fxdisk_read(name, data, sizeof(data), &got);
    if (rc == 0)
    {
      app_log_printf("FS DATA name=%s bytes=%lu text=%s\r\n", name,
                     (unsigned long)got, data);
    }
    else
    {
      app_log_printf("FS READ name=%s rc=%d\r\n", name, rc);
    }
    return;
  }

  if (strncmp(arg, "rm ", 3) == 0)
  {
    int rc;

    cursor = arg + 3;
    while (*cursor == ' ')
    {
      cursor++;
    }
    if ((*cursor == '\0') || (strlen(cursor) >= sizeof(name)))
    {
      app_log_printf("ERR usage: fs rm <name>\r\n");
      return;
    }
    snprintf(name, sizeof(name), "%s", cursor);

    rc = app_fxdisk_delete(name);
    app_log_printf("FS RM name=%s rc=%d\r\n", name, rc);
    return;
  }

  if (strncmp(arg, "fill ", 5) == 0)
  {
    uint32_t kb = (uint32_t)strtoul(arg + 5, NULL, 10);
    uint32_t len;
    int rc;

    if ((kb == 0U) || (kb > 48U))
    {
      app_log_printf("ERR usage: fs fill <1..48>  (KiB)\r\n");
      return;
    }

    len = kb * 1024U;
    rc = app_fxdisk_write_pattern("FILL.BIN", len, 0x5AU);
    if (rc == 0)
    {
      rc = app_fxdisk_verify_pattern("FILL.BIN", len, 0x5AU);
    }
    app_log_printf("FS FILL %s bytes=%lu rc=%d\r\n", (rc == 0) ? "PASS" : "FAIL",
                   (unsigned long)len, rc);
    return;
  }

  if (strncmp(arg, "stress ", 7) == 0)
  {
    uint32_t runs = (uint32_t)strtoul(arg + 7, NULL, 10);
    int ok = 1;

    if ((runs == 0U) || (runs > 500U))
    {
      app_log_printf("ERR usage: fs stress <1..500>\r\n");
      return;
    }

    for (uint32_t i = 0U; (i < runs) && (ok != 0); i++)
    {
      ok = (app_fxdisk_write_pattern("STRESS.TXT", 512U, i) == 0) &&
           (app_fxdisk_verify_pattern("STRESS.TXT", 512U, i) == 0);
    }
    app_log_printf("FS STRESS %s runs=%lu\r\n", ok ? "PASS" : "FAIL",
                   (unsigned long)runs);
    app_fxdisk_report();
    return;
  }

  app_log_printf("ERR unknown-fs=%s\r\n", arg);
}

static void console_stall(const char *arg)
{
  unsigned long n = strtoul(arg, NULL, 10);

  if (n == 0UL)
  {
    for (uint32_t i = 0U; i < (uint32_t)APP_HB_COUNT; i++)
    {
      app_hb_set_stall((app_hb_id_t)i, 0);
    }
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
  else
  {
    app_log_printf("ERR stall index must be 0, 1 or 2\r\n");
  }
}

static void console_fs(const char *arg)
{
  if (app_fxdisk_lock() != 0)
  {
    app_log_printf("ERR fs-busy\r\n");
    return;
  }
  console_fs_locked(arg);
  (void)app_fxdisk_unlock();
}

static void console_dma(const char *arg)
{
  while (*arg == ' ')
  {
    arg++;
  }

  if ((*arg == '\0') || (strcmp(arg, "env") == 0))
  {
    app_dma_env_report();
    return;
  }

  if (strcmp(arg, "test") == 0)
  {
    if (app_acq_running() != 0)
    {
      app_log_printf("ERR acq-running stop-capture-first\r\n");
      return;
    }
    int failures = app_dma_mem_selftest();
    app_log_printf("DMA RESULT %s\r\n", (failures == 0) ? "PASS" : "FAIL");
    return;
  }

  app_log_printf("ERR unknown-dma=%s\r\n", arg);
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
  else if (strcmp(line, "fault") == 0)
  {
    console_fault_report();
  }
  else if (strcmp(line, "wdg") == 0)
  {
    app_log_printf("WDG timeout_ms=%lu feed_ms=%lu feeds=%lu\r\n",
                   (unsigned long)APP_WDG_TIMEOUT_MS,
                   (unsigned long)APP_WDG_FEED_MS,
                   (unsigned long)app_watchdog_feed_count());
  }
  else if (strncmp(line, "log ", 4) == 0)
  {
    app_log_printf("LOG %s\r\n", line + 4);
  }
  else if (strncmp(line, "acq", 3) == 0)
  {
    console_acq(line + 3);
  }
  else if (strncmp(line, "fs", 2) == 0)
  {
    console_fs(line + 2);
  }
  else if (strncmp(line, "dma", 3) == 0)
  {
    console_dma(line + 3);
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
