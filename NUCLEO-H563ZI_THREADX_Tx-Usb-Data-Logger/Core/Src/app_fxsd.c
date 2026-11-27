#include "app_fxsd.h"
#include "app_config.h"
#include "app_log.h"
#include "app_sdspi.h"

#include "fx_api.h"
#include "tx_api.h"

#include <string.h>

static FX_MEDIA sd_media;
static FX_FILE sd_log_file;
static uint32_t sd_log_open;
static uint8_t sd_media_memory[1024];
static TX_MUTEX sd_mutex;
static uint32_t sd_mounted;
static uint32_t sd_format_sectors_last;
static uint32_t sd_part_lba;
static uint32_t sd_formatting;
static uint32_t sd_fx_reads;
static uint32_t sd_fx_writes;
static uint32_t sd_fx_sectors;

void app_fxsd_system_init(void)
{
  if (tx_mutex_create(&sd_mutex, "sd-mutex", TX_INHERIT) != TX_SUCCESS)
  {
    Error_Handler();
  }
}

void app_fxsd_init(void)
{
  (void)app_sdspi_init();
}

int app_fxsd_lock(void)
{
  return (tx_mutex_get(&sd_mutex, TX_WAIT_FOREVER) == TX_SUCCESS) ? 0 : -1;
}

int app_fxsd_lock_timeout(uint32_t ticks)
{
  return (tx_mutex_get(&sd_mutex, (ULONG)ticks) == TX_SUCCESS) ? 0 : -1;
}

int app_fxsd_unlock(void)
{
  return (tx_mutex_put(&sd_mutex) == TX_SUCCESS) ? 0 : -1;
}

/*
 * Cards written by a PC usually start with an MBR, while FileX expects the FAT
 * boot sector at logical sector zero. Look at sector zero once and remember the
 * partition offset so the volume can be mounted without touching the card.
 * Returns 0 when a FAT boot sector is already at sector zero.
 */
static uint32_t sd_probe_partition_lba(void)
{
  static uint8_t boot[APP_SD_BLOCK_SIZE];
  uint32_t offset = 0U;

  if (app_sdspi_read_block(0U, boot) != 0)
  {
    return 0U;
  }

  /* A real boot sector carries a plausible bytes-per-sector field and a FAT id. */
  {
    uint32_t bps = (uint32_t)boot[11] | ((uint32_t)boot[12] << 8);
    int is_boot = ((bps == 512U) || (bps == 1024U) || (bps == 2048U) || (bps == 4096U));

    if (is_boot != 0)
    {
      return 0U;
    }
  }

  if ((boot[510] == 0x55U) && (boot[511] == 0xAAU))
  {
    for (uint32_t i = 0U; i < 4U; i++)
    {
      const uint8_t *entry = &boot[446U + (i * 16U)];
      uint32_t type = (uint32_t)entry[4];
      uint32_t lba = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                     ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);

      if ((type == 0U) || (lba == 0U))
      {
        continue;
      }
      if ((type == 0x01U) || (type == 0x04U) || (type == 0x05U) || (type == 0x06U) ||
          (type == 0x0BU) || (type == 0x0CU) || (type == 0x0EU) || (type == 0x07U) ||
          (type == 0x83U) || (type == 0xEEU))
      {
        offset = lba;
        break;
      }
    }
  }

  return offset;
}

/* FileX asks for one or more consecutive sectors; the card driver moves one at
   a time, so the request is unrolled here. */
static UINT sd_driver_transfer(FX_MEDIA *media_ptr, int write)
{
  uint32_t sector = (uint32_t)media_ptr->fx_media_driver_logical_sector + sd_part_lba;
  uint8_t *buffer = (uint8_t *)media_ptr->fx_media_driver_buffer;
  uint32_t count = (uint32_t)media_ptr->fx_media_driver_sectors;
  uint32_t bytes = (uint32_t)media_ptr->fx_media_bytes_per_sector;
  int rc = 0;

  for (uint32_t i = 0U; (i < count) && (rc == 0); i++)
  {
    if (bytes == APP_SD_BLOCK_SIZE)
    {
      rc = (write != 0) ? app_sdspi_write_block(sector, buffer)
                        : app_sdspi_read_block(sector, buffer);
    }
    else
    {
      /* The card only speaks 512-byte blocks. */
      rc = -1;
    }
    sector++;
    buffer += bytes;
  }

  if (rc == 0)
  {
    if (write != 0)
    {
      sd_fx_writes++;
      sd_fx_sectors += count;
    }
    else
    {
      sd_fx_reads++;
    }
    return FX_SUCCESS;
  }
  return FX_IO_ERROR;
}

static VOID app_fxsd_driver(FX_MEDIA *media_ptr)
{
  switch (media_ptr->fx_media_driver_request)
  {
    case FX_DRIVER_READ:
      media_ptr->fx_media_driver_status = sd_driver_transfer(media_ptr, 0);
      break;

    case FX_DRIVER_WRITE:
      media_ptr->fx_media_driver_status = sd_driver_transfer(media_ptr, 1);
      break;

    /* The card's own controller handles grouping and wear, and SPI mode has no
       erase step, so these are accepted and ignored. */
    case FX_DRIVER_RELEASE_SECTORS:
    case FX_DRIVER_FLUSH:
    case FX_DRIVER_ABORT:
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_INIT:
      /* The card controller owns the free-sector bookkeeping; FileX has to
         maintain its own counters, so this stays FX_FALSE. */
      media_ptr->fx_media_driver_free_sector_update = FX_FALSE;
      media_ptr->fx_media_driver_write_protect = FX_FALSE;
      if (app_sdspi_ready() == 0)
      {
        media_ptr->fx_media_driver_status = FX_IO_ERROR;
        break;
      }
      /* While formatting, the volume is a superfloppy at sector zero: probing
         would still see the old MBR and shift every sector by its offset. */
      sd_part_lba = (sd_formatting != 0U) ? 0U : sd_probe_partition_lba();
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_UNINIT:
      media_ptr->fx_media_driver_status = FX_SUCCESS;
      break;

    case FX_DRIVER_BOOT_READ:
    case FX_DRIVER_BOOT_WRITE:
    {
      uint32_t lba = (media_ptr->fx_media_driver_request == FX_DRIVER_BOOT_WRITE)
                         ? 0U : sd_part_lba;
      int rc = (media_ptr->fx_media_driver_request == FX_DRIVER_BOOT_WRITE)
                   ? app_sdspi_write_block(lba, (uint8_t *)media_ptr->fx_media_driver_buffer)
                   : app_sdspi_read_block(lba, (uint8_t *)media_ptr->fx_media_driver_buffer);

      media_ptr->fx_media_driver_status = (rc == 0) ? FX_SUCCESS : FX_IO_ERROR;
      break;
    }

    default:
      media_ptr->fx_media_driver_status = FX_IO_ERROR;
      break;
  }
}

/* Same contract as app_fxdisk_ready(): 0 means "usable". */
int app_fxsd_mounted_ready(void)
{
  return (sd_mounted != 0U) ? 0 : -1;
}

int app_fxsd_is_mounted(void)
{
  return (sd_mounted != 0U) ? 1 : 0;
}

int app_fxsd_mount(void)
{
  UINT status;

  if (sd_mounted != 0U)
  {
    return 0;
  }

  if (app_sdspi_ready() == 0)
  {
    int rc = app_sdspi_init();

    if (rc != 0)
    {
      return rc;
    }
  }

  status = fx_media_open(&sd_media, "SDCARD", app_fxsd_driver, FX_NULL,
                         sd_media_memory, (ULONG)sizeof(sd_media_memory));
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  sd_mounted = 1U;
  return 0;
}

int app_fxsd_unmount(void)
{
  UINT status;

  if (sd_mounted == 0U)
  {
    return 0;
  }

  /* The volume is forgotten even if the close fails: keeping the "mounted"
     flag would make a later mount reuse a stale volume after a card swap. */
  status = fx_media_close(&sd_media);
  sd_mounted = 0U;
  return (status == FX_SUCCESS) ? 0 : (int)status;
}

int app_fxsd_format(uint32_t sectors)
{
  UINT status;

  if ((sectors == 0U) || (sectors > app_sdspi_blocks()))
  {
    return -1;
  }

  (void)app_fxsd_unmount();
  sd_part_lba = 0U;
  sd_formatting = 1U;

  /* 8 sectors per cluster (4 KiB), 512 root entries: the FAT stays small, so a
     128 MB test volume formats in well under a second over SPI. */
  status = fx_media_format(&sd_media,
                           app_fxsd_driver,
                           FX_NULL,
                           sd_media_memory,
                           (UINT)sizeof(sd_media_memory),
                           "SDCARD",
                           1U,
                           512U,
                           0U,
                           sectors,
                           APP_SD_BLOCK_SIZE,
                           8U,
                           1U,
                           1U);
  sd_formatting = 0U;
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }
  sd_format_sectors_last = sectors;

  return app_fxsd_mount();
}

void app_fxsd_report(void)
{
  ULONG available = 0UL;
  ULONG64 available64 = 0ULL;
  UINT status;

  app_log_printf("SD fs mounted=%lu part_lba=%lu total_sectors=%lu format_sectors=%lu\r\n",
                 (unsigned long)sd_mounted, (unsigned long)sd_part_lba,
                 (unsigned long)sd_media.fx_media_total_sectors,
                 (unsigned long)sd_format_sectors_last);

  if (sd_mounted == 0U)
  {
    return;
  }

  /* A 32 GB volume exceeds ULONG, so the 32-bit call alone saturates at
     0xFFFFFFFF; the 64-bit variant reports the real remaining space. */
  status = fx_media_space_available(&sd_media, &available);
  (void)fx_media_extended_space_available(&sd_media, &available64);
  app_log_printf("SD fs free_mb=%lu status=%u bytes_per_sector=%lu spc=%lu\r\n",
                 (unsigned long)(available64 / (1024ULL * 1024ULL)), (unsigned)status,
                 (unsigned long)sd_media.fx_media_bytes_per_sector,
                 (unsigned long)sd_media.fx_media_sectors_per_cluster);
  app_log_printf("SD fs drv reads=%lu writes=%lu sectors_written=%lu\r\n",
                 (unsigned long)sd_fx_reads, (unsigned long)sd_fx_writes,
                 (unsigned long)sd_fx_sectors);
}

int app_fxsd_list(void)
{
  CHAR name[FX_MAX_LONG_NAME_LEN];
  UINT status;
  int count = 0;

  if (sd_mounted == 0U)
  {
    return -1;
  }

  status = fx_directory_first_entry_find(&sd_media, name);
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

      (void)fx_directory_information_get(&sd_media, name, &attributes, &size,
                                         &year, &month, &day, &hour, &minute, &second);
      app_log_printf("SD f=%s size=%lu%s\r\n", name, (unsigned long)size,
                     ((attributes & FX_DIRECTORY) != 0U) ? " dir=1" : "");
      count++;
    }
    status = fx_directory_next_entry_find(&sd_media, name);
  }

  return count;
}

int app_fxsd_write_file(const char *name, const char *text, int append)
{
  FX_FILE file;
  UINT status;

  if ((sd_mounted == 0U) || (name == NULL) || (text == NULL))
  {
    return -1;
  }

  status = fx_file_create(&sd_media, (CHAR *)name);
  if ((status != FX_SUCCESS) && (status != FX_ALREADY_CREATED))
  {
    return (int)status;
  }

  status = fx_file_open(&sd_media, &file, (CHAR *)name, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  if (append == 0)
  {
    (void)fx_file_seek(&file, 0UL);
    (void)fx_file_truncate(&file, 0UL);
  }
  else
  {
    (void)fx_file_seek(&file, file.fx_file_current_file_size);
  }

  status = fx_file_write(&file, (VOID *)text, (ULONG)strlen(text));
  (void)fx_file_close(&file);
  /* The directory entry (including the file size) is still only in the media
     cache at this point; without the flush a power cycle shows size 0. */
  (void)fx_media_flush(&sd_media);
  return (status == FX_SUCCESS) ? 0 : (int)status;
}

/* Offset variant: reading a long file from the console needs windows, and a
   500 byte cap makes an 80 KB log useless for a trend. */
int app_fxsd_read_at(const char *name, uint32_t offset, char *out, uint32_t max,
                     uint32_t *out_len)
{
  FX_FILE file;
  UINT status;
  ULONG got = 0UL;

  if ((sd_mounted == 0U) || (name == NULL) || (out == NULL) || (max == 0U))
  {
    return -1;
  }

  status = fx_file_open(&sd_media, &file, (CHAR *)name, FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  status = fx_file_seek(&file, (ULONG)offset);
  if (status == FX_SUCCESS)
  {
    status = fx_file_read(&file, out, (ULONG)(max - 1U), &got);
  }
  (void)fx_file_close(&file);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  out[got] = '\0';
  if (out_len != NULL)
  {
    *out_len = (uint32_t)got;
  }
  return 0;
}

int app_fxsd_read_file(const char *name, char *out, uint32_t max, uint32_t *out_len)
{
  FX_FILE file;
  UINT status;
  ULONG got = 0UL;

  if ((sd_mounted == 0U) || (name == NULL) || (out == NULL) || (max == 0U))
  {
    return -1;
  }

  status = fx_file_open(&sd_media, &file, (CHAR *)name, FX_OPEN_FOR_READ);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  status = fx_file_read(&file, out, (ULONG)(max - 1U), &got);
  (void)fx_file_close(&file);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  out[got] = '\0';
  if (out_len != NULL)
  {
    *out_len = (uint32_t)got;
  }
  return 0;
}

/* ---- sequential log interface, mirroring app_fxdisk's ---- */

int app_fxsd_log_open(const char *name)
{
  UINT status;

  if ((sd_mounted == 0U) || (name == NULL))
  {
    return -1;
  }
  (void)app_fxsd_log_close();

  status = fx_file_create(&sd_media, (CHAR *)name);
  if ((status != FX_SUCCESS) && (status != FX_ALREADY_CREATED))
  {
    return (int)status;
  }

  status = fx_file_open(&sd_media, &sd_log_file, (CHAR *)name, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  sd_log_open = 1U;
  (void)fx_file_truncate(&sd_log_file, 0UL);
  return 0;
}

int app_fxsd_log_write(const void *data, uint32_t len)
{
  if (sd_log_open == 0U)
  {
    return -1;
  }
  return (fx_file_write(&sd_log_file, (VOID *)data, (ULONG)len) == FX_SUCCESS) ? 0 : -2;
}

int app_fxsd_log_close(void)
{
  if (sd_log_open == 0U)
  {
    return 0;
  }

  (void)fx_file_close(&sd_log_file);
  (void)fx_media_flush(&sd_media);
  sd_log_open = 0U;
  return 0;
}

int app_fxsd_log_flush(void)
{
  if (sd_log_open == 0U)
  {
    return -1;
  }
  return (fx_media_flush(&sd_media) == FX_SUCCESS) ? 0 : -2;
}

int app_fxsd_log_size(uint32_t *size)
{
  if ((sd_log_open == 0U) || (size == NULL))
  {
    return -1;
  }
  *size = (uint32_t)sd_log_file.fx_file_current_file_size;
  return 0;
}

int app_fxsd_read_stream(const char *name, app_fxsd_chunk_fn fn, void *ctx,
                         uint32_t *total)
{
  static UCHAR chunk[256];
  FX_FILE file;
  UINT status;
  ULONG actual = 0UL;
  uint32_t sum = 0U;

  if ((sd_mounted == 0U) || (name == NULL))
  {
    return -1;
  }

  status = fx_file_open(&sd_media, &file, (CHAR *)name, FX_OPEN_FOR_READ);
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

int app_fxsd_append_lazy(const char *name, const char *text)
{
  FX_FILE file;
  UINT status;

  if ((sd_mounted == 0U) || (name == NULL) || (text == NULL))
  {
    return -1;
  }

  status = fx_file_create(&sd_media, (CHAR *)name);
  if ((status != FX_SUCCESS) && (status != FX_ALREADY_CREATED))
  {
    return (int)status;
  }

  status = fx_file_open(&sd_media, &file, (CHAR *)name, FX_OPEN_FOR_WRITE);
  if (status != FX_SUCCESS)
  {
    return (int)status;
  }

  (void)fx_file_seek(&file, file.fx_file_current_file_size);
  status = fx_file_write(&file, (VOID *)text, (ULONG)strlen(text));
  (void)fx_file_close(&file);
  return (status == FX_SUCCESS) ? 0 : (int)status;
}

int app_fxsd_delete(const char *name)
{
  if ((sd_mounted == 0U) || (name == NULL))
  {
    return -1;
  }
  if (fx_file_delete(&sd_media, (CHAR *)name) != FX_SUCCESS)
  {
    return -1;
  }
  (void)fx_media_flush(&sd_media);
  return 0;
}
