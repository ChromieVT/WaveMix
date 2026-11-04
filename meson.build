# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ChromieVT

project('wavemix', 'cpp',
  version : files('VERSION'),
  license : 'GPL-3.0-or-later',
  meson_version : '>= 1.1',
  default_options : [
    'cpp_std=c++20',
    'warning_level=3',
    'buildtype=debugoptimized',
  ],
)

# PipeWire's headers are C and trip a few of our stricter C++ warnings, so they're
# included as system headers: their warnings stay out of our build output.
json_dep = dependency('nlohmann_json', version : '>= 3.10')
pipewire_dep = dependency('libpipewire-0.3', version : '>= 1.0', include_type : 'system')
libpulse_dep = dependency('libpulse', include_type : 'system')
gio_dep = dependency('gio-2.0', version : '>= 2.70', include_type : 'system')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', include_type : 'system')
gtkmm_dep = dependency('gtkmm-4.0', version : '>= 4.12', include_type : 'system')

# PipeWire's event tables are C structs that we fill in with designated initializers,
# naming only the callbacks we use. That's the intended way to use them (the others
# must be null), so GCC's warning about the unnamed ones is just noise.
add_project_arguments('-Wno-missing-field-initializers', language : 'cpp')

# build_config.hpp gives the code the version from the VERSION file.
build_config = configuration_data()
build_config.set_quoted('WAVEMIX_VERSION', meson.project_version())
configure_file(output : 'build_config.hpp', configuration : build_config)

# The top-level include path covers both src/ (for "core/ids.hpp" etc.) and the build
# folder (for build_config.hpp).
top_inc = include_directories('.', 'src')

subdir('src')
subdir('data')
subdir('tests')
