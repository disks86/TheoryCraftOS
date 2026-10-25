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
#include "Desktop/BrowserStore.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using database::DbParam;
using database::DbStatus;

bool BrowserStore::run(StringView sql, ArrayView<const DbParam> params) { return connection_.execute(sql, params, nullptr) == DbStatus::Ok; }

bool BrowserStore::ensure_tables() {
  return run("CREATE TABLE IF NOT EXISTS web_cookies (name TEXT NOT NULL, value TEXT NOT NULL, domain TEXT NOT NULL, path TEXT NOT NULL, host_only INTEGER, secure INTEGER, "
             "http_only INTEGER, partitioned INTEGER, same_site INTEGER, expires INTEGER, created INTEGER, accessed INTEGER, position INTEGER)") &&
         run("CREATE TABLE IF NOT EXISTS web_local_storage (origin TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL)") &&
         run("CREATE TABLE IF NOT EXISTS web_cache_entries (origin TEXT NOT NULL, cache TEXT NOT NULL, position INTEGER, method TEXT, url TEXT, request_headers TEXT, status INTEGER, "
             "status_text TEXT, headers TEXT, body BLOB, response_url TEXT, response_type TEXT, redirected INTEGER)") &&
         run("CREATE TABLE IF NOT EXISTS web_service_workers (origin TEXT NOT NULL, scope TEXT NOT NULL, script_url TEXT, update_via_cache TEXT, type TEXT, source BLOB, imports BLOB, "
             "preload_enabled INTEGER, preload_header TEXT)");
}

bool BrowserStore::save(browser::CookieJar& cookies, browser::StorageHub& local_storage, browser::CacheStore& caches, browser::IServiceWorkerPersistence& workers, uint64_t now_ms) {
  if (!ensure_tables() || connection_.begin() != DbStatus::Ok) return false;
  bool ok = run("DELETE FROM web_cookies") && run("DELETE FROM web_local_storage") && run("DELETE FROM web_cache_entries") && run("DELETE FROM web_service_workers");
  List<browser::CookieJar::Cookie> list(allocator_);
  if (ok) cookies.snapshot_persistent(now_ms, &list);
  for (uint64_t i = 0; ok && i < list.count(); ++i) {
    const browser::CookieJar::Cookie& c = list[i];
    const DbParam params[] = {DbParam::from_text(c.name.view()), DbParam::from_text(c.value.view()), DbParam::from_text(c.domain.view()), DbParam::from_text(c.path.view()),
                              DbParam::from_int(c.host_only), DbParam::from_int(c.secure), DbParam::from_int(c.http_only), DbParam::from_int(c.partitioned), DbParam::from_int(c.same_site),
                              DbParam::from_int(c.expires_ms), DbParam::from_int(static_cast<int64_t>(c.created)), DbParam::from_int(static_cast<int64_t>(c.accessed)),
                              DbParam::from_int(static_cast<int64_t>(c.order))};
    ok = run("INSERT INTO web_cookies VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 13));
  }
  for (uint32_t o = 0; ok && o < local_storage.origin_count(); ++o) {
    const StringView origin = local_storage.origin_name(o);
    browser::StorageArea& area = local_storage.area_at(o);
    for (uint32_t k = 0; ok && k < area.count(); ++k) {
      const DbParam params[] = {DbParam::from_text(origin), DbParam::from_text(area.key(k)), DbParam::from_text(area.value_at(k))};
      ok = run("INSERT INTO web_local_storage VALUES (?, ?, ?)", ArrayView<const DbParam>(params, 3));
    }
  }
  if (ok) {   // the Cache API's caches
    int64_t position = 0;
    caches.for_each([&](StringView origin, StringView cache, const browser::CacheStore::Entry* e) {
      if (!ok) return;
      if (!e) {
        const DbParam params[] = {DbParam::from_text(origin), DbParam::from_text(cache), DbParam::from_int(position++), DbParam::from_int(-1)};   // (status -1: a cache without entries)
        ok = run("INSERT INTO web_cache_entries (origin, cache, position, status) VALUES (?, ?, ?, ?)", ArrayView<const DbParam>(params, 4));
        return;
      }
      const DbParam params[] = {DbParam::from_text(origin), DbParam::from_text(cache), DbParam::from_int(position++), DbParam::from_text(e->method.view()), DbParam::from_text(e->url.view()),
                                DbParam::from_text(e->request_headers.view()), DbParam::from_int(e->status), DbParam::from_text(e->status_text.view()), DbParam::from_text(e->headers.view()),
                                DbParam::from_bytes(e->body.view().bytes()), DbParam::from_text(e->response_url.view()), DbParam::from_text(e->response_type.view()),
                                DbParam::from_int(e->redirected ? 1 : 0)};
      ok = run("INSERT INTO web_cache_entries VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 13));
    });
  }
  if (ok) {   // the registrations of service workers
    List<browser::SavedRegistration> saved(allocator_);
    workers.snapshot(&saved);
    for (uint64_t i = 0; ok && i < saved.count(); ++i) {
      const browser::SavedRegistration& r = saved[i];
      const DbParam params[] = {DbParam::from_text(r.origin.view()), DbParam::from_text(r.scope.view()), DbParam::from_text(r.script_url.view()), DbParam::from_text(r.update_via_cache.view()),
                                DbParam::from_text(r.type.view()), DbParam::from_bytes(r.source.view().bytes()), DbParam::from_bytes(r.imports.view().bytes()),
                                DbParam::from_int(r.preload_enabled ? 1 : 0), DbParam::from_text(r.preload_header.view())};
      ok = run("INSERT INTO web_service_workers VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 9));
    }
  }
  return ok ? connection_.commit() == DbStatus::Ok : (connection_.rollback(), false);
}

bool BrowserStore::load(browser::CookieJar& cookies, browser::StorageHub& local_storage, browser::CacheStore& caches, browser::IServiceWorkerPersistence& workers, uint64_t now_ms,
                        uint32_t* cookie_count, uint32_t* item_count, uint32_t* cache_entries, uint32_t* registrations) {
  *cookie_count = *item_count = 0;
  if (cache_entries) *cache_entries = 0;
  if (registrations) *registrations = 0;
  if (!ensure_tables()) return false;
  UniquePointer<database::IResultSet> rows;
  if (connection_.execute("SELECT name, value, domain, path, host_only, secure, http_only, partitioned, same_site, expires, created, accessed, position FROM web_cookies ORDER BY position", &rows) ==
          DbStatus::Ok && rows) {
    for (uint64_t r = 0; r < rows->row_count(); ++r) {
      browser::CookieJar::Cookie c(allocator_);
      int64_t host_only = 1, secure = 0, http_only = 0, partitioned = 0, same_site = 0, expires = -1, created = 0, accessed = 0, position = 0;
      c.name.assign(rows->text(r, 0));
      c.value.assign(rows->text(r, 1));
      c.domain.assign(rows->text(r, 2));
      c.path.assign(rows->text(r, 3));
      rows->get_int(r, 4, &host_only);
      rows->get_int(r, 5, &secure);
      rows->get_int(r, 6, &http_only);
      rows->get_int(r, 7, &partitioned);
      rows->get_int(r, 8, &same_site);
      rows->get_int(r, 9, &expires);
      rows->get_int(r, 10, &created);
      rows->get_int(r, 11, &accessed);
      rows->get_int(r, 12, &position);
      if (expires >= 0 && now_ms != 0 && expires <= static_cast<int64_t>(now_ms)) continue;
      c.host_only = host_only != 0;
      c.secure = secure != 0;
      c.http_only = http_only != 0;
      c.partitioned = partitioned != 0;
      c.same_site = static_cast<uint8_t>(same_site);
      c.expires_ms = expires;
      c.created = static_cast<uint64_t>(created);
      c.accessed = static_cast<uint64_t>(accessed);
      c.order = static_cast<uint64_t>(position);
      if (cookies.restore(util::move(c))) ++*cookie_count;
    }
  }
  rows.reset();
  if (connection_.execute("SELECT origin, key, value FROM web_local_storage", &rows) == DbStatus::Ok && rows) {
    for (uint64_t r = 0; r < rows->row_count(); ++r)
      if (local_storage.area(rows->text(r, 0)).set(rows->text(r, 1), rows->text(r, 2))) ++*item_count;
  }
  rows.reset();
  if (connection_.execute("SELECT origin, cache, position, method, url, request_headers, status, status_text, headers, body, response_url, response_type, redirected FROM web_cache_entries ORDER BY position",
                          &rows) == DbStatus::Ok && rows) {
    ByteBuffer body(allocator_);
    for (uint64_t r = 0; r < rows->row_count(); ++r) {
      int64_t position = 0, status = 200, redirected = 0;
      rows->get_int(r, 2, &position);
      rows->get_int(r, 6, &status);
      if (status < 0) {   // (a cache without entries)
        caches.open(rows->text(r, 0), rows->text(r, 1));
        continue;
      }
      browser::CacheStore::Entry e(allocator_);
      e.method.assign(rows->text(r, 3));
      e.url.assign(rows->text(r, 4));
      e.request_headers.assign(rows->text(r, 5));
      rows->get_int(r, 6, &status);
      e.status = static_cast<uint32_t>(status);
      e.status_text.assign(rows->text(r, 7));
      e.headers.assign(rows->text(r, 8));
      body.clear();
      if (rows->get_bytes(r, 9, &body)) e.body.assign(StringView(body.view()));
      e.response_url.assign(rows->text(r, 10));
      e.response_type.assign(rows->text(r, 11));
      rows->get_int(r, 12, &redirected);
      e.redirected = redirected != 0;
      if (caches.restore(rows->text(r, 0), rows->text(r, 1), util::move(e)) && cache_entries) ++*cache_entries;
    }
  }
  rows.reset();
  if (connection_.execute("SELECT origin, scope, script_url, update_via_cache, type, source, imports, preload_enabled, preload_header FROM web_service_workers", &rows) == DbStatus::Ok && rows) {
    ByteBuffer bytes(allocator_);
    for (uint64_t r = 0; r < rows->row_count(); ++r) {
      browser::SavedRegistration saved(allocator_);
      int64_t enabled = 0;
      saved.origin.assign(rows->text(r, 0));
      saved.scope.assign(rows->text(r, 1));
      saved.script_url.assign(rows->text(r, 2));
      saved.update_via_cache.assign(rows->text(r, 3));
      saved.type.assign(rows->text(r, 4));
      bytes.clear();
      if (rows->get_bytes(r, 5, &bytes)) saved.source.assign(StringView(bytes.view()));
      bytes.clear();
      if (rows->get_bytes(r, 6, &bytes)) saved.imports.assign(StringView(bytes.view()));
      rows->get_int(r, 7, &enabled);
      saved.preload_enabled = enabled != 0;
      saved.preload_header.assign(rows->text(r, 8));
      if (workers.restore(util::move(saved)) && registrations) ++*registrations;
    }
  }
  return true;
}

}  // namespace desktop
