/* bro-message-catalog.h: BroMessageCatalog, known MakeMKV message codes → severity and notices. */
#pragma once

#include "bro-makemkv-notice.h"
#include "bro-message-kind.h"
#include "bro-robot-message.h"

G_BEGIN_DECLS

/* Debug for code 1003 and debug-flagged "DEBUG…" text; error and warning by flag (0x200 / 0x400) or known code;
 * info otherwise. */
BroMessageKind bro_message_catalog_severity (const BroRobotMessage *message);

/* NULL when the message isn't a notice. */
BroMakemkvNotice *bro_message_catalog_notice (const BroRobotMessage *message);

G_END_DECLS
