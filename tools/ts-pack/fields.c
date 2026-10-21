/* The table pointers of a TSLanguage, by name. The struct is private to tree-sitter (lib/src/parser.h), which is C. */
#include <string.h>

#include "parser.h"

const void *aios_ts_field(const TSLanguage *l, const char *name) {
  if (!strcmp(name, "ts_parse_table")) return l->parse_table;
  if (!strcmp(name, "ts_small_parse_table")) return l->small_parse_table;
  if (!strcmp(name, "ts_small_parse_table_map")) return l->small_parse_table_map;
  if (!strcmp(name, "ts_parse_actions")) return l->parse_actions;
  if (!strcmp(name, "ts_lex_modes")) return l->lex_modes;
  if (!strcmp(name, "ts_primary_state_ids")) return l->primary_state_ids;
  if (!strcmp(name, "ts_alias_sequences")) return l->alias_sequences;
  return 0;
}
