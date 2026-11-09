#include "main.h"
#include "app_mpu.h"
#include "app_stacks.h"
#include "app_log.h"

#include "mpu_armv8.h"

#define APP_MPU_FLASH_BASE        0x08000000UL
#define APP_MPU_FLASH_LIMIT       0x081FFFFFUL
#define APP_MPU_RAM_BASE          0x20000000UL
#define APP_MPU_RAM_LIMIT         0x2009FFFFUL
#define APP_MPU_PERIPH_BASE       0x40000000UL
#define APP_MPU_PERIPH_LIMIT      0x5FFFFFFFUL
#define APP_MPU_GUARD_FIRST       3U
#define APP_MPU_ATTR_NORMAL       0U
#define APP_MPU_ATTR_DEVICE       1U

static uint32_t region_count;
static uint32_t guard_count;

void app_mpu_init(void)
{
  region_count = (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;

  ARM_MPU_SetMemAttr(APP_MPU_ATTR_NORMAL,
                     ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                  ARM_MPU_ATTR_NON_CACHEABLE));
  ARM_MPU_SetMemAttr(APP_MPU_ATTR_DEVICE,
                     ARM_MPU_ATTR(ARM_MPU_ATTR_DEVICE,
                                  ARM_MPU_ATTR_DEVICE_nGnRnE));

  /* Flash: read-only, executable. */
  ARM_MPU_SetRegion(0U,
                    ARM_MPU_RBAR(APP_MPU_FLASH_BASE, ARM_MPU_SH_NON, 1U, 0U, 0U),
                    ARM_MPU_RLAR(APP_MPU_FLASH_LIMIT, APP_MPU_ATTR_NORMAL));
  /* SRAM: read/write, execute-never. */
  ARM_MPU_SetRegion(1U,
                    ARM_MPU_RBAR(APP_MPU_RAM_BASE, ARM_MPU_SH_NON, 0U, 0U, 1U),
                    ARM_MPU_RLAR(APP_MPU_RAM_LIMIT, APP_MPU_ATTR_NORMAL));
  /* Peripherals: device memory, execute-never. */
  ARM_MPU_SetRegion(2U,
                    ARM_MPU_RBAR(APP_MPU_PERIPH_BASE, ARM_MPU_SH_NON, 0U, 0U, 1U),
                    ARM_MPU_RLAR(APP_MPU_PERIPH_LIMIT, APP_MPU_ATTR_DEVICE));

  guard_count = 0U;
  for (uint32_t id = 0U; id < (uint32_t)APP_STK_COUNT; id++)
  {
    uint32_t address = app_stacks_guard_addr((app_stack_id_t)id);
    uint32_t region = APP_MPU_GUARD_FIRST + guard_count;

    if ((address == 0U) || (region >= region_count))
    {
      break;
    }

    /* 32-byte guard: read-only (write faults), execute-never. */
    ARM_MPU_SetRegion(region,
                      ARM_MPU_RBAR(address, ARM_MPU_SH_NON, 1U, 0U, 1U),
                      ARM_MPU_RLAR(address + 31U, APP_MPU_ATTR_NORMAL));
    guard_count++;
  }

  ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);

  app_log_printf("MPU regions=%lu guards=%lu enabled\r\n",
                 (unsigned long)region_count,
                 (unsigned long)guard_count);
}

/*
 * The flash HAL programs a quad-word with plain CPU stores, which the
 * read-only flash region would reject with a MemManage fault. The caller keeps
 * interrupts masked around the operation, so the MPU is switched off only for
 * this short, single-threaded window and restored immediately afterwards.
 */
void app_mpu_param_write(int enable)
{
  if (enable != 0)
  {
    ARM_MPU_Disable();
  }
  else
  {
    ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
  }
  __DSB();
  __ISB();
}

uint32_t app_mpu_region_count(void)
{
  return region_count;
}

uint32_t app_mpu_guard_count(void)
{
  return guard_count;
}
