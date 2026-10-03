# L1 Domain (rearchitecture plan §3.1): pure functions and value types. GLib's core only (no GIO, no GObject).
bro_domain_inc = include_directories(
  'jobs',
  'messages',
)

bro_domain_lib = static_library('bro-domain',
  files(
    'jobs/bro-job-kind.c',
    'jobs/bro-job-plan.c',
    'jobs/bro-job-request.c',
    'jobs/bro-job-snapshot.c',
    'jobs/bro-job-state.c',
    'jobs/bro-outcome.c',
    'jobs/bro-queue.c',
    'jobs/bro-resource-set.c',
    'jobs/bro-resumability.c',
    'jobs/bro-step-kind.c',
    'jobs/bro-step-output.c',
    'jobs/bro-step-state.c',
    'messages/bro-bro-message.c',
    'messages/bro-message-code.c',
    'messages/bro-severity.c',
  ),
  include_directories: bro_domain_inc,
  dependencies: bro_foundation_dep,
)

bro_domain_dep = declare_dependency(
  link_with: bro_domain_lib,
  include_directories: bro_domain_inc,
  dependencies: bro_foundation_dep,
)
