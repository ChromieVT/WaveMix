# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ChromieVT

gtest_dep = dependency('gtest_main', required : get_option('tests'))
if not gtest_dep.found()
  subdir_done()
endif

core_tests = executable('core-tests',
  'test_app_icons.cpp',
  'test_apps.cpp',
  'test_config.cpp',
  'test_ids_and_names.cpp',
  'test_log.cpp',
  'test_mic.cpp',
  'test_profiles.cpp',
  'test_login_conf.cpp',
  dependencies : [core_dep, gtest_dep],
)

# The tests read the reference files in tests/golden/.
test('core', core_tests,
  env : {'WAVEMIX_GOLDEN_DIR' : meson.current_source_dir() / 'golden'},
  protocol : 'gtest',
)
