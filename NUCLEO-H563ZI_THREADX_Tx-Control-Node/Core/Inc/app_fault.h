#ifndef APP_FAULT_H
#define APP_FAULT_H

#include <stdint.h>

#define APP_FAULT_MAGIC  0x4641554CUL  /* 'FAUL' */

typedef struct
{
  uint32_t magic;
  uint32_t sequence;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  uint32_t lr;
  uint32_t pc;
  uint32_t xpsr;
  uint32_t exc_return;
  uint32_t sp;
  uint32_t guard_id;
  uint32_t valid;
} app_fault_record_t;

void app_fault_init(uint32_t reset_raw);
int app_fault_get_last(app_fault_record_t *out);
int app_fault_safe_mode(void);
void app_fault_clear(void);
const char *app_boot_reset_name(void);

uint32_t app_reset_cause_raw(void);
void app_reset_cause_clear(void);
const char *app_reset_cause_name(uint32_t raw);

void app_fault_capture(uint32_t *sp, uint32_t exc_return);

#endif
