# Writes the page client and its stylesheet out of src/userland/grain_webui_assets.inc - the text the server's page
# is made of - as grain-pane.js (an ES module exporting grainPane) and grain-pane.css, so a page that runs a pane on the
# WebAssembly build uses the same client, not a copy of it.
#
#   cmake -DINC=<grain_webui_assets.inc> -DOUT_DIR=<dir> -P extract-assets.cmake
file(READ "${INC}" text)

# The text of the first raw string R"<tag>( ... )<tag>" in what follows `from`.
function(raw_string tag out_var)
  string(FIND "${text}" "R\"${tag}(" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "no R\"${tag}( in ${INC}")
  endif()
  string(LENGTH "R\"${tag}(" opening)
  math(EXPR from "${at} + ${opening}")
  string(SUBSTRING "${text}" ${from} -1 rest)
  string(FIND "${rest}" ")${tag}\"" until)
  if(until EQUAL -1)
    message(FATAL_ERROR "R\"${tag}( is not closed in ${INC}")
  endif()
  string(SUBSTRING "${rest}" 0 ${until} body)
  string(STRIP "${body}" body)
  set(${out_var} "${body}" PARENT_SCOPE)
endfunction()

raw_string(CSS style)
# the first raw JS string after the style is kPaneFactory (kPageBootstrap, the server's own transport, is the second)
raw_string(JS factory)
file(WRITE "${OUT_DIR}/grain-pane.css" "${style}\n")
file(WRITE "${OUT_DIR}/grain-pane.js" "${factory}\nexport { grainPane };\n")
