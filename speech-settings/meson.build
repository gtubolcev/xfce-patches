project(
  'xfce4-speech-settings',
  'c',
  version: '0.1.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.61.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'],
)

i18n = import('i18n')

gettext_package = meson.project_name()
prefix = get_option('prefix')
localedir = prefix / get_option('localedir')

gtk = dependency('gtk+-3.0', version: '>= 3.24.0')
libxfce4ui = dependency('libxfce4ui-2', version: '>= 4.18.0')
libxfce4util = dependency('libxfce4util-1.0', version: '>= 4.18.0')
speechd = dependency('speech-dispatcher', version: '>= 0.11.0')
gtk_x11 = dependency('gtk+-x11-3.0', required: get_option('x11'))

conf = configuration_data()
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', gettext_package)
conf.set_quoted('PACKAGE_LOCALE_DIR', localedir)
conf.set_quoted('SPEECHD_SYSCONFDIR', get_option('speechd-sysconfdir'))
conf.set('HAVE_GTK_X11', gtk_x11.found())
configure_file(output: 'config.h', configuration: conf)

add_project_arguments(
  '-DHAVE_CONFIG_H',
  '-DG_LOG_DOMAIN="@0@"'.format(meson.project_name()),
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'c',
)

subdir('src')
subdir('data')
subdir('po')
subdir('tests')
