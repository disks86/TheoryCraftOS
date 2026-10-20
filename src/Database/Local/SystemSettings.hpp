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

// The settings of the system and of its programs, kept in the system database (table "settings": scope, name, value). A scope is the owner of the
// setting ("desktop", "network", "database-query"); a name is the setting within it. Values are kept as text; the typed calls convert. Safe to use
// from any thread.
//
//   SystemSettings settings(allocator, system_db);
//   int64_t volume = settings.get_int("audio", "volume", 80);
//   settings.set_int("audio", "volume", 65);
//
// Reading never fails: a setting that is not there (or cannot be read) gives the default asked for. Writing says whether it was kept.
#pragma once
#include "Database/Local/LocalDatabase.hpp"

namespace database::local {

class SystemSettings {
 public:
  SystemSettings(allocators::IAllocator& allocator, LocalDatabase& database) : allocator_(allocator), database_(database), connection_(allocator, database), error_(allocator) { ensure_table(); }

  // The database the settings are kept in, for programs that keep tables of their own beside them.
  LocalDatabase& database() { return database_; }

  // False when the settings table could not be made.
  bool ready() const { return ready_; }

  bool has(StringView scope, StringView name);
  // The value as text; false (and *out cleared) when there is none.
  bool get_text(StringView scope, StringView name, String* out);
  int64_t get_int(StringView scope, StringView name, int64_t fallback);
  bool get_bool(StringView scope, StringView name, bool fallback);
  double get_real(StringView scope, StringView name, double fallback);

  bool set_text(StringView scope, StringView name, StringView value);
  bool set_int(StringView scope, StringView name, int64_t value);
  bool set_bool(StringView scope, StringView name, bool value) { return set_text(scope, name, value ? "true" : "false"); }
  bool set_real(StringView scope, StringView name, double value);
  bool remove(StringView scope, StringView name);
  // Removes every setting of a scope; returns how many there were.
  uint64_t remove_scope(StringView scope);

 private:
  void ensure_table();

  allocators::IAllocator& allocator_;
  LocalDatabase& database_;
  kernel::Mutex mutex_;
  LocalConnection connection_;
  DbError error_;
  bool ready_ = false;
};

}  // namespace database::local
