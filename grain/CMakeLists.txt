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
  ${AIOS_ROOT}/src/userland/grain_jit_x64.cpp
  ${AIOS_ROOT}/src/userland/grain_builtins.cpp
  ${AIOS_ROOT}/src/userland/grain_std_hash.cpp
  ${AIOS_ROOT}/src/userland/grain_std_bytes.cpp
  ${AIOS_ROOT}/src/userland/grain_native.cpp
  ${AIOS_ROOT}/src/userland/grain_webui.cpp
  ${AIOS_ROOT}/src/userland/grain_app.cpp
  ${AIOS_ROOT}/src/userland/grain_app_cli.cpp
  ${AIOS_ROOT}/src/userland/runtime_registry.cpp
  ${AIOS_ROOT}/src/net/http_stream.cpp
  ${AIOS_ROOT}/src/userland/grain_std_regex.cpp
  ${AIOS_ROOT}/src/userland/grain_std_time.cpp
  ${AIOS_ROOT}/src/userland/grain_std_text.cpp
  ${AIOS_ROOT}/src/userland/grain_std_path.cpp
  ${AIOS_ROOT}/src/userland/grain_std_parallel.cpp
  ${AIOS_ROOT}/src/userland/grain_std_decimal.cpp
  ${AIOS_ROOT}/src/js/regexp.cpp
  ${AIOS_ROOT}/src/js/unicode.cpp
  ${AIOS_ROOT}/src/userland/grain_std_http.cpp
  ${AIOS_ROOT}/src/userland/grain_std_mail.cpp
  ${AIOS_ROOT}/src/userland/grain_std_kv.cpp
  ${AIOS_ROOT}/src/userland/grain_std_process.cpp
  ${AIOS_ROOT}/src/net/smtp.cpp
  ${AIOS_ROOT}/src/net/line_stream.cpp
  ${AIOS_ROOT}/src/userland/grain_std_db.cpp
  ${AIOS_ROOT}/src/userland/grain_std_events.cpp
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
  ${AIOS_ROOT}/src/core/json.cpp
  ${AIOS_ROOT}/src/core/ids.cpp
  ${AIOS_ROOT}/src/userland/grain_sockets.cpp
  ${AIOS_ROOT}/src/crypto/sha256.cpp
  ${AIOS_ROOT}/src/crypto/sha1.cpp
  ${AIOS_ROOT}/src/crypto/sha512.cpp
  ${AIOS_ROOT}/src/crypto/md5.cpp
  ${AIOS_ROOT}/src/crypto/keccak256.cpp
  ${AIOS_ROOT}/src/crypto/hmac_sha256.cpp
  ${AIOS_ROOT}/src/crypto/bcrypt.cpp
  ${AIOS_ROOT}/src/userland/glyph_engine.cpp
  ${AIOS_ROOT}/src/userland/font_atlas.cpp
  ${AIOS_ROOT}/src/userland/font_kerning.cpp
  # face widgets (ui.*): face's layout and painting, its type and pixels, PNG for its images
  ${AIOS_ROOT}/src/userland/grain_face.cpp
  ${AIOS_ROOT}/src/userland/face.cpp
  ${AIOS_ROOT}/src/userland/pixels.cpp
  ${AIOS_ROOT}/src/userland/web_type.cpp
  ${AIOS_ROOT}/src/userland/web_image.cpp
  ${AIOS_ROOT}/src/userland/web_dom.cpp
  ${AIOS_ROOT}/src/userland/web_css.cpp
  ${AIOS_ROOT}/src/userland/web_svg.cpp
  ${AIOS_ROOT}/src/userland/web_layout.cpp
  ${AIOS_ROOT}/src/userland/grain_std_doc.cpp
  ${AIOS_ROOT}/src/userland/archive.cpp
  ${AIOS_ROOT}/src/net/deflate.cpp)

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
# shm_open/shm_unlink (linux_host.cpp) live in librt before glibc 2.34 (CentOS 7, Ubuntu 20.04);
# newer glibc folds it into libc and an empty librt still links.
if(UNIX AND NOT APPLE)
  find_library(GRAIN_RT_LIB rt)
  if(GRAIN_RT_LIB)
    target_link_libraries(grain_lang PUBLIC ${GRAIN_RT_LIB})
  endif()
endif()
# The host layer without the telemetry it feeds aios's world model (src/hal/linux_host.cpp).
target_compile_definitions(grain_lang PRIVATE AIOS_HAL_HOST_ONLY)
if(WIN32)
  target_link_libraries(grain_lang PUBLIC ws2_32 bcrypt crypt32 iphlpapi)   # Winsock; random bytes; the system trust store; interfaces - all part of Windows
  target_compile_definitions(grain_lang PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  # font_atlas.cpp is 11 MB of glyph tables: past MSVC's default object section count.
  target_compile_options(grain_lang PRIVATE /bigobj /utf-8 /W3)
else()
  target_compile_options(grain_lang PRIVATE -Wall -Wno-unused-parameter)
endif()

add_executable(grain cli/main.cpp cli/host.cpp cli/ffi.cpp cli/plugin.cpp cli/serve.cpp cli/open_client.cpp cli/bundle.cpp cli/tools_fmt.cpp cli/tools_test.cpp cli/tools_lsp.cpp cli/app_cmd.cpp cli/pkg_cmd.cpp cli/debug_cmd.cpp)
target_include_directories(grain PRIVATE ${AIOS_ROOT}/cli ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(grain PRIVATE grain_lang ${CMAKE_DL_LIBS})

# Windows for panes (run_panes). On Linux grain speaks the X11 protocol itself
# (cli/display_x11.cpp): no library, and the same on Wayland through XWayland.
# On Windows it calls the Win32 API (cli/display_win32.cpp): user32 and gdi32 are
# the operating system, not a library. On macOS SDL2 is used until grain has its
# own Cocoa code.
set(GRAIN_NATIVE_DISPLAY OFF)
if((UNIX AND NOT APPLE) OR WIN32)
  set(GRAIN_NATIVE_DISPLAY ON)
endif()

# Windows for panes (run_panes) through SDL2, when it is installed: Homebrew's
# sdl2, a distribution's libsdl2-dev, or vcpkg's sdl2 on Windows. Without it
# grain builds the same and run_panes() says it has no display.
option(GRAIN_WITH_SDL2 "Show panes as windows through SDL2 when SDL2 is found (not on Linux, which has its own)" ON)
if(GRAIN_NATIVE_DISPLAY)
  set(GRAIN_WITH_SDL2 OFF)
endif()
# For a program you ship (grain build): SDL2 compiled from its release and
# linked in, so the executable needs no SDL on the machine it runs on. Fetches
# SDL's own repository at a release tag; a build without network access uses
# the installed SDL2 instead (leave this OFF). On Linux the X11 and Wayland
# development headers must be installed for SDL to talk to a desktop.
option(GRAIN_SDL2_STATIC "Build SDL2 from source and link it statically" OFF)
if(GRAIN_WITH_SDL2 AND GRAIN_SDL2_STATIC)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2)
  set(GRAIN_SDL2_TARGET SDL2::SDL2-static)
elseif(GRAIN_WITH_SDL2)
  find_package(SDL2 CONFIG QUIET)
  if(TARGET SDL2::SDL2)
    set(GRAIN_SDL2_TARGET SDL2::SDL2)
  elseif(TARGET SDL2::SDL2-static)
    set(GRAIN_SDL2_TARGET SDL2::SDL2-static)
  else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(GRAIN_SDL2 QUIET IMPORTED_TARGET sdl2)
      if(GRAIN_SDL2_FOUND)
        set(GRAIN_SDL2_TARGET PkgConfig::GRAIN_SDL2)
      endif()
    endif()
  endif()
endif()
if(GRAIN_NATIVE_DISPLAY AND WIN32)
  target_sources(grain PRIVATE cli/display_win32.cpp)
  target_link_libraries(grain PRIVATE user32 gdi32)
  target_compile_definitions(grain PRIVATE GRAIN_HAVE_WIN32)
  message(STATUS "grain: panes show as windows (Win32 API, no library)")
elseif(GRAIN_NATIVE_DISPLAY)
  target_sources(grain PRIVATE cli/display_x11.cpp)
  target_compile_definitions(grain PRIVATE GRAIN_HAVE_X11)
  message(STATUS "grain: panes show as windows (X11 protocol, no library)")
elseif(GRAIN_SDL2_TARGET)
  target_sources(grain PRIVATE cli/display_sdl.cpp)
  target_link_libraries(grain PRIVATE ${GRAIN_SDL2_TARGET})
  target_compile_definitions(grain PRIVATE GRAIN_HAVE_SDL2)
  message(STATUS "grain: panes show as windows (SDL2)")
else()
  message(STATUS "grain: SDL2 not found - run_panes() will report that this grain has no display")
endif()
target_compile_definitions(grain PRIVATE GRAIN_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(grain PRIVATE /utf-8 /W3)
endif()

install(TARGETS grain RUNTIME DESTINATION bin)

enable_testing()
add_subdirectory(tests)
