#ifndef APP_PARAM_H
#define APP_PARAM_H

#include <stdint.h>

typedef struct
{
  float offset;
  float scale;
  uint32_t stored;
  uint32_t next_slot;
  uint32_t slot_count;
  uint32_t erases;
  int flash_error;
} app_param_info_t;

/* Returns 1 when a valid record was found and the values were returned. */
int app_param_load(float *offset, float *scale);
int app_param_save(float offset, float scale);
int app_param_erase(void);
void app_param_info(app_param_info_t *info);
void app_param_report(void);
uint32_t app_param_base(void);

#endif
