/* bro-step-sources.h: BroStepSources: what the video-disc steps share about a job's source: its request details,
 * where makemkvcon opens it, the mode it would get before Plan, and the disc's label before there is a listing. */
#pragma once

#include "bro-disc-request.h"
#include "bro-drive-lookup.h"
#include "bro-effective-profile.h"
#include "bro-makemkv-source.h"
#include "bro-rip-mode.h"
#include "bro-step-context.h"

G_BEGIN_DECLS

/* The job's BroDiscRequest; NULL and internal.unexpected when it names no source. */
BroDiscRequest *bro_step_sources_request (const BroJobSnapshot *job, BroBroError **error);

/* A drive's location now; NULL and drive.notFound when it isn't connected. */
BroDriveLocation *bro_step_sources_drive (BroDriveLookup *lookup, const char *drive_id, BroBroError **error);

/* Where makemkvcon opens the source: disc:N for a drive (drive.notFound when it isn't connected or MakeMKV hasn't
 * numbered it), the image, or the disc a folder or file belongs to (bro_source_resolver_source). */
BroMakemkvSource *bro_step_sources_makemkv (const BroDiscSource *source, BroDriveLookup *lookup, BroBroError **error);

/* The profile before Plan: the drive entry's (@config_id nullable), with no facts yet but whether the rip is
 * automatic. */
BroEffectiveProfile *bro_step_sources_profile (const BroStepContext *context, const char *config_id);

/* The mode before Plan: the request's (options.mode), else the profile's mode.default. */
BroRipMode bro_step_sources_base_mode (const BroJobSnapshot *job, const BroEffectiveProfile *profile);

/* The disc's label before a listing names it: the drive's disc label (@drive nullable), an image's name without its
 * extension, or the folder's (or file's folder's) name. Free with g_free. */
char *bro_step_sources_label (const BroDiscSource *source, const BroDriveLocation *drive);

G_END_DECLS
