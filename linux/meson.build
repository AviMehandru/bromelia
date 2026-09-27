project('bromelia', 'c',
  version: '1.0.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.62.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

app_id = 'app.bromelia.Bromelia'

gnome = import('gnome')

glib_dep = dependency('glib-2.0', version: '>= 2.76')
gio_dep = dependency('gio-2.0', version: '>= 2.76')
gio_unix_dep = dependency('gio-unix-2.0', required: false)
json_dep = dependency('json-glib-1.0', version: '>= 1.6')
build_ui = get_option('ui')
gtk_dep = dependency('gtk4', version: '>= 4.12', required: build_ui)
adw_dep = dependency('libadwaita-1', version: '>= 1.5', required: build_ui)

cc = meson.get_compiler('c')
add_project_arguments(
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_76',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_76',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_12',
  '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_12',
  '-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5',
  '-DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5',
  '-DG_LOG_DOMAIN="Bromelia"',
  '-DAPP_ID="@0@"'.format(app_id),
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  cc.get_supported_arguments(['-Wno-unused-parameter', '-Wno-missing-field-initializers', '-Wno-sign-compare']),
  language: 'c',
)

subdir('data')
subdir('src')
subdir('tests')
