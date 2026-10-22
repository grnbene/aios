# grain build: packs tests/build/hello.gr and its assets onto a copy of the
# runtime in the build tree, runs the result with arguments, and compares what
# it printed with EXPECTED.
#
#   cmake -DGRAIN=... -DSRC_DIR=... -DOUT=... -DEXPECTED=... [-DUPDATE=1] -P run_build.cmake
foreach(v GRAIN SRC_DIR OUT EXPECTED)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "run_build.cmake: ${v} is not set")
  endif()
endforeach()
file(REMOVE ${OUT} ${OUT}.exe)
execute_process(COMMAND ${GRAIN} build hello.gr -o ${OUT} --include assets
  WORKING_DIRECTORY ${SRC_DIR} RESULT_VARIABLE built OUTPUT_VARIABLE bout ERROR_VARIABLE berr)
if(NOT built EQUAL 0)
  message(FATAL_ERROR "grain build failed (${built})\n${bout}${berr}")
endif()
set(program ${OUT})
if(EXISTS ${OUT}.exe)
  set(program ${OUT}.exe)
endif()
get_filename_component(run_dir ${OUT} DIRECTORY)
execute_process(COMMAND ${program} first "second arg"
  WORKING_DIRECTORY ${run_dir} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REPLACE "\r\n" "\n" out "${out}")
if(UPDATE)
  file(WRITE ${EXPECTED} "${out}")
  return()
endif()
if(NOT status EQUAL 0)
  message(FATAL_ERROR "the built program exited ${status}\n${out}${err}")
endif()
file(READ ${EXPECTED} expected)
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT "${out}" STREQUAL "${expected}")
  message(FATAL_ERROR "the built program printed\n${out}\nexpected\n${expected}")
endif()
