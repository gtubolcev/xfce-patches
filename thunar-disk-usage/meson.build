project(
  'thunar-disk-usage-plugin',
  'c',
  version: '0.1.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.62.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ],
)

i18n = import('i18n')

gettext_package = meson.project_name()

glib_dep = dependency('glib-2.0', version: '>= 2.72.0')
gio_dep = dependency('gio-2.0', version: '>= 2.72.0')
gtk_dep = dependency('gtk+-3.0', version: '>= 3.24.0')
thunarx_dep = dependency('thunarx-3', version: '>= 4.18.0')
libm_dep = meson.get_compiler('c').find_library('m', required: false)

# install into our own prefix (e.g. for a local test install), not into
# the system's thunarx directory
extensionsdir = thunarx_dep.get_variable(
  pkgconfig: 'extensionsdir',
  pkgconfig_define: ['libdir', get_option('prefix') / get_option('libdir')],
)

conf = configuration_data()
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
conf.set_quoted('PACKAGE_LOCALE_DIR', get_option('prefix') / get_option('localedir'))
conf.set_quoted('VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: conf)

add_project_arguments(
  '-DHAVE_CONFIG_H',
  '-DG_LOG_DOMAIN="thunar-disk-usage"',
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_72',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_72',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'c',
)

subdir('src')
subdir('po')
if get_option('tests')
  subdir('tests')
endif
