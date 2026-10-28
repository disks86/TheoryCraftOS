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

// Host-side tests for the Windows file name patterns (FindFirstFile): the table is the one of Wine's test_FindFirstFile_wildcards, which was recorded on Windows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Process/DosWildcard.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using process::dos_wildcard_matches;
using process::dos_short_name;

struct Row { const char* pattern; std::vector<const char*> names; };

int main() {
  const char* directory[] = {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"};
  const std::vector<Row> rows = {
    {"*.*.*", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"*.*.", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {".*.*", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa"}},
    {"*.*", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {".*", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa"}},
    {". *", {}},
    {"*.", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {"*", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"*..*", {".", "..", "..a", "..a.a", ".a..a", "a..a"}},
    {"*..", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {".*.", {".", "..", ".a", ".aaa"}},
    {"..*", {".", "..", "..a", "..a.a"}},
    {"**", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"**.", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"*. ", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"* .", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {"* . ", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"* . *", {}},
    {"*.. ", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"*. .", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {"* ..", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {" *..", {}},
    {"..* ", {".", "..", "..a", "..a.a"}},
    {"* .*.", {" .a"}},
    {"a*.", {"..a", ".a", ".aaa", "a", "aa", "aaa", "aaaa"}},
    {"*a ", {"..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"*aa*", {".aaa", "a.a.a", "aa", "aaa", "aaaa"}},
    {"aa*.", {".aaa", "aa", "aaa", "aaaa"}},
    {"aa.*", {"aa"}},
    {"a a*.*", {}},
    {"a\"*\"a", {"a..a", "a.a.a"}},
    {"aa*.*", {".aaa", "a.a.a", "aa", "aaa", "aaaa"}},
    {"a ?.*", {}},
    {"? a.*", {}},
    {"a* a", {}},
    {" *a", {" .a"}},
    {"* *", {" .a"}},
    {"a* .", {"a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {" ?a", {}},
    {"* .a", {" .a"}},
    {"< .a", {" .a"}},
    {"** .a", {" .a"}},
    {"<< .a", {" .a"}},
    {"aa? ", {"aa", "aaa"}},
    {"aa\"*", {"aa"}},
    {"*.a", {"..a", "..a.a", ".a", ".a..a", ".a.a", "a..a", "a.a", "a.a.a", " .a"}},
    {"<.a", {"..a", "..a.a", ".a", ".a..a", ".a.a", "a..a", "a.a", "a.a.a", " .a"}},
    {"<.<.<", {"..a", "..a.a", ".a..a", ".a.a", "a..a", "a.a.a"}},
    {"<.<.< ", {"..a", "..a.a", ".a..a", ".a.a", "a..a", "a.a.a"}},
    {"<.<.", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a..a", "a.a", "a.a.a", " .a"}},
    {"< .<.", {" .a"}},
    {"< .<. ", {" .a"}},
    {"<.<. ", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a..a", "a.a", "a.a.a", " .a"}},
    {".<.<", {"..a", "..a.a", ".a..a", ".a.a"}},
    {"<.<", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a..a", "a.a", "a.a.a", " .a"}},
    {".<", {".", "..", ".a", ".aaa"}},
    {"<.", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {"<", {".", "..", "..a", ".a", ".aaa", "a", "aa", "aaa", "aaaa"}},
    {"<..<", {"..a", ".a..a", "a..a"}},
    {"<..", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {".<.", {".", "..", ".a", ".aaa"}},
    {"..<", {"..a"}},
    {"<<", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"<<.", {".", "..", "..a", "..a.a", ".a", ".a..a", ".a.a", ".aaa", "a", "a..a", "a.a", "a.a.a", "aa", "aaa", "aaaa", " .a"}},
    {"<. ", {".", "..", "..a", ".a", ".aaa", "a", "aa", "aaa", "aaaa"}},
    {"< .", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {"< . ", {".", "..", "..a", ".a", ".aaa", "a", "aa", "aaa", "aaaa"}},
    {"<.. ", {".", "..", "..a", ".a", ".aaa", "a", "aa", "aaa", "aaaa"}},
    {"<. .", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {"< ..", {".", "..", "a", ".a", "..a", "aa", "aaa", "aaaa", ".aaa"}},
    {" <..", {}},
    {"..< ", {"..a"}},
    {"?", {".", "..", "a"}},
    {"?.", {".", "..", "a"}},
    {"?. ", {".", "..", "a"}},
    {"? .*", {}},
    {"??.", {".", "..", "a", "aa"}},
    {"??. ", {".", "..", "a", "aa"}},
    {"???.", {".", "..", "a", "aa", "aaa"}},
    {"?.??.", {".", "..", ".a", "a", "a.a", " .a"}},
    {". ?", {}},
    {">", {".", "..", "a"}},
    {">.", {".", "..", "a"}},
    {">. ", {".", "..", "a"}},
    {">>.", {".", "..", "a", "aa"}},
    {">>. ", {".", "..", "a", "aa"}},
    {">>>.", {".", "..", "a", "aa", "aaa"}},
    {">.>>.", {".", "..", ".a", "a.a", " .a"}}
  };
  for (const Row& row : rows) {
    for (const char* entry : directory) {
      bool expected = false;
      for (const char* name : row.names) if (strcmp(name, entry) == 0) expected = true;
      if (dos_wildcard_matches(StringView(row.pattern), StringView(entry)) != expected) {
        printf("FAILED: pattern '%s' on '%s' should %s\n", row.pattern, entry, expected ? "match" : "not match");
        ++g_failed;
      }
    }
  }

  // ordinary patterns
  CHECK(dos_wildcard_matches("*", "anything.txt") && dos_wildcard_matches("*.*", "noext") && dos_wildcard_matches("*.TXT", "readme.txt") && !dos_wildcard_matches("*.txt", "readme.text"));
  CHECK(dos_wildcard_matches("r?adme.txt", "README.TXT") && !dos_wildcard_matches("r?adme.txt", "rdme.txt") && dos_wildcard_matches("exact.name", "Exact.Name"));
  CHECK(!dos_wildcard_matches("", "a") && !dos_wildcard_matches("a", ""));

  // short names
  char alias[16];
  CHECK(dos_short_name("readme.txt", alias, sizeof(alias)) == 0);
  CHECK(dos_short_name("a very long name.text", alias, sizeof(alias)) == 12 && strncmp(alias, "AVERYL~1.TEX", 12) == 0);
  CHECK(dos_short_name("..", alias, sizeof(alias)) == 0 && dos_short_name("", alias, sizeof(alias)) == 0);

  // robustness: long and odd patterns and names do not run past anything
  std::string big(300, '*');
  CHECK(!dos_wildcard_matches(StringView(big.c_str(), big.size()), "a"));
  std::string pattern = "<>\"*?. ";
  srand(7);
  for (int round = 0; round < 3000; ++round) {
    std::string p, n;
    for (int i = rand() % 12; i > 0; --i) p += pattern[rand() % pattern.size()];
    for (int i = rand() % 12; i > 0; --i) n += "a. ~"[rand() % 4];
    dos_wildcard_matches(StringView(p.c_str(), p.size()), StringView(n.c_str(), n.size()));
  }
  std::string maximal(255, 'a');
  CHECK(dos_wildcard_matches("*a", StringView(maximal.c_str(), maximal.size())));
  std::string too_long(256, 'a');
  CHECK(!dos_wildcard_matches("*", StringView(too_long.c_str(), too_long.size())));

  printf("DosWildcardTest: %s\n", g_failed ? "FAILED" : "ok");
  return g_failed ? 1 : 0;
}
