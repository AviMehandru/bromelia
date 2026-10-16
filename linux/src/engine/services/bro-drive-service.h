/* bro-drive-service.h: BroDriveService: the optical drives (plan §11.1, §15.2), used on the executor. A scan (MakeMKV's
 * drive list and the OS's, on a worker, one at a time) runs at start, every automation.pollIntervalSeconds, on the OS's
 * device events and after a tray command; while an acquisition lease is held only asked-for scans run, unless
 * automation.pollWhileRipping. DriveJoin pairs the two lists; a drive's id comes from MakeMKV's identification. Each
 * drive counts its discs: a disc that arrives gets the next generation and mediumArrived (not the discs already in at
 * start, which never start a rip), one that leaves mediumRemoved. A drive a job holds keeps what it had (MakeMKV can't
 * see a drive in use), and a drive left out of a scan keeps it until the OS says it's gone. Configured drives that
 * aren't connected stay listed (connected false); others are removed. Close tray checks, in the first scan 4 s later,
 * that the tray closed (drive.trayStillOpen: a drive without a tray motor). As the BroDriveLookup of the steps it gives
 * a snapshot, from any thread. shared/fixtures/engine/drive-service.cases.json. */
#pragma once

#include "bro-config-source.h"
#include "bro-device-monitor.h"
#include "bro-drive-control.h"
#include "bro-drive-dto.h"
#include "bro-drive-lookup.h"
#include "bro-drive-repository.h"
#include "bro-engine-clock.h"
#include "bro-engine-executor.h"
#include "bro-event-bus.h"
#include "bro-makemkv.h"
#include "bro-problem-service.h"

G_BEGIN_DECLS

#define BRO_TYPE_DRIVE_SERVICE (bro_drive_service_get_type ())
G_DECLARE_FINAL_TYPE (BroDriveService, bro_drive_service, BRO, DRIVE_SERVICE, GObject)

/* Runs on the executor, never inside the call that started the work: @error is NULL when it succeeded. */
typedef void (*BroDriveDone) (const BroBroError *error, gpointer data);

BroDriveService *bro_drive_service_new (BroEngineExecutor *executor, BroEventBus *bus, BroProblemService *problems, BroConfigSource *config,
                                        BroEngineClock *clock, BroDeviceMonitor *monitor, BroDriveControl *control, BroMakemkv *makemkv,
                                        BroDriveRepository *repository);

/* Reads the stored drives, follows leases, the configuration and the OS's events, and scans; @done runs when the first
 * scan is applied. @destroy frees @data afterwards. */
void bro_drive_service_start (BroDriveService *self, BroDriveDone done, gpointer data, GDestroyNotify destroy);

/* Stops the monitor, the timers and the subscriptions. */
void bro_drive_service_stop (BroDriveService *self);

/* The connected drives in MakeMKV's order, then the configured ones that aren't connected (on the executor):
 * BroDriveDto *, the caller frees the array. */
GPtrArray *bro_drive_service_drives (BroDriveService *self);

/* Scans now (also while an acquisition runs); @done runs when that scan is applied. */
void bro_drive_service_rescan (BroDriveService *self, BroDriveDone done, gpointer data, GDestroyNotify destroy);

/* Ejects the drive's disc (drive.notFound when it isn't connected; the DriveControl's failure, such as
 * drive.ejectFailed), then scans 3 s later. */
void bro_drive_service_eject (BroDriveService *self, const char *drive_id, BroDriveDone done, gpointer data, GDestroyNotify destroy);

/* Closes the drive's tray, then scans 5 s later; the first scan 4 s after the command raises drive.trayStillOpen when
 * the tray is still open. */
void bro_drive_service_close_tray (BroDriveService *self, const char *drive_id, BroDriveDone done, gpointer data, GDestroyNotify destroy);

/* Closes the tray of every connected drive without a disc, one after another, then scans 5 s later. */
void bro_drive_service_close_all (BroDriveService *self, BroDriveDone done, gpointer data, GDestroyNotify destroy);

G_END_DECLS
