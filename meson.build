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
  'src/capnp_packed.c',
  'src/capnp_canonical.c',
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
  'include/capnp-janet/capnp_packed.h',
  'include/capnp-janet/capnp_canonical.h',
  'include/capnp-janet/capnp_copy.h',
  'include/capnp-janet/capnp_janet.h',
  subdir: 'capnp-janet',
)

capnpc_janet = executable(
  'capnpc-janet',
  'app/capnpc_janet.c',
  dependencies: capnp_janet_dep,
  install: true,
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

test_packed = executable(
  'test_packed',
  'test/test_packed.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test(
  'packed',
  test_packed,
  workdir: meson.project_source_root(),
  env: {'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root()},
)

test_lists = executable(
  'test_lists',
  'test/test_lists.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test('lists', test_lists)

test_list_evolution = executable(
  'test_list_evolution',
  'test/test_list_evolution.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test('list_evolution', test_list_evolution)

test_multiseg = executable(
  'test_multiseg',
  'test/test_multiseg.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test('multiseg', test_multiseg)


test_canonical = executable(
  'test_canonical',
  'test/test_canonical.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test(
  'canonical',
  test_canonical,
  workdir: meson.project_source_root(),
  env: {'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root()},
)

# capnpc-janet smoke (needs system `capnp` CLI).
bash = find_program('bash', required: true)
test(
  'codegen',
  bash,
  args: [meson.project_source_root() / 'test/test_codegen.sh'],
  depends: [capnpc_janet],
  workdir: meson.project_source_root(),
  env: {
    'CAPNP_JANET_BUILD': meson.project_build_root(),
  },
)

test_copy = executable(
  'test_copy',
  'test/test_copy.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)
test(
  'copy',
  test_copy,
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
