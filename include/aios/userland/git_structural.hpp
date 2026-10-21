#pragma once
// Language-aware merge: a file both sides changed, merged as a syntax tree instead of as lines.
//
// The line merge conflicts whenever two edits touch neighbouring lines, even when they are two new functions added
// at the end of a file, two methods added to one class, two keys added to one JSON object, or an import each. Here
// the three versions are parsed with tree-sitter (JavaScript, TypeScript/TSX, Python, Go, Rust, C, C++, Java, C#, JSON, PHP,
// SQL, YAML, TOML, XML: .xml .axaml .xaml .csproj .props .targets .resx .svg) and merged as ordered lists of keyed units: a declaration is known by its kind and name (a
// function, a class, a method, a variable, an import, an object key, a table...), and units both sides changed are
// merged one level further in (a class's members, an object's keys). A collection's items (a YAML list's entries, a
// PHP array's values, a SQL script's INSERTs) are known by their text: each side's new ones are kept. Each side's
// additions keep their place next to what preceded them on that side; a unit one side deleted and the other left
// alone is deleted; one changed by both in the same place, or deleted by one and changed by the other, is a real
// conflict and the merge gives up.
//
// It never guesses: it gives up (nullopt) on anything it cannot place exactly (a file that does not parse, two
// sides both rewriting unnamed statements at one level, conflicting reorderings), and a result that does not parse
// again is thrown away. Giving up leaves the line merge's verdict standing, so this can only turn a conflict into a
// clean merge, never a clean merge into anything else.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aios/userland/git_merge.hpp"
#include "aios/userland/git_syntax.hpp"

namespace aios::userland {

// Which grammar a path's file is read with: "javascript", "typescript", "tsx", "python", "go", "rust", "c", "cpp", "java",
// "csharp", "xml", "json", "php", "sql", "yaml", "toml", or "" when none.
std::string git_structural_language(std::string_view path);

// Whether `text` parses without errors as the language of `path` (true for a path no grammar here reads).
bool git_structural_parses(std::string_view path, std::string_view text);

// The merged file when the three versions merge cleanly as syntax trees; nullopt to keep the line merge's result.
std::optional<GitMerge3> git_structural_merge(std::string_view path, std::string_view base, std::string_view ours,
                                              std::string_view theirs);
// Why the last git_structural_merge on this thread gave up ("both sides changed what ours deleted", ...).
std::string git_structural_last_reason();

// The named units of a file, at every level the merge descends to: what a lease on part of a file names, and what
// a change to it touched. A scope is the names from the top down, joined by "." ("UserService.find", "jobs.build",
// "dependencies.serde"); a second unit with one name gets "#1", "#2"... after it. Imports, list items and unnamed
// statements are not units here: they belong to what holds them.
//
// With `previous` (another version of the file, typically the one just outlined), the parse is incremental from its
// tree: what an outline of every version of a file in history costs is about one parse plus the edits.
std::optional<GitSyntaxOutline> git_structural_outline(std::string_view path, std::string_view text,
                                                       std::string_view previous = {});
// An outline as bytes (what block files keep for every source blob) and back.
// The same, for a file with local parse errors too (a large C++ file with a macro tree-sitter cannot read): the units it
// could read. For naming what an edit touched, not for merging (merges keep the strict outline).
std::optional<GitSyntaxOutline> git_structural_outline_lenient(std::string_view path, std::string_view text,
                                                               std::string_view previous = {});
std::string git_syntax_outline_encode(const GitSyntaxOutline& o);
std::optional<GitSyntaxOutline> git_syntax_outline_decode(std::string_view bytes);

// The scopes a change from one version to the other touched: a unit added, removed, or changed in itself (not only
// in the units under it). "" when the file's own text outside its units changed (an import, a statement).
std::vector<std::string> git_structural_changed_scopes(const GitSyntaxOutline& a, const GitSyntaxOutline& b);
// The same, saying how: '+' added, '-' removed, '~' changed in itself.
struct GitScopeChange { std::string scope; char how = '~'; };
std::vector<GitScopeChange> git_structural_scope_changes(const GitSyntaxOutline& a, const GitSyntaxOutline& b);
std::optional<std::vector<std::string>> git_structural_changed_scopes(std::string_view path, std::string_view base,
                                                                      std::string_view side);
// A clean line merge, checked unit by unit. The line merge decides by lines alone, so it can be clean and still
// wrong: two sides adding a function of one name (a duplicate definition, which C++ refuses and JavaScript or Python
// silently resolve to the later one), or both sides' insertions at one place unioned into the middle of a unit only
// one side changed. Each unit of the merged file must be what the sides made of it: a unit one side changed is that
// side's version, one neither changed is the base's, a unit neither side has is not there. Units both sides changed
// differently are not judged (the line merge combined them; they are listed as `both`, the places a later check
// such as a compile should look). `ok` false names the first unit that came out wrong in `reason`. A file that does
// not parse in any of the four versions, or that no grammar reads, is not judged (ok).
struct GitMergeAudit {
  bool ok = true;
  std::string reason;
  std::vector<std::string> both;
};
GitMergeAudit git_structural_audit(std::string_view path, std::string_view base, std::string_view ours,
                                   std::string_view theirs, std::string_view merged);

// Whether a unit of this kind is an import (an import or include, a Rust use, a PHP use): no one claims one, so a
// lease check counts it as the file's own text ("").
bool git_syntax_is_import(const std::string& kind);
// Whether two scopes of one file overlap: the same unit, one inside the other, or either the whole file ("").
bool git_scopes_overlap(std::string_view a, std::string_view b);

// The syntax tree as tree-sitter prints it, parsed fresh, or with `from` incrementally from that version's tree (the
// two must print the same).
std::string git_structural_sexp(std::string_view path, std::string_view text, std::string_view from = {});

// How the trees the merge and the outlines read came to be: parsed whole, reparsed incrementally from another
// version's tree, or found in the cache of recent trees.
struct GitStructuralStats { uint64_t full = 0, incremental = 0, cached = 0; };
GitStructuralStats git_structural_stats();

}  // namespace aios::userland
