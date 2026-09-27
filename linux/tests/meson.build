test_env = environment()
test_env.set('BROMELIA_FIXTURES', meson.project_source_root() / '..' / 'shared' / 'fixtures')

test_core = executable('test-core', 'test-core.c', dependencies: bromelia_core_dep)
test('core', test_core, env: test_env, timeout: 600)
