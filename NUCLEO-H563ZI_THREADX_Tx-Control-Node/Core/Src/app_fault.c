#include "app_fault.h"
#include "app_config.h"
#include "app_log.h"
#include "app_stacks.h"

static app_fault_record_t fault_record __attribute__((section(".noinit"), used));
static app_fault_record_t last_fault;
static volatile int have_last_fault;
static const char *boot_reset_name = "unknown";
static volatile int safe_mode;

#define APP_FAULT_SAFE_THRESHOLD 3U

uint32_t app_reset_cause_raw(void)
{
  return RCC->RSR;
}

void app_reset_cause_clear(void)
{
  __HAL_RCC_CLEAR_RESET_FLAGS();
}

const char *app_reset_cause_name(uint32_t raw)
{
  if ((raw & RCC_RSR_IWDGRSTF) != 0U)
  {
    return "iwdg";
  }
  if ((raw & RCC_RSR_WWDGRSTF) != 0U)
  {
    return "wwdg";
  }
  if ((raw & RCC_RSR_SFTRSTF) != 0U)
  {
    return "software";
  }
  if ((raw & RCC_RSR_BORRSTF) != 0U)
  {
    return "brown-out";
  }
  if ((raw & RCC_RSR_LPWRRSTF) != 0U)
  {
    return "low-power";
  }
  if ((raw & RCC_RSR_PINRSTF) != 0U)
  {
    return "pin/power-on";
  }
  return "unknown";
}

const char *app_boot_reset_name(void)
{
  return boot_reset_name;
}

void app_fault_init(uint32_t reset_raw)
{
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                SCB_SHCSR_BUSFAULTENA_Msk |
                SCB_SHCSR_USGFAULTENA_Msk;
  __DSB();
  __ISB();

  boot_reset_name = app_reset_cause_name(reset_raw);

  if ((fault_record.magic == APP_FAULT_MAGIC) && (fault_record.valid == 1U))
  {
    last_fault = fault_record;
    have_last_fault = 1;
    fault_record.valid = 0U;
  }

  if ((fault_record.magic == APP_FAULT_MAGIC) &&
      (fault_record.sequence >= APP_FAULT_SAFE_THRESHOLD))
  {
    safe_mode = 1;
    app_log_printf("SAFE fault_count=%lu threshold=%u; control disabled, use 'fault clear'\r\n",
                   (unsigned long)fault_record.sequence,
                   (unsigned)APP_FAULT_SAFE_THRESHOLD);
  }

  app_log_printf("BOOT reset=%s fault=%s safe=%d profile=%s\r\n",
                 boot_reset_name,
                 (have_last_fault != 0) ? "yes" : "none",
                 safe_mode,
                 APP_PROFILE_NAME);

  if (have_last_fault != 0)
  {
    app_log_printf("FAULT seq=%lu cfsr=0x%08lx hfsr=0x%08lx mmfar=0x%08lx bfar=0x%08lx\r\n",
                   (unsigned long)last_fault.sequence,
                   (unsigned long)last_fault.cfsr,
                   (unsigned long)last_fault.hfsr,
                   (unsigned long)last_fault.mmfar,
                   (unsigned long)last_fault.bfar);
    app_log_printf("FAULT pc=0x%08lx lr=0x%08lx xpsr=0x%08lx sp=0x%08lx exc=0x%08lx\r\n",
                   (unsigned long)last_fault.pc,
                   (unsigned long)last_fault.lr,
                   (unsigned long)last_fault.xpsr,
                   (unsigned long)last_fault.sp,
                   (unsigned long)last_fault.exc_return);
    if (last_fault.guard_id < (uint32_t)APP_STK_COUNT)
    {
      app_log_printf("FAULT guard=%s\r\n",
                     app_stacks_name((app_stack_id_t)last_fault.guard_id));
    }
  }
}

int app_fault_safe_mode(void)
{
  return safe_mode;
}

void app_fault_clear(void)
{
  fault_record.magic = APP_FAULT_MAGIC;
  fault_record.sequence = 0U;
  fault_record.valid = 0U;
  last_fault = (app_fault_record_t){0};
  have_last_fault = 0;
  safe_mode = 0;
  app_log_printf("FAULT cleared\r\n");
}

int app_fault_get_last(app_fault_record_t *out)
{
  if ((out == NULL) || (have_last_fault == 0))
  {
    return 0;
  }
  *out = last_fault;
  return 1;
}

void app_fault_capture(uint32_t *sp, uint32_t exc_return)
{
  if (fault_record.magic != APP_FAULT_MAGIC)
  {
    fault_record.magic = APP_FAULT_MAGIC;
    fault_record.sequence = 0U;
    fault_record.valid = 0U;
  }

  uint32_t cfsr = SCB->CFSR;

  fault_record.sequence++;
  fault_record.cfsr = cfsr;
  fault_record.hfsr = SCB->HFSR;
  fault_record.mmfar = ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U) ? SCB->MMFAR : 0U;
  fault_record.bfar = ((cfsr & SCB_CFSR_BFARVALID_Msk) != 0U) ? SCB->BFAR : 0U;
  fault_record.guard_id = 0xFFFFFFFFU;
  if ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U)
  {
    int guard_id = app_stacks_guard_id(fault_record.mmfar);
    if (guard_id >= 0)
    {
      fault_record.guard_id = (uint32_t)guard_id;
    }
  }
  fault_record.r0 = sp[0];
  fault_record.r1 = sp[1];
  fault_record.r2 = sp[2];
  fault_record.r3 = sp[3];
  fault_record.r12 = sp[4];
  fault_record.lr = sp[5];
  fault_record.pc = sp[6];
  fault_record.xpsr = sp[7];
  fault_record.exc_return = exc_return;
  fault_record.sp = (uint32_t)sp;
  fault_record.valid = 1U;
  __DSB();

  NVIC_SystemReset();
  for (;;)
  {
  }
}

void HardFault_Handler(void) __attribute__((naked));
void HardFault_Handler(void)
{
  __asm volatile(
      "tst lr, #4        \n"
      "ite eq            \n"
      "mrseq r0, msp     \n"
      "mrsne r0, psp     \n"
      "mov r1, lr        \n"
      "b app_fault_capture\n");
}

void MemManage_Handler(void) __attribute__((naked));
void MemManage_Handler(void)
{
  __asm volatile(
      "tst lr, #4        \n"
      "ite eq            \n"
      "mrseq r0, msp     \n"
      "mrsne r0, psp     \n"
      "mov r1, lr        \n"
      "b app_fault_capture\n");
}

void BusFault_Handler(void) __attribute__((naked));
void BusFault_Handler(void)
{
  __asm volatile(
      "tst lr, #4        \n"
      "ite eq            \n"
      "mrseq r0, msp     \n"
      "mrsne r0, psp     \n"
      "mov r1, lr        \n"
      "b app_fault_capture\n");
}

void UsageFault_Handler(void) __attribute__((naked));
void UsageFault_Handler(void)
{
  __asm volatile(
      "tst lr, #4        \n"
      "ite eq            \n"
      "mrseq r0, msp     \n"
      "mrsne r0, psp     \n"
      "mov r1, lr        \n"
      "b app_fault_capture\n");
}
