#include "tools.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <vector>
#include <string>
#include <algorithm>

namespace fs = std::filesystem;
namespace g = aios::userland::grain;

namespace grain_cli {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool write_file(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << content;
  return true;
}


// Every file the entry point reaches through `import`, with the imports that name nothing.
// An import path is read relative to the file that imports it.
struct ImportWalk {
  std::vector<std::string> files;                       // entry first, each once
  std::vector<std::string> missing;                     // "main.gr:2: lib/missing.gr"
  std::vector<std::pair<std::string, std::string>> edges;   // (importing file, resolved path)
};

ImportWalk walk_imports(const std::string& entry) {
  static const std::regex import_re("\\bimport\\b[^\"\\n;]*\"([^\"\\n]+)\"");
  ImportWalk w;
  std::vector<std::string> todo{fs::path(entry).lexically_normal().generic_string()};
  while (!todo.empty()) {
    const std::string f = todo.back();
    todo.pop_back();
    if (std::find(w.files.begin(), w.files.end(), f) != w.files.end()) continue;
    w.files.push_back(f);
    std::string src;
    if (!read_file(f, src)) continue;
    std::istringstream lines(src);
    std::string text;
    for (int n = 1; std::getline(lines, text); ++n) {
      const size_t first = text.find_first_not_of(" \t");
      if (first == std::string::npos || text[first] == '#') continue;
      std::smatch m;
      if (!std::regex_search(text, m, import_re)) continue;
      const std::string imp = m[1].str();
      const std::string p = (fs::path(f).parent_path() / imp).lexically_normal().generic_string();
      if (fs::is_regular_file(p)) { w.edges.push_back({f, p}); todo.push_back(p); }
      else w.missing.push_back(f + ":" + std::to_string(n) + ": " + imp);
    }
  }
  return w;
}

std::string manifest_main() {
  std::string content, main_file = "main.gr";
  if (read_file("grain.pkg", content)) {
    static const std::regex main_re("\"main\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch m;
    if (std::regex_search(content, m, main_re)) main_file = m[1].str();
  }
  return main_file;
}

void print_pkg_usage(std::ostream& os) {
  os << "grain pkg: package and dependency management\n"
     << "\n"
     << "usage:\n"
     << "  grain pkg init [name]             initialize a new grain.pkg manifest\n"
     << "  grain pkg add <path> [name]       add a dependency and vendor it into vendor/\n"
     << "  grain pkg list                    list package manifest information and dependencies\n"
     << "  grain pkg vendor                  vendor all local imported modules into vendor/\n"
     << "  grain pkg check                   verify package manifest and compile entry point\n";
}

} // namespace

int pkg_command(const std::vector<std::string>& args) {
  if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
    print_pkg_usage(std::cout);
    return kExitOk;
  }

  const std::string& sub = args[0];

  if (sub == "init") {
    if (fs::exists("grain.pkg")) {
      std::cerr << "grain pkg: grain.pkg already exists in current directory\n";
      return kExitFailed;
    }
    std::string name = args.size() > 1 ? args[1] : fs::current_path().filename().string();
    if (name.empty()) name = "app";
    std::string manifest = "{\n"
                           "  \"name\": \"" + name + "\",\n"
                           "  \"version\": \"0.1.0\",\n"
                           "  \"main\": \"main.gr\",\n"
                           "  \"dependencies\": {}\n"
                           "}\n";
    if (!write_file("grain.pkg", manifest)) {
      std::cerr << "grain pkg: failed to write grain.pkg\n";
      return kExitFailed;
    }
    std::cout << "initialized package '" << name << "' in grain.pkg\n";
    return kExitOk;
  }

  if (sub == "list") {
    std::string content;
    if (!read_file("grain.pkg", content)) {
      std::cerr << "grain pkg: cannot find grain.pkg in current directory (run 'grain pkg init')\n";
      return kExitFailed;
    }
    std::cout << "Package Manifest (grain.pkg):\n" << content << "\n";
    return kExitOk;
  }

  if (sub == "add") {
    if (args.size() < 2) {
      std::cerr << "grain pkg add <path-to-file.gr> [name]\n";
      return kExitUsage;
    }
    const std::string& dep_path = args[1];
    if (!fs::exists(dep_path)) {
      std::cerr << "grain pkg: file not found: " << dep_path << "\n";
      return kExitFailed;
    }
    std::string dep_name = args.size() > 2 ? args[2] : fs::path(dep_path).filename().string();
    if (dep_name.size() > 3 && dep_name.substr(dep_name.size() - 3) == ".gr") {
      dep_name = dep_name.substr(0, dep_name.size() - 3);
    }
    fs::create_directories("vendor");
    std::string target_file = "vendor/" + dep_name + ".gr";
    fs::copy_file(dep_path, target_file, fs::copy_options::overwrite_existing);

    // Update grain.pkg if present
    std::string pkg_str;
    if (read_file("grain.pkg", pkg_str)) {
      // Basic insert into dependencies if not already there
      std::string dep_entry = "\"" + dep_name + "\": \"" + target_file + "\"";
      if (pkg_str.find(dep_entry) == std::string::npos) {
        size_t deps_pos = pkg_str.find("\"dependencies\": {");
        if (deps_pos != std::string::npos) {
          size_t brace = pkg_str.find("}", deps_pos);
          if (brace != std::string::npos) {
            std::string before = pkg_str.substr(0, brace);
            std::string after = pkg_str.substr(brace);
            std::string comma = (before.find('"', deps_pos + 17) != std::string::npos) ? ",\n    " : "\n    ";
            pkg_str = before + comma + dep_entry + "\n  " + after;
            write_file("grain.pkg", pkg_str);
          }
        }
      }
    }
    std::cout << "added dependency '" << dep_name << "' -> '" << target_file << "'\n";
    return kExitOk;
  }

  if (sub == "vendor") {
    // The imports that live outside this folder are what keep a project from standing alone. Copy those
    // in; the ones already inside it travel with the project as they are.
    const std::string main_file = manifest_main();
    if (!fs::exists(main_file)) { std::cerr << "grain pkg vendor: main entry point '" << main_file << "' does not exist\n"; return kExitFailed; }
    const ImportWalk w = walk_imports(main_file);
    const fs::path root = fs::current_path();
    size_t copied = 0;
    for (const auto& [from, to] : w.edges) {
      const fs::path abs = fs::weakly_canonical(to);
      const std::string rel = abs.lexically_relative(root).generic_string();
      if (rel.rfind("..", 0) != 0 || rel.rfind("vendor/", 0) == 0) continue;     // inside the project already
      const std::string dest = "vendor/" + abs.filename().string();
      std::string a, b;
      if (fs::exists(dest) && read_file(dest, a) && read_file(abs.string(), b) && a != b) {
        std::cerr << "grain pkg vendor: " << dest << " is already a different file; rename one of them (`grain pkg add <path> <name>`)\n";
        return kExitFailed;
      }
      fs::create_directories("vendor");
      fs::copy_file(abs, dest, fs::copy_options::overwrite_existing);
      std::cout << "vendored " << to << " -> " << dest << "   (in " << from << ", import \"" << fs::path(dest).lexically_relative(fs::path(from).parent_path().empty() ? fs::path(".") : fs::path(from).parent_path()).generic_string() << "\")\n";
      ++copied;
    }
    std::cout << (copied ? "vendoring complete: " + std::to_string(copied) + " module(s) copied into vendor/; point the imports above at them\n"
                         : std::string("nothing to vendor: every import is already inside this folder\n"));
    return kExitOk;
  }

  if (sub == "check") {
    std::string content;
    if (!read_file("grain.pkg", content)) {
      std::cerr << "grain pkg check: grain.pkg not found\n";
      return kExitFailed;
    }
    const std::string main_file = manifest_main();
    if (!fs::exists(main_file)) {
      std::cerr << "grain pkg check: main entry point '" << main_file << "' does not exist\n";
      return kExitFailed;
    }
    const ImportWalk w = walk_imports(main_file);
    int bad = 0;
    for (const auto& f : w.files) {
      std::string src;
      if (!read_file(f, src)) continue;
      auto comp = g::compile_source(src, f);
      if (comp.ok()) continue;
      ++bad;
      for (const auto& d : comp.errors) std::cerr << "  " << f << ":" << d.line << ": " << d.message << "\n";
    }
    for (const auto& m : w.missing) { ++bad; std::cerr << "  " << m << ": no such file\n"; }
    if (bad) { std::cerr << "grain pkg check: failed\n"; return kExitFailed; }
    std::cout << "package check passed: " << w.files.size() << " file(s) from " << main_file << " compile and every import resolves\n";
    return kExitOk;
  }

  std::cerr << "grain pkg: unknown subcommand '" << sub << "'\n";
  print_pkg_usage(std::cerr);
  return kExitUsage;
}

} // namespace grain_cli
