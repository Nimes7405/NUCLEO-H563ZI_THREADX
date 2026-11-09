#include "app_param.h"
#include "app_config.h"
#include "app_log.h"
#include "app_mpu.h"

#include "main.h"

#include <string.h>

/*
 * Calibration parameters live in the last 8 KB sector of Flash bank 2.
 * The application executes from bank 1, so erase/program does not stall
 * instruction fetch (read-while-write between banks).
 *
 * The sector is used as an append-only log of 16-byte records (one quad-word,
 * the H5 programming granularity). A torn write fails its CRC and is skipped,
 * so the previous record stays valid. The sector is erased only when full.
 */

#define APP_PARAM_MAGIC       0x304C4143UL /* "CAL0" */
#define APP_PARAM_ERASED      0xFFFFFFFFUL
#define APP_PARAM_SLOT_SIZE   16U
#define APP_PARAM_PAGE_SIZE   0x2000U
#define APP_PARAM_SLOT_COUNT  (APP_PARAM_PAGE_SIZE / APP_PARAM_SLOT_SIZE)
#define APP_PARAM_SECTOR      127U /* last sector of bank 1 */

extern uint8_t _param_page_start[];

typedef struct
{
  uint32_t magic;
  uint32_t offset_bits;
  uint32_t scale_bits;
  uint32_t crc;
} app_param_record_t;

typedef union
{
  app_param_record_t record;
  uint32_t align[4];
} app_param_slot_t;

static uint32_t param_base;
static uint32_t param_stored;
static uint32_t param_next_slot;
static uint32_t param_erases;
static uint32_t param_bank;      /* 0 = unknown, 1 or 2 once an erase worked */
static int param_flash_error;
static int param_scanned;

static float param_bits_to_float(uint32_t bits)
{
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t param_float_to_bits(float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint32_t param_crc32(const uint32_t *words, uint32_t count)
{
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0U; i < count; i++)
  {
    uint32_t word = words[i];
    for (uint32_t bit = 0U; bit < 32U; bit++)
    {
      if (((crc ^ word) & 1U) != 0U)
      {
        crc = (crc >> 1) ^ 0xEDB88320UL;
      }
      else
      {
        crc >>= 1;
      }
      word >>= 1;
    }
  }
  return ~crc;
}

static int param_record_valid(const app_param_record_t *record)
{
  float offset;
  float scale;

  if (record->magic != APP_PARAM_MAGIC)
  {
    return 0;
  }

  offset = param_bits_to_float(record->offset_bits);
  scale = param_bits_to_float(record->scale_bits);

  /* NaN-safe range checks: reject anything outside the usable range. */
  if (!((offset >= -4095.0f) && (offset <= 4095.0f)))
  {
    return 0;
  }
  if (!((scale >= 0.01f) && (scale <= 10.0f)))
  {
    return 0;
  }

  uint32_t words[3] = {record->magic, record->offset_bits, record->scale_bits};
  return (param_crc32(words, 3U) == record->crc) ? 1 : 0;
}

static int param_slot_erased(const app_param_record_t *record)
{
  return (record->magic == APP_PARAM_ERASED) &&
         (record->offset_bits == APP_PARAM_ERASED) &&
         (record->scale_bits == APP_PARAM_ERASED) &&
         (record->crc == APP_PARAM_ERASED);
}

static void param_scan(void)
{
  const app_param_record_t *page = (const app_param_record_t *)param_base;

  param_stored = 0U;
  param_next_slot = APP_PARAM_SLOT_COUNT;

  for (uint32_t i = 0U; i < APP_PARAM_SLOT_COUNT; i++)
  {
    if (param_slot_erased(&page[i]) != 0)
    {
      param_next_slot = i;
      break;
    }
    if (param_record_valid(&page[i]) != 0)
    {
      param_stored++;
    }
  }
  param_scanned = 1;
}

static void param_ensure_scanned(void)
{
  if (param_scanned == 0)
  {
    param_base = (uint32_t)_param_page_start;
    param_scan();
  }
}

static int param_erase_once(uint32_t bank, uint32_t *sector_error)
{
  FLASH_EraseInitTypeDef init = {0};
  HAL_StatusTypeDef status;

  init.TypeErase = FLASH_TYPEERASE_SECTORS;
  init.Banks = bank;
  init.Sector = APP_PARAM_SECTOR;
  init.NbSectors = 1U;

  HAL_FLASH_Unlock();
  status = HAL_FLASHEx_Erase(&init, sector_error);
  HAL_FLASH_Lock();

  return (status == HAL_OK) ? 0 : -1;
}

static int param_page_blank(void)
{
  const volatile uint32_t *page = (const volatile uint32_t *)param_base;

  for (uint32_t i = 0U; i < (APP_PARAM_PAGE_SIZE / 4U); i++)
  {
    if (page[i] != APP_PARAM_ERASED)
    {
      return 0;
    }
  }
  return 1;
}

/*
 * Bank selection depends on the option-byte geometry:
 *   dual bank  -> 8 KB sectors, page sits in bank 2 sector 127
 *   single bank-> 16 KB sectors, page sits in bank 1 sector 127
 * The HAL does not expose DBANK and a wrong BKSEL can still report success
 * without erasing anything, so the erase is verified by reading the page back.
 * The bank that actually worked is cached for later erases.
 */
static int param_erase_page(void)
{
  uint32_t sector_error = 0U;
  uint32_t order[2];
  int result = -1;

  order[0] = FLASH_BANK_1;
  order[1] = FLASH_BANK_2;

  for (uint32_t i = 0U; (i < 2U) && (result != 0); i++)
  {
    if (param_erase_once(order[i], &sector_error) == 0)
    {
      if (param_page_blank() != 0)
      {
        param_bank = (order[i] == FLASH_BANK_2) ? 2U : 1U;
        result = 0;
      }
    }
  }

  if (result != 0)
  {
    param_flash_error = 1;
    app_log_printf("PARAM erase failed sector=%lu err=0x%08lx\r\n",
                   (unsigned long)sector_error,
                   (unsigned long)HAL_FLASH_GetError());
    return -1;
  }

  param_erases++;
  param_stored = 0U;
  param_next_slot = 0U;
  return 0;
}

static const app_param_record_t *param_last_record(void)
{
  const app_param_record_t *page;
  const app_param_record_t *last = NULL;

  param_ensure_scanned();
  page = (const app_param_record_t *)param_base;
  for (uint32_t i = 0U; i < APP_PARAM_SLOT_COUNT; i++)
  {
    if (param_record_valid(&page[i]) != 0)
    {
      last = &page[i];
    }
  }
  return last;
}

int app_param_load(float *offset, float *scale)
{
  const app_param_record_t *last;

  param_base = (uint32_t)_param_page_start;
  param_scanned = 0;
  param_erases = 0U;
  param_flash_error = 0;
  param_scan();

  last = param_last_record();
  if (last == NULL)
  {
    return 0;
  }

  if (offset != NULL)
  {
    *offset = param_bits_to_float(last->offset_bits);
  }
  if (scale != NULL)
  {
    *scale = param_bits_to_float(last->scale_bits);
  }
  return 1;
}

int app_param_save(float offset, float scale)
{
  app_param_slot_t slot;
  uint32_t address;
  uint32_t words[3];
  HAL_StatusTypeDef status;
  uint32_t primask;

  param_ensure_scanned();

  /*
   * A sector erase blocks instruction fetch from bank 1, so keep the whole
   * sequence single threaded and report how long the system was paused.
   */
  uint32_t start_ms = HAL_GetTick();
  primask = __get_PRIMASK();
  __disable_irq();

  if (param_next_slot >= APP_PARAM_SLOT_COUNT)
  {
    if (param_erase_page() != 0)
    {
      __set_PRIMASK(primask);
      return -1;
    }
  }

  memset(&slot, 0, sizeof(slot));
  slot.record.magic = APP_PARAM_MAGIC;
  slot.record.offset_bits = param_float_to_bits(offset);
  slot.record.scale_bits = param_float_to_bits(scale);
  words[0] = slot.record.magic;
  words[1] = slot.record.offset_bits;
  words[2] = slot.record.scale_bits;
  slot.record.crc = param_crc32(words, 3U);

  address = param_base + (param_next_slot * APP_PARAM_SLOT_SIZE);

  app_mpu_param_write(1);
  HAL_FLASH_Unlock();
  status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, address, (uint32_t)&slot);
  HAL_FLASH_Lock();
  app_mpu_param_write(0);

  if (status != HAL_OK)
  {
    param_flash_error = 1;
    __set_PRIMASK(primask);
    app_log_printf("PARAM program failed addr=0x%08lx status=%d\r\n",
                   (unsigned long)address, (int)status);
    return -1;
  }

  if (memcmp((const void *)address, &slot, sizeof(slot)) != 0)
  {
    param_flash_error = 1;
    __set_PRIMASK(primask);
    app_log_printf("PARAM verify mismatch addr=0x%08lx\r\n", (unsigned long)address);
    return -1;
  }

  param_stored++;
  param_next_slot++;

  __set_PRIMASK(primask);

  /* No %f in the newlib-nano printf, so print the values scaled. */
  long off100 = (long)(offset * 100.0f);
  uint32_t scale1000 = (uint32_t)(scale * 1000.0f + 0.5f);
  app_log_printf("PARAM saved offset=%ld.%02ld scale=%lu.%03lu slot=%lu pause_ms=%lu\r\n",
                 off100 / 100, (off100 < 0) ? (-off100 % 100) : (off100 % 100),
                 (unsigned long)(scale1000 / 1000U), (unsigned long)(scale1000 % 1000U),
                 (unsigned long)(param_next_slot - 1U),
                 (unsigned long)(HAL_GetTick() - start_ms));
  return 0;
}

int app_param_erase(void)
{
  uint32_t primask;
  int result;

  param_ensure_scanned();
  primask = __get_PRIMASK();
  __disable_irq();
  result = param_erase_page();
  __set_PRIMASK(primask);

  return result;
}

void app_param_info(app_param_info_t *info)
{
  if (info == NULL)
  {
    return;
  }

  const app_param_record_t *last = param_last_record();

  info->offset = (last != NULL) ? param_bits_to_float(last->offset_bits) : 0.0f;
  info->scale = (last != NULL) ? param_bits_to_float(last->scale_bits) : 1.0f;
  info->stored = param_stored;
  info->next_slot = param_next_slot;
  info->slot_count = APP_PARAM_SLOT_COUNT;
  info->erases = param_erases;
  info->flash_error = param_flash_error;
}

void app_param_report(void)
{
  app_param_info_t info;
  app_param_info(&info);

  app_log_printf("PARAM addr=0x%08lx used=%lu/%lu slot=%lu erases=%lu bank=%lu err=%d\r\n",
                 (unsigned long)app_param_base(),
                 (unsigned long)info.stored,
                 (unsigned long)info.slot_count,
                 (unsigned long)info.next_slot,
                 (unsigned long)info.erases,
                 (unsigned long)param_bank,
                 info.flash_error);
}

uint32_t app_param_base(void)
{
  param_ensure_scanned();
  return param_base;
}
