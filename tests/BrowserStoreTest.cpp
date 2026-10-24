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
#include "Database/Local/LocalDatabase.hpp"
#include "Desktop/BrowserStore.hpp"
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
      cookies.store("session=1", url_of(a, "https://example.com/"), kNow, false);
      cookies.store("keep=two words; Max-Age=3600; Secure; HttpOnly; SameSite=Strict; Path=/a; Domain=example.com", url_of(a, "https://www.example.com/a/b"), kNow, false);
      cookies.store("part=3; Max-Age=3600; Secure; Partitioned", url_of(a, "https://example.com/"), kNow, false);
      local.area("https://example.com").set("theme", "dark");
      local.area("https://example.com").set("tab\tkey", "line\nbreak \"quoted\" 'single' \xC3\xA9");
      local.area("https://other.example:8443").set("k", "v");
      cookie_version = cookies.version();
      storage_version = local.version();
      CHECK(store.save(cookies, local, kNow));
      // saving again replaces, it does not add
      CHECK(store.save(cookies, local, kNow));
    }
    {   // the next start: the same data
      database::local::LocalDatabase database(a, &files, "/databases/system.db");
      CHECK(database.open() == database::DbStatus::Ok);
      BrowserStore store(a, database);
      CookieJar cookies(a);
      StorageHub local(a);
      uint32_t nc = 0, ni = 0;
      CHECK(store.load(cookies, local, kNow, &nc, &ni) && nc == 2 && ni == 3);
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
      CHECK(store.load(late, local2, kNow + 7200000, &nc, &ni) && nc == 0 && ni == 3);
    }
    {   // an empty jar and no storage: everything is gone from the table
      database::local::LocalDatabase database(a, &files, "/databases/system.db");
      CHECK(database.open() == database::DbStatus::Ok);
      BrowserStore store(a, database);
      CookieJar cookies(a);
      StorageHub local(a);
      CHECK(store.save(cookies, local, kNow));
      uint32_t nc = 9, ni = 9;
      CHECK(store.load(cookies, local, kNow, &nc, &ni) && nc == 0 && ni == 0);
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
