# Golden tests: each case's output and exit status, checked in under expected/.
# `cmake --build <dir> --target grain-update-expected` rewrites them from the
# current build - for a deliberate change in behaviour, reviewed in the diff.
set(GRAIN_EXAMPLES ${AIOS_ROOT}/examples/grain)
set(GRAIN_JIT_CASES ${AIOS_ROOT}/tools/grain-jit/cases)
set(EXPECTED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/expected)
set(RUNNER ${CMAKE_CURRENT_SOURCE_DIR}/run_case.cmake)
set(GRAIN_UPDATE_COMMANDS)

# grain_case(<name> <workdir> <expected exit> <grain args...>)
# A case runs in <workdir>; a case whose program writes files names "WRITES:<script dir>"
# instead, and runs in a work directory of its own in the build tree with its
# script copied there - so parallel cases never share a scratch file, the source
# can be read-only, and error messages still name the script the same short way.
function(grain_case name workdir status)
  string(JOIN "@@" args_escaped ${ARGN})
  set(copy "")
  if(workdir MATCHES "^WRITES:(.*)$")
    set(copy -DCOPY_FROM=${CMAKE_MATCH_1})
    set(workdir ${CMAKE_CURRENT_BINARY_DIR}/work/${name})
  endif()
  add_test(NAME grain.${name}
    COMMAND ${CMAKE_COMMAND} -DGRAIN=$<TARGET_FILE:grain> "-DARGS=${args_escaped}" -DWORKDIR=${workdir} ${copy}
            -DEXPECTED=${EXPECTED_DIR}/${name}.out -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/actual/${name}.out
            -DSTATUS=${status} -P ${RUNNER})
  set(GRAIN_UPDATE_COMMANDS ${GRAIN_UPDATE_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E env GRAIN_VIDEO=dummy SDL_VIDEODRIVER=dummy ${CMAKE_COMMAND} -DGRAIN=$<TARGET_FILE:grain> "-DARGS=${args_escaped}" -DWORKDIR=${workdir} ${copy}
            -DEXPECTED=${EXPECTED_DIR}/${name}.out -DSTATUS=${status} -DUPDATE=1 -P ${RUNNER}
    PARENT_SCOPE)
endfunction()

# The language's own examples, natively compiled where the machine can and interpreted.
foreach(ex tour closures words fib)   # conflicts.gr reads Things: aios only
  grain_case(example.${ex} ${GRAIN_EXAMPLES} 0 run ${ex}.gr)
  grain_case(example.${ex}.interp ${GRAIN_EXAMPLES} 0 --no-jit run ${ex}.gr)
endforeach()
grain_case(example.selftest WRITES:${GRAIN_EXAMPLES} 0 run selftest.gr)          # its file checks write
grain_case(example.selftest.interp WRITES:${GRAIN_EXAMPLES} 0 --no-jit run selftest.gr)

# The native-code comparison cases (tools/grain-jit). Some end in a runtime
# error on purpose (a runaway loop the step limit stops, a fault deep in native
# code), so their exit status is the one recorded with their output, and they
# run under the 200M-step limit their runner gave them.
file(GLOB jit_cases RELATIVE ${GRAIN_JIT_CASES} ${GRAIN_JIT_CASES}/*.gr)
foreach(f ${jit_cases})
  get_filename_component(n ${f} NAME_WE)
  grain_case(jit.${n} ${GRAIN_JIT_CASES} recorded --steps 200000000 run ${f})
  grain_case(jit.${n}.interp ${GRAIN_JIT_CASES} recorded --steps 200000000 --no-jit run ${f})
endforeach()

# What the standalone program itself promises.
set(CLI_CASES ${CMAKE_CURRENT_SOURCE_DIR}/cli)
grain_case(cli.args ${CLI_CASES} 0 run args.gr one "two words" 3)
grain_case(cli.shortcut ${CLI_CASES} 0 args.gr via-shortcut)
grain_case(cli.sys ${CLI_CASES} 0 run sys.gr)
grain_case(cli.files WRITES:${CLI_CASES} 0 run files.gr)
grain_case(cli.stdlib WRITES:${CLI_CASES} 0 run stdlib.gr)
grain_case(cli.exec ${CLI_CASES} 0 run exec.gr)
grain_case(cli.compile_error ${CLI_CASES} 1 run compile_error.gr)
grain_case(cli.runtime_error ${CLI_CASES} 2 run runtime_error.gr)
grain_case(cli.check_ok ${CLI_CASES} 0 check args.gr sys.gr files.gr)
grain_case(cli.check_error ${CLI_CASES} 1 check args.gr compile_error.gr)
grain_case(cli.steps ${CLI_CASES} 2 --steps 1000 run forever.gr)
grain_case(cli.eval ${CLI_CASES} 0 eval "1 + 2 * 3")
grain_case(cli.missing_file ${CLI_CASES} 64 run no_such_file.gr)
grain_case(cli.unknown_command ${CLI_CASES} 64 frobnicate)
grain_case(cli.version ${CLI_CASES} 0 version)
grain_case(cli.dis ${CLI_CASES} 0 dis args.gr)


# The HTTP client against a local server (tests/net): needs Python 3 to run the
# server, and nothing beyond this machine. The live HTTPS checks
# (tests/net/tls_live.gr) need the internet and are run by hand.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(HTTP_ARGS -DPYTHON=${Python3_EXECUTABLE} -DGRAIN=$<TARGET_FILE:grain> -DNET_DIR=${CMAKE_CURRENT_SOURCE_DIR}/net
                -DPORT_FILE=${CMAKE_CURRENT_BINARY_DIR}/http_port.txt -DEXPECTED=${EXPECTED_DIR}/net.http_local.out)
  add_test(NAME grain.net.http_local COMMAND ${CMAKE_COMMAND} ${HTTP_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/run_http.cmake)
  set_tests_properties(grain.net.http_local PROPERTIES TIMEOUT 60)
  add_custom_target(grain-update-expected-http
    COMMAND ${CMAKE_COMMAND} ${HTTP_ARGS} -DUPDATE=1 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_http.cmake DEPENDS grain)
else()
  message(STATUS "grain: no Python 3, so the HTTP client test (grain.net.http_local) is not run")
endif()

# Panes as windows (run_panes), driven by scripted input through SDL's own event
# queue, headless: SDL's dummy video driver needs no screen. Only in a grain
# built with SDL2.
if(GRAIN_SDL2_TARGET OR GRAIN_NATIVE_DISPLAY)
  grain_case(window.panes ${CMAKE_CURRENT_SOURCE_DIR}/window 0 run panes.gr)
  set_tests_properties(grain.window.panes PROPERTIES ENVIRONMENT "GRAIN_VIDEO=dummy;SDL_VIDEODRIVER=dummy" TIMEOUT 60)
  # Face widgets (ui.*) in a pane: fields, Enter, buttons found by ui.layout, and a view that fails.
  grain_case(window.face ${CMAKE_CURRENT_SOURCE_DIR}/window 2 run face.gr)
  set_tests_properties(grain.window.face PROPERTIES ENVIRONMENT "GRAIN_VIDEO=dummy;SDL_VIDEODRIVER=dummy" TIMEOUT 60)
endif()

# Keyboard layouts on Linux's own X11 backend: a fake X server with US and Russian
# layouts (and AltGr levels) sends key events that name the active layout.
if(GRAIN_NATIVE_DISPLAY AND Python3_Interpreter_FOUND)
  add_test(NAME grain.window.x11_layouts
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/window/x11_layouts.py $<TARGET_FILE:grain> ${CMAKE_CURRENT_SOURCE_DIR}/window/layouts.gr)
  set_tests_properties(grain.window.x11_layouts PROPERTIES TIMEOUT 60)
endif()

# grain build: one executable from a program and its assets, run from elsewhere.
set(BUILD_ARGS -DGRAIN=$<TARGET_FILE:grain> -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/build
               -DOUT=${CMAKE_CURRENT_BINARY_DIR}/built/hello -DEXPECTED=${EXPECTED_DIR}/build.hello.out)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/built)
add_test(NAME grain.build.hello COMMAND ${CMAKE_COMMAND} ${BUILD_ARGS} -P ${CMAKE_CURRENT_SOURCE_DIR}/run_build.cmake)
set(GRAIN_UPDATE_COMMANDS ${GRAIN_UPDATE_COMMANDS}
  COMMAND ${CMAKE_COMMAND} ${BUILD_ARGS} -DUPDATE=1 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_build.cmake)

# The developer tools.
set(TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/tools)
grain_case(tools.test ${TOOLS}/runner 1 test)
# The bytes type and its typed reads, writes, pack and unpack.
grain_case(lowlevel.bytes ${CMAKE_CURRENT_SOURCE_DIR}/bytes 0 test)
# A plugin written in C++ against include/grain_plugin.h, built here and loaded by a grain
# program that sits beside it (the test is copied next to the library it loads).
if(NOT WIN32)
  set(PLUGIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/plugins)
  add_library(grain_plugin_vec MODULE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/plugin/vec.cpp)
  target_include_directories(grain_plugin_vec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  set_target_properties(grain_plugin_vec PROPERTIES PREFIX "lib" CXX_VISIBILITY_PRESET hidden
                        LIBRARY_OUTPUT_DIRECTORY ${PLUGIN_DIR})
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/plugin/plugin_test.gr ${PLUGIN_DIR}/plugin_test.gr COPYONLY)
  grain_case(lowlevel.plugin ${PLUGIN_DIR} 0 test)
  set(GRAIN_PLUGIN_TARGET grain_plugin_vec)
endif()
# Native functions: typed code compiled to machine code (x86-64 Linux/macOS so far).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT WIN32)
  grain_case(lowlevel.native ${CMAKE_CURRENT_SOURCE_DIR}/native 0 test)
  grain_case(lowlevel.native_errors ${CMAKE_CURRENT_SOURCE_DIR}/native 1 check bad.gr)
  # An unsafe program under --safe: ptr, extern fn and raw access are refused, with the reason.
  grain_case(lowlevel.native_safe ${CMAKE_CURRENT_SOURCE_DIR}/native 1 --safe check unsafe_test.gr)
endif()
# A ui.pane program in a browser (`grain serve`): the protocol with Python, and the page script in a real
# Chrome when there is one. Both start their own servers on free ports.
if(NOT WIN32 AND Python3_Interpreter_FOUND)
  add_test(NAME grain.web.ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web/web_ui_test.py $<TARGET_FILE:grain> ${CMAKE_CURRENT_SOURCE_DIR}/web/counter.gr)
  set_tests_properties(grain.web.ui PROPERTIES TIMEOUT 60)
  add_test(NAME grain.web.native_client COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web/native_client_test.py $<TARGET_FILE:grain> ${CMAKE_CURRENT_SOURCE_DIR}/web/counter.gr)
  set_tests_properties(grain.web.native_client PROPERTIES TIMEOUT 120)
  add_test(NAME grain.web.live COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web/live_test.py $<TARGET_FILE:grain> ${CMAKE_CURRENT_SOURCE_DIR}/web/live.gr)
  set_tests_properties(grain.web.live PROPERTIES TIMEOUT 120)
  find_program(GRAIN_CHROME NAMES google-chrome chromium chromium-browser chrome)
  if(GRAIN_CHROME)
    add_test(NAME grain.web.browser COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/web/browser_test.py $<TARGET_FILE:grain> ${GRAIN_CHROME}
             ${CMAKE_CURRENT_SOURCE_DIR}/web/counter.gr ${CMAKE_CURRENT_SOURCE_DIR}/../examples/sysmon.gr ${CMAKE_CURRENT_SOURCE_DIR}/web/live.gr)
    set_tests_properties(grain.web.browser PROPERTIES TIMEOUT 120)
  else()
    message(STATUS "grain: no Chrome found, so the in-browser test (grain.web.browser) is not run")
  endif()
endif()

# Calling C from grain (the standalone grain only; not on Windows yet).
if(NOT WIN32)
  grain_case(lowlevel.ffi ${CMAKE_CURRENT_SOURCE_DIR}/ffi 0 test)
endif()
grain_case(tools.fmt_check_clean ${TOOLS} 0 fmt --check fmt_expected.gr)
grain_case(tools.fmt_check_dirty ${TOOLS} 1 fmt --check fmt_input.gr)
add_test(NAME grain.tools.fmt COMMAND ${CMAKE_COMMAND} -DGRAIN=$<TARGET_FILE:grain> -DINPUT=${TOOLS}/fmt_input.gr
         -DEXPECTED=${TOOLS}/fmt_expected.gr -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work/tools.fmt -P ${TOOLS}/run_fmt.cmake)
if(Python3_Interpreter_FOUND)
  add_test(NAME grain.tools.lsp COMMAND ${CMAKE_COMMAND} -DGRAIN=${Python3_EXECUTABLE} "-DARGS=${TOOLS}/lsp_client.py@@$<TARGET_FILE:grain>"
           -DWORKDIR=${TOOLS} -DEXPECTED=${EXPECTED_DIR}/tools.lsp.out -DSTATUS=0 -P ${RUNNER})
  set(GRAIN_UPDATE_COMMANDS ${GRAIN_UPDATE_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -DGRAIN=${Python3_EXECUTABLE} "-DARGS=${TOOLS}/lsp_client.py@@$<TARGET_FILE:grain>"
            -DWORKDIR=${TOOLS} -DEXPECTED=${EXPECTED_DIR}/tools.lsp.out -DSTATUS=0 -DUPDATE=1 -P ${RUNNER})
endif()

# Last, so it rewrites every case above (each headless: a window case shows on SDL's dummy display).
add_custom_target(grain-update-expected ${GRAIN_UPDATE_COMMANDS} DEPENDS grain ${GRAIN_PLUGIN_TARGET}
  COMMENT "rewriting grain/tests/expected from the current build")

# Panes belong to the VM that registered them (two VMs in one process).
add_executable(grain-pane-isolation pane_isolation.cpp)
target_link_libraries(grain-pane-isolation PRIVATE grain_lang)
add_test(NAME grain.pane_isolation COMMAND grain-pane-isolation)
