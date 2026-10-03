test_env = environment()
test_env.set('BROMELIA_FIXTURES', meson.project_source_root() / '..' / 'shared' / 'fixtures')

test_core = executable('test-core', 'test-core.c', dependencies: bromelia_core_dep)
test('core', test_core, env: test_env, timeout: 600)

# The rebuild's layers: one test binary per layer, against shared/fixtures.
bro_test_support = static_library('bro-test-support', 'bro-test-fixtures.c', dependencies: bro_foundation_dep)
bro_test_support_dep = declare_dependency(link_with: bro_test_support, include_directories: include_directories('.'),
  dependencies: bro_foundation_dep)

test('foundation', executable('test-foundation', 'test-foundation.c', dependencies: bro_test_support_dep), env: test_env)
