/* bro-drive-repository.c */
#include "bro-drive-repository.h"

G_DEFINE_INTERFACE (BroDriveRepository, bro_drive_repository, G_TYPE_OBJECT)

static void
bro_drive_repository_default_init (BroDriveRepositoryInterface *iface)
{
}

gboolean
bro_drive_repository_upsert (BroDriveRepository *self, const BroDriveRecord *drive, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_DRIVE_REPOSITORY (self), FALSE);
  return BRO_DRIVE_REPOSITORY_GET_IFACE (self)->upsert (self, drive, error);
}

GPtrArray *
bro_drive_repository_all (BroDriveRepository *self, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_DRIVE_REPOSITORY (self), NULL);
  return BRO_DRIVE_REPOSITORY_GET_IFACE (self)->all (self, error);
}

gboolean
bro_drive_repository_record_stats (BroDriveRepository *self, const char *drive_id, const char *day, const BroDriveStats *stats, BroBroError **error)
{
  g_return_val_if_fail (BRO_IS_DRIVE_REPOSITORY (self), FALSE);
  return BRO_DRIVE_REPOSITORY_GET_IFACE (self)->record_stats (self, drive_id, day, stats, error);
}
