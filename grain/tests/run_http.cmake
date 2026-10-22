# The HTTP client test: tests/net/server.py and `grain run http_local.gr` started
# together (one execute_process pipeline), the client's output compared with
# EXPECTED. The server writes its port to PORT_FILE and stops when the client
# asks for /quit; the test's TIMEOUT ends both if the client dies first.
#
#   cmake -DPYTHON=... -DGRAIN=... -DNET_DIR=... -DPORT_FILE=... -DEXPECTED=... [-DUPDATE=1] -P run_http.cmake
foreach(v PYTHON GRAIN NET_DIR PORT_FILE EXPECTED)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "run_http.cmake: ${v} is not set")
  endif()
endforeach()
file(REMOVE ${PORT_FILE})
execute_process(
  COMMAND ${PYTHON} ${NET_DIR}/server.py ${PORT_FILE}
  COMMAND ${GRAIN} run http_local.gr ${PORT_FILE}
  WORKING_DIRECTORY ${NET_DIR}
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULTS_VARIABLE codes)
file(REMOVE ${PORT_FILE})
string(REPLACE "\r\n" "\n" out "${out}")
if(UPDATE)
  file(WRITE ${EXPECTED} "${out}")
  message(STATUS "updated ${EXPECTED} (exit codes ${codes})")
  return()
endif()
if(NOT "${codes}" STREQUAL "0;0")
  message(FATAL_ERROR "server and client exited ${codes} (both should be 0)\n--- output ---\n${out}\n--- errors ---\n${err}")
endif()
file(READ ${EXPECTED} expected)
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT "${out}" STREQUAL "${expected}")
  message(FATAL_ERROR "output differs from ${EXPECTED}\n--- expected ---\n${expected}\n--- got ---\n${out}\n--- errors ---\n${err}")
endif()
