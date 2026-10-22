# grain on its own: the language, its standard library and the `grain` program,
# for macOS, Linux and Windows, without the rest of aios.
#
#   cmake -S grain -B build-grain -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-grain --parallel
#   ctest --test-dir build-grain --output-on-failure
#
# The sources are the ones aios builds (src/userland/grain_*.cpp and the host
# layer under src/hal/): one language, compiled two ways, never two copies.
cmake_minimum_required(VERSION 3.20)
project(grain VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The C++20 this needs: GCC 10, Clang 12 (Apple's 13), MSVC 2019 16.10. Said up
# front rather than as a page of template errors from an older compiler.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(FATAL_ERROR "grain needs GCC 10 or newer (found ${CMAKE_CXX_COMPILER_VERSION}); "
                      "on an older distribution use the manylinux2014 image - see grain/README.md")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  message(FATAL_ERROR "grain needs Clang 12 or newer (found ${CMAKE_CXX_COMPILER_VERSION})")
elseif(MSVC AND MSVC_VERSION LESS 1929)
  message(FATAL_ERROR "grain needs Visual Studio 2019 16.10 or newer (found MSVC ${MSVC_VERSION})")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
# Windows: the C runtime linked in, as aios links it, so grain.exe runs on a
# machine that has no Visual C++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

get_filename_component(AIOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# The language: front end, VM, native code, standard library, fonts for the canvas.
set(GRAIN_LANG_SOURCES
  ${AIOS_ROOT}/src/userland/grain_lexer.cpp
  ${AIOS_ROOT}/src/userland/grain_parser.cpp
  ${AIOS_ROOT}/src/userland/grain_compiler.cpp
  ${AIOS_ROOT}/src/userland/grain_vm.cpp
  ${AIOS_ROOT}/src/userland/grain_jit_arm64.cpp
  ${AIOS_ROOT}/src/userland/grain_builtins.cpp
  ${AIOS_ROOT}/src/userland/grain_std_hash.cpp
  ${AIOS_ROOT}/src/userland/grain_std_regex.cpp
  ${AIOS_ROOT}/src/userland/grain_std_time.cpp
  ${AIOS_ROOT}/src/userland/grain_std_text.cpp
  ${AIOS_ROOT}/src/userland/grain_std_path.cpp
  ${AIOS_ROOT}/src/js/regexp.cpp
  ${AIOS_ROOT}/src/js/unicode.cpp
  ${AIOS_ROOT}/src/userland/grain_std_http.cpp
  ${AIOS_ROOT}/src/channel/http_client.cpp
  ${AIOS_ROOT}/src/net/tls.cpp
  ${AIOS_ROOT}/src/net/tls_conn.cpp
  ${AIOS_ROOT}/src/node/secure.cpp
  ${AIOS_ROOT}/src/crypto/aes_gcm.cpp
  ${AIOS_ROOT}/src/crypto/curve25519.cpp
  ${AIOS_ROOT}/src/crypto/nistp.cpp
  ${AIOS_ROOT}/src/crypto/rsa.cpp
  ${AIOS_ROOT}/src/crypto/x509.cpp
  ${AIOS_ROOT}/src/core/base64.cpp
  ${AIOS_ROOT}/src/core/result.cpp
  ${AIOS_ROOT}/src/userland/grain_sockets.cpp
  ${AIOS_ROOT}/src/crypto/sha256.cpp
  ${AIOS_ROOT}/src/crypto/sha1.cpp
  ${AIOS_ROOT}/src/crypto/sha512.cpp
  ${AIOS_ROOT}/src/crypto/md5.cpp
  ${AIOS_ROOT}/src/crypto/keccak256.cpp
  ${AIOS_ROOT}/src/crypto/hmac_sha256.cpp
  ${AIOS_ROOT}/src/userland/glyph_engine.cpp
  ${AIOS_ROOT}/src/userland/font_atlas.cpp)

# The host: the machine (files, clocks, randomness - what TLS asks of it), threads
# and sleeping, processes, sockets - one file of each per OS.
if(WIN32)
  set(GRAIN_HOST_SOURCES
    ${AIOS_ROOT}/src/hal/win_host.cpp
    ${AIOS_ROOT}/src/hal/win_sync.cpp
    ${AIOS_ROOT}/src/hal/win_process.cpp
    ${AIOS_ROOT}/src/hal/win_net.cpp)
elseif(APPLE)
  set(GRAIN_HOST_SOURCES
    ${AIOS_ROOT}/src/hal/mac_host.cpp
    ${AIOS_ROOT}/src/hal/posix_sync.cpp
    ${AIOS_ROOT}/src/hal/posix_process.cpp
    ${AIOS_ROOT}/src/hal/mac_net.cpp)
else()
  set(GRAIN_HOST_SOURCES
    ${AIOS_ROOT}/src/hal/linux_host.cpp
    ${AIOS_ROOT}/src/hal/posix_sync.cpp
    ${AIOS_ROOT}/src/hal/posix_process.cpp
    ${AIOS_ROOT}/src/hal/linux_net.cpp)
endif()

add_library(grain_lang STATIC ${GRAIN_LANG_SOURCES} ${GRAIN_HOST_SOURCES} ${AIOS_ROOT}/cli/grain_net.cpp)
target_include_directories(grain_lang PUBLIC ${AIOS_ROOT}/include PRIVATE ${AIOS_ROOT}/cli)
find_package(Threads REQUIRED)
target_link_libraries(grain_lang PUBLIC Threads::Threads)
# The host layer without the telemetry it feeds aios's world model (src/hal/linux_host.cpp).
target_compile_definitions(grain_lang PRIVATE AIOS_HAL_HOST_ONLY)
if(WIN32)
  target_link_libraries(grain_lang PUBLIC ws2_32)   # win_net.cpp: Winsock
  target_compile_definitions(grain_lang PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  # font_atlas.cpp is 11 MB of glyph tables: past MSVC's default object section count.
  target_compile_options(grain_lang PRIVATE /bigobj /utf-8 /W3)
else()
  target_compile_options(grain_lang PRIVATE -Wall -Wno-unused-parameter)
endif()

add_executable(grain cli/main.cpp cli/host.cpp)
target_include_directories(grain PRIVATE ${AIOS_ROOT}/cli)
target_link_libraries(grain PRIVATE grain_lang)
target_compile_definitions(grain PRIVATE GRAIN_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(grain PRIVATE /utf-8 /W3)
endif()

install(TARGETS grain RUNTIME DESTINATION bin)

enable_testing()
add_subdirectory(tests)
