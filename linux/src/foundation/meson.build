# L0 Foundation (rearchitecture plan §3): types every layer uses. GLib's core only.
bro_foundation_lib = static_library('bro-foundation',
  files(
    'bro-bro-error.c',
    'bro-cancellation-source.c',
    'bro-cancellation-token.c',
    'bro-duration.c',
    'bro-http-request-spec.c',
    'bro-id.c',
    'bro-instant.c',
    'bro-json-value.c',
  ),
  dependencies: [glib_dep, libm_dep],
)

bro_foundation_dep = declare_dependency(
  link_with: bro_foundation_lib,
  include_directories: include_directories('.'),
  dependencies: [glib_dep, libm_dep],
)
