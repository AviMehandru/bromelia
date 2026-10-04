# L3 Adapters (rearchitecture plan §10): the ports on Linux (platform/) and the portable tools, HTTP and store
# adapters. GIO and other system libraries are allowed here, never in Domain or Engine.
bro_adapters_inc = include_directories('platform', 'clock')

bro_adapters_lib = static_library('bro-adapters',
  files(
    'clock/bro-system-clock.c',
    'platform/bro-platform-file-system.c',
    'platform/bro-platform-process-launcher.c',
  ),
  include_directories: bro_adapters_inc,
  dependencies: [bro_ports_dep, gio_dep, gio_unix_dep],
)

bro_adapters_dep = declare_dependency(
  link_with: bro_adapters_lib,
  include_directories: bro_adapters_inc,
  dependencies: [bro_ports_dep, gio_dep, gio_unix_dep],
)
