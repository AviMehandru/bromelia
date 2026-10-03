/* bro-instant.c */
#include "bro-instant.h"

#include <string.h>

static gint64
floor_div (gint64 a, gint64 b)
{
  return a / b - (a % b != 0 && (a < 0) != (b < 0) ? 1 : 0);
}

static int
days_in_month (int y, int m)
{
  if (m == 2)
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0) ? 29 : 28;
  return m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31;
}

/* Howard Hinnant's days_from_civil / civil_from_days. */
static gint64
days_from_civil (gint64 y, gint64 m, gint64 d)
{
  y -= m <= 2 ? 1 : 0;
  gint64 era = floor_div (y, 400);
  gint64 yoe = y - era * 400;
  gint64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  gint64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void
civil_from_days (gint64 z, gint64 *year, gint64 *month, gint64 *day)
{
  z += 719468;
  gint64 era = floor_div (z, 146097);
  gint64 doe = z - era * 146097;
  gint64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  gint64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  gint64 mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp + (mp < 10 ? 3 : -9);
  *year = yoe + era * 400 + (*month <= 2 ? 1 : 0);
}

typedef struct {
  const char *s;
  gsize i;
} Cursor;

static int
number (Cursor *c, int digits)
{
  int v = 0;
  for (int k = 0; k < digits; k++) {
    char ch = c->s[c->i + k];
    if (!g_ascii_isdigit (ch))
      return -1;
    v = v * 10 + (ch - '0');
  }
  c->i += digits;
  return v;
}

static gboolean
literal (Cursor *c, char ch)
{
  if (c->s[c->i] != ch)
    return FALSE;
  c->i++;
  return TRUE;
}

gboolean
bro_instant_parse (const char *text, BroInstant *out)
{
  if (!text)
    return FALSE;
  Cursor c = { text, 0 };
  int year = number (&c, 4);
  if (year < 0 || !literal (&c, '-'))
    return FALSE;
  int month = number (&c, 2);
  if (month < 1 || month > 12 || !literal (&c, '-'))
    return FALSE;
  int day = number (&c, 2);
  if (day < 1 || day > days_in_month (year, month) || !literal (&c, 'T'))
    return FALSE;
  int hour = number (&c, 2);
  if (hour < 0 || hour > 23 || !literal (&c, ':'))
    return FALSE;
  int minute = number (&c, 2);
  if (minute < 0 || minute > 59 || !literal (&c, ':'))
    return FALSE;
  int second = number (&c, 2);
  if (second < 0 || second > 59)
    return FALSE;
  int millis = 0;
  if (literal (&c, '.')) {
    int digits = 0;
    while (g_ascii_isdigit (text[c.i])) {
      if (digits < 3)
        millis = millis * 10 + (text[c.i] - '0');
      digits++;
      c.i++;
    }
    if (digits == 0)
      return FALSE;
    for (int k = digits; k < 3; k++)
      millis *= 10;
  }
  int offset_minutes = 0;
  if (!literal (&c, 'Z')) {
    int sign = literal (&c, '+') ? 1 : literal (&c, '-') ? -1 : 0;
    if (sign == 0)
      return FALSE;
    int oh = number (&c, 2);
    if (oh < 0 || oh > 23 || !literal (&c, ':'))
      return FALSE;
    int om = number (&c, 2);
    if (om < 0 || om > 59)
      return FALSE;
    offset_minutes = sign * (oh * 60 + om);
  }
  if (text[c.i] != '\0')
    return FALSE;
  gint64 seconds = days_from_civil (year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offset_minutes * 60;
  out->unix_milliseconds = seconds * 1000 + millis;
  return TRUE;
}

char *
bro_instant_format (BroInstant instant)
{
  gint64 ms = instant.unix_milliseconds;
  gint64 days = floor_div (ms, 86400000);
  gint64 in_day = ms - days * 86400000;
  gint64 y, m, d;
  civil_from_days (days, &y, &m, &d);
  return g_strdup_printf ("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", (int) y, (int) m, (int) d, (int) (in_day / 3600000),
                          (int) (in_day / 60000 % 60), (int) (in_day / 1000 % 60), (int) (in_day % 1000));
}
