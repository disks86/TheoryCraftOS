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

// The web browser: a window with a menu bar (File: Open File, Close; Go: Back, Forward, Reload; View: Scripts, Dark Colors), a bar with the Back, Forward
// and Reload buttons, the address box and Go, the page, and a status bar that says what is happening (and where the link under the pointer leads). It
// shows HTML pages as browser::Page loads them - a document tree (see dom::Document) that scripts can read and change (see browser::DomBindings), styled
// by CSS from <style>, style attributes and linked stylesheets, with selectors and @media queries answered for the size of the window and the colour
// scheme, drawn as text, headings, lists, tables and links, with its pictures (<img>: PNG, JPEG, GIF - animated too -, BMP, WebP lossless and more), videos (<video>: AVI, MP4 / MOV with H.264, GIF, APNG, Y4M, with sound and a control bar, on the machine's video hardware when it has some) and canvases (<canvas> with the 2D context, drawn by the GPU when the machine has one) in the places the layout gives them - and plain text, from the web (http:// and https://, through the page loader the
// desktop gives it) and from the system's own volumes (a path such as /docs/index.html).
// A click on the page goes to the page as a click event (and follows a link when no script stops it); timers run from tick(); when a script changes the
// page, or the window is resized, the page is drawn again where it was scrolled to.
// Enter in the address box, or Go, opens what is typed; Ctrl+L goes to the address box, Alt+Left and Alt+Right are Back and Forward, F5 and Ctrl+R
// reload, Ctrl+O opens a file. A file dragged here from a file explorer is opened.
//
// Pages are fetched on the window's handler thread (without one - in tests - right away), so the desktop stays alive while a server thinks; opening
// something while a page is still loading replaces the request that has not started yet. Everything that touches the loaded page runs on that thread.
#pragma once
#include "Browser/IPageLoader.hpp"
#include "Browser/PageHub.hpp"
#include "Browser/SessionHistory.hpp"
#include "Browser/Page.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Audio/AudioMixer.hpp"
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
  ~WebBrowser();

  // Does the file name say it is a web page (.html, .htm)?
  static bool is_page_name(StringView name);

  // The filesystem local pages are read from (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  // The dialog that File > Open uses (nullptr: none). It must outlive the window.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }
  // Where web pages come from (nullptr: none, web addresses then say so). It must outlive the window.
  void set_page_loader(browser::IPageLoader* loader) { loader_ = loader; }
  // The cookies the browser keeps, to share between its windows (without one, the window keeps its own).
  void set_cookie_jar(browser::CookieJar* jar) { shared_jar_ = jar; }
  browser::CookieJar& jar() { return shared_jar_ ? *shared_jar_ : own_jar_; }
  // The localStorage of the browser and the hub that carries events between its pages, to share between its windows (without them the window keeps its own).
  void set_shared_hubs(browser::StorageHub* local, browser::PageHub* hub) {
    shared_local_ = local;
    shared_hub_ = hub;
  }
  void set_clock(UnixClock clock, void* context) { unix_clock_ = clock; unix_clock_context_ = context; }
  // Whether the scripts of pages are run (on by default); the next page loaded (or Reload) takes it.
  void set_scripts_enabled(bool enabled);
  // Whether pages are told the user prefers dark colours (prefers-color-scheme); the page shown follows right away.
  void set_dark(bool dark);
  bool dark() const { return dark_; }
  // The machine's sound mixer, video codecs and clock, for the <video> elements of pages (any may be nullptr: videos then show their poster and say they cannot
  // be played). They must outlive the window.
  void set_media_services(audio::AudioMixer* mixer, hardware::SystemVideoCodecs* codecs, const hardware::IClock* clock) {
    mixer_ = mixer;
    codecs_ = codecs;
    clock_ = clock;
  }
  // Is the canvas of the page drawn by the GPU (as of the last time it was shown)?
  bool canvas_accelerated() const { return canvas_accelerated_; }
  // Called by the desktop's loop with the uptime: runs the timers of the page that are due (on the handler thread).
  void tick(uint64_t uptime_us);
  // The page loaded now (nullptr for text, and before the first page). Use it only from the handler thread.
  browser::Page* live_page() { return live_.get(); }
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
  bool can_go_back() const { return history_.can_go(-1); }
  bool can_go_forward() const { return history_.can_go(1); }
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
  enum class Request : uint8_t { None, Go, Back, Forward, Reload, Submit };
  struct Fetched;

  // What the page asks of the window.
  class Env final : public browser::IPageEnvironment {
   public:
    Env(WebBrowser& owner, allocators::IAllocator& allocator);
    bool fetch_text(StringView address, uint64_t limit, String* text) override;
    bool fetch_bytes(StringView address, uint64_t limit, ByteBuffer* out) override;
    UniquePointer<browser::IVideoClip> open_video(UniquePointer<ByteBuffer> data) override;
    UniquePointer<browser::IVideoClip> open_audio(UniquePointer<ByteBuffer> data) override;
    void navigate(StringView address, bool replace) override;
    void navigate_post(StringView address, StringView content_type, StringView body) override;
    bool clipboard_get(String* text) override;
    void clipboard_set(StringView text) override;
    void pick_file(uint32_t node_serial, bool multiple) override;
    void set_hash(StringView hash) override;
    void reload() override;
    void history_go(int32_t delta) override;
    uint32_t history_length() override;
    void push_state(StringView address, StringView state_json, bool has_state, bool replace) override;
    void alert(StringView text) override;
    bool confirm(StringView) override { return false; }
    void scroll_to(int32_t line) override;
    uint64_t now_ms() override;
    void console(int, StringView) override {}
    browser::StorageHub& local_storage_hub() override;
    browser::StorageHub& session_storage_hub() override { return session_; }
    browser::PageHub& page_hub() override;
    uint64_t tab_id() override { return tab_; }
    browser::CookieJar& cookies() override;
    bool http_request(const browser::FetchOptions& options, browser::NetResponse* response) override;
    UniquePointer<browser::IWebSocket> open_websocket(StringView url, StringView protocols, StringView origin, String* error) override;

   private:
    WebBrowser& owner_;
    browser::StorageHub session_;   // this tab's sessionStorage
    uint64_t tab_;
  };
  friend class Env;

  static void on_back(void* self, gui::Control&);
  static void on_forward(void* self, gui::Control&);
  static void on_reload(void* self, gui::Control&);
  static void on_go(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_file_picked(void* self, gui::Control&);
  void ask_file(uint32_t node_serial);
  static void on_close(void* self, gui::Control&);
  static void on_load(void* self, gui::Control&);
  static void on_link(void* self, gui::Control&);
  static void on_hover(void* self, gui::Control&);
  static void on_node_click(void* self, gui::Control&);
  static void on_pointer(void* self, gui::Control&);
  static void on_form_key(void* self, gui::Control&);
  static void on_view_resized(void* self, gui::Control&);
  static void on_timers(void* self, gui::Control&);
  static void on_toggle_scripts(void* self, gui::Control&);
  static void on_toggle_dark(void* self, gui::Control&);
  void update_media();
  void settle();                       // after the page ran something: follow what it asked for, draw it again if it changed
  void refresh(bool keep_scroll);      // draws the loaded page again
  void present(UniquePointer<browser::HtmlDocument> doc, bool keep_scroll, uint32_t http_status);
  void schedule_timers();
  void apply_hash(StringView hash);
  uint64_t page_clock_ms();
  browser::MediaEnvironment current_media();
  bool post(Request kind, StringView target);
  void run_request();
  void load(Request kind, StringView target);
  bool post_form(StringView target, StringView content_type, StringView body);
  // `subresource`: a style sheet, script or picture that a page asked for (otherwise the page itself is being opened).
  bool fetch(StringView address, Fetched* page, uint64_t limit = kMaxFileBytes, bool subresource = false);
  bool fetch_post(StringView address, Fetched* page);
  void update_key_capture();
  bool fetch_file(StringView path, Fetched* page, uint64_t limit = kMaxFileBytes);
  void show(Fetched& page);
  void remember(Request kind, StringView address);
  // Back / Forward to an entry of the document that is shown: nothing is loaded, the page gets a popstate.
  bool traverse_in_place(int32_t delta);
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
  browser::CookieJar* shared_jar_ = nullptr;
  browser::StorageHub* shared_local_ = nullptr;
  browser::PageHub* shared_hub_ = nullptr;
  browser::StorageHub own_local_;
  browser::PageHub own_hub_;
  int32_t request_delta_ = 0;       // how far Back / Forward goes in the history
  uint32_t document_ = 0;           // the number of the document shown (it changes whenever a page is loaded)
  gui::Handler load_handler_;
  UnixClock unix_clock_ = nullptr;
  void* unix_clock_context_ = nullptr;
  audio::AudioMixer* mixer_ = nullptr;
  hardware::SystemVideoCodecs* codecs_ = nullptr;
  const hardware::IClock* clock_ = nullptr;
  bool canvas_accelerated_ = false;
  bool scripts_enabled_ = true, dark_ = false;
  gui::MenuItem *scripts_item_ = nullptr, *dark_item_ = nullptr;
  gui::Handler pointer_handler_, node_handler_, resize_handler_, timer_handler_, key_handler_;
  Env env_;
  UniquePointer<browser::Page> live_;
  browser::MediaEnvironment media_;
  // what the page asked for while it ran, done when it has returned
  bool defer_reload_ = false;
  int32_t defer_history_ = 0, defer_scroll_ = -1;
  String defer_go_;
  bool want_go_ = false, want_post_ = false;
  uint32_t pending_file_node_ = 0;   // the file input a picked file is for
  String defer_type_, defer_body_, request_type_, request_body_;
  uint64_t uptime_ms_ = 0, clock_base_unix_ = 0, clock_base_up_ = 0, next_due_ms_ = 0;
  bool clock_based_ = false, timer_pending_ = false;
  uint32_t reported_errors_ = 0;
  uint32_t script_errors_ = 0;
  String script_log_;
  browser::SessionHistory history_;
  browser::CookieJar own_jar_;   // (the cookies, when the window is not given a jar to share)
  String current_, request_target_, status_text_;
  Request request_ = Request::None;
  bool loading_ = false;
};

}  // namespace desktop
