project(
  'capnp-janet',
  'c',
  version: files('VERSION'),
  license: 'MIT',
  meson_version: '>=1.2',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

inc = include_directories('src')

lib_sources = files(
  'src/capnp_message.c',
  'src/capnp_builder.c',
)

capnp_janet_lib = library(
  'capnp_janet',
  lib_sources,
  include_directories: inc,
  install: true,
)

capnp_janet_dep = declare_dependency(
  link_with: capnp_janet_lib,
  include_directories: inc,
)

install_headers(
  'src/capnp_kinds.h',
  'src/capnp_pointer.h',
  'src/capnp_message.h',
  'src/capnp_builder.h',
  'src/capnp_janet.h',
  subdir: 'capnp-janet',
)

test_wire = executable(
  'test_wire',
  'test/test_wire.c',
  dependencies: capnp_janet_dep,
)
test('wire', test_wire)

janet_dep = dependency('janet', required: get_option('janet'))
if janet_dep.found()
  shared_module(
    'capnp',
    'src/janet_mod.c',
    dependencies: [capnp_janet_dep, janet_dep],
    name_prefix: '',
    install: true,
    install_dir: get_option('libdir') / 'janet',
  )
endif

pkg = import('pkgconfig')
pkg.generate(
  capnp_janet_lib,
  name: 'capnp-janet',
  description: 'Native Cap\'n Proto runtime for Janet (and C embedders)',
  subdirs: 'capnp-janet',
)
