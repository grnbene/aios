# One golden test: runs `grain [ARGS]` in WORKDIR and compares what it printed
# (stdout, then stderr) and its exit status with EXPECTED.
#
#   cmake -DGRAIN=... -DARGS="run@@x.gr" -DWORKDIR=... -DEXPECTED=x.out -DSTATUS=0 [-DCOPY_FROM=dir] [-DACTUAL=...] [-DUPDATE=1] -P run_case.cmake
#
# Lines that report how long something took differ from run to run and machine
# to machine; they are dropped before comparing. UPDATE=1 writes the output as the new
# expectation instead of comparing (review the diff before committing it).
foreach(v GRAIN ARGS WORKDIR EXPECTED STATUS)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "run_case.cmake: ${v} is not set")
  endif()
endforeach()

string(REPLACE "@@" ";" grain_args "${ARGS}")
# COPY_FROM: the case writes files, so it runs in a directory of its own with
# its script copied in (the script is the argument after "run").
if(DEFINED COPY_FROM)
  list(FIND grain_args run at)
  math(EXPR at "${at} + 1")
  list(GET grain_args ${at} script)
  file(REMOVE_RECURSE ${WORKDIR})
  file(MAKE_DIRECTORY ${WORKDIR})
  file(COPY ${COPY_FROM}/${script} DESTINATION ${WORKDIR})
endif()
set(stdin_arg "")
if(DEFINED INPUT)   # a script of what to type, for the interactive commands
  set(stdin_arg INPUT_FILE ${INPUT})
endif()
execute_process(
  COMMAND ${GRAIN} ${grain_args}
  WORKING_DIRECTORY ${WORKDIR}
  ${stdin_arg}
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE status)

set(actual "${out}${err}")
string(REPLACE "\r\n" "\n" actual "${actual}")
# Timings: "... in 6 ms", "12.5 ms". Machine facts that vary: none are printed by the cases.
string(REGEX REPLACE "[^\n]*[0-9] ?ms[^\n]*\n" "" actual "${actual}")

if(UPDATE)
  file(WRITE ${EXPECTED} "${actual}")
  file(WRITE ${EXPECTED}.status "${status}\n")
  message(STATUS "updated ${EXPECTED} (exit ${status})")
  return()
endif()

if(NOT EXISTS ${EXPECTED})
  message(FATAL_ERROR "no expected output ${EXPECTED}; run the case with -DUPDATE=1 and review it\n--- got ---\n${actual}")
endif()
file(READ ${EXPECTED} expected)
string(REPLACE "\r\n" "\n" expected "${expected}")
if(STATUS STREQUAL "recorded")   # the case's status is whatever was recorded and reviewed with its output
  if(NOT EXISTS ${EXPECTED}.status)
    message(FATAL_ERROR "no recorded exit status ${EXPECTED}.status")
  endif()
  file(STRINGS ${EXPECTED}.status STATUS LIMIT_COUNT 1)
endif()
if(NOT "${status}" STREQUAL "${STATUS}")
  message(FATAL_ERROR "exit status ${status}, expected ${STATUS}\n--- output ---\n${actual}")
endif()
if(NOT "${actual}" STREQUAL "${expected}")
  if(DEFINED ACTUAL)   # kept in the build tree for a diff: the source may be read-only
    file(WRITE ${ACTUAL} "${actual}")
    set(kept " (what it printed is in ${ACTUAL})")
  endif()
  message(FATAL_ERROR "output differs from ${EXPECTED}${kept}\n--- expected ---\n${expected}\n--- got ---\n${actual}")
endif()
