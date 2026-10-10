# L4 Engine (rearchitecture plan §3.2, §11): the executor, the event bus, services, the scheduler, the pipeline and
# its steps. Reaches the outside world only through the ports: no adapters, no GIO, no platform calls.
bro_engine_inc = include_directories('core')

bro_engine_lib = static_library('bro-engine',
  files(
    'core/bro-coalescer.c',
    'core/bro-engine-clock.c',
    'core/bro-engine-event.c',
    'core/bro-engine-event-type.c',
    'core/bro-engine-executor.c',
    'core/bro-engine-timer.c',
    'core/bro-event-bus.c',
    'core/bro-event-stream.c',
    'core/bro-stream-item.c',
    'core/bro-subscription.c',
  ),
  include_directories: bro_engine_inc,
  dependencies: [bro_ports_dep],
)

bro_engine_dep = declare_dependency(
  link_with: bro_engine_lib,
  include_directories: bro_engine_inc,
  dependencies: [bro_ports_dep],
)
