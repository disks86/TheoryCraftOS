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

class PageObjects;

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
    keys_.clear();
    values_.clear();
  }

 private:
  allocators::IAllocator& allocator_;
  List<String> keys_, values_;
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
  virtual void alert(StringView text) = 0;
  virtual bool confirm(StringView text) = 0;
  virtual void scroll_to(int32_t y) = 0;   // scrollTo(): the vertical position in CSS pixels
  // The style of an element as it is drawn now (false if the page cannot say).
  virtual bool computed_style(const dom::Node* element, Style* out) = 0;
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
  // fetch() and XMLHttpRequest: sends a request (cookies, redirects and the cross-origin rules included; see Fetcher.hpp). False (with response->error) if it failed.
  virtual bool http_request(const FetchOptions& options, NetResponse* response) = 0;
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
    enum class Kind : uint8_t { Null, Bool, Number, String } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    StringView text;
    static HookArg null() { return HookArg(); }
    static HookArg of(bool b) { HookArg a; a.kind = Kind::Bool; a.boolean = b; return a; }
    static HookArg of(double n) { HookArg a; a.kind = Kind::Number; a.number = n; return a; }
    static HookArg of(StringView s) { HookArg a; a.kind = Kind::String; a.text = s; return a; }
  };
  bool call_hook(StringView name, const HookArg* args, uint32_t count);
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
