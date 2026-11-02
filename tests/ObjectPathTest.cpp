/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Host-side tests for the walk of object names (src/Process/ObjectPath.*): flat names, directories, links, the syntax errors, truncated and bit-flipped names.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include "Process/ObjectPath.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using namespace process::objpath;

struct Fake : ILookup {
  std::map<std::string, Entry> entries;
  void add_dir(const std::string& flat, const std::string& path) { Entry e; e.kind = EntryKind::Directory; e.text_size = uint32_t(path.size()); memcpy(e.text, path.data(), path.size()); entries[flat] = e; }
  void add_link(const std::string& flat, const std::string& target) { Entry e; e.kind = EntryKind::Link; e.text_size = uint32_t(target.size()); memcpy(e.text, target.data(), target.size()); entries[flat] = e; }
  void add_object(const std::string& flat) { Entry e; e.kind = EntryKind::Object; e.object_kind = 1; entries[flat] = e; }
  bool find(StringView flat, bool case_insensitive, Entry* out) override {
    auto it = entries.find(std::string(flat.data(), flat.size()));
    if (it == entries.end() && case_insensitive) {
      for (it = entries.begin(); it != entries.end() && !StringView(it->first.data(), it->first.size()).equals_ignore_case(flat); ++it) {}
    }
    if (it == entries.end()) return false;
    *out = it->second;
    out->flat_size = uint32_t(it->first.size());
    memcpy(out->flat, it->first.data(), it->first.size());
    return true;
  }
};

static Resolved walk(Fake& f, const char* name, const char* root = nullptr, bool ci = false, bool open_link = false) {
  ResolveRequest rq;
  rq.has_root = root != nullptr;
  if (root) rq.root_path = root;
  rq.name_given = name != nullptr;
  if (name) rq.name = name;
  rq.case_insensitive = ci;
  rq.open_link = open_link;
  Resolved r;
  resolve(f, rq, &r);
  return r;
}
static bool same(const Resolved& r, const char* flat) { return StringView(r.flat, r.flat_size) == StringView(flat); }

static void test_flat_names() {
  Path p;
  char buf[kMaxPath];
  uint32_t n = 0;
  CHECK(p.parse("\\BaseNamedObjects\\a\\b"));
  CHECK(p.to_flat(buf, sizeof(buf), &n) && StringView(buf, n) == "a\\b");
  CHECK(p.parse("\\basenamedobjects\\a") && p.to_flat(buf, sizeof(buf), &n) && StringView(buf, n) == "a");
  CHECK(p.parse("\\Sessions\\1\\BaseNamedObjects\\x") && p.to_flat(buf, sizeof(buf), &n) && StringView(buf, n) == "\x01x");
  CHECK(p.parse("\\??\\PIPE\\x") && p.to_flat(buf, sizeof(buf), &n) && StringView(buf, n) == "\\??\\PIPE\\x");
  CHECK(p.parse("\\") && p.to_flat(buf, sizeof(buf), &n) && StringView(buf, n) == "\\");
  CHECK(!p.parse("\\\\a") && !p.parse("\\a\\") && !p.parse("a"));
  CHECK(flat_to_path("x\\y", &p) && p.format(buf, sizeof(buf), &n) && StringView(buf, n) == "\\BaseNamedObjects\\x\\y");
  CHECK(flat_to_path("\x01z", &p) && p.format(buf, sizeof(buf), &n) && StringView(buf, n) == "\\Sessions\\1\\BaseNamedObjects\\z");
  CHECK(flat_to_path("", &p) && p.format(buf, sizeof(buf), &n) && StringView(buf, n) == "\\BaseNamedObjects");
  StringView dir, leaf;
  split_flat("a\\b\\c", &dir, &leaf);
  CHECK(dir == "a\\b" && leaf == "c");
  split_flat("c", &dir, &leaf);
  CHECK(dir.empty() && leaf == "c");
  split_flat("\x01""c", &dir, &leaf);
  CHECK(dir == StringView("\x01", 1) && leaf == "c");
  split_flat("\\??\\x", &dir, &leaf);
  CHECK(dir == "\\??" && leaf == "x");
  split_flat("\\x", &dir, &leaf);
  CHECK(dir == "\\" && leaf == "x");
}

static void test_syntax() {
  Fake f;
  f.add_dir("d", "\\BaseNamedObjects\\d");
  CHECK(walk(f, "BaseNamedObjects").status == kStatusPathSyntaxBad);
  CHECK(walk(f, "").status == kStatusPathSyntaxBad);
  CHECK(walk(f, nullptr).status == kStatusPathSyntaxBad);
  CHECK(walk(f, "\\BaseNamedObjects\\").status == kStatusNameInvalid);
  CHECK(walk(f, "\\\\BaseNamedObjects").status == kStatusNameInvalid);
  CHECK(walk(f, "\\BaseNamedObjects\\\\x").status == kStatusNameInvalid);
  CHECK(walk(f, "\\BaseNamedObjects\\nothing\\").status == kStatusPathNotFound);
  CHECK(walk(f, "\\BaseNamedObjects\\d\\").status == kStatusNameInvalid);
  CHECK(walk(f, "\\BaseNamedObjects\\nothing\\x").status == kStatusPathNotFound);
  // with a root directory
  const char* base = "\\BaseNamedObjects";
  CHECK(walk(f, "\\", base).status == kStatusPathSyntaxBad);
  CHECK(walk(f, "\\x", base).status == kStatusPathSyntaxBad);
  CHECK(walk(f, nullptr, base).status == kStatusNameInvalid);
  CHECK(walk(f, "x\\", base).status == kStatusPathNotFound);
  Resolved r = walk(f, "", base);
  CHECK(r.status == 0 && r.is_root && r.entry.kind == EntryKind::Directory && r.entry.builtin);
}

static void test_walk() {
  Fake f;
  f.add_dir("d", "\\BaseNamedObjects\\d");
  f.add_object("d\\obj");
  f.add_object("top");
  Resolved r = walk(f, "\\BaseNamedObjects\\d\\obj");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "d\\obj"));
  r = walk(f, "\\BaseNamedObjects\\d\\free");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::None && same(r, "d\\free"));
  r = walk(f, "d\\obj", "\\BaseNamedObjects");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "d\\obj"));
  r = walk(f, "obj", "\\BaseNamedObjects\\d");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "d\\obj"));
  r = walk(f, "\\BaseNamedObjects\\top\\x");
  CHECK(r.status == kStatusTypeMismatch);
  r = walk(f, "\\BaseNamedObjects\\d");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Directory && !r.entry.builtin);
  r = walk(f, "\\");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Directory && r.entry.builtin && same(r, "\\"));
  // built-in names: case matters unless asked
  r = walk(f, "\\BASENamedObjects\\top");
  CHECK(r.status == kStatusPathNotFound);
  r = walk(f, "\\BASENamedObjects\\top", nullptr, true);
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "top"));
  // a name found in another case has the spelling it was made with
  r = walk(f, "\\BaseNamedObjects\\TOP", nullptr, true);
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "top"));
  r = walk(f, "\\BaseNamedObjects\\TOP");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::None && same(r, "TOP"));
  // the session directory and \BaseNamedObjects\Session\<n>
  r = walk(f, "\\Sessions\\1\\BaseNamedObjects\\q");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::None && same(r, "\x01q"));
  r = walk(f, "\\BaseNamedObjects\\Session\\1\\q");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::None && same(r, "\x01q"));
  r = walk(f, "\\BaseNamedObjects\\Session\\1");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Directory && same(r, "\x01"));
}

static void test_links() {
  Fake f;
  f.add_object("top");
  f.add_dir("sub", "\\BaseNamedObjects\\sub");
  f.add_link("\\??\\mine", "\\BaseNamedObjects\\sub");
  f.add_link("loop1", "\\BaseNamedObjects\\loop2");
  f.add_link("loop2", "\\BaseNamedObjects\\loop1");
  f.add_link("relative", "somewhere");
  // \DosDevices -> \??, then a link made there
  Resolved r = walk(f, "\\DosDevices\\mine\\thing");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::None && same(r, "sub\\thing"));
  r = walk(f, "\\DosDevices\\mine");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Directory && same(r, "sub"));
  r = walk(f, "\\DosDevices\\mine", nullptr, false, true);      // OBJ_OPENLINK: the link itself
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Link && !r.entry.builtin);
  r = walk(f, "\\DosDevices", nullptr, false, true);
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Link && r.entry.builtin && StringView(r.entry.text, r.entry.text_size) == "\\??");
  r = walk(f, "\\DosDevices");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Directory && StringView(r.path, r.path_size) == "\\??");
  // Local and Global
  r = walk(f, "\\BaseNamedObjects\\Global\\top");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "top"));
  r = walk(f, "\\BaseNamedObjects\\Local\\top");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "top"));
  // loops and bad targets
  CHECK(walk(f, "\\BaseNamedObjects\\loop1\\x").status == kStatusPathNotFound);
  CHECK(walk(f, "\\BaseNamedObjects\\relative\\x").status == kStatusPathNotFound);
  // \??\GLOBALROOT is the root again
  r = walk(f, "\\??\\GLOBALROOT\\BaseNamedObjects\\top");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Object && same(r, "top"));
  r = walk(f, "\\??\\GLOBALROOT\\??\\GLOBALROOT\\DosDevices");
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Directory && StringView(r.path, r.path_size) == "\\??");
  r = walk(f, "\\??\\GLOBALROOT", nullptr, false, true);
  CHECK(r.status == 0 && r.entry.kind == EntryKind::Link && r.entry.text_size == 0);
  // a trailing backslash after a link to a directory
  CHECK(walk(f, "\\DosDevices\\mine\\").status == kStatusNameInvalid);
}

static void test_damaged_names() {
  Fake f;
  f.add_dir("d", "\\BaseNamedObjects\\d");
  f.add_object("d\\o");
  const char* good = "\\BaseNamedObjects\\d\\o";
  const size_t length = strlen(good);
  for (size_t cut = 0; cut <= length; ++cut) {      // every truncation: an answer, never a crash
    std::string s(good, cut);
    Resolved r = walk(f, s.c_str());
    CHECK(r.status == 0 || r.status != 0);
  }
  for (size_t i = 0; i < length; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string s(good);
      s[i] = char(s[i] ^ (1 << bit));
      if (s[i] == 0) continue;
      Resolved r = walk(f, s.c_str(), nullptr, true);
      CHECK(r.flat_size < kMaxPath);
    }
  }
  std::string big(2000, 'a');
  CHECK(walk(f, ("\\BaseNamedObjects\\" + big).c_str()).status == kStatusNameTooLong);
  std::string deep;
  for (int i = 0; i < 100; ++i) deep += "\\a";
  CHECK(walk(f, deep.c_str()).status != 0);
}

int main() {
  test_flat_names();
  test_syntax();
  test_walk();
  test_links();
  test_damaged_names();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("ObjectPathTest passed\n");
  return 0;
}
