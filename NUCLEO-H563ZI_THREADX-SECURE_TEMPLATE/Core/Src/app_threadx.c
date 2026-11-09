#include "main.h"
#include "tx_api.h"

#define APP_THREAD_STACK_SIZE 512U
#define APP_THREAD_PRIORITY 10U
#define APP_THREAD_TIME_SLICE 5U
#define THREADX_TICKS_PER_SECOND 100U

static TX_THREAD app_thread;
static UCHAR app_thread_stack[APP_THREAD_STACK_SIZE] __attribute__((aligned(8)));

static void App_Thread_Entry(ULONG thread_input)
{
  (void)thread_input;

  for (;;)
  {
    HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_4);
    tx_thread_sleep(THREADX_TICKS_PER_SECOND);
  }
}

void tx_application_define(VOID *first_unused_memory)
{
  (void)first_unused_memory;

  if (tx_thread_create(&app_thread,
                       "Heartbeat",
                       App_Thread_Entry,
                       0U,
                       app_thread_stack,
                       sizeof(app_thread_stack),
                       APP_THREAD_PRIORITY,
                       APP_THREAD_PRIORITY,
                       APP_THREAD_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS)
  {
    Error_Handler();
  }
}
