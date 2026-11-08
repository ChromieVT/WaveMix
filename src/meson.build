# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ChromieVT

# core: plain C++ with no PipeWire or GTK. Everything that can be unit-tested lives here.
core_lib = static_library('wavemix-core',
  'core/app_icons.cpp',
  'core/apps.cpp',
  'core/config.cpp',
  'core/ids.cpp',
  'core/json_file.cpp',
  'core/log.cpp',
  'core/links.cpp',
  'core/login_conf.cpp',
  'core/mic.cpp',
  'core/names.cpp',
  'core/notifications.cpp',
  'core/paths.cpp',
  'core/profiles.cpp',
  include_directories : top_inc,
  dependencies : json_dep,
)
core_dep = declare_dependency(
  link_with : core_lib,
  include_directories : top_inc,
  dependencies : json_dep,
)

# pw: the PipeWire layer.
pw_lib = static_library('wavemix-pw',
  'pw/connection.cpp',
  'pw/graph.cpp',
  'pw/registry.cpp',
  'pw/server_info.cpp',
  dependencies : [core_dep, pipewire_dep],
)
pw_dep = declare_dependency(
  link_with : pw_lib,
  dependencies : [core_dep, pipewire_dep],
)

# plumbing: creating and removing the channels' sinks and loopbacks, and the requests
# that go through pipewire-pulse. libpulse is used here, and only here (see
# plumbing/pulse_control.hpp for why).
plumbing_lib = static_library('wavemix-plumbing',
  'plumbing/pulse_control.cpp',
  'plumbing/pulse_plumbing.cpp',
  'plumbing/meters.cpp',
  'plumbing/service_links.cpp',
  dependencies : [pw_dep, libpulse_dep],
)
plumbing_dep = declare_dependency(
  link_with : plumbing_lib,
  dependencies : [pw_dep, libpulse_dep],
)

# mixer: the daemon's logic, independent of D-Bus.
mixer_lib = static_library('wavemix-mixer',
  'mixer/mixer.cpp',
  dependencies : [plumbing_dep],
)
mixer_dep = declare_dependency(
  link_with : mixer_lib,
  dependencies : [plumbing_dep],
)

# The D-Bus interface the service registers, from the one XML file that describes it
# (the tests read it too). Meson re-reads it whenever it changes.
fs = import('fs')
interface_xml = configuration_data()
interface_xml.set('INTROSPECTION_XML', fs.read('../data/io.github.ChromieVT.WaveMix.Daemon1.xml'))
dbus_interface = configure_file(
  input : 'daemon/dbus_interface.hpp.in',
  output : 'dbus_interface.hpp',
  configuration : interface_xml,
)

# daemon: the D-Bus service and the tray icon, on the GLib main loop.
daemon_lib = static_library('wavemix-daemon',
  'daemon/autostart.cpp',
  'daemon/notifier.cpp',
  'daemon/service.cpp',
  'daemon/tray.cpp',
  dbus_interface,
  dependencies : [mixer_dep, gio_dep, gdk_pixbuf_dep],
)
daemon_dep = declare_dependency(
  link_with : daemon_lib,
  dependencies : [mixer_dep, gio_dep, gdk_pixbuf_dep],
)

# client: talking to the daemon over D-Bus; shared by the window and the CLI.
client_lib = static_library('wavemix-client',
  'client/client.cpp',
  dependencies : [core_dep, gio_dep],
)
client_dep = declare_dependency(
  link_with : client_lib,
  dependencies : [core_dep, gio_dep],
)

# report: the bug report, made by the CLI and by the window.
report_lib = static_library('wavemix-report',
  'report/bug_report.cpp',
  dependencies : [pw_dep, gio_dep],
)
report_dep = declare_dependency(
  link_with : report_lib,
  dependencies : [pw_dep, gio_dep],
)

# gui: the GTK 4 mixer window.
gui_lib = static_library('wavemix-gui',
  'gui/application.cpp',
  'gui/helpers.cpp',
  'gui/mic_column.cpp',
  'gui/notifications.cpp',
  'gui/theme.cpp',
  'gui/widgets.cpp',
  'gui/window.cpp',
  dependencies : [client_dep, report_dep, gtkmm_dep],
)
gui_dep = declare_dependency(
  link_with : gui_lib,
  dependencies : [client_dep, report_dep, gtkmm_dep],
)

executable('wavemix',
  'main.cpp',
  'cli.cpp',
  'dump_graph.cpp',
  dependencies : [core_dep, pw_dep, daemon_dep, client_dep, gui_dep],
  install : true,
)
