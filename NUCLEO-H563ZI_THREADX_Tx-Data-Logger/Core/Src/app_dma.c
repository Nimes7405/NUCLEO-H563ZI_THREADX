#include "app_dma.h"
#include "app_log.h"
#include "main.h"

#include <string.h>

/*
 * Why this file exists
 * --------------------
 * The ADC + GPDMA1 capture path was abandoned because the channel reported a
 * completed transfer while the destination buffer stayed untouched.  Nothing
 * in that path could separate "the DMA engine does not write SRAM" from "the
 * ADC never produced a request" or "FileX/console code hid the data".
 *
 * So the transfer matrix below keeps only the DMA: source and destination are
 * ordinary .bss words, the transfer is memory-to-memory, and completion is
 * polled instead of interrupt driven.  Each case reconfigures one knob:
 *
 *   - channel attributes (privilege, channel security, source/destination
 *     security)
 *   - the allocated GPDMA port pair
 *   - which SRAM bank holds the buffer
 *   - the non-secure (0x2xxx_xxxx) versus secure (0x3xxx_xxxx) memory alias
 *
 * Dump with the console command `dma env` / `dma test`.
 */

#define DMA_TEST_WORDS    32U
#define DMA_TEST_BYTES    (DMA_TEST_WORDS * 4U)
#define DMA_TIMEOUT_MS    20U

#define DMA_ALIAS_OFFSET  0x10000000UL
#define DMA_SRAM2_SCRATCH 0x20040000UL
#define DMA_SRAM3_SCRATCH 0x20050000UL
#define DMA_MPCBB_BLOCK   512U

#define DMA_PORT_P0P1     (DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT1)
#define DMA_PORT_P0P0     (DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0)

typedef struct
{
  const char *attr;
  const char *port;
  const char *buf;
  uint32_t    attributes;
  uint32_t    ports;
  uint32_t    src_phys;
  uint32_t    src_xfer;
  uint32_t    dst_phys;
  uint32_t    dst_xfer;
  int         required;
} dma_case_t;

static uint32_t dma_src[DMA_TEST_WORDS] __attribute__((aligned(32)));
static uint32_t dma_dst[DMA_TEST_WORDS] __attribute__((aligned(32)));
static DMA_HandleTypeDef dma_handle;

static uint32_t dma_rd(uint32_t addr)
{
  return *(volatile uint32_t *)addr;
}

static void dma_wr(uint32_t addr, uint32_t value)
{
  *(volatile uint32_t *)addr = value;
}

/*
 * Bring channel 0 back to its power-on defaults without touching the rest of
 * the controller: the acquisition path owns GPDMA1 channel 1, and a whole
 * controller reset would silently drop the linked-list setup (and with it the
 * secure beat attributes) out from under it.
 * The values below are what `dma env` reports on a fresh boot.
 */
static void dma_hw_reset(void)
{
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  GPDMA1_Channel0->CCR = 0U;
  GPDMA1->SECCFGR &= ~(1UL << 0U);
  GPDMA1->PRIVCFGR &= ~(1UL << 0U);
  GPDMA1_Channel0->CTR1 = 0U;
  GPDMA1_Channel0->CTR2 = 0U;
  GPDMA1_Channel0->CBR1 = 0U;
  GPDMA1_Channel0->CFCR = 0x00007F00U;
}

/* Print the TrustZone attribution of the 512-byte block holding `addr`. */
static void dma_dump_region(const char *name, uint32_t addr)
{
  const GTZC_MPCBB_TypeDef *mpcbb;
  uint32_t base;
  uint32_t idx;
  uint32_t sec;
  uint32_t priv;
  const char *bank;

  if (addr >= DMA_ALIAS_OFFSET + SRAM1_BASE_NS)
  {
    addr -= DMA_ALIAS_OFFSET;
  }

  if (addr < SRAM2_BASE_NS)
  {
    mpcbb = GTZC_MPCBB1;
    base = SRAM1_BASE_NS;
    bank = "sram1";
  }
  else if (addr < SRAM3_BASE_NS)
  {
    mpcbb = GTZC_MPCBB2;
    base = SRAM2_BASE_NS;
    bank = "sram2";
  }
  else
  {
    mpcbb = GTZC_MPCBB3;
    base = SRAM3_BASE_NS;
    bank = "sram3";
  }

  idx = (addr - base) / DMA_MPCBB_BLOCK;
  sec = (mpcbb->SECCFGR[idx / 32U] >> (idx % 32U)) & 1U;
  priv = (mpcbb->PRIVCFGR[idx / 32U] >> (idx % 32U)) & 1U;
  app_log_printf("DMA ENV %s=%08lx %s sec=%u priv=%u\r\n", name,
                 (unsigned long)addr, bank, (unsigned)sec, (unsigned)priv);
}

void app_dma_env_report(void)
{
  uint32_t ctrl = SAU->CTRL;

  app_log_printf("DMA ENV sau_ctrl=%08lx sau_type=%08lx allns=%u\r\n",
                 (unsigned long)ctrl, (unsigned long)SAU->TYPE,
                 (unsigned)((ctrl & SAU_CTRL_ALLNS_Msk) != 0U));
  app_log_printf("DMA ENV gpdma1 seccfgr=%08lx privcfgr=%08lx lock=%08lx\r\n",
                 (unsigned long)GPDMA1->SECCFGR,
                 (unsigned long)GPDMA1->PRIVCFGR,
                 (unsigned long)GPDMA1->RCFGLOCKR);
  app_log_printf("DMA ENV ch0 ccr=%08lx ctr1=%08lx ctr2=%08lx\r\n",
                 (unsigned long)GPDMA1_Channel0->CCR,
                 (unsigned long)GPDMA1_Channel0->CTR1,
                 (unsigned long)GPDMA1_Channel0->CTR2);

  dma_dump_region("src", (uint32_t)(uintptr_t)dma_src);
  dma_dump_region("dst", (uint32_t)(uintptr_t)dma_dst);
  dma_dump_region("sram2", DMA_SRAM2_SCRATCH);
  dma_dump_region("sram3", DMA_SRAM3_SCRATCH);
}

/* Returns 0 when the transfer both completed and landed in the buffer. */
static int dma_case_run(uint32_t index, const dma_case_t *c)
{
  HAL_StatusTypeDef start_rc = HAL_ERROR;
  HAL_StatusTypeDef poll_rc = HAL_BUSY;
  uint32_t err = 0U;
  uint32_t csr = 0U;
  uint32_t cbr1 = 0U;
  uint32_t mism = 0U;
  uint32_t i;
  int cpu_ok = 1;
  int init_ok;
  int attr_ok;
  const char *verdict = "FAIL";
  int ok = 1;

  dma_hw_reset();

  for (i = 0U; i < DMA_TEST_WORDS; i++)
  {
    dma_wr(c->src_phys + (4U * i), 0xA5A50000UL + i);
    dma_wr(c->dst_phys + (4U * i), 0xDEAD0000UL + i);
  }
  /* Prove the CPU can reach the buffer before blaming the DMA for it. */
  for (i = 0U; i < DMA_TEST_WORDS; i++)
  {
    if ((dma_rd(c->src_phys + (4U * i)) != (0xA5A50000UL + i)) ||
        (dma_rd(c->dst_phys + (4U * i)) != (0xDEAD0000UL + i)))
    {
      cpu_ok = 0;
      break;
    }
  }

  memset(&dma_handle, 0, sizeof(dma_handle));
  dma_handle.Instance = GPDMA1_Channel0;
  dma_handle.Init.Request = DMA_REQUEST_SW;
  dma_handle.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
  dma_handle.Init.Direction = DMA_MEMORY_TO_MEMORY;
  dma_handle.Init.SrcInc = DMA_SINC_INCREMENTED;
  dma_handle.Init.DestInc = DMA_DINC_INCREMENTED;
  dma_handle.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_WORD;
  dma_handle.Init.DestDataWidth = DMA_DEST_DATAWIDTH_WORD;
  dma_handle.Init.Priority = DMA_LOW_PRIORITY_HIGH_WEIGHT;
  dma_handle.Init.SrcBurstLength = 1U;
  dma_handle.Init.DestBurstLength = 1U;
  dma_handle.Init.TransferAllocatedPort = c->ports;
  dma_handle.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
  dma_handle.Init.Mode = DMA_NORMAL;

  init_ok = (HAL_DMA_Init(&dma_handle) == HAL_OK);
  attr_ok = init_ok && ((c->attributes == 0U) ||
                        (HAL_DMA_ConfigChannelAttributes(&dma_handle, c->attributes) == HAL_OK));

  if (init_ok && attr_ok && cpu_ok)
  {
    start_rc = HAL_DMA_Start(&dma_handle, c->src_xfer, c->dst_xfer, DMA_TEST_BYTES);
    if (start_rc == HAL_OK)
    {
      poll_rc = HAL_DMA_PollForTransfer(&dma_handle, HAL_DMA_FULL_TRANSFER, DMA_TIMEOUT_MS);
    }

    for (i = 0U; i < DMA_TEST_WORDS; i++)
    {
      if (dma_rd(c->dst_phys + (4U * i)) != (0xA5A50000UL + i))
      {
        mism++;
      }
    }

    csr = GPDMA1_Channel0->CSR;
    cbr1 = GPDMA1_Channel0->CBR1;
    err = dma_handle.ErrorCode;
    (void)HAL_DMA_Abort(&dma_handle);

    if ((start_rc == HAL_OK) && (poll_rc == HAL_OK) && (mism == 0U))
    {
      verdict = "PASS";
    }
    else
    {
      ok = 0;
    }
  }
  else if (!init_ok)
  {
    verdict = "INIT";
    ok = 0;
  }
  else if (!attr_ok)
  {
    verdict = "ATTR";
    ok = 0;
  }
  else
  {
    verdict = "CPU";
    ok = 0;
  }

  app_log_printf("DMA c=%02lu attr=%s port=%s buf=%s start=%d poll=%d err=%04lx "
                 "csr=%04lx cbr1=%04lx mism=%02lu dst0=%08lx %s\r\n",
                 (unsigned long)index, c->attr, c->port, c->buf,
                 (int)start_rc, (int)poll_rc,
                 (unsigned long)err, (unsigned long)csr, (unsigned long)cbr1,
                 (unsigned long)mism, (unsigned long)dma_rd(c->dst_phys),
                 verdict);
  return (ok != 0) ? 0 : 1;
}

int app_dma_mem_selftest(void)
{
  const uint32_t src = (uint32_t)(uintptr_t)dma_src;
  const uint32_t dst = (uint32_t)(uintptr_t)dma_dst;
  int failures = 0;
  uint32_t i;
  int rc;

  dma_hw_reset();

#define DMA_ATTR_FULL  (DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC | \
                        DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC)

  const dma_case_t cases[] =
  {
    /* attr   port     buf      attributes         ports           required */
    { .attr = "-",    .port = "p0/p1", .buf = "sram1", .attributes = 0U,
      .ports = DMA_PORT_P0P1 },
    { .attr = "P",    .port = "p0/p1", .buf = "sram1", .attributes = DMA_CHANNEL_PRIV,
      .ports = DMA_PORT_P0P1 },
    { .attr = "PS",   .port = "p0/p1", .buf = "sram1",
      .attributes = DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC, .ports = DMA_PORT_P0P1 },
    /* The configuration this firmware needs: secure + privileged channel that
       also performs secure source and destination beats. */
    { .attr = "PSSD", .port = "p0/p1", .buf = "sram1", .attributes = DMA_ATTR_FULL,
      .ports = DMA_PORT_P0P1, .required = 1 },
    { .attr = "SSD",  .port = "p0/p1", .buf = "sram1",
      .attributes = DMA_CHANNEL_SEC | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC,
      .ports = DMA_PORT_P0P1 },
    { .attr = "PSD",  .port = "p0/p1", .buf = "sram1",
      .attributes = DMA_CHANNEL_PRIV | DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC,
      .ports = DMA_PORT_P0P1 },
    { .attr = "PSSD", .port = "p0/p0", .buf = "sram1", .attributes = DMA_ATTR_FULL,
      .ports = DMA_PORT_P0P0 },
    { .attr = "PSSD", .port = "p0/p1", .buf = "sram2", .attributes = DMA_ATTR_FULL,
      .ports = DMA_PORT_P0P1 },
    { .attr = "PSSD", .port = "p0/p1", .buf = "sram3", .attributes = DMA_ATTR_FULL,
      .ports = DMA_PORT_P0P1 },
    { .attr = "PSSD", .port = "p0/p1", .buf = "alias", .attributes = DMA_ATTR_FULL,
      .ports = DMA_PORT_P0P1 },
  };

  app_log_printf("DMA MEMTEST word-count=%lu timeout=%lums attr: -=reset default "
                 "P=priv S=secure SS=src-sec DS=dst-sec\r\n",
                 (unsigned long)DMA_TEST_WORDS, (unsigned long)DMA_TIMEOUT_MS);

  for (i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++)
  {
    dma_case_t c = cases[i];

    c.src_phys = src;
    c.src_xfer = src;
    c.dst_phys = dst;
    c.dst_xfer = dst;

    if (strcmp(c.buf, "sram2") == 0)
    {
      c.dst_phys = DMA_SRAM2_SCRATCH;
      c.dst_xfer = DMA_SRAM2_SCRATCH;
    }
    else if (strcmp(c.buf, "sram3") == 0)
    {
      c.dst_phys = DMA_SRAM3_SCRATCH;
      c.dst_xfer = DMA_SRAM3_SCRATCH;
    }
    else if (strcmp(c.buf, "alias") == 0)
    {
      /* Same physical SRAM1 words, reached through the secure alias. */
      c.src_xfer = src + DMA_ALIAS_OFFSET;
      c.dst_xfer = dst + DMA_ALIAS_OFFSET;
    }

    rc = dma_case_run(i, &c);
    if ((c.required != 0) && (rc != 0))
    {
      failures++;
    }
  }

  dma_hw_reset();

  app_log_printf("DMA MEMTEST %s failures=%d\r\n", (failures == 0) ? "PASS" : "FAIL", failures);
  return failures;
}
