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

// Host-side tests for WebUrl: the URL Standard's parsing (special and other schemes, hosts, ports, paths, queries, fragments), relative references, the setters,
// origins and form encoding - with damaged and truncated input.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/WebUrl.hpp"

using namespace browser;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static TestAllocator g_alloc;

// The href of `input` relative to `base` ("" base: none); "FAIL" if it does not parse.
static std::string href(const char* input, const char* base = "") {
  WebUrl b(g_alloc), u(g_alloc);
  const bool has_base = base[0] != 0;
  if (has_base && !WebUrl::parse(g_alloc, base, nullptr, &b)) return "BASE FAIL";
  if (!WebUrl::parse(g_alloc, input, has_base ? &b : nullptr, &u)) return "FAIL";
  String out(g_alloc);
  u.serialize(&out);
  return std::string(out.c_str());
}

static bool is(const char* input, const char* base, const char* expected) {
  const std::string got = href(input, base);
  if (got != expected) {
    printf("   '%s' (base '%s')\n   gives   %s\n   expects %s\n", input, base, got.c_str(), expected);
    return false;
  }
  return true;
}

static std::string part(const char* url, WebUrl::Part p) {
  WebUrl u(g_alloc);
  if (!WebUrl::parse(g_alloc, url, nullptr, &u)) return "FAIL";
  String out(g_alloc);
  u.get(p, &out);
  return std::string(out.c_str());
}

static std::string after_set(const char* url, WebUrl::Part p, const char* value) {
  WebUrl u(g_alloc);
  if (!WebUrl::parse(g_alloc, url, nullptr, &u)) return "FAIL";
  const bool ok = u.set(p, value);
  String out(g_alloc);
  u.serialize(&out);
  return std::string(ok ? "" : "!") + out.c_str();
}

static std::string origin_of(const char* url) {
  WebUrl u(g_alloc);
  if (!WebUrl::parse(g_alloc, url, nullptr, &u)) return "FAIL";
  String out(g_alloc);
  u.origin(&out);
  return std::string(out.c_str());
}

static void test_absolute() {
  CHECK(is("http://example.com/a/b/../c", "", "http://example.com/a/c"));
  CHECK(is("HTTP://EXAMPLE.com:80/", "", "http://example.com/"));
  CHECK(is("https://example.com:443/x", "", "https://example.com/x"));
  CHECK(is("https://example.com:8443/x", "", "https://example.com:8443/x"));
  CHECK(is("http://example.com", "", "http://example.com/"));
  CHECK(is("http://user:pa ss@host:8080/p?q#f", "", "http://user:pa%20ss@host:8080/p?q#f"));
  CHECK(is("http://u@h/", "", "http://u@h/"));
  CHECK(is("http://example.com/%2e%2E/x", "", "http://example.com/x"));
  CHECK(is("http://example.com/a/./b/.", "", "http://example.com/a/b/"));
  CHECK(is("http://example.com/a/b/..", "", "http://example.com/a/"));
  CHECK(is("http://example.com/a b/c\\d", "", "http://example.com/a%20b/c/d"));
  CHECK(is("http://example.com/?a=b c&d='e'#x y", "", "http://example.com/?a=b%20c&d=%27e%27#x%20y"));
  CHECK(is("  \thttp://exa\nmple.com/\r  ", "", "http://example.com/"));
  CHECK(is("http:example.com", "", "http://example.com/"));
  CHECK(is("http:///example.com/", "", "http://example.com/"));
  CHECK(is("http://example.com/\xC3\xA9", "", "http://example.com/%C3%A9"));
  CHECK(is("http://example.com/?\xC3\xA9", "", "http://example.com/?%C3%A9"));
  CHECK(is("http://example.com/a%20b%zz", "", "http://example.com/a%20b%zz"));
  CHECK(is("wss://chat.example/ws", "", "wss://chat.example/ws"));
  CHECK(is("ftp://files.example:21/x", "", "ftp://files.example/x"));
}

static void test_hosts() {
  CHECK(is("http://0x7f.1/", "", "http://127.0.0.1/"));
  CHECK(is("http://127.1/", "", "http://127.0.0.1/"));
  CHECK(is("http://2130706433/", "", "http://127.0.0.1/"));
  CHECK(is("http://0177.0.0.1/", "", "http://127.0.0.1/"));
  CHECK(is("http://1.2.3.4./", "", "http://1.2.3.4/"));
  CHECK(is("http://256.1.1.1/", "", "FAIL"));
  CHECK(is("http://1.2.3.4.5/", "", "FAIL"));
  CHECK(is("http://[::1]:3000/", "", "http://[::1]:3000/"));
  CHECK(is("http://[2001:DB8:0:0:0:0:0:1]/", "", "http://[2001:db8::1]/"));
  CHECK(is("http://[0:0:0:0:0:0:0:0]/", "", "http://[::]/"));
  CHECK(is("http://[1:0:0:2:0:0:0:3]/", "", "http://[1:0:0:2::3]/"));
  CHECK(is("http://[::ffff:1.2.3.4]/", "", "http://[::ffff:102:304]/"));
  CHECK(is("http://[::1/", "", "FAIL"));
  CHECK(is("http://[1::2::3]/", "", "FAIL"));
  CHECK(is("http://m\xC3\xBCnchen.de/", "", "http://xn--mnchen-3ya.de/"));
  CHECK(is("http://caf\xC3\xA9.example/", "", "http://xn--caf-dma.example/"));
  CHECK(is("http://EXAMPLE.Com/", "", "http://example.com/"));
  CHECK(is("http://exa%6dple.com/", "", "http://example.com/"));
  CHECK(is("http://a b/", "", "FAIL"));
  CHECK(is("http://a<b/", "", "FAIL"));
  CHECK(is("http://example.com:99999/", "", "FAIL"));
  CHECK(is("http://example.com:abc/", "", "FAIL"));
  CHECK(is("http://example.com:/", "", "http://example.com/"));
  CHECK(is("http://:80/", "", "FAIL"));
  CHECK(is("http://user@/", "", "FAIL"));
  CHECK(is("http://", "", "FAIL"));
  CHECK(is("foo://Host:99999/x", "", "FAIL"));
  CHECK(is("foo://Ho st/x", "", "FAIL"));
}

static void test_other_schemes() {
  CHECK(is("mailto:a@b.com", "", "mailto:a@b.com"));
  CHECK(is("data:text/plain,hi there", "", "data:text/plain,hi there"));
  CHECK(is("javascript:alert(1)", "", "javascript:alert(1)"));
  CHECK(is("foo://host/a b", "", "foo://host/a%20b"));
  CHECK(is("foo://Host:8/x?q#f", "", "foo://Host:8/x?q#f"));
  CHECK(is("foo:/x/../y", "", "foo:/y"));
  CHECK(is("file:///C:/a/../b", "", "file:///C:/b"));
  CHECK(is("file:///home/user/x.txt", "", "file:///home/user/x.txt"));
  CHECK(is("file://localhost/x", "", "file:///x"));
  CHECK(is("file:/x", "", "file:///x"));
  CHECK(is("blob:https://example.com/uuid", "", "blob:https://example.com/uuid"));
  CHECK(is("1http://x/", "", "FAIL"));
  CHECK(is("", "", "FAIL"));
  CHECK(is("/relative", "", "FAIL"));
}

static void test_relative() {
  const char* base = "http://a.example/b/c/d;p?q#f";
  CHECK(is("g", base, "http://a.example/b/c/g"));
  CHECK(is("./g", base, "http://a.example/b/c/g"));
  CHECK(is("g/", base, "http://a.example/b/c/g/"));
  CHECK(is("/g", base, "http://a.example/g"));
  CHECK(is("//g", base, "http://g/"));
  CHECK(is("?y", base, "http://a.example/b/c/d;p?y"));
  CHECK(is("g?y", base, "http://a.example/b/c/g?y"));
  CHECK(is("#s", base, "http://a.example/b/c/d;p?q#s"));
  CHECK(is("g#s", base, "http://a.example/b/c/g#s"));
  CHECK(is("g?y#s", base, "http://a.example/b/c/g?y#s"));
  CHECK(is(";x", base, "http://a.example/b/c/;x"));
  CHECK(is("", base, "http://a.example/b/c/d;p?q"));
  CHECK(is(".", base, "http://a.example/b/c/"));
  CHECK(is("./", base, "http://a.example/b/c/"));
  CHECK(is("..", base, "http://a.example/b/"));
  CHECK(is("../", base, "http://a.example/b/"));
  CHECK(is("../g", base, "http://a.example/b/g"));
  CHECK(is("../..", base, "http://a.example/"));
  CHECK(is("../../g", base, "http://a.example/g"));
  CHECK(is("../../../g", base, "http://a.example/g"));
  CHECK(is("g;x=1/../y", base, "http://a.example/b/c/y"));
  CHECK(is("https://other/x", base, "https://other/x"));
  CHECK(is("http:g", base, "http://a.example/b/c/g"));
  CHECK(is("mailto:x@y", base, "mailto:x@y"));
  CHECK(is("g", "mailto:x@y", "FAIL"));
  CHECK(is("#frag", "mailto:x@y", "mailto:x@y#frag"));
  CHECK(is("x", "file:///a/b", "file:///a/x"));
  CHECK(is("//h/x", "https://a/", "https://h/x"));
  CHECK(is("\\\\h\\x", "https://a/", "https://h/x"));
  CHECK(is("x", "foo://host/a/b", "foo://host/a/x"));
}

static void test_parts_and_setters() {
  const char* u = "https://user:pw@example.com:8443/a/b?x=1&y=2#top";
  CHECK(part(u, WebUrl::Part::Protocol) == "https:");
  CHECK(part(u, WebUrl::Part::Username) == "user");
  CHECK(part(u, WebUrl::Part::Password) == "pw");
  CHECK(part(u, WebUrl::Part::Host) == "example.com:8443");
  CHECK(part(u, WebUrl::Part::Hostname) == "example.com");
  CHECK(part(u, WebUrl::Part::Port) == "8443");
  CHECK(part(u, WebUrl::Part::Pathname) == "/a/b");
  CHECK(part(u, WebUrl::Part::Search) == "?x=1&y=2");
  CHECK(part(u, WebUrl::Part::Hash) == "#top");
  CHECK(part("https://example.com/", WebUrl::Part::Search) == "" && part("https://example.com/?", WebUrl::Part::Search) == "");
  CHECK(part("https://example.com/#", WebUrl::Part::Hash) == "");
  CHECK(part("https://example.com/", WebUrl::Part::Port) == "");

  CHECK(after_set(u, WebUrl::Part::Pathname, "/q w/../z") == "https://user:pw@example.com:8443/z?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Search, "a=b c") == "https://user:pw@example.com:8443/a/b?a=b%20c#top");
  CHECK(after_set(u, WebUrl::Part::Search, "?z=1") == "https://user:pw@example.com:8443/a/b?z=1#top");
  CHECK(after_set(u, WebUrl::Part::Search, "") == "https://user:pw@example.com:8443/a/b#top");
  CHECK(after_set(u, WebUrl::Part::Hash, "n x") == "https://user:pw@example.com:8443/a/b?x=1&y=2#n%20x");
  CHECK(after_set(u, WebUrl::Part::Hash, "") == "https://user:pw@example.com:8443/a/b?x=1&y=2");
  CHECK(after_set(u, WebUrl::Part::Port, "443") == "https://user:pw@example.com/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Port, "") == "https://user:pw@example.com/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Port, "99999").substr(0, 1) == "!");
  CHECK(after_set(u, WebUrl::Part::Port, "81ab") == "https://user:pw@example.com:81/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Hostname, "Other.ORG") == "https://user:pw@other.org:8443/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Host, "h.example:9000") == "https://user:pw@h.example:9000/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Host, "").substr(0, 1) == "!");
  CHECK(after_set(u, WebUrl::Part::Protocol, "http") == "http://user:pw@example.com:8443/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Protocol, "foo").substr(0, 1) == "!");
  CHECK(after_set(u, WebUrl::Part::Username, "a b") == "https://a%20b:pw@example.com:8443/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Password, "") == "https://user@example.com:8443/a/b?x=1&y=2#top");
  CHECK(after_set(u, WebUrl::Part::Href, "http://new/") == "http://new/");
  CHECK(after_set(u, WebUrl::Part::Href, "nonsense").substr(0, 1) == "!");
  CHECK(after_set("mailto:a@b", WebUrl::Part::Pathname, "x").substr(0, 1) == "!");
  CHECK(after_set("data:x", WebUrl::Part::Hostname, "h").substr(0, 1) == "!");
  CHECK(after_set("file:///x", WebUrl::Part::Port, "1").substr(0, 1) == "!");
}

static void test_origin_and_encoding() {
  CHECK(origin_of("https://a.com:8443/x") == "https://a.com:8443");
  CHECK(origin_of("http://a.com:80/x") == "http://a.com");
  CHECK(origin_of("file:///x") == "null");
  CHECK(origin_of("data:text/plain,x") == "null");
  CHECK(origin_of("foo://h/x") == "null");
  CHECK(origin_of("blob:https://a.com/uuid") == "https://a.com");
  CHECK(origin_of("blob:foo") == "null");
  WebUrl a(g_alloc), b(g_alloc), c(g_alloc);
  CHECK(WebUrl::parse(g_alloc, "https://a.com/x", nullptr, &a) && WebUrl::parse(g_alloc, "https://a.com:443/y", nullptr, &b) && WebUrl::parse(g_alloc, "http://a.com/x", nullptr, &c));
  CHECK(a.same_origin(b) && !a.same_origin(c));

  String out(g_alloc);
  WebUrl::form_encode("a b&c=d/é*-._~", &out);
  CHECK(std::string(out.c_str()) == "a+b%26c%3Dd%2F%C3%A9*-._%7E");
  String back(g_alloc);
  CHECK(WebUrl::form_decode(out.view(), &back) && std::string(back.c_str()) == "a b&c=d/\xC3\xA9*-._~");
  String bad(g_alloc);
  WebUrl::form_decode("100%+%4", &bad);
  CHECK(std::string(bad.c_str()) == "100% %4");
  String decoded(g_alloc);
  WebUrl::percent_decode("a%20b+c%", &decoded);
  CHECK(std::string(decoded.c_str()) == "a b+c%");
}

// Every prefix of some URLs, and the URLs with one byte changed, must parse or fail - and serialize to something that parses to the same thing.
static void test_damaged() {
  const char* samples[] = {"https://user:pw@example.com:8443/a/b?x=1&y=2#top", "http://[2001:db8::1]:80/p", "file:///C:/a/b", "mailto:a@b.com?subject=x", "http://m\xC3\xBCnchen.de/%41"};
  for (const char* sample : samples) {
    const std::string full = sample;
    for (size_t n = 0; n <= full.size(); ++n) {
      WebUrl u(g_alloc);
      if (WebUrl::parse(g_alloc, StringView(full.data(), n), nullptr, &u)) {
        String once(g_alloc), twice(g_alloc);
        u.serialize(&once);
        WebUrl v(g_alloc);
        CHECK(WebUrl::parse(g_alloc, once.view(), nullptr, &v));
        v.serialize(&twice);
        CHECK(once.view().equals(twice.view()));
      }
    }
    for (size_t i = 0; i < full.size(); ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        std::string flipped = full;
        flipped[i] = static_cast<char>(flipped[i] ^ (1 << bit));
        WebUrl u(g_alloc);
        if (WebUrl::parse(g_alloc, flipped.c_str(), nullptr, &u)) {
          String once(g_alloc), twice(g_alloc);
          u.serialize(&once);
          WebUrl v(g_alloc);
          if (!WebUrl::parse(g_alloc, once.view(), nullptr, &v)) {
            printf("   reparse failed: %s\n", once.c_str());
            ++g_failed;
            continue;
          }
          v.serialize(&twice);
          if (!once.view().equals(twice.view())) {
            printf("   not stable: %s -> %s\n", once.c_str(), twice.c_str());
            ++g_failed;
          }
        }
      }
    }
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_absolute();
  test_hosts();
  test_other_schemes();
  test_relative();
  test_parts_and_setters();
  test_origin_and_encoding();
  test_damaged();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("url tests passed\n");
  return 0;
}
