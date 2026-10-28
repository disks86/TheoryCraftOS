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

// The DOM as scripts see it: window, document, nodes and elements (with the properties and methods pages use - tree walking and changing, attributes, class
// lists, dataset, inline style, innerHTML / outerHTML / textContent, querySelector and the other lookups, getComputedStyle, matchMedia), events (listeners, the capture
// and bubble phases, preventDefault, stopPropagation, the Event constructors, on* handlers, the standard page events), location and history, storage
// (localStorage, sessionStorage), timers, DOMParser and the small parts of the platform pages expect (navigator, screen, performance, btoa, URI coding).
//
// Nodes are wrapped in host objects (see script::HostClass): a wrapper stands for one node for as long as the page lives, so `a === b` works. The
// collections that the DOM calls live (childNodes, getElementsByTagName, ...) are arrays made when asked for. What is not here: layout (sizes and positions
// are zero), the network (XMLHttpRequest, fetch), observers that report (MutationObserver and the like exist but never fire), forms as controls, canvas.
#pragma once
#include "Browser/Css.hpp"
#include "Browser/Fetcher.hpp"
#include "Browser/Media.hpp"
#include "Dom/Dom.hpp"
#include "Script/Vm.hpp"

namespace browser {

// __encoding_label and __decode_legacy: the legacy text encodings for TextDecoder (the main page and the workers both define them).
void install_text_encoding_natives(script::Vm& vm);

class PageObjects;
class CacheStore;
class ServiceWorkerHub;

// A set of named strings: localStorage and sessionStorage.
class StorageArea {
 public:
  explicit StorageArea(allocators::IAllocator& allocator) : allocator_(allocator), keys_(allocator), values_(allocator) {}
  uint32_t count() const { return static_cast<uint32_t>(keys_.count()); }
  StringView key(uint32_t i) const { return i < keys_.count() ? keys_[i].view() : StringView(); }
  bool get(StringView key, StringView* value) const;
  // False if memory ran out or the storage limit (about 5 MB) was passed.
  bool set(StringView key, StringView value);
  bool remove(StringView key);
  void clear() {
    if (keys_.count() != 0) ++version_;
    keys_.clear();
    values_.clear();
  }
  // Goes up with every change (for whoever saves the storage), and the bytes it holds (keys and values).
  uint64_t version() const { return version_; }
  uint64_t bytes() const;
  StringView value_at(uint32_t i) const { return i < values_.count() ? values_[i].view() : StringView(); }

 private:
  allocators::IAllocator& allocator_;
  List<String> keys_, values_;
  uint64_t version_ = 0;
};

// What a page puts on the clipboard in one go: plain text, html and a picture (PNG bytes); `has_` says which are given.
struct ClipboardData {
  bool has_text = false, has_html = false, has_image = false;
  StringView text, html, image_png;
};

// What the bindings need from the page they live in.
class IDomHost {
 public:
  virtual const MediaEnvironment& media() = 0;
  virtual StringView address() = 0;                            // where the page is from (with any #hash set by script)
  virtual void navigate(StringView address, bool replace) = 0; // location.assign / href = / replace
  virtual void set_hash(StringView hash) = 0;                  // location.hash = : no new page
  virtual void reload() = 0;
  virtual void history_go(int32_t delta) = 0;
  virtual uint32_t history_length() = 0;
  // history.pushState() / replaceState(): the address, and the state as JSON text (`has_state` false: none). history.state reads the entry the page is on.
  virtual void push_state(StringView address, StringView state_json, bool has_state, bool replace) = 0;
  virtual bool history_state(String* json) = 0;
  // The Navigation API's view of the session history (see IPageEnvironment), window.open() and window.close(), document.referrer, and Element.scrollIntoView().
  virtual bool history_entries(String* json, int64_t* index) { (void)json; (void)index; return false; }
  virtual void set_navigation_state(StringView json, bool has_state) { (void)json; (void)has_state; }
  virtual void history_go_to_key(uint64_t key) { (void)key; }
  virtual bool open_window(StringView address, StringView target, bool noopener) { (void)address; (void)target; (void)noopener; return false; }
  virtual void close_window() {}
  virtual StringView referrer() { return StringView(); }
  virtual void scroll_node_into_view(dom::Node* node) { (void)node; }
  // Where the element is on the page as the last picture laid it out, in CSS pixels: x, y, width, height of its border box from the top left of the document, then
  // the width and height of its content box (0 for an inline box that is not replaced) and the padding at its left and top. False
  // if it has no box (display: none, not in the document) or nothing was laid out yet; layout_ready() says whether anything was.
  virtual bool node_rect(dom::Node* node, double* rect8) { (void)node; (void)rect8; return false; }
  virtual bool layout_ready() { return false; }
  virtual void alert(StringView text) = 0;
  virtual bool confirm(StringView text) = 0;
  virtual void scroll_to(int32_t y) = 0;   // scrollTo(): the vertical position in CSS pixels
  // Fullscreen API: can the window do it; the element shown fullscreen (nullptr: none); make an element fullscreen (false if the window would not); leave it.
  // Picture-in-Picture: can the browser do it; the video shown in the window (nullptr: none); put a video in it (0 done, 1 not supported, 2 invalid state: the video has
  // nothing to show yet or is not a video, 3 not allowed: disablePictureInPicture; `width` and `height` get the window's size); take it out.
  virtual bool pip_supported() { return false; }
  virtual dom::Node* pip_element() { return nullptr; }
  virtual int pip_request(dom::Node* video, uint32_t* width, uint32_t* height) { (void)video; (void)width; (void)height; return 1; }
  virtual void pip_exit() {}
  // The text on the clipboard (navigator.clipboard): false if there is none / no clipboard.
  virtual bool clipboard_read(String* text) { (void)text; return false; }
  virtual void clipboard_write(StringView text) { (void)text; }
  // The rest of the clipboard (navigator.clipboard.read() / write()): the formats on it as a comma separated list of MIME types, the bytes of one, and a write of text,
  // html and / or a PNG picture (false if it did not work).
  virtual void clipboard_formats(String* csv) { csv->clear(); }
  virtual bool clipboard_read_format(StringView format, String* bytes) { (void)format; (void)bytes; return false; }
  virtual bool clipboard_write_items(const ClipboardData& data) { (void)data; return false; }
  virtual bool fullscreen_supported() { return false; }
  virtual dom::Node* fullscreen_element() { return nullptr; }
  virtual bool fullscreen_request(dom::Node* element) { (void)element; return false; }
  virtual void fullscreen_exit() {}
  // The style of an element as it is drawn now (false if the page cannot say).
  virtual bool computed_style(const dom::Node* element, Style* out) = 0;
  // The style of a pseudo-element (::before, ::after) of an element, as the rules for it give it (false if the page cannot say).
  virtual bool computed_pseudo_style(const dom::Node* element, PseudoElement pseudo, Style* out) { (void)element; (void)pseudo; (void)out; return false; }
  // The style sheet that computed_style() worked against: what the TextRefs of a Style point into, and the readers for list valued properties (font-family,
  // box-shadow, transform ...). nullptr if the host has none.
  virtual const StyleSheet* computed_style_sheet() { return nullptr; }
  // A new document made from html text (DOMParser); owned by the host. nullptr if memory ran out.
  virtual dom::Document* parse_document(StringView html) = 0;
  // The Web Storage of the page's origin. After a change that is visible (a new value, a removed key, a cleared area) the other pages of the origin are told.
  virtual StorageArea& local_storage() = 0;
  virtual StorageArea& session_storage() = 0;
  virtual void storage_changed(bool session, const String* key, const String* old_value, const String* new_value) = 0;
  // document.cookie: what scripts of this page can read, and what a script's assignment does (the cookie jar of the browser, for this page's address).
  virtual void cookie_get(String* out) = 0;
  virtual void cookie_set(StringView text) = 0;
  // The Cookie Store API: the cookies a script of the page can see for `url` (relative to the page; "" for the page itself; only the page's own origin), one per line,
  // fields separated by tabs: name, value, domain, path, expires (ms since 1970, -1 for a session cookie), secure, sameSite (0 none given, 1 lax, 2 strict, 3 none), partitioned, hostOnly.
  // And a Set-Cookie string stored as document.cookie would: true if the cookie was kept (or removed an older one).
  virtual void cookie_details(StringView url, String* out) { (void)url; (void)out; }
  virtual bool cookie_write(StringView text) { (void)text; return false; }
  // fetch() and XMLHttpRequest: sends a request (cookies, redirects and the cross-origin rules included; see Fetcher.hpp). False (with response->error) if it failed.
  // May be called on a worker thread, several at a time (asynchronous requests run in parallel).
  virtual bool http_request(const FetchOptions& options, NetResponse* response) = 0;
  // An answer that http_request() returned reaches the script (always on the script thread): what has to be done on that thread (Clear-Site-Data of storage).
  virtual void http_answered(const NetResponse& response) { (void)response; }
  // The origin of the page ("https://example.com", or "null" for a file or a page without an address).
  virtual void page_origin(String* out) = 0;
  // Compiles the code of an on* attribute (or a javascript: address) into a function taking `event`. False (with a message sent to console) if it does not
  // compile; the function is not kept alive by this: the caller stores it where the script engine can see it.
  virtual bool compile_handler(StringView code, script::Value* function) = 0;
  // The pictures, videos and canvases of the page.
  virtual PageObjects& objects() = 0;
  // --- Forms (what is not in the tree: the focus, the open list, the submission)
  virtual dom::Node* focused() = 0;
  // Moves the focus to a control (nullptr: nowhere); blur / focus / change events follow.
  virtual void set_focus(dom::Node* control) = 0;
  // What a click does to a control, a label or an option (checks a box, presses a button, opens a list ...). True if the node was one of those.
  virtual bool activate_control(dom::Node* node) = 0;
  // Submits a form. `interactive`: as a click on a submit button does (the constraints are checked, `submit` fires and may stop it); otherwise as
  // form.submit() does (it just goes). Reports a constraint that is broken with `invalid` events and a message by the control.
  virtual void submit_form(dom::Node* form, dom::Node* submitter, bool interactive) = 0;
  // form.reset(): a cancelable `reset` event, then every control goes back to its markup.
  virtual void reset_form(dom::Node* form) = 0;
  // <dialog>: show(), showModal() and close(returnValue) (nullptr: leave the return value as it was); `close` fires when it was open.
  virtual void show_dialog(dom::Node* dialog, bool modal) = 0;
  virtual void close_dialog(dom::Node* dialog, const char* return_value) = 0;
  // Windows and messages. Every page of a tab (the page itself and its frames) has a window number. A message (already serialized) goes to the window with that
  // number if the target origin allows ("*" for any, "/" for the sender's own, otherwise an origin) as a task; `ports` lists the ids of the message ports that
  // go with it (comma separated).
  virtual uint32_t window_id() = 0;
  virtual uint32_t parent_window_id() = 0;   // 0: none
  virtual uint32_t top_window_id() = 0;
  virtual uint32_t frame_window_id(uint32_t iframe_serial) = 0;   // the window of the page in that <iframe> (0: none)
  virtual bool post_window_message(uint32_t target_window, StringView target_origin, StringView payload, StringView ports) = 0;
  // Message ports: a pair of ids is made (the second is the first plus one); a port lives in the window that has it; a message to a port goes to its window.
  virtual uint32_t new_port_pair() = 0;
  virtual void move_port(uint32_t port, uint32_t window) = 0;
  virtual void post_port_message(uint32_t port, StringView payload, StringView ports) = 0;
  // BroadcastChannel: a message (serialized) for the other pages of the origin.
  virtual void broadcast(StringView channel, StringView payload) = 0;
  virtual bool in_frame() = 0;
  // WebSockets. ws_open() returns the id of a connection (0 and *error set when it cannot be made); the page drives it with ws_poll() and reads with
  // ws_state() (0 connecting, 1 open, 2 closing, 3 closed) and ws_receive().
  virtual uint32_t ws_open(StringView url, StringView protocols, String* error) {
    (void)url;
    (void)protocols;
    error->assign("This browser cannot open WebSockets.");
    return 0;
  }
  virtual int ws_state(uint32_t id) { (void)id; return 3; }
  virtual bool ws_send(uint32_t id, bool text, ConstByteView data) { (void)id; (void)text; (void)data; return false; }
  virtual void ws_close(uint32_t id, uint16_t code, StringView reason) { (void)id; (void)code; (void)reason; }
  virtual bool ws_receive(uint32_t id, bool* text, ByteBuffer* data) { (void)id; (void)text; (void)data; return false; }
  virtual void ws_info(uint32_t id, String* protocol, uint16_t* code, String* reason, bool* opened) { (void)id; (void)protocol; (void)code; (void)reason; (void)opened; }
  virtual void ws_release(uint32_t id) { (void)id; }
  // WebRTC: peer connections made and driven by name (see IRtcService). rtc_available() says whether the browser has the machinery at all.
  virtual bool rtc_available() { return false; }
  virtual uint32_t rtc_create(StringView ice_servers, String* error) {
    (void)ice_servers;
    error->assign("NotSupportedError: This browser cannot make peer connections");
    return 0;
  }
  virtual bool rtc_op(uint32_t id, StringView name, StringView a, StringView b, ConstByteView bytes, int64_t n1, int64_t n2, String* out) {
    (void)id; (void)name; (void)a; (void)b; (void)bytes; (void)n1; (void)n2;
    out->assign("InvalidStateError: The connection is gone");
    return false;
  }
  virtual void rtc_release(uint32_t id) { (void)id; }
  // Web Workers: a worker is a script engine of its own that the page owns. worker_start() runs `source` in it (0 and *error set when that cannot be done);
  // worker_post() hands it a message (serialized); worker_pump() lets it run (messages in, timers) and collects what it says, which worker_next_event() gives
  // one at a time: kind 0 a message (text), 1 an uncaught error (text), 2 the worker closed itself.
  virtual uint32_t worker_start(StringView source, StringView url, StringView name, String* error) {
    (void)source;
    (void)url;
    (void)name;
    error->assign("This browser cannot run workers.");
    return 0;
  }
  virtual void worker_post(uint32_t id, StringView message) { (void)id; (void)message; }
  virtual void worker_terminate(uint32_t id) { (void)id; }
  virtual void worker_pump() {}
  virtual bool worker_next_event(uint32_t* id, uint32_t* kind, String* text) { (void)id; (void)kind; (void)text; return false; }
  // Service workers and the Cache API. service_workers() is the hub of the browser (nullptr: none); service_worker_client() is the id of this document there (0: it is not one
  // that service workers know); service_worker_id() is not 0 when this page is the global scope of that worker. cache_store() holds the caches of every origin.
  virtual ServiceWorkerHub* service_workers() { return nullptr; }
  virtual uint32_t service_worker_client() { return 0; }
  virtual uint32_t service_worker_id() { return 0; }
  virtual CacheStore& cache_store() = 0;
  // Shows what is wrong with a control (reportValidity()); true if nothing is.
  virtual bool report_validity(dom::Node* control) = 0;

 protected:
  IDomHost() = default;
  ~IDomHost() = default;
};

struct EventInit {
  bool bubbles = true, cancelable = true;
  double detail = 0, client_x = 0, client_y = 0;
  int button = 0;
  // Keyboard events: key ("a", "Enter", "Backspace" ...; empty for other events), keyCode, and the modifiers held.
  char key[16] = {0};
  double key_code = 0;
  bool ctrl = false, shift = false, alt = false;
  // A submit event: the button that was pressed (nullptr: none).
  dom::Node* submitter = nullptr;
  // A message event: what was sent and where from.
  StringView message_data, message_origin;
};

class DomBindings {
 public:
  DomBindings(script::Vm& vm, dom::Document& document, IDomHost& host);
  ~DomBindings();
  DomBindings(const DomBindings&) = delete;
  DomBindings& operator=(const DomBindings&) = delete;

  // Defines everything on the global object. False if memory ran out.
  bool install();

  // Calls a global function that the page's built-in scripts define (a "hook") with these arguments: how the browser delivers the events that are made out of
  // script values (a message with its data, a storage change). False if there is no such function or it threw (counted like a listener that threw).
  struct HookArg {
    enum class Kind : uint8_t { Null, Bool, Number, String, Node } kind = Kind::Null;
    dom::Node* node = nullptr;
    bool boolean = false;
    double number = 0;
    StringView text;
    static HookArg null() { return HookArg(); }
    static HookArg of(bool b) { HookArg a; a.kind = Kind::Bool; a.boolean = b; return a; }
    static HookArg of(double n) { HookArg a; a.kind = Kind::Number; a.number = n; return a; }
    static HookArg of(StringView s) { HookArg a; a.kind = Kind::String; a.text = s; return a; }
    static HookArg of(dom::Node* n) { HookArg a; a.kind = Kind::Node; a.node = n; return a; }
  };
  // `result` (optional) gets the value the function returned as text ("" for undefined and null).
  bool call_hook(StringView name, const HookArg* args, uint32_t count, String* result = nullptr);
  // The same, as a task of its own: it runs when the event loop gets to it (the next timer turn), not now.
  void schedule_hook(StringView name, const HookArg* args, uint32_t count);

  // Fires an event of that type at a node (nullptr: the window); true unless a listener called preventDefault(). Listeners that throw are counted and
  // the first message kept (take_error()).
  bool dispatch(dom::Node* target, StringView type, const EventInit& init = EventInit());
  // What a click on a node does when no listener stopped it: follows a link, toggles a check box. (Called by element.click() and by the browser after
  // dispatch(node, "click") returned true.)
  void activate(dom::Node* node);
  // What setCustomValidity() was given for a control ("" if nothing).
  bool custom_validity(const dom::Node* control, String* message);
  // The element whose script is running now (document.currentScript).
  void set_current_script(dom::Node* script);
  // document.readyState, and the window's numbers (innerWidth ...) from the media environment.
  void set_ready_state(StringView state);
  // document.hidden / visibilityState before any script runs (no event); later changes go through the page's __page_visibility hook.
  void set_hidden(bool hidden);
  // The page was scrolled (by the user or a script) to this vertical position: window.scrollY follows.
  void set_scroll(int32_t y);
  void update_window_metrics();
  // Looks at the media queries that matchMedia gave out: those whose answer changed get their change listeners called. Returns how many changed.
  uint32_t media_changed();
  // Text that listeners (and callbacks of timers) failed with.
  uint32_t errors() const;
  void take_error(String* message);
  // The wrapper of a node (made on first use).
  script::JsObject* wrap(dom::Node* node);
  // The node a wrapper stands for, or nullptr.
  static dom::Node* unwrap(const script::Value& value);

 // (the state of one page's bindings: see DomBindingsInternal.hpp)
  struct Impl;

 private:
  script::Vm& vm_;
  Impl* impl_;
};

}  // namespace browser
