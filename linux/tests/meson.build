test_env = environment()
test_env.set('BROMELIA_FIXTURES', meson.project_source_root() / '..' / 'shared' / 'fixtures')

test_core = executable('test-core', 'test-core.c', dependencies: bromelia_core_dep)
test('core', test_core, env: test_env, timeout: 600)

# The rebuild's layers: one test binary per layer, against shared/fixtures.
bro_test_support = static_library('bro-test-support', 'bro-test-fixtures.c', dependencies: bro_foundation_dep)
bro_test_support_dep = declare_dependency(link_with: bro_test_support, include_directories: include_directories('.'),
  dependencies: bro_foundation_dep)
# English text for the tests that check a message's: the host's MessageTextRenderer (Adapters).
bro_test_english = static_library('bro-test-english', 'bro-test-english.c', dependencies: bro_adapters_dep)
bro_test_english_dep = declare_dependency(link_with: bro_test_english, dependencies: bro_adapters_dep)
# The test kit (plan §17): fakes of the ports for the adapter tests and the test composition root.
bro_test_kit = static_library('bro-test-kit', 'bro-test-kit.c', dependencies: bro_ports_dep)
bro_test_kit_dep = declare_dependency(link_with: bro_test_kit, include_directories: include_directories('.'),
  dependencies: bro_ports_dep)

test('foundation', executable('test-foundation', 'test-foundation.c', dependencies: bro_test_support_dep), env: test_env)
test('domain', executable('test-domain', 'test-domain.c', dependencies: [bro_test_support_dep, bro_test_english_dep, bro_domain_dep]), env: test_env)
test('ports', executable('test-ports', 'test-ports.c', dependencies: [bro_test_support_dep, bro_ports_dep]), env: test_env)
test('adapters', executable('test-adapters', 'test-adapters.c', dependencies: [bro_test_support_dep, bro_test_english_dep, bro_test_kit_dep,
  bro_adapters_dep]), env: test_env, timeout: 180)
test('engine', executable('test-engine', 'test-engine.c', dependencies: [bro_test_support_dep, bro_engine_dep]), env: test_env)
