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

#include "Desktop/SessionStore.hpp"

namespace desktop {

using database::DbParam;
using database::DbStatus;

bool SessionStore::run(StringView sql, ArrayView<const DbParam> params) { return connection_.execute(sql, params, nullptr) == DbStatus::Ok; }

bool SessionStore::ensure_tables() {
  return run("CREATE TABLE IF NOT EXISTS session_windows (id INTEGER PRIMARY KEY, kind TEXT NOT NULL, x INTEGER, y INTEGER, width INTEGER, height INTEGER, "
             "state TEXT, before TEXT, dock TEXT, selected INTEGER)") &&
         run("CREATE TABLE IF NOT EXISTS session_tabs (window INTEGER NOT NULL, position INTEGER NOT NULL, kind TEXT NOT NULL, path TEXT, PRIMARY KEY (window, position))");
}

bool SessionStore::begin() {
  if (open_) end(false);
  if (!ensure_tables() || connection_.begin() != DbStatus::Ok) return false;
  open_ = true;
  next_window_ = 0;
  if (!run("DELETE FROM session_tabs") || !run("DELETE FROM session_windows")) {
    end(false);
    return false;
  }
  return true;
}

bool SessionStore::add_window(StringView kind, int32_t x, int32_t y, uint32_t width, uint32_t height, StringView state, StringView before, StringView dock, uint32_t selected) {
  if (!open_) return false;
  const DbParam params[] = {DbParam::from_int(next_window_), DbParam::from_text(kind), DbParam::from_int(x), DbParam::from_int(y), DbParam::from_int(width), DbParam::from_int(height),
                            DbParam::from_text(state), DbParam::from_text(before), DbParam::from_text(dock), DbParam::from_int(selected)};
  if (!run("INSERT INTO session_windows VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", ArrayView<const DbParam>(params, 10))) return false;
  ++next_window_;
  return true;
}

bool SessionStore::add_tab(uint32_t window, uint32_t position, StringView kind, StringView path) {
  if (!open_) return false;
  const DbParam params[] = {DbParam::from_int(window), DbParam::from_int(position), DbParam::from_text(kind), path.empty() ? DbParam::null() : DbParam::from_text(path)};
  return run("INSERT INTO session_tabs VALUES (?, ?, ?, ?)", ArrayView<const DbParam>(params, 4));
}

bool SessionStore::end(bool keep) {
  if (!open_) return false;
  open_ = false;
  return keep ? connection_.commit() == DbStatus::Ok : connection_.rollback() == DbStatus::Ok;
}

bool SessionStore::read(UniquePointer<database::IResultSet>* windows, UniquePointer<database::IResultSet>* tabs) {
  return ensure_tables() && connection_.execute("SELECT id, kind, x, y, width, height, state, before, dock, selected FROM session_windows ORDER BY id", windows) == DbStatus::Ok &&
         connection_.execute("SELECT window, position, kind, path FROM session_tabs ORDER BY window, position", tabs) == DbStatus::Ok;
}

}  // namespace desktop
