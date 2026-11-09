#include "app_control.h"
#include "app_config.h"
#include "app_fault.h"
#include "app_log.h"
#include "app_param.h"
#include "app_watchdog.h"

#ifndef APP_PLANT_ADC_DEFAULT
#define APP_PLANT_ADC_DEFAULT 0
#endif

/* 250 MHz / (12499 + 1) = 20 kHz PWM; control runs every 2nd update = 10 kHz. */
#define CTRL_PWM_ARR          12499U
#define CTRL_CTRL_DIV         2U
#define CTRL_OUT_MAX          1000
#define CTRL_ADC_MAX          4095
#define CTRL_TIMER_HZ         250000000U
#define CTRL_PWM_HZ           20000U
#define CTRL_EXPECTED_CYCLES  (CTRL_TIMER_HZ / CTRL_PWM_HZ)
#define CTRL_OVERRUN_CYCLES   (CTRL_EXPECTED_CYCLES / 2U)

static TIM_HandleTypeDef htim1;
static ADC_HandleTypeDef hadc1;

static volatile int32_t ctrl_setpoint;
static volatile int32_t ctrl_feedback;
static volatile int32_t ctrl_output;
static volatile uint32_t ctrl_adc_raw;
static volatile uint32_t ctrl_isr_count;
static volatile uint32_t ctrl_overruns;
static volatile uint32_t ctrl_jitter_min = 0xFFFFFFFFU;
static volatile uint32_t ctrl_jitter_max;
static volatile uint8_t ctrl_plant = (APP_PLANT_ADC_DEFAULT != 0) ? (uint8_t)APP_PLANT_ADC
                                                                  : (uint8_t)APP_PLANT_SIM;
static volatile uint8_t ctrl_enabled = 1U;

static volatile float ctrl_kp = 0.05f;
static volatile float ctrl_ki = 0.002f;
static volatile float ctrl_cal_offset = 0.0f;
static volatile float ctrl_cal_scale = 1.0f;
static float ctrl_integral;
static float ctrl_sim_fb;
static float ctrl_adc_fb;
static uint32_t ctrl_div;
static uint32_t ctrl_last_cycles;

static void control_step(void);

void app_control_init(void)
{
  TIM_ClockConfigTypeDef clock_config = {0};
  TIM_MasterConfigTypeDef master_config = {0};
  TIM_OC_InitTypeDef oc_config = {0};
  ADC_InjectionConfTypeDef injected = {0};

  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0U;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = CTRL_PWM_ARR;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0U;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }

  clock_config.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &clock_config) != HAL_OK)
  {
    Error_Handler();
  }

  master_config.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master_config.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  master_config.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &master_config) != HAL_OK)
  {
    Error_Handler();
  }

  oc_config.OCMode = TIM_OCMODE_PWM1;
  oc_config.Pulse = 0U;
  oc_config.OCPolarity = TIM_OCPOLARITY_HIGH;
  oc_config.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  oc_config.OCFastMode = TIM_OCFAST_DISABLE;
  oc_config.OCIdleState = TIM_OCIDLESTATE_RESET;
  oc_config.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc_config, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  htim1.Instance->CCMR1 |= TIM_CCMR1_OC1PE; /* preload CCR1 at the next update */

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1U;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.SamplingMode = ADC_SAMPLING_MODE_NORMAL;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  injected.InjectedChannel = ADC_CHANNEL_10; /* PC0 / Arduino A1 */
  injected.InjectedRank = ADC_INJECTED_RANK_1;
  injected.InjectedSamplingTime = ADC_SAMPLETIME_24CYCLES_5;
  injected.InjectedSingleDiff = ADC_SINGLE_ENDED;
  injected.InjectedOffsetNumber = ADC_OFFSET_NONE;
  injected.InjectedOffset = 0U;
  injected.InjectedOffsetSign = ADC_OFFSET_SIGN_POSITIVE;
  injected.InjectedOffsetSaturation = DISABLE;
  injected.InjectedNbrOfConversion = 1U;
  injected.InjectedDiscontinuousConvMode = DISABLE;
  injected.AutoInjectedConv = DISABLE;
  injected.QueueInjectedContext = DISABLE;
  injected.ExternalTrigInjecConv = ADC_EXTERNALTRIGINJEC_T1_TRGO;
  injected.ExternalTrigInjecConvEdge = ADC_EXTERNALTRIGINJECCONV_EDGE_RISING;
  injected.InjecOversamplingMode = DISABLE;
  if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &injected) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }

  ctrl_setpoint = 0;
  ctrl_feedback = 0;
  ctrl_output = 0;
  ctrl_integral = 0.0f;
  ctrl_sim_fb = 0.0f;
  ctrl_adc_fb = 0.0f;
  ctrl_div = 0U;
  ctrl_isr_count = 0U;
  ctrl_overruns = 0U;

  /* Restore calibration before the ISR starts, so the first control step
     already uses the stored values. */
  float stored_offset = 0.0f;
  float stored_scale = 1.0f;
  if (app_param_load(&stored_offset, &stored_scale) != 0)
  {
    app_control_set_calibration(stored_offset, stored_scale);
  }
}

void app_control_start(void)
{
  if (app_fault_safe_mode() != 0)
  {
    ctrl_enabled = 0U;
    app_log_printf("SAFE control=off\r\n");
  }

  ctrl_last_cycles = DWT->CYCCNT;

  if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);

  if (HAL_ADCEx_InjectedStart_IT(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
}

void app_control_tim_isr(void)
{
  uint32_t now = DWT->CYCCNT;
  uint32_t delta = now - ctrl_last_cycles;
  ctrl_last_cycles = now;

  if (ctrl_isr_count > 0U)
  {
    if (delta < ctrl_jitter_min)
    {
      ctrl_jitter_min = delta;
    }
    if (delta > ctrl_jitter_max)
    {
      ctrl_jitter_max = delta;
    }
    if ((delta > (CTRL_EXPECTED_CYCLES + CTRL_OVERRUN_CYCLES)) ||
        (delta < (CTRL_EXPECTED_CYCLES - CTRL_OVERRUN_CYCLES)))
    {
      ctrl_overruns++;
    }
  }
  ctrl_isr_count++;
  app_hb_mark(APP_HB_CONTROL);

  ctrl_div++;
  if (ctrl_div >= CTRL_CTRL_DIV)
  {
    ctrl_div = 0U;
    control_step();
  }
}

static void control_step(void)
{
  int32_t feedback;

  if (ctrl_plant == (uint8_t)APP_PLANT_SIM)
  {
    float target = ((float)ctrl_output * (float)CTRL_ADC_MAX) / (float)CTRL_OUT_MAX;
    ctrl_sim_fb += (target - ctrl_sim_fb) * 0.02f; /* first-order plant, tau = 5 ms */
    feedback = (int32_t)ctrl_sim_fb;
  }
  else
  {
    float calibrated = ((float)ctrl_adc_raw - ctrl_cal_offset) * ctrl_cal_scale;
    if (calibrated < 0.0f)
    {
      calibrated = 0.0f;
    }
    else if (calibrated > (float)CTRL_ADC_MAX)
    {
      calibrated = (float)CTRL_ADC_MAX;
    }
    ctrl_adc_fb += (calibrated - ctrl_adc_fb) * 0.25f;
    feedback = (int32_t)ctrl_adc_fb;
  }
  ctrl_feedback = feedback;

  if (ctrl_enabled == 0U)
  {
    ctrl_integral = 0.0f;
    ctrl_output = 0;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 0U);
    return;
  }

  int32_t error = ctrl_setpoint - feedback;
  float feedforward = ((float)ctrl_setpoint * (float)CTRL_OUT_MAX) / (float)CTRL_ADC_MAX;
  float p = ctrl_kp * (float)error;
  float i = ctrl_integral + ctrl_ki * (float)error;
  float u = feedforward + p + i;

  if (u > (float)CTRL_OUT_MAX)
  {
    u = (float)CTRL_OUT_MAX;
    if (error > 0)
    {
      i = ctrl_integral;
    }
  }
  else if (u < 0.0f)
  {
    u = 0.0f;
    if (error < 0)
    {
      i = ctrl_integral;
    }
  }

  ctrl_integral = i;
  ctrl_output = (int32_t)u;
  uint32_t compare = (uint32_t)((u * (float)CTRL_PWM_ARR) / (float)CTRL_OUT_MAX);
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, compare);
}

void app_control_report(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  int32_t sp = ctrl_setpoint;
  int32_t fb = ctrl_feedback;
  int32_t out = ctrl_output;
  uint32_t adc = ctrl_adc_raw;
  uint32_t jmin = ctrl_jitter_min;
  uint32_t jmax = ctrl_jitter_max;
  uint32_t ovr = ctrl_overruns;
  uint32_t cnt = ctrl_isr_count;
  uint8_t mode = ctrl_plant;
  uint8_t on = ctrl_enabled;
  float kp = ctrl_kp;
  float ki = ctrl_ki;

  ctrl_jitter_min = 0xFFFFFFFFU;
  ctrl_jitter_max = 0U;

  __set_PRIMASK(primask);

  uint32_t jmin_us100 = (jmin == 0xFFFFFFFFU) ? 0U : (uint32_t)(((uint64_t)jmin * 100U) / 250U);
  uint32_t jmax_us100 = (uint32_t)(((uint64_t)jmax * 100U) / 250U);
  uint32_t kp100 = (uint32_t)(kp * 100.0f);
  uint32_t ki1000 = (uint32_t)(ki * 1000.0f);

  app_log_printf("CTRL mode=%s on=%u sp=%ld fb=%ld adc=%lu out=%ld err=%ld jit_us=%lu.%02lu/%lu.%02lu ovr=%lu cnt=%lu kp=%lu.%02lu ki=%lu.%03lu\r\n",
                 (mode == (uint8_t)APP_PLANT_SIM) ? "sim" : "adc",
                 (unsigned)on,
                 (long)sp,
                 (long)fb,
                 (unsigned long)adc,
                 (long)out,
                 (long)(sp - fb),
                 (unsigned long)(jmin_us100 / 100U),
                 (unsigned long)(jmin_us100 % 100U),
                 (unsigned long)(jmax_us100 / 100U),
                 (unsigned long)(jmax_us100 % 100U),
                 (unsigned long)ovr,
                 (unsigned long)cnt,
                 (unsigned long)(kp100 / 100U),
                 (unsigned long)(kp100 % 100U),
                 (unsigned long)(ki1000 / 1000U),
                 (unsigned long)(ki1000 % 1000U));
}

void app_control_clear_stats(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ctrl_jitter_min = 0xFFFFFFFFU;
  ctrl_jitter_max = 0U;
  ctrl_overruns = 0U;
  __set_PRIMASK(primask);
}

int app_control_set_setpoint(int32_t setpoint)
{
  if (setpoint < 0)
  {
    setpoint = 0;
  }
  if (setpoint > CTRL_ADC_MAX)
  {
    setpoint = CTRL_ADC_MAX;
  }
  ctrl_setpoint = setpoint;
  return 0;
}

void app_control_set_plant(app_plant_mode_t mode)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  if (mode == APP_PLANT_SIM)
  {
    ctrl_sim_fb = (float)ctrl_feedback;
  }
  else
  {
    ctrl_adc_fb = (float)ctrl_adc_raw;
  }
  ctrl_plant = (uint8_t)mode;
  ctrl_integral = 0.0f;

  __set_PRIMASK(primask);
}

int app_control_set_enabled(int on)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  ctrl_enabled = (on != 0) ? 1U : 0U;
  if (ctrl_enabled == 0U)
  {
    ctrl_integral = 0.0f;
  }

  __set_PRIMASK(primask);
  return 0;
}

void app_control_set_calibration(float offset, float scale)
{
  if (offset < -4095.0f)
  {
    offset = -4095.0f;
  }
  else if (offset > 4095.0f)
  {
    offset = 4095.0f;
  }
  if (scale < 0.01f)
  {
    scale = 0.01f;
  }
  else if (scale > 10.0f)
  {
    scale = 10.0f;
  }

  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ctrl_cal_offset = offset;
  ctrl_cal_scale = scale;
  float calibrated = ((float)ctrl_adc_raw - offset) * scale;
  if (calibrated < 0.0f)
  {
    calibrated = 0.0f;
  }
  else if (calibrated > (float)CTRL_ADC_MAX)
  {
    calibrated = (float)CTRL_ADC_MAX;
  }
  ctrl_adc_fb = calibrated;
  ctrl_integral = 0.0f;
  __set_PRIMASK(primask);
}

void app_control_get_calibration(float *offset, float *scale)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  float local_offset = ctrl_cal_offset;
  float local_scale = ctrl_cal_scale;
  __set_PRIMASK(primask);

  if (offset != NULL)
  {
    *offset = local_offset;
  }
  if (scale != NULL)
  {
    *scale = local_scale;
  }
}

void app_control_reset_calibration(void)
{
  app_control_set_calibration(0.0f, 1.0f);
}

void app_control_set_gains(float kp, float ki)
{
  if (kp < 0.0f)
  {
    kp = 0.0f;
  }
  if (ki < 0.0f)
  {
    ki = 0.0f;
  }

  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  ctrl_kp = kp;
  ctrl_ki = ki;
  __set_PRIMASK(primask);
}

app_plant_mode_t app_control_get_plant(void)
{
  return (app_plant_mode_t)ctrl_plant;
}

int32_t app_control_get_setpoint(void)
{
  return ctrl_setpoint;
}

void TIM1_UP_IRQHandler(void)
{
  if ((__HAL_TIM_GET_FLAG(&htim1, TIM_FLAG_UPDATE) != RESET) &&
      (__HAL_TIM_GET_IT_SOURCE(&htim1, TIM_IT_UPDATE) != RESET))
  {
    __HAL_TIM_CLEAR_IT(&htim1, TIM_IT_UPDATE);
    app_control_tim_isr();
  }
}

void ADC1_IRQHandler(void)
{
  HAL_ADC_IRQHandler(&hadc1);
}

void HAL_ADCEx_InjectedConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    ctrl_adc_raw = HAL_ADCEx_InjectedGetValue(hadc, ADC_INJECTED_RANK_1);
  }
}
