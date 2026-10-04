/* test-power.inc: BroPlatformPowerManager, included by test-adapters.c. Where logind runs (a Linux desktop, the Ubuntu VM)
 * a guard's inhibitor is listed until it is released; where it doesn't (macOS, CI's container) it is power.unavailable. */

#include <gio/gio.h>

/* Bromelia's inhibitors held by this process: their "what". NULL when logind can't be asked. */
static GPtrArray *
our_inhibitors (void)
{
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariantIter) iter = NULL;
  const char *what, *who, *why, *mode;
  guint32 uid, pid;
  GPtrArray *found;
  if (!bus)
    return NULL;
  reply = g_dbus_connection_call_sync (bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                                       "ListInhibitors", NULL, G_VARIANT_TYPE ("(a(ssssuu))"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);
  if (!reply)
    return NULL;
  found = g_ptr_array_new_with_free_func (g_free);
  g_variant_get (reply, "(a(ssssuu))", &iter);
  while (g_variant_iter_next (iter, "(&s&s&s&suu)", &what, &who, &why, &mode, &uid, &pid))
    if (pid == (guint32) getpid () && g_str_equal (who, "Bromelia") && g_str_equal (mode, "block"))
      g_ptr_array_add (found, g_strdup_printf ("%s %s", what, why));
  return found;
}

static void
test_power_manager (void)
{
  g_autoptr (BroPlatformPowerManager) power = bro_platform_power_manager_new ();
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (GPtrArray) before = our_inhibitors ();
  g_autoptr (GPtrArray) held = NULL;
  g_autoptr (GPtrArray) after = NULL;
  g_autofree char *saved = g_strdup (g_getenv ("DBUS_SYSTEM_BUS_ADDRESS"));
  BroPowerGuard *one, *two;

  if (!before)
    {
      one = bro_power_manager_inhibit (BRO_POWER_MANAGER (power), "Ripping", &error);
      g_assert_null (one);
      g_assert_cmpstr (error->code, ==, "power.unavailable");
      g_test_message ("no logind here: %s", bro_json_value_get_string (bro_json_value_member (error->params, "reason"), NULL));
      return;
    }
  g_assert_cmpuint (before->len, ==, 0);
  one = bro_power_manager_inhibit (BRO_POWER_MANAGER (power), "Ripping", &error);
  g_assert_null (error);
  two = bro_power_manager_inhibit (BRO_POWER_MANAGER (power), "Verifying", &error);
  g_assert_null (error);
  held = our_inhibitors ();
  g_assert_cmpuint (held->len, ==, 2);
  for (guint i = 0; i < held->len; i++)
    {
      const char *line = held->pdata[i];
      g_test_message ("held: %s", line);
      g_assert_true (g_str_has_prefix (line, "sleep:idle ") || g_str_has_prefix (line, "idle "));
    }
  bro_power_guard_release (one);
  bro_power_guard_release (one); /* a second release does nothing */
  g_object_unref (one);
  g_clear_pointer (&held, g_ptr_array_unref);
  held = our_inhibitors ();
  g_assert_cmpuint (held->len, ==, 1);
  g_assert_true (g_str_has_suffix (held->pdata[0], " Verifying"));
  g_object_unref (two); /* dropping the last reference releases it too */
  after = our_inhibitors ();
  g_assert_cmpuint (after->len, ==, 0);

  /* No system bus: power.unavailable with the reason. */
  g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/bromelia-bus", TRUE);
  one = bro_power_manager_inhibit (BRO_POWER_MANAGER (power), "Ripping", &error);
  if (saved)
    g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", saved, TRUE);
  else
    g_unsetenv ("DBUS_SYSTEM_BUS_ADDRESS");
  g_assert_null (one);
  g_assert_cmpstr (error->code, ==, "power.unavailable");
  g_assert_nonnull (bro_json_value_get_string (bro_json_value_member (error->params, "reason"), NULL));
}
