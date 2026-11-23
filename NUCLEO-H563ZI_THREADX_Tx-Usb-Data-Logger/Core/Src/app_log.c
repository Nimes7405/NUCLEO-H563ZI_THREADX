#include "app_log.h"
#include "app_config.h"

#include <stdarg.h>
#include <stdio.h>

#define APP_LOG_EVENT  1UL

static uint8_t log_ring[APP_LOG_RING_SIZE];
static volatile uint32_t log_head;
static volatile uint32_t log_tail;
static volatile uint32_t log_dropped;
static volatile int log_ready;
static volatile int uart_ready;

static TX_MUTEX log_mutex;
static TX_EVENT_FLAGS_GROUP log_events;

static void uart_tx_blocking(const char *data, uint32_t len)
{
  if (!uart_ready || len == 0U)
  {
    return;
  }
  (void)HAL_UART_Transmit(&huart3, (uint8_t *)data, (uint16_t)len, 200U);
}

void app_log_set_uart_ready(void)
{
  uart_ready = 1;
}

void app_log_init(void)
{
  if (tx_mutex_create(&log_mutex, "log-mutex", TX_INHERIT) != TX_SUCCESS)
  {
    Error_Handler();
  }
  if (tx_event_flags_create(&log_events, "log-events") != TX_SUCCESS)
  {
    Error_Handler();
  }
}

void app_log_set_ready(void)
{
  log_ready = 1;
}

/* Caller must hold log_mutex. */
static void log_push_locked(const char *data, uint32_t len)
{
  uint32_t head = log_head;
  uint32_t used = head - log_tail;

  if ((APP_LOG_RING_SIZE - used) < len)
  {
    log_dropped++;
    return;
  }

  for (uint32_t i = 0U; i < len; i++)
  {
    log_ring[(head + i) & (APP_LOG_RING_SIZE - 1U)] = (uint8_t)data[i];
  }
  __DMB();
  log_head = head + len;
  (void)tx_event_flags_set(&log_events, APP_LOG_EVENT, TX_OR);
}

void app_log_write(const char *data, uint32_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return;
  }

  if (!log_ready)
  {
    uart_tx_blocking(data, len);
    return;
  }

  if (tx_mutex_get(&log_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
  {
    log_dropped++;
    return;
  }
  log_push_locked(data, len);
  (void)tx_mutex_put(&log_mutex);
}

void app_log_printf(const char *fmt, ...)
{
  char buffer[192];
  va_list args;
  int len;

  va_start(args, fmt);
  len = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (len <= 0)
  {
    return;
  }
  if ((size_t)len >= sizeof(buffer))
  {
    len = (int)sizeof(buffer) - 1;
  }

  /* Serialize newlib formatting as well as the ring update: _impure_ptr is
     shared, and concurrent vsnprintf calls are not thread-safe. */
  if (!log_ready)
  {
    uart_tx_blocking(buffer, (uint32_t)len);
    return;
  }
  if (tx_mutex_get(&log_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
  {
    log_dropped++;
    return;
  }
  log_push_locked(buffer, (uint32_t)len);
  (void)tx_mutex_put(&log_mutex);
}

uint32_t app_log_dropped(void)
{
  return log_dropped;
}

uint32_t app_log_wait(uint32_t ticks)
{
  ULONG flags = 0U;
  return (uint32_t)tx_event_flags_get(&log_events, APP_LOG_EVENT,
                                      TX_OR_CLEAR, &flags, ticks);
}

uint32_t app_log_drain(void)
{
  char buffer[256];
  uint32_t head = log_head;
  uint32_t tail = log_tail;
  uint32_t avail = head - tail;

  if (avail == 0U)
  {
    return 0U;
  }
  if (avail > sizeof(buffer))
  {
    avail = (uint32_t)sizeof(buffer);
  }

  for (uint32_t i = 0U; i < avail; i++)
  {
    buffer[i] = (char)log_ring[(tail + i) & (APP_LOG_RING_SIZE - 1U)];
  }
  __DMB();
  log_tail = tail + avail;
  uart_tx_blocking(buffer, avail);
  return avail;
}
