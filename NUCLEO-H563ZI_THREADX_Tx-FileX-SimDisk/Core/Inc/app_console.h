#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H

#include "tx_api.h"

void app_console_init(void);
void app_console_thread_entry(ULONG thread_input);
void app_console_status(void);

#endif
