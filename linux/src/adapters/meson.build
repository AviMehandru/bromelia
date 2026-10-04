# L3 Adapters (rearchitecture plan §10): the ports on Linux (platform/) and the portable tools, HTTP and store
# adapters. GIO and other system libraries are allowed here, never in Domain or Engine.
sqlite_dep = dependency('sqlite3', version: '>= 3.37') # STRICT tables
# The HTTP client is libsoup 3 (plan §30, decision 4): required on Linux, its real target; elsewhere (the macOS build
# that runs the tests) it is built only when libsoup is installed.
soup_dep = dependency('libsoup-3.0', version: '>= 3.0', required: host_machine.system() == 'linux')

bro_adapters_inc = include_directories('platform', 'clock', 'http', 'store', 'tools')

bro_adapters_sources = files(
  'clock/bro-system-clock.c',
  'http/bro-beta-key-source.c',
  'http/bro-notification-sender.c',
  'platform/bro-platform-file-system.c',
  'platform/bro-platform-process-launcher.c',
  'platform/bro-platform-tool-paths.c',
  'store/bro-migrations.c',
  'store/bro-sqlite-store.c',
  'tools/bro-apprise-tool.c',
  'tools/bro-home-dir-isolation.c',
  'tools/bro-home-layout.c',
  'tools/bro-listing-run.c',
  'tools/bro-makemkv-invocation.c',
  'tools/bro-makemkv-run.c',
  'tools/bro-makemkv-tool.c',
  'tools/bro-run-sink.c',
  'tools/bro-system-tool-locator.c',
)

bro_adapters_deps = [bro_ports_dep, gio_dep, gio_unix_dep, sqlite_dep]
bro_adapters_args = []
if soup_dep.found()
  bro_adapters_sources += files('http/bro-platform-http-client.c')
  bro_adapters_deps += soup_dep
  bro_adapters_args += '-DBRO_HAVE_LIBSOUP'
endif

bro_adapters_lib = static_library('bro-adapters',
  bro_adapters_sources,
  include_directories: bro_adapters_inc,
  dependencies: bro_adapters_deps,
)

bro_adapters_dep = declare_dependency(
  link_with: bro_adapters_lib,
  include_directories: bro_adapters_inc,
  dependencies: bro_adapters_deps,
  compile_args: bro_adapters_args,
)
