project(
  'thunar-disk-usage-plugin',
  'c',
  version : '0.1.0',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.62.0',
  default_options : ['c_std=gnu11', 'buildtype=debugoptimized', 'warning_level=2']
)

project_namespace = 'thunar-plugins'
copyright_year = '2026'

cc = meson.get_compiler('c')
i18n = import('i18n')

dependency_versions = {
  'glib': '>= 2.72.0',
  'gtk': '>= 3.24.0',
  'xfce4': '>= 4.18.0',
}

glib = dependency('glib-2.0', version: dependency_versions['glib'])
gio = dependency('gio-2.0', version: dependency_versions['glib'])
gtk = dependency('gtk+-3.0', version: dependency_versions['gtk'])
thunarx = dependency('thunarx-3', version: dependency_versions['xfce4'])
libm = cc.find_library('m', required: false)

# the extensions directory of thunarx, but below our own prefix (e.g. for a
# local test install), not the system's one
extensionsdir = thunarx.get_variable(
  pkgconfig: 'extensionsdir',
  pkgconfig_define: ['libdir', get_option('prefix') / get_option('libdir')],
)

extra_cflags = []
extra_cflags_check = [
  '-Wmissing-declarations',
  '-Wmissing-noreturn',
  '-Wold-style-definition',
  '-Wredundant-decls',
  '-Wpointer-arith',
  '-Wcast-align',
  '-Winit-self',
  '-Wshadow',
  '-Wmissing-include-dirs',
  '-Wundef',
  '-Wformat',
  '-Wformat-security',
  '-Wformat-y2k',
  '-Wnested-externs',
  '-Wno-unused-parameter',
  '-Wno-declaration-after-statement',
  '-Wno-missing-field-initializers',
  '-Werror=implicit-function-declaration',
  '-Wno-error=deprecated-declarations',
]

optimization = get_option('optimization')
if get_option('debug') and optimization in ['0', 'g']
  extra_cflags_check += '-fstack-protector-strong'
  extra_cflags += [
    '-DDEBUG=1',
    '-DDEBUG_TRACE=1',
    '-DG_ENABLE_DEBUG',
  ]
elif optimization in ['3', 's']
  extra_cflags += [
    '-DNDEBUG',
    '-DG_DISABLE_CAST_CHECKS',
    '-DG_DISABLE_ASSERT',
  ]
endif

glib_min_version_parts = dependency_versions['glib'].split(' ')[1].split('.')
glib_min_version_define = 'GLIB_VERSION_@0@_@1@'.format(glib_min_version_parts[0], glib_min_version_parts[1])
extra_cflags += [
  '-DGLIB_VERSION_MIN_REQUIRED=@0@'.format(glib_min_version_define),
  '-DGLIB_VERSION_MAX_ALLOWED=@0@'.format(glib_min_version_define),
  '-DG_LOG_DOMAIN="@0@"'.format(meson.project_name()),
  '-DG_LOG_USE_STRUCTURED=1',
]

extra_cflags += [
  '-DPACKAGE="@0@"'.format(meson.project_name()),
  '-DPACKAGE_NAME="@0@"'.format(meson.project_name()),
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE_STRING="@0@ @1@"'.format(meson.project_name(), meson.project_version()),
  '-DCOPYRIGHT_YEAR="@0@"'.format(copyright_year),
  '-DPACKAGE_LOCALE_DIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DPACKAGE_BUGREPORT="https://gitlab.xfce.org/@0@/@1@/-/issues"'.format(project_namespace, meson.project_name()),
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
]

add_project_arguments(cc.get_supported_arguments(extra_cflags_check), language: 'c')
add_project_arguments(extra_cflags, language: 'c')

subdir('po')
subdir('thunar-plugin')
if get_option('tests')
  subdir('tests')
endif
