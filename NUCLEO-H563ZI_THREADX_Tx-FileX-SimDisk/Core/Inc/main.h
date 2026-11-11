#ifndef MAIN_H
#define MAIN_H

#include "stm32h5xx_hal.h"
#include <stdio.h>

void Error_Handler(void);

/* NUCLEO-H563ZI LEDs:
   - LED1 (green): PB0
   - LED3 (red):   PG4 */
#define APP_LED_GREEN_Pin        GPIO_PIN_0
#define APP_LED_GREEN_GPIO_Port  GPIOB
#define APP_LED_RED_Pin          GPIO_PIN_4
#define APP_LED_RED_GPIO_Port    GPIOG

extern UART_HandleTypeDef huart3;
extern IWDG_HandleTypeDef hiwdg;

#endif
