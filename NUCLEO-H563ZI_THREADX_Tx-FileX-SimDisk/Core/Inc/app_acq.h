#ifndef APP_ACQ_H
#define APP_ACQ_H

#include <stdint.h>

#define APP_ACQ_SAMPLE_HZ       10000U
#define APP_ACQ_WINDOW_SAMPLES  500U
#define APP_ACQ_FILE_MAX_BYTES  2048U
#define APP_ACQ_FILE_KEEP       4U

void app_acq_init(void);
void app_acq_thread_entry(unsigned long thread_input);
int app_acq_start(void);
void app_acq_stop(void);
int app_acq_running(void);
void app_acq_report(void);
int app_acq_check(void);

#endif
