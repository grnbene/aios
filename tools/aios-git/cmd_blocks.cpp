// git blocks (and git repack --blocks): the repository's objects in block storage (see git_blocks.hpp).
//   git blocks build [-d] [--no-outlines]
//                             every object into one new block file (-d: then remove the loose objects, the packs
//                             and older block files it replaces), with every source file's syntax outline
//   git blocks stats          what the block files hold, against the packs and loose objects beside them
//   git blocks verify         every object in every block file read back and its id checked
//   git blocks unpack         back to an ordinary pack git reads (git repack -a -d)
#include <unordered_set>

#include "aios/hal/host.hpp"
#include "aios/userland/git_blocks.hpp"
#include "cli.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

std::string human(uint64_t b) {
  char buf[32];
  if (b >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.2f GiB", b / 1073741824.0);
  else if (b >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.2f MiB", b / 1048576.0);
  else if (b >= 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
  else std::snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long)b);
  return buf;
}

void print_stats(Ctx& c, const GitBlockStats& s) {
  c.out << s.path << "\n  " << s.objects << " objects: " << s.commits << " commits, " << s.trees << " trees, " << s.blobs
        << " blobs, " << s.tags << " tags\n  " << s.chunks << " distinct chunks for " << s.chunk_refs << " chunk references, "
        << s.names << " distinct names, " << s.frames << " frames\n  " << s.outlines << " source files outlined\n  "
        << human(s.file_bytes) << " on disk";
  if (s.raw_bytes) c.out << " for " << human(s.raw_bytes) << " of object content";
  c.out << "\n";
}

}  // namespace

int build_blocks(Ctx& c, bool remove_old, bool quiet, bool outlines) {
  auto& h = c.host();
  std::vector<std::string> loose = c.repo->local_loose_ids();
  std::vector<std::string> packs = c.repo->local_packs();
  std::vector<std::string> old_blocks = c.repo->local_block_files();
  // everything this repository holds (unreachable objects are kept, as a repack keeps them)
  std::vector<std::string> ids;
  std::unordered_set<std::string> seen;
  for (auto& o : loose) if (seen.insert(o).second) ids.push_back(o);
  for (auto& o : c.repo->local_packed_ids()) if (seen.insert(o).second) ids.push_back(o);
  for (auto& o : c.repo->local_block_ids()) if (seen.insert(o).second) ids.push_back(o);
  if (ids.empty()) { if (!quiet) c.out << "Nothing to store.\n"; return 0; }
  auto st = git_build_blocks(*c.repo, ids, outlines);
  if (!st.ok()) die(c, "block build failed: " + st.error().message);
  if (!quiet) print_stats(c, *st);
  if (!remove_old) return 0;
  // the new file is on disk and every object in it has been read back: only now does anything go
  uint64_t removed = 0;
  for (auto& b : old_blocks) if (b != st->path && h.remove(b).ok()) ++removed;
  for (auto& p : packs) {
    if (h.exists(p + ".keep")) continue;
    for (const char* ext : {".idx", ".pack", ".rev", ".bitmap", ".mtimes", ".promisor"}) if (h.exists(p + ext)) (void)h.remove(p + ext);
    ++removed;
  }
  const std::string objects = c.repo->common_dir() + "/objects/";
  for (auto& id : loose) if (h.remove(objects + id.substr(0, 2) + "/" + id.substr(2)).ok()) ++removed;
  for (auto& id : loose) (void)h.remove(objects + id.substr(0, 2));
  c.repo->rescan_packs();
  if (!quiet) c.out << "  replaced " << removed << " loose objects, packs and older block files\n";
  return 0;
}

int cmd_blocks(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "stats";
  c.open_repo(false);
  bool d = false, outlines = true;
  for (size_t i = 2; i < c.args.size(); ++i) {
    if (c.args[i] == "-d") d = true;
    if (c.args[i] == "--no-outlines") outlines = false;
  }
  if (sub == "build") return build_blocks(c, d, false, outlines);
  if (sub == "stats" || sub == "verify") {
    auto files = c.repo->local_block_files();
    if (files.empty()) c.out << "No block files here ('git blocks build -d' makes one).\n";
    for (auto& f : files) {
      auto st = git_verify_blocks(*c.repo, f, sub == "verify");
      if (!st.ok()) die(c, st.error().message);
      print_stats(c, *st);
      if (sub == "verify") c.out << "  every object reads back to its own id\n";
    }
    uint64_t pack_bytes = 0;
    for (auto& p : c.repo->local_packs()) if (auto s = c.host().stat(p + ".pack"); s.ok()) pack_bytes += s->size;
    c.out << "beside them: " << c.repo->local_loose_ids().size() << " loose objects, " << c.repo->local_packs().size()
          << " packs (" << human(pack_bytes) << ")\n";
    return 0;
  }
  if (sub == "unpack") {
    auto r = git_repack(*c.repo, true, true);
    if (!r.ok()) die(c, "repack failed: " + r.error().message);
    c.out << "Back in an ordinary pack: " << r->objects << " objects in " << r->pack << ".pack\n";
    return 0;
  }
  usage(c, "usage: git blocks (build [-d] [--no-outlines] | stats | verify | unpack)");
}

}  // namespace aios_git
