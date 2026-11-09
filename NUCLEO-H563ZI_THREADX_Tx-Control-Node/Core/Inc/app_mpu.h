#ifndef APP_MPU_H
#define APP_MPU_H

#include <stdint.h>

void app_mpu_init(void);
uint32_t app_mpu_region_count(void);
uint32_t app_mpu_guard_count(void);

#endif
