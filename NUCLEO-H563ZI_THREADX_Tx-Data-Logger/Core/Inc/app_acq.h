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
int app_acq_check(void);

#endif
