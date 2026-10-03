/* bro-cd-ripper-args.h: BroCdRipperArgs, the audio CD command. */
#pragma once

#include "bro-command-line.h"
#include "bro-json-value.h"

G_BEGIN_DECLS

/* The profile's otherDiscs.audioCommand ({device} is the drive; its program is resolved by the adapter), else
 * cyanrip, else abcde, when @available (the tools found, NULL-terminated) has it; NULL when there is none. The
 * command runs in the output folder. */
BroCommandLine *bro_cd_ripper_args_build (BroJsonValue *other_discs, const char *device, const char *const *available);

G_END_DECLS
