#ifndef APP_ACQ_H
#define APP_ACQ_H

#include <stdint.h>

#define APP_ACQ_SAMPLE_HZ       10000U
/* Channels are converted as one sequence per trigger, so a "window" is a
   number of frames (one sample per channel), not samples. */
#define APP_ACQ_CH_MAX          4U
#define APP_ACQ_WINDOW_FRAMES   500U
#define APP_ACQ_FILE_MAX_BYTES  4096U
#define APP_ACQ_FILE_KEEP       4U
/* Rotation limit on a real card, where space is not the constraint. */
#define APP_ACQ_SD_FILE_MAX_BYTES  (1024U * 1024U)

#define APP_ACQ_TARGET_SIM      0
#define APP_ACQ_TARGET_SD       1

void app_acq_system_init(void);
void app_acq_init(void);
void app_acq_thread_entry(unsigned long thread_input);
void app_acq_writer_entry(unsigned long thread_input);
/* use_dma: 0 = ADC end-of-conversion interrupt, 1 = GPDMA circular transfer.
   hz: 0 keeps the current rate, otherwise 1000..100000 Hz. */
int app_acq_start(int use_dma, uint32_t hz);
/* 1..APP_ACQ_CH_MAX; refused while a capture is running. */
int app_acq_set_channels(uint32_t nch);
uint32_t app_acq_channels(void);
/* Deterministic de-interleave check over the real window path. */
int app_acq_demux_selftest(void);
void app_acq_stop(void);
int app_acq_running(void);
void app_acq_report(void);
/* Software-triggered one-shot conversion; also accepts internal channels such
   as ADC_CHANNEL_VREFINT.  Refused while a capture is running. */
int app_acq_one_shot(uint32_t adc_channel, uint32_t samples, uint32_t *raw);
/* Sample channel `adc_channel_number` (plain 0..19 index) once. */
int app_acq_sample_index(uint32_t adc_channel_number, uint32_t samples,
                         uint32_t *raw);
/* have_channel: 0 prints every configured pin plus VREFINT and the VDD
   estimate; 1 samples only `adc_channel_number`. */
void app_acq_level_report(uint32_t adc_channel_number, int have_channel);
/* Diagnostic: DAC1_OUT1 (PA4) -> ADC1_INP18 loopback on the same pad. */
int app_acq_dac_loopback(uint32_t code, uint32_t *raw);
int app_acq_check(void);
/* Select where window records are logged. Refused while capturing.
   APP_ACQ_TARGET_SD mounts the card first and returns its mount status. */
int app_acq_set_target(int target);
const char *app_acq_target_name(void);
/* Analog front-end health check. The first half needs no wiring: it validates
   VREFINT/temperature/VBAT and a DAC-driven pad loopback, which distinguishes a
   dead input path from a dead converter. The "ext" half expects PC0 at 3.3 V
   and PC3 at 0 V and additionally verifies that the four-channel capture keeps
   the two levels in clearly separated bands. Both return 0 when everything
   passed. */
int app_acq_hwcheck(void);
int app_acq_hwcheck_ext(void);
/* 1 when the writer gave up on a repeatedly failing medium. */
int app_acq_media_gave_up(void);

#endif
