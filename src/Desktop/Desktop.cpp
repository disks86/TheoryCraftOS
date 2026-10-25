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

#include "Desktop/Desktop.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;

Desktop::Desktop(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, hardware::IKeyboard* keyboard,
                 hardware::IMouse* mouse)
    : allocator_(allocator),
      window_(allocator, renderer, theme_, renderer.extent()),
      input_(window_, keyboard, mouse),
      render_loop_(window_),
      surface_(allocator),
      taskbar_(allocator),
      start_(allocator),
      gap_left_(allocator),
      gap_right_(allocator),
      windows_(allocator),
      switcher_(allocator, windows_),
      tray_(allocator),
      clock_(allocator),
      shutdown_dialog_(allocator),
      notice_dialog_(allocator),
      toasts_(allocator),
      file_dialog_(allocator),
      copy_progress_(allocator),
      color_dialog_(allocator),
      print_dialog_(allocator),
      explorer_(allocator),
      computer_manager_(allocator),
      notepad_(allocator),
      paint_(allocator),
      spreadsheet_(allocator),
      database_query_(allocator),
      cookie_jar_(allocator),
      local_storage_(allocator),
      page_hub_(allocator),
      cache_store_(allocator),
      service_workers_(allocator),
      web_browser_(allocator),
      terminal_(allocator),
      calculator_(allocator),
      file_properties_(allocator),
      media_player_(allocator),
      graphics_test_(allocator),
      display_confirm_dialog_(allocator),
      display_settings_(allocator),
      network_properties_(allocator),
      about_(allocator),
      about_lines_{Label(allocator), Label(allocator), Label(allocator)},
      database_url_(allocator),
      extra_explorers_(allocator),
      extra_notepads_(allocator),
      extra_paints_(allocator),
      extra_sheets_(allocator),
      extra_queries_(allocator),
      extra_browsers_(allocator),
      extra_terminals_(allocator),
      extra_calculators_(allocator),
      hosts_(allocator) {
  // Taskbar: Start at the left, the clock in a sunken tray at the right, the window switcher in between.
  taskbar_.set_style(PanelStyle::Raised);
  taskbar_.set_dock(DockStyle::Bottom);
  taskbar_.set_size(0, kTaskbarHeight);
  taskbar_.set_padding(Insets::all(4));

  start_.set_text("Start");
  start_.set_dock(DockStyle::Left);
  start_.set_size(76, 0);
  start_.set_direction(MenuButton::Direction::Up);
  start_.set_menu_min_width(200);
  start_.menu().add_item("File Explorer", {&Desktop::on_explorer, this});
  start_.menu().add_item("Computer Manager", {&Desktop::on_computer_manager, this});
  start_.menu().add_item("Text Editor", {&Desktop::on_text_editor, this});
  start_.menu().add_item("Spreadsheet", {&Desktop::on_spreadsheet, this});
  start_.menu().add_item("Database", {&Desktop::on_database_query, this});
  start_.menu().add_item("Web Browser", {&Desktop::on_web_browser, this});
  start_.menu().add_item("Terminal", {&Desktop::on_terminal, this});
  start_.menu().add_item("Calculator", {&Desktop::on_calculator, this});
  start_.menu().add_item("Paint", {&Desktop::on_paint, this});
  start_.menu().add_item("Media Player", {&Desktop::on_media_player, this});
  start_.menu().add_item("3D Test", {&Desktop::on_graphics_test, this});
  start_.menu().add_item("About TheoryCraftOS...", {&Desktop::on_about, this});
  start_.menu().add_separator();
  start_.menu().add_item("Shut Down...", {&Desktop::on_shut_down, this});

  gap_left_.set_style(PanelStyle::Transparent);
  gap_left_.set_dock(DockStyle::Left);
  gap_left_.set_size(6, 0);
  gap_right_.set_style(PanelStyle::Transparent);
  gap_right_.set_dock(DockStyle::Right);
  gap_right_.set_size(6, 0);

  tray_.set_style(PanelStyle::Sunken);
  tray_.set_dock(DockStyle::Right);
  tray_.set_size(180, 0);
  clock_.set_dock(DockStyle::Fill);
  clock_.set_text("--/--/---- --:-- --");
  tray_.add(&clock_);

  switcher_.set_dock(DockStyle::Fill);

  taskbar_.add(&start_);
  taskbar_.add(&gap_left_);
  taskbar_.add(&tray_);
  taskbar_.add(&gap_right_);
  taskbar_.add(&switcher_);  // Fill goes last

  // The window manager fills the desktop surface: windows are stacked above the desktop, below the taskbar.
  windows_.set_dock(DockStyle::Fill);
  surface_.set_dock(DockStyle::Fill);
  surface_.add(&windows_);
  surface_.add(&toasts_);   // over the windows

  shutdown_dialog_.set_texts("Shut Down", "Shut down TheoryCraftOS now?", "Running services will be stopped.", "Shut Down", "Cancel");
  shutdown_dialog_.on_result({&Desktop::on_shutdown_answer, this});

  media_player_.set_app_kind(kAppMedia);
  media_player_.set_notices(&notice_dialog_);
  media_player_.set_file_dialog(&file_dialog_);
  file_dialog_.set_notices(&notice_dialog_);
  file_dialog_.set_clock(&now_us_);
  setup_notepad(notepad_);
  setup_paint(paint_);
  setup_spreadsheet(spreadsheet_);
  setup_database_query(database_query_);
  setup_web_browser(web_browser_);
  service_workers_.set_host(&web_browser_.environment());
  setup_terminal(terminal_);
  setup_calculator(calculator_);
  setup_explorer(explorer_);
  copy_progress_.on_finished({&Desktop::on_copy_finished, this});

  about_.set_title("About TheoryCraftOS");
  about_.set_bounds({140, 90, 400, 170});
  about_.set_padding(Insets::all(12));
  about_.set_minimize_enabled(true);
  static const char* const kAbout[3] = {"TheoryCraftOS", "A minimal UEFI x86_64 operating system.",
                                        "Move, resize, minimize or maximize windows."};
  for (int i = 0; i < 3; ++i) {
    about_lines_[i].set_text(kAbout[i]);
    about_lines_[i].set_dock(DockStyle::Top);
    about_lines_[i].set_size(0, 22);
    about_.add(&about_lines_[i]);
  }

  // Docking order: the strip first, then what fills the rest.
  surface_.set_dock(DockStyle::Fill);
  window_.add(&taskbar_);
  window_.add(&surface_);
  window_.show_cursor(mouse != nullptr);
  display_settings_.set_applier(&Desktop::apply_changes_thunk, this);
  display_settings_.set_confirm_dialog(&display_confirm_dialog_);
  computer_manager_.on_open_display_settings({&Desktop::on_display_settings, this});
  computer_manager_.on_open_network_properties({&Desktop::on_network_properties, this});
  input_.set_confine(&Desktop::confine_pointer, this);
  window_.set_key_filter(&Desktop::on_shortcut, this);
  window_.layout();
}

bool Desktop::on_shortcut(void* self, const KeyArgs& args) {
  namespace key = hardware::key;
  auto* d = static_cast<Desktop*>(self);
  if (d->windows_.has_modal()) return false;   // a dialog wants an answer first
  if (args.usage == key::kTab && (args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt))) {
    d->window_.close_popup();
    d->windows_.cycle(!(args.modifiers & hardware::modifier::kShift));
    return true;
  }
  if (args.usage == key::kTab && (args.modifiers & hardware::modifier::kControl)) {   // Ctrl+Tab: the next tab, whatever the focused program does with it
    if (Frame* active = d->windows_.active())
      if (TabFrame* tabs = active->as_tabs()) {
        tabs->select_next(!(args.modifiers & hardware::modifier::kShift));
        return true;
      }
    return false;
  }
  if (args.usage == key::kLeftGui || args.usage == key::kRightGui) {
    d->start_.toggle_menu();
    return true;
  }
  return false;
}

void Desktop::set_file_system(filesystem::IFileSystem* fs) {
  window_.locked([&] {
    fs_ = fs;
    file_properties_.set_file_system(fs);
    explorer_.set_file_system(fs);
    notepad_.set_file_system(fs);
    paint_.set_file_system(fs);
    spreadsheet_.set_file_system(fs);
    database_query_.set_file_system(fs);
    web_browser_.set_file_system(fs);
    terminal_.set_file_system(fs);
    for (auto& e : extra_explorers_) e->set_file_system(fs);
    for (auto& n : extra_notepads_) n->set_file_system(fs);
    for (auto& p : extra_paints_) p->set_file_system(fs);
    for (auto& s : extra_sheets_) s->set_file_system(fs);
    for (auto& q : extra_queries_) q->set_file_system(fs);
    for (auto& b : extra_browsers_) b->set_file_system(fs);
    for (auto& t : extra_terminals_) t->set_file_system(fs);
  });
  media_player_.set_file_system(fs);
  file_dialog_.set_file_system(fs);
}

void Desktop::setup_explorer(FileExplorer& explorer) {
  explorer.set_app_kind(kAppExplorer);
  explorer.set_notices(&notice_dialog_);
  explorer.set_clock(&now_us_);
  explorer.on_file_opened({&Desktop::on_explorer_file, this});
  explorer.on_files_changed({&Desktop::on_files_changed, this});
  explorer.on_properties({&Desktop::on_explorer_properties, this});
  explorer.set_transfer_dialog(&copy_progress_);
  explorer.set_file_system(fs_);
}

void Desktop::setup_notepad(Notepad& notepad) {
  notepad.set_app_kind(kAppEditor);
  notepad.set_notices(&notice_dialog_);
  notepad.set_file_dialog(&file_dialog_);
  notepad.set_print_dialog(&print_dialog_);
  notepad.set_file_system(fs_);
}

void Desktop::setup_spreadsheet(Spreadsheet& sheet) {
  sheet.set_app_kind(kAppSheet);
  sheet.set_notices(&notice_dialog_);
  sheet.set_file_dialog(&file_dialog_);
  sheet.set_file_system(fs_);
}

void Desktop::setup_database_query(DatabaseQuery& query) {
  query.set_app_kind(kAppDatabase);
  query.set_notices(&notice_dialog_);
  query.set_file_dialog(&file_dialog_);
  query.set_file_system(fs_);
  query.set_databases(databases_);
  query.set_settings(settings_, database_url_.view());
  query.set_clock(&now_us_);
}

void Desktop::setup_web_browser(WebBrowser& web) {
  web.set_app_kind(kAppBrowser);
  web.set_notices(&notice_dialog_);
  web.set_file_dialog(&file_dialog_);
  web.set_file_system(fs_);
  web.set_page_loader(page_loader_);
  web.set_cookie_jar(&cookie_jar_);
  web.set_shared_hubs(&local_storage_, &page_hub_);
  web.set_service_workers(&service_workers_, &cache_store_);
  web.set_clock(&Desktop::unix_ms, this);
  web.set_media_services(media_mixer_, media_codecs_, media_clock_);
  web.set_open_window_handler(&Desktop::open_script_window, this);
}

// window.open() of a page: another browser tab next to the page's (or a window of its own), showing the address. It may close itself with window.close().
bool Desktop::open_script_window(void* self, WebBrowser& from, StringView address) {
  auto* d = static_cast<Desktop*>(self);
  Frame* host = from.host();
  WebBrowser* opened = d->open_web_browser(host ? host->as_tabs() : nullptr);
  if (!opened) return false;
  opened->set_opened_by_script(true);
  return opened->navigate(address);
}

// The local time of the taskbar clock as milliseconds since 1970 (what a script's Date.now() says; 0 without a clock).
uint64_t Desktop::unix_ms(void* self) {
  auto* d = static_cast<Desktop*>(self);
  DateTime t;
  if (!d->clock_source_ || !d->clock_source_(d->clock_context_, &t)) return 0;
  int64_t y = t.year, m = t.month;
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  return static_cast<uint64_t>(days * 86400 + t.hour * 3600 + t.minute * 60) * 1000;
}

void Desktop::set_page_loader(browser::IPageLoader* loader) {
  window_.locked([&] {
    page_loader_ = loader;
    web_browser_.set_page_loader(loader);
    for (auto& b : extra_browsers_) b->set_page_loader(loader);
  });
}

void Desktop::set_databases(database::DatabaseManager* databases) {
  window_.locked([&] {
    databases_ = databases;
    database_query_.set_databases(databases);
    for (auto& q : extra_queries_) q->set_databases(databases);
  });
}

void Desktop::set_settings(database::local::SystemSettings* settings, StringView database_url) {
  window_.locked([&] {
    settings_ = settings;
    sessions_ = settings ? UniquePointer<SessionStore>::make(allocator_, allocator_, settings->database()) : UniquePointer<SessionStore>();
    browser_store_ = settings ? UniquePointer<BrowserStore>::make(allocator_, allocator_, settings->database()) : UniquePointer<BrowserStore>();
    database_url_.assign(database_url);
    database_query_.set_settings(settings, database_url_.view());
    for (auto& q : extra_queries_) q->set_settings(settings, database_url_.view());
  });
}

void Desktop::setup_terminal(Terminal& terminal) {
  terminal.set_app_kind(kAppTerminal);
  terminal.set_notices(&notice_dialog_);
  terminal.set_file_dialog(&file_dialog_);
  terminal.on_new_terminal({&Desktop::on_new_terminal, this});
  terminal.on_open_path({&Desktop::on_terminal_path, this});
  terminal.set_file_system(fs_);
}

void Desktop::setup_calculator(Calculator& calculator) { calculator.set_app_kind(kAppCalculator); }

void Desktop::setup_paint(Paint& paint) {
  paint.set_app_kind(kAppPaint);
  paint.set_notices(&notice_dialog_);
  paint.set_file_dialog(&file_dialog_);
  paint.set_print_dialog(&print_dialog_);
  paint.set_color_dialog(&color_dialog_);
  paint.set_file_system(fs_);
}

// The first window of a kind is a member of the desktop; when it is already open the next ones are made on the heap, each a little
// lower and to the right of the last so that they do not hide each other, and are destroyed (reap_closed) after they are closed.
template <class T>
T* Desktop::open_instance(T& first, List<UniquePointer<T>>& extras, void (Desktop::*setup)(T&), TabFrame* into) {
  T* target = nullptr;
  window_.locked([&] {
    if (!first.manager()) {
      target = &first;
    } else {
      uint32_t open = 1;
      for (auto& e : extras) open += e->manager() ? 1 : 0;
      UniquePointer<T> fresh = UniquePointer<T>::make(allocator_, allocator_);
      if (!fresh) return;
      target = fresh.get();
      (this->*setup)(*target);
      const Rect2D at = first.home_bounds().width ? first.home_bounds() : first.bounds();
      const int32_t step = static_cast<int32_t>(open % 8) * 24;
      target->set_bounds({at.x + step, at.y + step, at.width, at.height});
      target->on_closed({&Desktop::on_extra_closed, this});
      if (!extras.push_back(util::move(fresh))) {
        target = nullptr;
        return;
      }
    }
    if (!place(target, into)) target = nullptr;
  });
  return target;
}

TabFrame* Desktop::new_host() {
  UniquePointer<TabFrame> fresh = UniquePointer<TabFrame>::make(allocator_, allocator_);
  if (!fresh) return nullptr;
  TabFrame* host = fresh.get();
  host->on_new_tab({&Desktop::on_new_tab, this});
  host->on_tab_torn_off({&Desktop::on_tab_torn_off, this});
  host->on_closed({&Desktop::on_extra_closed, this});
  if (!hosts_.push_back(util::move(fresh))) return nullptr;
  return host;
}

bool Desktop::place(Frame* tab, TabFrame* into) {
  bool placed = false;
  window_.locked([&] {
    if (into) {
      placed = into->add_tab(tab);
      return;
    }
    TabFrame* host = new_host();
    if (!host) return;
    const Rect2D b = tab->home_bounds().width ? tab->home_bounds() : tab->bounds();
    Rect2D where{0, 0, 0, 0};
    if (windows_.area_count() > 0) where = windows_.dialog_area();   // several monitors: it opens on the one in use
    host->set_bounds({b.x + where.x, b.y + where.y, b.width, b.height + static_cast<uint32_t>(TabFrame::kStripHeight)});
    if (!host->add_tab(tab)) return;
    placed = windows_.add_window(host);
    if (!placed) host->remove_tab(tab);
  });
  return placed;
}

// The "+" of a tab strip: another tab of the program the window shows.
void Desktop::on_new_tab(void* self, Control& sender) {
  auto* d = static_cast<Desktop*>(self);
  TabFrame* host = static_cast<TabFrame*>(&sender);
  const uint8_t kind = host->selected() ? host->selected()->app_kind() : static_cast<uint8_t>(kAppExplorer);
  switch (kind) {
    case kAppEditor: d->open_text_editor(host); break;
    case kAppPaint: d->open_paint(host); break;
    case kAppSheet: d->open_spreadsheet(host); break;
    case kAppDatabase: d->open_database_query(host); break;
    case kAppBrowser: d->open_web_browser(host); break;
    case kAppTerminal: d->open_terminal(host); break;
    case kAppCalculator: d->open_calculator(host); break;
    case kAppMedia: d->open_media_player(host); break;
    default: d->open_explorer(host); break;
  }
}

// A tab was dragged out of its window onto the desktop: it gets a window of its own where the pointer was let go.
void Desktop::on_tab_torn_off(void* self, Control& sender) {
  auto* d = static_cast<Desktop*>(self);
  TabFrame* host = static_cast<TabFrame*>(&sender);
  d->window_.locked([&] {
    Frame* tab = host->torn_tab();
    if (!tab || tab->host() != host || host->tab_count() < 2) return;
    const Point at = host->torn_point();
    const Rect2D size = host->bounds();
    host->remove_tab(tab);
    TabFrame* fresh = d->new_host();
    if (fresh) {
      fresh->set_bounds({at.x - 60, at.y - 12, size.width, size.height});
      if (fresh->add_tab(tab) && d->windows_.add_window(fresh)) return;
      fresh->remove_tab(tab);
    }
    host->add_tab(tab);   // (no memory: it stays where it was)
  });
}

void Desktop::on_extra_closed(void* self, Control&) { __atomic_store_n(&static_cast<Desktop*>(self)->reap_needed_, true, __ATOMIC_RELEASE); }

void Desktop::reap_closed() {
  window_.locked([&] {
    auto sweep = [](auto& pool) {
      for (uint64_t i = pool.count(); i > 0; --i)
        if (!pool[i - 1]->manager()) pool.remove_at(i - 1);
    };
    sweep(extra_explorers_);
    sweep(extra_notepads_);
    sweep(extra_paints_);
    sweep(extra_sheets_);
    sweep(extra_queries_);
    sweep(extra_browsers_);
    sweep(extra_terminals_);
    sweep(extra_calculators_);
    sweep(hosts_);
  });
}

FileExplorer* Desktop::open_explorer(TabFrame* into) { return open_instance(explorer_, extra_explorers_, &Desktop::setup_explorer, into); }

void Desktop::refresh_explorers() {
  window_.locked([&] {
    if (explorer_.manager()) explorer_.refresh();
    for (auto& e : extra_explorers_)
      if (e->manager()) e->refresh();
  });
}

uint32_t Desktop::explorer_count() const {
  uint32_t n = explorer_.manager() ? 1 : 0;
  for (const auto& e : extra_explorers_) n += e->manager() ? 1 : 0;
  return n;
}
uint32_t Desktop::text_editor_count() const {
  uint32_t n = notepad_.manager() ? 1 : 0;
  for (const auto& e : extra_notepads_) n += e->manager() ? 1 : 0;
  return n;
}
uint32_t Desktop::paint_count() const {
  uint32_t n = paint_.manager() ? 1 : 0;
  for (const auto& e : extra_paints_) n += e->manager() ? 1 : 0;
  return n;
}

void Desktop::open_computer_manager() {
  if (computer_manager_.manager()) {
    computer_manager_.activate();
  } else {
    computer_manager_.update(now_us_);
    windows_.add_window(&computer_manager_);
  }
}

Notepad* Desktop::open_text_editor(TabFrame* into) { return open_instance(notepad_, extra_notepads_, &Desktop::setup_notepad, into); }

Terminal* Desktop::open_terminal(TabFrame* into) { return open_instance(terminal_, extra_terminals_, &Desktop::setup_terminal, into); }

uint32_t Desktop::terminal_count() const {
  uint32_t n = terminal_.manager() ? 1 : 0;
  for (const auto& e : extra_terminals_) n += e->manager() ? 1 : 0;
  return n;
}

Calculator* Desktop::open_calculator(TabFrame* into) { return open_instance(calculator_, extra_calculators_, &Desktop::setup_calculator, into); }

uint32_t Desktop::calculator_count() const {
  uint32_t n = calculator_.manager() ? 1 : 0;
  for (const auto& e : extra_calculators_) n += e->manager() ? 1 : 0;
  return n;
}

void Desktop::on_calculator(void* self, Control&) { static_cast<Desktop*>(self)->open_calculator(); }

void Desktop::on_terminal(void* self, Control&) { static_cast<Desktop*>(self)->open_terminal(); }
void Desktop::on_new_terminal(void* self, Control&) { static_cast<Desktop*>(self)->open_terminal(); }

Paint* Desktop::open_paint(TabFrame* into) { return open_instance(paint_, extra_paints_, &Desktop::setup_paint, into); }

Spreadsheet* Desktop::open_spreadsheet(TabFrame* into) { return open_instance(spreadsheet_, extra_sheets_, &Desktop::setup_spreadsheet, into); }

DatabaseQuery* Desktop::open_database_query(TabFrame* into) { return open_instance(database_query_, extra_queries_, &Desktop::setup_database_query, into); }

WebBrowser* Desktop::open_web_browser(TabFrame* into) { return open_instance(web_browser_, extra_browsers_, &Desktop::setup_web_browser, into); }

uint32_t Desktop::web_browser_count() const {
  uint32_t n = web_browser_.manager() ? 1 : 0;
  for (const auto& e : extra_browsers_) n += e->manager() ? 1 : 0;
  return n;
}

uint32_t Desktop::database_query_count() const {
  uint32_t n = database_query_.manager() ? 1 : 0;
  for (const auto& e : extra_queries_) n += e->manager() ? 1 : 0;
  return n;
}

// The query window to open a SQL file in: the one that already shows it, an open empty one, or a new one.
DatabaseQuery* Desktop::database_query_for(StringView path) {
  DatabaseQuery* found = nullptr;
  window_.locked([&] {
    DatabaseQuery* blank = nullptr;
    auto look = [&](DatabaseQuery& q) {
      if (!q.manager()) return;
      if (q.path().equals(path)) found = &q;
      else if (!blank && q.path().empty() && !q.editor().modified() && q.editor().text().empty()) blank = &q;
    };
    look(database_query_);
    for (auto& e : extra_queries_) look(*e);
    if (!found) found = blank ? blank : open_database_query();
    if (found) found->activate();
  });
  return found;
}

uint32_t Desktop::spreadsheet_count() const {
  uint32_t n = spreadsheet_.manager() ? 1 : 0;
  for (const auto& e : extra_sheets_) n += e->manager() ? 1 : 0;
  return n;
}

// The spreadsheet window to open a file in: the one that already shows it, an open empty one, or a new one.
Spreadsheet* Desktop::spreadsheet_for(StringView path) {
  Spreadsheet* found = nullptr;
  window_.locked([&] {
    Spreadsheet* blank = nullptr;
    auto look = [&](Spreadsheet& s) {
      if (!s.manager()) return;
      uint32_t columns, rows;
      s.sheet().used_extent(&columns, &rows);
      if (s.path().equals(path)) found = &s;
      else if (!blank && s.path().empty() && !s.sheet().modified() && columns == 0) blank = &s;
    };
    look(spreadsheet_);
    for (auto& e : extra_sheets_) look(*e);
    if (!found) found = blank ? blank : open_spreadsheet();
    if (found) found->activate();
  });
  return found;
}

Notepad* Desktop::editor_for(StringView path) {
  Notepad* found = nullptr;
  window_.locked([&] {
    Notepad* empty = nullptr;
    auto look = [&](Notepad& n) {
      if (!n.manager()) return;
      if (n.path().equals(path)) found = &n;
      else if (!empty && n.path().empty() && !n.editor().modified() && n.editor().text().empty()) empty = &n;
    };
    look(notepad_);
    for (auto& e : extra_notepads_) look(*e);
    if (!found) found = empty ? empty : open_text_editor();
    if (found) found->activate();
  });
  return found;
}

Paint* Desktop::paint_for(StringView path) {
  Paint* found = nullptr;
  window_.locked([&] {
    Paint* blank = nullptr;
    auto look = [&](Paint& p) {
      if (!p.manager()) return;
      if (p.path().equals(path)) found = &p;
      else if (!blank && p.path().empty() && !p.canvas().modified()) blank = &p;
    };
    look(paint_);
    for (auto& e : extra_paints_) look(*e);
    if (!found) found = blank ? blank : open_paint();
    if (found) found->activate();
  });
  return found;
}

void Desktop::open_media_player(TabFrame* into) {
  window_.locked([&] {
    if (media_player_.manager()) media_player_.activate();
    else place(&media_player_, into);
  });
}

void Desktop::open_graphics_test() {
  window_.locked([&] {
    if (graphics_test_.manager()) graphics_test_.activate();
    else windows_.add_window(&graphics_test_);
  });
}

void Desktop::open_display_settings() {
  window_.locked([&] {
    display_settings_.refresh();
    if (display_settings_.manager()) display_settings_.activate();
    else windows_.add_window(&display_settings_);
  });
}

void Desktop::open_network_properties(uint32_t adapter) {
  window_.locked([&] {
    network_properties_.show_adapter(adapter);
    if (network_properties_.manager()) network_properties_.activate();
    else windows_.add_window(&network_properties_);
  });
}

void Desktop::on_network_properties(void* self, Control&) {
  auto* d = static_cast<Desktop*>(self);
  d->open_network_properties(d->computer_manager_.network_adapter());
}

void Desktop::open_file_properties(StringView path) {
  window_.locked([&] {
    file_properties_.show(path);
    if (file_properties_.manager()) file_properties_.activate();
    else windows_.add_window(&file_properties_);
  });
}

void Desktop::on_explorer_properties(void* self, Control& sender) {
  auto* d = static_cast<Desktop*>(self);
  String path(d->allocator_);
  path.assign(static_cast<FileExplorer&>(sender).properties_path());
  d->open_file_properties(path.view());
}

void Desktop::on_display_settings(void* self, Control&) { static_cast<Desktop*>(self)->open_display_settings(); }
bool Desktop::apply_changes_thunk(void* self) { return static_cast<Desktop*>(self)->apply_monitor_changes(); }

// --- Monitors ---------------------------------------------------------------------------------------

void Desktop::set_monitors(hardware::SystemMonitors* monitors) {
  monitors_ = monitors;
  display_settings_.set_monitors(monitors);
  monitors_changed();
}

bool Desktop::apply_monitor_changes() {
  if (!monitors_) return false;
  bool ok = true;
  if (changer_) {
    const bool rendering = render_loop_.running();
    if (rendering) render_loop_.stop();   // (nothing may draw while the renderer is replaced)
    graphics::IRenderer2D* renderer = changer_(changer_context_);
    ok = renderer != nullptr;
    if (ok) window_.set_renderer(*renderer);
    if (rendering) render_loop_.start();
  } else {
    monitors_->reapply();
  }
  monitors_changed();
  // The pointer goes to the middle of the primary monitor if it was left outside the new desktop.
  const hardware::MonitorSlot* primary = monitors_->layout().primary();
  const gui::Point at = input_.cursor();
  if (primary && monitors_->layout().monitor_at(at.x, at.y) < 0)
    input_.set_cursor(primary->rect.x + static_cast<int32_t>(primary->rect.width / 2), primary->rect.y + static_cast<int32_t>(primary->rect.height / 2));
  else
    input_.set_cursor(at.x, at.y);   // (kept inside what is there now)
  window_.mark_dirty();
  return ok;
}

void Desktop::monitors_changed() {
  if (!monitors_) return;
  window_.locked([&] {
    using hardware::MonitorRect;
    const hardware::MonitorLayout& layout = monitors_->layout();
    const bool several = layout.count() > 1;
    // The pointer may be on any monitor that is there.
    {
      kernel::ScopedMutex guard(pointer_lock_);
      monitor_rect_count_ = 0;
      for (uint32_t i = 0; i < layout.count(); ++i)
        if (layout.at(i).enabled) monitor_rects_[monitor_rect_count_++] = layout.at(i).rect;
    }
    if (!several) {
      toasts_.clear_area();
      display_settings_.refresh();
      return;
    }
    if (!taskbar_in_surface_) {   // the taskbar is placed by hand, above the windows (the docked arrangement is for one screen)
      window_.remove(&taskbar_);
      taskbar_.set_dock(DockStyle::None);
      surface_.add(&taskbar_);
      taskbar_in_surface_ = true;
    }
    const hardware::MonitorSlot* primary = layout.primary();
    gui::Rect2D regions[hardware::MonitorLayout::kMaxMonitors], areas[hardware::MonitorLayout::kMaxMonitors];
    uint32_t count = 0;
    auto to_rect = [](const MonitorRect& r) { return gui::Rect2D{r.x, r.y, r.width, r.height}; };
    if (primary) {
      regions[count] = to_rect(primary->rect);
      areas[count] = regions[count];
      areas[count].height = areas[count].height > static_cast<uint32_t>(kTaskbarHeight) ? areas[count].height - static_cast<uint32_t>(kTaskbarHeight) : 0;
      taskbar_.set_bounds({primary->rect.x, primary->rect.bottom() - kTaskbarHeight, primary->rect.width, static_cast<uint32_t>(kTaskbarHeight)});
      ++count;
    }
    for (uint32_t i = 0; i < layout.count(); ++i) {
      const hardware::MonitorSlot& s = layout.at(i);
      if (!s.enabled || &s == primary) continue;
      regions[count] = to_rect(s.rect);
      areas[count] = regions[count];
      ++count;
    }
    surface_.set_regions(regions, count);
    windows_.set_areas(areas, count);
    if (count) toasts_.set_area(areas[0]);   // the primary monitor above its taskbar
    display_settings_.refresh();
  });
}

// Keeps the pointer on a monitor. (Runs on the thread that delivers input.)
void Desktop::confine_pointer(void* self, int32_t* x, int32_t* y) {
  auto* d = static_cast<Desktop*>(self);
  kernel::ScopedMutex guard(d->pointer_lock_);
  const uint32_t n = d->monitor_rect_count_;
  if (n == 0) return;
  int32_t best = -1;
  uint64_t best_distance = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const hardware::MonitorRect& r = d->monitor_rects_[i];
    if (r.contains(*x, *y)) return;
    const int32_t cx = *x < r.x ? r.x : *x >= r.right() ? r.right() - 1 : *x;
    const int32_t cy = *y < r.y ? r.y : *y >= r.bottom() ? r.bottom() - 1 : *y;
    const int64_t dx = int64_t{*x} - cx, dy = int64_t{*y} - cy;
    const uint64_t distance = static_cast<uint64_t>(dx * dx + dy * dy);
    if (best < 0 || distance < best_distance) {
      best = static_cast<int32_t>(i);
      best_distance = distance;
    }
  }
  const hardware::MonitorRect& r = d->monitor_rects_[best];
  *x = *x < r.x ? r.x : *x >= r.right() ? r.right() - 1 : *x;
  *y = *y < r.y ? r.y : *y >= r.bottom() ? r.bottom() - 1 : *y;
}

void Desktop::open_about() {
  if (about_.manager()) about_.activate();
  else windows_.add_window(&about_);
}

void Desktop::ask_shut_down() { shutdown_dialog_.show(windows_); }

void Desktop::on_computer_manager(void* self, Control&) { static_cast<Desktop*>(self)->open_computer_manager(); }
void Desktop::on_paint(void* self, Control&) { static_cast<Desktop*>(self)->open_paint(); }
void Desktop::on_media_player(void* self, Control&) { static_cast<Desktop*>(self)->open_media_player(); }
void Desktop::on_graphics_test(void* self, Control&) { static_cast<Desktop*>(self)->open_graphics_test(); }
void Desktop::on_text_editor(void* self, Control&) { static_cast<Desktop*>(self)->open_text_editor(); }
void Desktop::on_spreadsheet(void* self, Control&) { static_cast<Desktop*>(self)->open_spreadsheet(); }
void Desktop::on_database_query(void* self, Control&) { static_cast<Desktop*>(self)->open_database_query(); }
void Desktop::on_web_browser(void* self, Control&) { static_cast<Desktop*>(self)->open_web_browser(); }
void Desktop::on_explorer_file(void* self, Control& sender) {
  auto* d = static_cast<Desktop*>(self);
  d->open_file(static_cast<FileExplorer&>(sender).opened_file());   // (only explorers fire this)
}

void Desktop::open_file(StringView file) {
  if (Paint::is_picture_name(file)) {  // pictures open in Paint, everything else as text
    if (Paint* paint = paint_for(file)) paint->open_file(file);
    return;
  }
  if (DatabaseQuery::is_sql_name(file)) {   // SQL files open in the database query window
    if (DatabaseQuery* query = database_query_for(file)) query->open_file(file);
    return;
  }
  if (Spreadsheet::is_sheet_name(file)) {   // CSV files open in the spreadsheet
    if (Spreadsheet* sheet = spreadsheet_for(file)) sheet->open_file(file);
    return;
  }
  if (MediaPlayer::is_media_name(file)) {   // sound and video open in the media player
    open_media_player();
    media_player_.open_file(file);
    return;
  }
  if (Notepad* editor = editor_for(file)) editor->open_file(file);
}

bool Desktop::open_path(StringView path) {
  if (!fs_) return false;
  filesystem::DirectoryEntry entry;
  const bool root = path.equals("/");
  if (!root && fs_->stat(path, &entry) != filesystem::Status::Ok) return false;
  if (!root && entry.type != filesystem::NodeType::Directory) {
    open_file(path);
    return true;
  }
  FileExplorer* target = nullptr;
  window_.locked([&] {
    if (explorer_.manager()) target = &explorer_;
    for (auto& e : extra_explorers_)
      if (!target && e->manager()) target = e.get();
    if (!target) target = open_explorer();
    if (target) target->activate();
  });
  return target && target->open_folder(path);
}

void Desktop::on_terminal_path(void* self, Control& sender) {
  static_cast<Desktop*>(self)->open_path(static_cast<Terminal&>(sender).opened_path());
}
Desktop::~Desktop() {
  copy_progress_.cancel();
  copy_progress_.wait_until_idle(30000000);
  window_.wait_for_background(30000000);
  service_workers_.stop_all();   // (the workers borrow the main browser's network: they go before it)
  service_workers_.set_host(nullptr);
}

void Desktop::on_copy_finished(void* self, Control&) {
  auto* d = static_cast<Desktop*>(self);
  d->refresh_explorers();
  if (!d->copy_progress_.message().empty()) d->notice_dialog_.show(d->windows_, NoticeKind::Error, d->copy_progress_.message());
}
void Desktop::on_files_changed(void* self, Control&) { static_cast<Desktop*>(self)->refresh_explorers(); }
void Desktop::on_explorer(void* self, Control&) { static_cast<Desktop*>(self)->open_explorer(); }
void Desktop::on_about(void* self, Control&) { static_cast<Desktop*>(self)->open_about(); }
void Desktop::on_shut_down(void* self, Control&) { static_cast<Desktop*>(self)->ask_shut_down(); }

void Desktop::on_shutdown_answer(void* self, Control&) {
  auto* d = static_cast<Desktop*>(self);
  if (d->shutdown_dialog_.accepted()) d->request_shutdown();
}

uint32_t format_date_time(const DateTime& t, char* out) {
  auto two = [&](uint32_t at, uint32_t v) {
    out[at] = static_cast<char>('0' + (v / 10) % 10);
    out[at + 1] = static_cast<char>('0' + v % 10);
  };
  uint32_t hour12 = t.hour % 12;
  if (hour12 == 0) hour12 = 12;
  two(0, t.month);
  out[2] = '/';
  two(3, t.day);
  out[5] = '/';
  two(6, t.year / 100);
  two(8, t.year % 100);
  out[10] = ' ';
  two(11, hour12);
  out[13] = ':';
  two(14, t.minute);
  out[16] = ' ';
  out[17] = t.hour < 12 ? 'A' : 'P';
  out[18] = 'M';
  out[19] = 0;
  return 19;
}

bool Desktop::start() {
  const bool handlers = window_.start_handler_thread();
  const bool rendering = render_loop_.start();
  if (handlers && rendering) return true;
  stop();
  return false;
}

void Desktop::stop() {
  render_loop_.stop();
  window_.stop_handler_thread();
}

bool Desktop::step(uint64_t uptime_us) {
  pump(uptime_us);
  if (!window_.dirty()) return false;
  window_.render();
  return true;
}

void Desktop::pump(uint64_t uptime_us) {
  __atomic_store_n(&now_us_, uptime_us, __ATOMIC_RELAXED);
  input_.pump();
  web_browser_.tick(uptime_us);
  toasts_.tick(uptime_us);
  if (browser_store_ && browser_data_restored_ && uptime_us - last_browser_save_us_ > 30000000ull) {   // (the data of the browser is saved when it changed, at most every half minute)
    last_browser_save_us_ = uptime_us;
    if (cookie_jar_.version() != saved_cookie_version_ || local_storage_.version() != saved_storage_version_ || cache_store_.version() != saved_cache_version_ ||
        service_workers_.version() != saved_worker_version_)
      save_browser_data();
  }
  for (uint64_t i = 0; i < extra_browsers_.count(); ++i) extra_browsers_[i]->tick(uptime_us);
  if (__atomic_exchange_n(&reap_needed_, false, __ATOMIC_ACQ_REL)) reap_closed();
  // Watchdog: a changed window that the render thread has not redrawn for seconds means that thread (or
  // whatever it is waiting for - the GPU, the UI lock) is stuck. Say so where it can be read afterwards.
  if (render_loop_.running() && diagnostic_) {
    const uint32_t frames = render_loop_.frames();
    if (!watching_ || frames != last_frames_ || !window_.dirty()) {
      watching_ = true;
      last_frames_ = frames;
      stall_since_us_ = uptime_us;
    } else if (uptime_us - stall_since_us_ > 3000000 && uptime_us - stall_reported_us_ > 5000000) {
      stall_reported_us_ = uptime_us;
      char text[64] = "desktop: no frame drawn for    s although the window changed\n";
      const uint64_t seconds = (uptime_us - stall_since_us_) / 1000000;
      text[26] = seconds >= 100 ? static_cast<char>('0' + (seconds / 100) % 10) : ' ';
      text[27] = seconds >= 10 ? static_cast<char>('0' + (seconds / 10) % 10) : ' ';
      text[28] = static_cast<char>('0' + seconds % 10);
      diagnostic_(text);
    }
  }
  const uint64_t second = uptime_us / 1000000;
  if (monitors_ && second != polled_monitors_second_) {
    polled_monitors_second_ = second;
    if (monitors_->poll()) apply_monitor_changes();
  }
  if (second != polled_second_) {
    polled_second_ = second;
    if (network_properties_.manager()) network_properties_.update();
    display_settings_.tick();
    if (computer_manager_.manager()) computer_manager_.update(uptime_us);
    DateTime now;
    const bool ok = clock_source_ && clock_source_(clock_context_, &now);
    // The label only changes when the minute does; ~0 stands for "no time".
    const uint64_t packed = ok ? ((((uint64_t{now.year} * 13 + now.month) * 32 + now.day) * 24 + now.hour) * 60 + now.minute) : ~0ull - 1;
    if (packed != shown_) {
      shown_ = packed;
      char text[24];
      if (ok) {
        clock_.set_text(StringView(text, format_date_time(now, text)));
      } else {
        clock_.set_text("--/--/---- --:-- --");
      }
    }
  }
}

}  // namespace desktop
