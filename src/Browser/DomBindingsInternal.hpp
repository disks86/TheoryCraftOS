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

namespace browser {

struct DomBindings::Impl {
  Impl(script::Vm& v, dom::Document& d, IDomHost& h, DomBindings& o) : vm(v), doc(d), host(h), owner(o), first_error(v.allocator()), custom_validity(v.allocator()) {}
  struct CustomValidity {
    explicit CustomValidity(allocators::IAllocator& a) : message(a) {}
    uint32_t serial = 0;
    String message;
  };

  script::Vm& vm;
  dom::Document& doc;
  IDomHost& host;
  DomBindings& owner;

  script::JsObject *node_proto = nullptr, *element_proto = nullptr, *html_proto = nullptr, *text_proto = nullptr, *comment_proto = nullptr, *document_proto = nullptr,
                   *fragment_proto = nullptr, *event_proto = nullptr, *token_list_proto = nullptr, *style_proto = nullptr, *computed_proto = nullptr,
                   *storage_proto = nullptr, *location_proto = nullptr, *history_proto = nullptr, *media_query_proto = nullptr, *exception_proto = nullptr,
                   *canvas_context_proto = nullptr, *gradient_proto = nullptr, *pattern_proto = nullptr, *path2d_proto = nullptr, *typed_array_proto = nullptr,
                   *image_data_proto = nullptr, *text_metrics_proto = nullptr, *thenable_proto = nullptr, *time_ranges_proto = nullptr;
  script::JsObject* wrappers = nullptr;       // every wrapper made (kept alive)
  script::JsObject* media_lists = nullptr;    // the MediaQueryList objects given out
  script::JsObject* window_listeners = nullptr;
  script::JsObject* location = nullptr;
  script::JsObject* local_storage = nullptr;
  script::JsObject* session_storage = nullptr;
  dom::Node* current_script = nullptr;
  uint32_t error_count = 0;
  String first_error;
  List<CustomValidity> custom_validity;   // setCustomValidity() messages of controls
  int32_t scroll_line = 0;
  script::JsString *k_length = nullptr;
};

namespace bind {

using namespace script;

// The host class of node wrappers, and the object kinds the other files make.
extern const HostClass kNodeHostClass;

inline DomBindings::Impl& impl(Vm& vm) { return *static_cast<DomBindings::Impl*>(vm.user()); }

inline Value arg_at(const Value* args, uint32_t argc, uint32_t i) { return i < argc ? args[i] : Value(); }
inline Value str_value(Vm& vm, StringView text) { return Value::make_string(vm.new_string(text)); }
inline Value num_value(double d) { return Value::make_number(d); }
inline Value bool_value(bool b) { return Value::make_bool(b); }
inline Value object_value(JsObject* o) { return o ? Value::make_object(o) : Value::make_null(); }

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
JsObject* make_collection(DomBindings::Impl& c, const List<dom::Node*>& nodes);

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
bool install_media(DomBindings::Impl& c);   // <img>, <video>, <canvas>, the 2D context, ImageData, Path2D, typed arrays (DomBindingsMedia.cpp)
// Properties of <img>, <video> and <canvas> elements; true when handled / Handled when it was.
bool media_get(DomBindings::Impl& c, dom::Node* element, StringView key, Value* out);
HostClass::Set media_set(DomBindings::Impl& c, dom::Node* element, StringView key, const Value& value);
void update_metrics(DomBindings::Impl& c);
uint32_t media_changed_impl(DomBindings::Impl& c);

// Events (DomBindingsEvents.cpp).
bool dispatch_event(DomBindings::Impl& c, dom::Node* target, bool target_is_window, const Value& event, bool* prevented);
Value make_event(DomBindings::Impl& c, StringView type, const EventInit& init, JsObject* proto = nullptr);

// Styles (DomBindingsStyle.cpp).
Value make_style_object(DomBindings::Impl& c, dom::Node* element);
Value make_computed_style(DomBindings::Impl& c, dom::Node* element);
Value make_token_list(DomBindings::Impl& c, dom::Node* element);
Value make_dataset(DomBindings::Impl& c, dom::Node* element);

// The camel-case name of a CSS property and back ("backgroundColor" <-> "background-color").
void css_name_from_js(StringView js, String* out);
void js_name_from_css(StringView css, String* out);
}  // namespace bind
}  // namespace browser
