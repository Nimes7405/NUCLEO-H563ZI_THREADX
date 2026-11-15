#ifndef APP_DMA_H
#define APP_DMA_H

/*
 * GPDMA1 diagnostics.
 *
 * The ADC + GPDMA1 capture path once reported "transfer counted but no data
 * in SRAM" on this board.  These helpers exist to isolate that: they dump the
 * TrustZone configuration that filters GPDMA accesses, and they run a pure
 * memory-to-memory transfer matrix (no ADC, no FileX) so the DMA engine can be
 * judged on its own.
 *
 * `app_dma_mem_selftest()` resets GPDMA1; nothing else in the firmware uses
 * the controller.
 */

void app_dma_env_report(void);
int app_dma_mem_selftest(void);

#endif
