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

// Host-side tests for the request layer: cookies sent and kept, forbidden headers, redirects (methods, credentials, limits), the cross-origin rules (CORS
// answers, preflight, credentials, opaque responses), mixed content and data: URLs - against a loader that hands out canned answers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Browser/Fetcher.hpp"

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
static const uint64_t kNow = 1800000000000ull;

struct Sent {
  std::string method, url, headers, body;
};

// Answers are keyed by "METHOD url"; an unknown one is a 404.
struct FakeLoader final : IPageLoader {
  struct Answer {
    uint32_t status = 200;
    std::string headers, body;
  };
  std::map<std::string, Answer> answers;
  std::vector<Sent> sent;
  bool fail_all = false;
  bool fetch(StringView, PageResult*) override { return false; }
  bool exchange(const NetRequest& request, NetResponse* response) override {
    Sent s;
    s.method.assign(request.method.data(), request.method.size());
    s.url.assign(request.url.data(), request.url.size());
    s.headers.assign(request.headers.data(), request.headers.size());
    s.body.assign(reinterpret_cast<const char*>(request.body.data()), request.body.size());
    sent.push_back(s);
    if (fail_all) {
      response->error.assign("connection failed");
      return false;
    }
    auto it = answers.find(s.method + " " + s.url);
    Answer a;
    if (it != answers.end()) a = it->second;
    else a.status = 404;
    response->status = a.status;
    response->reason.assign("R");
    response->headers.assign(StringView(a.headers.data(), a.headers.size()));
    response->body.clear();
    response->body.append(ConstByteView(reinterpret_cast<const uint8_t*>(a.body.data()), a.body.size()));
    return true;
  }
};

struct Result {
  bool ok = false;
  uint32_t status = 0;
  std::string error, headers, body, url;
  bool redirected = false, opaque = false;
};

static Result run(FakeLoader& loader, CookieJar& jar, const char* method, const char* url, const char* page, const char* headers = "", const char* body = "",
                  FetchOptions::Mode mode = FetchOptions::Mode::Cors, FetchOptions::Credentials cred = FetchOptions::Credentials::SameOrigin,
                  FetchOptions::Redirect redirect = FetchOptions::Redirect::Follow) {
  FetchOptions o;
  o.method = method;
  o.url = url;
  o.headers = headers;
  o.body = ConstByteView(reinterpret_cast<const uint8_t*>(body), strlen(body));
  o.mode = mode;
  o.credentials = cred;
  o.redirect = redirect;
  o.page_address = page;
  o.now_ms = kNow;
  NetResponse r(g_alloc);
  Result out;
  out.ok = fetch_resource(g_alloc, loader, jar, o, &r);
  out.status = r.status;
  out.error = r.error.c_str();
  out.headers = r.headers.c_str();
  out.body.assign(reinterpret_cast<const char*>(r.body.view().data()), r.body.size());
  out.url = r.url.c_str();
  out.redirected = r.redirected;
  out.opaque = r.opaque;
  return out;
}

static bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

static WebUrl url_of(const char* text) {
  WebUrl u(g_alloc);
  WebUrl::parse(g_alloc, text, nullptr, &u);
  return u;
}

static void test_same_origin() {
  FakeLoader loader;
  CookieJar jar(g_alloc);
  jar.store("sid=abc; Path=/", url_of("https://site.example/"), kNow, false);
  loader.answers["GET https://site.example/api"] = {200, "Content-Type: application/json\r\nSet-Cookie: seen=1; Path=/\r\nX-Other: yes\r\n", "{\"ok\":true}"};
  Result r = run(loader, jar, "get", "https://site.example/api", "https://site.example/page.html#frag", "Accept: */*\r\nX-Token: t1\r\nCookie: evil=1\r\nHost: evil\r\nOrigin: evil\r\nContent-Length: 99\r\n");
  CHECK(r.ok && r.status == 200 && r.body == "{\"ok\":true}" && r.url == "https://site.example/api");
  CHECK(loader.sent.size() == 1 && loader.sent[0].method == "GET");
  const std::string& h = loader.sent[0].headers;
  CHECK(has(h, "Cookie: sid=abc\r\n") && !has(h, "evil") && has(h, "X-Token: t1") && has(h, "Accept: */*") && !has(h, "Content-Length") && !has(h, "Origin:"));
  CHECK(has(h, "Referer: https://site.example/page.html\r\n"));   // (no fragment)
  CHECK(has(r.headers, "Content-Type: application/json") && has(r.headers, "X-Other: yes") && !has(r.headers, "Set-Cookie"));
  String got(g_alloc);
  jar.header_for(url_of("https://site.example/"), kNow, CookieJar::Site::SameSite, false, &got);
  CHECK(std::string(got.c_str()) == "sid=abc; seen=1");

  // POST sends the Origin
  loader.answers["POST https://site.example/api"] = {201, "", "made"};
  r = run(loader, jar, "POST", "https://site.example/api", "https://site.example/", "Content-Type: application/json\r\n", "{\"a\":1}");
  CHECK(r.ok && r.status == 201 && loader.sent.back().body == "{\"a\":1}" && has(loader.sent.back().headers, "Origin: https://site.example\r\n") && has(loader.sent.back().headers, "Content-Type: application/json"));
  // a GET with a body is refused; a bad method too
  CHECK(!run(loader, jar, "GET", "https://site.example/api", "https://site.example/", "", "x").ok);
  CHECK(!run(loader, jar, "TRACE", "https://site.example/api", "https://site.example/").ok);
  CHECK(!run(loader, jar, "GE T", "https://site.example/api", "https://site.example/").ok);
  // credentials: omit keeps the cookies home and ignores Set-Cookie
  size_t before = loader.sent.size();
  loader.answers["GET https://site.example/x"] = {200, "Set-Cookie: ignored=1\r\n", ""};
  r = run(loader, jar, "GET", "https://site.example/x", "https://site.example/", "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::Omit);
  CHECK(r.ok && loader.sent.size() == before + 1 && !has(loader.sent.back().headers, "Cookie:"));
  got.clear();
  jar.header_for(url_of("https://site.example/"), kNow, CookieJar::Site::SameSite, false, &got);
  CHECK(!has(got.c_str(), "ignored"));
  // a failure and a status are different things
  loader.fail_all = true;
  r = run(loader, jar, "GET", "https://site.example/x", "https://site.example/");
  CHECK(!r.ok && r.error == "connection failed");
  loader.fail_all = false;
  r = run(loader, jar, "GET", "https://site.example/missing", "https://site.example/");
  CHECK(r.ok && r.status == 404);
  // bad addresses
  CHECK(!run(loader, jar, "GET", "not a url", "https://site.example/").ok);
  CHECK(!run(loader, jar, "GET", "ftp://site.example/", "https://site.example/").ok);
  CHECK(!run(loader, jar, "GET", "http://site.example/", "https://site.example/").ok && has(run(loader, jar, "GET", "http://site.example/", "https://site.example/").error, "Mixed"));
  CHECK(!has(run(loader, jar, "GET", "http://localhost/", "https://site.example/").error, "Mixed"));
}

static void test_redirects() {
  FakeLoader loader;
  CookieJar jar(g_alloc);
  loader.answers["POST https://a.example/login"] = {302, "Location: /home\r\nSet-Cookie: session=s1; Path=/\r\n", ""};
  loader.answers["GET https://a.example/home"] = {200, "", "welcome"};
  Result r = run(loader, jar, "POST", "https://a.example/login", "https://a.example/", "Content-Type: text/plain\r\n", "user=x");
  CHECK(r.ok && r.body == "welcome" && r.redirected && r.url == "https://a.example/home");
  CHECK(loader.sent.size() == 2 && loader.sent[1].method == "GET" && loader.sent[1].body.empty() && has(loader.sent[1].headers, "Cookie: session=s1"));
  // 307 keeps the method and body
  loader.sent.clear();
  loader.answers["POST https://a.example/old"] = {307, "Location: https://a.example/new\r\n", ""};
  loader.answers["POST https://a.example/new"] = {200, "", "kept"};
  r = run(loader, jar, "POST", "https://a.example/old", "https://a.example/", "", "payload");
  CHECK(r.ok && r.body == "kept" && loader.sent.size() == 2 && loader.sent[1].method == "POST" && loader.sent[1].body == "payload");
  // 303 turns anything into GET (HEAD stays)
  loader.answers["PUT https://a.example/p"] = {303, "Location: /done\r\n", ""};
  loader.answers["GET https://a.example/done"] = {200, "", "done"};
  r = run(loader, jar, "PUT", "https://a.example/p", "https://a.example/", "", "x");
  CHECK(r.ok && r.body == "done" && loader.sent.back().method == "GET");
  // redirect modes
  r = run(loader, jar, "POST", "https://a.example/login", "https://a.example/", "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::SameOrigin, FetchOptions::Redirect::Manual);
  CHECK(r.ok && r.status == 302 && !r.redirected);
  r = run(loader, jar, "POST", "https://a.example/login", "https://a.example/", "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::SameOrigin, FetchOptions::Redirect::Error);
  CHECK(!r.ok);
  // loops stop
  loader.answers["GET https://a.example/loop"] = {302, "Location: /loop\r\n", ""};
  r = run(loader, jar, "GET", "https://a.example/loop", "https://a.example/");
  CHECK(!r.ok && has(r.error, "redirects"));
  // credentials do not follow to another origin; the other origin's cookies are not sent without credentials
  loader.sent.clear();
  loader.answers["GET https://a.example/away"] = {302, "Location: https://b.example/in\r\n", ""};
  loader.answers["GET https://b.example/in"] = {200, "Access-Control-Allow-Origin: https://a.example\r\n", "arrived"};
  jar.store("bc=1", url_of("https://b.example/"), kNow, false);
  r = run(loader, jar, "GET", "https://a.example/away", "https://a.example/", "Authorization: Bearer t\r\n");
  CHECK(r.ok && r.body == "arrived" && has(loader.sent[0].headers, "Authorization") && !has(loader.sent[1].headers, "Authorization") && !has(loader.sent[1].headers, "Cookie:"));
  CHECK(has(loader.sent[1].headers, "Origin: https://a.example"));
  // https to http
  loader.answers["GET https://a.example/down"] = {302, "Location: http://a.example/plain\r\n", ""};
  CHECK(!run(loader, jar, "GET", "https://a.example/down", "https://a.example/").ok);
  // a redirect to a bad place
  loader.answers["GET https://a.example/bad"] = {302, "Location: javascript:alert(1)\r\n", ""};
  CHECK(!run(loader, jar, "GET", "https://a.example/bad", "https://a.example/").ok);
}

static void test_cors() {
  FakeLoader loader;
  CookieJar jar(g_alloc);
  jar.store("a=1", url_of("https://api.other/"), kNow, false);
  const char* page = "https://app.example/";
  // no header: blocked
  loader.answers["GET https://api.other/data"] = {200, "", "secret"};
  Result r = run(loader, jar, "GET", "https://api.other/data", page);
  CHECK(!r.ok && has(r.error, "CORS") && has(loader.sent[0].headers, "Origin: https://app.example") && !has(loader.sent[0].headers, "Cookie:"));
  // the right header; only safe headers are shown
  loader.answers["GET https://api.other/data"] = {200, "Access-Control-Allow-Origin: https://app.example\r\nContent-Type: text/plain\r\nX-Secret: s\r\nX-Shown: v\r\nAccess-Control-Expose-Headers: X-Shown\r\n", "public"};
  r = run(loader, jar, "GET", "https://api.other/data", page);
  CHECK(r.ok && r.body == "public" && has(r.headers, "Content-Type") && has(r.headers, "X-Shown") && !has(r.headers, "X-Secret"));
  // *
  loader.answers["GET https://api.other/star"] = {200, "Access-Control-Allow-Origin: *\r\n", "star"};
  CHECK(run(loader, jar, "GET", "https://api.other/star", page).ok);
  CHECK(!run(loader, jar, "GET", "https://api.other/star", page, "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::Include).ok);   // (* is not allowed with credentials)
  // a wrong origin
  loader.answers["GET https://api.other/wrong"] = {200, "Access-Control-Allow-Origin: https://evil.example\r\n", "x"};
  CHECK(!run(loader, jar, "GET", "https://api.other/wrong", page).ok);
  // credentials
  loader.sent.clear();
  loader.answers["GET https://api.other/cred"] = {200, "Access-Control-Allow-Origin: https://app.example\r\nAccess-Control-Allow-Credentials: true\r\nSet-Cookie: got=1; Secure; SameSite=None\r\n", "with cookies"};
  r = run(loader, jar, "GET", "https://api.other/cred", page, "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::Include);
  CHECK(r.ok && has(loader.sent[0].headers, "Cookie: a=1") == false);   // (a=1 has the default SameSite=Lax: not sent on a cross-site request)
  jar.store("n=1; Secure; SameSite=None", url_of("https://api.other/"), kNow, false);
  loader.sent.clear();
  r = run(loader, jar, "GET", "https://api.other/cred", page, "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::Include);
  CHECK(r.ok && has(loader.sent[0].headers, "Cookie: got=1; n=1"));
  loader.answers["GET https://api.other/nocred"] = {200, "Access-Control-Allow-Origin: https://app.example\r\n", "x"};
  CHECK(!run(loader, jar, "GET", "https://api.other/nocred", page, "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::Include).ok);   // (needs Allow-Credentials)

  // preflight for a custom header and for PUT
  loader.sent.clear();
  loader.answers["OPTIONS https://api.other/item"] = {204, "Access-Control-Allow-Origin: https://app.example\r\nAccess-Control-Allow-Methods: PUT, DELETE\r\nAccess-Control-Allow-Headers: x-token, content-type\r\n", ""};
  loader.answers["PUT https://api.other/item"] = {200, "Access-Control-Allow-Origin: https://app.example\r\n", "updated"};
  r = run(loader, jar, "PUT", "https://api.other/item", page, "X-Token: t\r\nContent-Type: application/json\r\n", "{}");
  CHECK(r.ok && r.body == "updated" && loader.sent.size() == 2 && loader.sent[0].method == "OPTIONS");
  CHECK(has(loader.sent[0].headers, "Access-Control-Request-Method: PUT") && has(loader.sent[0].headers, "Access-Control-Request-Headers: x-token,content-type") && !has(loader.sent[0].headers, "Cookie"));
  CHECK(loader.sent[1].body == "{}");
  // a method or header the server did not allow
  loader.answers["PATCH https://api.other/item"] = {200, "Access-Control-Allow-Origin: https://app.example\r\n", "no"};
  loader.sent.clear();
  CHECK(!run(loader, jar, "PATCH", "https://api.other/item", page).ok && loader.sent.size() == 1);   // (the real request never goes out)
  CHECK(!run(loader, jar, "PUT", "https://api.other/item", page, "X-Other: 1\r\n").ok);
  // a failed preflight
  loader.answers["OPTIONS https://api.other/denied"] = {403, "", ""};
  CHECK(!run(loader, jar, "DELETE", "https://api.other/denied", page).ok);
  // a simple POST needs no preflight
  loader.sent.clear();
  loader.answers["POST https://api.other/form"] = {200, "Access-Control-Allow-Origin: *\r\n", "posted"};
  r = run(loader, jar, "POST", "https://api.other/form", page, "Content-Type: application/x-www-form-urlencoded\r\n", "a=1");
  CHECK(r.ok && loader.sent.size() == 1);
  // application/json is not simple
  loader.sent.clear();
  r = run(loader, jar, "POST", "https://api.other/form", page, "Content-Type: application/json\r\n", "{}");
  CHECK(!r.ok && loader.sent.size() == 1 && loader.sent[0].method == "OPTIONS");
  // same-origin mode refuses, no-cors gives an opaque answer
  CHECK(!run(loader, jar, "GET", "https://api.other/data", page, "", "", FetchOptions::Mode::SameOrigin).ok);
  r = run(loader, jar, "GET", "https://api.other/data", page, "", "", FetchOptions::Mode::NoCors);
  CHECK(r.ok && r.opaque && r.status == 0 && r.body.empty() && r.headers.empty());
  CHECK(!run(loader, jar, "PUT", "https://api.other/data", page, "", "", FetchOptions::Mode::NoCors).ok);
  // a page that is a local file has an opaque origin
  loader.answers["GET https://api.other/local"] = {200, "Access-Control-Allow-Origin: null\r\n", "ok"};
  CHECK(run(loader, jar, "GET", "https://api.other/local", "/files/page.html").ok);
  // navigation is not subject to CORS, and sends the cookies
  loader.sent.clear();
  loader.answers["GET https://api.other/nav"] = {200, "", "page"};
  jar.store("lax=1; SameSite=Lax", url_of("https://api.other/"), kNow, false);
  r = run(loader, jar, "GET", "https://api.other/nav", page, "", "", FetchOptions::Mode::Navigate);
  CHECK(r.ok && has(loader.sent[0].headers, "lax=1") && has(loader.sent[0].headers, "n=1"));
}

static void test_data_urls() {
  FakeLoader loader;
  CookieJar jar(g_alloc);
  Result r = run(loader, jar, "GET", "data:text/plain;base64,SGVsbG8sIHdvcmxkIQ==", "https://a.example/");
  CHECK(r.ok && r.status == 200 && r.body == "Hello, world!" && has(r.headers, "Content-Type: text/plain") && loader.sent.empty());
  r = run(loader, jar, "GET", "data:,a%20b%2Cc", "https://a.example/");
  CHECK(r.ok && r.body == "a b,c" && has(r.headers, "text/plain;charset=US-ASCII"));
  r = run(loader, jar, "GET", "data:application/json,{\"x\":1}#frag", "https://a.example/");
  CHECK(r.ok && r.body == "{\"x\":1}");
  CHECK(!run(loader, jar, "GET", "data:text/plain", "https://a.example/").ok);
  CHECK(!run(loader, jar, "GET", "data:;base64,@@@@", "https://a.example/").ok);
}

static void test_header_helpers() {
  StringView v;
  CHECK(header_value("A: 1\r\nContent-Type:  text/html ; x=y \r\nB: 2\r\n", "content-type", &v) && v.equals("text/html ; x=y"));
  CHECK(!header_value("A: 1\r\n", "b", &v));
  CHECK(header_value("no colon\r\n:empty\r\nGood: yes\r\n", "good", &v) && v.equals("yes"));
  int count = 0;
  for_each_header("A: 1\nB: 2\r\nC: 3", [&](StringView, StringView) { ++count; });
  CHECK(count == 3);
}

// Truncated and flipped Set-Cookie / Location / CORS headers in answers must never break the request layer.
static void test_damaged_answers() {
  const std::string base = "Location: /next\r\nSet-Cookie: id=1; Path=/; Secure; SameSite=None\r\nAccess-Control-Allow-Origin: https://app.example\r\nAccess-Control-Allow-Credentials: true\r\n";
  for (size_t n = 0; n <= base.size(); ++n) {
    for (int mutate = 0; mutate < 3; ++mutate) {
      std::string headers = base.substr(0, n);
      if (mutate == 1 && !headers.empty()) headers[n / 2 % headers.size()] ^= 0x20;
      if (mutate == 2 && !headers.empty()) headers[n / 3 % headers.size()] ^= 0x01;
      FakeLoader loader;
      CookieJar jar(g_alloc);
      loader.answers["GET https://api.other/x"] = {302, headers, ""};
      loader.answers["GET https://api.other/next"] = {200, headers, "body"};
      run(loader, jar, "GET", "https://api.other/x", "https://app.example/", "", "", FetchOptions::Mode::Cors, FetchOptions::Credentials::Include);
    }
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_same_origin();
  test_redirects();
  test_cors();
  test_data_urls();
  test_header_helpers();
  test_damaged_answers();
  CHECK(g_alloc.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("fetcher tests passed\n");
  return 0;
}
