#ifndef APP_FXDISK_H
#define APP_FXDISK_H

#include <stdint.h>

/* Simulated NOR disk: RAM-backed, no external media required. */
#define APP_FXDISK_BLOCKS               8U
#define APP_FXDISK_SECTORS_PER_BLOCK    16U
#define APP_FXDISK_SECTOR_BYTES         512U
#define APP_FXDISK_TOTAL_SECTORS        (APP_FXDISK_BLOCKS * (APP_FXDISK_SECTORS_PER_BLOCK - 1U))

void app_fxdisk_system_init(void);
void app_fxdisk_init(void);
int app_fxdisk_ready(void);
int app_fxdisk_format(void);
int app_fxdisk_mount(void);
int app_fxdisk_unmount(void);
void app_fxdisk_report(void);
int app_fxdisk_list(void);
int app_fxdisk_write(const char *name, const void *data, uint32_t len, int append);
int app_fxdisk_read(const char *name, char *out, uint32_t max, uint32_t *out_len);
int app_fxdisk_delete(const char *name);
int app_fxdisk_write_pattern(const char *name, uint32_t len, uint32_t seed);
int app_fxdisk_verify_pattern(const char *name, uint32_t len, uint32_t seed);
uint32_t app_fxdisk_bit_violations(void);

#endif
