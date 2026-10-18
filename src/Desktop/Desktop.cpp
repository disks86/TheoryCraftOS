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
      file_dialog_(allocator),
      copy_progress_(allocator),
      color_dialog_(allocator),
      print_dialog_(allocator),
      explorer_(allocator),
      task_manager_(allocator),
      notepad_(allocator),
      paint_(allocator),
      terminal_(allocator),
      media_player_(allocator),
      about_(allocator),
      about_lines_{Label(allocator), Label(allocator), Label(allocator)},
      extra_explorers_(allocator),
      extra_notepads_(allocator),
      extra_paints_(allocator),
      extra_terminals_(allocator) {
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
  start_.menu().add_item("Task Manager", {&Desktop::on_task_manager, this});
  start_.menu().add_item("Text Editor", {&Desktop::on_text_editor, this});
  start_.menu().add_item("Terminal", {&Desktop::on_terminal, this});
  start_.menu().add_item("Paint", {&Desktop::on_paint, this});
  start_.menu().add_item("Media Player", {&Desktop::on_media_player, this});
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

  shutdown_dialog_.set_texts("Shut Down", "Shut down TheoryCraftOS now?", "Running services will be stopped.", "Shut Down", "Cancel");
  shutdown_dialog_.on_result({&Desktop::on_shutdown_answer, this});

  media_player_.set_notices(&notice_dialog_);
  media_player_.set_file_dialog(&file_dialog_);
  file_dialog_.set_notices(&notice_dialog_);
  file_dialog_.set_clock(&now_us_);
  setup_notepad(notepad_);
  setup_paint(paint_);
  setup_terminal(terminal_);
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
  if (args.usage == key::kLeftGui || args.usage == key::kRightGui) {
    d->start_.toggle_menu();
    return true;
  }
  return false;
}

void Desktop::set_file_system(filesystem::IFileSystem* fs) {
  window_.locked([&] {
    fs_ = fs;
    explorer_.set_file_system(fs);
    notepad_.set_file_system(fs);
    paint_.set_file_system(fs);
    terminal_.set_file_system(fs);
    for (auto& e : extra_explorers_) e->set_file_system(fs);
    for (auto& n : extra_notepads_) n->set_file_system(fs);
    for (auto& p : extra_paints_) p->set_file_system(fs);
    for (auto& t : extra_terminals_) t->set_file_system(fs);
  });
  media_player_.set_file_system(fs);
  file_dialog_.set_file_system(fs);
}

void Desktop::setup_explorer(FileExplorer& explorer) {
  explorer.set_notices(&notice_dialog_);
  explorer.set_clock(&now_us_);
  explorer.on_file_opened({&Desktop::on_explorer_file, this});
  explorer.on_files_changed({&Desktop::on_files_changed, this});
  explorer.set_transfer_dialog(&copy_progress_);
  explorer.set_file_system(fs_);
}

void Desktop::setup_notepad(Notepad& notepad) {
  notepad.set_notices(&notice_dialog_);
  notepad.set_file_dialog(&file_dialog_);
  notepad.set_print_dialog(&print_dialog_);
  notepad.set_file_system(fs_);
}

void Desktop::setup_terminal(Terminal& terminal) {
  terminal.set_notices(&notice_dialog_);
  terminal.set_file_dialog(&file_dialog_);
  terminal.on_new_terminal({&Desktop::on_new_terminal, this});
  terminal.on_open_path({&Desktop::on_terminal_path, this});
  terminal.set_file_system(fs_);
}

void Desktop::setup_paint(Paint& paint) {
  paint.set_notices(&notice_dialog_);
  paint.set_file_dialog(&file_dialog_);
  paint.set_print_dialog(&print_dialog_);
  paint.set_color_dialog(&color_dialog_);
  paint.set_file_system(fs_);
}

// The first window of a kind is a member of the desktop; when it is already open the next ones are made on the heap, each a little
// lower and to the right of the last so that they do not hide each other, and are destroyed (reap_closed) after they are closed.
template <class T>
T* Desktop::open_instance(T& first, List<UniquePointer<T>>& extras, void (Desktop::*setup)(T&)) {
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
      const Rect2D at = first.bounds();
      const int32_t step = static_cast<int32_t>(open % 8) * 24;
      target->set_bounds({at.x + step, at.y + step, at.width, at.height});
      target->on_closed({&Desktop::on_extra_closed, this});
      if (!extras.push_back(util::move(fresh))) return;
    }
    if (!windows_.add_window(target)) target = nullptr;
  });
  return target;
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
    sweep(extra_terminals_);
  });
}

FileExplorer* Desktop::open_explorer() { return open_instance(explorer_, extra_explorers_, &Desktop::setup_explorer); }

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

void Desktop::open_task_manager() {
  if (task_manager_.manager()) {
    task_manager_.activate();
  } else {
    task_manager_.update(now_us_);
    windows_.add_window(&task_manager_);
  }
}

Notepad* Desktop::open_text_editor() { return open_instance(notepad_, extra_notepads_, &Desktop::setup_notepad); }

Terminal* Desktop::open_terminal() { return open_instance(terminal_, extra_terminals_, &Desktop::setup_terminal); }

uint32_t Desktop::terminal_count() const {
  uint32_t n = terminal_.manager() ? 1 : 0;
  for (const auto& e : extra_terminals_) n += e->manager() ? 1 : 0;
  return n;
}

void Desktop::on_terminal(void* self, Control&) { static_cast<Desktop*>(self)->open_terminal(); }
void Desktop::on_new_terminal(void* self, Control&) { static_cast<Desktop*>(self)->open_terminal(); }

Paint* Desktop::open_paint() { return open_instance(paint_, extra_paints_, &Desktop::setup_paint); }

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

void Desktop::open_media_player() {
  if (media_player_.manager()) media_player_.activate();
  else windows_.add_window(&media_player_);
}

void Desktop::open_about() {
  if (about_.manager()) about_.activate();
  else windows_.add_window(&about_);
}

void Desktop::ask_shut_down() { shutdown_dialog_.show(windows_); }

void Desktop::on_task_manager(void* self, Control&) { static_cast<Desktop*>(self)->open_task_manager(); }
void Desktop::on_paint(void* self, Control&) { static_cast<Desktop*>(self)->open_paint(); }
void Desktop::on_media_player(void* self, Control&) { static_cast<Desktop*>(self)->open_media_player(); }
void Desktop::on_text_editor(void* self, Control&) { static_cast<Desktop*>(self)->open_text_editor(); }
void Desktop::on_explorer_file(void* self, Control& sender) {
  auto* d = static_cast<Desktop*>(self);
  d->open_file(static_cast<FileExplorer&>(sender).opened_file());   // (only explorers fire this)
}

void Desktop::open_file(StringView file) {
  if (Paint::is_picture_name(file)) {  // pictures open in Paint, everything else as text
    if (Paint* paint = paint_for(file)) paint->open_file(file);
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
  if (second != polled_second_) {
    polled_second_ = second;
    if (task_manager_.manager()) task_manager_.update(uptime_us);
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
