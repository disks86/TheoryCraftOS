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

#include "Browser/DomBindings.hpp"
#include <new>
#include <initializer_list>
#include "Browser/DomBindingsInternal.hpp"
#include "Utility/Move.hpp"

namespace browser {

using namespace script;

namespace bind {

// --- Helpers -----------------------------------------------------------------------------------------------------

int dom_exception_code(StringView name) {
  static const struct {
    const char* name;
    int code;
  } kCodes[] = {{"IndexSizeError", 1}, {"HierarchyRequestError", 3}, {"WrongDocumentError", 4}, {"InvalidCharacterError", 5}, {"NoModificationAllowedError", 7},
                {"NotFoundError", 8}, {"NotSupportedError", 9}, {"InUseAttributeError", 10}, {"InvalidStateError", 11}, {"SyntaxError", 12}, {"InvalidModificationError", 13},
                {"NamespaceError", 14}, {"InvalidAccessError", 15}, {"TypeMismatchError", 17}, {"SecurityError", 18}, {"NetworkError", 19}, {"AbortError", 20},
                {"URLMismatchError", 21}, {"QuotaExceededError", 22}, {"TimeoutError", 23}, {"InvalidNodeTypeError", 24}, {"DataCloneError", 25}};
  for (const auto& k : kCodes)
    if (name.equals(k.name)) return k.code;
  return 0;
}

bool throw_dom(Vm& vm, const char* name, StringView message) {
  DomBindings::Impl& c = impl(vm);
  JsObject* e = vm.new_error(ErrorKind::Error, message);
  if (c.exception_proto) e->proto = c.exception_proto;
  vm.define_named(e, "name", str_value(vm, name), kWritable);
  const int code = dom_exception_code(name);
  vm.define_named(e, "code", num_value(code), kWritable);
  return vm.throw_value(Value::make_object(e));
}

bool throw_dom_error(Vm& vm, dom::DomError error) {
  switch (error) {
    case dom::DomError::HierarchyRequest: return throw_dom(vm, "HierarchyRequestError", "The new child element contains the parent.");
    case dom::DomError::NotFound: return throw_dom(vm, "NotFoundError", "The node to be removed is not a child of this node.");
    case dom::DomError::TooManyNodes: return throw_dom(vm, "QuotaExceededError", "The document has too many nodes.");
    default: return throw_dom(vm, "InvalidStateError", "The operation could not be done (out of memory).");
  }
}

Value node_value(DomBindings::Impl& c, dom::Node* node) {
  if (!node) return Value::make_null();
  return Value::make_object(c.owner.wrap(node));
}

dom::Node* node_of(const Value& v) { return DomBindings::unwrap(v); }

dom::Node* this_node(Vm& vm, const Value& self) {
  dom::Node* n = node_of(self);
  if (!n) vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  return n;
}

dom::Node* arg_node(Vm& vm, const Value* args, uint32_t argc, uint32_t i) {
  dom::Node* n = node_of(arg_at(args, argc, i));
  if (!n) vm.throw_error(ErrorKind::TypeError, "parameter is not of type 'Node'.");
  return n;
}

void set_interface(Vm& vm, JsObject* array, const char* name) {
  Value ctor, proto;
  if (vm.get_named(Value::make_object(vm.global()), name, &ctor) && ctor.is_object() && vm.get_named(ctor, "prototype", &proto) && proto.is_object()) array->proto = proto.as_object();
}

JsObject* make_collection(DomBindings::Impl& c, const List<dom::Node*>& nodes, bool html_collection) {
  Vm& vm = c.vm;
  JsObject* list = vm.new_array(static_cast<uint32_t>(nodes.count()));
  for (uint64_t i = 0; i < nodes.count(); ++i) vm.array_push(list, node_value(c, nodes[i]));
  Value item, named;
  vm.get_named(Value::make_object(vm.global()), "__collection_item", &item);
  vm.get_named(Value::make_object(vm.global()), "__collection_named", &named);
  vm.define_named(list, "item", item, kWritable);
  vm.define_named(list, "namedItem", named, kWritable);
  set_interface(vm, list, html_collection ? "HTMLCollection" : "NodeList");
  return list;
}

bool arg_text(Vm& vm, const Value& v, String* out) {
  out->clear();
  if (v.is_nullish()) return true;
  JsString* s;
  if (!vm.to_string(v, &s)) return false;
  out->assign(s->view());
  return true;
}

void split_address(StringView address, AddressParts* out) {
  StringView rest = address;
  StringView hash;
  const uint64_t hash_at = rest.find('#');
  if (hash_at != StringView::npos) {
    hash = rest.substr(hash_at);
    rest = rest.first(hash_at);
  }
  if (hash.size() == 1) hash = StringView();
  StringView search;
  const uint64_t q = rest.find('?');
  const bool web = address.starts_with("http://") || address.starts_with("https://");
  if (q != StringView::npos && web) {
    search = rest.substr(q);
    rest = rest.first(q);
    if (search.size() == 1) search = StringView();
  }
  out->hash.assign(hash);
  out->search.assign(search);
  if (!web) {
    out->protocol.assign("file:");
    out->pathname.assign(rest);
    out->origin.assign("null");
    return;
  }
  const bool secure = address.starts_with("https://");
  out->protocol.assign(secure ? "https:" : "http:");
  rest = rest.substr(secure ? 8 : 7);
  const uint64_t slash = rest.find('/');
  const StringView authority = slash == StringView::npos ? rest : rest.first(slash);
  out->host.assign(authority);
  const uint64_t colon = authority.rfind(':');
  if (colon != StringView::npos && colon + 1 < authority.size()) {
    out->hostname.assign(authority.first(colon));
    out->port.assign(authority.substr(colon + 1));
  } else {
    out->hostname.assign(authority);
  }
  out->pathname.assign(slash == StringView::npos ? StringView("/") : rest.substr(slash));
  out->origin.assign(out->protocol.view());
  out->origin.append("//");
  out->origin.append(authority);
}

}  // namespace bind

// --- DomBindings -------------------------------------------------------------------------------------------------

DomBindings::DomBindings(Vm& vm, dom::Document& document, IDomHost& host) : vm_(vm), impl_(nullptr) {
  void* mem = vm.allocator().allocate(sizeof(Impl));
  if (mem) impl_ = new (mem) Impl(vm, document, host, *this);
}

DomBindings::~DomBindings() {
  if (!impl_) return;
  impl_->~Impl();
  vm_.allocator().deallocate(impl_);
}

dom::Node* DomBindings::unwrap(const Value& v) {
  if (!v.is_object()) return nullptr;
  JsObject* o = v.as_object();
  return o->host == &bind::kNodeHostClass ? static_cast<dom::Node*>(o->host_data) : nullptr;
}

JsObject* DomBindings::wrap(dom::Node* node) {
  if (node->script) return static_cast<JsObject*>(node->script);
  Impl& c = *impl_;
  JsObject* proto = c.html_proto;
  switch (node->type) {
    case dom::NodeType::Text: proto = c.text_proto; break;
    case dom::NodeType::Comment: proto = c.comment_proto; break;
    case dom::NodeType::Document: proto = c.document_proto; break;
    case dom::NodeType::Fragment: proto = node->shadow_host && c.shadow_root_proto ? c.shadow_root_proto : c.fragment_proto; break;
    default:
      if (node->is_element() && c.tag_protos) {
        Value tagged;
        if (vm_.get_plain(c.tag_protos, vm_.intern(node->name.view()), &tagged) && tagged.is_object()) proto = tagged.as_object();
      }
      break;
  }
  JsObject* w = vm_.new_host_object(proto, &bind::kNodeHostClass, node);
  node->script = w;
  if (c.wrappers) vm_.array_push(c.wrappers, Value::make_object(w));   // (kept alive for as long as the page)
  return w;
}

bool DomBindings::install() {
  if (!impl_) return false;
  Impl& c = *impl_;
  vm_.set_user(&c);
  c.k_length = vm_.length_name();
  c.wrappers = vm_.new_array();
  c.media_lists = vm_.new_array();
  c.window_listeners = vm_.new_plain();
  vm_.define_named(vm_.global(), "__wrappers", Value::make_object(c.wrappers), 0);
  vm_.define_named(vm_.global(), "__media_lists", Value::make_object(c.media_lists), 0);
  const bool ok = bind::install_nodes(c) && bind::install_events(c) && bind::install_styles(c) && bind::install_window(c) && bind::install_media(c) && bind::install_mse(c) && bind::install_tracks(c) && bind::install_dash(c) && bind::install_forms(c) && bind::install_net(c) && !vm_.halted();
  // The prototypes and objects that only this code holds are kept alive by a hidden global: a collection before the page first used one would free them.
  if (ok) {
    JsObject* kept = vm_.new_array();
    for (JsObject* o : {c.node_proto, c.element_proto, c.html_proto, c.character_data_proto, c.text_proto, c.comment_proto, c.document_proto, c.fragment_proto, c.event_proto,
                        c.token_list_proto, c.style_proto, c.computed_proto, c.storage_proto, c.location_proto, c.history_proto, c.media_query_proto, c.exception_proto,
                        c.canvas_context_proto, c.gradient_proto, c.pattern_proto, c.path2d_proto, c.typed_array_proto, c.image_data_proto, c.text_metrics_proto,
                        c.thenable_proto, c.time_ranges_proto, c.media_error_proto, c.shadow_root_proto, c.tag_protos, c.window_listeners, c.location, c.local_storage, c.session_storage})
      if (o) vm_.array_push(kept, Value::make_object(o));
    vm_.define_named(vm_.global(), "__native_protos", Value::make_object(kept), 0);
  }
  return ok;
}

bool DomBindings::dispatch(dom::Node* target, StringView type, const EventInit& init) {
  Impl& c = *impl_;
  vm_.hold_collector();
  const Value event = bind::make_event(c, type, init);
  bool prevented = false;
  bind::dispatch_event(c, target, target == nullptr, event, &prevented);
  vm_.release_collector();
  return !prevented;
}

bool DomBindings::call_hook(StringView name, const HookArg* args, uint32_t count) {
  Impl& c = *impl_;
  Value fn;
  if (!vm_.get_named(Value::make_object(vm_.global()), name, &fn) || !Vm::is_callable(fn)) return false;
  List<Value> values(vm_.allocator());   // (the arguments are copied onto the VM's stack by the call, before anything can collect them)
  for (uint32_t i = 0; i < count; ++i) {
    switch (args[i].kind) {
      case HookArg::Kind::Null: values.push_back(Value::make_null()); break;
      case HookArg::Kind::Bool: values.push_back(Value::make_bool(args[i].boolean)); break;
      case HookArg::Kind::Number: values.push_back(Value::make_number(args[i].number)); break;
      case HookArg::Kind::String: values.push_back(Value::make_string(vm_.new_string(args[i].text))); break;
    }
  }
  Value ignored;
  const bool ok = vm_.call(fn, Value::make_object(vm_.global()), count ? &values[0] : nullptr, count, &ignored);
  if (!ok) {
    String text(vm_.allocator());
    vm_.take_exception(&text);
    if (c.error_count++ == 0) c.first_error.assign(text.view());
        bind::fire_error_event(c, text.view());
    if (vm_.host()) {
      String line(vm_.allocator());
      line.append("Uncaught ");
      line.append(text.view());
      vm_.host()->console(3, line.view());
    }
  }
  return ok;
}

void DomBindings::schedule_hook(StringView name, const HookArg* args, uint32_t count) {
  Value runner;
  if (!vm_.get_named(Value::make_object(vm_.global()), "__run_hook", &runner)) return;
  List<Value> values(vm_.allocator());
  values.push_back(Value::make_string(vm_.new_string(name)));
  for (uint32_t i = 0; i < count; ++i) {
    switch (args[i].kind) {
      case HookArg::Kind::Null: values.push_back(Value::make_null()); break;
      case HookArg::Kind::Bool: values.push_back(Value::make_bool(args[i].boolean)); break;
      case HookArg::Kind::Number: values.push_back(Value::make_number(args[i].number)); break;
      case HookArg::Kind::String: values.push_back(Value::make_string(vm_.new_string(args[i].text))); break;
    }
  }
  vm_.add_timer(runner, 0, -1, &values[0], static_cast<uint32_t>(values.count()));
}

void DomBindings::set_current_script(dom::Node* script) { impl_->current_script = script; }

bool DomBindings::custom_validity(const dom::Node* control, String* message) {
  for (uint64_t i = 0; i < impl_->custom_validity.count(); ++i) {
    const auto& c = impl_->custom_validity[i];
    if (c.serial != control->serial || c.message.empty()) continue;
    if (message) message->assign(c.message.view());
    return true;
  }
  return false;
}

uint32_t DomBindings::errors() const { return impl_->error_count; }

void DomBindings::take_error(String* message) {
  message->assign(impl_->first_error.view());
  impl_->first_error.clear();
  impl_->error_count = 0;
}

}  // namespace browser
