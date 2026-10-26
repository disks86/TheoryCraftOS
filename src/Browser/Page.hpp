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

// A web page that has been loaded: its document tree, its style sheets, its scripts (an Engine with the DOM installed), and everything that happens to it
// after the first drawing - clicks, timers, a window of another size, a changed colour scheme. The browser window creates a Page for each HTML page it
// shows, asks it to draw itself into an HtmlDocument whenever changed() says the tree or the display changed, and tells it what the user does. Pages
// reach the outside through an IPageEnvironment. All of it is used from one thread (the browser's handler thread).
//
//   Page page(allocator, environment, "http://example.com/", media_environment, /*scripts*/ true);
//   page.load(html);                    // reads the tree, runs the scripts, fires DOMContentLoaded and load
//   page.render(&document);             // fetches the style sheets it links to, and fills `document` with what the page looks like
//   page.click(serial, x, y);           // a click on the node a run came from (see Run::node)
//   if (page.changed()) page.render(&document);
#pragma once
#include "Browser/CookieJar.hpp"
#include "Browser/DomBindings.hpp"
#include "Browser/IRtcService.hpp"
#include "Browser/HtmlDocument.hpp"
#include "Browser/PageHub.hpp"
#include "Browser/PagePicture.hpp"
#include "Browser/Media.hpp"
#include "Browser/Objects.hpp"
#include "Browser/Renderer.hpp"
#include "Dom/Dom.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "Font/FontSet.hpp"
#include "Script/Engine.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

// What a page asks of the browser it is shown in.
class PageObjectEnv;

class IPageEnvironment : public IObjectEnvironment {
 public:
  // The service workers of the browser and the caches of the Cache API, shared by all its pages (nullptr: the browser has none; a page then keeps caches of its own).
  virtual ServiceWorkerHub* service_workers() { return nullptr; }
  virtual CacheStore* cache_store() { return nullptr; }
  // WebRTC (nullptr: the browser has none).
  virtual IRtcService* rtc_service() { return nullptr; }
  // The text at an address (a script or a style sheet), at most `limit` bytes. False if it cannot be had.
  virtual bool fetch_text(StringView address, uint64_t limit, String* text) = 0;
  // The page is going to ask for `address` (a script, style sheet or picture) soon: start getting it, without waiting. Optional; what it does not do the later
  // fetch_text() / fetch_bytes() does.
  virtual void prefetch(StringView address) { (void)address; }
  virtual void navigate(StringView address, bool replace) = 0;
  virtual void set_hash(StringView hash) = 0;
  virtual void reload() = 0;
  virtual void history_go(int32_t delta) = 0;
  virtual uint32_t history_length() = 0;
  // history.pushState() / replaceState() from the page (the browser adds or changes the history entry), and the state of the entry the page is on (JSON text).
  virtual void push_state(StringView address, StringView state_json, bool has_state, bool replace) = 0;
  virtual bool history_state(String* json) { (void)json; return false; }
  // The session history for the Navigation API: a JSON array of {"key","id","url","state"} for the entries of the tab, and the index of the current one. False if
  // the browser has none. The navigation state of the current entry is kept apart from history.state; traverseTo() names an entry by its key.
  virtual bool history_entries(String* json, int64_t* index) { (void)json; (void)index; return false; }
  virtual void set_navigation_state(StringView json, bool has_state) { (void)json; (void)has_state; }
  virtual void history_go_to_key(uint64_t key) { (void)key; }
  // window.open(): a new window (tab) for `address`; `target` is the name it was given ("" or _blank: always a new one). False if the browser cannot or will not (a
  // pop-up blocker). close_window() is window.close() of a page that was opened by a script.
  virtual bool open_window(StringView address, StringView target, bool noopener) { (void)address; (void)target; (void)noopener; return false; }
  virtual void close_window() {}
  virtual void alert(StringView text) = 0;
  virtual bool confirm(StringView text) = 0;
  virtual void scroll_to(int32_t y) = 0;   // the page asked for a vertical position, in CSS pixels
  // The Fullscreen API: can the window go fullscreen, and make it so (true) or put it back (false). Returns whether the window did. The window may also leave
  // fullscreen by itself (Escape, another page): Page::fullscreen_left() then says so.
  virtual bool fullscreen_supported() { return false; }
  virtual bool set_fullscreen(bool on) { (void)on; return false; }
  // Picture-in-Picture: can the browser show a video in a window of its own; open that window for the video with this serial (objects.paint_pip() draws it), `width` x
  // `height` pixels; close it. When the user closes it or resizes it, Page::pip_left() / pip_resized() say so.
  virtual bool pip_supported() { return false; }
  virtual bool open_pip(uint32_t video_serial, uint32_t width, uint32_t height, PageObjects& objects) { (void)video_serial; (void)width; (void)height; (void)objects; return false; }
  virtual void close_pip() {}
  // Notifications and Push (the service worker hub keeps the permissions, the notifications and the push subscriptions; the browser shows them to the user).
  // Asks the user whether `origin` may show notifications: 1 allowed, 0 blocked, -1 not answered yet (the browser then calls ServiceWorkerHub::set_notification_permission()
  // when the user has answered). Without a user to ask, a page is blocked.
  virtual int notification_prompt(StringView origin) { (void)origin; return 0; }
  // Shows a notice to the user (`sticky`: until the user deals with it; `actions`: the texts of its buttons, one on each line, at most two). Returns its number (0: it cannot be shown). When the user clicks it or sends it away the browser calls
  // ServiceWorkerHub::notification_activated() (with the button, if one was) / notification_closed() with that number.
  virtual uint32_t notification_show(StringView origin, StringView title, StringView body, bool sticky, StringView actions) {
    (void)origin; (void)title; (void)body; (void)sticky; (void)actions;
    return 0;
  }
  virtual void notification_close(uint32_t number) { (void)number; }
  virtual uint64_t now_ms() = 0;                 // milliseconds since 1970 (0 without a clock)
  virtual void console(int level, StringView text) = 0;
  // Web Storage: the hub of localStorage (all origins, all windows) and the one of this tab's sessionStorage; and what the pages share to talk to each other.
  virtual StorageHub& local_storage_hub() = 0;
  virtual StorageHub& session_storage_hub() = 0;
  virtual PageHub& page_hub() = 0;
  virtual uint64_t tab_id() { return 1; }   // (tells the pages of one tab from those of another: they share a sessionStorage)
  // The cookies of the browser (shared by all its pages), and the network for fetch() and XMLHttpRequest.
  virtual CookieJar& cookies() = 0;
  // May be called on worker threads, several at a time.
  virtual bool http_request(const FetchOptions& options, NetResponse* response) {
    (void)options;
    response->error.assign("This browser cannot make requests.");
    return false;
  }
  // The answer to http_request() reaches the script (on the thread that runs the page).
  virtual void http_answered(const NetResponse& response) { (void)response; }
  // A WebSocket for a page (see IPageLoader::open_websocket).
  virtual UniquePointer<IWebSocket> open_websocket(StringView url, StringView protocols, StringView origin, String* error) {
    (void)url;
    (void)protocols;
    (void)origin;
    error->assign("This browser cannot open WebSockets.");
    return UniquePointer<IWebSocket>();
  }
  // A form was submitted with method=post: the page at `address` is the answer to `body` (of that content type). Without this, the browser has no way to
  // post, and the form is sent as a GET.
  virtual void navigate_post(StringView address, StringView content_type, StringView body) { (void)content_type; (void)body; navigate(address, false); }
  // The clipboard, for Ctrl+C / X / V in a text field. (False / nothing without one.)
  virtual bool clipboard_get(String* text) { (void)text; return false; }
  virtual void clipboard_set(StringView text) { (void)text; }
  // The same for more than text (see IDomHost). The defaults know only text.
  virtual void clipboard_formats(String* csv) {
    String text(csv->allocator());
    csv->clear();
    if (clipboard_get(&text) && !text.empty()) csv->assign("text/plain");
  }
  virtual bool clipboard_read_format(StringView format, String* bytes) { return format.equals("text/plain") && clipboard_get(bytes); }
  virtual bool clipboard_write_items(const ClipboardData& data) {
    if (!data.has_text) return false;
    clipboard_set(data.text);
    return true;
  }
  // The user clicked a file input (the node with that serial): let them choose; Page::file_chosen() is called for each file they pick. (Nothing without a file dialog.)
  virtual void pick_file(uint32_t node_serial, bool multiple) { (void)node_serial; (void)multiple; }
  // (IObjectEnvironment: fetch_bytes() for pictures and videos, open_video(); pages without pictures need neither)
  bool fetch_bytes(StringView, uint64_t, ByteBuffer*) override { return false; }

 protected:
  IPageEnvironment() = default;
  ~IPageEnvironment() = default;
};

class Page final : public script::IHost, public IDomHost, public dom::IScriptRunner, public IStyleSource, public IFrameSource {
 public:
  static constexpr uint32_t kMaxExternalScripts = 12, kMaxStyleSheets = 8;
  static constexpr uint64_t kMaxScriptBytes = 1u << 20, kMaxStyleSheetBytes = 512u << 10;

  Page(allocators::IAllocator& allocator, IPageEnvironment& environment, StringView address, const MediaEnvironment& media, bool scripts);
  ~Page();
  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  // Reads the page: the tree, the scripts (when enabled), then readyState "interactive", DOMContentLoaded, "complete" and load. False if memory ran out.
  bool load(StringView html);
  // Fetches the style sheets the page links to that have not been fetched, and draws the page into `out`.
  bool render(HtmlDocument* out);
  // The same as render(), as pixels: lays the page out for the display's width in CSS pixels (media().width) with `fonts` and builds what is painted into `out`.
  // `fonts` is for this thread's measuring: the painter uses a font set of its own.
  bool render_picture(PagePicture* out, font::FontSet& fonts);
  // Has the tree changed, or the display (set_media()), since the last render()?
  bool changed() const;
  // Did something visible change that is not in the tree (a canvas drawn on, a video frame)? Then the view only needs painting again.
  bool take_visual_change() { return objects_.take_dirty(); }

  // The display changed (the window was resized, the colour scheme was switched): the page's numbers and its matchMedia() lists follow, and `resize`
  // fires. A style sheet's @media rules are read again by the next render().
  void set_media(const MediaEnvironment& media);
  // A click on a node (Run::node): the click event goes to it and up; if nothing stops it, a link is followed. Without scripts only the link is. When the
  // node is a picture, video or canvas, `object` is its ObjectKind and (lx, ly) where in its w x h rectangle the click was: a video with controls plays or
  // pauses.
  void click(uint32_t node_serial, int32_t x, int32_t y, uint8_t object = 0, int32_t lx = 0, int32_t ly = 0, uint32_t w = 0, uint32_t h = 0);
  // The pointer is over the node with that serial (0: over none), pressed or not: the node and its ancestors take :hover (and :active), and the page is drawn again
  // if a style rule depends on that.
  void set_pointer(uint32_t node_serial, bool pressed);
  // Moves pictures and videos along (animated pictures, playing videos) and fires the events that follow (load, error, timeupdate, ended ...); true if
  // anything visible changed or an event was fired. Called often while objects().animating().
  bool step_objects(double now_ms);
  PageObjects& objects() override { return objects_; }

  // A file the user picked for the file input with that node serial (the top byte says which frame): its name, type and bytes.
  void file_chosen(uint32_t node_serial, StringView name, StringView type, ConstByteView bytes);

  // --- History
  // The state (JSON text) that the history entry this page was opened at carries: set before load(), when the browser goes back or forward to a page that was
  // left with pushState().
  void set_history_state(StringView json, bool has_state);
  // The browser went to another entry of this same document (Back / Forward between pushState() entries, or fragment changes): the address and state are those of that
  // entry, and `popstate` (and `hashchange`, if only the fragment differs) fire.
  void traverse(StringView address, StringView state_json, bool has_state);

  // --- Frames
  Page* root() { return parent_ ? parent_->root() : this; }
  // This page is the content of the <iframe> with that node serial in `parent` (set before load()). Frames are nested up to 3 deep, 8 to a page.
  void set_parent(Page* parent, uint32_t frame_serial, uint32_t depth) {
    parent_ = parent;
    frame_serial_ = frame_serial;
    depth_ = depth;
    window_id_ = parent->frame_window_id(frame_serial);
  }
  void blur() { set_focus(nullptr); }
  // --- IFrameSource
  const HtmlDocument* frame_document(const dom::Node* iframe, uint32_t* tag) override;

  // --- Forms
  // The keys a form control understands. `modifiers`: 1 shift, 2 control, 4 alt.
  enum class FormKey : uint8_t { Character, Enter, Backspace, Delete, Tab, Escape, Left, Right, Up, Down, Home, End };
  // A key pressed while the page has the focus. True if a control took it (typing, Tab, Enter ...); false if the view should do what it does with it
  // (scroll). With nothing focused only Tab is taken (it focuses the first control).
  bool key(FormKey key, char character, uint8_t modifiers);
  // Is a control focused (a key goes to it)? Does the page have any control to step to?
  bool has_focus() const;
  // Does the focused control (here or in a frame) show a caret, so that a blink asks for a new picture of the page?
  bool caret_blinks() const;
  // The blink phase of the caret of the focused field. The next changed()/render() reflect it.
  void set_caret_visible(bool visible);
  bool has_controls();
  // Is a modal <dialog> open (the rest of the page does not take clicks or keys)?
  bool has_modal() { return modal_dialog() != nullptr; }
  // Runs the timers that are due at `now_ms` (the host clock); true if any ran.
  bool run_due_timers(double now_ms);
  // When the next timer is due (host clock); false if none is waiting.
  bool next_timer(double* due_ms);
  // Where the page is scrolled to (a script called scrollTo): the vertical position in pixels it asked for, or -1 if it did not since the last call.
  int32_t take_scroll_request();
  // The address of the page that led here (document.referrer); set before load().
  void set_referrer(StringView referrer) { referrer_.assign(referrer); }
  // Leaving the page: fires `beforeunload`; true if a listener asked for a confirmation (preventDefault() or a returnValue).
  bool before_unload();
  // The page is going away: `visibilitychange` (hidden), `pagehide`, `unload`.
  void unload();
  // The page was scrolled (by the user or a script): window.scrollY follows, `scroll` fires and intersection observers look again. `y` is in CSS pixels.
  void set_scroll(int32_t y);
  // The page is shown to the user (true) or not - its tab is in the background, its window minimized: document.hidden / visibilityState follow, `visibilitychange` fires
  // when that changes, and requestAnimationFrame callbacks wait while it is hidden. Frames of the page follow. May be called before load().
  void set_visible(bool visible);
  bool visible() const { return !hidden_; }
  // Has the page set history.scrollRestoration to "manual"?
  bool scroll_restoration_manual();
  // The fragment of the page's address is scrolled to when the page is next drawn (after load, and a fragment change).
  void scroll_to_fragment() { fragment_pending_ = true; }
  // A <meta http-equiv=refresh> in the page: the delay in seconds and the address ("" = reload); false if there is none.
  bool meta_refresh(double* seconds, String* address);

  dom::Document& document() { return doc_; }
  StringView address() override { return address_.view(); }
  void set_address(StringView address) { address_.assign(address); }
  // The title: what <title> says now.
  StringView page_title();
  bool scripting() const { return scripts_enabled_; }
  // What scripts printed (one line each, errors included), how many errors they had and the first of them.
  StringView log() const { return log_.view(); }
  uint32_t errors() const { return errors_; }
  StringView first_error() const { return first_error_.view(); }

  // --- script::IHost
  void console(int level, StringView text) override;
  void alert(StringView text) override { env_.alert(text); }
  void document_write(StringView text, bool newline) override;
  uint64_t now_ms() override { return env_.now_ms(); }
  StringView location() override { return address_.view(); }
  StringView title() override { return page_title(); }
  void set_title(StringView title) override;
  StringView user_agent() override { return "Mozilla/5.0 (compatible; TheoryCraftOS-Browser/1.0)"; }

  // --- IDomHost
  const MediaEnvironment& media() override { return media_; }
  void navigate(StringView address, bool replace) override { env_.navigate(address, replace); }
  void set_hash(StringView hash) override;
  void reload() override { env_.reload(); }
  void history_go(int32_t delta) override { env_.history_go(delta); }
  uint32_t history_length() override { return env_.history_length(); }
  void push_state(StringView address, StringView state_json, bool has_state, bool replace) override;
  bool history_state(String* json) override;
  bool history_entries(String* json, int64_t* index) override { return env_.history_entries(json, index); }
  void set_navigation_state(StringView json, bool has_state) override { env_.set_navigation_state(json, has_state); }
  void history_go_to_key(uint64_t key) override { env_.history_go_to_key(key); }
  bool open_window(StringView address, StringView target, bool noopener) override { return env_.open_window(address, target, noopener); }
  void close_window() override { env_.close_window(); }
  StringView referrer() override { return referrer_.view(); }
  void scroll_node_into_view(dom::Node* node) override;
  bool node_rect(dom::Node* node, double* rect8) override;
  bool layout_ready() override { return geometry_ready_; }
  bool confirm(StringView text) override { return env_.confirm(text); }
  void scroll_to(int32_t y) override;
  bool pip_supported() override { return env_.pip_supported(); }
  dom::Node* pip_element() override;
  int pip_request(dom::Node* video, uint32_t* width, uint32_t* height) override;
  void pip_exit() override;
  bool clipboard_read(String* text) override { return env_.clipboard_get(text); }
  void clipboard_write(StringView text) override { env_.clipboard_set(text); }
  void clipboard_formats(String* csv) override { env_.clipboard_formats(csv); }
  bool clipboard_read_format(StringView format, String* bytes) override { return env_.clipboard_read_format(format, bytes); }
  bool clipboard_write_items(const ClipboardData& data) override { return env_.clipboard_write_items(data); }
  bool fullscreen_supported() override { return env_.fullscreen_supported(); }
  dom::Node* fullscreen_element() override;
  bool fullscreen_request(dom::Node* element) override;
  void fullscreen_exit() override;
  bool computed_style(const dom::Node* element, Style* out) override;
  const StyleSheet* computed_style_sheet() override { return &style_sheet(); }
  dom::Document* parse_document(StringView html) override;
  StorageArea& local_storage() override { return env_.local_storage_hub().area(storage_origin()); }
  StorageArea& session_storage() override { return env_.session_storage_hub().area(storage_origin()); }
  void storage_changed(bool session, const String* key, const String* old_value, const String* new_value) override;
  void cookie_get(String* out) override;
  void cookie_set(StringView text) override;
  void cookie_details(StringView url, String* out) override;
  bool cookie_write(StringView text) override;
  bool http_request(const FetchOptions& options, NetResponse* response) override;
  void http_answered(const NetResponse& response) override { env_.http_answered(response); }
  void page_origin(String* out) override;
  uint32_t ws_open(StringView url, StringView protocols, String* error) override;
  int ws_state(uint32_t id) override;
  bool ws_send(uint32_t id, bool text, ConstByteView data) override;
  void ws_close(uint32_t id, uint16_t code, StringView reason) override;
  bool ws_receive(uint32_t id, bool* text, ByteBuffer* data) override;
  void ws_info(uint32_t id, String* protocol, uint16_t* code, String* reason, bool* opened) override;
  bool rtc_available() override { return env_.rtc_service() != nullptr; }
  uint32_t rtc_create(StringView ice_servers, String* error) override;
  bool rtc_op(uint32_t id, StringView name, StringView a, StringView b, ConstByteView bytes, int64_t n1, int64_t n2, String* out) override;
  void rtc_release(uint32_t id) override;
  void ws_release(uint32_t id) override;
  uint32_t worker_start(StringView source, StringView url, StringView name, String* error) override;
  void worker_post(uint32_t id, StringView message) override;
  void worker_terminate(uint32_t id) override;
  void worker_pump() override;
  bool worker_next_event(uint32_t* id, uint32_t* kind, String* text) override;
  bool compile_handler(StringView code, script::Value* function) override;
  ServiceWorkerHub* service_workers() override { return sw_hub_ ? sw_hub_ : env_.service_workers(); }
  uint32_t service_worker_client() override { return sw_client_; }
  uint32_t service_worker_id() override { return sw_worker_; }
  CacheStore& cache_store() override;

  // --- Service workers
  IPageEnvironment& environment() { return env_; }
  // This page is the global scope of that worker of `hub`, not a document (set before the first script runs).
  void set_service_worker(ServiceWorkerHub* hub, uint32_t worker);
  // Runs the worker's script in the global scope (the engine is started if needed). False with *error if it threw or does not compile.
  bool run_worker_script(StringView source, String* error);
  // Calls a global function of the built-in scripts (see DomBindings::call_hook); microtasks the call queued have run when it returns.
  bool call_global(StringView name, const DomBindings::HookArg* args, uint32_t count, String* result = nullptr);
  // Runs the timers that are waiting without waiting for them to be due (at most `max`); true if any ran. has_timers(): is one waiting?
  bool run_timers_ahead(uint32_t max);
  bool has_timers();
  // The bytes at `address` for the document (a script, style sheet, picture, frame: `destination` says which), asked of the service worker in charge of the page first.
  // False if they cannot be had (or the status is 400 or more).
  bool sw_fetch_bytes(StringView address, StringView destination, uint64_t limit, ByteBuffer* out);

  // --- IDomHost: forms
  dom::Node* focused() override;
  void set_focus(dom::Node* control) override;
  bool activate_control(dom::Node* node) override;
  void submit_form(dom::Node* form, dom::Node* submitter, bool interactive) override;
  void reset_form(dom::Node* form) override;
  bool report_validity(dom::Node* control) override;
  uint32_t window_id() override { return window_id_; }
  uint32_t parent_window_id() override { return parent_ ? parent_->window_id_ : 0; }
  uint32_t top_window_id() override { return root()->window_id_; }
  uint32_t frame_window_id(uint32_t iframe_serial) override;
  bool post_window_message(uint32_t target_window, StringView target_origin, StringView payload, StringView ports) override;
  uint32_t new_port_pair() override;
  void move_port(uint32_t port, uint32_t window) override;
  void post_port_message(uint32_t port, StringView payload, StringView ports) override;
  void broadcast(StringView channel, StringView payload) override;
  bool in_frame() override { return parent_ != nullptr; }
  void show_dialog(dom::Node* dialog, bool modal) override;
  void close_dialog(dom::Node* dialog, const char* return_value) override;

  // --- dom::IScriptRunner
  StringView external(uint32_t index, StringView src) override;
  void run(dom::Node* script, StringView source, String* written) override;

  // --- IStyleSource
  StringView linked(const dom::Node* link_element) override;

 private:
  struct Fetched {
    explicit Fetched(allocators::IAllocator& a) : address(a), text(a) {}
    uint32_t serial = 0;   // the link element (style sheets) or the position (scripts)
    String address, text;
  };

  bool start_engine();
  void resolve_scroll_target(const layout::BoxTree& tree);
  // Fires `copy`, `cut` or `paste` (a ClipboardEvent with clipboardData) at `target`; true if a listener called preventDefault() (it did the work itself).
  bool fire_clipboard(dom::Node* target, StringView type);
  // Ctrl+A / C / X / V with no text field to work on: the page's own selection and the events.
  void page_clipboard_key(dom::Node* target, char key);
  void deliver_object_events();
  // Forms
  struct Frame;
  class FrameEnv;
  friend class FrameEnv;
  void sync_frames();
  void frame_navigate(uint32_t frame_serial, StringView address);
  void frame_pick_file(uint32_t frame_serial, uint32_t node_serial, bool multiple);
  void paste_text(dom::Node* control, StringView text);
  // Messages for this window (from windows and message ports) wait here until the next turn of the event loop.
  void queue_window_message(StringView payload, StringView origin, uint32_t source, StringView ports);
  void queue_port_message(uint32_t port, StringView payload, StringView ports);
  void deliver_messages();
  void poll_hub();
  Page* find_window(uint32_t id);
  const String& storage_origin();   // the key of this page's Web Storage (its origin; for a file, the file)
  void claim_ports(StringView ports, uint32_t window);
  void press_focus(dom::Node* node);
  dom::Node* modal_dialog();
 public:
  // The window left fullscreen by itself (the user pressed Escape): the page is told (fullscreenchange).
  void fullscreen_left();
  bool is_fullscreen() const { return fullscreen_stack_.count() > 0; }
  // The user closed the Picture-in-Picture window / changed its size.
  void pip_left();
  void pip_resized(uint32_t width, uint32_t height);
 private:
  void clear_fullscreen();
  // Takes out the fullscreen elements that left the document (the window goes back when none is left; `fullscreenchange` follows).
  void prune_fullscreen();
  void sync_fullscreen_media();
  void fire_fullscreen_change(dom::Node* target);
  bool frame_allows_fullscreen(uint32_t frame_serial);
  List<uint32_t> fullscreen_stack_;   // the elements shown fullscreen, the innermost last (requestFullscreen() on a descendant of the fullscreen element nests)
  uint32_t pip_serial_ = 0;
  void toggle_details(dom::Node* details);
  void close_dialog_text(dom::Node* dialog, StringView value);
  bool fire(dom::Node* target, StringView type, bool bubbles = true, bool cancelable = false, dom::Node* submitter = nullptr);
  void send_form(dom::Node* form, dom::Node* submitter);
  void input_changed(dom::Node* control, bool final_change);
  void move_focus(int32_t direction);
  void toggle_list(dom::Node* select);
  void choose_option(dom::Node* option);
  bool press_key(dom::Node* n, FormKey key, char character, uint8_t modifiers);
  void flush_errors();
  static bool custom_error(void* context, const dom::Node* control);
  void note_error(StringView text);
  void fetch_pending_styles();
  bool pending_style_link(const dom::Node* link, StringView* href) const;
  void prefetch_resources(StringView html);
  bool resolve(StringView reference, String* out);
  bool fetched_already(const dom::Node* link) const;
  const StyleSheet& style_sheet();

  allocators::IAllocator& allocator_;
  IPageEnvironment& env_;
  bool scripts_enabled_;
  MediaEnvironment media_, rendered_media_;
  String address_, log_, first_error_, title_text_, written_;
  dom::Document doc_;
  List<UniquePointer<dom::Document>> extra_documents_;
  List<Fetched> scripts_, sheets_;
  PageObjects objects_;
  uint32_t errors_ = 0;
  uint64_t rendered_version_ = 0;
  bool caret_visible_ = true, caret_dirty_ = false;   // the blink phase, and whether it changed since the last render
  bool loading_ = true, engine_failed_ = false;
  int32_t scroll_request_ = -1;
  String referrer_;
  bool fragment_pending_ = false;
  bool hidden_ = false;   // see set_visible()
  struct BoxRect {
    double x = 0, y = 0, width = 0, height = 0, content_width = 0, content_height = 0, padding_left = 0, padding_top = 0;
  };
  Dictionary<uint32_t, BoxRect> geometry_;   // node serial -> its box on the page as the last picture laid it out
  bool geometry_ready_ = false;
  int32_t scroll_y_ = 0;
  void record_geometry(const layout::BoxTree& tree);
  uint32_t scroll_node_ = 0;   // an element scrollIntoView() was called on: its place is worked out when the page is drawn
  uint32_t focus_ = 0, open_select_ = 0, invalid_node_ = 0;   // forms: the focused control, the open list, the control a submit stopped at
  struct Message {
    explicit Message(allocators::IAllocator& a) : payload(a), origin(a), ports(a) {}
    bool to_port = false;
    uint32_t port = 0, source = 0;
    String payload, origin, ports;
  };
  struct PortHome {
    uint32_t port, window;
  };
  List<UniquePointer<Frame>> frames_;
  List<Message> messages_;
  Page* parent_ = nullptr;
  uint32_t frame_serial_ = 0, depth_ = 0;
  uint32_t window_id_ = 1, next_window_id_ = 2;           // (the top page numbers the windows of its frames)
  List<PortHome> frame_windows_;                           // (the window number of each <iframe>, by the node's serial, made when first asked for)
  uint32_t next_port_ = 1;
  List<PortHome> port_homes_;                              // (kept by the top page: which window each message port is in)
  uint32_t hub_id_ = 0;
  uint64_t hub_seen_ = 0;
  ServiceWorkerHub* sw_hub_ = nullptr;   // set for the page that is a service worker's scope
  uint32_t sw_worker_ = 0, sw_client_ = 0;
  UniquePointer<CacheStore> own_caches_;   // for an environment without a store of its own
  UniquePointer<PageObjectEnv> object_env_;   // what the page's pictures and videos fetch through (kept: PageObjects holds on to it)
  IObjectEnvironment& object_environment();
  void deliver_sw_events();
  bool fetch_text_via_sw(StringView address, const char* destination, uint64_t limit, String* text);
  String storage_origin_;
  String state_json_;           // history.state: the state of the entry this page is on, as JSON
  bool has_state_ = false;
  uint64_t controls_version_ = 0;
  bool controls_cached_ = false;
  String focus_value_, invalid_text_;                         // (what the focused control held when it got the focus; what to tell about the invalid one)
  // the style sheets as of now, for getComputedStyle
  StyleSheet style_cache_;
  uint32_t pointer_node_ = 0;
  bool pointer_pressed_ = false;
  struct Socket {
    uint32_t id = 0;
    UniquePointer<IWebSocket> socket;
  };
  List<Socket> sockets_;
  List<uint32_t> rtc_ids_, rtc_sources_;   // the connections this page made, and the microphones it opened   // the peer connections this page made (let go of with it)
  struct WorkerRuntime;
  List<UniquePointer<WorkerRuntime>> workers_;
  uint32_t next_worker_ = 1;
  uint32_t next_socket_ = 1;
  IWebSocket* socket_of(uint32_t id);
  uint64_t style_cache_version_ = 0;
  MediaEnvironment style_cache_media_;
  bool style_cache_valid_ = false;
  // what the scripts run in (destroyed first)
  UniquePointer<script::Engine> engine_;
  UniquePointer<DomBindings> bindings_;
};

}  // namespace browser
