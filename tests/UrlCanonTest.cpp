// TheoryCraftOS - a minimal UEFI x86_64 operating system.
// Copyright (C) 2026 Christopher Schaefer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// Host test of the UrlCanonicalize core of shlwapi.dll (src/Windows/shlwapi/urlcanon.inc, a freestanding C file for Windows): rows taken from Wine's tests
// (tests/windows/wine/shlwapi/url.c, which the OS runs in full under QEMU) and a truncation / single-character mutation sweep for the sanitizers.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

typedef unsigned short WCHAR;
typedef uint32_t DWORD;
typedef void *HANDLE;
#define URL_MAX 8400
#define F_DONT_SIMPLIFY 0x08000000
#define F_FILE_USE_PATHURL 0x00010000
#define HeapAlloc(h, f, n) malloc(n)
#define HeapFree(h, f, p) free(p)
#define GetProcessHeap() 0

static unsigned up_char(unsigned c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
struct UrlScheme { const char *name; int scheme; int opaque; };
static const UrlScheme g_schemes[] = {
  {"ftp", 1, 0}, {"http", 2, 0}, {"gopher", 3, 0}, {"mailto", 4, 1}, {"news", 5, 0}, {"nntp", 6, 0}, {"telnet", 7, 0}, {"wais", 8, 0}, {"file", 9, 0},
  {"mk", 10, 0}, {"https", 11, 0}, {"shell", 12, 1}, {"snews", 13, 0}, {"local", 14, 0}, {"javascript", 15, 1}, {"vbscript", 16, 1}, {"about", 17, 1}, {"res", 18, 0},
};
static int url_scheme_length(const WCHAR *url) {
  int i = 0;
  while (url[i] && (((url[i] | 0x20) >= 'a' && (url[i] | 0x20) <= 'z') || (url[i] >= '0' && url[i] <= '9') || url[i] == '+' || url[i] == '-' || url[i] == '.')) ++i;
  return i >= 2 && url[i] == ':' ? i : 0;
}
static const UrlScheme *url_find_scheme(const WCHAR *url, int length) {
  for (const UrlScheme &s : g_schemes) {
    int k = 0;
    while (k < length && s.name[k] && up_char(url[k]) == up_char((unsigned char)s.name[k])) ++k;
    if (k == length && !s.name[k]) return &s;
  }
  return nullptr;
}
#include "../src/Windows/shlwapi/urlcanon.inc"

static std::string canon(const std::string &in, DWORD flags) {
  WCHAR source[URL_MAX + 1], out[URL_MAX + 1], dos[URL_MAX + 1];
  int n = 0;
  for (unsigned char c : in) if (n < URL_MAX) source[n++] = c;      // (the callers cut the input at URL_MAX)
  source[n] = 0;
  const int converted = ucn_dos_to_url(source, n, dos);
  const int m = converted >= 0 ? url_canon_core(dos, converted, flags, out) : url_canon_core(source, n, flags, out);
  std::string result;
  for (int i = 0; i < m; ++i) result += (char)out[i];
  return result;
}

static int failures = 0;
static void check(const char *url, DWORD flags, const char *expect) {
  const std::string got = canon(url, flags);
  if (got != expect) { ++failures; std::printf("FAIL %s flags %#x: expected %s, got %s\n", url, flags, expect, got.c_str()); }
}

int main() {
  const DWORD PATHURL = F_FILE_USE_PATHURL, WININET = 0x80000000u, NOSIMPLE = F_DONT_SIMPLIFY;
  // unknown scheme and no scheme
  check("wine://a/b/../c", 0, "wine://a/c");
  check("wine://a/..", 0, "wine://a/../");
  check("wine://a/.", NOSIMPLE, "wine://a/./");
  check("wine://a/b#c?d", 0, "wine://a/b?d#c");
  check("//", 0, "///");
  check("/a/b/../..", 0, "/");
  check("a/b/../c", 0, "a/c");
  check("..", 0, "../");
  check(".", NOSIMPLE, "./");
  // web schemes
  check("http://A/b/../../C", 0, "http://a/C");
  check("http://a/./b/./../", 0, "http://a/./b/./../");
  check("http://a/b/../../../c", 0, "http://a/c");
  check("http:a/..", 0, "http:");
  check("ftp://a\\b/..", 0, "ftp://a\\b/..");
  check("gopher://a\\b/..", 0, "gopher://a/");
  // mk and opaque
  check("mk:@progid/a/../..", 0, "mk:@progid/");
  check("mk://../..", 0, "mk:");
  check("mk:a\\./b", 0, "mk:a\\b");
  check("about://a/b/./c/../d", 0, "about://a/b/./c/../d");
  // file
  check("file://a/b/../c", 0, "file://a/c");
  check("file:////a/./b/../c", 0, "file://a/./b/../c");
  check("file://a:/b/../..", 0, "file:///a:/");
  check("file://a/b#c/../d\\e", 0, "file://a/d/e");
  check("file://a/b.html#c/../d\\e", 0, "file://a/b.html#c/../d\\e");
  check("file://a/b", PATHURL, "file://\\\\a\\b");
  check("file://localhost/b", WININET, "file://\\b");
  check("file:///c:/tests/foo", PATHURL, "file://c:\\tests\\foo");
  check("C:\\dir\\file", 0, "file:///C:/dir/file");
  check("\\\\a\\b", PATHURL, "file://\\\\a\\b");

  // every prefix and every one-character change of these must stay inside the buffers (the sanitizers check) and give something
  const char *seeds[] = {"wine://a/b/../c?x#y", "http://A/b/./../c\\d", "ftp:/\\a\\..", "mk:@MSITStore:dir/test.chm::/file.html/../..", "file://localhost/a:/b/../..#c?d",
                         "file://a/b.html#c/../d\\e", "q:/a/b/../../..", "\\\\a/./b/../c", "about:x", "//a//../../..", "file:////C:/user/file", " \t\nwine://. /.."};
  const char mutations[] = {'.', '/', '\\', ':', '?', '#', '@', ' ', '%', 'a', '|', '\0'};
  const DWORD flag_sets[] = {0, NOSIMPLE, PATHURL, WININET, PATHURL | WININET, NOSIMPLE | PATHURL};
  unsigned runs = 0;
  for (const char *seed : seeds) {
    const std::string base = seed;
    for (DWORD flags : flag_sets) {
      for (size_t cut = 0; cut <= base.size(); ++cut) { (void)canon(base.substr(0, cut), flags); ++runs; }
      for (size_t i = 0; i < base.size(); ++i)
        for (char m : mutations) { if (!m) continue; std::string t = base; t[i] = m; (void)canon(t, flags); ++runs; }
      for (size_t i = 0; i < base.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) { std::string t = base; t[i] = (char)(t[i] ^ (1 << bit)); if (t[i]) { (void)canon(t, flags); ++runs; } }
    }
  }
  // a very long path
  { std::string big = "http://a/"; for (int i = 0; i < 3000; ++i) big += "b/../c/"; for (DWORD f : flag_sets) { const std::string r = canon(big, f); if (r.size() > URL_MAX) { ++failures; std::puts("FAIL long result too long"); } } }
  std::printf("%u mutated runs, %d failures\n", runs, failures);
  return failures ? 1 : 0;
}
