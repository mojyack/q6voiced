project('q6voiced', 'cpp', version : '0.0', default_options : ['warning_level=3', 'werror=false', 'cpp_std=c++20'])
add_project_arguments('-Wfatal-errors', language: 'cpp')
add_project_arguments('-Wno-missing-field-initializers', language: 'cpp')

dbus_dep = dependency('dbus-1')
tinyalsa_dep = dependency('tinyalsa')

executable('q6voiced', 
  files(
    'src/main.cpp',
    'src/spawn/process.cpp',
  ), 
  dependencies : [dbus_dep, tinyalsa_dep],
  install : true,
)
