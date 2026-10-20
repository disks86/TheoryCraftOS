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

#include "Database/Local/SystemSettings.hpp"
#include "Database/NumberText.hpp"

namespace database::local {

void SystemSettings::ensure_table() {
  kernel::ScopedMutex lock(mutex_);
  ready_ = connection_.execute("CREATE TABLE IF NOT EXISTS settings (scope TEXT NOT NULL, name TEXT NOT NULL, value TEXT, PRIMARY KEY (scope, name))") == DbStatus::Ok;
}

bool SystemSettings::get_text(StringView scope, StringView name, String* out) {
  out->clear();
  kernel::ScopedMutex lock(mutex_);
  const DbParam params[] = {DbParam::from_text(scope), DbParam::from_text(name)};
  UniquePointer<IResultSet> rows;
  if (connection_.execute("SELECT value FROM settings WHERE scope = ? AND name = ?", ArrayView<const DbParam>(params, 2), &rows) != DbStatus::Ok) return false;
  if (!rows || rows->row_count() == 0 || rows->is_null(0, 0)) return false;
  return out->assign(rows->text(0, 0));
}

bool SystemSettings::has(StringView scope, StringView name) {
  String ignored(allocator_);
  return get_text(scope, name, &ignored);
}

int64_t SystemSettings::get_int(StringView scope, StringView name, int64_t fallback) {
  String text(allocator_);
  int64_t value;
  return get_text(scope, name, &text) && parse_int64(text.view().trimmed(), &value) ? value : fallback;
}

bool SystemSettings::get_bool(StringView scope, StringView name, bool fallback) {
  String text(allocator_);
  if (!get_text(scope, name, &text)) return fallback;
  const StringView t = text.view().trimmed();
  if (t.equals_ignore_case("true") || t.equals("1") || t.equals_ignore_case("yes") || t.equals_ignore_case("on")) return true;
  if (t.equals_ignore_case("false") || t.equals("0") || t.equals_ignore_case("no") || t.equals_ignore_case("off")) return false;
  return fallback;
}

double SystemSettings::get_real(StringView scope, StringView name, double fallback) {
  String text(allocator_);
  double value;
  return get_text(scope, name, &text) && parse_double(text.view().trimmed(), &value) ? value : fallback;
}

bool SystemSettings::set_text(StringView scope, StringView name, StringView value) {
  kernel::ScopedMutex lock(mutex_);
  if (!ready_) return false;
  const DbParam params[] = {DbParam::from_text(scope), DbParam::from_text(name), DbParam::from_text(value)};
  return connection_.execute("INSERT OR REPLACE INTO settings (scope, name, value) VALUES (?, ?, ?)", ArrayView<const DbParam>(params, 3), nullptr) == DbStatus::Ok;
}

bool SystemSettings::set_int(StringView scope, StringView name, int64_t value) {
  String text(allocator_);
  return text.append_int(value) && set_text(scope, name, text.view());
}

bool SystemSettings::set_real(StringView scope, StringView name, double value) {
  String text(allocator_);
  return append_double(&text, value) && set_text(scope, name, text.view());
}

bool SystemSettings::remove(StringView scope, StringView name) {
  kernel::ScopedMutex lock(mutex_);
  const DbParam params[] = {DbParam::from_text(scope), DbParam::from_text(name)};
  return connection_.execute("DELETE FROM settings WHERE scope = ? AND name = ?", ArrayView<const DbParam>(params, 2), nullptr) == DbStatus::Ok;
}

uint64_t SystemSettings::remove_scope(StringView scope) {
  kernel::ScopedMutex lock(mutex_);
  const DbParam params[] = {DbParam::from_text(scope)};
  UniquePointer<IResultSet> result;
  if (connection_.execute("DELETE FROM settings WHERE scope = ?", ArrayView<const DbParam>(params, 1), &result) != DbStatus::Ok || !result) return 0;
  return result->rows_affected();
}

}  // namespace database::local
