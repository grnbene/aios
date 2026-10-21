# The tree-sitter grammars the structural merge reads (third_party/tree-sitter-grammars, each MIT). One row per
# grammar, fields separated by |: id | directory under third_party/tree-sitter-grammars ; the C function that returns its TSLanguage ;
# | whether it has an external scanner (scanner.c next to parser.c).
#
# Included by CMakeLists.txt (which compiles each grammar from its parser_packed.c), by the aios-ts-pack tool
# (which makes the parser_packed.c files) and by the freshness test (cmake/ts-pack-fresh.cmake).
# (A CMake list splits on ';', so a row is split on '|' by whoever reads it: string(REPLACE "|" ";" row "${row}").)
set(AIOS_TS_GRAMMARS
  "javascript|javascript|tree_sitter_javascript|1"
  "typescript|typescript/typescript/src|tree_sitter_typescript|1"
  "tsx|typescript/tsx/src|tree_sitter_tsx|1"
  "python|python|tree_sitter_python|1"
  "go|go|tree_sitter_go|0"
  "rust|rust|tree_sitter_rust|1"
  "c|c|tree_sitter_c|0"
  "cpp|cpp|tree_sitter_cpp|1"
  "java|java|tree_sitter_java|0"
  "csharp|c-sharp|tree_sitter_c_sharp|1"
  "xml|xml/xml/src|tree_sitter_xml|1"
  "json|json|tree_sitter_json|0"
  "php|php/php/src|tree_sitter_php|1"
  "sql|sql|tree_sitter_sql|1"
  "yaml|yaml|tree_sitter_yaml|1"
  "toml|toml|tree_sitter_toml|1")
