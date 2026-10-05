/* test-http.inc: the HTTP adapters, included by test-adapters.c: http-client.cases.json (when libsoup is built),
 * notification-sender.cases.json and beta-key-source.cases.json. */

/* ---- a tiny HTTP/1.1 server on 127.0.0.1 with the routes of http-client.cases.json ---- */

#ifdef BRO_HAVE_LIBSOUP

static gpointer
serve_request (gpointer data)
{
  g_autoptr (GSocketConnection) connection = data;
  GInputStream *in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  GOutputStream *out = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  g_autoptr (GString) head = g_string_new (NULL);
  g_autofree char *header = NULL;
  g_autofree char *body = NULL;
  g_autofree char *text = NULL;
  g_autofree char *reply = NULL;
  g_auto (GStrv) lines = NULL;
  g_auto (GStrv) parts = NULL;
  gsize length = 0, got = 0;
  int status = 200;
  char c;
  while (!g_str_has_suffix (head->str, "\r\n\r\n"))
    {
      if (g_input_stream_read (in, &c, 1, NULL, NULL) != 1)
        return NULL;
      g_string_append_c (head, c);
    }
  lines = g_strsplit (head->str, "\r\n", -1);
  parts = g_strsplit (lines[0], " ", 3);
  for (guint i = 1; lines[i]; i++)
    {
      const char *colon = strchr (lines[i], ':');
      g_autofree char *name = NULL;
      if (!colon)
        continue;
      name = g_ascii_strdown (lines[i], colon - lines[i]);
      if (g_str_equal (name, "x-test"))
        header = g_strstrip (g_strdup (colon + 1));
      if (g_str_equal (name, "content-length"))
        length = (gsize) g_ascii_strtoull (colon + 1, NULL, 10);
    }
  body = g_malloc0 (length + 1);
  while (got < length)
    {
      gssize n = g_input_stream_read (in, body + got, length - got, NULL, NULL);
      if (n <= 0)
        break;
      got += (gsize) n;
    }
  if (g_str_equal (parts[1], "/echo"))
    {
      g_autoptr (BroJsonValue) v = bro_json_value_new_object ();
      bro_json_value_set (v, "method", bro_json_value_new_string (parts[0]));
      bro_json_value_set (v, "path", bro_json_value_new_string ("/echo"));
      bro_json_value_set (v, "header", header ? bro_json_value_new_string (header) : bro_json_value_new_null ());
      bro_json_value_set (v, "body", bro_json_value_new_string (body));
      text = bro_json_value_to_text (v);
    }
  else if (g_str_has_prefix (parts[1], "/status/"))
    {
      status = atoi (parts[1] + strlen ("/status/"));
      text = g_strdup_printf ("status %d", status);
    }
  else
    {
      g_usleep (5 * G_USEC_PER_SEC);
      text = g_strdup ("slow");
    }
  reply = g_strdup_printf ("HTTP/1.1 %d X\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", status, strlen (text), text);
  g_output_stream_write_all (out, reply, strlen (reply), NULL, NULL, NULL);
  return NULL;
}

/* Accepts until the socket is closed; each connection gets a thread of its own (no main loop runs in these tests). */
static gpointer
accept_loop (gpointer data)
{
  GSocket *listener = data;
  GSocket *client;
  while ((client = g_socket_accept (listener, NULL, NULL)) != NULL)
    {
      GSocketConnection *connection = g_socket_connection_factory_create_connection (client);
      g_object_unref (client);
      g_thread_unref (g_thread_new ("serve", serve_request, connection));
    }
  g_object_unref (listener);
  return NULL;
}

static GSocket *
start_server (guint16 *port)
{
  GSocket *listener = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
  g_autoptr (GInetAddress) loopback = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  g_autoptr (GSocketAddress) address = g_inet_socket_address_new (loopback, 0);
  g_autoptr (GSocketAddress) bound = NULL;
  g_assert_true (g_socket_bind (listener, address, TRUE, NULL));
  g_assert_true (g_socket_listen (listener, NULL));
  bound = g_socket_get_local_address (listener, NULL);
  *port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));
  g_thread_unref (g_thread_new ("accept", accept_loop, g_object_ref (listener)));
  return listener;
}

static char *server_url, *closed_url;

static gpointer
cancel_after (gpointer data)
{
  g_usleep (G_USEC_PER_SEC * 3 / 10);
  bro_cancellation_token_cancel (data);
  return NULL;
}

static gboolean
http_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (BroPlatformHttpClient) client = bro_platform_http_client_new (0);
  g_autoptr (GString) url = g_string_new (bro_json_value_get_string (bro_json_value_member (given, "url"), ""));
  g_autoptr (BroHttpRequestSpec) request = NULL;
  g_autoptr (BroHttpResponse) response = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autoptr (BroCancellationToken) cancel = bro_cancellation_token_new ();
  BroJsonValue *headers = bro_json_value_member (given, "headers");
  const char *body = bro_json_value_get_string (bro_json_value_member (given, "body"), NULL);
  BroJsonValue *v;
  GThread *canceller = NULL;
  gint64 t0 = g_get_monotonic_time ();
  g_string_replace (url, "<server>", server_url, 0);
  g_string_replace (url, "<closed>", closed_url, 0);
  request = bro_http_request_spec_new (bro_json_value_get_string (bro_json_value_member (given, "method"), "GET"), url->str);
  for (guint i = 0; headers && i < bro_json_value_length (headers); i++)
    bro_http_request_spec_add_header (request, headers->keys->pdata[i], bro_json_value_get_string (bro_json_value_at (headers, i), ""));
  if (body)
    request->body = g_bytes_new (body, strlen (body));
  if (bro_json_value_member (given, "cancelAfter"))
    canceller = g_thread_new ("cancel", cancel_after, cancel);
  response = bro_http_client_send (BRO_HTTP_CLIENT (client), request, cancel, &error);
  if (canceller)
    g_thread_join (canceller);
  bro_test_same_string (failures, id, "error", bro_json_value_get_string (bro_json_value_member (expect, "error"), NULL), error ? error->code : NULL);
  if (response)
    {
      gsize n = 0;
      const char *data = g_bytes_get_data (response->body, &n);
      g_autofree char *text = g_strndup (data ? data : "", n);
      if (response->status != bro_json_value_get_integer (bro_json_value_member (expect, "status"), -1))
        bro_test_fail (failures, id, "status %d", response->status);
      if ((v = bro_json_value_member (expect, "json")))
        {
          g_autoptr (BroJsonValue) got = bro_json_value_parse (text, -1);
          bro_test_same_json (failures, id, "json", v, got);
        }
      if ((v = bro_json_value_member (expect, "text")))
        bro_test_same_string (failures, id, "text", bro_json_value_get_string (v, ""), text);
    }
  if ((v = bro_json_value_member (expect, "within")) && since (t0) >= bro_json_value_get_number (v, 0))
    bro_test_fail (failures, id, "took %.1f s", since (t0));
  return TRUE;
}
#endif

static void
test_http_client (void)
{
#ifdef BRO_HAVE_LIBSOUP
  guint16 port = 0, closed = 0;
  g_autoptr (GSocket) server = start_server (&port);
  GSocket *gone = start_server (&closed);
  g_socket_close (gone, NULL);
  g_object_unref (gone);
  server_url = g_strdup_printf ("http://127.0.0.1:%u", port);
  closed_url = g_strdup_printf ("http://127.0.0.1:%u", closed);
  bro_test_run_cases ("adapters/http-client.cases.json", http_case);
  g_socket_close (server, NULL);
  g_clear_pointer (&server_url, g_free);
  g_clear_pointer (&closed_url, g_free);
#else
  g_test_skip ("libsoup 3 isn't installed here; the HTTP client is tested on Linux");
#endif
}

/* ---- an HttpClient that records the request and gives a fixed answer ---- */

#define TEST_TYPE_FAKE_HTTP (test_fake_http_get_type ())
G_DECLARE_FINAL_TYPE (TestFakeHttp, test_fake_http, TEST, FAKE_HTTP, GObject)

struct _TestFakeHttp {
  GObject parent_instance;
  char *method, *url;
  GBytes *sent_body;
  int status;
  GBytes *body;
  char *fail_reason;
};

static void test_fake_http_iface_init (BroHttpClientInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestFakeHttp, test_fake_http, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (BRO_TYPE_HTTP_CLIENT, test_fake_http_iface_init))

static BroHttpResponse *
fake_send (BroHttpClient *c, const BroHttpRequestSpec *request, BroCancellationToken *cancel, BroBroError **error)
{
  TestFakeHttp *self = TEST_FAKE_HTTP (c);
  BroHttpResponse *r;
  g_free (self->method);
  g_free (self->url);
  self->method = g_strdup (request->method);
  self->url = g_strdup (request->url);
  g_clear_pointer (&self->sent_body, g_bytes_unref);
  self->sent_body = request->body ? g_bytes_ref (request->body) : NULL;
  if (self->fail_reason)
    {
      BroJsonValue *params = bro_json_value_new_object ();
      bro_json_value_set (params, "reason", bro_json_value_new_string (self->fail_reason));
      bro_bro_error_set (error, "http.failed", params);
      return NULL;
    }
  r = bro_http_response_new ();
  r->status = self->status;
  g_clear_pointer (&r->body, g_bytes_unref);
  r->body = self->body ? g_bytes_ref (self->body) : g_bytes_new (NULL, 0);
  return r;
}

static void
test_fake_http_finalize (GObject *o)
{
  TestFakeHttp *self = TEST_FAKE_HTTP (o);
  g_free (self->method);
  g_free (self->url);
  g_clear_pointer (&self->sent_body, g_bytes_unref);
  g_clear_pointer (&self->body, g_bytes_unref);
  g_free (self->fail_reason);
  G_OBJECT_CLASS (test_fake_http_parent_class)->finalize (o);
}

static void test_fake_http_class_init (TestFakeHttpClass *k) { G_OBJECT_CLASS (k)->finalize = test_fake_http_finalize; }
static void test_fake_http_init (TestFakeHttp *self) { self->status = 200; }
static void test_fake_http_iface_init (BroHttpClientInterface *iface) { iface->send = fake_send; }

static char *
bytes_text (GBytes *b)
{
  gsize n = 0;
  const char *d = b ? g_bytes_get_data (b, &n) : NULL;
  return g_strndup (d ? d : "", n);
}

static void
same_error (GPtrArray *failures, const char *id, BroJsonValue *want, const BroBroError *got)
{
  BroJsonValue *params = bro_json_value_member (want, "params");
  bro_test_same_string (failures, id, "code", bro_json_value_get_string (bro_json_value_member (want, "code"), NULL), got ? got->code : NULL);
  for (guint i = 0; got && params && i < bro_json_value_length (params); i++)
    bro_test_same_json (failures, id, params->keys->pdata[i], bro_json_value_at (params, i), bro_json_value_member (got->params, params->keys->pdata[i]));
}

static gboolean
notification_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestFakeHttp) http = g_object_new (TEST_TYPE_FAKE_HTTP, NULL);
  g_autoptr (TestScriptedLauncher) launcher = g_object_new (TEST_TYPE_SCRIPTED_LAUNCHER, NULL);
  g_autoptr (TestFixedLocator) locator = g_object_new (TEST_TYPE_FIXED_LOCATOR, NULL);
  g_autoptr (BroAppriseTool) apprise = NULL;
  g_autoptr (BroNotificationSender) sender = NULL;
  g_autoptr (BroBroError) error = NULL;
  BroJsonValue *answer = bro_json_value_member (given, "answer");
  BroJsonValue *apprise_given = bro_json_value_member (given, "apprise");
  BroJsonValue *want = bro_json_value_member (expect, "error");
  BroJsonValue *v;
  BroStatusWord status = BRO_STATUS_WORD_SUCCESS;
  gboolean ok;
  if (answer)
    {
      http->status = (int) bro_json_value_get_integer (bro_json_value_member (answer, "status"), 200);
      http->fail_reason = g_strdup (bro_json_value_get_string (bro_json_value_member (answer, "reason"), NULL));
    }
  launcher->exit_code = (int) bro_json_value_get_integer (bro_json_value_member (given, "appriseExit"), 0);
  locator->apprise = apprise_given && apprise_given->kind == BRO_JSON_VALUE_NULL ? NULL : g_strdup ("/opt/apprise");
  apprise = bro_apprise_tool_new (BRO_PROCESS_LAUNCHER (launcher), BRO_TOOL_LOCATOR (locator));
  sender = bro_notification_sender_new (BRO_HTTP_CLIENT (http), apprise);
  bro_status_word_from_wire (bro_json_value_get_string (bro_json_value_member (given, "status"), "success"), &status);
  ok = bro_notification_sender_send (sender, bro_json_value_get_string (bro_json_value_member (given, "url"), ""), "T", "B", status, NULL, &error);
  if (want)
    same_error (failures, id, want, error);
  else if (!ok)
    bro_test_fail (failures, id, "failed: %s", error ? error->code : "?");
  if ((v = bro_json_value_member (expect, "request")))
    {
      g_autofree char *text = bytes_text (http->sent_body);
      BroJsonValue *json = bro_json_value_member (v, "json");
      bro_test_same_string (failures, id, "method", bro_json_value_get_string (bro_json_value_member (v, "method"), ""), http->method);
      bro_test_same_string (failures, id, "url", bro_json_value_get_string (bro_json_value_member (v, "url"), ""), http->url);
      if (json)
        {
          g_autoptr (BroJsonValue) got = bro_json_value_parse (text, -1);
          bro_test_same_json (failures, id, "json", json, got);
        }
      if (bro_json_value_member (v, "text"))
        bro_test_same_string (failures, id, "text", bro_json_value_get_string (bro_json_value_member (v, "text"), ""), text);
    }
  if ((v = bro_json_value_member (expect, "apprise")))
    {
      BroProcessSpec *spec = launcher->started->len ? launcher->started->pdata[0] : NULL;
      g_autofree char *want_argv = joined_strings (v);
      g_autofree char *args = spec ? g_strjoinv ("|", spec->arguments) : NULL;
      g_autofree char *got = spec ? g_strconcat (spec->executable, "|", args, NULL) : NULL;
      bro_test_same_string (failures, id, "apprise", want_argv, got);
      {
        BroJsonValue *env = bro_json_value_member (expect, "appriseEnvironment");
        g_autoptr (GString) want_env = g_string_new (NULL), got_env = g_string_new (NULL);
        g_autoptr (GList) keys = spec ? g_list_sort (g_hash_table_get_keys (spec->environment), (GCompareFunc) strcmp) : NULL;
        for (guint i = 0; i < bro_json_value_length (env); i++)
          g_string_append_printf (want_env, "%s=%s;", (char *) env->keys->pdata[i], bro_json_value_get_string (bro_json_value_at (env, i), ""));
        for (GList *k = keys; k; k = k->next)
          g_string_append_printf (got_env, "%s=%s;", (char *) k->data, (char *) g_hash_table_lookup (spec->environment, k->data));
        bro_test_same_string (failures, id, "appriseEnvironment", want_env->str, got_env->str);
      }
    }
  return TRUE;
}

static void
test_notification_sender (void)
{
  bro_test_run_cases ("adapters/notification-sender.cases.json", notification_case);
}

static gboolean
beta_key_case (const char *id, BroJsonValue *given, BroJsonValue *expect, GPtrArray *failures)
{
  g_autoptr (TestFakeHttp) http = g_object_new (TEST_TYPE_FAKE_HTTP, NULL);
  g_autoptr (BroBetaKeySource) source = NULL;
  g_autoptr (BroBroError) error = NULL;
  g_autofree char *key = NULL;
  BroJsonValue *answer = bro_json_value_member (given, "answer");
  const char *file = bro_json_value_get_string (bro_json_value_member (answer, "file"), NULL);
  http->status = (int) bro_json_value_get_integer (bro_json_value_member (answer, "status"), 200);
  http->fail_reason = g_strdup (bro_json_value_get_string (bro_json_value_member (answer, "reason"), NULL));
  if (file)
    {
      gsize n = 0;
      char *text = bro_test_fixture_text (file, &n);
      http->body = g_bytes_new_take (text, n);
    }
  else
    {
      const char *text = bro_json_value_get_string (bro_json_value_member (answer, "text"), "");
      http->body = g_bytes_new (text, strlen (text));
    }
  source = bro_beta_key_source_new (BRO_HTTP_CLIENT (http));
  key = bro_beta_key_source_current_key (source, NULL, &error);
  if (bro_json_value_member (expect, "error"))
    same_error (failures, id, bro_json_value_member (expect, "error"), error);
  else
    {
      if (!key || !g_str_has_prefix (key, bro_json_value_get_string (bro_json_value_member (expect, "keyPrefix"), "")))
        bro_test_fail (failures, id, "key %s", key ? key : "(none)");
      bro_test_same_string (failures, id, "url", BRO_BETA_KEY_PAGE_URL, http->url);
      bro_test_same_string (failures, id, "method", "GET", http->method);
    }
  return TRUE;
}

static void
test_beta_key_source (void)
{
  bro_test_run_cases ("adapters/beta-key-source.cases.json", beta_key_case);
}
