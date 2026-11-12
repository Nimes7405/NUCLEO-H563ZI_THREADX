#include "app_stacks.h"
#include "app_config.h"
#include "app_log.h"

#include <string.h>

#define APP_STACK_FILL_BYTE  0xEFU

typedef struct
{
  uint8_t *base;
  uint32_t size;
  const char *name;
} app_stack_desc_t;

static app_stack_desc_t stacks[APP_STK_COUNT] = {
    {NULL, 0U, "monitor"},
    {NULL, 0U, "logger"},
    {NULL, 0U, "console"},
    {NULL, 0U, "prod_a"},
    {NULL, 0U, "prod_b"},
    {NULL, 0U, "sampler"},
};

void app_stacks_bind(app_stack_id_t id, void *base, uint32_t size)
{
  if ((id >= APP_STK_COUNT) || (base == NULL) || (size == 0U))
  {
    return;
  }

  stacks[id].base = (uint8_t *)base;
  stacks[id].size = size;
  memset(base, APP_STACK_FILL_BYTE, size);
  __DMB();
}

uint32_t app_stacks_used(app_stack_id_t id)
{
  if ((id >= APP_STK_COUNT) || (stacks[id].base == NULL))
  {
    return 0U;
  }

  uint32_t free_bytes = 0U;
  while ((free_bytes < stacks[id].size) &&
         (stacks[id].base[free_bytes] == APP_STACK_FILL_BYTE))
  {
    free_bytes++;
  }
  return stacks[id].size - free_bytes;
}

uint32_t app_stacks_size(app_stack_id_t id)
{
  if ((id >= APP_STK_COUNT) || (stacks[id].base == NULL))
  {
    return 0U;
  }
  return stacks[id].size;
}

int app_stacks_overflow_id(void)
{
  for (uint32_t i = 0U; i < (uint32_t)APP_STK_COUNT; i++)
  {
    if (stacks[i].base == NULL)
    {
      continue;
    }
    if (stacks[i].base[0] != APP_STACK_FILL_BYTE)
    {
      return (int)i;
    }
  }
  return -1;
}

int app_stacks_any_overflow(void)
{
  return (app_stacks_overflow_id() >= 0) ? 1 : 0;
}

uint8_t app_stacks_first_byte(app_stack_id_t id)
{
  if ((id >= APP_STK_COUNT) || (stacks[id].base == NULL))
  {
    return 0U;
  }
  return stacks[id].base[0];
}

void app_stacks_report(void)
{
  app_log_printf("STACK ");
  for (uint32_t i = 0U; i < (uint32_t)APP_STK_COUNT; i++)
  {
    app_log_printf("%s=%lu/%lu%s",
                   stacks[i].name,
                   (unsigned long)app_stacks_used((app_stack_id_t)i),
                   (unsigned long)stacks[i].size,
                   (i + 1U == (uint32_t)APP_STK_COUNT) ? "" : " ");
  }
  app_log_printf("\r\n");
}
