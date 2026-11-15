#ifndef APP_STACKS_H
#define APP_STACKS_H

#include <stdint.h>

typedef enum
{
  APP_STK_MONITOR = 0,
  APP_STK_LOGGER,
  APP_STK_CONSOLE,
  APP_STK_PROD_A,
  APP_STK_PROD_B,
  APP_STK_SAMPLER,
  APP_STK_WRITER,
  APP_STK_COUNT
} app_stack_id_t;

void app_stacks_bind(app_stack_id_t id, void *base, uint32_t size);
uint32_t app_stacks_used(app_stack_id_t id);
uint32_t app_stacks_size(app_stack_id_t id);
int app_stacks_any_overflow(void);
int app_stacks_overflow_id(void);
uint8_t app_stacks_first_byte(app_stack_id_t id);
void app_stacks_report(void);

#endif
