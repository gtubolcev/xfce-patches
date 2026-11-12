executable(
  'xfce4-speech-settings',
  'main.c',
  'speech-page.c',
  'speech-page.h',
  'speechd-config.c',
  'speechd-config.h',
  include_directories: include_directories('..'),
  dependencies: [gtk, gtk_x11, libxfce4ui, libxfce4util, speechd],
  install: true,
)
