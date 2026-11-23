#include "app_fxdisk.h"
#include "app_log.h"
#include "main.h"

#include "fx_api.h"
#include "lx_api.h"
#include "tx_api.h"

#include <string.h>

/*
 * LevelX NOR geometry. A block reserves one physical sector for metadata, so
 * usable logical sectors are BLOCKS * (SECTORS_PER_BLOCK - 1).
 */
#define SIM_WORDS_PER_SECTOR    (APP_FXDISK_SECTOR_BYTES / sizeof(ULONG))
#define SIM_WORDS_PER_BLOCK     (SIM_WORDS_PER_SECTOR * APP_FXDISK_SECTORS_PER_BLOCK)
#define SIM_TIMEOUT_MS          2000U

static ULONG sim_flash[APP_FXDISK_BLOCKS * SIM_WORDS_PER_BLOCK];
static ULONG sim_sector_buffer[SIM_WORDS_PER_SECTOR];
static ULONG sim_erase_count[APP_FXDISK_BLOCKS];
static ULONG sim_erase_ops;
static ULONG sim_bit_violations;
static ULONG sim_fx_read_ops;
static ULONG sim_fx_write_ops;
static ULONG sim_fx_sectors_written;
static UINT sim_flash_prepared;

static FX_FILE log_file;
static UINT log_file_open;

static LX_NOR_FLASH nor_flash;
static FX_MEDIA sim_media;
static UCHAR media_memory[1024];
static UINT disk_mounted;

/* Guards the FileX media against concurrent use: the acquisition writer thread
   logs while the console thread may run `fs` commands. */
static TX_MUTEX media_mutex;
static UINT media_mutex_ready;

static UINT sim_nor_read(ULONG *flash_address, ULONG *destination, ULONG words);
static UINT sim_nor_write(ULONG *flash_address, ULONG *source, ULONG words);
static UINT sim_nor_block_erase(ULONG block, ULONG erase_count);
static UINT sim_nor_block_erased_verify(ULONG block);
static UINT sim_nor_system_error(UINT error_code);
static UINT sim_nor_initialize(LX_NOR_FLASH *nor);
static VOID app_fxdisk_driver(FX_MEDIA *media_ptr);

static UINT sim_nor_read(ULONG *flash_address, ULONG *destination, ULONG words)
{
  while (words-- > 0U)
  {
    *destination++ = *flash_address++;
  }
  return LX_SUCCESS;
}

static UINT sim_nor_write(ULONG *flash_address, ULONG *source, ULONG words)
{
  while (words-- > 0U)
  {
    ULONG current = *flash_address;
    ULONG value = *source;

    /* NOR programming can only clear bits. Setting a bit back to 1 is a bug. */
    if ((value & (ULONG)~current) != 0U)
    {
      sim_bit_violations++;
      value &= current;
    }

    *flash_address++ = value;
    source++;
  }
  return LX_SUCCESS;
}

static UINT sim_nor_block_erase(ULONG block, ULONG erase_count)
{
  ULONG *pointer;
  ULONG words;

  if (block >= APP_FXDISK_BLOCKS)
  {
    return LX_ERROR;
  }

  pointer = &sim_flash[block * SIM_WORDS_PER_BLOCK];
  for (words = 0U; words < SIM_WORDS_PER_BLOCK; words++)
  {
    pointer[words] = (ULONG)0xFFFFFFFFU;
  }

  sim_erase_count[block] = erase_count;
  sim_erase_ops++;
  return LX_SUCCESS;
}

static UINT sim_nor_block_erased_verify(ULONG block)
{
  const ULONG *pointer;
  ULONG words;

  if (block >= APP_FXDISK_BLOCKS)
  {
    return LX_ERROR;
  }

  pointer = &sim_flash[block * SIM_WORDS_PER_BLOCK];
  for (words = 0U; words < SIM_WORDS_PER_BLOCK; words++)
  {
    if (pointer[words] != (ULONG)0xFFFFFFFFU)
    {
      return LX_ERROR;
    }
  }
  return LX_SUCCESS;
}

static UINT sim_nor_system_error(UINT error_code)
{
  app_log_printf("FSD err=levelx-%u\r\n", (unsigned)error_code);
  return LX_SUCCESS;
}

static UINT sim_nor_initialize(LX_NOR_FLASH *nor)
{
  if (sim_flash_prepared == 0U)
  {
    /* Power-on content of a fresh NOR device is erased (all ones). */
    for (ULONG i = 0U; i < (ULONG)(APP_FXDISK_BLOCKS * SIM_WORDS_PER_BLOCK); i++)
    {
      sim_flash[i] = (ULONG)0xFFFFFFFFU;
    }
    sim_flash_prepared = 1U;
  }

  nor->lx_nor_flash_base_address = &sim_flash[0];
  nor->lx_nor_flash_total_blocks = APP_FXDISK_BLOCKS;
  nor->lx_nor_flash_words_per_block = SIM_WORDS_PER_BLOCK;
  nor->lx_nor_flash_driver_read = sim_nor_read;
  nor->lx_nor_flash_driver_write = sim_nor_write;
  nor->lx_nor_flash_driver_block_erase = sim_nor_block_erase;
  nor->lx_nor_flash_driver_block_erased_verify = sim_nor_block_erased_verify;
  nor->lx_nor_flash_driver_system_error = sim_nor_system_error;
  nor->lx_nor_flash_sector_buffer = &sim_sector_buffer[0];

  return LX_SUCCESS;
}

static VOID app_fxdisk_driver(FX_MEDIA *media_ptr)
{
  UINT status;
  UCHAR *buffer;
  ULONG sector;
  UINT i;

  switch (media_ptr->fx_media_driver_request)
  {
    case FX_DRIVER_READ:
      sim_fx_read_ops++;
      sector = media_ptr->fx_media_driver_logical_sector;
      buffer = (UCHAR *)media_ptr->fx_media_driver_buffer;
      for (i = 0U; i < media_ptr->fx_media_driver_sectors; i++)
      {
        status = lx_nor_flash_sector_read(&nor_flash, sector, buffer);
        if (status != LX_SUCCESS)
        {
          media_ptr->fx_media_driver_status = FX_IO_ERROR;
          return;
        }
        sector++;
        buffer += media_ptr->fx_media_bytes_per_sector;
      }
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_WRITE:
      sim_fx_write_ops++;
      sim_fx_sectors_written += media_ptr->fx_media_driver_sectors;
      sector = media_ptr->fx_media_driver_logical_sector;
      buffer = (UCHAR *)media_ptr->fx_media_driver_buffer;
      for (i = 0U; i < media_ptr->fx_media_driver_sectors; i++)
      {
        status = lx_nor_flash_sector_write(&nor_flash, sector, buffer);
        if (status != LX_SUCCESS)
        {
          media_ptr->fx_media_driver_status = FX_IO_ERROR;
          return;
        }
        sector++;
        buffer += media_ptr->fx_media_bytes_per_sector;
      }
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_RELEASE_SECTORS:
      sector = media_ptr->fx_media_driver_logical_sector;
      for (i = 0U; i < media_ptr->fx_media_driver_sectors; i++)
      {
        (void)lx_nor_flash_sector_release(&nor_flash, sector);
        sector++;
      }
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_FLUSH:
    case FX_DRIVER_ABORT:
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_INIT:
      media_ptr->fx_media_driver_free_sector_update = FX_TRUE;
      media_ptr->fx_media_driver_write_protect = FX_FALSE;
      status = lx_nor_flash_open(&nor_flash, "sim-nor", sim_nor_initialize);
      media_ptr->fx_media_driver_status = (status == LX_SUCCESS) ? FX_SUCCESS : FX_IO_ERROR;
      break;

    case FX_DRIVER_UNINIT:
      status = lx_nor_flash_close(&nor_flash);
      media_ptr->fx_media_driver_status = (status == LX_SUCCESS) ? FX_SUCCESS : FX_IO_ERROR;
      break;

    case FX_DRIVER_BOOT_READ:
      buffer = (UCHAR *)media_ptr->fx_media_driver_buffer;
      status = lx_nor_flash_sector_read(&nor_flash, 0U, buffer);
      if (status != LX_SUCCESS)
      {
        media_ptr->fx_media_driver_status = FX_IO_ERROR;
        return;
      }
      if ((buffer[0] != (UCHAR)0xEB) || (buffer[1] != (UCHAR)0x34) || (buffer[2] != (UCHAR)0x90))
      {
        media_ptr->fx_media_driver_status = FX_MEDIA_INVALID;
        return;
      }
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_BOOT_WRITE:
      buffer = (UCHAR *)media_ptr->fx_media_driver_buffer;
      status = lx_nor_flash_sector_write(&nor_flash, 0U, buffer);
      media_ptr->fx_media_driver_status = (status == LX_SUCCESS) ? FX_SUCCESS : FX_IO_ERROR;
      break;

    default:
      media_ptr->fx_media_driver_status = FX_IO_ERROR;
      break;
  }
}

int app_fxdisk_format(void)
{
  UINT status;

  (void)app_fxdisk_unmount();

  status = fx_media_format(&sim_media,
                           app_fxdisk_driver,
                           FX_NULL,
                           media_memory,
                           (UINT)sizeof(media_memory),
                           "SIMDISK",
                           1U,
                           64U,
                           0U,
                           APP_FXDISK_TOTAL_SECTORS,
                           APP_FXDISK_SECTOR_BYTES,
                           1U,
                           1U,
                           1U);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  return app_fxdisk_mount();
}

int app_fxdisk_mount(void)
{
  UINT status;

  if (disk_mounted != 0U)
  {
    return 0;
  }

  status = fx_media_open(&sim_media, "SIMDISK", app_fxdisk_driver, FX_NULL,
                         media_memory, (ULONG)sizeof(media_memory));
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  disk_mounted = 1U;
  return 0;
}

int app_fxdisk_unmount(void)
{
  if (disk_mounted == 0U)
  {
    return 0;
  }

  if (fx_media_close(&sim_media) != FX_SUCCESS)
  {
    return -1;
  }

  disk_mounted = 0U;
  return 0;
}

int app_fxdisk_ready(void)
{
  return (disk_mounted != 0U) ? 0 : -1;
}

void app_fxdisk_system_init(void)
{
  /* Creates the FileX system timer; must run in kernel context. */
  fx_system_initialize();

  if (tx_mutex_create(&media_mutex, "fxdisk", TX_INHERIT) != TX_SUCCESS)
  {
    Error_Handler();
  }
  media_mutex_ready = 1U;
}

int app_fxdisk_lock(void)
{
  if (media_mutex_ready == 0U)
  {
    return -1;
  }
  return (tx_mutex_get(&media_mutex, TX_WAIT_FOREVER) == TX_SUCCESS) ? 0 : -1;
}

int app_fxdisk_unlock(void)
{
  if (media_mutex_ready == 0U)
  {
    return -1;
  }
  return (tx_mutex_put(&media_mutex) == TX_SUCCESS) ? 0 : -1;
}

void app_fxdisk_init(void)
{
  UINT status;

  memset(&sim_media, 0, sizeof(sim_media));

  status = (UINT)app_fxdisk_mount();
  if (status == FX_SUCCESS)
  {
    app_log_printf("FSD mount=existing sectors=%u\r\n", (unsigned)APP_FXDISK_TOTAL_SECTORS);
    return;
  }

  /* A volatile RAM disk starts blank, so format once and mount again. */
  if (app_fxdisk_format() == 0)
  {
    app_log_printf("FSD mount=formatted sectors=%u reason=%u\r\n",
                   (unsigned)APP_FXDISK_TOTAL_SECTORS, (unsigned)status);
    return;
  }

  app_log_printf("FSD mount=FAILED status=%u\r\n", (unsigned)status);
}

void app_fxdisk_report(void)
{
  ULONG available = 0UL;
  UINT status;

  if (disk_mounted == 0U)
  {
    app_log_printf("FSD mounted=0\r\n");
    return;
  }

  status = fx_media_space_available(&sim_media, &available);
  app_log_printf("FSD mounted=1 media=%s sectors=%u sector_bytes=%u free_bytes=%lu status=%u\r\n",
                 sim_media.fx_media_name,
                 (unsigned)APP_FXDISK_TOTAL_SECTORS,
                 (unsigned)APP_FXDISK_SECTOR_BYTES,
                 (unsigned long)available,
                 (unsigned)status);
  app_log_printf("FSD nor blocks=%u blk_sectors=%u erases=%lu violations=%lu",
                 (unsigned)APP_FXDISK_BLOCKS,
                 (unsigned)APP_FXDISK_SECTORS_PER_BLOCK,
                 (unsigned long)sim_erase_ops,
                 (unsigned long)sim_bit_violations);
  for (ULONG i = 0U; i < APP_FXDISK_BLOCKS; i++)
  {
    app_log_printf(" e%lu=%lu", (unsigned long)i, (unsigned long)sim_erase_count[i]);
  }
  app_log_printf("\r\n");
  app_log_printf("FSD drv reads=%lu writes=%lu sectors=%lu\r\n",
                 (unsigned long)sim_fx_read_ops,
                 (unsigned long)sim_fx_write_ops,
                 (unsigned long)sim_fx_sectors_written);
  app_log_printf("FSD lx alloc=%lu erased=%lu format=%lu obsolete=%lu free=%lu mapped=%lu\r\n",
                 (unsigned long)nor_flash.lx_nor_flash_physical_block_allocates,
                 (unsigned long)nor_flash.lx_nor_flash_diagnostic_erased_block,
                 (unsigned long)nor_flash.lx_nor_flash_diagnostic_initial_format,
                 (unsigned long)nor_flash.lx_nor_flash_obsolete_physical_sectors,
                 (unsigned long)nor_flash.lx_nor_flash_free_physical_sectors,
                 (unsigned long)nor_flash.lx_nor_flash_mapped_physical_sectors);
}

int app_fxdisk_list(void)
{
  CHAR name[FX_MAX_LONG_NAME_LEN];
  UINT status;
  UINT count = 0U;

  if (disk_mounted == 0U)
  {
    return -1;
  }

  status = fx_directory_first_entry_find(&sim_media, name);
  while (status == FX_SUCCESS)
  {
    if ((name[0] != '.') || (name[1] != '\0'))
    {
      UINT attributes = 0U;
      ULONG size = 0UL;
      UINT year = 0U;
      UINT month = 0U;
      UINT day = 0U;
      UINT hour = 0U;
      UINT minute = 0U;
      UINT second = 0U;
      UINT info = fx_directory_information_get(&sim_media, name, &attributes, &size,
                                               &year, &month, &day, &hour, &minute, &second);
      app_log_printf("FS f=%s size=%lu%s\r\n", name, (unsigned long)size,
                     (info == FX_SUCCESS && (attributes & FX_DIRECTORY) != 0U) ? " dir=1" : "");
      count++;
    }
    status = fx_directory_next_entry_find(&sim_media, name);
  }

  return (int)count;
}

int app_fxdisk_write(const char *name, const void *data, uint32_t len, int append)
{
  FX_FILE file;
  UINT status;

  if (disk_mounted == 0U)
  {
    return -1;
  }

  status = fx_file_create(&sim_media, (CHAR *)name);
  if ((status != FX_SUCCESS) && (status != FX_ALREADY_CREATED))
  {
    return (int)status;
  }

  status = fx_file_open(&sim_media, &file, (CHAR *)name, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  if (append == 0)
  {
    (void)fx_file_truncate(&file, 0UL);
  }

  status = fx_file_write(&file, (VOID *)data, (ULONG)len);
  (void)fx_file_close(&file);
  (void)fx_media_flush(&sim_media);
  return (status == FX_SUCCESS) ? 0 : (int)status;
}

int app_fxdisk_read(const char *name, char *out, uint32_t max, uint32_t *out_len)
{
  FX_FILE file;
  UINT status;
  ULONG actual = 0UL;

  if (disk_mounted == 0U)
  {
    return -1;
  }

  status = fx_file_open(&sim_media, &file, (CHAR *)name, FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  status = fx_file_read(&file, out, (ULONG)(max - 1U), &actual);
  (void)fx_file_close(&file);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  out[actual] = '\0';
  if (out_len != NULL)
  {
    *out_len = (uint32_t)actual;
  }
  return 0;
}

int app_fxdisk_delete(const char *name)
{
  if (disk_mounted == 0U)
  {
    return -1;
  }
  return (int)fx_file_delete(&sim_media, (CHAR *)name);
}

uint32_t app_fxdisk_bit_violations(void)
{
  return (uint32_t)sim_bit_violations;
}

static UCHAR pattern_byte(uint32_t offset, uint32_t seed)
{
  return (UCHAR)((offset * 31U) + seed);
}

int app_fxdisk_write_pattern(const char *name, uint32_t len, uint32_t seed)
{
  static UCHAR chunk[256];
  FX_FILE file;
  UINT status;
  uint32_t done = 0U;

  if (disk_mounted == 0U)
  {
    return -1;
  }

  (void)fx_file_create(&sim_media, (CHAR *)name);
  status = fx_file_open(&sim_media, &file, (CHAR *)name, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  (void)fx_file_truncate(&file, 0UL);

  while ((done < len) && (status == FX_SUCCESS))
  {
    uint32_t count = len - done;
    if (count > sizeof(chunk))
    {
      count = sizeof(chunk);
    }
    for (uint32_t i = 0U; i < count; i++)
    {
      chunk[i] = pattern_byte(done + i, seed);
    }
    status = fx_file_write(&file, chunk, (ULONG)count);
    done += count;
  }

  (void)fx_file_close(&file);
  (void)fx_media_flush(&sim_media);

  if ((status != FX_SUCCESS) || (done != len))
  {
    return (int)status;
  }
  return 0;
}

int app_fxdisk_verify_pattern(const char *name, uint32_t len, uint32_t seed)
{
  static UCHAR chunk[256];
  FX_FILE file;
  UINT status;
  ULONG actual = 0UL;
  uint32_t offset = 0U;

  if (disk_mounted == 0U)
  {
    return -1;
  }

  status = fx_file_open(&sim_media, &file, (CHAR *)name, FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  do
  {
    status = fx_file_read(&file, chunk, (ULONG)sizeof(chunk), &actual);
    if (status != FX_SUCCESS)
    {
      break;
    }
    for (ULONG i = 0UL; i < actual; i++)
    {
      if (chunk[i] != pattern_byte(offset + (uint32_t)i, seed))
      {
        (void)fx_file_close(&file);
        return -2;
      }
    }
    offset += (uint32_t)actual;
  } while (actual > 0UL);

  (void)fx_file_close(&file);
  return (offset == len) ? 0 : -3;
}

int app_fxdisk_log_open(const char *name)
{
  UINT status;

  if (disk_mounted == 0U)
  {
    return -1;
  }
  (void)app_fxdisk_log_close();

  (void)fx_file_create(&sim_media, (CHAR *)name);
  status = fx_file_open(&sim_media, &log_file, (CHAR *)name, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  log_file_open = 1U;
  (void)fx_file_truncate(&log_file, 0UL);
  return 0;
}

int app_fxdisk_log_write(const void *data, uint32_t len)
{
  if (log_file_open == 0U)
  {
    return -1;
  }
  return (fx_file_write(&log_file, (VOID *)data, (ULONG)len) == FX_SUCCESS) ? 0 : -2;
}

int app_fxdisk_log_close(void)
{
  if (log_file_open == 0U)
  {
    return 0;
  }

  (void)fx_file_close(&log_file);
  (void)fx_media_flush(&sim_media);
  log_file_open = 0U;
  return 0;
}

/* Push cached sectors to the medium without closing the file, so a long-lived
   log handle can flush in batches instead of once per line. */
int app_fxdisk_log_flush(void)
{
  if (log_file_open == 0U)
  {
    return -1;
  }
  return (fx_media_flush(&sim_media) == FX_SUCCESS) ? 0 : -2;
}

int app_fxdisk_log_size(uint32_t *size)
{
  if ((log_file_open == 0U) || (size == NULL))
  {
    return -1;
  }
  *size = (uint32_t)log_file.fx_file_current_file_size;
  return 0;
}

int app_fxdisk_read_stream(const char *name, app_fxdisk_chunk_fn fn, void *ctx, uint32_t *total)
{
  static UCHAR chunk[256];
  FX_FILE file;
  UINT status;
  ULONG actual = 0UL;
  uint32_t sum = 0U;

  if (disk_mounted == 0U)
  {
    return -1;
  }

  status = fx_file_open(&sim_media, &file, (CHAR *)name, FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  do
  {
    status = fx_file_read(&file, chunk, (ULONG)sizeof(chunk), &actual);
    if ((status != FX_SUCCESS) || (actual == 0UL))
    {
      break;
    }
    if (fn != NULL)
    {
      if (fn((const char *)chunk, (uint32_t)actual, ctx) != 0)
      {
        (void)fx_file_close(&file);
        return -2;
      }
    }
    sum += (uint32_t)actual;
  } while (actual > 0UL);

  (void)fx_file_close(&file);
  if (total != NULL)
  {
    *total = sum;
  }
  if ((status == FX_SUCCESS) || (status == FX_END_OF_FILE))
  {
    return 0;
  }
  return (int)status;
}
