/* bro-profile-xml.h: BroProfileXml, the MakeMKV conversion profile (.mmcp.xml) Bromelia generates. */
#pragma once

#include "bro-json-value.h"

G_BEGIN_DECLS

/* MakeMKV's own default selection rule, used when the profile has none. */
#define BRO_PROFILE_XML_MAKEMKV_DEFAULT_SELECTION                                                                      \
  "-sel:all,+sel:(favlang|nolang|single),-sel:(havemulti|havecore),-sel:mvcvideo,=100:all,-10:favlang"

/* The profile for @generated (config-3.json's makemkv.profile.generated; missing fields take their defaults). */
char *bro_profile_xml_render (BroJsonValue *generated);

G_END_DECLS
