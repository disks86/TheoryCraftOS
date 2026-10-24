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
         run("CREATE TABLE IF NOT EXISTS web_local_storage (origin TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL)");
}

bool BrowserStore::save(browser::CookieJar& cookies, browser::StorageHub& local_storage, uint64_t now_ms) {
  if (!ensure_tables() || connection_.begin() != DbStatus::Ok) return false;
  bool ok = run("DELETE FROM web_cookies") && run("DELETE FROM web_local_storage");
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
  return ok ? connection_.commit() == DbStatus::Ok : (connection_.rollback(), false);
}

bool BrowserStore::load(browser::CookieJar& cookies, browser::StorageHub& local_storage, uint64_t now_ms, uint32_t* cookie_count, uint32_t* item_count) {
  *cookie_count = *item_count = 0;
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
  return true;
}

}  // namespace desktop
