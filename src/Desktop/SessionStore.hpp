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

// What was open on the desktop when the system last shut down, kept in the system database so that the next start can open it again: the windows
// with their places and states (in stacking order, bottom first), and the tabs of each with the file or folder it showed.
//
//   session_windows (id INTEGER PRIMARY KEY, kind TEXT, x, y, width, height INTEGER, state TEXT, before TEXT, dock TEXT, selected INTEGER)
//   session_tabs    (window INTEGER, position INTEGER, kind TEXT, path TEXT, PRIMARY KEY (window, position))
//
// A window that is maximized, docked or minimized keeps in x, y, width and height the place it returns to. Saving replaces the whole session in
// one transaction; reading gives the two tables in the order they are to be opened.
#pragma once
#include "Database/Local/LocalDatabase.hpp"

namespace desktop {

class SessionStore {
 public:
  // The columns of the answers of read().
  enum WindowColumn : uint32_t { kWinId, kWinKind, kWinX, kWinY, kWinWidth, kWinHeight, kWinState, kWinBefore, kWinDock, kWinSelected };
  enum TabColumn : uint32_t { kTabWindow, kTabPosition, kTabKind, kTabPath };

  SessionStore(allocators::IAllocator& allocator, database::local::LocalDatabase& database) : allocator_(allocator), connection_(allocator, database) {}

  // Starts a new session: the old one is dropped (when end(true) comes). False if the database is busy or cannot be written.
  bool begin();
  // Adds a window; windows must come bottom first. `state` and `before` are "normal", "maximized" or "minimized" (before: what a minimized one returns
  // to), `dock` is "none", "left", "right", "top" or "bottom".
  bool add_window(StringView kind, int32_t x, int32_t y, uint32_t width, uint32_t height, StringView state, StringView before, StringView dock, uint32_t selected);
  bool add_tab(uint32_t window, uint32_t position, StringView kind, StringView path);
  // Keeps (true) or drops (false) what was added since begin().
  bool end(bool keep);

  // The saved session; false if there is none (or it cannot be read).
  bool read(UniquePointer<database::IResultSet>* windows, UniquePointer<database::IResultSet>* tabs);

 private:
  bool ensure_tables();
  bool run(StringView sql, ArrayView<const database::DbParam> params = ArrayView<const database::DbParam>());

  allocators::IAllocator& allocator_;
  database::local::LocalConnection connection_;
  uint32_t next_window_ = 0;
  bool open_ = false;
};

}  // namespace desktop
