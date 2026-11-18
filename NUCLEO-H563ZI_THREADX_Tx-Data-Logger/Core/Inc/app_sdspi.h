#ifndef APP_SDSPI_H
#define APP_SDSPI_H

#include <stdint.h>

/*
 * MicroSD card accessed in SPI mode on SPI1.
 *
 *   SCK  = PA5   (Arduino D13)
 *   MISO = PG9   (Arduino D12)
 *   MOSI = PB5   (Arduino D11)
 *   CS   = PD14  (Arduino D10, plain GPIO so the card can be released early)
 *
 * All calls are blocking and must not run concurrently.
 */

#define APP_SD_BLOCK_SIZE   512U

/* 0 on success, negative on failure. Safe to call repeatedly. */
int app_sdspi_init(void);
/* Non-zero once a card completed the SPI-mode initialisation sequence. */
int app_sdspi_ready(void);
void app_sdspi_info(void);
/* Dump the raw bytes of one CMD0 exchange (bus-level diagnosis). */
void app_sdspi_probe(void);
void app_sdspi_pins(void);
void app_sdspi_echo(void);
void app_sdspi_jump(void);
void app_sdspi_read_probe(uint32_t lba);
/* Card capacity in 512-byte blocks (0 when no card is present). */
uint32_t app_sdspi_blocks(void);
/* 1 when the card uses block addresses, 0 when it uses byte addresses. */
uint32_t app_sdspi_block_addressed(void);

int app_sdspi_read_block(uint32_t lba, uint8_t *buf);
int app_sdspi_write_block(uint32_t lba, const uint8_t *buf);

/* op: 1=CMD17 r1, 2=data token, 3=CMD24 r1, 4=data response */
void app_sdspi_last_error(uint32_t *op, uint32_t *code, uint32_t *lba);

#endif
