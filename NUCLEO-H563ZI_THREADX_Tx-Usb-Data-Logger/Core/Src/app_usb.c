#include "app_usb.h"
#include "app_config.h"
#include "app_log.h"
#include "app_stacks.h"
#include "app_watchdog.h"
#include "main.h"

#include "ux_api.h"
#include "ux_device_class_cdc_acm.h"
#include "ux_dcd_stm32.h"
#include "stm32h5xx_ll_ucpd.h"

#include <stdio.h>
#include <string.h>

PCD_HandleTypeDef hpcd_USB_DRD_FS;

/* ------------------------------------------------------------------------- */
/* Device framework                                                          */
/*                                                                           */
/* One contiguous buffer: the device descriptor followed by a single          */
/* configuration holding a CDC-ACM function. Only two interfaces are needed,  */
/* so no Interface Association Descriptor is required - the device class in   */
/* the device descriptor already tells the host this is a CDC device.         */
/*                                                                           */
/* What Windows calls a virtual COM port is exactly this pair of interfaces:  */
/*   interface 0  CDC communication  + interrupt IN endpoint 0x81             */
/*                (line state and line coding requests arrive here)          */
/*   interface 1  CDC data           + bulk OUT 0x02 / bulk IN 0x83          */
/* ------------------------------------------------------------------------- */
static UCHAR usb_device_framework[] =
{
  /* Device descriptor, 18 bytes. VID/PID are ST's virtual COM port pair, so
     the drivers ship with every desktop OS (usbser.sys, cdc_acm, ...). */
  0x12,                       /* bLength                                        */
  0x01,                       /* bDescriptorType: DEVICE                        */
  0x00, 0x02,                 /* bcdUSB 2.00                                    */
  0x02,                       /* bDeviceClass: CDC                              */
  0x00,                       /* bDeviceSubClass                                */
  0x00,                       /* bDeviceProtocol                                */
  0x40,                       /* bMaxPacketSize0: 64                            */
  0x83, 0x04,                 /* idVendor 0x0483                                */
  0x40, 0x57,                 /* idProduct 0x5740                               */
  0x00, 0x01,                 /* bcdDevice 1.00                                 */
  0x01,                       /* iManufacturer                                  */
  0x02,                       /* iProduct                                       */
  0x03,                       /* iSerialNumber                                  */
  0x01,                       /* bNumConfigurations                             */

  /* Configuration descriptor, wTotalLength = 67. */
  0x09,                       /* bLength                                        */
  0x02,                       /* bDescriptorType: CONFIGURATION                 */
  0x43, 0x00,                 /* wTotalLength 67                                */
  0x02,                       /* bNumInterfaces: 2                              */
  0x01,                       /* bConfigurationValue                            */
  0x00,                       /* iConfiguration                                 */
  0x80,                       /* bmAttributes: bus powered                      */
  0x32,                       /* bMaxPower: 100 mA                              */

  /* Interface 0: CDC communication, one interrupt IN endpoint. */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,

  /* Header functional descriptor: CDC 1.10. */
  0x05, 0x24, 0x00, 0x10, 0x01,

  /* Call management: no call handling, data rides on interface 1. */
  0x05, 0x24, 0x01, 0x00, 0x01,

  /* ACM functional descriptor: supports Set/Get_Line_Coding and line state. */
  0x04, 0x24, 0x02, 0x02,

  /* Union functional descriptor: interface 0 controls interface 1. */
  0x05, 0x24, 0x06, 0x00, 0x01,

  /* Endpoint 0x81: interrupt IN, serial state notifications. */
  0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x0A,

  /* Interface 1: CDC data, two bulk endpoints. */
  0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,

  /* Endpoint 0x02: bulk OUT, host to device. */
  0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,

  /* Endpoint 0x83: bulk IN, device to host. */
  0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00,
};

static UCHAR usb_language_id_framework[] =
{
  0x04, 0x03, 0x09, 0x04, /* only US English */
};

static UCHAR usb_string_framework[APP_USB_STRING_MAX];
static ULONG usb_string_framework_length;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */
#define USB_EVT_TX      0x01UL
#define USB_EVT_READY   0x02UL

static UCHAR usb_pool[APP_USB_POOL_SIZE] __attribute__((aligned(8)));

static UX_SLAVE_CLASS_CDC_ACM *usb_cdc;
static volatile int usb_connected;
static volatile int usb_ready;
static volatile int usb_dump_active;

static TX_EVENT_FLAGS_GROUP usb_events;

static uint8_t usb_rx_ring[APP_USB_RX_SIZE];
static volatile uint32_t usb_rx_head;
static volatile uint32_t usb_rx_tail;

static uint8_t usb_tx_ring[APP_USB_TX_SIZE];
static volatile uint32_t usb_tx_head;
static volatile uint32_t usb_tx_tail;

static volatile uint32_t usb_rx_bytes;
static volatile uint32_t usb_tx_bytes;
static volatile uint32_t usb_rx_dropped;
static volatile uint32_t usb_tx_dropped;

static void usb_cdc_activate(VOID *instance);
static void usb_cdc_deactivate(VOID *instance);
static void usb_cdc_parameter_change(VOID *instance);
static int usb_stack_start(void);
static void usb_push_rx(const uint8_t *data, uint32_t len);

static UX_SLAVE_CLASS_CDC_ACM_PARAMETER usb_cdc_parameter =
{
  usb_cdc_activate,
  usb_cdc_deactivate,
  usb_cdc_parameter_change,
};

/* ------------------------------------------------------------------------- */
/* String descriptors                                                        */
/* ------------------------------------------------------------------------- */
static ULONG usb_string_add(ULONG offset, const char *text)
{
  ULONG len = (ULONG)strlen(text);

  usb_string_framework[offset + 0U] = (UCHAR)(2U + (2U * len));
  usb_string_framework[offset + 1U] = 0x03U; /* STRING */
  for (ULONG i = 0U; i < len; i++)
  {
    usb_string_framework[offset + 2U + (2U * i)] = (UCHAR)text[i];
    usb_string_framework[offset + 3U + (2U * i)] = 0x00U;
  }
  return offset + 2U + (2U * len);
}

static void usb_build_strings(void)
{
  char serial[24];
  ULONG offset = 0U;

  /* The serial number comes from the 96-bit unique ID so two boards on the
     same host do not collide in the Windows device cache. */
  snprintf(serial, sizeof(serial), "%08lX%08lX",
           (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw2());

  offset = usb_string_add(offset, "Nimes7405");
  offset = usb_string_add(offset, "H563ZI UsbDataLogger");
  offset = usb_string_add(offset, serial);

  usb_string_framework_length = offset;
}

/* ------------------------------------------------------------------------- */
/* Class callbacks (invoked by the USBX device stack)                        */
/* ------------------------------------------------------------------------- */
static void usb_cdc_activate(VOID *instance)
{
  UX_SLAVE_CLASS_CDC_ACM_LINE_CODING_PARAMETER coding;
  UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_PARAMETER state;

  usb_cdc = (UX_SLAVE_CLASS_CDC_ACM *)instance;

  /* Present 115200 8N1 until the host overrides it. Without this the host
     sees an until-configured port and some terminals refuse to open it. */
  coding.ux_slave_class_cdc_acm_parameter_baudrate = 115200UL;
  coding.ux_slave_class_cdc_acm_parameter_data_bit = 8U;
  coding.ux_slave_class_cdc_acm_parameter_parity = 0U;
  coding.ux_slave_class_cdc_acm_parameter_stop_bit = 0U;
  (void)ux_device_class_cdc_acm_ioctl(usb_cdc,
                                      UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_LINE_CODING,
                                      &coding);

  state.ux_slave_class_cdc_acm_parameter_rts = 1U;
  state.ux_slave_class_cdc_acm_parameter_dtr = 1U;
  (void)ux_device_class_cdc_acm_ioctl(usb_cdc,
                                      UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_LINE_STATE,
                                      &state);

  /* Bound every transfer so the reader thread cannot park inside USBX and
     miss its heartbeat; the watchdog would reset the board for a link that is
     merely idle. Values are ticks (100 Hz). */
  (void)ux_device_class_cdc_acm_ioctl(usb_cdc,
                                      UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_READ_TIMEOUT,
                                      (VOID *)(ALIGN_TYPE)APP_USB_READ_TIMEOUT);
  (void)ux_device_class_cdc_acm_ioctl(usb_cdc,
                                      UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_WRITE_TIMEOUT,
                                      (VOID *)(ALIGN_TYPE)APP_USB_WRITE_TIMEOUT);

  usb_rx_head = 0U;
  usb_rx_tail = 0U;
  usb_tx_head = 0U;
  usb_tx_tail = 0U;
  usb_connected = 1;
}

static void usb_cdc_deactivate(VOID *instance)
{
  (void)instance;

  usb_connected = 0;
  usb_cdc = UX_NULL;
}

static void usb_cdc_parameter_change(VOID *instance)
{
  (void)instance;
}

/* ------------------------------------------------------------------------- */
/* Type-C front end                                                          */
/* ------------------------------------------------------------------------- */
/*
 * The user connector is a Type-C receptacle behind a TCPP01 protector, and its
 * CC1/CC2 lines go to UCPD1, not to fixed resistors. A host only applies VBUS
 * once it sees a sink termination, so without this the port stays invisible:
 * the device controller happily enables its pull-up and then never sees a bus
 * reset (ISTR stays 0 forever).
 *
 * Rd is all that is needed for USB 2.0 at 5 V - no PD messaging, so the USBPD
 * stack is not linked in.
 */
static void app_usb_ucpd_init(void)
{
  LL_UCPD_InitTypeDef settings;
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_UCPD1_CLK_ENABLE();

  /* The UCPD front end only owns the pins in analog mode. */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_13 | GPIO_PIN_14;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &gpio);

  LL_UCPD_StructInit(&settings);
  (void)LL_UCPD_Init(UCPD1, &settings);

  /* Order matters: the CC termination controls are only latched once the
     peripheral is enabled. Writing the role first leaves CR at zero, which
     reads back as "nothing attached" and looks exactly like an unplugged
     cable. */
  LL_UCPD_Enable(UCPD1);

  /* Dead-battery mode would hand CC control to the protector chip instead. */
  HAL_PWREx_DisableUCPDDeadBattery();

  LL_UCPD_SetSNKRole(UCPD1);
  LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1CC2);
}

/* ------------------------------------------------------------------------- */
/* Stack start                                                               */
/* ------------------------------------------------------------------------- */
static int usb_stack_start(void)
{
  app_usb_ucpd_init();

  if (ux_system_initialize(usb_pool, sizeof(usb_pool), UX_NULL, 0U) != UX_SUCCESS)
  {
    return -1;
  }

  hpcd_USB_DRD_FS.Instance = USB_DRD_FS;
  hpcd_USB_DRD_FS.Init.dev_endpoints = 8U;
  hpcd_USB_DRD_FS.Init.speed = USBD_FS_SPEED;
  hpcd_USB_DRD_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_DRD_FS.Init.Sof_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.battery_charging_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.vbus_sensing_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.bulk_doublebuffer_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.iso_singlebuffer_enable = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_DRD_FS) != HAL_OK)
  {
    return -1;
  }

  /* High speed is not wired on this board, so the FS framework is passed
     twice with a zero length for the HS slot. */
  if (ux_device_stack_initialize(UX_NULL, 0U,
                                 usb_device_framework,
                                 (ULONG)sizeof(usb_device_framework),
                                 usb_string_framework,
                                 usb_string_framework_length,
                                 usb_language_id_framework,
                                 (ULONG)sizeof(usb_language_id_framework),
                                 UX_NULL) != UX_SUCCESS)
  {
    return -1;
  }

  /* Interface 0 is the CDC communication interface; USBX derives the data
     interface from the union descriptor. */
  if (ux_device_stack_class_register(_ux_system_slave_class_cdc_acm_name,
                                     ux_device_class_cdc_acm_entry,
                                     1U, 0U,
                                     &usb_cdc_parameter) != UX_SUCCESS)
  {
    return -1;
  }

  if (ux_dcd_stm32_initialize((ULONG)USB_DRD_FS,
                              (ULONG)&hpcd_USB_DRD_FS) != UX_SUCCESS)
  {
    return -1;
  }

  if (HAL_PCD_Start(&hpcd_USB_DRD_FS) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
/* Rings                                                                     */
/* ------------------------------------------------------------------------- */
static void usb_push_rx(const uint8_t *data, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++)
  {
    uint32_t next = (usb_rx_head + 1U) & (APP_USB_RX_SIZE - 1U);

    if (next == usb_rx_tail)
    {
      usb_rx_dropped++;
      continue;
    }
    usb_rx_ring[usb_rx_head] = data[i];
    __DMB();
    usb_rx_head = next;
  }
  usb_rx_bytes += len;
}

uint32_t app_usb_read(uint8_t *buffer, uint32_t max)
{
  uint32_t count = 0U;

  while ((count < max) && (usb_rx_tail != usb_rx_head))
  {
    buffer[count++] = usb_rx_ring[usb_rx_tail];
    __DMB();
    usb_rx_tail = (usb_rx_tail + 1U) & (APP_USB_RX_SIZE - 1U);
  }
  return count;
}

void app_usb_write(const char *data, uint32_t len)
{
  if ((data == NULL) || (len == 0U) || (usb_connected == 0) || (usb_dump_active != 0))
  {
    return;
  }

  for (uint32_t i = 0U; i < len; i++)
  {
    uint32_t next = (usb_tx_head + 1U) & (APP_USB_TX_SIZE - 1U);

    if (next == usb_tx_tail)
    {
      usb_tx_dropped++;
      continue;
    }
    usb_tx_ring[usb_tx_head] = (uint8_t)data[i];
    __DMB();
    usb_tx_head = next;
  }
  (void)tx_event_flags_set(&usb_events, USB_EVT_TX, TX_OR);
}

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */
/* ------------------------------------------------------------------------- */
void app_usb_rx_thread_entry(ULONG thread_input)
{
  uint8_t buffer[64];

  (void)thread_input;

  usb_build_strings();

  if (usb_stack_start() != 0)
  {
    app_log_printf("USB init failed: no virtual COM port on this boot\r\n");
    for (;;)
    {
      app_hb_mark(APP_HB_USB_RX);
      tx_thread_sleep(APP_MS_TO_TICKS(500));
    }
  }

  usb_ready = 1;
  (void)tx_event_flags_set(&usb_events, USB_EVT_READY, TX_OR);
  app_log_printf("USB cdc ready vid=0483 pid=5740\r\n");

  for (;;)
  {
    ULONG actual = 0U;
    UINT status;

    app_hb_mark(APP_HB_USB_RX);

    if ((usb_connected == 0) || (usb_cdc == UX_NULL))
    {
      tx_thread_sleep(APP_MS_TO_TICKS(20));
      continue;
    }

    status = ux_device_class_cdc_acm_read(usb_cdc, buffer,
                                          (ULONG)sizeof(buffer), &actual);
    if ((status == UX_SUCCESS) && (actual > 0U))
    {
      usb_push_rx(buffer, (uint32_t)actual);
    }
  }
}

void app_usb_tx_thread_entry(ULONG thread_input)
{
  uint8_t buffer[256];

  (void)thread_input;

  for (;;)
  {
    ULONG flags = 0U;
    uint32_t count = 0U;
    ULONG actual = 0U;

    app_hb_mark(APP_HB_USB_TX);

    (void)tx_event_flags_get(&usb_events, USB_EVT_TX | USB_EVT_READY,
                             TX_OR_CLEAR, &flags, APP_MS_TO_TICKS(100));

    if ((usb_connected == 0) || (usb_cdc == UX_NULL))
    {
      usb_tx_head = 0U;
      usb_tx_tail = 0U;
      continue;
    }

    /* A blocking dump owns the pipe; letting a queued log line through here
       would splice it into the middle of the file. */
    if (usb_dump_active != 0)
    {
      continue;
    }

    while ((count < sizeof(buffer)) && (usb_tx_tail != usb_tx_head))
    {
      buffer[count++] = usb_tx_ring[usb_tx_tail];
      __DMB();
      usb_tx_tail = (usb_tx_tail + 1U) & (APP_USB_TX_SIZE - 1U);
    }

    if (count == 0U)
    {
      continue;
    }

    if (ux_device_class_cdc_acm_write(usb_cdc, buffer, (ULONG)count, &actual) == UX_SUCCESS)
    {
      usb_tx_bytes += (uint32_t)actual;
    }
    else
    {
      /* The host stopped reading; drop the frame instead of retrying forever,
         otherwise a detached terminal would stall every later log line. */
      usb_tx_dropped += (uint32_t)(count - actual);
      usb_tx_tail = usb_tx_head;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Blocking send (used by `dump`)                                            */
/* ------------------------------------------------------------------------- */
void app_usb_set_dump(int on)
{
  usb_dump_active = (on != 0) ? 1 : 0;
}

int app_usb_send_blocking(const uint8_t *data, uint32_t len)
{
  uint32_t sent = 0U;

  if ((usb_connected == 0) || (usb_cdc == UX_NULL) || (data == NULL))
  {
    return -1;
  }

  while (sent < len)
  {
    ULONG chunk = (ULONG)((len - sent) > 512U ? 512U : (len - sent));
    ULONG actual = 0U;

    if (ux_device_class_cdc_acm_write(usb_cdc, (UCHAR *)&data[sent], chunk, &actual) != UX_SUCCESS)
    {
      return -1;
    }
    usb_tx_bytes += (uint32_t)actual;
    sent += (uint32_t)actual;
    if (actual == 0U)
    {
      return -1;
    }
  }
  return 0;
}

int app_usb_flush(uint32_t timeout_ticks)
{
  uint32_t waited = 0U;

  while ((usb_tx_tail != usb_tx_head) && (waited < timeout_ticks))
  {
    tx_thread_sleep(1);
    waited++;
  }
  return (usb_tx_tail == usb_tx_head) ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------- */
int app_usb_connected(void)
{
  return (usb_connected != 0) ? 1 : 0;
}

int app_usb_ready(void)
{
  return (usb_ready != 0) ? 1 : 0;
}

void app_usb_report(void)
{
  app_log_printf("USB ready=%d connected=%d rx=%lu tx=%lu rx_drop=%lu tx_drop=%lu\r\n",
                 (usb_ready != 0) ? 1 : 0,
                 (usb_connected != 0) ? 1 : 0,
                 (unsigned long)usb_rx_bytes,
                 (unsigned long)usb_tx_bytes,
                 (unsigned long)usb_rx_dropped,
                 (unsigned long)usb_tx_dropped);
}

/* Write the sink termination again and show the register before/after, so a
   silently ignored CR write is visible instead of looking like "no cable". */
void app_usb_cc_enable(void)
{
  app_log_printf("USB CC before cr=0x%08lx cfg1=0x%08lx\r\n",
                 (unsigned long)UCPD1->CR, (unsigned long)UCPD1->CFG1);
  LL_UCPD_Enable(UCPD1);
  LL_UCPD_SetSNKRole(UCPD1);
  LL_UCPD_SetccEnable(UCPD1, LL_UCPD_CCENABLE_CC1CC2);
  app_log_printf("USB CC after cr=0x%08lx cfg1=0x%08lx\r\n",
                 (unsigned long)UCPD1->CR, (unsigned long)UCPD1->CFG1);
}

void app_usb_report_regs(void)
{
  app_log_printf("USB REG cntr=0x%08lx istr=0x%08lx fnr=0x%08lx bcdr=0x%08lx\r\n",
                 (unsigned long)USB_DRD_FS->CNTR,
                 (unsigned long)USB_DRD_FS->ISTR,
                 (unsigned long)USB_DRD_FS->FNR,
                 (unsigned long)USB_DRD_FS->BCDR);
  app_log_printf("USB PAD moder=0x%08lx idr=0x%08lx dppu=%lu dp=%lu dm=%lu\r\n",
                 (unsigned long)GPIOA->MODER,
                 (unsigned long)GPIOA->IDR,
                 (unsigned long)((USB_DRD_FS->BCDR >> 15) & 1U),
                 (unsigned long)((GPIOA->IDR >> 12) & 1U),
                 (unsigned long)((GPIOA->IDR >> 11) & 1U));
  app_log_printf("USB UCPD cfg1=0x%08lx cr=0x%08lx sr=0x%08lx\r\n",
                 (unsigned long)UCPD1->CFG1,
                 (unsigned long)UCPD1->CR,
                 (unsigned long)UCPD1->SR);
  /* VSTATE: 0 = below 0.2 V, 1 = 0.2-0.66 V (Rd), 2 = above 0.66 V (Rp, i.e.
     a host is driving this CC line). A sink plugged into a live host port sees
     2 on exactly one of the two lines. */
  app_log_printf("USB CC cc1=%lu cc2=%lu (0=none 1=rd 2=source)\r\n",
                 (unsigned long)((LL_UCPD_GetTypeCVstateCC1(UCPD1) >> UCPD_SR_TYPEC_VSTATE_CC1_Pos) & 3U),
                 (unsigned long)((LL_UCPD_GetTypeCVstateCC2(UCPD1) >> UCPD_SR_TYPEC_VSTATE_CC2_Pos) & 3U));
}

void app_usb_system_init(void)
{
  if (tx_event_flags_create(&usb_events, "usb-events") != TX_SUCCESS)
  {
    Error_Handler();
  }
}

/* ------------------------------------------------------------------------- */
/* PCD low level                                                             */
/* ------------------------------------------------------------------------- */
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
  RCC_PeriphCLKInitTypeDef clk = {0};

  if (hpcd->Instance != USB_DRD_FS)
  {
    return;
  }

  /* HSI48 feeds the USB core and needs no external crystal. */
  clk.PeriphClockSelection = RCC_PERIPHCLK_USB;
  clk.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
  {
    Error_Handler();
  }

  /* The transceiver sits in the isolated VDD33USB domain. */
  HAL_PWREx_EnableVddUSB();

  __HAL_RCC_USB_CLK_ENABLE();

  /* PA9 is the TCPP01 dead-battery input. Driving it high disables the
     protector's own termination so UCPD1 owns CC1/CC2 (see app_usb_ucpd_init);
     leaving it low makes two Rd networks fight over the same lines. */
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET);
  gpio.Pin = GPIO_PIN_9;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* PA11/PA12 are the dedicated USB_DRD_FS pads and are driven by the
     controller itself, which is why ST's working samples leave them at their
     reset state. Only the NVIC side needs configuring. */
  HAL_NVIC_SetPriority(USB_DRD_FS_IRQn, 6U, 0U);
  HAL_NVIC_EnableIRQ(USB_DRD_FS_IRQn);
}
