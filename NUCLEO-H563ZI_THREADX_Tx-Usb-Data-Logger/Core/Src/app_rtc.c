#include "app_rtc.h"
#include "app_log.h"

#include "main.h"

#include <stdio.h>

/*
 * The calendar is kept in year/month/day form because the RTC's BCD registers
 * are civil, not an epoch. Conversions here are the usual days-from-civil
 * algorithm, valid for 2000-03-01 onward, which is all a data logger needs.
 */

#define RTC_BKP_MAGIC   0x52435431U   /* "RCT1" */
#define LSE_STARTUP_MS  2000U

typedef enum
{
  RTC_SRC_NONE = 0U,
  RTC_SRC_LSE,
  RTC_SRC_LSI
} app_rtc_source_t;

static RTC_HandleTypeDef hrtc;
static uint32_t rtc_ready;
static app_rtc_source_t rtc_source;

static int rtc_is_leap(uint32_t y)
{
  return (((y % 4U) == 0U) && ((y % 100U) != 0U)) || ((y % 400U) == 0U);
}

static uint32_t rtc_days_in_month(uint32_t y, uint32_t m)
{
  static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U,
                                   31U, 31U, 30U, 31U, 30U, 31U};

  if ((m < 1U) || (m > 12U))
  {
    return 0U;
  }
  if ((m == 2U) && (rtc_is_leap(y) != 0))
  {
    return 29U;
  }
  return days[m - 1U];
}

/* Days since 2000-01-01 for a civil date. */
static uint32_t rtc_days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
  uint32_t days = 0U;

  for (uint32_t year = APP_RTC_EPOCH_BASE_YEAR; year < y; year++)
  {
    days += (rtc_is_leap(year) != 0) ? 366U : 365U;
  }
  for (uint32_t month = 1U; month < m; month++)
  {
    days += rtc_days_in_month(y, month);
  }
  return days + (d - 1U);
}

static void rtc_civil_from_days(uint32_t days, uint32_t *y, uint32_t *m,
                                uint32_t *d)
{
  uint32_t year = APP_RTC_EPOCH_BASE_YEAR;
  uint32_t month = 1U;

  for (;;)
  {
    uint32_t in_year = (rtc_is_leap(year) != 0) ? 366U : 365U;

    if (days < in_year)
    {
      break;
    }
    days -= in_year;
    year++;
  }

  for (;;)
  {
    uint32_t in_month = rtc_days_in_month(year, month);

    if (days < in_month)
    {
      break;
    }
    days -= in_month;
    month++;
  }

  *y = year;
  *m = month;
  *d = days + 1U;
}

static uint8_t rtc_bcd(uint32_t v)
{
  return (uint8_t)(((v / 10U) << 4) | (v % 10U));
}

static uint32_t rtc_unbcd(uint8_t v)
{
  return ((uint32_t)(v >> 4) * 10U) + (uint32_t)(v & 0x0FU);
}

static int rtc_clock_config(app_rtc_source_t source)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_PeriphCLKInitTypeDef pclk = {0};

  osc.PLL.PLLState = RCC_PLL_NONE;

  if (source == RTC_SRC_LSE)
  {
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;
    pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
  }
  else
  {
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
    osc.LSIState = RCC_LSI_ON;
    pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  }

  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    return -1;
  }

  if (source == RTC_SRC_LSE)
  {
    /* Wait for the crystal; a missing one must not wedge the start-up. */
    uint32_t start = HAL_GetTick();

    while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == 0U)
    {
      if ((HAL_GetTick() - start) > LSE_STARTUP_MS)
      {
        return -2;
      }
    }
  }

  pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
  if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
  {
    return -3;
  }
  return 0;
}

int app_rtc_init(void)
{
  RTC_DateTypeDef date;
  RTC_TimeTypeDef time;
  uint32_t warm;

  /* Without a VBAT cell the whole backup domain starts from scratch, so the
     magic value tells a cold start from a warm reset of a running calendar. */
  warm = (TAMP->BKP0R == RTC_BKP_MAGIC) ? 1U : 0U;

  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_RTC_ENABLE();
  __HAL_RCC_RTC_CLK_ENABLE();

  if (warm != 0U)
  {
    /* Keep the calendar that is already ticking. */
    rtc_source = RTC_SRC_LSE;
    rtc_ready = 1U;
    return 0;
  }

  if (rtc_clock_config(RTC_SRC_LSE) == 0)
  {
    rtc_source = RTC_SRC_LSE;
    /* (127+1) * (255+1) = 32768 -> 1 Hz. */
    hrtc.Init.AsynchPrediv = 127U;
    hrtc.Init.SynchPrediv = 255U;
  }
  else if (rtc_clock_config(RTC_SRC_LSI) == 0)
  {
    rtc_source = RTC_SRC_LSI;
    /* LSI is nominally 32 kHz: (127+1) * (249+1) = 32000 -> 1 Hz. */
    hrtc.Init.AsynchPrediv = 127U;
    hrtc.Init.SynchPrediv = 249U;
  }
  else
  {
    rtc_source = RTC_SRC_NONE;
    return -1;
  }

  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.BinMode = RTC_BINARY_NONE;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    rtc_source = RTC_SRC_NONE;
    return -2;
  }

  /* Start at the epoch base so unset timestamps are obvious in the log. */
  date.Year = (uint8_t)0U;
  date.Month = RTC_MONTH_JANUARY;
  date.Date = 1U;
  date.WeekDay = RTC_WEEKDAY_SATURDAY;
  (void)HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BCD);

  time.Hours = 0U;
  time.Minutes = 0U;
  time.Seconds = 0U;
  time.TimeFormat = RTC_HOURFORMAT_24;
  time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  time.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BCD) != HAL_OK)
  {
    rtc_source = RTC_SRC_NONE;
    return -3;
  }

  TAMP->BKP0R = RTC_BKP_MAGIC;
  rtc_ready = 1U;
  return 0;
}

int app_rtc_ready(void)
{
  return (rtc_ready != 0U) ? 1 : 0;
}

/* Civil date/time of the RTC registers. */
static int rtc_read_civil(uint32_t *y, uint32_t *m, uint32_t *d, uint32_t *hh,
                          uint32_t *mm, uint32_t *ss)
{
  RTC_DateTypeDef date;
  RTC_TimeTypeDef time;

  if (rtc_ready == 0U)
  {
    return -1;
  }

  if ((HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BCD) != HAL_OK) ||
      (HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BCD) != HAL_OK))
  {
    return -1;
  }

  *y = APP_RTC_EPOCH_BASE_YEAR + rtc_unbcd(date.Year);
  *m = rtc_unbcd(date.Month);
  *d = rtc_unbcd(date.Date);
  *hh = rtc_unbcd(time.Hours);
  *mm = rtc_unbcd(time.Minutes);
  *ss = rtc_unbcd(time.Seconds);
  return 0;
}

uint32_t app_rtc_epoch(void)
{
  uint32_t y, m, d, hh, mm, ss;

  if (rtc_read_civil(&y, &m, &d, &hh, &mm, &ss) != 0)
  {
    return 0U;
  }
  return (rtc_days_from_civil(y, m, d) * 86400U) + (hh * 3600U) + (mm * 60U) + ss;
}

int app_rtc_set(uint32_t year, uint32_t month, uint32_t day, uint32_t hour,
                uint32_t minute, uint32_t second)
{
  RTC_DateTypeDef date;
  RTC_TimeTypeDef time;

  if (rtc_ready == 0U)
  {
    return -1;
  }
  if ((year < APP_RTC_EPOCH_BASE_YEAR) || (year > (APP_RTC_EPOCH_BASE_YEAR + 199U)) ||
      (month < 1U) || (month > 12U) || (day < 1U) ||
      (day > rtc_days_in_month(year, month)) ||
      (hour > 23U) || (minute > 59U) || (second > 59U))
  {
    return -2;
  }

  date.Year = rtc_bcd(year - APP_RTC_EPOCH_BASE_YEAR);
  date.Month = rtc_bcd(month);
  date.Date = rtc_bcd(day);
  date.WeekDay = RTC_WEEKDAY_MONDAY;   /* not used by the log */
  if (HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BCD) != HAL_OK)
  {
    return -3;
  }

  time.Hours = rtc_bcd(hour);
  time.Minutes = rtc_bcd(minute);
  time.Seconds = rtc_bcd(second);
  time.TimeFormat = RTC_HOURFORMAT_24;
  time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  time.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BCD) != HAL_OK)
  {
    return -4;
  }
  return 0;
}

static uint32_t rtc_parse_num(const char **p, uint32_t digits, int *ok)
{
  uint32_t v = 0U;

  for (uint32_t i = 0U; i < digits; i++)
  {
    char c = **p;

    if ((c < '0') || (c > '9'))
    {
      *ok = 0;
      return 0U;
    }
    v = (v * 10U) + (uint32_t)(c - '0');
    (*p)++;
  }
  return v;
}

int app_rtc_set_from_string(const char *text)
{
  const char *p = text;
  int ok = 1;
  uint32_t year, month, day, hour, minute, second;

  while (*p == ' ')
  {
    p++;
  }

  year = rtc_parse_num(&p, 4U, &ok);
  if ((ok == 0) || (*p != '-'))
  {
    return -1;
  }
  p++;
  month = rtc_parse_num(&p, 2U, &ok);
  if ((ok == 0) || (*p != '-'))
  {
    return -1;
  }
  p++;
  day = rtc_parse_num(&p, 2U, &ok);
  if (ok == 0)
  {
    return -1;
  }

  while ((*p == ' ') || (*p == 'T') || (*p == 't'))
  {
    p++;
  }

  hour = rtc_parse_num(&p, 2U, &ok);
  if ((ok == 0) || (*p != ':'))
  {
    return -1;
  }
  p++;
  minute = rtc_parse_num(&p, 2U, &ok);
  if ((ok == 0) || (*p != ':'))
  {
    return -1;
  }
  p++;
  second = rtc_parse_num(&p, 2U, &ok);
  if (ok == 0)
  {
    return -1;
  }

  return app_rtc_set(year, month, day, hour, minute, second);
}

void app_rtc_format_epoch(uint32_t epoch, char *out, uint32_t size)
{
  uint32_t y, m, d;

  if ((out == NULL) || (size == 0U))
  {
    return;
  }

  rtc_civil_from_days(epoch / 86400U, &y, &m, &d);
  (void)snprintf(out, size, "%04lu-%02lu-%02lu %02lu:%02lu:%02lu",
                 (unsigned long)y, (unsigned long)m, (unsigned long)d,
                 (unsigned long)((epoch % 86400U) / 3600U),
                 (unsigned long)(((epoch % 86400U) % 3600U) / 60U),
                 (unsigned long)(epoch % 60U));
}

void app_rtc_format(char *out, uint32_t size)
{
  app_rtc_format_epoch(app_rtc_epoch(), out, size);
}

void app_rtc_report(void)
{
  char stamp[24];
  const char *src = "none";

  if (rtc_source == RTC_SRC_LSE)
  {
    src = "lse(32.768kHz)";
  }
  else if (rtc_source == RTC_SRC_LSI)
  {
    src = "lsi(~32kHz,drifts)";
  }

  if (rtc_ready == 0U)
  {
    app_log_printf("RTC ready=0 source=%s\r\n", src);
    return;
  }

  app_rtc_format(stamp, sizeof(stamp));
  app_log_printf("RTC ready=1 source=%s time=%s epoch=%lu\r\n",
                 src, stamp, (unsigned long)app_rtc_epoch());
}
