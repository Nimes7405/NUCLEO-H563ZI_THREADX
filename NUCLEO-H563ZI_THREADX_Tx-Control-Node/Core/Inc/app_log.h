#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

void app_log_init(void);
void app_log_set_ready(void);
void app_log_set_uart_ready(void);
void app_log_write(const char *data, uint32_t len);
void app_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t app_log_dropped(void);
uint32_t app_log_wait(uint32_t ticks);
uint32_t app_log_drain(void);

#endif
