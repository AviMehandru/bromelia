/* bro-robot.c */
#include "bro-robot.h"
#include "bro-robot-private.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

GStrv
bro_robot_split_fields (const char *body)
{
  GPtrArray *fields = g_ptr_array_new ();
  GString *cur = g_string_new (NULL);
  gboolean in_quotes = FALSE, was_quoted = FALSE;
  for (const char *p = body; *p; p++) {
    char c = *p;
    if (in_quotes) {
      if (c == '\\' && p[1] != '\0') {
        if (p[1] == '"' || p[1] == '\\') {
          g_string_append_c (cur, p[1]);
          p++;
        } else {
          g_string_append_c (cur, c);
        }
      } else if (c == '"') {
        in_quotes = FALSE;
      } else {
        g_string_append_c (cur, c);
      }
    } else if (c == ',') {
      g_ptr_array_add (fields, g_string_free (cur, FALSE));
      cur = g_string_new (NULL);
      was_quoted = FALSE;
    } else if (c == '"' && cur->len == 0 && !was_quoted) {
      in_quotes = TRUE;
      was_quoted = TRUE;
    } else {
      g_string_append_c (cur, c);
    }
  }
  g_ptr_array_add (fields, g_string_free (cur, FALSE));
  g_ptr_array_add (fields, NULL);
  return (GStrv) g_ptr_array_free (fields, FALSE);
}

gboolean
_bro_robot_parse_int (const char *text, int *out)
{
  while (*text == ' ' || *text == '\t')
    text++;
  const char *p = text;
  if (*p == '-' || *p == '+')
    p++;
  if (!g_ascii_isdigit (*p))
    return FALSE;
  while (g_ascii_isdigit (*p))
    p++;
  const char *end = p;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '\0')
    return FALSE;
  g_autofree char *digits = g_strndup (text, end - text);
  gint64 v = g_ascii_strtoll (digits, NULL, 10);
  if (v < INT_MIN || v > INT_MAX)
    return FALSE;
  *out = (int) v;
  return TRUE;
}

static BroRobotEvent *
raw (const char *line)
{
  BroRobotEvent *e = bro_robot_event_new (BRO_ROBOT_EVENT_RAW);
  e->text = g_strdup (line);
  return e;
}

#define F(i) (f[i])

static BroRobotEvent *
parse_record (const char *tag, const char *body)
{
  g_auto (GStrv) f = bro_robot_split_fields (body);
  guint n = g_strv_length (f);
  int a, b, c, d;
  BroRobotEvent *e = NULL;
  if (strcmp (tag, "MSG") == 0) {
    if (n >= 5 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_MESSAGE);
      BroRobotMessage *m = g_new0 (BroRobotMessage, 1);
      m->code = a;
      m->flags = b;
      if (!_bro_robot_parse_int (F (2), &m->count))
        m->count = 0;
      m->text = g_strdup (F (3));
      m->format = g_strdup (F (4));
      m->params = g_strdupv (f + 5);
      e->message = m;
    }
  } else if (strcmp (tag, "PRGC") == 0 || strcmp (tag, "PRGT") == 0) {
    if (n >= 3 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b)) {
      e = bro_robot_event_new (tag[3] == 'C' ? BRO_ROBOT_EVENT_PROGRESS_CURRENT : BRO_ROBOT_EVENT_PROGRESS_TOTAL);
      e->code = a;
      e->id = b;
      e->name = g_strdup (F (2));
    }
  } else if (strcmp (tag, "PRGV") == 0) {
    if (n >= 3 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b) && _bro_robot_parse_int (F (2), &c)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_PROGRESS_VALUE);
      e->current = a;
      e->total = b;
      e->max = c;
    }
  } else if (strcmp (tag, "DRV") == 0) {
    if (n >= 7 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b) && _bro_robot_parse_int (F (3), &c)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_DRIVE);
      e->index = a;
      e->state = b;
      e->flags = c;
      e->identification = g_strdup (F (4));
      e->label = g_strdup (F (5));
      e->device = g_strdup (F (6));
    }
  } else if (strcmp (tag, "TCOUNT") == 0) {
    if (_bro_robot_parse_int (body, &a)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_TITLE_COUNT);
      e->count = a;
    }
  } else if (strcmp (tag, "CINFO") == 0) {
    if (n >= 3 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_DISC_INFO);
      e->id = a;
      e->code = b;
      e->value = g_strdup (F (2));
    }
  } else if (strcmp (tag, "TINFO") == 0) {
    if (n >= 4 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b) && _bro_robot_parse_int (F (2), &c)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_TITLE_INFO);
      e->title = a;
      e->id = b;
      e->code = c;
      e->value = g_strdup (F (3));
    }
  } else if (strcmp (tag, "SINFO") == 0) {
    if (n >= 5 && _bro_robot_parse_int (F (0), &a) && _bro_robot_parse_int (F (1), &b) && _bro_robot_parse_int (F (2), &c)
        && _bro_robot_parse_int (F (3), &d)) {
      e = bro_robot_event_new (BRO_ROBOT_EVENT_STREAM_INFO);
      e->title = a;
      e->stream = b;
      e->id = c;
      e->code = d;
      e->value = g_strdup (F (4));
    }
  }
  return e;
}

BroRobotEvent *
bro_robot_parse_line (const char *line)
{
  if (!line)
    return NULL;
  gsize len = strlen (line);
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
    len--;
  if (len == 0)
    return NULL;
  g_autofree char *text = g_strndup (line, len);
  char *colon = strchr (text, ':');
  if (!colon)
    return raw (text);
  g_autofree char *tag = g_strndup (text, colon - text);
  BroRobotEvent *e = parse_record (tag, colon + 1);
  return e ? e : raw (text);
}

char *
_bro_robot_ascii_lower (const char *text)
{
  return g_ascii_strdown (text ? text : "", -1);
}
