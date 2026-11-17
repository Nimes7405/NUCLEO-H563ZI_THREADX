#include "app_sdspi.h"
#include "app_config.h"
#include "app_log.h"

#include "main.h"

#include <string.h>

/*
 * SD card in SPI mode. The protocol is the one from the "SD Specifications
 * Part 1: Physical Layer, SPI Bus Protocol" chapter: every command is a
 * 6-byte frame, responses arrive as R1/R3/R7, data blocks are guarded by a
 * 0xFE start token plus a 16-bit CRC.
 *
 * The kernel clock of SPI1 is PLL1Q (250 MHz here), so the prescaler picks the
 * bus clock directly: init runs at 976 kHz, data at 3.9 MHz. Both are chosen
 * for jumper-wire signal integrity rather than for throughput.
 */
#define SD_CS_PORT         GPIOD
#define SD_CS_PIN          GPIO_PIN_14

#define SD_SPI_PRESC_INIT  SPI_BAUDRATEPRESCALER_256   /* 64 MHz per_ck / 256 = 250 kHz */
#define SD_SPI_PRESC_DATA  SPI_BAUDRATEPRESCALER_128   /* 250 MHz / 128 = 1.95 MHz */

#define SD_CMD_TIMEOUT_MS  500U
#define SD_SPI_XFER_MS     5U
#define SD_DATA_TIMEOUT_MS 2000U
#define SD_INIT_RETRIES    3U

/* R1 status bits */
#define SD_R1_IDLE         0x01U
#define SD_R1_ILLEGAL      0x04U

static SPI_HandleTypeDef hspi1_sd;
static uint32_t sd_ready;
static uint32_t sd_block_addressed;
static uint32_t sd_blocks;
static uint32_t sd_sdhc;
static uint32_t sd_hc_capable;
static uint32_t sd_probe_byte = 0xEEU;
static uint32_t sd_spi_state;
static uint32_t sd_hal_status;
static uint32_t sd_hal_error;
static uint32_t sd_spi_prescaler = SD_SPI_PRESC_INIT;
static uint32_t sd_csd_token = 0xEEU;
static uint32_t csd_token_seen;

static void sd_cs(int level)
{
  HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN,
                    (level != 0) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t sd_xfer(uint8_t out)
{
  uint8_t in = 0xFFU;

  (void)HAL_SPI_TransmitReceive(&hspi1_sd, &out, &in, 1U, SD_SPI_XFER_MS);
  return in;
}

/* The card needs at least 74 clocks with CS high after power-up. */
static void sd_clock_idle(uint32_t bytes)
{
  for (uint32_t i = 0U; i < bytes; i++)
  {
    (void)sd_xfer(0xFFU);
  }
}

/* Returns the first byte that is not 0xFF, or 0xFF on timeout. */
static uint8_t sd_read_response(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();
  uint8_t value = 0xFFU;

  do
  {
    value = sd_xfer(0xFFU);
    if ((value & 0x80U) == 0U)
    {
      return value;
    }
  } while ((HAL_GetTick() - start) < timeout_ms);

  return 0xFFU;
}

/* Data blocks start with 0xFE; an error token is 0b0000xxxx. Anything else
   (0xFF) is filler and is skipped. */
static uint8_t sd_wait_data_token(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  for (;;)
  {
    uint8_t value = sd_xfer(0xFFU);

    if (value == 0xFEU)
    {
      return 0xFEU;
    }
    if ((value & 0xF0U) == 0x00U)
    {
      return value;
    }
    if ((HAL_GetTick() - start) >= timeout_ms)
    {
      return 0xFFU;
    }
  }
}

/*
 * Send one command. `body` receives the additional bytes of the longer
 * response types (R3/R7 carry four). Returns the R1 byte, 0xFF on timeout.
 */
static uint8_t sd_command(uint8_t cmd, uint32_t arg, uint8_t crc,
                          uint8_t *body)
{
  uint8_t r1;

  sd_cs(0);
  (void)sd_xfer(0xFFU);
  (void)sd_xfer(0xFFU);

  (void)sd_xfer((uint8_t)(0x40U | cmd));
  (void)sd_xfer((uint8_t)(arg >> 24));
  (void)sd_xfer((uint8_t)(arg >> 16));
  (void)sd_xfer((uint8_t)(arg >> 8));
  (void)sd_xfer((uint8_t)arg);
  (void)sd_xfer(crc);

  r1 = sd_read_response(SD_CMD_TIMEOUT_MS);
  if ((body != NULL) && (r1 != 0xFFU))
  {
    for (uint32_t i = 0U; i < 4U; i++)
    {
      body[i] = sd_xfer(0xFFU);
    }
  }
  return r1;
}

/* Command with CS held past the command itself (ACMD needs the CMD55 state). */
static uint8_t sd_command_keep_cs(uint8_t cmd, uint32_t arg, uint8_t crc)
{
  uint8_t r1;

  (void)sd_xfer(0xFFU);
  (void)sd_xfer((uint8_t)(0x40U | cmd));
  (void)sd_xfer((uint8_t)(arg >> 24));
  (void)sd_xfer((uint8_t)(arg >> 16));
  (void)sd_xfer((uint8_t)(arg >> 8));
  (void)sd_xfer((uint8_t)arg);
  (void)sd_xfer(crc);
  r1 = sd_read_response(SD_CMD_TIMEOUT_MS);
  return r1;
}

static int sd_spi_setup(void);

/* SPI1 can run from several kernel clocks; the SD spec caps the init clock at
   400 kHz, and 250 MHz / 256 is still 976 kHz, so the first phase runs from
   per_ck (HSI, 64 MHz) instead. The kernel clock may only be changed while the
   peripheral is disabled, hence the DeInit/Init around it. */
static int sd_reconfigure(uint32_t source, uint32_t prescaler)
{
  RCC_PeriphCLKInitTypeDef pclk = {0};

  if (hspi1_sd.Instance != NULL)
  {
    (void)HAL_SPI_DeInit(&hspi1_sd);
  }

  pclk.PeriphClockSelection = RCC_PERIPHCLK_SPI1;
  pclk.Spi1ClockSelection = source;
  if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
  {
    return -1;
  }

  sd_spi_prescaler = prescaler;
  return sd_spi_setup();
}

static int sd_spi_setup(void)
{
  hspi1_sd.Instance = SPI1;
  hspi1_sd.Init.Mode = SPI_MODE_MASTER;
  hspi1_sd.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1_sd.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1_sd.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1_sd.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1_sd.Init.NSS = SPI_NSS_SOFT;
  hspi1_sd.Init.BaudRatePrescaler = sd_spi_prescaler;
  hspi1_sd.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1_sd.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1_sd.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1_sd.Init.CRCPolynomial = 7U;
  hspi1_sd.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1_sd.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  hspi1_sd.Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
  hspi1_sd.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
  hspi1_sd.Init.TxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
  hspi1_sd.Init.RxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
  hspi1_sd.Init.MasterSSIdleness = SPI_MASTER_SS_IDLENESS_00CYCLE;
  hspi1_sd.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
  hspi1_sd.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_DISABLE;
  hspi1_sd.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_DISABLE;
  hspi1_sd.Init.IOSwap = SPI_IO_SWAP_DISABLE;

  sd_hal_status = (uint32_t)HAL_SPI_Init(&hspi1_sd);
  sd_hal_error = (uint32_t)hspi1_sd.ErrorCode;
  return (sd_hal_status == (uint32_t)HAL_OK) ? 0 : -1;
}

/* Read the 4-byte OCR/R7 payload into `out` as a 32-bit value. */
static uint32_t sd_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int sd_card_init_sequence(void)
{
  uint8_t body[4];
  uint8_t r1;
  uint32_t start;

  sd_block_addressed = 0U;
  sd_sdhc = 0U;
  sd_hc_capable = 0U;
  sd_blocks = 0U;

  sd_cs(1);
  sd_clock_idle(10U);

  /* CMD0: reset into idle, needs the fixed CRC 0x95. */
  r1 = sd_command(0U, 0U, 0x95U, NULL);
  sd_cs(1);
  (void)sd_xfer(0xFFU);
  if (r1 != SD_R1_IDLE)
  {
    return -1;
  }

  /* CMD8: voltage check and v2 detection, fixed CRC 0x87. */
  r1 = sd_command(8U, 0x000001AAU, 0x87U, body);
  sd_cs(1);
  (void)sd_xfer(0xFFU);
  if (r1 == SD_R1_IDLE)
  {
    if ((body[2] != 0x01U) || (body[3] != 0xAAU))
    {
      return -2;
    }
    sd_hc_capable = 1U;
  }
  else if ((r1 & SD_R1_ILLEGAL) == 0U)
  {
    return -3;
  }

  /* ACMD41 until the card leaves idle. HCS asks for block addressing. */
  start = HAL_GetTick();
  do
  {
    uint8_t r;

    sd_cs(0);
    (void)sd_xfer(0xFFU);
    r = sd_command_keep_cs(55U, 0U, 0x01U);
    if (r > SD_R1_IDLE)
    {
      sd_cs(1);
      (void)sd_xfer(0xFFU);
      if ((r & SD_R1_ILLEGAL) != 0U)
      {
        return -4;
      }
      continue;
    }
    r = sd_command_keep_cs(41U, sd_hc_capable ? 0x40000000U : 0U, 0x01U);
    sd_cs(1);
    (void)sd_xfer(0xFFU);

    if (r == 0U)
    {
      break;
    }
    if (r > SD_R1_IDLE)
    {
      return -5;
    }
    HAL_Delay(1U);
  } while ((HAL_GetTick() - start) < 2000U);

  if (HAL_GetTick() - start >= 2000U)
  {
    return -6;
  }

  /* CMD58: OCR tells us whether block addressing is really active. */
  r1 = sd_command(58U, 0U, 0x01U, body);
  sd_cs(1);
  (void)sd_xfer(0xFFU);
  if (r1 != 0U)
  {
    return -7;
  }
  {
    uint32_t ocr = sd_be32(body);

    sd_sdhc = ((ocr & 0x40000000U) != 0U) ? 1U : 0U;
  }
  sd_block_addressed = sd_sdhc;

  /*
   * Set the block length unconditionally: byte-addressed cards require it, and
   * a few block-addressed cards still refuse data transfers without it.
   */
  r1 = sd_command(16U, APP_SD_BLOCK_SIZE, 0x01U, NULL);
  sd_cs(1);
  (void)sd_xfer(0xFFU);
  if (r1 != 0U)
  {
    return -8;
  }

  /* CMD9: the CSD carries the exact capacity (v2 uses a 48-bit C_SIZE). */
  r1 = sd_command(9U, 0U, 0x01U, NULL);
  if (r1 == 0U)
  {
    uint8_t csd[16];
    uint8_t token = sd_wait_data_token(SD_DATA_TIMEOUT_MS);

    if (token == 0xFEU)
    {
      for (uint32_t i = 0U; i < 16U; i++)
      {
        csd[i] = sd_xfer(0xFFU);
      }
      (void)sd_xfer(0xFFU);
      (void)sd_xfer(0xFFU);
      csd_token_seen = token;

      if ((csd[0] >> 6) == 1U)
      {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16) |
                          ((uint32_t)csd[8] << 8) | (uint32_t)csd[9];
        /* CSD v2 capacity is (C_SIZE + 1) * 512 KiB, i.e. 1024 blocks of 512 B. */
        uint64_t blocks = (uint64_t)(c_size + 1U) * 1024ULL;

        sd_blocks = (blocks > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)blocks;
      }
      else
      {
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03U) << 10) |
                          ((uint32_t)csd[7] << 2) | ((uint32_t)csd[8] >> 6);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03U) << 1) |
                               ((uint32_t)csd[10] >> 7);
        uint32_t read_bl_len = (uint32_t)(csd[5] & 0x0FU);
        uint64_t blocks = (uint64_t)(c_size + 1U);

        blocks *= (uint64_t)1U << (c_size_mult + 2U);
        blocks *= (uint64_t)1U << read_bl_len;
        sd_blocks = (uint32_t)(blocks / (uint64_t)APP_SD_BLOCK_SIZE);
      }
    }
    else
    {
      sd_blocks = 0U;
    }
  }
  sd_cs(1);
  (void)sd_xfer(0xFFU);

  return 0;
}

static uint32_t sd_arg(uint32_t lba)
{
  return (sd_block_addressed != 0U) ? lba : (lba * APP_SD_BLOCK_SIZE);
}

int app_sdspi_init(void)
{
  int rc = -1;

  sd_ready = 0U;

  /* CS idles high and the bus needs a few clocks before the card listens. */
  HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET);
  HAL_Delay(20U);

  if (sd_reconfigure(RCC_SPI1CLKSOURCE_CLKP, SD_SPI_PRESC_INIT) != 0)
  {
    sd_spi_state = (uint32_t)hspi1_sd.State;
    return -1;
  }

  /* With no card (or a dead MISO) the bus idles high, so a 0xFF here means
     the SPI frame itself works. */
  sd_probe_byte = sd_xfer(0xFFU);
  sd_spi_state = (uint32_t)hspi1_sd.State;

  for (uint32_t attempt = 0U; (attempt < SD_INIT_RETRIES) && (rc != 0); attempt++)
  {
    rc = sd_card_init_sequence();
    if (rc == 0)
    {
      break;
    }
    /* A failed attempt leaves the card in an unknown state; a fresh CMD0
       inside the sequence resets it, so just retry. */
    HAL_Delay(5U);
  }

  if (rc == 0)
  {
    /*
     * Both phases run at the same clock, so the peripheral is left alone: any
     * DeInit/Init here puts glitches on SCK while CS is high, and a card treats
     * clocks with CS deasserted as a request to leave SPI mode.
     */
    if (SD_SPI_PRESC_DATA != SD_SPI_PRESC_INIT)
    {
      if (sd_reconfigure(RCC_SPI1CLKSOURCE_PLL1Q, SD_SPI_PRESC_DATA) != 0)
      {
        return -2;
      }
    }
    sd_ready = 1U;
  }
  return rc;
}

void app_sdspi_probe(void)
{
  uint8_t rx[16];

  if (app_sdspi_ready() == 0)
  {
    (void)app_sdspi_init();
  }

  if (sd_reconfigure(RCC_SPI1CLKSOURCE_CLKP, SD_SPI_PRESC_INIT) != 0)
  {
    app_log_printf("SD PROBE spi-init-failed\r\n");
    return;
  }

  sd_cs(1);
  sd_clock_idle(10U);

  /* CMD0 frame, then 10 extra clocks: a card answers 0x01 somewhere here. */
  sd_cs(0);
  (void)sd_xfer(0xFFU);
  (void)sd_xfer(0xFFU);
  (void)sd_xfer(0x40U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x95U);
  for (uint32_t i = 0U; i < 10U; i++)
  {
    rx[i] = sd_xfer(0xFFU);
  }
  sd_cs(1);
  (void)sd_xfer(0xFFU);

  app_log_printf("SD PROBE rx=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                 rx[0], rx[1], rx[2], rx[3], rx[4],
                 rx[5], rx[6], rx[7], rx[8], rx[9]);
}

/* Raw levels of the four signals: IDR reflects the pad even when the pin is in
   alternate-function mode, so this needs no reconfiguration. */
void app_sdspi_pins(void)
{
  uint32_t cs = ((SD_CS_PORT->IDR & SD_CS_PIN) != 0U) ? 1U : 0U;
  uint32_t sck = ((GPIOA->IDR & GPIO_PIN_5) != 0U) ? 1U : 0U;
  uint32_t mosi = ((GPIOB->IDR & GPIO_PIN_5) != 0U) ? 1U : 0U;
  uint32_t miso = ((GPIOG->IDR & GPIO_PIN_9) != 0U) ? 1U : 0U;

  app_log_printf("SD PIN cs=%lu sck=%lu mosi=%lu miso=%lu\r\n",
                 (unsigned long)cs, (unsigned long)sck,
                 (unsigned long)mosi, (unsigned long)miso);
}

/* Loopback check: with D11 (MOSI) shorted to D12 (MISO) the received bytes
   must equal the transmitted ones, which proves the MCU side and the two
   jumpers before blaming the card. */
void app_sdspi_echo(void)
{
  static const uint8_t pattern[4] = {0x55U, 0xAAU, 0x01U, 0xFEU};
  uint8_t rx[4];

  if (sd_reconfigure(RCC_SPI1CLKSOURCE_CLKP, SD_SPI_PRESC_INIT) != 0)
  {
    app_log_printf("SD ECHO spi-init-failed\r\n");
    return;
  }

  sd_cs(1);
  sd_clock_idle(2U);
  for (uint32_t i = 0U; i < 4U; i++)
  {
    rx[i] = sd_xfer(pattern[i]);
  }

  app_log_printf("SD ECHO tx=55 aa 01 fe rx=%02x %02x %02x %02x %s\r\n",
                 rx[0], rx[1], rx[2], rx[3],
                 ((rx[0] == pattern[0]) && (rx[1] == pattern[1]) &&
                  (rx[2] == pattern[2]) && (rx[3] == pattern[3])) ? "PASS" : "FAIL");
}

/* Pure GPIO check: drive MOSI (PB5) as a plain output and read it back on
   MISO (PG9). This bypasses the SPI peripheral, so a high/low pair here proves
   the jumper really links D11 and D12. */
void app_sdspi_jump(void)
{
  GPIO_InitTypeDef gpio = {0};
  uint32_t low;
  uint32_t high;

  if (hspi1_sd.Instance != NULL)
  {
    (void)HAL_SPI_DeInit(&hspi1_sd);
  }

  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gpio);

  gpio.Pin = GPIO_PIN_9;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOG, &gpio);

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);
  HAL_Delay(1U);
  low = ((GPIOG->IDR & GPIO_PIN_9) != 0U) ? 1U : 0U;

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
  HAL_Delay(1U);
  high = ((GPIOG->IDR & GPIO_PIN_9) != 0U) ? 1U : 0U;

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);
  app_log_printf("SD JUMP pb5_low->pg9=%lu pb5_high->pg9=%lu %s\r\n",
                 (unsigned long)low, (unsigned long)high,
                 ((low == 0U) && (high == 1U)) ? "PASS" : "FAIL");

  /* Put the SPI pins back so the next sd command starts from a clean bus. */
  (void)sd_reconfigure(RCC_SPI1CLKSOURCE_CLKP, SD_SPI_PRESC_INIT);
}

/* Raw CMD17 exchange: dump the first bytes after the command frame so a stuck
   MISO line is distinguishable from a missing data token. */
void app_sdspi_read_probe(uint32_t lba)
{
  uint8_t rx[12];
  uint32_t arg = sd_arg(lba);

  if (app_sdspi_ready() == 0)
  {
    if (app_sdspi_init() != 0)
    {
      app_log_printf("SD RDPROBE not-ready\r\n");
      return;
    }
  }

  sd_cs(0);
  (void)sd_xfer(0xFFU);
  (void)sd_xfer(0xFFU);
  (void)sd_xfer(0x40U | 17U);
  (void)sd_xfer((uint8_t)(arg >> 24));
  (void)sd_xfer((uint8_t)(arg >> 16));
  (void)sd_xfer((uint8_t)(arg >> 8));
  (void)sd_xfer((uint8_t)arg);
  (void)sd_xfer(0x01U);
  for (uint32_t i = 0U; i < 12U; i++)
  {
    rx[i] = sd_xfer(0xFFU);
  }
  sd_cs(1);
  (void)sd_xfer(0xFFU);

  app_log_printf("SD RDPROBE lba=%lu rx=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                 (unsigned long)lba, rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
                 rx[6], rx[7], rx[8], rx[9], rx[10], rx[11]);

  /* CMD13 returns R2: the first byte is R1, the second carries the card state. */
  {
    uint8_t st[2];
    uint8_t s1 = sd_command(13U, 0U, 0x01U, NULL);

    st[0] = sd_xfer(0xFFU);
    st[1] = sd_xfer(0xFFU);
    sd_cs(1);
    (void)sd_xfer(0xFFU);
    app_log_printf("SD STATUS r1=%02x r2=%02x%02x state=%lu\r\n",
                   s1, st[0], st[1],
                   (unsigned long)((st[1] >> 1) & 0x0FU));
  }
}

int app_sdspi_ready(void)
{
  return (sd_ready != 0U) ? 1 : 0;
}

uint32_t app_sdspi_blocks(void)
{
  return sd_blocks;
}

uint32_t app_sdspi_block_addressed(void)
{
  return sd_block_addressed;
}

void app_sdspi_info(void)
{
  if (sd_ready == 0U)
  {
    app_log_printf("SD ready=0 spi_state=%lu status=%lu err=%lu probe=%02lx csd=%02lx\r\n",
                   (unsigned long)sd_spi_state, (unsigned long)sd_hal_status,
                   (unsigned long)sd_hal_error, (unsigned long)sd_probe_byte,
                   (unsigned long)sd_csd_token);
    return;
  }

  app_log_printf("SD ready=1 type=%s addressing=%s hc=%lu blocks=%lu mb=%lu csd=%02lx\r\n",
                 (sd_hc_capable != 0U) ? "sdc-v2" : "sdc-v1",
                 (sd_block_addressed != 0U) ? "block" : "byte",
                 (unsigned long)sd_sdhc,
                 (unsigned long)sd_blocks,
                 (unsigned long)(((uint64_t)sd_blocks * APP_SD_BLOCK_SIZE) /
                                 (1024U * 1024U)),
                 (unsigned long)sd_csd_token);
}

int app_sdspi_read_block(uint32_t lba, uint8_t *buf)
{
  uint8_t r1;
  uint8_t token;

  if ((sd_ready == 0U) || (buf == NULL))
  {
    return -1;
  }

  r1 = sd_command(17U, sd_arg(lba), 0x01U, NULL);
  if (((r1 & SD_R1_IDLE) != 0U) && (app_sdspi_init() == 0))
  {
    /* The card came back idle, which means it was reset by a supply dip. */
    r1 = sd_command(17U, sd_arg(lba), 0x01U, NULL);
  }
  if (r1 != 0U)
  {
    sd_cs(1);
    (void)sd_xfer(0xFFU);
    app_log_printf("SD CMD17 r1=%02x\r\n", r1);
    return -2;
  }

  token = sd_wait_data_token(SD_DATA_TIMEOUT_MS);
  if (token != 0xFEU)
  {
    sd_cs(1);
    (void)sd_xfer(0xFFU);
    app_log_printf("SD token=%02x\r\n", token);
    return -3;
  }

  for (uint32_t i = 0U; i < APP_SD_BLOCK_SIZE; i++)
  {
    buf[i] = sd_xfer(0xFFU);
  }
  (void)sd_xfer(0xFFU);   /* CRC, not checked in SPI mode */
  (void)sd_xfer(0xFFU);
  sd_cs(1);
  (void)sd_xfer(0xFFU);
  return 0;
}

int app_sdspi_write_block(uint32_t lba, const uint8_t *buf)
{
  uint8_t r1;
  uint8_t data_response;
  uint32_t start;

  if ((sd_ready == 0U) || (buf == NULL))
  {
    return -1;
  }

  r1 = sd_command(24U, sd_arg(lba), 0x01U, NULL);
  if (r1 != 0U)
  {
    sd_cs(1);
    (void)sd_xfer(0xFFU);
    app_log_printf("SD CMD24 r1=%02x\r\n", r1);
    return -2;
  }

  (void)sd_xfer(0xFFU);      /* one gap byte before the start token */
  (void)sd_xfer(0xFEU);      /* start block token */

  for (uint32_t i = 0U; i < APP_SD_BLOCK_SIZE; i++)
  {
    (void)sd_xfer(buf[i]);
  }
  (void)sd_xfer(0xFFU);      /* CRC is ignored by the card in SPI mode */
  (void)sd_xfer(0xFFU);

  data_response = sd_xfer(0xFFU);
  if ((data_response & 0x1FU) != 0x05U)
  {
    sd_cs(1);
    (void)sd_xfer(0xFFU);
    return -3;
  }

  /* The card holds the bus while it programs the block. */
  start = HAL_GetTick();
  while (sd_xfer(0xFFU) == 0U)
  {
    if ((HAL_GetTick() - start) > SD_DATA_TIMEOUT_MS)
    {
      sd_cs(1);
      (void)sd_xfer(0xFFU);
      return -4;
    }
  }
  sd_cs(1);
  (void)sd_xfer(0xFFU);
  return 0;
}
