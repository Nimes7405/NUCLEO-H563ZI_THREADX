#include "app_config.h"
#include "app_console.h"
#include "app_control.h"
#include "app_log.h"
#include "app_mpu.h"
#include "app_stacks.h"
#include "app_watchdog.h"

static TX_THREAD monitor_thread;
static TX_THREAD logger_thread;
static TX_THREAD console_thread;
static TX_THREAD prod_a_thread;
static TX_THREAD prod_b_thread;

#define APP_STACK_BLOCK(name, size)                                      \
  typedef struct                                                         \
  {                                                                      \
    uint32_t guard[8];                                                   \
    UCHAR stack[size] __attribute__((aligned(32)));                      \
  } name##_block_t;                                                      \
  static name##_block_t name##_block __attribute__((aligned(32)))

APP_STACK_BLOCK(monitor, APP_STACK_MONITOR);
APP_STACK_BLOCK(logger, APP_STACK_LOGGER);
APP_STACK_BLOCK(console, APP_STACK_CONSOLE);
APP_STACK_BLOCK(prod_a, APP_STACK_PROD_A);
APP_STACK_BLOCK(prod_b, APP_STACK_PROD_B);

static void monitor_entry(ULONG thread_input);
static void logger_entry(ULONG thread_input);
static void prod_a_entry(ULONG thread_input);
static void prod_b_entry(ULONG thread_input);

void tx_application_define(VOID *first_unused_memory)
{
  (void)first_unused_memory;

  app_log_init();

  app_stacks_bind(APP_STK_MONITOR, monitor_block.stack, sizeof(monitor_block.stack));
  app_stacks_bind_guard(APP_STK_MONITOR, monitor_block.guard, sizeof(monitor_block.guard));
  app_stacks_bind(APP_STK_LOGGER, logger_block.stack, sizeof(logger_block.stack));
  app_stacks_bind_guard(APP_STK_LOGGER, logger_block.guard, sizeof(logger_block.guard));
  app_stacks_bind(APP_STK_CONSOLE, console_block.stack, sizeof(console_block.stack));
  app_stacks_bind_guard(APP_STK_CONSOLE, console_block.guard, sizeof(console_block.guard));
  app_stacks_bind(APP_STK_PROD_A, prod_a_block.stack, sizeof(prod_a_block.stack));
  app_stacks_bind_guard(APP_STK_PROD_A, prod_a_block.guard, sizeof(prod_a_block.guard));
  app_stacks_bind(APP_STK_PROD_B, prod_b_block.stack, sizeof(prod_b_block.stack));
  app_stacks_bind_guard(APP_STK_PROD_B, prod_b_block.guard, sizeof(prod_b_block.guard));

  app_mpu_init();

  if (tx_thread_create(&monitor_thread, "monitor", monitor_entry, 0U,
                       monitor_block.stack, sizeof(monitor_block.stack),
                       APP_PRIO_MONITOR, APP_PRIO_MONITOR,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&logger_thread, "logger", logger_entry, 0U,
                       logger_block.stack, sizeof(logger_block.stack),
                       APP_PRIO_LOGGER, APP_PRIO_LOGGER,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&console_thread, "console", app_console_thread_entry, 0U,
                       console_block.stack, sizeof(console_block.stack),
                       APP_PRIO_CONSOLE, APP_PRIO_CONSOLE,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&prod_a_thread, "prod_a", prod_a_entry, 0U,
                       prod_a_block.stack, sizeof(prod_a_block.stack),
                       APP_PRIO_PROD_A, APP_PRIO_PROD_A,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&prod_b_thread, "prod_b", prod_b_entry, 0U,
                       prod_b_block.stack, sizeof(prod_b_block.stack),
                       APP_PRIO_PROD_B, APP_PRIO_PROD_B,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }

  app_control_start();
  app_log_set_ready();
}

static void monitor_entry(ULONG thread_input)
{
  (void)thread_input;
  uint32_t loops = 0U;

  app_watchdog_wwdg_start();
  tx_thread_sleep(APP_MS_TO_TICKS(80));

  for (;;)
  {
    app_hb_mark(APP_HB_MONITOR);
    (void)app_watchdog_poll();

    if ((loops % 5U) == 0U)
    {
      app_control_report();
    }
    if ((loops % 50U) == 0U)
    {
      app_console_status();
    }
    loops++;

    tx_thread_sleep(APP_MS_TO_TICKS(APP_MONITOR_PERIOD_MS));
  }
}

static void logger_entry(ULONG thread_input)
{
  (void)thread_input;

  for (;;)
  {
    app_hb_mark(APP_HB_LOGGER);
    (void)app_log_wait(APP_MS_TO_TICKS(500));
    while (app_log_drain() > 0U)
    {
    }
  }
}

static void prod_a_entry(ULONG thread_input)
{
  (void)thread_input;
  uint32_t n = 0U;

  for (;;)
  {
    app_hb_mark(APP_HB_PROD_A);
    HAL_GPIO_TogglePin(APP_LED_GREEN_GPIO_Port, APP_LED_GREEN_Pin);
    if ((n % 20U) == 0U)
    {
      app_log_printf("PROD_A alive n=%lu\r\n", (unsigned long)n);
    }
    n++;
    tx_thread_sleep(APP_MS_TO_TICKS(500));
  }
}

static void prod_b_entry(ULONG thread_input)
{
  (void)thread_input;
  uint32_t n = 0U;

  for (;;)
  {
    app_hb_mark(APP_HB_PROD_B);
    if ((n % 10U) == 0U)
    {
      app_log_printf("PROD_B counter=%lu\r\n", (unsigned long)n);
    }
    n++;
    tx_thread_sleep(APP_MS_TO_TICKS(500));
  }
}
