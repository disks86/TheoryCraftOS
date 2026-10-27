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

// What the files of the DOM bindings share: the state of one page's bindings (DomBindings::Impl) and the small helpers the natives are written with.
#pragma once
#include "Browser/DomBindings.hpp"
#include "Browser/Canvas.hpp"
#include "Browser/Fetcher.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

namespace bind {
struct WasmState;   // the WebAssembly runtime of the page (DomBindingsWasm.cpp)
struct GlState;     // the WebGL contexts of the page (DomBindingsGl.cpp)
struct GpuState;    // the WebGPU devices of the page (DomBindingsGpu.cpp)
}

// An asynchronous fetch()/XMLHttpRequest. The script thread makes it and polls it from a timer; a worker thread makes the request (the request is copied, so the
// worker never touches script objects) and the script thread builds the answer once it is done.
struct NetJob {
  explicit NetJob(allocators::IAllocator& a)
      : method(a), url(a), headers(a), mode(a), credentials(a), redirect(a), referrer(a), referrer_policy(a), page_address(a), body(a), response(a) {}
  enum class State : uint8_t { Queued, Running, Done };
  double id = 0;
  double timer = 0;   // the polling timer of the script thread
  String method, url, headers, mode, credentials, redirect, referrer, referrer_policy, page_address;
  ByteBuffer body;
  bool referrer_given = false;
  uint64_t now_ms = 0;
  uint32_t timeout_ms = 0;
  NetResponse response;
  bool ok = false;
  State state = State::Queued;   // (guarded by Impl::net_mutex)
};

struct DomBindings::Impl {
  Impl(script::Vm& v, dom::Document& d, IDomHost& h, DomBindings& o) : vm(v), doc(d), host(h), owner(o), first_error(v.allocator()), custom_validity(v.allocator()), net_jobs(v.allocator()), pending_scripts(v.allocator()) {}
  ~Impl();   // waits for the request threads
  dom::Document* inert_document = nullptr;   // the document the contents of templates belong to as far as ownerDocument tells (made when first asked for)
  struct CustomValidity {
    explicit CustomValidity(allocators::IAllocator& a) : message(a) {}
    uint32_t serial = 0;
    String message;
  };

  script::Vm& vm;
  dom::Document& doc;
  IDomHost& host;
  DomBindings& owner;

  script::JsObject *node_proto = nullptr, *element_proto = nullptr, *html_proto = nullptr, *character_data_proto = nullptr, *text_proto = nullptr, *comment_proto = nullptr, *document_proto = nullptr,
                   *fragment_proto = nullptr, *event_proto = nullptr, *token_list_proto = nullptr, *style_proto = nullptr, *computed_proto = nullptr,
                   *storage_proto = nullptr, *location_proto = nullptr, *history_proto = nullptr, *media_query_proto = nullptr, *exception_proto = nullptr,
                   *canvas_context_proto = nullptr, *gradient_proto = nullptr, *pattern_proto = nullptr, *path2d_proto = nullptr, *typed_array_proto = nullptr,
                   *image_data_proto = nullptr, *text_metrics_proto = nullptr, *thenable_proto = nullptr, *time_ranges_proto = nullptr, *media_error_proto = nullptr, *shadow_root_proto = nullptr;
  bool style_dirty = false;                   // the page changed since a script was last told (see style_change_hook)
  script::JsObject *svg_proto = nullptr, *svg_tag_protos = nullptr;   // SVGElement.prototype, and local name -> the prototype of its interface (SVGRectElement.prototype ...)
  script::JsObject* tag_protos = nullptr;     // tag name -> the prototype of its interface (HTMLInputElement.prototype ...)
  script::JsObject* wrappers = nullptr;       // every wrapper made (kept alive)
  script::JsObject* media_lists = nullptr;    // the MediaQueryList objects given out
  script::JsObject* window_listeners = nullptr;
  script::JsObject* location = nullptr;
  script::JsObject* local_storage = nullptr;
  script::JsObject* session_storage = nullptr;
  dom::Node* current_script = nullptr;
  List<uint32_t> pending_scripts;   // <script> elements a script put in the tree, for __script_inserted (run when the call that did it returns)
  uint32_t error_count = 0;
  bool in_error_event = false;
  String first_error;
  List<CustomValidity> custom_validity;   // setCustomValidity() messages of controls
  int32_t scroll_line = 0;
  bool page_hidden = false;   // document.hidden / visibilityState: true while the page is being left
  script::JsString *k_length = nullptr;
  // Requests in flight (see NetJob). At most kMaxNetWorkers threads run at once and they exist only while there is something to send.
  static constexpr uint32_t kMaxNetWorkers = 6, kMaxNetJobs = 256;
  kernel::Mutex net_mutex;
  List<UniquePointer<NetJob>> net_jobs;
  double net_next_id = 0;   // (script thread only)
  uint32_t net_workers = 0;   // threads started and not yet finished (changed under net_mutex; read atomically by the destructor)
  bool net_closing = false;
  UniquePointer<bind::WasmState> wasm;   // the WebAssembly store of the page (made by install_wasm)
  UniquePointer<bind::GlState> gl;       // the WebGL contexts of the page (made by the first getContext('webgl'))
  UniquePointer<bind::GpuState> gpu;     // the WebGPU devices of the page (made by the first requestDevice())
};

namespace bind {

using namespace script;

// The host class of node wrappers, and the object kinds the other files make.
extern const HostClass kNodeHostClass;

inline DomBindings::Impl& impl(Vm& vm) { return *static_cast<DomBindings::Impl*>(vm.user()); }

inline Value arg_at(const Value* args, uint32_t argc, uint32_t i) { return i < argc ? args[i] : Value(); }

// Text offsets: the DOM keeps text as UTF-8 and counts bytes; scripts count UTF-16 code units. (A byte that continues a character adds nothing; a four-byte
// character is two units.)
inline uint64_t utf16_length(StringView s) {
  uint64_t units = 0;
  for (uint64_t i = 0; i < s.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
  }
  return units;
}
inline uint64_t utf16_to_byte(StringView s, uint64_t unit) {
  uint64_t units = 0, i = 0;
  while (i < s.size() && units < unit) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    units += c >= 0xF0 ? 2 : 1;
    ++i;
    while (i < s.size() && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) ++i;
  }
  return i;
}
inline uint64_t byte_to_utf16(StringView s, uint64_t byte) { return utf16_length(s.first(byte < s.size() ? byte : s.size())); }
inline Value str_value(Vm& vm, StringView text) { return Value::make_string(vm.new_string(text)); }
inline Value num_value(double d) { return Value::make_number(d); }
inline Value bool_value(bool b) { return Value::make_bool(b); }
inline Value object_value(JsObject* o) { return o ? Value::make_object(o) : Value::make_null(); }

// The legacy code of a DOMException name ("NotFoundError" -> 8; 0 for names without one).
int dom_exception_code(StringView name);
// Throws a DOMException (an Error with that name); returns false for the natives to return.
bool throw_dom(Vm& vm, const char* name, StringView message);
// The error a DomError stands for.
bool throw_dom_error(Vm& vm, dom::DomError error);

// The wrapper of a node, or null for nullptr.
Value node_value(DomBindings::Impl& c, dom::Node* node);
// The node a value stands for (nullptr if it is not a node wrapper).
dom::Node* node_of(const Value& v);
// `this` of a native as a node; throws a TypeError and returns nullptr if it is not one.
dom::Node* this_node(Vm& vm, const Value& self);
// A node argument; throws a TypeError if it is not a node.
dom::Node* arg_node(Vm& vm, const Value* args, uint32_t argc, uint32_t i);

// A new array of the wrappers of these nodes, with item(i) (the collections the DOM calls NodeList and HTMLCollection).
JsObject* make_collection(DomBindings::Impl& c, const List<dom::Node*>& nodes, bool html_collection = true);
// Makes an array an instance of the interface of script with that name ("NodeList", "HTMLCollection"...), if the built-in script has defined it.
void set_interface(Vm& vm, JsObject* array, const char* name);

// The text of a string-like argument ("" for undefined and null, ToString otherwise).
bool arg_text(Vm& vm, const Value& v, String* out);

// The address of the page and the parts of it, for location and URL resolution.
struct AddressParts {
  explicit AddressParts(allocators::IAllocator& a) : protocol(a), host(a), hostname(a), port(a), pathname(a), search(a), hash(a), origin(a) {}
  String protocol, host, hostname, port, pathname, search, hash, origin;
};
void split_address(StringView address, AddressParts* out);

// Installers, one for each file.
bool install_nodes(DomBindings::Impl& c);
bool install_events(DomBindings::Impl& c);
bool install_window(DomBindings::Impl& c);
bool install_styles(DomBindings::Impl& c);
bool install_forms(DomBindings::Impl& c);   // <form>, controls, FormData (DomBindingsForm.cpp)
// Properties of form controls and forms; true when handled / Handled when it was.
bool form_get(DomBindings::Impl& c, dom::Node* element, StringView key, Value* out);
HostClass::Set form_set(DomBindings::Impl& c, dom::Node* element, StringView key, const Value& value);
bool is_form_element(const dom::Node* n);
bool form_lookup_named(DomBindings::Impl& c, dom::Node* n, StringView key, Value* out);
bool install_crypto(DomBindings::Impl& c);   // the natives under crypto.getRandomValues() and crypto.subtle (DomBindingsCrypto.cpp)
bool install_gpu(DomBindings::Impl& c);              // WebGPU's natives (DomBindingsGpu.cpp)
bool install_gl(DomBindings::Impl& c);               // WebGL's natives (DomBindingsGl.cpp)
void flush_gl(DomBindings::Impl& c, uint32_t serial);   // copies what a WebGL canvas has drawn into its bitmap
// An ImageSource for an <img>, <canvas> or <video> element value (DomBindingsMedia.cpp).
bool media_source_pixels(Vm& vm, const Value& v, ImageSource* src);
bool install_rtc(DomBindings::Impl& c);              // RTCPeerConnection's natives (DomBindingsRtc.cpp)
bool install_service_worker(DomBindings::Impl& c);   // the Cache API and service worker natives (DomBindingsServiceWorker.cpp)
bool install_net(DomBindings::Impl& c);    // URL, fetch()'s and XMLHttpRequest's natives, byte and text helpers (DomBindingsNet.cpp)
// Uint8Array objects (DomBindingsMedia.cpp): a new one holding these bytes, and the bytes of one (a view into the array; false for anything else).
JsObject* make_byte_array(DomBindings::Impl& c, StringView bytes);
bool byte_array_bytes(const Value& v, StringView* out);
bool install_dash(DomBindings::Impl& c);     // the natives under the dashjs-style player (DomBindingsDash.cpp)
bool install_tracks(DomBindings::Impl& c);   // the natives under text tracks and WebVTT (DomBindingsTracks.cpp)
bool install_mse(DomBindings::Impl& c);    // the natives under MediaSource and SourceBuffer (DomBindingsMse.cpp)
// A TimeRanges holding `count` ranges, as start and end seconds one after the other in `pairs`.
Value make_time_ranges(DomBindings::Impl& c, const double* pairs, uint32_t count);
bool install_media(DomBindings::Impl& c);   // <img>, <video>, <canvas>, the 2D context, ImageData, Path2D, typed arrays (DomBindingsMedia.cpp)
// Properties of <img>, <video> and <canvas> elements; true when handled / Handled when it was.
bool media_get(DomBindings::Impl& c, dom::Node* element, StringView key, Value* out);
HostClass::Set media_set(DomBindings::Impl& c, dom::Node* element, StringView key, const Value& value);
void update_metrics(DomBindings::Impl& c);
uint32_t media_changed_impl(DomBindings::Impl& c);

// Events (DomBindingsEvents.cpp).
// (`object_target`: a script object that is the target, instead of a node or the window)
bool dispatch_event(DomBindings::Impl& c, dom::Node* target, bool target_is_window, const Value& event, bool* prevented, JsObject* object_target = nullptr);
Value make_event(DomBindings::Impl& c, StringView type, const EventInit& init, JsObject* proto = nullptr);

// Styles (DomBindingsStyle.cpp).
Value make_style_object(DomBindings::Impl& c, dom::Node* element);
Value make_computed_style(DomBindings::Impl& c, dom::Node* element, uint32_t pseudo = 0);   // pseudo: a PseudoElement
Value make_token_list(DomBindings::Impl& c, dom::Node* element);
Value make_dataset(DomBindings::Impl& c, dom::Node* element);

// The camel-case name of a CSS property and back ("backgroundColor" <-> "background-color").
void css_name_from_js(StringView js, String* out);
void js_name_from_css(StringView css, String* out);
// Fires `error` on the window for an uncaught exception in an event handler, hook or timer (text: what the exception said).
void fire_error_event(DomBindings::Impl& c, StringView text);
}  // namespace bind

}  // namespace browser
