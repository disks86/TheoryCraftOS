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

// Remembering the windows across a shutdown: save_session() writes what is open to the system database (see SessionStore), restore_session() opens
// it all again, in the same places, with the same tabs and the files they had open.
#include "Desktop/Desktop.hpp"
#include "Desktop/SessionStore.hpp"

namespace desktop {

using gui::Frame;
using gui::TabFrame;

namespace {

struct KindName {
  uint8_t kind;
  const char* name;
};
const KindName kKinds[] = {
    {Desktop::kAppExplorer, "explorer"}, {Desktop::kAppEditor, "editor"}, {Desktop::kAppPaint, "paint"},   {Desktop::kAppTerminal, "terminal"},
    {Desktop::kAppMedia, "media"},       {Desktop::kAppSheet, "sheet"},   {Desktop::kAppDatabase, "database"}, {Desktop::kAppBrowser, "browser"},
};

const char* kind_name(uint8_t kind) {
  for (const KindName& k : kKinds)
    if (k.kind == kind) return k.name;
  return nullptr;
}

uint8_t kind_from_name(StringView name) {
  for (const KindName& k : kKinds)
    if (name.equals(k.name)) return k.kind;
  return Desktop::kAppNone;
}

const char* state_name(Frame::State state) {
  return state == Frame::State::Maximized ? "maximized" : state == Frame::State::Minimized ? "minimized" : "normal";
}
Frame::State state_from_name(StringView name) {
  return name.equals("maximized") ? Frame::State::Maximized : name.equals("minimized") ? Frame::State::Minimized : Frame::State::Normal;
}

const char* dock_name(Frame::Dock dock) {
  switch (dock) {
    case Frame::Dock::Left: return "left";
    case Frame::Dock::Right: return "right";
    case Frame::Dock::Top: return "top";
    case Frame::Dock::Bottom: return "bottom";
    default: return "none";   // (a tiled window is remembered as a free one)
  }
}
Frame::Dock dock_from_name(StringView name) {
  return name.equals("left") ? Frame::Dock::Left : name.equals("right") ? Frame::Dock::Right : name.equals("top") ? Frame::Dock::Top : name.equals("bottom") ? Frame::Dock::Bottom : Frame::Dock::None;
}

// Files on network shares are not reopened at start-up: the network may not be there yet, and asking for a login or waiting for a server is not
// what a restart should do.
bool on_network_share(StringView path) {
  StringView p = path;
  while (p.starts_with('/')) p = p.without_prefix(1);
  const uint64_t slash = p.find('/');
  const StringView first = slash == StringView::npos ? p : p.first(slash);
  return first.equals_ignore_case(FileExplorer::kNetworkFolder) || first.equals_ignore_case(FileExplorer::kNfsFolder) || first.equals_ignore_case(FileExplorer::kSftpFolder) ||
         first.equals_ignore_case(FileExplorer::kFtpsFolder);
}

// The file or folder a tab shows ("" if none or not worth remembering).
StringView tab_path(Frame* tab) {
  StringView path;
  switch (tab->app_kind()) {
    case Desktop::kAppExplorer: path = static_cast<FileExplorer*>(tab)->path(); break;
    case Desktop::kAppEditor: path = static_cast<Notepad*>(tab)->path(); break;
    case Desktop::kAppPaint: path = static_cast<Paint*>(tab)->path(); break;
    case Desktop::kAppSheet: path = static_cast<Spreadsheet*>(tab)->path(); break;
    case Desktop::kAppDatabase: path = static_cast<DatabaseQuery*>(tab)->path(); break;
    case Desktop::kAppBrowser: path = static_cast<WebBrowser*>(tab)->address(); break;
    default: break;   // the media player would start playing; a terminal has no file
  }
  return on_network_share(path) ? StringView() : path;
}

}  // namespace

uint32_t Desktop::save_session() {
  if (!sessions_) return 0;
  uint32_t saved = 0;
  window_.locked([&] {
    // The windows from the bottom of the stack up, so that opening them in this order stacks them the same way.
    List<Frame*> stack(allocator_);
    for (uint32_t i = 0; i < windows_.window_count(); ++i) {
      Frame* f = windows_.window_at(i);
      if (!f || f->modal() || !stack.push_back(f)) continue;
      for (uint64_t k = stack.count() - 1; k > 0 && windows_.stacking_index(stack[k]) < windows_.stacking_index(stack[k - 1]); --k) {
        Frame* swap = stack[k];
        stack[k] = stack[k - 1];
        stack[k - 1] = swap;
      }
    }
    if (!sessions_->begin()) return;
    bool ok = true;
    uint32_t count = 0;
    for (uint64_t i = 0; i < stack.count() && ok; ++i) {
      Frame* f = stack[i];
      TabFrame* tabs = f->as_tabs();
      const char* kind = nullptr;
      if (tabs && tabs->tab_count() > 0) kind = "tabs";
      else if (f == &computer_manager_) kind = "computer-manager";
      else if (f == &graphics_test_) kind = "graphics-test";
      if (!kind) continue;   // dialogs and the like
      const gui::Rect2D b = f->normal_bounds();
      const int32_t selected = tabs ? tabs->index_of(tabs->selected()) : 0;
      ok = sessions_->add_window(kind, b.x, b.y, b.width, b.height, state_name(f->state()), state_name(f->state_before_minimize()), dock_name(f->docked()),
                                 selected < 0 ? 0 : static_cast<uint32_t>(selected));
      for (uint32_t t = 0; ok && tabs && t < tabs->tab_count(); ++t) {
        Frame* tab = tabs->tab_at(t);
        const char* name = kind_name(tab->app_kind());
        if (name) ok = sessions_->add_tab(count, t, name, tab_path(tab));
      }
      ++count;
    }
    if (sessions_->end(ok) && ok) saved = count;
  });
  return saved;
}

namespace {
// Puts a window where it was: its normal place first, then maximized, docked or minimized the way it was.
void apply_geometry(Frame* window, const gui::Rect2D& bounds, Frame::State state, Frame::State before, Frame::Dock dock) {
  window->set_bounds(bounds);
  const Frame::State shown = state == Frame::State::Minimized ? before : state;
  if (shown == Frame::State::Maximized) window->maximize();
  else if (dock != Frame::Dock::None) window->dock(dock);
  if (state == Frame::State::Minimized) window->minimize();
}
}  // namespace

uint32_t Desktop::restore_session() {
  if (!sessions_ || (settings_ && !settings_->get_bool("desktop", "restore_session", true))) return 0;
  UniquePointer<database::IResultSet> windows, tabs;
  if (!sessions_->read(&windows, &tabs) || !windows || !tabs) return 0;
  uint32_t restored = 0;
  window_.locked([&] {
    uint64_t next_tab = 0;
    for (uint64_t w = 0; w < windows->row_count(); ++w) {
      int64_t id = 0, x = 0, y = 0, width = 0, height = 0, selected = 0;
      windows->get_int(w, SessionStore::kWinId, &id);
      windows->get_int(w, SessionStore::kWinX, &x);
      windows->get_int(w, SessionStore::kWinY, &y);
      windows->get_int(w, SessionStore::kWinWidth, &width);
      windows->get_int(w, SessionStore::kWinHeight, &height);
      windows->get_int(w, SessionStore::kWinSelected, &selected);
      if (width < Frame::kMinWidth || height < Frame::kMinHeight || width > 32000 || height > 32000 || x < -32000 || x > 32000 || y < -32000 || y > 32000) width = 0;   // nonsense: the tabs are still opened
      const gui::Rect2D bounds{static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
      const Frame::State state = state_from_name(windows->text(w, SessionStore::kWinState));
      const Frame::State before = state_from_name(windows->text(w, SessionStore::kWinBefore));
      const Frame::Dock dock = dock_from_name(windows->text(w, SessionStore::kWinDock));
      const StringView kind = windows->text(w, SessionStore::kWinKind);
      Frame* window = nullptr;
      if (kind.equals("computer-manager")) {
        open_computer_manager();
        window = &computer_manager_;
      } else if (kind.equals("graphics-test")) {
        open_graphics_test();
        window = &graphics_test_;
      } else if (kind.equals("tabs")) {
        TabFrame* host = nullptr;
        for (; next_tab < tabs->row_count(); ++next_tab) {   // the tabs of this window (the rows come in window order)
          int64_t owner = 0;
          tabs->get_int(next_tab, SessionStore::kTabWindow, &owner);
          if (owner < id) continue;
          if (owner > id) break;
          const uint8_t app = kind_from_name(tabs->text(next_tab, SessionStore::kTabKind));
          const StringView path = tabs->text(next_tab, SessionStore::kTabPath);
          Frame* tab = open_app(app, host, path);
          if (tab && !host) host = static_cast<TabFrame*>(tab->host());
        }
        if (host && selected >= 0 && selected < host->tab_count()) host->select_tab(host->tab_at(static_cast<uint32_t>(selected)));
        window = host;
      }
      if (!window) continue;
      if (width > 0) apply_geometry(window, bounds, state, before, dock);
      ++restored;
    }
  });
  return restored;
}

// Opens a program window of kind `app` (as a tab of `into`, or in a window of its own), showing `path` when that is a file or folder that is still
// there. Returns the window of the program, or nullptr.
Frame* Desktop::open_app(uint8_t app, TabFrame* into, StringView path) {
  filesystem::DirectoryEntry entry;
  const bool exists = !path.empty() && fs_ && fs_->stat(path, &entry) == filesystem::Status::Ok;
  const bool is_file = exists && entry.type == filesystem::NodeType::File;
  const bool is_folder = exists && entry.type == filesystem::NodeType::Directory;
  switch (app) {
    case kAppExplorer: {
      FileExplorer* e = open_explorer(into);
      if (e && is_folder) e->open_folder(path);
      return e;
    }
    case kAppEditor: {
      Notepad* n = open_text_editor(into);
      if (n && is_file) n->open_file(path);
      return n;
    }
    case kAppPaint: {
      Paint* p = open_paint(into);
      if (p && is_file) p->open_file(path);
      return p;
    }
    case kAppSheet: {
      Spreadsheet* s = open_spreadsheet(into);
      if (s && is_file) s->open_file(path);
      return s;
    }
    case kAppDatabase: {
      DatabaseQuery* q = open_database_query(into);
      if (q && is_file) q->open_file(path);
      return q;
    }
    case kAppBrowser: {
      WebBrowser* w = open_web_browser(into);
      if (w && is_file) w->navigate(path);   // (a web address is only shown: the network may not be up yet, and a restart should not go and fetch pages)
      else if (w && !path.empty()) w->show_address(path);
      return w;
    }
    case kAppTerminal: return open_terminal(into);
    case kAppMedia:   // (the file is not played again: the window just comes back)
      open_media_player(into);
      return &media_player_;
    default: return nullptr;
  }
}

}  // namespace desktop
