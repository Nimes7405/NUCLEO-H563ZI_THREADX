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

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
  if (hspi->Instance == SPI1)
  {
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* SCK PA5 (D13), MISO PG9 (D12), MOSI PB5 (D11). MISO keeps an internal
       pull-up so an absent card reads back as 0xFF instead of noise; the pin
       must stay in alternate-function mode or the SPI never sees the line. */
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;

    gpio.Pin = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* SPI-mode cards leave DO high-Z between transfers, so the pull-up is
       what gives MISO a defined idle level (0xFF). */
    gpio.Pull = GPIO_PULLUP;
    gpio.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* CS on D10 is driven by software. */
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin = GPIO_PIN_14;
    HAL_GPIO_Init(GPIOD, &gpio);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
  }
}
