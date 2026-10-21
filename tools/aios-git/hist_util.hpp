#pragma once
// Small readers shared by the history commands (blame, shortlog, describe...): idents and the mailmap.
#include <cstdint>
#include <string>
#include <vector>

#include "cli.hpp"

namespace aios_git {

// "Name <email>" as a commit header holds it; `email` is without the angle brackets.
struct IdentParts { std::string name, email; };
IdentParts split_ident(const std::string& ident);

// .mailmap (top of the work tree, else HEAD's) and mailmap.file / mailmap.blob, as git reads them.
class Mailmap {
 public:
  void load(Ctx& c);
  // Rewrites `name` and `email` the way an entry says; returns whether one matched.
  bool map(std::string& name, std::string& email) const;
 private:
  struct Entry { std::string old_name, old_email, new_name, new_email; bool has_old_name = false, has_new_email = false, has_new_name = false; };
  void add_text(const std::string& text);
  std::vector<Entry> entries_;
};

// The packs of this repository: where each object sits, how big its entry is, what it is a delta of.
struct PackedInfo {
  uint64_t offset = 0, disk = 0;
  int type = 0;                 // the entry's own type: 1 commit .. 4 tag, 6 offset delta, 7 reference delta
  uint64_t size = 0;            // the size its header gives (a delta's own size)
  std::string base;             // the object a delta is made against
  std::string pack;             // path of the .pack
  uint32_t crc = 0;
};
class PackSet {
 public:
  void load(Ctx& c);
  bool find(const std::string& oid, PackedInfo& out);
  struct Pack {
    std::string idx_path, pack_path;
    uint64_t pack_size = 0;
    std::vector<std::string> oids;           // in idx order
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> crcs;
    std::vector<std::pair<uint64_t, size_t>> by_offset;
    uint32_t version = 0;
  };
  const std::vector<Pack>& packs() const { return packs_; }
  PackedInfo info(Pack& p, size_t pos);
 private:
  Ctx* c_ = nullptr;
  std::vector<Pack> packs_;
};

// A tag object's fields.
struct TagInfo { std::string target, target_type, name; int64_t date = 0; bool has_date = false; };
bool read_tag_object(Ctx& c, const std::string& oid, TagInfo& out);

// Names for commits from the refs that reach them, as name-rev and describe --contains give them.
struct NameRevOptions {
  bool tags_only = false, name_only = false;
  std::vector<std::string> refs, excludes;
};
class NameRev {
 public:
  NameRev(Ctx& c, NameRevOptions o) : c_(c), o_(std::move(o)) {}
  // Walks from every ref that passes the filters. `cutoff`: commits older than that are not named.
  void run(int64_t cutoff);
  // "main~2", "tags/v1.0^0": empty when nothing names the commit.
  std::string commit_name(const std::string& oid) const;
  // The ref that points straight at this (non-commit) object, or "".
  std::string exact_ref(const std::string& oid) const;
  // --all: the commits git's walk has met, in the order git lists them (its object hash table's slots). Set before
  // run(); the table is replayed as git builds it (see ObjectTable in cmd_namerev.cpp).
  void track_objects();
  std::vector<std::string> commits_in_table_order() const;
 private:
  struct ObjectTable;
  std::shared_ptr<ObjectTable> table_;
  void replay_parse_object(const std::string& oid);   // git's parse_object, on the table
  void replay_parse_commit(const std::string& oid);   // git's repo_parse_commit
  struct Rev { std::string tip; int64_t taggerdate = 0; int generation = 0, distance = 0; bool from_tag = false; };
  Ctx& c_;
  NameRevOptions o_;
  std::map<std::string, Rev> names_;
  std::map<std::string, std::string> tips_;
  int64_t cutoff_ = 0;
  bool create_or_update(const std::string& commit, const std::string& tip, int64_t taggerdate, int gen, int dist, bool from_tag);
};

// The history and read-only commands of the cmd_<name>.cpp files, registered by one line of the command table.
int cmd_blame(Ctx&);
int cmd_check_attr(Ctx&);
int cmd_diff_tree(Ctx&);
int cmd_describe(Ctx&);
int cmd_name_rev(Ctx&);

#define AIOS_GIT_HISTORY_COMMANDS {"blame", cmd_blame}, {"check-attr", cmd_check_attr}, {"diff-tree", cmd_diff_tree}, {"describe", cmd_describe}, {"name-rev", cmd_name_rev},

}  // namespace aios_git
