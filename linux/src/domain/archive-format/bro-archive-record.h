/* bro-archive-record.h: an archive record (bromelia-<unit8>.json, version 3; today's bromelia.json, version 2):
 * the document as read (key order kept) and the fields Bromelia reads back: status, name, kind, the disc's label
 * and place in its set (-1: not in the record), its fingerprint, the unit's id (version 3), the episodes' numbers
 * and the files SHA256SUMS lists. Fields with the same meaning have the same path in both versions
 * (archive-record-3.json). */
#pragma once

#include "bro-json-value.h"
#include "bro-sum-entry.h"

G_BEGIN_DECLS

typedef struct {
  int version;
  BroJsonValue *document;
  char *status;
  char *name;
  char *kind;
  char *label;
  char *volume_name;
  int season, part, volume, disc;
  char *fingerprint; /* nullable */
  char *unit_id;     /* nullable */
  GArray *episodes;  /* int */
  GPtrArray *files;  /* BroSumEntry * */
} BroArchiveRecord;

void bro_archive_record_free (BroArchiveRecord *record);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BroArchiveRecord, bro_archive_record_free)

G_END_DECLS
