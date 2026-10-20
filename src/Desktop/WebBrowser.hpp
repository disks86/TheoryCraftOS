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

// The web browser: a window with a menu bar (File: Open File, Close; Go: Back, Forward, Reload), a bar with the Back, Forward and Reload buttons, the
// address box and Go, the page, and a status bar that says what is happening (and where the link under the pointer leads). It shows HTML pages (see
// browser::HtmlDocument and browser::StyleSheet for what is understood - text, headings, lists, tables, links, and the CSS that colours, spaces and
// aligns them, from <style>, style attributes and linked stylesheets, and the scripts of the page (see script::Engine; they run while the page is read,
// write into it with document.write, set its title, and print to the script log; there is no DOM to change afterwards); no pictures) and plain text, from the web
// (http:// and https://, through the page loader the desktop gives it) and from the system's own volumes (a path such as /docs/index.html).
// Enter in the address box, or Go, opens what is typed; Ctrl+L goes to the address box, Alt+Left and Alt+Right are Back and Forward, F5 and Ctrl+R
// reload, Ctrl+O opens a file. Clicking a link opens it; a page that links to another by a relative address works in files as it does on the web.
// A file dragged here from a file explorer is opened.
//
// Pages are fetched on the window's handler thread (without one - in tests - right away), so the desktop stays alive while a server thinks; opening
// something while a page is still loading replaces the request that has not started yet.
#pragma once
#include "Browser/IPageLoader.hpp"
#include "Script/Engine.hpp"
#include "Desktop/FileDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/PageView.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class WebBrowser : public gui::Frame {
 public:
  static constexpr uint64_t kMaxFileBytes = 4u << 20;
  static constexpr uint32_t kMaxHistory = 100;
  static constexpr uint32_t kMaxStyleSheets = 8;
  static constexpr uint64_t kMaxStyleSheetBytes = 512u << 10;
  static constexpr uint32_t kMaxExternalScripts = 12;
  static constexpr uint64_t kMaxScriptBytes = 1u << 20;
  // The time of day in milliseconds since 1970 for Date (0 if there is none).
  using UnixClock = uint64_t (*)(void* context);

  explicit WebBrowser(allocators::IAllocator& allocator);

  // Does the file name say it is a web page (.html, .htm)?
  static bool is_page_name(StringView name);

  // The filesystem local pages are read from (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  // The dialog that File > Open uses (nullptr: none). It must outlive the window.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }
  // Where web pages come from (nullptr: none, web addresses then say so). It must outlive the window.
  void set_page_loader(browser::IPageLoader* loader) { loader_ = loader; }
  void set_clock(UnixClock clock, void* context) { unix_clock_ = clock; unix_clock_context_ = context; }
  // Whether the scripts of pages are run (on by default).
  void set_scripts_enabled(bool enabled) { scripts_enabled_ = enabled; }
  bool scripts_enabled() const { return scripts_enabled_; }
  // What the scripts of the last page printed (console.log and so on, one line each, errors included) and how many of them failed.
  StringView script_log() const { return script_log_.view(); }
  uint32_t script_errors() const { return script_errors_; }

  // Opens what was typed as an address (see browser::normalize_address). False if it is empty. The page is fetched on the handler thread: when
  // this returns the request is only made (without a handler thread the page is already shown).
  bool navigate(StringView typed);
  bool go_back();
  bool go_forward();
  bool reload();
  bool can_go_back() const { return index_ > 0; }
  bool can_go_forward() const { return index_ + 1 < static_cast<int64_t>(history_.count()); }
  // Puts an address in the address box without opening it (a restored session shows it so; the user presses Go).
  void show_address(StringView address);
  // The address of the page shown ("" before the first one).
  StringView address() const { return current_.view(); }
  bool loading() const { return loading_; }

  gui::PageView& page() { return page_; }
  gui::TextBox& address_box() { return address_; }
  gui::Button& back_button() { return back_; }
  gui::Button& forward_button() { return forward_; }
  gui::Button& reload_button() { return reload_; }
  gui::Button& go_button() { return go_; }
  gui::Label& status_label() { return status_; }
  gui::MenuBar& menu_bar() { return menu_; }
  void ask_open();

  bool accepts_drop(StringView text, int32_t, int32_t, const gui::Control*) const override { return !text.empty() && text[0] == '/'; }
  void on_drop(StringView text, int32_t, int32_t, const gui::Control*) override { navigate(text); }
  bool on_key(const gui::KeyArgs& args) override;

 private:
  enum class Request : uint8_t { None, Go, Back, Forward, Reload };
  struct Fetched;
  friend class PageScripts;

  static void on_back(void* self, gui::Control&);
  static void on_forward(void* self, gui::Control&);
  static void on_reload(void* self, gui::Control&);
  static void on_go(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_load(void* self, gui::Control&);
  static void on_link(void* self, gui::Control&);
  static void on_hover(void* self, gui::Control&);
  bool post(Request kind, StringView target);
  void run_request();
  void load(Request kind, StringView target);
  bool fetch(StringView address, Fetched* page);
  bool fetch_file(StringView path, Fetched* page);
  void show(Fetched& page);
  void remember(Request kind, StringView address);
  void update_buttons();
  void update_title(StringView page_title);
  void say(StringView text);
  void notify(gui::NoticeKind kind, StringView text) {
    if (!gui::NoticeDialog::post(notices_, manager(), kind, text)) say(text);
  }

  gui::MenuBar menu_;
  gui::Panel toolbar_, status_bar_;
  gui::Button back_, forward_, reload_, go_;
  gui::TextBox address_;
  gui::Label status_;
  gui::PageView page_;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  gui::NoticeDialog* notices_ = nullptr;
  browser::IPageLoader* loader_ = nullptr;
  gui::Handler load_handler_;
  UnixClock unix_clock_ = nullptr;
  void* unix_clock_context_ = nullptr;
  bool scripts_enabled_ = true;
  uint32_t script_errors_ = 0;
  String script_log_;
  List<String> history_;
  int64_t index_ = -1;
  String current_, request_target_, status_text_;
  Request request_ = Request::None;
  bool loading_ = false;
};

}  // namespace desktop
