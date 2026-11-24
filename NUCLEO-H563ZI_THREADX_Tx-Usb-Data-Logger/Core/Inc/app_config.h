#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "main.h"
#include "tx_api.h"

#define APP_VERSION "1.0.0"

/* ThreadX tick is 100 Hz (see tx_user.h). */
#define APP_TICK_HZ             100U
#define APP_MS_TO_TICKS(ms)     (((ms) * APP_TICK_HZ) / 1000U)

/* Thread priorities: lower value = higher priority (ThreadX). */
#define APP_PRIO_MONITOR        3U
#define APP_PRIO_CONSOLE        8U
#define APP_PRIO_PROD_A         10U
#define APP_PRIO_PROD_B         11U
#define APP_PRIO_SAMPLER        5U
#define APP_PRIO_WRITER         6U
#define APP_PRIO_USB_RX         12U
#define APP_PRIO_USB_TX         13U
#define APP_PRIO_LOGGER         20U

#define APP_STACK_MONITOR       2048U
#define APP_STACK_LOGGER        1024U
#define APP_STACK_CONSOLE       4096U
#define APP_STACK_SAMPLER       2048U
#define APP_STACK_PROD_A        2048U
#define APP_STACK_PROD_B        2048U
#define APP_STACK_WRITER        3072U
#define APP_STACK_USB_RX        2048U
#define APP_STACK_USB_TX        1536U

#define APP_LOG_RING_SIZE       4096U
#define APP_CON_RX_SIZE         256U
#define APP_CON_LINE_SIZE       128U
#define APP_CON_FS_NAME_MAX     64U
#define APP_CON_FS_DATA_MAX     512U

/* USBX: the CDC-ACM rings are separate from the log ring so a stalled host
   cannot cost console output. Sizes must stay powers of two. */
#define APP_USB_RX_SIZE         1024U
#define APP_USB_TX_SIZE         4096U
#define APP_USB_POOL_SIZE       12288U
#define APP_USB_STRING_MAX      96U
/* Transfer timeouts in ticks: bounded so the usb threads keep their heartbeat
   and cannot be mistaken for a hang while the link is merely idle. */
#define APP_USB_READ_TIMEOUT    10U
#define APP_USB_WRITE_TIMEOUT   20U

#define APP_MONITOR_PERIOD_MS   100U
#define APP_HB_STALL_MS         1500U
#define APP_WDG_TIMEOUT_MS      4000U
#define APP_WDG_FEED_MS         500U

typedef enum
{
  APP_HB_MONITOR = 0,
  APP_HB_LOGGER,
  APP_HB_CONSOLE,
  APP_HB_PROD_A,
  APP_HB_PROD_B,
  APP_HB_SAMPLER,
  APP_HB_WRITER,
  APP_HB_USB_RX,
  APP_HB_USB_TX,
  APP_HB_COUNT
} app_hb_id_t;

#endif
