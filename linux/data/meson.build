resources = gnome.compile_resources('bromelia-resources', 'bromelia.gresource.xml',
  source_dir: [meson.current_source_dir(), meson.project_source_root() / '..' / 'shared' / 'catalog',
              meson.project_source_root() / '..' / 'shared' / 'web'],
  c_name: 'bromelia',
)

desktop_conf = configuration_data()
desktop_conf.set('APP_ID', app_id)
configure_file(
  input: 'bromelia.desktop.in',
  output: app_id + '.desktop',
  configuration: desktop_conf,
  install: true,
  install_dir: get_option('datadir') / 'applications',
)
configure_file(
  input: 'bromelia.metainfo.xml.in',
  output: app_id + '.metainfo.xml',
  configuration: desktop_conf,
  install: true,
  install_dir: get_option('datadir') / 'metainfo',
)
install_data('icons' / app_id + '.png',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / '256x256' / 'apps',
)
