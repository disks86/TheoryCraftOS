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
#pragma once
// The cookies that outlive a session and the localStorage of the web browser, kept in the system database so that the next start has them again.
//
//   web_cookies        (name, value, domain, path TEXT; host_only, secure, http_only, partitioned, same_site, expires, created, accessed, position INTEGER)
//   web_local_storage  (origin, key, value TEXT)
//
// Saving replaces both tables in one transaction; loading puts what was saved into a CookieJar and a StorageHub (cookies that have expired are left out).
#include "Browser/CookieJar.hpp"
#include "Browser/PageHub.hpp"
#include "Database/Local/LocalDatabase.hpp"

namespace desktop {

class BrowserStore {
 public:
  BrowserStore(allocators::IAllocator& allocator, database::local::LocalDatabase& database) : allocator_(allocator), connection_(allocator, database) {}

  // Writes the persistent cookies and all of localStorage. False if the database is busy or cannot be written (nothing is changed then).
  bool save(browser::CookieJar& cookies, browser::StorageHub& local_storage, uint64_t now_ms);
  // Reads them back. Returns how many cookies and storage items were restored.
  bool load(browser::CookieJar& cookies, browser::StorageHub& local_storage, uint64_t now_ms, uint32_t* cookie_count, uint32_t* item_count);

 private:
  bool ensure_tables();
  bool run(StringView sql, ArrayView<const database::DbParam> params = ArrayView<const database::DbParam>());

  allocators::IAllocator& allocator_;
  database::local::LocalConnection connection_;
};

}  // namespace desktop
