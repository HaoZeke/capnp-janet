# Smoke: build a tiny program against capnp_janet via add_subdirectory,
# the same path FetchContent_MakeAvailable takes.
cmake_minimum_required(VERSION 3.16)
project(capnp_janet_consumer_smoke LANGUAGES C)

set(CAPNP_JANET_BUILD_PLUGIN OFF CACHE BOOL "" FORCE)
set(CAPNP_JANET_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${CAPNP_JANET_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/capnp_janet-build")

add_executable(smoke smoke.c)
target_link_libraries(smoke PRIVATE capnp_janet::capnp_janet)
