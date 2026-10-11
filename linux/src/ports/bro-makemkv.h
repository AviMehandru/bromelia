/* bro-makemkv.h: BroMakemkv: makemkvcon (plan §10.1): the drive scan, a disc's listing, MKV rips and backups. Every
 * call isolates the run's settings, reports each robot event to the sink and classifies the run (RunOutcome).
 * Synchronous: call it from a worker thread (plan §6). BroMakemkvTool (Adapters) implements it. */
#pragma once

#include "bro-bro-error.h"
#include "bro-cancellation-token.h"
#include "bro-listing-run.h"
#include "bro-makemkv-invocation.h"
#include "bro-makemkv-run.h"
#include "bro-makemkv-source.h"
#include "bro-run-sink.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define BRO_TYPE_MAKEMKV (bro_makemkv_get_type ())
G_DECLARE_INTERFACE (BroMakemkv, bro_makemkv, BRO, MAKEMKV, GObject)

struct _BroMakemkvInterface {
  GTypeInterface parent_iface;
  GPtrArray *(*scan_drives) (BroMakemkv *self, BroCancellationToken *cancel, BroBroError **error);
  BroListingRun *(*listing) (BroMakemkv *self, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation,
                             BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error);
  BroMakemkvRun *(*rip) (BroMakemkv *self, const BroMakemkvSource *source, const char *title, const char *destination,
                         const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error);
  BroMakemkvRun *(*backup) (BroMakemkv *self, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
                            const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error);
};

/* Every drive MakeMKV sees (BroMakemkvDrive *, DRV lines). NULL and @error set when makemkvcon can't be run. */
GPtrArray *bro_makemkv_scan_drives (BroMakemkv *self, BroCancellationToken *cancel, BroBroError **error);

BroListingRun *bro_makemkv_listing (BroMakemkv *self, const BroMakemkvSource *source, const BroMakemkvInvocation *invocation,
                                    BroRunSink *sink, BroCancellationToken *cancel, BroBroError **error);

/* @title: a title index or "all". */
BroMakemkvRun *bro_makemkv_rip (BroMakemkv *self, const BroMakemkvSource *source, const char *title, const char *destination,
                                const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel,
                                BroBroError **error);

/* A disc structure or an image at @destination, which must not exist yet (fs.alreadyExists). */
BroMakemkvRun *bro_makemkv_backup (BroMakemkv *self, const BroMakemkvSource *source, gboolean decrypt, const char *destination,
                                   const BroMakemkvInvocation *invocation, BroRunSink *sink, BroCancellationToken *cancel,
                                   BroBroError **error);

G_END_DECLS
