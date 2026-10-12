/* bro-disc-request.c */
#include "bro-disc-request.h"

BroSessionSnapshot *
bro_session_snapshot_new (BroListing *listing, BroSessionChoices *choices)
{
  BroSessionSnapshot *s = g_new0 (BroSessionSnapshot, 1);
  s->listing = listing;
  s->choices = choices;
  return s;
}

BroSessionSnapshot *
bro_session_snapshot_copy (const BroSessionSnapshot *snapshot)
{
  return snapshot ? bro_session_snapshot_new (bro_listing_ref (snapshot->listing), bro_session_choices_copy (snapshot->choices)) : NULL;
}

void
bro_session_snapshot_free (BroSessionSnapshot *snapshot)
{
  if (!snapshot)
    return;
  bro_listing_unref (snapshot->listing);
  bro_session_choices_free (snapshot->choices);
  g_free (snapshot);
}

BroDiscRequest *
bro_disc_request_new (BroDiscSource *source, BroSessionSnapshot *session)
{
  BroDiscRequest *r = g_new0 (BroDiscRequest, 1);
  r->source = source;
  r->session = session;
  return r;
}

void
bro_disc_request_free (BroDiscRequest *request)
{
  if (!request)
    return;
  bro_disc_source_free (request->source);
  bro_session_snapshot_free (request->session);
  g_free (request);
}

BroJsonValue *
bro_disc_request_encode (const BroDiscRequest *request)
{
  BroJsonValue *o = bro_json_value_new_object ();
  bro_json_value_set (o, "source", bro_disc_source_encode (request->source));
  if (request->session) {
    BroJsonValue *s = bro_json_value_new_object ();
    bro_json_value_set (s, "listing", bro_listing_encode (request->session->listing));
    bro_json_value_set (s, "choices", bro_session_choices_encode (request->session->choices));
    bro_json_value_set (o, "session", s);
  }
  return o;
}

BroDiscRequest *
bro_disc_request_decode (const BroJobRequest *request)
{
  BroJsonValue *details = request->details;
  BroJsonValue *s = bro_json_value_member (details, "source");
  BroDiscSource *source = s ? bro_disc_source_decode (s) : NULL;
  if (!source && request->drive_id)
    source = bro_disc_source_new (BRO_DISC_SOURCE_DRIVE, request->drive_id);
  if (!source)
    return NULL;
  BroSessionSnapshot *session = NULL;
  BroJsonValue *o = bro_json_value_member (details, "session");
  if (o && o->kind == BRO_JSON_VALUE_OBJECT)
    session = bro_session_snapshot_new (bro_listing_decode (bro_json_value_member (o, "listing")),
                                        bro_session_choices_decode (bro_json_value_member (o, "choices")));
  return bro_disc_request_new (source, session);
}
