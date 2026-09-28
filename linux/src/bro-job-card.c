/* bro-job-card.c — progress card for one job. */
#include "bro-pages.h"
#include "bro-ui-util.h"

typedef struct {
  BroJob *job;
  GtkWidget *root;
  GtkWidget *title, *state, *phase, *timing, *overall, *overall_label, *current, *current_label;
  GtkWidget *progress_box, *error, *summary, *buttons, *log_revealer;
  char *last_buttons;
  gulong handler;
} Card;

static void card_update (Card *c);

static void
card_free (Card *c)
{
  if (c->handler)
    g_signal_handler_disconnect (c->job, c->handler);
  g_object_unref (c->job);
  g_free (c->last_buttons);
  g_free (c);
}

static void
on_cancel (GtkButton *b, Card *c) { bro_state_cancel_job (bro_app_state (), c->job); }
static void
on_start_now (GtkButton *b, Card *c) { bro_state_start_now (bro_app_state (), c->job); }
static void
on_retry (GtkButton *b, Card *c) { bro_state_retry (bro_app_state (), c->job); }
static void
on_up (GtkButton *b, Card *c) { bro_state_move_job (bro_app_state (), c->job, -1); }
static void
on_down (GtkButton *b, Card *c) { bro_state_move_job (bro_app_state (), c->job, 1); }
static void
on_open (GtkButton *b, Card *c) { if (c->job->output_dir) bro_open_path (GTK_WIDGET (b), c->job->output_dir); }

static void
on_open_log (GtkButton *b, Card *c)
{
  g_autofree char *p = bro_job_log_path (c->job);
  bro_open_path (GTK_WIDGET (b), p);
}

static void
on_copy (GtkButton *b, Card *c)
{
  g_ptr_array_add (c->job->commands, NULL);
  g_autofree char *text = g_strjoinv ("\n", (char **) c->job->commands->pdata);
  g_ptr_array_remove_index (c->job->commands, c->job->commands->len - 1);
  bro_copy_text (GTK_WIDGET (b), text);
}

static void
on_toggle_log (GtkToggleButton *t, Card *c)
{
  gtk_revealer_set_reveal_child (GTK_REVEALER (c->log_revealer), gtk_toggle_button_get_active (t));
}

static GtkWidget *
button (const char *label, GCallback cb, Card *c)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  g_signal_connect (b, "clicked", cb, c);
  return b;
}

static void
build_buttons (Card *c)
{
  GtkWidget *child;
  GtkWidget *log_toggle;
  while ((child = gtk_widget_get_first_child (c->buttons)))
    gtk_box_remove (GTK_BOX (c->buttons), child);
  switch (c->job->state)
    {
    case BRO_JOB_RUNNING:
      gtk_box_append (GTK_BOX (c->buttons), button ("Cancel", G_CALLBACK (on_cancel), c));
      break;
    case BRO_JOB_WAITING:
      gtk_box_append (GTK_BOX (c->buttons), button ("Start Now", G_CALLBACK (on_start_now), c));
      gtk_box_append (GTK_BOX (c->buttons), button ("Cancel", G_CALLBACK (on_cancel), c));
      break;
    case BRO_JOB_QUEUED:
      gtk_box_append (GTK_BOX (c->buttons), button ("Remove", G_CALLBACK (on_cancel), c));
      gtk_box_append (GTK_BOX (c->buttons), button ("Move Up", G_CALLBACK (on_up), c));
      gtk_box_append (GTK_BOX (c->buttons), button ("Move Down", G_CALLBACK (on_down), c));
      break;
    case BRO_JOB_FAILED:
    case BRO_JOB_CANCELLED:
    case BRO_JOB_COMPLETED_WITH_ERRORS:
      gtk_box_append (GTK_BOX (c->buttons), button ("Retry", G_CALLBACK (on_retry), c));
      break;
    default:
      break;
    }
  if (c->job->output_dir)
    gtk_box_append (GTK_BOX (c->buttons), button ("Open Folder", G_CALLBACK (on_open), c));
  log_toggle = gtk_toggle_button_new_with_label ("Log");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (log_toggle), gtk_revealer_get_reveal_child (GTK_REVEALER (c->log_revealer)));
  g_signal_connect (log_toggle, "toggled", G_CALLBACK (on_toggle_log), c);
  gtk_box_append (GTK_BOX (c->buttons), log_toggle);
  gtk_box_append (GTK_BOX (c->buttons), button ("Open Log File", G_CALLBACK (on_open_log), c));
  gtk_box_append (GTK_BOX (c->buttons), button ("Copy Commands", G_CALLBACK (on_copy), c));
}

static void
card_update (Card *c)
{
  BroJob *j = c->job;
  g_autofree char *title = bro_job_title (j);
  g_autofree char *head = g_strdup_printf ("%s   ·   %s%s", title, bro_rip_mode_short (j->mode), j->automatic ? " · automatic" : "");
  gboolean live = j->state == BRO_JOB_RUNNING || j->state == BRO_JOB_WAITING || j->state == BRO_JOB_QUEUED;
  g_autofree char *buttons_key = g_strdup_printf ("%d|%d", j->state, j->output_dir != NULL);

  gtk_label_set_text (GTK_LABEL (c->title), head);
  gtk_label_set_text (GTK_LABEL (c->state), bro_job_state_label (j->state));
  gtk_widget_set_visible (c->progress_box, live);
  if (live)
    {
      double overall = bro_job_overall (j);
      g_autofree char *ov = g_strdup_printf ("%s — %d%%", *j->total_operation ? j->total_operation : "Overall", (int) (overall * 100));
      if (j->state == BRO_JOB_WAITING)
        {
          g_autofree char *cd = g_strdup_printf ("Starting in %d s", (int) MAX (0, j->start_at - g_get_real_time () / G_USEC_PER_SEC));
          gtk_label_set_text (GTK_LABEL (c->phase), cd);
        }
      else
        gtk_label_set_text (GTK_LABEL (c->phase), j->phase);
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (c->overall), overall);
      gtk_label_set_text (GTK_LABEL (c->overall_label), ov);
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (c->current), CLAMP (j->current, 0, 1));
      gtk_label_set_text (GTK_LABEL (c->current_label), j->operation);
      if (j->state == BRO_JOB_RUNNING)
        {
          g_autofree char *el = bro_format_elapsed (bro_job_elapsed (j));
          gint64 rem = bro_job_remaining (j);
          g_autofree char *re = rem >= 0 ? bro_format_elapsed (rem) : NULL;
          g_autofree char *t = re ? g_strdup_printf ("Elapsed %s · %s left", el, re) : g_strdup_printf ("Elapsed %s", el);
          gtk_label_set_text (GTK_LABEL (c->timing), t);
        }
      else
        gtk_label_set_text (GTK_LABEL (c->timing), "");
    }
  gboolean problem = j->state == BRO_JOB_FAILED || j->state == BRO_JOB_CANCELLED || j->state == BRO_JOB_COMPLETED_WITH_ERRORS;
  gtk_label_set_text (GTK_LABEL (c->error), problem && j->error ? j->error : "");
  gtk_widget_set_visible (c->error, problem && j->error);
  if (j->state == BRO_JOB_COMPLETED_WITH_ERRORS)
    {
      gtk_widget_remove_css_class (c->error, "error");
      gtk_widget_add_css_class (c->error, "warning");
    }
  else
    {
      gtk_widget_remove_css_class (c->error, "warning");
      gtk_widget_add_css_class (c->error, "error");
    }
  if (bro_job_state_finished (j->state))
    {
      g_autofree char *el = bro_format_elapsed (bro_job_elapsed (j));
      g_autofree char *sum = g_strdup_printf ("%u item(s) · %s · %d warning(s) · %d error(s)", j->files->len, el, j->warnings, j->errors);
      gtk_label_set_text (GTK_LABEL (c->summary), sum);
    }
  gtk_widget_set_visible (c->summary, bro_job_state_finished (j->state));
  if (g_strcmp0 (buttons_key, c->last_buttons) != 0)
    {
      g_free (c->last_buttons);
      c->last_buttons = g_steal_pointer (&buttons_key);
      build_buttons (c);
    }
}

GtkWidget *
bro_job_card_new (BroJob *job)
{
  Card *c = g_new0 (Card, 1);
  GtkWidget *header, *phase_row, *log;
  c->job = g_object_ref (job);
  c->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (c->root, "card");
  gtk_widget_add_css_class (c->root, "job-card");
  gtk_widget_set_valign (c->root, GTK_ALIGN_START);

  header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  c->title = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (c->title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (c->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (c->title, TRUE);
  gtk_widget_add_css_class (c->title, "heading");
  c->state = bro_tag ("", NULL);
  gtk_box_append (GTK_BOX (header), c->title);
  gtk_box_append (GTK_BOX (header), c->state);

  c->progress_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  phase_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  c->phase = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (c->phase), 0);
  gtk_widget_set_hexpand (c->phase, TRUE);
  c->timing = gtk_label_new (NULL);
  gtk_widget_add_css_class (c->timing, "caption");
  gtk_widget_add_css_class (c->timing, "numeric");
  gtk_box_append (GTK_BOX (phase_row), c->phase);
  gtk_box_append (GTK_BOX (phase_row), c->timing);
  c->overall = gtk_progress_bar_new ();
  c->overall_label = bro_caption ("");
  c->current = gtk_progress_bar_new ();
  gtk_widget_set_opacity (c->current, 0.7);
  c->current_label = bro_caption ("");
  gtk_box_append (GTK_BOX (c->progress_box), phase_row);
  gtk_box_append (GTK_BOX (c->progress_box), c->overall);
  gtk_box_append (GTK_BOX (c->progress_box), c->overall_label);
  gtk_box_append (GTK_BOX (c->progress_box), c->current);
  gtk_box_append (GTK_BOX (c->progress_box), c->current_label);

  c->error = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (c->error), TRUE);
  gtk_label_set_xalign (GTK_LABEL (c->error), 0);
  gtk_label_set_selectable (GTK_LABEL (c->error), TRUE);
  gtk_widget_add_css_class (c->error, "error");
  c->summary = bro_caption ("");
  c->buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (c->buttons, "compact");

  c->log_revealer = gtk_revealer_new ();
  log = bro_log_view_new (G_OBJECT (job), job->log);
  gtk_widget_set_size_request (log, -1, 220);
  gtk_widget_set_vexpand (log, FALSE);
  gtk_revealer_set_child (GTK_REVEALER (c->log_revealer), log);

  gtk_box_append (GTK_BOX (c->root), header);
  gtk_box_append (GTK_BOX (c->root), c->progress_box);
  gtk_box_append (GTK_BOX (c->root), c->error);
  gtk_box_append (GTK_BOX (c->root), c->summary);
  gtk_box_append (GTK_BOX (c->root), c->buttons);
  gtk_box_append (GTK_BOX (c->root), c->log_revealer);

  c->handler = g_signal_connect_swapped (job, "changed", G_CALLBACK (card_update), c);
  g_object_set_data_full (G_OBJECT (c->root), "bro-card", c, (GDestroyNotify) card_free);
  card_update (c);
  return c->root;
}
