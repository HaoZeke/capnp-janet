# capnp-janet — CMake build for FetchContent / add_subdirectory consumers.
#
# Official Cap'n Proto C++ owns find_package(CapnProto) and CapnProto::capnp.
# This runtime deliberately uses a different project / namespace so the two
# can be installed side by side:
#
#   include(FetchContent)
#   FetchContent_Declare(
#     capnp_janet
#     GIT_REPOSITORY https://github.com/HaoZeke/capnp-janet.git
#     GIT_TAG        v0.2.0
#   )
#   FetchContent_MakeAvailable(capnp_janet)
#   target_link_libraries(myapp PRIVATE capnp_janet::capnp_janet)
#
# An installed copy is found with find_package(capnp_janet) or through
# pkg-config as `capnp-janet`. Meson remains the development build.
#
# The Janet native module itself is not built here: it needs janet's own
# headers and a shared-module layout that meson already handles. This file
# covers the C runtime and capnpc-janet, which is what C embedders consume.

cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" _capnp_janet_version_raw)
string(STRIP "${_capnp_janet_version_raw}" _capnp_janet_version_raw)
# CMake project() rejects a -dev suffix; keep the numeric part.
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" CAPNP_JANET_VERSION "${_capnp_janet_version_raw}")

project(capnp_janet
  VERSION ${CAPNP_JANET_VERSION}
  DESCRIPTION "Native Cap'n Proto runtime for Janet (wire, packed, canonical)"
  HOMEPAGE_URL "https://capnp-janet.rgoswami.me"
  LANGUAGES C
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_capnp_janet_is_toplevel ON)
else()
  set(_capnp_janet_is_toplevel OFF)
endif()

# Prefix CAPNP_JANET_* — Cap'n Proto C++ already uses CAPNP_* (CAPNP_EXECUTABLE, …).
option(CAPNP_JANET_BUILD_PLUGIN "Build the capnpc-janet schema plugin" ${_capnp_janet_is_toplevel})
option(CAPNP_JANET_INSTALL "Install targets and package config" ${_capnp_janet_is_toplevel})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Same source set as meson.build lib_sources.
add_library(capnp_janet
  src/capnp_message.c
  src/capnp_builder.c
  src/capnp_packed.c
  src/capnp_canonical.c
  src/capnp_rpc.c
)
add_library(capnp_janet::capnp_janet ALIAS capnp_janet)

set_target_properties(capnp_janet PROPERTIES
  OUTPUT_NAME capnp_janet
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  EXPORT_NAME capnp_janet
)

target_include_directories(capnp_janet
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(CAPNP_JANET_BUILD_PLUGIN)
  add_executable(capnpc-janet app/capnpc_janet.c)
  target_link_libraries(capnpc-janet PRIVATE capnp_janet::capnp_janet)
endif()

if(CAPNP_JANET_INSTALL)
  install(TARGETS capnp_janet
    EXPORT capnp_janetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  if(CAPNP_JANET_BUILD_PLUGIN)
    install(TARGETS capnpc-janet
      EXPORT capnp_janetTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()

  # Same header set meson installs, under the same capnp-janet/ subdir.
  install(FILES
    include/capnp-janet/capnp_kinds.h
    include/capnp-janet/capnp_pointer.h
    include/capnp-janet/capnp_message.h
    include/capnp-janet/capnp_builder.h
    include/capnp-janet/capnp_packed.h
    include/capnp-janet/capnp_canonical.h
    include/capnp-janet/capnp_copy.h
    include/capnp-janet/capnp_janet.h
    include/capnp-janet/capnp_rpc.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/capnp-janet
  )
  install(FILES janet/policy.janet
    DESTINATION ${CMAKE_INSTALL_DATADIR}/capnp-janet/janet
  )

  install(EXPORT capnp_janetTargets
    FILE capnp_janetTargets.cmake
    NAMESPACE capnp_janet::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/capnp_janet
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/capnp_janetConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/capnp_janetConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/capnp_janet
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/capnp_janetConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/capnp_janetConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/capnp_janetConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/capnp_janet
  )

  # pkg-config, matching the file meson's pkg.generate writes.
  set(prefix "${CMAKE_INSTALL_PREFIX}")
  set(exec_prefix "\${prefix}")
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(libdir "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(libdir "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}")
    set(bindir "${CMAKE_INSTALL_BINDIR}")
  else()
    set(bindir "\${prefix}/${CMAKE_INSTALL_BINDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(includedir "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/capnp-janet.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/capnp-janet.pc"
    @ONLY
  )
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/capnp-janet.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  )
endif()
