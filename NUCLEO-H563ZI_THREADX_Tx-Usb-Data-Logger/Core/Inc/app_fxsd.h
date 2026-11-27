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
/* Bounded wait, so a caller that must keep marking a heartbeat can do so while
   another thread holds the media for a long self-check or dump. */
int app_fxsd_lock_timeout(uint32_t ticks);
int app_fxsd_unlock(void);

int app_fxsd_is_mounted(void);
/* 0 when a volume is mounted, mirroring app_fxdisk_ready(). */
int app_fxsd_mounted_ready(void);
int app_fxsd_mount(void);
int app_fxsd_unmount(void);
/* Destroys the card's partition table: the volume starts at sector 0. */
int app_fxsd_format(uint32_t sectors);

void app_fxsd_report(void);
int app_fxsd_list(void);
int app_fxsd_write_file(const char *name, const char *text, int append);
/* Append without flushing the media. Callers that write small records next to a
   busy capture file use this: fx_media_flush blocks the acquisition writer long
   enough to overflow its queue, and the next capture flush persists this data
   anyway. */
int app_fxsd_append_lazy(const char *name, const char *text);
int app_fxsd_read_file(const char *name, char *out, uint32_t max, uint32_t *out_len);
/* Read a window starting at `offset`; used by the console to page through a
   log longer than its output buffer. */
int app_fxsd_read_at(const char *name, uint32_t offset, char *out, uint32_t max,
                     uint32_t *out_len);
int app_fxsd_delete(const char *name);

/* Sequential log interface, same shape as app_fxdisk's, so the acquisition
   writer can target either medium. */
int app_fxsd_log_open(const char *name);
int app_fxsd_log_write(const void *data, uint32_t len);
int app_fxsd_log_close(void);
int app_fxsd_log_flush(void);
int app_fxsd_log_size(uint32_t *size);
typedef int (*app_fxsd_chunk_fn)(const char *data, uint32_t len, void *ctx);
int app_fxsd_read_stream(const char *name, app_fxsd_chunk_fn fn, void *ctx,
                         uint32_t *total);

#endif
