executable(
  'du-scan-test',
  ['du-scan-test.c', '../thunar-plugin/du-scanner.c'],
  include_directories: include_directories('..'),
  dependencies: [glib, gio],
  install: false,
)

executable(
  'du-view-test',
  [
    'du-view-test.c',
    '../thunar-plugin/du-chart.c',
    '../thunar-plugin/du-rings-chart.c',
    '../thunar-plugin/du-scanner.c',
    '../thunar-plugin/du-view.c',
  ],
  include_directories: include_directories('..'),
  dependencies: [glib, gio, gtk, libm],
  install: false,
)
