/* bro-sleep.h — keeping the computer awake while jobs or background steps run, without GTK. */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _BroSleepInhibitor BroSleepInhibitor;

/* Something that can keep the computer from sleeping. set() is called on the main thread, only when the wanted
 * state changes. problem() (optional, may be NULL) is told once, on the main thread, why it can't. */
struct _BroSleepInhibitor {
  void (*set) (BroSleepInhibitor *self, gboolean on);
  void (*free) (BroSleepInhibitor *self);
  void (*problem) (const char *message, gpointer data);
  gpointer problem_data;
};

/* systemd-logind over the system bus: Inhibit("sleep:idle", "Bromelia", "Ripping discs", "block"), holding the
 * returned file descriptor while on. Where logind isn't there (containers, other systems) it reports a problem once. */
BroSleepInhibitor *bro_sleep_inhibitor_logind_new (void);
void               bro_sleep_inhibitor_free (BroSleepInhibitor *self);

G_END_DECLS
