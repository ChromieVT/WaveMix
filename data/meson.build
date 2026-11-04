# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ChromieVT

app_id = 'io.github.ChromieVT.WaveMix'
bindir = get_option('prefix') / get_option('bindir')

# Desktop files and the D-Bus service name the binary by its full installed path, so
# they work even when that folder isn't on PATH (a ~/.local install, say).
paths = configuration_data()
paths.set('BINDIR', bindir)
paths.set('VERSION', meson.project_version())

# The menu entry.
configure_file(
  input : app_id + '.desktop.in',
  output : app_id + '.desktop',
  configuration : paths,
  install_dir : get_option('datadir') / 'applications',
)

# What software centres show about WaveMix.
configure_file(
  input : app_id + '.metainfo.xml.in',
  output : app_id + '.metainfo.xml',
  configuration : paths,
  install_dir : get_option('datadir') / 'metainfo',
)

# Lets the window and the CLI start the background service on demand.
configure_file(
  input : app_id + '.Daemon.service.in',
  output : app_id + '.Daemon.service',
  configuration : paths,
  install_dir : get_option('datadir') / 'dbus-1' / 'services',
)

# Starts the background service at login, so apps are routed from the start. Users can
# switch it off in their desktop's startup-applications settings. Off in the Flatpak,
# which asks the Background portal instead.
if get_option('autostart')
  configure_file(
    input : app_id + '.Daemon.autostart.desktop.in',
    output : app_id + '.Daemon.desktop',
    configuration : paths,
    install_dir : get_option('sysconfdir') / 'xdg' / 'autostart',
  )
endif

# The app icon in every size (menu, window, tray).
install_subdir('icons/hicolor', install_dir : get_option('datadir') / 'icons')

# Running from the build folder, the program finds its icons at <build>/data/icons
# (core::icon_theme_dir), so link them there. Nothing about this ends up installed.
run_command('ln', '-sfn', meson.current_source_dir() / 'icons', meson.current_build_dir() / 'icons',
            check : true)

# Packaging checks, part of `meson test` when the tools are installed.
desktop_file_validate = find_program('desktop-file-validate', required : false)
if desktop_file_validate.found()
  test('desktop-file', desktop_file_validate,
    args : [meson.current_build_dir() / app_id + '.desktop'])
  if get_option('autostart')
    test('autostart-file', desktop_file_validate,
      args : [meson.current_build_dir() / app_id + '.Daemon.desktop'])
  endif
endif
appstreamcli = find_program('appstreamcli', required : false)
if appstreamcli.found()
  test('metainfo', appstreamcli,
    args : ['validate', '--no-net', '--explain', meson.current_build_dir() / app_id + '.metainfo.xml'])
endif
