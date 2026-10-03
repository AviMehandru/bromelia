/* bro-robot.h: BroRobot, makemkvcon's robot mode (-r): lines → events. */
#pragma once

#include "bro-robot-event.h"

G_BEGIN_DECLS

/* Splits comma-separated fields; a quoted field may contain commas and \" / \\ escapes. Free with g_strfreev. */
GStrv bro_robot_split_fields (const char *body);

/* One line of output. NULL for an empty line; a RAW event for a line that isn't a robot record (or is one with
 * fields missing). */
BroRobotEvent *bro_robot_parse_line (const char *line);

G_END_DECLS
