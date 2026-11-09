#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdint.h>

typedef enum
{
  APP_PLANT_SIM = 0,
  APP_PLANT_ADC = 1
} app_plant_mode_t;

void app_control_init(void);
void app_control_start(void);
void app_control_tim_isr(void);
void app_control_report(void);

int app_control_set_setpoint(int32_t setpoint);
void app_control_set_plant(app_plant_mode_t mode);
int app_control_set_enabled(int on);
void app_control_set_gains(float kp, float ki);
void app_control_set_calibration(float offset, float scale);
void app_control_get_calibration(float *offset, float *scale);
void app_control_reset_calibration(void);
app_plant_mode_t app_control_get_plant(void);
int32_t app_control_get_setpoint(void);

#endif
