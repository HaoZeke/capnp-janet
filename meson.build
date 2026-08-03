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

inc = include_directories('include')

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

# Pure C Cap'n runtime. pkg-config / subproject consumers use this.
capnp_janet_dep = declare_dependency(
  link_with: capnp_janet_lib,
  include_directories: inc,
)

# Janet CFUN TU for hosts that embed their own Janet amalgamation.
capnp_janet_mod_src = files('src/janet_mod.c')

meson.override_dependency('capnp-janet', capnp_janet_dep)

install_headers(
  'include/capnp-janet/capnp_kinds.h',
  'include/capnp-janet/capnp_pointer.h',
  'include/capnp-janet/capnp_message.h',
  'include/capnp-janet/capnp_builder.h',
  'include/capnp-janet/capnp_janet.h',
  subdir: 'capnp-janet',
)

test_inc = include_directories('test')

test_wire = executable(
  'test_wire',
  'test/test_wire.c',
  dependencies: capnp_janet_dep,
)
test('wire', test_wire)

test_list_text = executable(
  'test_list_text',
  'test/test_list_text.c',
  dependencies: capnp_janet_dep,
)
test('list_text', test_list_text)

# Cap'n C++ / pycapnp sample parity (AddressBook + calculator Expression trees).
test_addressbook = executable(
  'test_addressbook',
  'test/test_addressbook.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test(
  'addressbook',
  test_addressbook,
  workdir: meson.project_source_root(),
  env: {'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root()},
)

test_calculator = executable(
  'test_calculator',
  'test/test_calculator.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test(
  'calculator',
  test_calculator,
  workdir: meson.project_source_root(),
  env: {'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root()},
)

janet_dep = dependency('janet', required: get_option('janet'))
if janet_dep.found()
  shared_module(
    'capnp',
    capnp_janet_mod_src,
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
  filebase: 'capnp-janet',
)
