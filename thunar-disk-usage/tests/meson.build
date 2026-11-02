executable(
  'du-scan-test',
  ['du-scan-test.c', '../src/du-scanner.c'],
  include_directories: include_directories('..'),
  dependencies: [glib_dep, gio_dep],
  install: false,
)

executable(
  'du-view-test',
  [
    'du-view-test.c',
    '../src/du-chart.c',
    '../src/du-rings-chart.c',
    '../src/du-scanner.c',
    '../src/du-view.c',
  ],
  include_directories: include_directories('..'),
  dependencies: [glib_dep, gio_dep, gtk_dep, libm_dep],
  install: false,
)
