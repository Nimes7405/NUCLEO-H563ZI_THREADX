#ifndef APP_USB_H
#define APP_USB_H

#include <stdint.h>
#include "tx_api.h"

/* CDC-ACM device: gives the host a virtual serial port on the same USB-C
   connector that will later carry the mass-storage export. The console keeps
   working over USART3, so a dead USB link never costs the debug channel. */

void app_usb_system_init(void);
void app_usb_rx_thread_entry(ULONG thread_input);
void app_usb_tx_thread_entry(ULONG thread_input);

/* Pull bytes received from the host. Returns 0 when nothing is pending. */
uint32_t app_usb_read(uint8_t *buffer, uint32_t max);

/* Queue bytes for the host. Best effort: drops when the ring is full or the
   host has not configured the device, because log output must never block. */
void app_usb_write(const char *data, uint32_t len);

/* Wait for the queued bytes to reach the host. Used where ordering matters,
   e.g. the trailing line of a dump. */
int app_usb_flush(uint32_t timeout_ticks);

/* Send a block synchronously. Only used by `dump`, where the file content must
   stay contiguous. Returns 0 on success. */
int app_usb_send_blocking(const uint8_t *data, uint32_t len);

/* Suppress the async mirror while a block transfer owns the pipe. */
void app_usb_set_dump(int on);

int app_usb_connected(void);
int app_usb_ready(void);
void app_usb_report(void);

#endif
