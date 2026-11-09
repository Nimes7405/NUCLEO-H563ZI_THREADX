#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include "app_config.h"

void app_watchdog_init(void);
void app_watchdog_wwdg_start(void);
int app_watchdog_poll(void);
uint32_t app_watchdog_feed_count(void);

void app_hb_mark(app_hb_id_t id);
uint32_t app_hb_get(app_hb_id_t id);
const char *app_hb_name(app_hb_id_t id);
void app_hb_set_stall(app_hb_id_t id, int on);
void app_watchdog_set_wwdg_stall(int on);

#endif
