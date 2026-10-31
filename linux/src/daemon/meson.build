# L5 Hosts (rearchitecture plan §4, §17; decision 167): the composition root, the one place that builds the real graph.
# A static library in phase 3; phase 4 adds bromeliad's main, HostOptions and EngineLock. The app version is meson's
# project_version().
bro_host_lib = static_library('bro-host',
  files('bro-composition-root.c'),
  c_args: ['-DBRO_HOST_APP_VERSION="@0@"'.format(meson.project_version())],
  dependencies: [bro_engine_dep, bro_adapters_dep],
)

bro_host_dep = declare_dependency(
  link_with: bro_host_lib,
  include_directories: include_directories('.'),
  dependencies: [bro_engine_dep, bro_adapters_dep],
  compile_args: ['-DBRO_HOST_APP_VERSION="@0@"'.format(meson.project_version())],
)
