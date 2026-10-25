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
// Host-side test of the web browser's saved data: the cookies that outlive a session and localStorage go into the system database and come back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Database/Local/LocalDatabase.hpp"
#include "Desktop/BrowserStore.hpp"
#include "Browser/CacheStore.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"

using namespace browser;
using namespace desktop;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static const uint64_t kNow = 1800000000000ull;

// Stands in for the service worker hub: the registrations it holds.
struct FakeWorkers final : IServiceWorkerPersistence {
  explicit FakeWorkers(TestAllocator& a) : alloc(a), list(a) {}
  TestAllocator& alloc;
  List<SavedRegistration> list;
  uint64_t changes = 0;
  void snapshot(List<SavedRegistration>* out) override {
    for (uint64_t i = 0; i < list.count(); ++i) {
      SavedRegistration copy(alloc);
      copy.origin.assign(list[i].origin.view());
      copy.scope.assign(list[i].scope.view());
      copy.script_url.assign(list[i].script_url.view());
      copy.update_via_cache.assign(list[i].update_via_cache.view());
      copy.type.assign(list[i].type.view());
      copy.source.assign(list[i].source.view());
      copy.imports.assign(list[i].imports.view());
      copy.preload_enabled = list[i].preload_enabled;
      copy.preload_header.assign(list[i].preload_header.view());
      out->push_back(static_cast<SavedRegistration&&>(copy));
    }
  }
  bool restore(SavedRegistration&& saved) override { return list.push_back(static_cast<SavedRegistration&&>(saved)); }
  uint64_t version() override { return changes; }
};

static CacheStore::Entry entry_of(TestAllocator& a, const char* url, uint32_t status, const char* body) {
  CacheStore::Entry e(a);
  e.method.assign("GET");
  e.url.assign(url);
  e.request_headers.assign("Accept: */*\r\n");
  e.status = status;
  e.status_text.assign("OK");
  e.headers.assign("Content-Type: text/plain\r\n");
  e.body.assign(StringView(body, strlen(body)));
  e.response_url.assign(url);
  e.response_type.assign("basic");
  return e;
}

static WebUrl url_of(TestAllocator& a, const char* text) {
  WebUrl u(a);
  WebUrl::parse(a, text, nullptr, &u);
  return u;
}

int main() {
  TestAllocator a;
  {
    filesystem::RamFileSystem files(a);
    uint64_t cookie_version = 0, storage_version = 0;
    {   // a first run: cookies and storage are made and saved
      database::local::LocalDatabase database(a, &files, "/databases/system.db");
      CHECK(database.open() == database::DbStatus::Ok);
      BrowserStore store(a, database);
      CookieJar cookies(a);
      StorageHub local(a);
      CacheStore caches(a);
      FakeWorkers workers(a);
      cookies.store("session=1", url_of(a, "https://example.com/"), kNow, false);
      cookies.store("keep=two words; Max-Age=3600; Secure; HttpOnly; SameSite=Strict; Path=/a; Domain=example.com", url_of(a, "https://www.example.com/a/b"), kNow, false);
      cookies.store("part=3; Max-Age=3600; Secure; Partitioned", url_of(a, "https://example.com/"), kNow, false);
      local.area("https://example.com").set("theme", "dark");
      local.area("https://example.com").set("tab\tkey", "line\nbreak \"quoted\" 'single' \xC3\xA9");
      local.area("https://other.example:8443").set("k", "v");
      cookie_version = cookies.version();
      storage_version = local.version();
      // the Cache API: two caches, one with binary bodies, one empty
      bool over = false;
      CHECK(caches.open("https://example.com", "pages") && caches.open("https://example.com", "empty") && caches.open("https://other.example:8443", "pages"));
      CHECK(caches.write("https://example.com", "pages", -1, entry_of(a, "https://example.com/a", 200, "alpha"), &over));
      CacheStore::Entry binary = entry_of(a, "https://example.com/b?x=1", 201, "");
      const char raw[] = {'\0', '\1', static_cast<char>(0xFF), '\r', '\n', '"', '\'', 'z'};
      binary.body.assign(StringView(raw, sizeof raw));
      binary.redirected = true;
      CHECK(caches.write("https://example.com", "pages", -1, static_cast<CacheStore::Entry&&>(binary), &over));
      CHECK(caches.write("https://other.example:8443", "pages", -1, entry_of(a, "https://other.example:8443/c", 200, "gamma"), &over));
      // a service worker registration
      SavedRegistration saved(a);
      saved.origin.assign("https://example.com");
      saved.scope.assign("https://example.com/app/");
      saved.script_url.assign("https://example.com/app/sw.js");
      saved.update_via_cache.assign("imports");
      saved.type.assign("classic");
      saved.source.assign("self.addEventListener('fetch', function (e) { /* \xC3\xA9 */ });");
      saved.imports.assign("https://example.com/lib.js\n9\nvar x = 1;");
      saved.preload_enabled = true;
      saved.preload_header.assign("custom");
      workers.list.push_back(static_cast<SavedRegistration&&>(saved));
      CHECK(store.save(cookies, local, caches, workers, kNow));
      // saving again replaces, it does not add
      CHECK(store.save(cookies, local, caches, workers, kNow));
    }
    {   // the next start: the same data
      database::local::LocalDatabase database(a, &files, "/databases/system.db");
      CHECK(database.open() == database::DbStatus::Ok);
      BrowserStore store(a, database);
      CookieJar cookies(a);
      StorageHub local(a);
      CacheStore caches(a);
      FakeWorkers workers(a);
      uint32_t nc = 0, ni = 0, ne = 0, nr = 0;
      CHECK(store.load(cookies, local, caches, workers, kNow, &nc, &ni, &ne, &nr) && nc == 2 && ni == 3 && ne == 3 && nr == 1);
      {   // the caches and the registration are as they were saved
        List<String> names(a);
        caches.names("https://example.com", &names);
        CHECK(names.count() == 2 && names[0].view().equals("pages") && names[1].view().equals("empty"));
        CHECK(caches.count("https://example.com", "pages") == 2 && caches.count("https://example.com", "empty") == 0 && caches.count("https://other.example:8443", "pages") == 1);
        CacheStore::Entry e(a);
        CHECK(caches.read("https://example.com", "pages", 1, &e, true) && e.url.view().equals("https://example.com/b?x=1") && e.status == 201 && e.redirected && e.body.size() == 8 &&
              e.body[2] == static_cast<char>(0xFF) && e.body[0] == '\0' && e.response_type.view().equals("basic") && e.request_headers.view().equals("Accept: */*\r\n"));
        CHECK(caches.read("https://example.com", "pages", 0, &e, true) && e.body.view().equals("alpha") && !e.redirected);
        CHECK(workers.list.count() == 1 && workers.list[0].scope.view().equals("https://example.com/app/") && workers.list[0].source.view().starts_with("self.addEventListener") &&
              workers.list[0].imports.view().equals("https://example.com/lib.js\n9\nvar x = 1;") && workers.list[0].preload_enabled && workers.list[0].preload_header.view().equals("custom"));
      }
      String header(a);
      cookies.header_for(url_of(a, "https://sub.example.com/a/x"), kNow, CookieJar::Site::SameSite, false, &header);
      CHECK(header.view().equals("keep=two words"));   // (the session cookie did not survive; "part" is host-only for example.com)
      header.clear();
      cookies.header_for(url_of(a, "https://example.com/"), kNow, CookieJar::Site::SameSite, false, &header);
      CHECK(header.view().equals("part=3"));
      header.clear();
      cookies.header_for(url_of(a, "https://sub.example.com/a/x"), kNow, CookieJar::Site::SameSite, true, &header);
      CHECK(header.empty());   // (HttpOnly stayed HttpOnly)
      StringView value;
      CHECK(local.area("https://example.com").get("theme", &value) && value.equals("dark"));
      CHECK(local.area("https://example.com").get("tab\tkey", &value) && value.equals("line\nbreak \"quoted\" 'single' \xC3\xA9"));
      CHECK(local.area("https://other.example:8443").get("k", &value) && value.equals("v") && local.origin_count() == 2);
      (void)cookie_version;
      (void)storage_version;
      // cookies that expired meanwhile are not brought back
      CookieJar late(a);
      StorageHub local2(a);
      CacheStore caches2(a);
      FakeWorkers workers2(a);
      CHECK(store.load(late, local2, caches2, workers2, kNow + 7200000, &nc, &ni) && nc == 0 && ni == 3);
    }
    {   // an empty jar and no storage: everything is gone from the table
      database::local::LocalDatabase database(a, &files, "/databases/system.db");
      CHECK(database.open() == database::DbStatus::Ok);
      BrowserStore store(a, database);
      CookieJar cookies(a);
      StorageHub local(a);
      CacheStore caches(a);
      FakeWorkers workers(a);
      CHECK(store.save(cookies, local, caches, workers, kNow));
      uint32_t nc = 9, ni = 9, ne = 9, nr = 9;
      CHECK(store.load(cookies, local, caches, workers, kNow, &nc, &ni, &ne, &nr) && nc == 0 && ni == 0 && ne == 0 && nr == 0 && caches.total_bytes() == 0);
    }
  }
  CHECK(a.live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("browser store tests passed\n");
  return 0;
}
