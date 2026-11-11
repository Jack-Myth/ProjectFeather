project('opusfile', 'c', version : '0.12')

deps = [dependency('opus'), dependency('ogg'), meson.get_compiler('c').find_library('m', required : false)]
headers = include_directories('include')
lib = static_library('opusfile', 'src/info.c', 'src/internal.c', 'src/opusfile.c', 'src/stream.c',
  include_directories : headers, dependencies : deps, pic : true)
opusfile_dep = declare_dependency(link_with : lib, include_directories : headers, dependencies : deps)
