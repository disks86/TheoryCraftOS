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
#include "Browser/HtmlDocument.hpp"
#include "Browser/PageHub.hpp"
#include "Browser/PagePicture.hpp"
#include "Browser/Media.hpp"
#include "Browser/Objects.hpp"
#include "Browser/Renderer.hpp"
#include "Dom/Dom.hpp"
#include "Font/FontSet.hpp"
#include "Script/Engine.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

// What a page asks of the browser it is shown in.
class IPageEnvironment : public IObjectEnvironment {
 public:
  // The text at an address (a script or a style sheet), at most `limit` bytes. False if it cannot be had.
  virtual bool fetch_text(StringView address, uint64_t limit, String* text) = 0;
  virtual void navigate(StringView address, bool replace) = 0;
  virtual void set_hash(StringView hash) = 0;
  virtual void reload() = 0;
  virtual void history_go(int32_t delta) = 0;
  virtual uint32_t history_length() = 0;
  // history.pushState() / replaceState() from the page (the browser adds or changes the history entry), and the state of the entry the page is on (JSON text).
  virtual void push_state(StringView address, StringView state_json, bool has_state, bool replace) = 0;
  virtual bool history_state(String* json) { (void)json; return false; }
  virtual void alert(StringView text) = 0;
  virtual bool confirm(StringView text) = 0;
  virtual void scroll_to(int32_t y) = 0;   // the page asked for a vertical position, in CSS pixels
  virtual uint64_t now_ms() = 0;                 // milliseconds since 1970 (0 without a clock)
  virtual void console(int level, StringView text) = 0;
  // Web Storage: the hub of localStorage (all origins, all windows) and the one of this tab's sessionStorage; and what the pages share to talk to each other.
  virtual StorageHub& local_storage_hub() = 0;
  virtual StorageHub& session_storage_hub() = 0;
  virtual PageHub& page_hub() = 0;
  virtual uint64_t tab_id() { return 1; }   // (tells the pages of one tab from those of another: they share a sessionStorage)
  // The cookies of the browser (shared by all its pages), and the network for fetch() and XMLHttpRequest.
  virtual CookieJar& cookies() = 0;
  virtual bool http_request(const FetchOptions& options, NetResponse* response) {
    (void)options;
    response->error.assign("This browser cannot make requests.");
    return false;
  }
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
  bool has_controls();
  // Is a modal <dialog> open (the rest of the page does not take clicks or keys)?
  bool has_modal() { return modal_dialog() != nullptr; }
  // Runs the timers that are due at `now_ms` (the host clock); true if any ran.
  bool run_due_timers(double now_ms);
  // When the next timer is due (host clock); false if none is waiting.
  bool next_timer(double* due_ms);
  // Where the page is scrolled to (a script called scrollTo): the vertical position in pixels it asked for, or -1 if it did not since the last call.
  int32_t take_scroll_request();

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
  bool confirm(StringView text) override { return env_.confirm(text); }
  void scroll_to(int32_t y) override;
  bool computed_style(const dom::Node* element, Style* out) override;
  const StyleSheet* computed_style_sheet() override { return &style_sheet(); }
  dom::Document* parse_document(StringView html) override;
  StorageArea& local_storage() override { return env_.local_storage_hub().area(storage_origin()); }
  StorageArea& session_storage() override { return env_.session_storage_hub().area(storage_origin()); }
  void storage_changed(bool session, const String* key, const String* old_value, const String* new_value) override;
  void cookie_get(String* out) override;
  void cookie_set(StringView text) override;
  bool http_request(const FetchOptions& options, NetResponse* response) override { return env_.http_request(options, response); }
  void page_origin(String* out) override;
  uint32_t ws_open(StringView url, StringView protocols, String* error) override;
  int ws_state(uint32_t id) override;
  bool ws_send(uint32_t id, bool text, ConstByteView data) override;
  void ws_close(uint32_t id, uint16_t code, StringView reason) override;
  bool ws_receive(uint32_t id, bool* text, ByteBuffer* data) override;
  void ws_info(uint32_t id, String* protocol, uint16_t* code, String* reason, bool* opened) override;
  void ws_release(uint32_t id) override;
  uint32_t worker_start(StringView source, StringView url, StringView name, String* error) override;
  void worker_post(uint32_t id, StringView message) override;
  void worker_terminate(uint32_t id) override;
  void worker_pump() override;
  bool worker_next_event(uint32_t* id, uint32_t* kind, String* text) override;
  bool compile_handler(StringView code, script::Value* function) override;

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
  bool loading_ = true, engine_failed_ = false;
  int32_t scroll_request_ = -1;
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
