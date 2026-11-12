test_config = executable(
  'test-config',
  'test-config.c',
  '../src/speechd-config.c',
  include_directories: include_directories('..'),
  dependencies: [dependency('gio-2.0')],
)
test('config', test_config)
