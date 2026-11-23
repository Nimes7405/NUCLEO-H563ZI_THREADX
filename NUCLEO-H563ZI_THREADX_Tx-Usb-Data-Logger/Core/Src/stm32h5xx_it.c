#include "main.h"
#include "stm32h5xx_it.h"

extern TIM_HandleTypeDef htim6;

void ADC1_IRQHandler(void);

void TIM6_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}

void USART3_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart3);
}
