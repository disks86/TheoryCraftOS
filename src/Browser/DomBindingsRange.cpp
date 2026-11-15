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

// What Range and NodeIterator (written in script: WebPrelude.cpp) keep in the document: their boundary points and reference nodes live in the page's
// dom::LiveRanges, where the tree operations move them as the nodes change. A script object names its entry by a number; the entry goes when the collector
// frees the object (Vm::set_sweep_hook), so the registry holds no range alive.
#include "Browser/DomBindingsInternal.hpp"

namespace browser {
namespace bind {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

namespace {

bool illegal(Vm& vm) { return vm.throw_error(ErrorKind::TypeError, "Illegal invocation"); }

// A number argument that names an entry (anything else names none).
uint32_t arg_id(const Value* args, uint32_t argc, uint32_t i) {
  const Value v = arg_at(args, argc, i);
  if (!v.is_number()) return ~uint32_t{0};
  const double d = v.u.number;
  return d >= 0 && d < 4294967295.0 ? static_cast<uint32_t>(d) : ~uint32_t{0};
}

uint32_t arg_offset(const Value* args, uint32_t argc, uint32_t i) {
  const Value v = arg_at(args, argc, i);
  const double d = v.is_number() ? v.u.number : 0;
  return d >= 0 && d < 4294967295.0 ? static_cast<uint32_t>(d) : 0;
}

// The document of `n` tells the page's registry of its changes from now on.
void attach(DomBindings::Impl& c, dom::Node* n) {
  dom::Document* d = n->owner;
  if (d->live() == &c.live) return;
  if (c.live_documents.push_back(d)) d->set_live(&c.live);
}

void sweep_hook(void* user) {
  DomBindings::Impl& c = *static_cast<DomBindings::Impl*>(user);
  if (!c.live.active()) return;
  c.live.sweep([&](const void* owner) { return c.vm.dying(static_cast<const JsObject*>(owner)); });
}

// __lr_new(range, node): a live range collapsed at the start of `node` that lives as long as the object `range`; its number
NATIVE(lr_new) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = node_of(arg_at(args, argc, 1));
  if (!n || !arg_at(args, argc, 0).is_object()) return illegal(vm);
  const Optional<uint32_t> id = c.live.add_range(n, args[0].as_object());
  if (!id.has_value()) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  attach(c, n);
  *result = num_value(*id);
  return true;
}

// __lr_get(id, which): 0 the start node, 1 the start offset, 2 the end node, 3 the end offset
NATIVE(lr_get) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  const dom::LiveRange* r = c.live.range(arg_id(args, argc, 0));
  if (!r) return illegal(vm);
  switch (arg_offset(args, argc, 1)) {
    case 0: *result = node_value(c, r->start); break;
    case 1: *result = num_value(r->start_offset); break;
    case 2: *result = node_value(c, r->end); break;
    default: *result = num_value(r->end_offset); break;
  }
  return true;
}

// __lr_set(id, startNode, startOffset, endNode, endOffset)
NATIVE(lr_set) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  dom::LiveRange* r = c.live.range(arg_id(args, argc, 0));
  dom::Node* start = node_of(arg_at(args, argc, 1));
  dom::Node* end = node_of(arg_at(args, argc, 3));
  if (!r || !start || !end) return illegal(vm);
  r->start = start;
  r->start_offset = arg_offset(args, argc, 2);
  r->end = end;
  r->end_offset = arg_offset(args, argc, 4);
  attach(c, start);
  attach(c, end);
  *result = Value();
  return true;
}

// __ni_new(iterator, root): a node iterator before `root`; its number
NATIVE(ni_new) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = node_of(arg_at(args, argc, 1));
  if (!n || !arg_at(args, argc, 0).is_object()) return illegal(vm);
  const Optional<uint32_t> id = c.live.add_iterator(n, args[0].as_object());
  if (!id.has_value()) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  attach(c, n);
  *result = num_value(*id);
  return true;
}

// __ni_get(id, which): 0 the reference node, 1 whether the pointer is before it
NATIVE(ni_get) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  const dom::LiveIterator* it = c.live.iterator(arg_id(args, argc, 0));
  if (!it) return illegal(vm);
  if (arg_offset(args, argc, 1) == 0) *result = node_value(c, it->reference);
  else *result = bool_value(it->before);
  return true;
}

// __ni_set(id, reference, before)
NATIVE(ni_set) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  dom::LiveIterator* it = c.live.iterator(arg_id(args, argc, 0));
  dom::Node* reference = node_of(arg_at(args, argc, 1));
  if (!it || !reference) return illegal(vm);
  it->reference = reference;
  it->before = Vm::to_boolean(arg_at(args, argc, 2));
  *result = Value();
  return true;
}

// __replace_data(node, offset, count, data): "replace data" of a text or comment node, offsets in UTF-16 code units; an IndexSizeError if the offset is past the end
NATIVE(replace_data_native) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = node_of(arg_at(args, argc, 0));
  if (!n || (!n->is_text() && n->type != dom::NodeType::Comment)) return illegal(vm);
  String data(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 3), &data)) return false;
  const uint64_t length = utf16_length(n->data.view());
  const uint64_t offset = arg_offset(args, argc, 1);
  if (offset > length) return throw_dom(vm, "IndexSizeError", "The offset is larger than the node's length.");
  uint64_t count = arg_offset(args, argc, 2);
  if (count > length - offset) count = length - offset;
  *result = Value();
  return replace_data(c, n, offset, count, data.view());
}

bool character_data(const dom::Node* n) { return n->is_text() || (n->type == dom::NodeType::Comment && n->dom_type != 10); }

// __node_length(node): the length of a node as the standard counts it (the UTF-16 code units of text, the children of anything else)
NATIVE(node_length) {
  (void)self;
  const dom::Node* n = node_of(arg_at(args, argc, 0));
  if (!n) return illegal(vm);
  uint64_t length = 0;
  if (character_data(n)) length = utf16_length(n->data.view());
  else if (n->type != dom::NodeType::Comment)
    for (const dom::Node* c = n->first_child; c; c = c->next) ++length;
  *result = num_value(static_cast<double>(length));
  return true;
}

// __node_index(node): the number of siblings before it
NATIVE(node_index) {
  (void)self;
  const dom::Node* n = node_of(arg_at(args, argc, 0));
  if (!n) return illegal(vm);
  *result = num_value(dom::child_index(n));
  return true;
}

// __node_root(node): the top of the tree the node is in (a shadow root is the top of its tree)
NATIVE(node_root) {
  (void)self;
  dom::Node* n = node_of(arg_at(args, argc, 0));
  if (!n) return illegal(vm);
  while (n->parent) n = n->parent;
  *result = node_value(impl(vm), n);
  return true;
}

// The position of the boundary point (a, ao) relative to (b, bo), both in one tree: -1 before, 0 equal, 1 after.
int compare_points(const dom::Node* a, uint32_t ao, const dom::Node* b, uint32_t bo) {
  if (a == b) return ao < bo ? -1 : ao > bo ? 1 : 0;
  if (dom::contains(a, b)) {
    const dom::Node* child = b;
    while (child->parent != a) child = child->parent;
    return dom::child_index(child) < ao ? 1 : -1;
  }
  if (dom::contains(b, a)) {
    const dom::Node* child = a;
    while (child->parent != b) child = child->parent;
    return dom::child_index(child) < bo ? -1 : 1;
  }
  return dom::compare_order(a, b) < 0 ? -1 : 1;
}

// __pt_cmp(nodeA, offsetA, nodeB, offsetB): the position of one boundary point relative to another in the same tree
NATIVE(pt_cmp) {
  (void)self;
  const dom::Node* a = node_of(arg_at(args, argc, 0));
  const dom::Node* b = node_of(arg_at(args, argc, 2));
  if (!a || !b) return illegal(vm);
  *result = num_value(compare_points(a, arg_offset(args, argc, 1), b, arg_offset(args, argc, 3)));
  return true;
}

// __lr_cmp(id, node, offset): -1 if the point is before the range of that number, 1 if after it, 0 if in it
NATIVE(lr_cmp) {
  (void)self;
  const dom::LiveRange* r = impl(vm).live.range(arg_id(args, argc, 0));
  const dom::Node* n = node_of(arg_at(args, argc, 1));
  if (!r || !n) return illegal(vm);
  const uint32_t offset = arg_offset(args, argc, 2);
  *result = num_value(compare_points(n, offset, r->start, r->start_offset) < 0 ? -1 : compare_points(n, offset, r->end, r->end_offset) > 0 ? 1 : 0);
  return true;
}

// __check_insert(parent, node, reference): throws what insertBefore would throw, changes nothing
NATIVE(check_insert_native) {
  (void)self;
  const dom::Node* parent = node_of(arg_at(args, argc, 0));
  const dom::Node* node = node_of(arg_at(args, argc, 1));
  const dom::Node* reference = node_of(arg_at(args, argc, 2));
  if (!parent || !node) return illegal(vm);
  *result = Value();
  const dom::DomError e = dom::check_insert(parent, node, reference);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

}  // namespace

bool replace_data(DomBindings::Impl& c, dom::Node* n, uint64_t offset, uint64_t count, StringView data) {
  const StringView old = n->data.view();
  const uint64_t from = utf16_to_byte(old, offset), to = utf16_to_byte(old, offset + count);
  String text(c.vm.allocator());
  if (!text.append(old.first(from)) || !text.append(data) || !text.append(old.substr(to))) return throw_dom_error(c.vm, dom::DomError::OutOfMemory);
  const dom::DomError e = dom::set_text_content(n, text.view());
  if (e != dom::DomError::None) return throw_dom_error(c.vm, e);
  if (c.live.active()) c.live.data_replaced(n, static_cast<uint32_t>(offset), static_cast<uint32_t>(count), static_cast<uint32_t>(utf16_length(data)));
  return true;
}

bool install_ranges(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__lr_new", lr_new, 2);
  vm.define_native(g, "__lr_get", lr_get, 2);
  vm.define_native(g, "__lr_set", lr_set, 5);
  vm.define_native(g, "__ni_new", ni_new, 2);
  vm.define_native(g, "__ni_get", ni_get, 2);
  vm.define_native(g, "__ni_set", ni_set, 3);
  vm.define_native(g, "__replace_data", replace_data_native, 4);
  vm.define_native(g, "__node_length", node_length, 1);
  vm.define_native(g, "__node_index", node_index, 1);
  vm.define_native(g, "__node_root", node_root, 1);
  vm.define_native(g, "__pt_cmp", pt_cmp, 4);
  vm.define_native(g, "__lr_cmp", lr_cmp, 3);
  vm.define_native(g, "__check_insert", check_insert_native, 3);
  vm.set_sweep_hook(sweep_hook, &c);
  return true;
}

}  // namespace bind
}  // namespace browser
