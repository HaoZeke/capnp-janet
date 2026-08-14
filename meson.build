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
  'src/capnp_rpc.c',
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
  'include/capnp-janet/capnp_rpc.h',
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

test_rpc_join = executable(
  'test_rpc_join',
  'test/test_rpc_join.c',
  dependencies: capnp_janet_dep,
  include_directories: test_inc,
)
test(
  'rpc_join',
  test_rpc_join,
  env: {'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root()},
)

test_rpc_client = executable(
  'test_rpc_client',
  'test/test_rpc_client.c',
  dependencies: capnp_janet_dep,
  include_directories: test_inc,
)
test('rpc_client', test_rpc_client)

# --- live-peer RPC interop: this vat against capnp-C++ ---------------
#
# Byte equality against our own encoders says nothing about whether the
# reference implementation will talk to us, so this bootstraps a real
# EzRpcServer and calls it.
cpp_ok = add_languages('cpp', required: false, native: false)
capnp_rpc_dep = dependency('capnp-rpc', required: false)
capnp_tool = find_program('capnp', required: false)
capnpc_cpp = find_program('capnpc-c++', required: false)
bash = find_program('bash', required: false)

if cpp_ok and capnp_rpc_dep.found() and capnp_tool.found() and capnpc_cpp.found() and bash.found()
  adder_cpp = custom_target(
    'adder-capnp-cpp',
    input: 'schema' / 'adder.capnp',
    output: ['adder.capnp.c++', 'adder.capnp.h'],
    command: [
      capnp_tool, 'compile',
      '--src-prefix=' + meson.current_source_dir() / 'schema',
      '-o' + capnpc_cpp.full_path() + ':@OUTDIR@',
      '@INPUT@',
    ],
  )

  rpc_peer_server = executable(
    'rpc_peer_server',
    ['interop' / 'rpc_peer_server.c++', adder_cpp],
    dependencies: capnp_rpc_dep,
    override_options: ['cpp_std=c++17'],
  )

  rpc_client_c = executable(
    'rpc_client_c',
    'interop' / 'rpc_client.c',
    dependencies: capnp_janet_dep,
    c_args: ['-D_POSIX_C_SOURCE=200809L'],
  )

  test(
    'rpc_interop_cpp',
    bash,
    args: [
      meson.current_source_dir() / 'interop' / 'run_rpc_interop.sh',
      rpc_peer_server.full_path(),
      rpc_client_c.full_path(),
    ],
    depends: [rpc_peer_server, rpc_client_c],
    timeout: 60,
  )
else
  message('interop/rpc: skipped (needs a C++ toolchain, capnp-rpc, and the capnp tools).')
endif

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

install_data(
  'janet/policy.janet',
  install_dir: get_option('datadir') / 'capnp-janet' / 'janet',
)

pkg = import('pkgconfig')
pkg.generate(
  capnp_janet_lib,
  name: 'capnp-janet',
  description: 'Native Cap\'n Proto runtime for Janet (and C embedders)',
  filebase: 'capnp-janet',
)
