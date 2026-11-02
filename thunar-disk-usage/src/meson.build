shared_module(
  'thunar-disk-usage-plugin',
  [
    'du-chart.c',
    'du-rings-chart.c',
    'du-scanner.c',
    'du-view.c',
    'tdu-page.c',
    'tdu-plugin.c',
    'tdu-provider.c',
    'tdu-window.c',
  ],
  name_prefix: '',
  include_directories: include_directories('..'),
  dependencies: [glib_dep, gio_dep, gtk_dep, thunarx_dep, libm_dep],
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: extensionsdir,
)
