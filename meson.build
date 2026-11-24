project(
  'capnp-janet',
  'c',
  version: files('VERSION.txt'),
  license: 'MIT',
  meson_version: '>=1.2',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

inc = include_directories('include')

cc = meson.get_compiler('c')

# glibc 2.33 added mallinfo2. The pixi sysroot is older, and mallinfo
# still reports the bytes the reparse check compares.
robust_c_args = []
if cc.compiles(
  '#include <malloc.h>\nint main(void){struct mallinfo2 i=mallinfo2();return (int)i.uordblks;}',
  name: 'mallinfo2',
)
  robust_c_args += ['-DCAPNP_HAS_MALLINFO2']
elif cc.compiles(
  '#include <malloc.h>\nint main(void){struct mallinfo i=mallinfo();return (int)i.uordblks;}',
  name: 'mallinfo',
)
  robust_c_args += ['-DCAPNP_HAS_MALLINFO']
else
  message('robust heap check: no mallinfo2 or mallinfo; that check is skipped')
endif

# dlsym lives in libdl on glibc before 2.34 and in libc after that.
libdl = cc.find_library('dl', required: false)
threads = dependency('threads')

lib_sources = files(
  'src/capnp_message.c',
  'src/capnp_builder.c',
  'src/capnp_packed.c',
  'src/capnp_canonical.c',
  'src/capnp_rpc.c',
  'src/capnp_transport.c',
  'src/capnp_dyn.c',
)

# MSVC writes an import library only when something is dllexport, so a
# shared build without this produces no capnp_janet.lib and a consumer
# cannot link. CAPNP_JANET_BUILDING marks the library objects;
# CAPNP_JANET_STATIC keeps the headers from saying dllimport when the
# archive is static.
capnp_janet_consumer_args = []
if get_option('default_library') == 'static'
  capnp_janet_consumer_args += '-DCAPNP_JANET_STATIC'
endif

capnp_janet_lib = library(
  'capnp_janet',
  lib_sources,
  include_directories: inc,
  dependencies: threads,
  c_args: capnp_janet_consumer_args + ['-DCAPNP_JANET_BUILDING'],
  install: true,
)

# Pure C Cap'n runtime. pkg-config / subproject consumers use this.
capnp_janet_dep = declare_dependency(
  link_with: capnp_janet_lib,
  include_directories: inc,
  compile_args: capnp_janet_consumer_args,
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
  'include/capnp-janet/capnp_transport.h',
  'include/capnp-janet/capnp_dyn.h',
  subdir: 'capnp-janet',
)

capnpc_janet = executable(
  'capnpc-janet',
  'app/capnpc_janet.c',
  dependencies: capnp_janet_dep,
  install: true,
)

dump_request = executable(
  'dump-request',
  'app/dump_request.c',
)

# capnp-eval links the library objects so it can reach the hidden entry
# points in src/capnp_internal.h, which the shared library does not export.
capnp_eval = executable(
  'capnp-eval',
  'app/capnp_eval.c',
  'app/kj_stream.c',
  objects: capnp_janet_lib.extract_all_objects(recursive: true),
  include_directories: [inc, include_directories('src')],
  c_args: capnp_janet_consumer_args,
  dependencies: threads,
  install: true,
)

# Tests that load capnp.so depend on it, so `meson test` builds the
# module and they never skip for want of it.
janet_dep = dependency('janet', required: get_option('janet'))
janet_module = []
if janet_dep.found()
  janet_module = [shared_module(
    'capnp',
    capnp_janet_mod_src,
    dependencies: [capnp_janet_dep, janet_dep],
    name_prefix: '',
    install: true,
    install_dir: get_option('libdir') / 'janet',
  )]
endif

test_inc = include_directories('test')

test_wire = executable(
  'test_wire',
  'test/test_wire.c',
  dependencies: capnp_janet_dep,
)
test('wire', test_wire)

test_robust = executable(
  'test_robust',
  'test/test_robust.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
  c_args: robust_c_args,
)
test('robust', test_robust)

test_orphan = executable(
  'test_orphan',
  'test/test_orphan.c',
  dependencies: capnp_janet_dep,
)
test('orphan', test_orphan)

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

# capnpc-janet smoke (needs bash and the system `capnp` CLI; the script
# skips itself without capnp). Optional: a musl host has no bash, and a
# consumer pulling this in as a subproject should not be told to install
# one to build the library.
bash = find_program('bash', required: false)
test_conform = executable(
  'test_conform',
  'test/test_conform.c',
  include_directories: test_inc,
  dependencies: capnp_janet_dep,
)

dyn_deps = [capnp_janet_dep]
if libdl.found()
  dyn_deps += [libdl]
endif

test_dyn = executable(
  'test_dyn',
  'test/test_dyn.c',
  include_directories: test_inc,
  dependencies: dyn_deps,
)

if bash.found()
  test(
    'codegen',
    bash,
    args: [meson.project_source_root() / 'test/test_codegen.sh'],
    depends: [capnpc_janet] + janet_module,
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_NEED_MODULE': janet_dep.found() ? '1' : '',
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'differential',
    bash,
    args: [meson.project_source_root() / 'test/test_differential.sh'],
    depends: [test_conform],
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'dyn',
    bash,
    args: [meson.project_source_root() / 'test/test_dyn.sh'],
    depends: [test_dyn],
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'eval',
    bash,
    args: [meson.project_source_root() / 'test/test_eval.sh'],
    depends: [capnp_eval],
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'convert_parity',
    bash,
    args: [meson.project_source_root() / 'test/test_convert_parity.sh'],
    depends: [capnp_eval],
    workdir: meson.project_source_root(),
    # About 25 s, alone or in a parallel sanitizer run, against the
    # 30 s default.
    timeout: 90,
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'random_parity',
    bash,
    args: [meson.project_source_root() / 'test/test_random_parity.sh'],
    depends: [capnp_eval],
    workdir: meson.project_source_root(),
    # About 5 s, and 11 s under sanitizers.
    timeout: 60,
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'bump_hooks',
    bash,
    args: [meson.project_source_root() / 'test/test_bump_hooks.sh'],
  )
else
  message('codegen smoke: skipped (no bash).')
endif

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

test_rpc_handoff = executable(
  'test_rpc_handoff',
  'test/test_rpc_handoff.c',
  dependencies: capnp_janet_dep,
  include_directories: test_inc,
)
test('rpc_handoff', test_rpc_handoff)

test_rpc_client = executable(
  'test_rpc_client',
  'test/test_rpc_client.c',
  dependencies: capnp_janet_dep,
  include_directories: test_inc,
)
test('rpc_client', test_rpc_client)

test_transport = executable(
  'test_transport',
  'test/test_transport.c',
  dependencies: [capnp_janet_dep, threads],
  c_args: ['-D_POSIX_C_SOURCE=200809L'] + robust_c_args,
)
test('transport', test_transport)

test_abi_layout = executable(
  'test_abi_layout',
  'test/test_abi_layout.c',
  dependencies: capnp_janet_dep,
)
test('abi_layout', test_abi_layout)

# --- live-peer RPC interop: this vat against capnp-C++ ---------------
#
# Byte equality against our own encoders says nothing about whether the
# reference implementation will talk to us, so this bootstraps a real
# EzRpcServer and calls it.
cpp_ok = add_languages('cpp', required: false, native: false)
if not cpp_ok
  warning('no C++ compiler found: every interop test against capnp-C++ is skipped; set CXX (CC=gcc CXX=g++) to run them.')
endif
capnp_rpc_dep = dependency('capnp-rpc', required: false)
capnp_tool = find_program('capnp', required: false)
capnpc_cpp = find_program('capnpc-c++', required: false)

if cpp_ok and capnp_rpc_dep.found() and capnp_tool.found() and capnpc_cpp.found() and bash.found()
  # `capnp compile` searches only /usr/include and /usr/local/include for
  # `/capnp/c++.capnp`, so a prefixed install must be named.
  capnp_include = capnp_rpc_dep.get_variable(
    pkgconfig: 'includedir',
    default_value: '/usr/include',
  )

  adder_cpp = custom_target(
    'adder-capnp-cpp',
    input: 'schema' / 'adder.capnp',
    output: ['adder.capnp.c++', 'adder.capnp.h'],
    command: [
      capnp_tool, 'compile',
      '--src-prefix=' + meson.current_source_dir() / 'schema',
      '-I' + capnp_include,
      '-o' + capnpc_cpp.full_path() + ':@OUTDIR@',
      '@INPUT@',
    ],
  )

  wide_cpp = custom_target(
    'wide-capnp-cpp',
    input: 'schema' / 'wide.capnp',
    output: ['wide.capnp.c++', 'wide.capnp.h'],
    command: [
      capnp_tool, 'compile',
      '--src-prefix=' + meson.current_source_dir() / 'schema',
      '-o' + capnpc_cpp.full_path() + ':@OUTDIR@',
      '@INPUT@',
    ],
  )

  wide_server = executable(
    'wide_server',
    'interop' / 'wide_server.c',
    dependencies: capnp_janet_dep,
    c_args: ['-D_POSIX_C_SOURCE=200809L'],
  )

  wide_client = executable(
    'wide_client',
    ['interop' / 'wide_client.c++', wide_cpp],
    dependencies: capnp_rpc_dep,
    override_options: ['cpp_std=c++17'],
  )

  test(
    'wide_result',
    bash,
    args: [meson.project_source_root() / 'test/test_wide.sh'],
    depends: [wide_server, wide_client],
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
    timeout: 60,
  )

  rpc_peer_client = executable(
    'rpc_peer_client',
    ['interop' / 'rpc_peer_client.c++', adder_cpp],
    dependencies: capnp_rpc_dep,
    override_options: ['cpp_std=c++17'],
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
  test(
    'rpc_janet',
    bash,
    args: [meson.project_source_root() / 'test/test_rpc_janet.sh'],
    depends: [rpc_peer_server] + janet_module,
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_NEED_MODULE': janet_dep.found() ? '1' : '',
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
  )
  test(
    'transport_peer',
    bash,
    args: [meson.project_source_root() / 'test/test_transport_peer.sh'],
    depends: [test_transport, rpc_peer_server, rpc_peer_client],
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
    timeout: 60,
  )
  test(
    'rpc_janet_host',
    bash,
    args: [meson.project_source_root() / 'test/test_rpc_janet_host.sh'],
    depends: [rpc_peer_server, rpc_peer_client] + janet_module,
    workdir: meson.project_source_root(),
    env: {
      'CAPNP_JANET_NEED_MODULE': janet_dep.found() ? '1' : '',
      'CAPNP_JANET_BUILD': meson.project_build_root(),
    },
    timeout: 60,
  )
else
  warning('interop/rpc: skipped (needs a C++ toolchain, capnp-rpc, and the capnp tools).')
endif

if bash.found()
  valgrind_bins = [
    test_wire,
    test_robust,
    test_orphan,
    test_list_text,
    test_addressbook,
    test_calculator,
    test_packed,
    test_lists,
    test_list_evolution,
    test_multiseg,
    test_canonical,
    test_copy,
    test_rpc_join,
    test_rpc_handoff,
    test_rpc_client,
    test_transport,
    test_abi_layout,
  ]
  test(
    'valgrind_c',
    bash,
    args: [meson.project_source_root() / 'test/test_valgrind.sh'],
    depends: valgrind_bins,
    workdir: meson.project_source_root(),
    timeout: 600,
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
      'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root(),
    },
  )
  test(
    'valgrind_guard',
    bash,
    args: [meson.project_source_root() / 'test/test_valgrind_guard.sh'],
    depends: valgrind_bins,
    workdir: meson.project_source_root(),
    timeout: 600,
    env: {
      'CAPNP_JANET_BUILD': meson.project_build_root(),
      'CAPNP_JANET_SOURCE_ROOT': meson.project_source_root(),
    },
  )
  test(
    'abi_diff',
    bash,
    args: [meson.project_source_root() / 'scripts/abi-diff.sh'],
    workdir: meson.project_source_root(),
    timeout: 600,
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
