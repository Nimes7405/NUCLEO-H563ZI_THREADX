#ifndef APP_FXSD_H
#define APP_FXSD_H

#include <stdint.h>

/*
 * FileX on the SPI MicroSD card: the same FileX core the simulated NOR disk
 * uses, but the media driver talks to app_sdspi instead of LevelX.
 */

/* Default volume size for `sd format`: a full 64 GB format would have to write
   a ~120 MB FAT over a 1.95 MHz SPI link, so the test volume is smaller. */
#define APP_FXSD_FORMAT_SECTORS_DEFAULT  262144U   /* 128 MB */

void app_fxsd_system_init(void);
void app_fxsd_init(void);

int app_fxsd_lock(void);
int app_fxsd_unlock(void);

int app_fxsd_is_mounted(void);
int app_fxsd_mount(void);
int app_fxsd_unmount(void);
/* Destroys the card's partition table: the volume starts at sector 0. */
int app_fxsd_format(uint32_t sectors);

void app_fxsd_report(void);
int app_fxsd_list(void);
int app_fxsd_write_file(const char *name, const char *text, int append);
int app_fxsd_read_file(const char *name, char *out, uint32_t max, uint32_t *out_len);
int app_fxsd_delete(const char *name);

#endif
