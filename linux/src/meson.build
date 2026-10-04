# The rebuild's layers (rearchitecture plan §4), one static library each.
subdir('foundation')
subdir('domain')
subdir('ports')

# Today's app.
core_sources = files(
  'bro-robot.c',
  'bro-config.c',
  'bro-logic.c',
  'bro-makemkv.c',
  'bro-runner.c',
  'bro-state.c',
  'bro-identity.c',
  'bro-dvd.c',
  'bro-integrations.c',
  'bro-web.c',
  'bro-sleep.c',
  'bro-archive.c',
)

core_deps = [glib_dep, gio_dep, gio_unix_dep, json_dep]

bromelia_core = static_library('bromelia-core', core_sources, resources,
  dependencies: core_deps,
)

bromelia_core_dep = declare_dependency(
  link_with: bromelia_core,
  sources: resources,
  include_directories: include_directories('.'),
  dependencies: core_deps,
)

executable('bromelia-daemon', 'bromelia-daemon.c',
  dependencies: [bromelia_core_dep],
  install: true,
)

if build_ui
ui_sources = files(
  'main.c',
  'bro-ui-util.c',
  'bro-job-card.c',
  'bro-window.c',
  'bro-drive-page.c',
  'bro-pages.c',
  'bro-config-dialog.c',
  'bro-preferences.c',
)

executable('bromelia', ui_sources,
  dependencies: [bromelia_core_dep, gtk_dep, adw_dep],
  install: true,
)
endif
