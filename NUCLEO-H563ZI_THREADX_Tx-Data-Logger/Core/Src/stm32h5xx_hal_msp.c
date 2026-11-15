#include "main.h"


void HAL_MspInit(void)
{
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM6)
  {
    __HAL_RCC_TIM6_CLK_ENABLE();
  }
  else if (htim->Instance == TIM3)
  {
    __HAL_RCC_TIM3_CLK_ENABLE();
  }
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef gpio = {0};
  RCC_PeriphCLKInitTypeDef clk = {0};

  if (huart->Instance == USART3)
  {
    clk.PeriphClockSelection = RCC_PERIPHCLK_USART3;
    clk.Usart3ClockSelection = RCC_USART3CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* PD8 -> USART3_TX, PD9 -> USART3_RX (ST-LINK virtual COM port). */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &gpio);
  }
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_ADC_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC0..PC3 -> ADC1_INP10..INP13 (Arduino A1 plus three neighbouring
       header pins). */
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_NVIC_SetPriority(ADC1_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(ADC1_IRQn);
  }
}
