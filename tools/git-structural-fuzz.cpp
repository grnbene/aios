// The language-aware merge, fuzzed.
//
//   aios-git-structural-fuzz [cases] [seed]     (default 3000 cases; failing cases are written under
//                                                $AIOS_FUZZ_OUT, default /tmp/aios-structural-fuzz)
//   aios-git-structural-fuzz --merge BASE OURS THEIRS   one merge (the language is OURS' extension), or why not
//
// Each case takes a sample file of one language, makes two versions of it with random edits of its syntax units
// (a declaration deleted, cloned under a new name, edited inside, moved past its neighbour; a line copied and
// changed), keeps only versions that parse, and merges them with git_structural_merge. The merge is allowed to give
// up whenever it likes; what it may not do is merge wrongly. So when it merges:
//
//   - the result parses;
//   - nothing in it is made up: every line is a line of the base or of a side;
//   - what each side added is there, and what one side deleted while the other kept it is gone;
//   - the sides swapped, when that merges too, give the same tokens;
//   - merging a side with the base, or with itself, gives that side.
//
// And the trees under it are the ones a fresh parse would give: every incremental reparse (a side from the base's
// tree) prints the same tree as a parse from nothing. Every check counts; the exit code is nonzero if any failed.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "aios/userland/git_merge.hpp"
#include "aios/userland/git_structural.hpp"

using namespace aios::userland;
namespace fs = std::filesystem;

namespace {

struct Sample { const char* path; const char* text; };

const Sample kSamples[] = {
    {"a.js", R"(import fs from 'fs';
import { join } from 'path';

const LIMIT = 10;

export function load(name) {
  const p = join('/data', name);
  return fs.readFileSync(p, 'utf8');
}

export function save(name, body) {
  fs.writeFileSync(join('/data', name), body);
}

class Store {
  constructor(root) {
    this.root = root;
  }

  get(key) {
    return this.items[key];
  }

  put(key, value) {
    this.items[key] = value;
  }
}

const config = {
  name: 'app',
  debug: false,
  retries: 3,
};

module.exports = { load, save, Store };
)"},
    {"a.ts", R"(import { Injectable } from '@angular/core';
import type { User } from './user';

export interface Options {
  verbose: boolean;
  depth: number;
}

export enum Mode {
  Fast,
  Safe,
}

@Injectable()
export class UserService {
  private cache = new Map<string, User>();

  find(id: string): User | undefined {
    return this.cache.get(id);
  }

  add(user: User): void {
    this.cache.set(user.id, user);
  }
}

export function total(xs: number[]): number {
  return xs.reduce((a, b) => a + b, 0);
}
)"},
    {"a.py", R"(import os
import sys
from typing import List, Optional

LIMIT = 10


def load(name: str) -> str:
    with open(os.path.join("/data", name)) as f:
        return f.read()


def save(name: str, body: str) -> None:
    with open(os.path.join("/data", name), "w") as f:
        f.write(body)


class Store:
    def __init__(self, root):
        self.root = root
        self.items = {}

    def get(self, key):
        return self.items.get(key)

    def put(self, key, value):
        self.items[key] = value


CONFIG = {
    "name": "app",
    "debug": False,
    "retries": 3,
}
)"},
    {"a.go", R"(package store

import (
	"fmt"
	"os"
)

const Limit = 10

type Store struct {
	Root  string
	Items map[string]string
}

func New(root string) *Store {
	return &Store{Root: root, Items: map[string]string{}}
}

func (s *Store) Get(key string) string {
	return s.Items[key]
}

func (s *Store) Put(key, value string) {
	s.Items[key] = value
}

func Load(name string) ([]byte, error) {
	return os.ReadFile(fmt.Sprintf("/data/%s", name))
}
)"},
    {"a.rs", R"(use std::collections::HashMap;
use std::fs;

pub const LIMIT: usize = 10;

pub struct Store {
    root: String,
    items: HashMap<String, String>,
}

impl Store {
    pub fn new(root: &str) -> Self {
        Store { root: root.to_string(), items: HashMap::new() }
    }

    pub fn get(&self, key: &str) -> Option<&String> {
        self.items.get(key)
    }

    pub fn put(&mut self, key: &str, value: &str) {
        self.items.insert(key.to_string(), value.to_string());
    }
}

pub fn load(name: &str) -> std::io::Result<String> {
    fs::read_to_string(format!("/data/{}", name))
}
)"},
    {"a.c", R"(#include <stdio.h>
#include <stdlib.h>

#define LIMIT 10

struct store {
  int count;
  char *root;
};

static int helper(int x) {
  return x + 1;
}

int store_get(struct store *s, int key) {
  return s->count + key;
}

void store_put(struct store *s, int key) {
  s->count = key;
}

int main(void) {
  printf("%d\n", helper(LIMIT));
  return 0;
}
)"},
    {"A.java", R"(package app;

import java.util.HashMap;
import java.util.Map;

public class Store {
    private final Map<String, String> items = new HashMap<>();
    private int limit = 10;

    public Store() {
    }

    public String get(String key) {
        return items.get(key);
    }

    public void put(String key, String value) {
        items.put(key, value);
    }

    public int size() {
        return items.size();
    }
}
)"},
    {"Store.cs", R"(using System;
using System.Collections.Generic;
using System.Linq;

namespace App.Data;

public enum Mode { Fast, Safe, Verbose }

public interface IStore
{
    string Get(string key);
    void Put(string key, string value);
}

public record Entry(string Key, string Value);

public class Store : IStore
{
    private readonly Dictionary<string, string> items = new();
    private int limit = 10, count;

    public event Action<string>? Changed;

    public string Name { get; set; } = "store";

    public int Limit
    {
        get => limit;
        set { limit = value; }
    }

    public Store()
    {
    }

    public Store(int limit)
    {
        this.limit = limit;
    }

    public string Get(string key)
    {
        return items[key];
    }

    public string Get(string key, string fallback)
    {
        return items.TryGetValue(key, out var v) ? v : fallback;
    }

    public void Put(string key, string value)
    {
        items[key] = value;
        Changed?.Invoke(key);
    }

    public int Size()
    {
        return items.Count;
    }

    private static class Helper
    {
        public static string Clean(string s) => s.Trim();
    }
}
)"},
    {"View.axaml", R"(<UserControl xmlns="https://github.com/avaloniaui" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Grid RowDefinitions="Auto,*,Auto">
    <StackPanel Grid.Row="0" Orientation="Horizontal" Spacing="8">
      <TextBlock Text="Tasks" Classes="section"/>
      <Button Content="New" Command="{Binding NewCommand}"/>
      <Button Content="Stop" Command="{Binding StopCommand}" IsVisible="{Binding IsRunning}"/>
    </StackPanel>
    <ListBox Grid.Row="1" x:Name="TaskList" ItemsSource="{Binding Tasks}">
      <ListBox.ItemTemplate>
        <DataTemplate>
          <StackPanel>
            <TextBlock Text="{Binding Title}"/>
            <TextBlock Text="{Binding Status}" Classes="caption"/>
          </StackPanel>
        </DataTemplate>
      </ListBox.ItemTemplate>
    </ListBox>
    <Border Grid.Row="2" x:Name="Footer" Padding="8">
      <TextBox x:Name="ReplyBox" Text="{Binding Reply}" Watermark="Next step"/>
    </Border>
  </Grid>
</UserControl>
)"},
    {"a.json", R"({
  "name": "app",
  "version": "1.0.0",
  "scripts": {
    "build": "tsc",
    "test": "node test.js"
  },
  "dependencies": {
    "express": "^4.18.0",
    "lodash": "^4.17.21"
  },
  "files": ["dist", "README.md"]
}
)"},
    {"a.php", R"(<?php
namespace App\Services;

use App\Models\User;
use Illuminate\Support\Str;

const LIMIT = 10;

function helper($x) {
    return $x + 1;
}

class UserService {
    private $repo;
    const KIND = 'user';

    public function find($id) {
        return $this->repo->find($id);
    }

    public function all() {
        return [];
    }
}

$config = [
    'name' => 'app',
    'debug' => false,
];
)"},
    {"a.sql", R"(-- schema
CREATE TABLE users (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  email TEXT
);

CREATE TABLE orders (
  id INTEGER PRIMARY KEY,
  user_id INTEGER,
  total INTEGER
);

CREATE INDEX users_email ON users (email);

INSERT INTO users (id, name) VALUES (1, 'root');
INSERT INTO users (id, name) VALUES (2, 'ops');
)"},
    {"a.yml", R"(name: ci
on:
  push:
    branches: [main]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: make
      - run: make test
  lint:
    runs-on: ubuntu-latest
    steps:
      - run: make lint
deps:
  - alpha
  - beta
)"},
    {"a.toml", R"([package]
name = "x"
version = "0.1.0"
edition = "2021"

[dependencies]
serde = "1"
tokio = { version = "1", features = ["full"] }
regex = "1.10"

[dev-dependencies]
criterion = "0.5"

[features]
default = ["std"]
std = []
)"},
};

std::mt19937_64 rng;
size_t pick(size_t n) { return n ? (size_t)(rng() % n) : 0; }

std::string ext_of(const std::string& path) { return path.substr(path.find_last_of('.') + 1); }

std::vector<std::string> lines_of(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
std::string join_lines(const std::vector<std::string>& v) {
  std::string s;
  for (auto& l : v) s += l + "\n";
  return s;
}
// A line as the checks compare it: without indentation and without the separator after it (a merge moves commas).
std::string norm(std::string l) {
  while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.pop_back();
  while (!l.empty() && (l.back() == ',' || l.back() == ';')) l.pop_back();
  size_t a = l.find_first_not_of(" \t");
  return a == std::string::npos ? "" : l.substr(a);
}
std::map<std::string, int> counts(const std::string& s) {
  std::map<std::string, int> m;
  for (auto& l : lines_of(s)) if (!norm(l).empty()) ++m[norm(l)];
  return m;
}

std::map<std::string, int> tokens(const std::string& s) {
  std::map<std::string, int> m;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    if (std::isspace(c)) { ++i; continue; }
    size_t j = i + 1;
    if (std::isalnum(c) || c == '_' || c == '$')
      while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
    ++m[s.substr(i, j - i)];
    i = j;
  }
  return m;
}

int fresh = 0;

// One random edit; empty when the edit came out the same or there was nothing to edit.
std::string mutate(const std::string& path, const std::string& text) {
  auto outline = git_structural_outline(path, text);
  auto lines = lines_of(text);
  int op = (int)pick(6);
  if (outline && !outline->units.empty() && op < 4) {
    const auto& u = outline->units[pick(outline->units.size())];
    size_t l0 = u.line - 1, l1 = u.end_line;   // [l0, l1) lines
    if (l1 > lines.size()) return "";
    std::vector<std::string> unit(lines.begin() + (long)l0, lines.begin() + (long)l1);
    switch (op) {
      case 0: {   // delete it
        lines.erase(lines.begin() + (long)l0, lines.begin() + (long)l1);
        return join_lines(lines);
      }
      case 1: {   // a copy under a new name, after it
        std::string name = u.name;
        if (name.empty() || name.find_first_of(" \n\t#(@") != std::string::npos || name.size() > 40) return "";
        std::string nn = name + "Z" + std::to_string(++fresh);
        if (!name.empty() && (name[0] == '$')) nn = name + "z" + std::to_string(fresh);
        bool renamed = false;
        for (auto& l : unit) {
          size_t at = l.find(name);
          if (at != std::string::npos && !renamed) { l.replace(at, name.size(), nn); renamed = true; }
        }
        if (!renamed) return "";
        std::vector<std::string> ins = unit;
        if (pick(2)) ins.insert(ins.begin(), "");
        lines.insert(lines.begin() + (long)l1, ins.begin(), ins.end());
        return join_lines(lines);
      }
      case 2: {   // an edit inside it: a letter into a word on one of its lines
        size_t li = l0 + pick(l1 - l0);
        std::string& l = lines[li];
        std::vector<size_t> spots;
        for (size_t i = 1; i < l.size(); ++i)
          if (std::isalpha((unsigned char)l[i]) && std::isalpha((unsigned char)l[i - 1])) spots.push_back(i);
        if (spots.empty()) return "";
        l.insert(spots[pick(spots.size())], 1, (char)('a' + pick(26)));
        return join_lines(lines);
      }
      case 3: {   // moved after the next unit of its level
        for (size_t k = 0; k < outline->units.size(); ++k) {
          const auto& v = outline->units[k];
          if (v.depth != u.depth || v.line <= u.end_line) continue;
          if (u.scope.rfind('.') != v.scope.rfind('.') ||
              u.scope.substr(0, u.scope.rfind('.') + 1) != v.scope.substr(0, v.scope.rfind('.') + 1))
            break;
          size_t m0 = v.line - 1, m1 = v.end_line;
          if (m1 > lines.size()) return "";
          std::vector<std::string> out(lines.begin(), lines.begin() + (long)l0);
          out.insert(out.end(), lines.begin() + (long)m0, lines.begin() + (long)m1);
          out.insert(out.end(), lines.begin() + (long)l1, lines.begin() + (long)m0);
          out.insert(out.end(), unit.begin(), unit.end());
          out.insert(out.end(), lines.begin() + (long)m1, lines.end());
          return join_lines(out);
        }
        return "";
      }
    }
  }
  // a line copied after itself and changed (a new list item, a new statement, a new key)
  if (lines.empty()) return "";
  size_t li = pick(lines.size());
  std::string l = lines[li];
  if (norm(l).empty()) return "";
  std::vector<size_t> spots;
  for (size_t i = 1; i < l.size(); ++i)
    if (std::isalnum((unsigned char)l[i]) && std::isalnum((unsigned char)l[i - 1])) spots.push_back(i);
  if (spots.empty()) return "";
  l.insert(spots[pick(spots.size())], std::to_string(++fresh));
  if (op == 5) lines[li] = l; else lines.insert(lines.begin() + (long)li + 1, l);
  return join_lines(lines);
}

std::string version(const std::string& path, const std::string& base) {
  for (int tries = 0; tries < 40; ++tries) {
    std::string t = base;
    int edits = 1 + (int)pick(3);
    bool ok = true;
    for (int e = 0; e < edits && ok; ++e) {
      std::string next = mutate(path, t);
      if (next.empty() || next == t) { ok = false; break; }
      t = next;
    }
    if (ok && t != base && git_structural_parses(path, t)) return t;
  }
  return "";
}

int passed = 0, failed = 0, saved = 0, one_way = 0;
std::string out_dir;

void save_case(const std::string& why, const std::string& path, const std::string& b, const std::string& o,
               const std::string& t, const std::string* r) {
  if (saved >= 25) return;
  fs::path d = fs::path(out_dir) / std::to_string(++saved);
  fs::create_directories(d);
  std::string e = ext_of(path);
  std::ofstream(d / ("base." + e)) << b;
  std::ofstream(d / ("ours." + e)) << o;
  std::ofstream(d / ("theirs." + e)) << t;
  if (r) std::ofstream(d / ("merged." + e)) << *r;
  std::ofstream(d / "why.txt") << why << "\n";
}

bool check(bool ok, const std::string& why, const std::string& path, const std::string& b, const std::string& o,
           const std::string& t, const std::string* r = nullptr) {
  if (ok) { ++passed; return true; }
  ++failed;
  if (failed <= 40) std::cout << "[FAIL] " << path << ": " << why << "\n";
  save_case(why, path, b, o, t, r);
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string(argv[1]) == "--merge") {   // one merge of three files, and why it gave up if it did
    auto read = [](const char* f) { std::ifstream in(f, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
    auto r = git_structural_merge(argv[3], read(argv[2]), read(argv[3]), read(argv[4]));
    if (r) { std::cout << r->merged; return 0; }
    std::cerr << "gave up: " << git_structural_last_reason() << "\n";
    return 1;
  }
  if (argc == 3 && std::string(argv[1]) == "--sexp") {   // a file's syntax tree, as the merge reads it
    std::ifstream in(argv[2], std::ios::binary); std::stringstream ss; ss << in.rdbuf();
    std::cout << git_structural_sexp(argv[2], ss.str()) << "\n";
    return 0;
  }
  int cases = argc > 1 ? std::atoi(argv[1]) : 3000;
  uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : std::random_device{}();
  rng.seed(seed);
  const char* od = std::getenv("AIOS_FUZZ_OUT");
  out_dir = ((od ? fs::path(od) : fs::temp_directory_path() / "aios-structural-fuzz") / ("seed-" + std::to_string(seed))).string();
  std::error_code ec;
  fs::remove_all(out_dir, ec);   // this seed's failures from an earlier run
  std::cout << "seed " << seed << ", " << cases << " cases, failures under " << out_dir << "\n";

  std::map<std::string, std::map<std::string, int>> tally;   // language -> outcome -> count
  for (int c = 0; c < cases; ++c) {
    const Sample& s = kSamples[pick(std::size(kSamples))];
    std::string path = s.path, b = s.text;
    auto& tl = tally[ext_of(path)];
    if (!check(git_structural_parses(path, b), "the sample does not parse", path, b, b, b)) continue;
    std::string o = version(path, b), t = version(path, b);
    if (o.empty() || t.empty()) { ++tl["no edit"]; continue; }

    // incremental reparses give the fresh parse's tree
    check(git_structural_sexp(path, o) == git_structural_sexp(path, o, b), "incremental parse of ours differs", path, b, o, t);
    check(git_structural_sexp(path, t, o) == git_structural_sexp(path, t), "incremental parse of theirs from ours differs",
          path, b, o, t);

    // identities
    for (auto [x, y, want, name] : {std::tuple{b, o, o, "merge(b, b, o)"}, std::tuple{o, b, o, "merge(b, o, b)"},
                                    std::tuple{o, o, o, "merge(b, o, o)"}}) {
      auto r = git_structural_merge(path, b, x, y);
      if (r) check(r->merged == want, std::string(name) + " is not ours", path, b, x, y, &r->merged);
    }

    auto line = git_merge3(b, o, t);
    auto r = git_structural_merge(path, b, o, t);
    std::string why_r = git_structural_last_reason();
    auto rs = git_structural_merge(path, b, t, o);
    std::string why_rs = git_structural_last_reason();
    ++tl[r ? (line.clean ? "merged (lines clean too)" : "merged (lines conflict)") : (line.clean ? "gave up (lines clean)" : "gave up (lines conflict)")];
    // merging one way and giving up the other is allowed (the merge keeps one side's order and its separators, and
    // gives up when they do not fit), but counted: it is where the merge could do better
    if (r.has_value() != rs.has_value()) {
      ++one_way;
      if (std::getenv("AIOS_FUZZ_ONE_WAY"))
        save_case(std::string("merges one way only (") + (r ? "swapped" : "as is") + " gave up: " + (r ? why_rs : why_r) + ")",
                  path, b, o, t, r ? &r->merged : nullptr);
    }
    if (!r) continue;
    const std::string& m = r->merged;
    check(git_structural_parses(path, m), "the result does not parse", path, b, o, t, &m);

    // tokens (words, and each punctuation mark): a merge of independent edits applies both, so a token whose count
    // only one side changed has that side's count in the result
    auto wb = tokens(b), wo = tokens(o), wt = tokens(t), wm = tokens(m);
    auto n = [](std::map<std::string, int>& tc, const std::string& k) { auto it = tc.find(k); return it == tc.end() ? 0 : it->second; };
    for (auto& [k, cnt] : wm)
      if (!wb.count(k) && !wo.count(k) && !wt.count(k)) { check(false, "made-up token: " + k, path, b, o, t, &m); break; }
    std::set<std::string> all;
    for (auto* w : {&wb, &wo, &wt, &wm}) for (auto& [k, cnt] : *w) all.insert(k);
    for (auto& k : all) {
      int nb = n(wb, k), no = n(wo, k), nt = n(wt, k), nm = n(wm, k);
      if (no != nb && nt == nb && nm != no) {
        check(false, "ours changed the count of '" + k + "' to " + std::to_string(no) + ", the result has " + std::to_string(nm),
              path, b, o, t, &m);
        break;
      }
      if (nt != nb && no == nb && nm != nt) {
        check(false, "theirs changed the count of '" + k + "' to " + std::to_string(nt) + ", the result has " + std::to_string(nm),
              path, b, o, t, &m);
        break;
      }
    }
    if (rs) check(tokens(rs->merged) == wm, "swapped sides merge to other tokens", path, b, o, t, &m);
    // a line one side deleted and the other kept as it was is gone
    auto cb = counts(b), co = counts(o), ct = counts(t), cm = counts(m);
    auto once = [](std::map<std::string, int>& lc, const std::string& l) { return lc.count(l) && lc[l] == 1; };
    for (auto [side, other, name] : {std::tuple{&co, &ct, "ours"}, std::tuple{&ct, &co, "theirs"}}) {
      for (auto& [l, cnt] : cb) {
        if (cnt != 1 || side->count(l) || !once(*other, l)) continue;
        if (cm.count(l)) { check(false, std::string(name) + "' deleted line is still there: " + l, path, b, o, t, &m); break; }
      }
    }
  }
  for (auto& [lang, outcomes] : tally) {
    std::cout << "  " << lang << ":";
    for (auto& [k, n] : outcomes) std::cout << " " << k << " " << n << ";";
    std::cout << "\n";
  }
  auto st = git_structural_stats();
  std::cout << "trees: " << st.full << " parsed whole, " << st.incremental << " reparsed incrementally, " << st.cached
            << " from the cache\n";
  std::cout << one_way << " cases merged one way and gave up the other (AIOS_FUZZ_ONE_WAY=1 keeps them)\n";
  std::cout << passed << " checks passed, " << failed << " failed\n";
  return failed ? 1 : 0;
}
