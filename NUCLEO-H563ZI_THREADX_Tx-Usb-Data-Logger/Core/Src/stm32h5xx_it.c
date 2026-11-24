#include "main.h"
#include "stm32h5xx_it.h"

extern TIM_HandleTypeDef htim6;
extern PCD_HandleTypeDef hpcd_USB_DRD_FS;

/* Counted so "the host never talked to us" is distinguishable from
   "the interrupts fired and were serviced". */
volatile uint32_t app_usb_irq_count;

void ADC1_IRQHandler(void);

void TIM6_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}

void USART3_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart3);
}

void USB_DRD_FS_IRQHandler(void)
{
  app_usb_irq_count++;
  HAL_PCD_IRQHandler(&hpcd_USB_DRD_FS);
}
