#include "main.h"
#include "tx_api.h"
#include "app_acq.h"
#include "app_config.h"
#include "app_console.h"
#include "app_fault.h"
#include "app_log.h"
#include "app_watchdog.h"

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();

  app_console_init();
  app_log_printf("\r\n=== NUCLEO-H563ZI Tx-Data-Logger %s ===\r\n", APP_VERSION);

  uint32_t reset_raw = app_reset_cause_raw();
  app_reset_cause_clear();
  app_fault_init(reset_raw);
  app_watchdog_init();
  app_acq_init();

  tx_kernel_enter();

  Error_Handler();
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_BYPASS_DIGITAL;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLL1_SOURCE_HSE;
  osc.PLL.PLLM = 4;
  osc.PLL.PLLN = 250;
  osc.PLL.PLLP = 2;
  osc.PLL.PLLQ = 2;
  osc.PLL.PLLR = 2;
  osc.PLL.PLLRGE = RCC_PLL1_VCIRANGE_1;
  osc.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE;
  osc.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    Error_Handler();
  }

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                  RCC_CLOCKTYPE_PCLK3;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  clk.APB3CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  HAL_GPIO_WritePin(APP_LED_GREEN_GPIO_Port, APP_LED_GREEN_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(APP_LED_RED_GPIO_Port, APP_LED_RED_Pin, GPIO_PIN_RESET);

  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;

  gpio.Pin = APP_LED_GREEN_Pin;
  HAL_GPIO_Init(APP_LED_GREEN_GPIO_Port, &gpio);

  gpio.Pin = APP_LED_RED_Pin;
  HAL_GPIO_Init(APP_LED_RED_GPIO_Port, &gpio);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
}
