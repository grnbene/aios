# grain in a browser: the sources `grain` is built from (GRAIN_LANG_SOURCES, one list, never two copies), without
# what needs a machine a page does not have - processes, sockets, TLS, mail, the database drivers, the app server -
# plus a pane session (pane.cpp), the WebAssembly host (src/hal/wasm_host.cpp), http_* over the page (http_browser.cpp), what a page cannot do said as errors (unavailable.cpp), blank
# font tables (no_fonts.cpp: a page draws its own text) and one entry point (grain_wasm.cpp). Native code generation says no on this architecture: everything is interpreted.
#
#   emcmake cmake -S grain -B build-grain-wasm -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-grain-wasm --parallel
#   node grain/wasm/run.mjs build-grain-wasm/grain_wasm.js program.gr
#
# Included from grain/CMakeLists.txt when EMSCRIPTEN is set.
set(GRAIN_WASM_EXCLUDE
  grain_webui grain_app runtime_registry http_stream grain_std_mail grain_std_process
  /smtp mail_message line_stream grain_std_db grain_std_events /tls tls_conn node/secure curve25519 nistp
  /rsa x509 grain_sockets font_atlas)
set(GRAIN_WASM_SOURCES ${GRAIN_LANG_SOURCES})
foreach(pattern IN LISTS GRAIN_WASM_EXCLUDE)
  list(FILTER GRAIN_WASM_SOURCES EXCLUDE REGEX "${pattern}")
endforeach()

add_executable(grain_wasm ${GRAIN_WASM_SOURCES}
  ${AIOS_ROOT}/src/hal/wasm_host.cpp
  ${AIOS_ROOT}/src/hal/posix_sync.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/wasm/no_fonts.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/wasm/http_browser.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/wasm/unavailable.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/wasm/pane.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/wasm/grain_wasm.cpp)
target_include_directories(grain_wasm PRIVATE ${AIOS_ROOT}/include)
target_compile_definitions(grain_wasm PRIVATE AIOS_HAL_HOST_ONLY GRAIN_WASM)
target_compile_options(grain_wasm PRIVATE -Wall -Wno-unused-parameter)
set_target_properties(grain_wasm PROPERTIES SUFFIX ".js")
target_link_options(grain_wasm PRIVATE
  --no-entry
  -sMODULARIZE=1 -sEXPORT_NAME=createGrain -sEXPORT_ES6=1
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=2097152
  -sEXPORTED_FUNCTIONS=_grain_run,_grain_free,_grain_pane_open,_grain_pane_event,_grain_pane_tick,_grain_pane_close,_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,FS
  -sFORCE_FILESYSTEM=1)

# The page client and the stylesheet, from the same text the server's page is made of.
set(GRAIN_PANE_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/grain-pane.js ${CMAKE_CURRENT_BINARY_DIR}/grain-pane.css)
add_custom_command(OUTPUT ${GRAIN_PANE_ASSETS}
  COMMAND ${CMAKE_COMMAND} -DINC=${AIOS_ROOT}/src/userland/grain_webui_assets.inc -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/wasm/extract-assets.cmake
  DEPENDS ${AIOS_ROOT}/src/userland/grain_webui_assets.inc ${CMAKE_CURRENT_SOURCE_DIR}/wasm/extract-assets.cmake)
add_custom_target(grain_pane_assets ALL DEPENDS ${GRAIN_PANE_ASSETS})
add_dependencies(grain_wasm grain_pane_assets)
