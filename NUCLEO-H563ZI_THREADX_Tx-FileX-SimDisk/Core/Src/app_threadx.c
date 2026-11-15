#include "app_config.h"
#include "app_acq.h"
#include "app_console.h"
#include "app_fxdisk.h"
#include "app_log.h"
#include "app_stacks.h"
#include "app_watchdog.h"

static TX_THREAD monitor_thread;
static TX_THREAD logger_thread;
static TX_THREAD console_thread;
static TX_THREAD prod_a_thread;
static TX_THREAD prod_b_thread;
static TX_THREAD sampler_thread;
static TX_THREAD writer_thread;

static UCHAR monitor_stack[APP_STACK_MONITOR] __attribute__((aligned(8)));
static UCHAR logger_stack[APP_STACK_LOGGER] __attribute__((aligned(8)));
static UCHAR console_stack[APP_STACK_CONSOLE] __attribute__((aligned(8)));
static UCHAR prod_a_stack[APP_STACK_PROD_A] __attribute__((aligned(8)));
static UCHAR prod_b_stack[APP_STACK_PROD_B] __attribute__((aligned(8)));
static UCHAR sampler_stack[APP_STACK_SAMPLER] __attribute__((aligned(8)));
static UCHAR writer_stack[APP_STACK_WRITER] __attribute__((aligned(8)));

static void monitor_entry(ULONG thread_input);
static void logger_entry(ULONG thread_input);
static void prod_a_entry(ULONG thread_input);
static void prod_b_entry(ULONG thread_input);

void tx_application_define(VOID *first_unused_memory)
{
  (void)first_unused_memory;

  app_log_init();
  app_fxdisk_system_init();
  app_acq_system_init();

  app_stacks_bind(APP_STK_MONITOR, monitor_stack, sizeof(monitor_stack));
  app_stacks_bind(APP_STK_LOGGER, logger_stack, sizeof(logger_stack));
  app_stacks_bind(APP_STK_CONSOLE, console_stack, sizeof(console_stack));
  app_stacks_bind(APP_STK_PROD_A, prod_a_stack, sizeof(prod_a_stack));
  app_stacks_bind(APP_STK_PROD_B, prod_b_stack, sizeof(prod_b_stack));
  app_stacks_bind(APP_STK_SAMPLER, sampler_stack, sizeof(sampler_stack));
  app_stacks_bind(APP_STK_WRITER, writer_stack, sizeof(writer_stack));

  if (tx_thread_create(&monitor_thread, "monitor", monitor_entry, 0U,
                       monitor_stack, sizeof(monitor_stack),
                       APP_PRIO_MONITOR, APP_PRIO_MONITOR,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&logger_thread, "logger", logger_entry, 0U,
                       logger_stack, sizeof(logger_stack),
                       APP_PRIO_LOGGER, APP_PRIO_LOGGER,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&console_thread, "console", app_console_thread_entry, 0U,
                       console_stack, sizeof(console_stack),
                       APP_PRIO_CONSOLE, APP_PRIO_CONSOLE,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&prod_a_thread, "prod_a", prod_a_entry, 0U,
                       prod_a_stack, sizeof(prod_a_stack),
                       APP_PRIO_PROD_A, APP_PRIO_PROD_A,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_thread_create(&prod_b_thread, "prod_b", prod_b_entry, 0U,
                       prod_b_stack, sizeof(prod_b_stack),
                       APP_PRIO_PROD_B, APP_PRIO_PROD_B,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }

  if (tx_thread_create(&sampler_thread, "sampler", app_acq_thread_entry, 0U,
                       sampler_stack, sizeof(sampler_stack),
                       APP_PRIO_SAMPLER, APP_PRIO_SAMPLER,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }

  if (tx_thread_create(&writer_thread, "writer", app_acq_writer_entry, 0U,
                       writer_stack, sizeof(writer_stack),
                       APP_PRIO_WRITER, APP_PRIO_WRITER,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }

  app_log_set_ready();
}

static void monitor_entry(ULONG thread_input)
{
  (void)thread_input;
  uint32_t loops = 0U;

  for (;;)
  {
    app_hb_mark(APP_HB_MONITOR);
    (void)app_watchdog_poll();

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
