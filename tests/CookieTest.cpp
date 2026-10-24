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

// Host-side tests for the cookie jar: Set-Cookie parsing and its attributes, domain and path matching, expiry, Secure / HttpOnly / SameSite, the cookie name
// prefixes, ordering, the limits, and HTTP dates.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "Browser/CookieJar.hpp"
#include "Browser/PublicSuffix.hpp"

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
static const uint64_t kNow = 1800000000000ull;   // 2027-01-15

static WebUrl url_of(const char* text) {
  WebUrl u(g_alloc);
  if (!WebUrl::parse(g_alloc, text, nullptr, &u)) printf("bad test url %s\n", text);
  return u;
}

static bool store(CookieJar& jar, const char* header, const char* url, bool script = false, uint64_t now = kNow) { return jar.store(header, url_of(url), now, script); }

static std::string header(CookieJar& jar, const char* url, CookieJar::Site site = CookieJar::Site::SameSite, bool script = false, uint64_t now = kNow) {
  String out(g_alloc);
  jar.header_for(url_of(url), now, site, script, &out);
  return std::string(out.c_str());
}

static void test_basics() {
  CookieJar jar(g_alloc);
  CHECK(store(jar, "a=1", "http://example.com/"));
  CHECK(store(jar, "b=2; Path=/", "http://example.com/x/y"));
  CHECK(header(jar, "http://example.com/") == "a=1; b=2");
  CHECK(header(jar, "http://other.com/").empty());
  CHECK(store(jar, "a=3", "http://example.com/"));   // replaces
  CHECK(header(jar, "http://example.com/") == "a=3; b=2");
  CHECK(jar.count() == 2);
  // no "=": a cookie with no name
  CHECK(store(jar, "novalue", "http://example.com/"));
  CHECK(header(jar, "http://example.com/").find("novalue") != std::string::npos);
  CHECK(!store(jar, "", "http://example.com/") && !store(jar, " ; Path=/", "http://example.com/"));
  CHECK(!store(jar, "a=b\nc", "http://example.com/"));
  // whitespace around names and values
  CookieJar j2(g_alloc);
  CHECK(store(j2, "  k  =  v w  ; Path=/", "http://example.com/") && header(j2, "http://example.com/") == "k=v w");
  // size limit
  CookieJar j3(g_alloc);
  std::string big = "big=" + std::string(5000, 'x');
  CHECK(!store(j3, big.c_str(), "http://example.com/"));
}

static void test_path() {
  CookieJar jar(g_alloc);
  CHECK(store(jar, "d=1", "http://example.com/a/b/c"));   // default path /a/b
  CHECK(header(jar, "http://example.com/a/b/x") == "d=1");
  CHECK(header(jar, "http://example.com/a/bc").empty());
  CHECK(header(jar, "http://example.com/a/b") == "d=1");
  CHECK(header(jar, "http://example.com/a").empty());
  CHECK(store(jar, "p=1; Path=/a", "http://example.com/a/b/c"));
  CHECK(store(jar, "q=1; Path=/", "http://example.com/a/b/c"));
  CHECK(header(jar, "http://example.com/a/b/c") == "d=1; p=1; q=1");   // longest path first
  CHECK(store(jar, "s=1; Path=nonsense", "http://example.com/a/b/c") && header(jar, "http://example.com/a/b/z").find("s=1") != std::string::npos);
  CookieJar root(g_alloc);
  CHECK(store(root, "r=1", "http://example.com/file") && header(root, "http://example.com/other") == "r=1");
}

static void test_domain() {
  CookieJar jar(g_alloc);
  CHECK(store(jar, "host=1", "http://www.example.com/"));
  CHECK(header(jar, "http://www.example.com/") == "host=1" && header(jar, "http://sub.www.example.com/").empty() && header(jar, "http://example.com/").empty());
  CHECK(store(jar, "wide=1; Domain=example.com", "http://www.example.com/"));
  CHECK(header(jar, "http://example.com/") == "wide=1" && header(jar, "http://a.b.example.com/") == "wide=1");
  CHECK(store(jar, "dot=1; Domain=.example.com", "http://www.example.com/"));
  CHECK(!store(jar, "evil=1; Domain=other.com", "http://www.example.com/"));
  CHECK(!store(jar, "evil=1; Domain=com", "http://www.example.com/"));
  CHECK(!store(jar, "evil=1; Domain=ple.com", "http://www.example.com/"));
  CHECK(!store(jar, "evil=1; Domain=sub.www.example.com", "http://www.example.com/"));
  CHECK(header(jar, "http://notexample.com/").empty());
  // Domain equal to the host is host-only
  CookieJar j2(g_alloc);
  CHECK(store(j2, "x=1; Domain=www.example.com", "http://www.example.com/") && header(j2, "http://a.www.example.com/") == "x=1");
  // IP addresses and localhost
  CookieJar j3(g_alloc);
  CHECK(store(j3, "i=1", "http://127.0.0.1:8080/") && header(j3, "http://127.0.0.1/") == "i=1");
  CHECK(!store(j3, "i=1; Domain=0.0.1", "http://127.0.0.1/"));
  CHECK(store(j3, "l=1", "http://localhost:3000/") && header(j3, "http://localhost/") == "l=1");
  // cookies are shared between ports, as the standard says
  CHECK(header(j3, "http://localhost:9/") == "l=1");
}

static void test_expiry() {
  CookieJar jar(g_alloc);
  CHECK(store(jar, "s=1", "http://example.com/"));
  CHECK(store(jar, "m=1; Max-Age=60", "http://example.com/"));
  CHECK(header(jar, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 59000) == "s=1; m=1");
  CHECK(header(jar, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 61000) == "s=1");
  CHECK(jar.count() == 1);
  CHECK(store(jar, "gone=1", "http://example.com/") && store(jar, "gone=; Max-Age=0", "http://example.com/"));
  CHECK(header(jar, "http://example.com/") == "s=1");
  CHECK(store(jar, "neg=1", "http://example.com/") && store(jar, "neg=x; Max-Age=-5", "http://example.com/") && header(jar, "http://example.com/") == "s=1");
  CHECK(store(jar, "e=1; Expires=Fri, 15 Jan 2027 08:00:00 GMT", "http://example.com/"));   // an hour after kNow... 2027-01-15 08:00 UTC
  CHECK(!header(jar, "http://example.com/", CookieJar::Site::SameSite, false, kNow).empty());
  CHECK(store(jar, "old=1; Expires=Thu, 01 Jan 1970 00:00:01 GMT", "http://example.com/") && header(jar, "http://example.com/").find("old") == std::string::npos);
  // Max-Age wins over Expires
  CHECK(store(jar, "w=1; Expires=Fri, 15 Jan 2027 08:00:00 GMT; Max-Age=10", "http://example.com/"));
  CHECK(header(jar, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 20000).find("w=1") == std::string::npos);
  // a far future expiry is capped at 400 days
  CookieJar j2(g_alloc);
  CHECK(store(j2, "far=1; Max-Age=99999999999", "http://example.com/"));
  CHECK(!header(j2, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 399ull * 86400000).empty());
  CHECK(header(j2, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 401ull * 86400000).empty());
}

static void test_flags() {
  CookieJar jar(g_alloc);
  CHECK(store(jar, "h=1; HttpOnly", "http://example.com/"));
  CHECK(header(jar, "http://example.com/") == "h=1" && header(jar, "http://example.com/", CookieJar::Site::SameSite, true).empty());
  CHECK(!store(jar, "h=2", "http://example.com/", true));   // a script cannot replace it
  CHECK(!store(jar, "n=1; HttpOnly", "http://example.com/", true));
  CHECK(header(jar, "http://example.com/") == "h=1");
  // Secure
  CHECK(!store(jar, "sec=1; Secure", "http://example.com/"));
  CHECK(store(jar, "sec=1; Secure", "https://example.com/"));
  CHECK(header(jar, "https://example.com/").find("sec=1") != std::string::npos && header(jar, "http://example.com/").find("sec=1") == std::string::npos);
  CHECK(!store(jar, "sec=2", "http://example.com/"));   // an insecure page cannot overwrite a secure cookie
  CHECK(header(jar, "https://example.com/").find("sec=1") != std::string::npos);
  // prefixes
  CHECK(!store(jar, "__Secure-a=1", "https://example.com/") && store(jar, "__Secure-a=1; Secure", "https://example.com/"));
  CHECK(!store(jar, "__Host-a=1; Secure", "https://example.com/"));   // no Path=/
  CHECK(!store(jar, "__Host-b=1; Secure; Path=/; Domain=example.com", "https://example.com/"));
  CHECK(store(jar, "__Host-c=1; Secure; Path=/", "https://example.com/"));
  // SameSite
  CookieJar s(g_alloc);
  CHECK(store(s, "lax=1; SameSite=Lax", "https://example.com/") && store(s, "strict=1; SameSite=Strict", "https://example.com/") && store(s, "none=1; SameSite=None; Secure", "https://example.com/") && store(s, "dflt=1", "https://example.com/"));
  CHECK(!store(s, "bad=1; SameSite=None", "https://example.com/"));
  CHECK(header(s, "https://example.com/", CookieJar::Site::SameSite) == "lax=1; strict=1; none=1; dflt=1");
  CHECK(header(s, "https://example.com/", CookieJar::Site::CrossSiteNavigation) == "lax=1; none=1; dflt=1");
  CHECK(header(s, "https://example.com/", CookieJar::Site::CrossSiteRequest) == "none=1");
}

static void test_limits_and_dates() {
  CookieJar jar(g_alloc);
  for (int i = 0; i < 300; ++i) {
    std::string h = "c" + std::to_string(i) + "=v";
    store(jar, h.c_str(), "http://example.com/", false, kNow + i);
  }
  CHECK(jar.count() == CookieJar::kMaxPerHost);
  CHECK(header(jar, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 1000).find("c299=v") != std::string::npos);
  CHECK(header(jar, "http://example.com/", CookieJar::Site::SameSite, false, kNow + 1000).find("c0=v") == std::string::npos);

  int64_t ms = 0;
  CHECK(CookieJar::parse_http_date("Wed, 21 Oct 2015 07:28:00 GMT", &ms) && ms == 1445412480000ll);
  CHECK(CookieJar::parse_http_date("Wednesday, 21-Oct-15 07:28:00 GMT", &ms) && ms == 1445412480000ll);
  CHECK(CookieJar::parse_http_date("Wed Oct 21 07:28:00 2015", &ms) && ms == 1445412480000ll);
  CHECK(CookieJar::parse_http_date("21 Oct 1999 00:00:00", &ms) && ms == 940464000000ll);
  CHECK(!CookieJar::parse_http_date("tomorrow", &ms) && !CookieJar::parse_http_date("32 Oct 2015 07:28:00 GMT", &ms) && !CookieJar::parse_http_date("21 Oct 2015 25:28:00", &ms));
  String text(g_alloc);
  CookieJar::format_http_date(1445412480000ll, &text);
  CHECK(std::string(text.c_str()) == "Wed, 21 Oct 2015 07:28:00 GMT");
  text.clear();
  CookieJar::format_http_date(0, &text);
  CHECK(std::string(text.c_str()) == "Thu, 01 Jan 1970 00:00:00 GMT");
}

// Damaged headers must never crash or leave the jar in a state that breaks the next request.
static void test_damaged() {
  const char* samples[] = {"id=abc123; Path=/app; Domain=example.com; Expires=Wed, 21 Oct 2026 07:28:00 GMT; Max-Age=3600; Secure; HttpOnly; SameSite=Lax",
                           "__Host-x=1; Secure; Path=/"};
  for (const char* sample : samples) {
    const std::string full = sample;
    for (size_t n = 0; n <= full.size(); ++n) {
      CookieJar jar(g_alloc);
      store(jar, std::string(full, 0, n).c_str(), "https://www.example.com/app/x");
      header(jar, "https://www.example.com/app/y");
    }
    for (size_t i = 0; i < full.size(); ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        std::string flipped = full;
        flipped[i] = static_cast<char>(flipped[i] ^ (1 << bit));
        CookieJar jar(g_alloc);
        store(jar, flipped.c_str(), "https://www.example.com/app/x");
        header(jar, "https://www.example.com/app/y");
      }
    }
  }
}

static void test_public_suffix_and_more() {
  // a Domain naming a public suffix is refused, unless it is the host itself (then the cookie is a host cookie)
  CookieJar jar(g_alloc);
  CHECK(!store(jar, "a=1; Domain=com", "http://example.com/") && !store(jar, "b=1; Domain=co.uk", "http://shop.example.co.uk/") && !store(jar, "c=1; Domain=github.io", "http://me.github.io/"));
  CHECK(store(jar, "d=1; Domain=example.co.uk", "http://shop.example.co.uk/") && header(jar, "http://www.example.co.uk/") == "d=1");
  CHECK(store(jar, "e=1; Domain=github.io", "http://github.io/") && header(jar, "http://github.io/") == "e=1" && header(jar, "http://me.github.io/").empty());
  CHECK(is_public_suffix("com") && is_public_suffix("co.uk") && is_public_suffix("herokuapp.com") && !is_public_suffix("example.com") && !is_public_suffix("example.co.uk"));
  // Partitioned needs Secure
  CookieJar p(g_alloc);
  CHECK(!store(p, "x=1; Partitioned", "https://example.com/") && store(p, "x=1; Secure; Partitioned", "https://example.com/"));
  List<CookieJar::Cookie> list(g_alloc);
  p.list_for_script(url_of("https://example.com/"), kNow, &list);
  CHECK(list.count() == 1 && list[0].partitioned && list[0].secure);
  // the version changes with every change
  CookieJar v(g_alloc);
  const uint64_t v0 = v.version();
  store(v, "a=1", "http://example.com/");
  const uint64_t v1 = v.version();
  store(v, "a=2", "http://example.com/");
  CHECK(v1 > v0 && v.version() > v1);
  const uint64_t v2 = v.version();
  CHECK(v.remove_cookie("a", "example.com", "/", true) && v.version() > v2 && v.count() == 0 && !v.remove_cookie("a", "example.com", "/", true));
  // saving and putting back: only the cookies that outlive the session, with every attribute
  CookieJar src(g_alloc);
  store(src, "session=1", "https://example.com/");
  store(src, "keep=2; Max-Age=3600; Secure; HttpOnly; SameSite=None; Path=/a; Domain=example.com", "https://www.example.com/a/b");
  store(src, "gone=3; Max-Age=10", "https://example.com/");
  List<CookieJar::Cookie> saved(g_alloc);
  src.snapshot_persistent(kNow + 20000, &saved);   // (twenty seconds on: "gone" has expired)
  CHECK(saved.count() == 1 && saved[0].name.view().equals("keep") && saved[0].http_only && saved[0].secure && !saved[0].host_only && saved[0].same_site == 3);
  CookieJar dst(g_alloc);
  for (uint64_t i = 0; i < saved.count(); ++i) CHECK(dst.restore(static_cast<CookieJar::Cookie&&>(saved[i])));
  CHECK(header(dst, "https://sub.example.com/a/x", CookieJar::Site::SameSite, false, kNow + 20000) == "keep=2" && header(dst, "https://sub.example.com/a/x", CookieJar::Site::SameSite, true, kNow + 20000).empty());
  // the cookies a script sees, in header order, without HttpOnly ones
  CookieJar l(g_alloc);
  store(l, "a=1; Path=/", "http://example.com/x/y");
  store(l, "b=2", "http://example.com/x/y");
  store(l, "h=3; HttpOnly", "http://example.com/x/y");
  List<CookieJar::Cookie> seen(g_alloc);
  l.list_for_script(url_of("http://example.com/x/z"), kNow, &seen);
  CHECK(seen.count() == 2 && seen[0].name.view().equals("b") && seen[1].name.view().equals("a"));
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_basics();
  test_path();
  test_domain();
  test_expiry();
  test_flags();
  test_limits_and_dates();
  test_damaged();
  test_public_suffix_and_more();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("cookie tests passed\n");
  return 0;
}
