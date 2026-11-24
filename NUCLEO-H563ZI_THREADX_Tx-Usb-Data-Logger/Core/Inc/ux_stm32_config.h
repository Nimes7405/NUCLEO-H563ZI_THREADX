#ifndef UX_STM32_CONFIG_H
#define UX_STM32_CONFIG_H

/* The device controller port expects the HAL types (PCD_HandleTypeDef) to be
   visible, so pull in the HAL headers from here. */
#include "stm32h5xx_hal.h"

/* Limits of the STM32 USB_DRD_FS device controller port. The hardware has 8
   bidirectional endpoints including EP0, so claim all of them. */
#define UX_DCD_STM32_MAX_ED             8
#define UX_HCD_STM32_MAX_NB_CHANNELS    8

#endif
