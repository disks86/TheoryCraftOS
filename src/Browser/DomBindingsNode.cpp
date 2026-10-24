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

// Nodes, elements and documents for scripts: the properties (through the host class of the wrappers) and the methods (on the prototypes).
#include <initializer_list>
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/Renderer.hpp"
#include "Browser/Url.hpp"
#include "Dom/HtmlBuilder.hpp"
#include "Dom/Selector.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)
#define IS(text) k.equals(text)

char lower_char(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// The local name of an element: its qualified name without the prefix, as it was written (for elements made with createElementNS); else the lowercase tag.
StringView local_name_of(const dom::Node* n) {
  if (n->qname.empty()) return n->name.view();
  const uint64_t colon = n->qname.view().find(':');
  return colon == StringView::npos ? n->qname.view() : n->qname.view().substr(colon + 1);
}

Value upper_value(Vm& vm, StringView s);
// tagName / nodeName: the qualified name, in capitals for an element of HTML in a page of HTML.
Value tag_name_value(Vm& vm, const dom::Node* n) {
  const bool html = !n->ns_set || n->ns.view().equals("http://www.w3.org/1999/xhtml");
  if (!n->qname.empty()) return html && n->owner->content_type.empty() ? upper_value(vm, n->qname.view()) : Value::make_string(vm.new_string(n->qname.view()));
  return html && n->owner->content_type.empty() ? upper_value(vm, n->name.view()) : Value::make_string(vm.new_string(n->name.view()));
}

Value upper_value(Vm& vm, StringView s) {
  String u(vm.allocator());
  for (uint64_t i = 0; i < s.size(); ++i) u.push_back(s[i] >= 'a' && s[i] <= 'z' ? static_cast<char>(s[i] - 32) : s[i]);
  return str_value(vm, u.view());
}

// --- Reflected attributes --------------------------------------------------------------------------------------------

enum class Reflect : uint8_t { Str, Bool, Int, Url };
struct ReflectedAttribute {
  const char* js;
  const char* attribute;
  Reflect kind;
};
const ReflectedAttribute kReflected[] = {
    {"id", "id", Reflect::Str}, {"className", "class", Reflect::Str}, {"title", "title", Reflect::Str}, {"lang", "lang", Reflect::Str}, {"dir", "dir", Reflect::Str},
    {"hidden", "hidden", Reflect::Bool}, {"accessKey", "accesskey", Reflect::Str}, {"tabIndex", "tabindex", Reflect::Int}, {"href", "href", Reflect::Url},
    {"src", "src", Reflect::Url}, {"alt", "alt", Reflect::Str}, {"name", "name", Reflect::Str}, {"type", "type", Reflect::Str}, {"value", "value", Reflect::Str},
    {"placeholder", "placeholder", Reflect::Str}, {"target", "target", Reflect::Str}, {"rel", "rel", Reflect::Str}, {"download", "download", Reflect::Str},
    {"action", "action", Reflect::Url}, {"method", "method", Reflect::Str}, {"htmlFor", "for", Reflect::Str}, {"content", "content", Reflect::Str},
    {"charset", "charset", Reflect::Str}, {"disabled", "disabled", Reflect::Bool}, {"checked", "checked", Reflect::Bool}, {"selected", "selected", Reflect::Bool},
    {"readOnly", "readonly", Reflect::Bool}, {"required", "required", Reflect::Bool}, {"multiple", "multiple", Reflect::Bool}, {"autofocus", "autofocus", Reflect::Bool},
    {"open", "open", Reflect::Bool}, {"async", "async", Reflect::Bool}, {"defer", "defer", Reflect::Bool}, {"reversed", "reversed", Reflect::Bool},
    {"width", "width", Reflect::Int}, {"height", "height", Reflect::Int}, {"colSpan", "colspan", Reflect::Int}, {"rowSpan", "rowspan", Reflect::Int},
    {"maxLength", "maxlength", Reflect::Int}, {"size", "size", Reflect::Int}, {"cols", "cols", Reflect::Int}, {"rows", "rows", Reflect::Int}, {"start", "start", Reflect::Int},
    {"span", "span", Reflect::Int}, {"min", "min", Reflect::Str}, {"max", "max", Reflect::Str}, {"step", "step", Reflect::Str}, {"pattern", "pattern", Reflect::Str},
    {"autocomplete", "autocomplete", Reflect::Str}, {"media", "media", Reflect::Str}, {"role", "role", Reflect::Str}, {"srcset", "srcset", Reflect::Str},
    {"label", "label", Reflect::Str}, {"scope", "scope", Reflect::Str}, {"headers", "headers", Reflect::Str}, {"enctype", "enctype", Reflect::Str},
    {"noValidate", "novalidate", Reflect::Bool}, {"srcdoc", "srcdoc", Reflect::Str}, {"sandbox", "sandbox", Reflect::Str}, {"allow", "allow", Reflect::Str}, {"useMap", "usemap", Reflect::Str}, {"loading", "loading", Reflect::Str}, {"crossOrigin", "crossorigin", Reflect::Str},
};

const ReflectedAttribute* find_reflected(StringView js) {
  for (const ReflectedAttribute& r : kReflected)
    if (js.equals(r.js)) return &r;
  return nullptr;
}

// --- Small helpers -----------------------------------------------------------------------------------------------------

bool is_container(const dom::Node* n) { return n->type == dom::NodeType::Element || n->type == dom::NodeType::Document || n->type == dom::NodeType::Fragment; }

void children_of(const dom::Node* n, List<dom::Node*>* out, bool elements_only) {
  for (dom::Node* c = n->first_child; c; c = c->next)
    if (!elements_only || c->is_element()) out->push_back(c);
}

void descendants_with(const dom::Node* root, List<dom::Node*>* out, bool (*pred)(const dom::Node*, const void*), const void* data) {
  for (dom::Node* n = root->first_child; n; n = dom::next_in_tree(n, root))
    if (n->is_element() && pred(n, data)) out->push_back(n);
}

bool has_tag(const dom::Node* n, const void* data) { return static_cast<const StringView*>(data)->equals("*") || n->name.view().equals(*static_cast<const StringView*>(data)); }

bool is_control(const dom::Node* n) { return n->is_tag("input") || n->is_tag("select") || n->is_tag("textarea") || n->is_tag("button") || n->is_tag("fieldset") || n->is_tag("output"); }

JsObject* style_collection_empty(DomBindings::Impl& c) {
  List<dom::Node*> none(c.vm.allocator());
  return make_collection(c, none);
}

// Compiles `selector` and finds what matches below `root` (the first only unless `all`). Throws a SyntaxError for a bad selector (false).
bool select(Vm& vm, dom::Node* root, StringView selector, bool all, List<dom::Node*>* found) {
  dom::SelectorSet set(vm.allocator());
  uint32_t added = 0;
  if (!set.parse(selector, 0, &added, false)) {
    String message(vm.allocator());
    message.append("'");
    message.append(selector);
    message.append("' is not a valid selector.");
    return throw_dom(vm, "SyntaxError", message.view());
  }
  const dom::Node* scope = root->is_element() ? root : nullptr;
  for (dom::Node* n = root->first_child; n; n = dom::next_in_tree(n, root)) {
    if (!n->is_element() || !set.matches_any(n, scope)) continue;
    found->push_back(n);
    if (!all) break;
  }
  return true;
}

// The nodes made from the arguments of append() and the like: nodes as they are, anything else as text. Put in a fragment.
dom::Node* nodes_to_fragment(Vm& vm, const Value* args, uint32_t argc) {
  dom::Document& doc = impl(vm).doc;
  dom::Node* fragment = doc.create_fragment();
  if (!fragment) {
    throw_dom_error(vm, dom::DomError::OutOfMemory);
    return nullptr;
  }
  for (uint32_t i = 0; i < argc; ++i) {
    dom::Node* n = node_of(args[i]);
    if (n) {
      if (n->type == dom::NodeType::Document) {
        throw_dom(vm, "HierarchyRequestError", "A document cannot be inserted.");
        return nullptr;
      }
      if (dom::append_child(fragment, n) != dom::DomError::None) {
        throw_dom_error(vm, dom::DomError::HierarchyRequest);
        return nullptr;
      }
      continue;
    }
    String text(vm.allocator());
    if (!arg_text(vm, args[i], &text)) return nullptr;
    dom::Node* t = doc.create_text(text.view());
    if (!t || dom::append_child(fragment, t) != dom::DomError::None) {
      throw_dom_error(vm, dom::DomError::OutOfMemory);
      return nullptr;
    }
  }
  return fragment;
}

// --- Properties ----------------------------------------------------------------------------------------------------------

Value cached_object(Vm& vm, JsObject* o, const char* name, Value (*make)(DomBindings::Impl&, dom::Node*), dom::Node* n) {
  Value cached;
  JsString* key = vm.intern(name);
  if (vm.get_plain(o, key, &cached)) return cached;
  cached = make(impl(vm), n);
  vm.define(o, key, cached, 0);
  return cached;
}

Value attributes_value(DomBindings::Impl& c, dom::Node* n) {
  Vm& vm = c.vm;
  JsObject* list = vm.new_array(static_cast<uint32_t>(n->attrs.count()));
  for (uint64_t i = 0; i < n->attrs.count(); ++i) {
    JsObject* a = vm.new_plain();
    vm.define_named(a, "name", str_value(vm, n->attrs[i].name.view()));
    vm.define_named(a, "value", str_value(vm, n->attrs[i].value.view()));
    vm.define_named(a, "specified", bool_value(true));
    {   // namespaceURI, prefix, localName of the attribute
      const StringView full = n->attrs[i].name.view();
      const uint64_t colon = full.find(':');
      vm.define_named(a, "namespaceURI", n->attrs[i].ns.empty() ? Value::make_null() : str_value(vm, n->attrs[i].ns.view()));
      vm.define_named(a, "prefix", colon == StringView::npos ? Value::make_null() : str_value(vm, full.substr(0, colon)));
      vm.define_named(a, "localName", str_value(vm, colon == StringView::npos ? full : full.substr(colon + 1)));
    }
    vm.define_named(a, "ownerElement", node_value(c, n));
    vm.array_push(list, Value::make_object(a));
  }
  Value item;
  vm.get_named(Value::make_object(vm.global()), "__collection_item", &item);
  vm.define_named(list, "item", item, kWritable);
  set_interface(vm, list, "NamedNodeMap");
  return Value::make_object(list);
}

bool node_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  dom::Node* n = static_cast<dom::Node*>(o->host_data);
  DomBindings::Impl& c = impl(vm);
  const StringView k = key->view();
  if (k.size() < 2 || (k[0] == '_' && k[1] == '_')) return false;
  const bool element = n->is_element(), document = n->type == dom::NodeType::Document, container = is_container(n);
  // --- every node
  if (IS("nodeType")) { *out = num_value(n->dom_type ? n->dom_type : static_cast<int>(n->type)); return true; }
  if (IS("target") && n->dom_type == 7) { *out = str_value(vm, n->name.view()); return true; }
  if (IS("nodeName")) {
    *out = element ? tag_name_value(vm, n) : str_value(vm, n->name.view());
    return true;
  }
  if (IS("nodeValue")) {
    *out = n->is_text() || n->type == dom::NodeType::Comment ? str_value(vm, n->data.view()) : Value::make_null();
    return true;
  }
  if (IS("textContent")) {
    if (document) *out = Value::make_null();
    else {
      String text(vm.allocator());
      dom::text_content(n, &text);
      *out = str_value(vm, text.view());
    }
    return true;
  }
  if (IS("parentNode")) { *out = node_value(c, n->parent); return true; }
  if (IS("parentElement")) { *out = node_value(c, n->parent && n->parent->is_element() ? n->parent : nullptr); return true; }
  if (IS("firstChild")) { *out = node_value(c, n->first_child); return true; }
  if (IS("lastChild")) { *out = node_value(c, n->last_child); return true; }
  if (IS("previousSibling")) { *out = node_value(c, n->prev); return true; }
  if (IS("nextSibling")) { *out = node_value(c, n->next); return true; }
  if (IS("childNodes")) {
    List<dom::Node*> list(vm.allocator());
    children_of(n, &list, false);
    *out = Value::make_object(make_collection(c, list, false));
    return true;
  }
  if (IS("ownerDocument")) { *out = document ? Value::make_null() : node_value(c, n->owner->root()); return true; }
  if (IS("isConnected")) { *out = bool_value(dom::is_connected(n)); return true; }
  if (IS("baseURI")) { *out = str_value(vm, c.host.address()); return true; }
  if (container) {
    if (IS("children")) {
      List<dom::Node*> list(vm.allocator());
      children_of(n, &list, true);
      *out = Value::make_object(make_collection(c, list));
      return true;
    }
    if (IS("firstElementChild")) { *out = node_value(c, dom::first_element_child(n)); return true; }
    if (IS("lastElementChild")) { *out = node_value(c, dom::last_element_child(n)); return true; }
    if (IS("childElementCount")) { *out = num_value(dom::child_element_count(n)); return true; }
  }
  if (element || n->is_text() || n->type == dom::NodeType::Comment) {
    if (IS("previousElementSibling")) { *out = node_value(c, dom::previous_element_sibling(n)); return true; }
    if (IS("nextElementSibling")) { *out = node_value(c, dom::next_element_sibling(n)); return true; }
  }
  if (element && IS("shadowRoot")) { *out = n->shadow && !n->shadow->shadow_closed ? node_value(c, n->shadow) : Value::make_null(); return true; }
  if (element && IS("slot")) {
    StringView slot;
    *out = str_value(vm, dom::get_attribute(n, "slot", &slot) ? slot : StringView());
    return true;
  }
  if (n->shadow_host) {   // a shadow root
    if (IS("host")) { *out = node_value(c, n->shadow_host); return true; }
    if (IS("mode")) { *out = str_value(vm, n->shadow_closed ? "closed" : "open"); return true; }
  }
  if (n->is_text() && IS("wholeText")) { *out = str_value(vm, n->data.view()); return true; }
  if (n->is_text() && IS("length")) { *out = num_value(static_cast<double>(n->data.size())); return true; }
  if (n->is_text() && IS("data")) { *out = str_value(vm, n->data.view()); return true; }
  // --- documents
  if (document) {
    if (IS("documentElement")) { *out = node_value(c, n->owner->document_element()); return true; }
    if (IS("head")) { *out = node_value(c, n->owner->head()); return true; }
    if (IS("body")) { *out = node_value(c, n->owner->body()); return true; }
    if (IS("title")) {
      String title(vm.allocator());
      document_title(*n->owner, &title);
      *out = str_value(vm, title.view());
      return true;
    }
    if (IS("URL") || IS("documentURI")) { *out = str_value(vm, c.host.address()); return true; }
    if (IS("domain")) {
      AddressParts parts(vm.allocator());
      split_address(c.host.address(), &parts);
      *out = str_value(vm, parts.hostname.view());
      return true;
    }
    if (IS("cookie")) {
      String jar(vm.allocator());
      c.host.cookie_get(&jar);
      *out = str_value(vm, jar.view());
      return true;
    }
    if (IS("characterSet") || IS("charset") || IS("inputEncoding")) { *out = str_value(vm, "UTF-8"); return true; }
    if (IS("contentType")) { *out = str_value(vm, n->owner->content_type.empty() ? StringView("text/html") : n->owner->content_type.view()); return true; }
    if (IS("compatMode")) { *out = str_value(vm, "CSS1Compat"); return true; }
    if (IS("doctype")) {   // a page that began with <!DOCTYPE ...> has one: the built-in script makes the object
      *out = Value::make_null();
      Value make;
      if (!n->owner->quirks_mode && n->owner->content_type.empty() && vm.get_named(Value::make_object(vm.global()), "__doctype_of", &make) && Vm::is_callable(make)) {
        const Value arg = node_value(c, n);
        if (!vm.call(make, Value(), &arg, 1, out)) {
          String ignored(vm.allocator());
          vm.take_exception(&ignored);
          *out = Value::make_null();
        }
      }
      return true;
    }
    if (IS("defaultView")) { *out = Value::make_object(vm.global()); return true; }
    if (IS("location")) { *out = object_value(c.location); return true; }
    if (IS("activeElement") || IS("scrollingElement")) { *out = node_value(c, IS("activeElement") ? (c.host.focused() ? c.host.focused() : n->owner->body()) : n->owner->document_element()); return true; }
    if (IS("currentScript")) { *out = node_value(c, c.current_script); return true; }
    if (IS("hidden")) { *out = bool_value(false); return true; }
    if (IS("visibilityState")) { *out = str_value(vm, "visible"); return true; }
    if (IS("referrer") || IS("lastModified")) { *out = str_value(vm, ""); return true; }
    if (IS("forms") || IS("images") || IS("links") || IS("scripts") || IS("anchors") || IS("embeds")) {
      List<dom::Node*> list(vm.allocator());
      for (dom::Node* d = n->first_child; d; d = dom::next_in_tree(d, n)) {
        if (!d->is_element()) continue;
        if ((IS("forms") && d->is_tag("form")) || (IS("images") && d->is_tag("img")) || (IS("scripts") && d->is_tag("script")) || (IS("embeds") && d->is_tag("embed")) ||
            (IS("links") && (d->is_tag("a") || d->is_tag("area")) && dom::has_attribute(d, "href")) || (IS("anchors") && d->is_tag("a") && dom::has_attribute(d, "name")))
          list.push_back(d);
      }
      *out = Value::make_object(make_collection(c, list));
      return true;
    }
    return false;
  }
  if (!element) return false;
  // --- elements
  if (IS("tagName")) { *out = tag_name_value(vm, n); return true; }
  if (IS("localName")) { *out = str_value(vm, local_name_of(n)); return true; }
  if (IS("namespaceURI")) {
    if (!n->ns_set) *out = str_value(vm, "http://www.w3.org/1999/xhtml");
    else *out = n->ns.empty() ? Value::make_null() : str_value(vm, n->ns.view());
    return true;
  }
  if (IS("prefix")) {
    const uint64_t colon = n->qname.view().find(':');
    *out = colon == StringView::npos ? Value::make_null() : str_value(vm, n->qname.view().substr(0, colon));
    return true;
  }
  if (IS("innerHTML")) {
    String html(vm.allocator());
    dom::inner_html(n, &html);
    *out = str_value(vm, html.view());
    return true;
  }
  if (IS("outerHTML")) {
    String html(vm.allocator());
    dom::outer_html(n, &html);
    *out = str_value(vm, html.view());
    return true;
  }
  if (IS("innerText") || IS("outerText")) {
    String text(vm.allocator());
    dom::text_content(n, &text);
    *out = str_value(vm, text.view());
    return true;
  }
  if (IS("classList")) { *out = cached_object(vm, o, "__classList", make_token_list, n); return true; }
  if (IS("style")) { *out = cached_object(vm, o, "__style", make_style_object, n); return true; }
  if (IS("dataset")) { *out = cached_object(vm, o, "__dataset", make_dataset, n); return true; }
  if (IS("attributes")) { *out = attributes_value(c, n); return true; }
  if (IS("offsetWidth") || IS("offsetHeight") || IS("offsetLeft") || IS("offsetTop") || IS("clientWidth") || IS("clientHeight") || IS("clientLeft") || IS("clientTop") ||
      IS("scrollWidth") || IS("scrollHeight") || IS("scrollLeft") || IS("scrollTop")) {
    *out = num_value(0);
    return true;
  }
  if (IS("offsetParent")) { *out = Value::make_null(); return true; }
  if (IS("isContentEditable")) { *out = bool_value(false); return true; }
  if (IS("contentEditable")) { *out = str_value(vm, "inherit"); return true; }
  if (IS("draggable") || IS("spellcheck")) { *out = bool_value(false); return true; }
  if (IS("sheet")) {   // the CSSStyleSheet of a <style> or <link rel=stylesheet>: the built-in script makes it
    *out = Value::make_null();
    Value make;
    if (element && (n->is_tag("style") || n->is_tag("link")) && vm.get_named(Value::make_object(vm.global()), "__sheet_of", &make) && Vm::is_callable(make)) {
      const Value arg = node_value(c, n);
      if (!vm.call(make, Value(), &arg, 1, out)) {
        String ignored(vm.allocator());
        vm.take_exception(&ignored);
        *out = Value::make_null();
      }
    }
    return true;
  }
  if (element && IS("content") && n->is_tag("template")) {
    *out = node_value(c, dom::template_content(n));
    return true;
  }
  if (element && is_form_element(n) && form_get(c, n, k, out)) return true;
  if (IS("form")) {
    dom::Node* f = nullptr;
    if (is_control(n))
      for (dom::Node* p = n->parent; p && !f; p = p->parent)
        if (p->is_tag("form")) f = p;
    *out = node_value(c, f);
    return true;
  }
  if (IS("elements") && n->is_tag("form")) {
    List<dom::Node*> list(vm.allocator());
    for (dom::Node* d = n->first_child; d; d = dom::next_in_tree(d, n))
      if (d->is_element() && is_control(d)) list.push_back(d);
    *out = Value::make_object(make_collection(c, list));
    return true;
  }
  if ((IS("options") && n->is_tag("select")) || (IS("rows") && n->is_tag("table")) || (IS("cells") && n->is_tag("tr"))) {
    List<dom::Node*> list(vm.allocator());
    for (dom::Node* d = n->first_child; d; d = dom::next_in_tree(d, n)) {
      if (!d->is_element()) continue;
      if ((IS("options") && d->is_tag("option")) || (IS("rows") && d->is_tag("tr")) || (IS("cells") && (d->is_tag("td") || d->is_tag("th")))) list.push_back(d);
      if (IS("cells") && d->parent != n) continue;
    }
    *out = Value::make_object(make_collection(c, list));
    return true;
  }
  if (IS("selectedIndex") && n->is_tag("select")) {
    int index = -1, i = 0;
    for (dom::Node* d = n->first_child; d; d = dom::next_in_tree(d, n)) {
      if (!d->is_tag("option")) continue;
      if (dom::has_attribute(d, "selected") && index < 0) index = i;
      ++i;
    }
    *out = num_value(index < 0 && i > 0 ? 0 : index);
    return true;
  }
  if (IS("text") && (n->is_tag("option") || n->is_tag("a") || n->is_tag("script") || n->is_tag("title"))) {
    String text(vm.allocator());
    dom::text_content(n, &text);
    *out = str_value(vm, text.view());
    return true;
  }
  if (IS("value") && n->is_tag("textarea")) {
    String text(vm.allocator());
    dom::text_content(n, &text);
    *out = str_value(vm, text.view());
    return true;
  }
  if (element && (n->is_tag("img") || n->is_tag("video") || n->is_tag("audio") || n->is_tag("canvas") || n->is_tag("track")) && media_get(c, n, k, out)) return true;
  if (IS("type") && !dom::has_attribute(n, "type")) {
    if (n->is_tag("input")) { *out = str_value(vm, "text"); return true; }
    if (n->is_tag("button")) { *out = str_value(vm, "submit"); return true; }
  }
  if (const ReflectedAttribute* r = find_reflected(k)) {
    StringView v;
    const bool present = dom::get_attribute(n, r->attribute, &v);
    switch (r->kind) {
      case Reflect::Bool: *out = bool_value(present); return true;
      case Reflect::Int: {
        double d = r->js[0] == 't' ? -1 : 0;
        if (present) {
          uint64_t digits = 0;
          bool negative = false, any = false;
          StringView t = v.trimmed();
          if (t.starts_with('-')) { negative = true; t = t.substr(1); }
          for (uint64_t i = 0; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i) { digits = digits * 10 + static_cast<uint64_t>(t[i] - '0'); any = true; }
          if (any) d = negative ? -static_cast<double>(digits) : static_cast<double>(digits);
        }
        *out = num_value(d);
        return true;
      }
      case Reflect::Url: {
        if (!present) { *out = str_value(vm, ""); return true; }
        String resolved(vm.allocator());
        if (!v.empty() && resolve_reference(vm.allocator(), c.host.address(), v, &resolved)) *out = str_value(vm, resolved.view());
        else *out = str_value(vm, v);
        return true;
      }
      default: *out = str_value(vm, present ? v : StringView()); return true;
    }
  }
  if (element && n->is_tag("form") && form_lookup_named(c, n, k, out)) return true;
  return false;
}

HostClass::Set node_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  dom::Node* n = static_cast<dom::Node*>(o->host_data);
  DomBindings::Impl& c = impl(vm);
  const StringView k = key->view();
  if (k.size() < 2 || (k[0] == '_' && k[1] == '_')) return HostClass::Set::NotHandled;
  const bool element = n->is_element(), document = n->type == dom::NodeType::Document;
  String text(vm.allocator());
  auto done = [&](dom::DomError e) {
    if (e == dom::DomError::None) return HostClass::Set::Handled;
    throw_dom_error(vm, e);
    return HostClass::Set::Threw;
  };
  if (element && (n->is_tag("img") || n->is_tag("video") || n->is_tag("audio") || n->is_tag("canvas") || n->is_tag("track"))) {
    const HostClass::Set handled = media_set(c, n, k, value);
    if (handled != HostClass::Set::NotHandled) return handled;
  }
  if (element && is_form_element(n)) {
    const HostClass::Set handled = form_set(c, n, k, value);
    if (handled != HostClass::Set::NotHandled) return handled;
  }
  if (IS("textContent") || (element && (IS("innerText") || IS("outerText")))) {
    if (document) return HostClass::Set::Handled;
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    return done(dom::set_text_content(n, text.view()));
  }
  if (IS("nodeValue") || (n->is_text() && IS("data"))) {
    if (n->is_text() || n->type == dom::NodeType::Comment) {
      if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
      return done(dom::set_text_content(n, text.view()));
    }
    return HostClass::Set::Handled;
  }
  if (document) {
    if (IS("title")) {
      if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
      dom::Node* title = nullptr;
      for (dom::Node* d = n->first_child; d && !title; d = dom::next_in_tree(d, n))
        if (d->is_tag("title")) title = d;
      if (!title) {
        dom::Node* head = n->owner->head();
        if (!head) return HostClass::Set::Handled;
        title = n->owner->create_element("title");
        if (!title || dom::append_child(head, title) != dom::DomError::None) return done(dom::DomError::OutOfMemory);
      }
      return done(dom::set_text_content(title, text.view()));
    }
    if (IS("cookie")) {
      if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
      c.host.cookie_set(text.view());
      return HostClass::Set::Handled;
    }
    if (IS("location")) {
      if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
      c.host.navigate(text.view(), false);
      return HostClass::Set::Handled;
    }
    return HostClass::Set::NotHandled;
  }
  if (!element && n->shadow_host && IS("innerHTML")) {   // shadowRoot.innerHTML = ...
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    return done(dom::set_inner_html(n, text.view()));
  }
  if (!element) return HostClass::Set::NotHandled;
  if (IS("slot")) {
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    return done(dom::set_attribute(n, "slot", text.view()));
  }
  if (IS("innerHTML")) {
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    return done(dom::set_inner_html(n, text.view()));
  }
  if (IS("outerHTML")) {
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    dom::Node* parent = n->parent;
    if (!parent) return HostClass::Set::Handled;
    if (parent->type == dom::NodeType::Document) return done(dom::DomError::HierarchyRequest);
    dom::Node* fragment = dom::parse_fragment(*n->owner, text.view(), parent->name.view());
    if (!fragment) return done(dom::DomError::OutOfMemory);
    return done(dom::replace_child(parent, fragment, n));
  }
  if (IS("style")) {   // style = "..." sets the style attribute
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    return done(dom::set_attribute(n, "style", text.view()));
  }
  if (IS("scrollTop") || IS("scrollLeft") || IS("offsetWidth") || IS("offsetHeight") || IS("classList") || IS("dataset") || IS("attributes") || IS("tagName") || IS("children") ||
      IS("parentNode") || IS("childNodes") || IS("nodeType") || IS("nodeName"))
    return HostClass::Set::Handled;   // (read-only or without effect here)
  if (IS("value") && n->is_tag("textarea")) {
    if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
    return done(dom::set_text_content(n, text.view()));
  }
  if (const ReflectedAttribute* r = find_reflected(k)) {
    switch (r->kind) {
      case Reflect::Bool:
        if (Vm::to_boolean(value)) return done(dom::set_attribute(n, r->attribute, ""));
        dom::remove_attribute(n, r->attribute);
        return HostClass::Set::Handled;
      default:
        if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
        if (r->kind == Reflect::Int && value.is_number()) {
          String number(vm.allocator());
          const double d = value.u.number;
          if (d < 0) number.push_back('-');
          number.append_uint(static_cast<uint64_t>(d < 0 ? -d : d));
          return done(dom::set_attribute(n, r->attribute, number.view()));
        }
        return done(dom::set_attribute(n, r->attribute, text.view()));
    }
  }
  return HostClass::Set::NotHandled;
}

// --- Methods: nodes ------------------------------------------------------------------------------------------------------

NATIVE(node_append_child) {
  dom::Node* parent = this_node(vm, self);
  dom::Node* child = arg_node(vm, args, argc, 0);
  if (!parent || !child) return false;
  const dom::DomError e = dom::append_child(parent, child);
  if (e != dom::DomError::None) return throw_dom_error(vm, e);
  *result = args[0];
  return true;
}

NATIVE(node_insert_before) {
  dom::Node* parent = this_node(vm, self);
  dom::Node* child = arg_node(vm, args, argc, 0);
  if (!parent || !child) return false;
  dom::Node* ref = nullptr;
  if (!arg_at(args, argc, 1).is_nullish()) {
    ref = node_of(args[1]);
    if (!ref) return vm.throw_error(ErrorKind::TypeError, "parameter 2 is not of type 'Node'.");
  }
  const dom::DomError e = dom::insert_before(parent, child, ref);
  if (e != dom::DomError::None) return throw_dom_error(vm, e);
  *result = args[0];
  return true;
}

NATIVE(node_remove_child) {
  dom::Node* parent = this_node(vm, self);
  dom::Node* child = arg_node(vm, args, argc, 0);
  if (!parent || !child) return false;
  const dom::DomError e = dom::remove_child(parent, child);
  if (e != dom::DomError::None) return throw_dom_error(vm, e);
  *result = args[0];
  return true;
}

NATIVE(node_replace_child) {
  dom::Node* parent = this_node(vm, self);
  dom::Node* child = arg_node(vm, args, argc, 0);
  dom::Node* old_child = arg_node(vm, args, argc, 1);
  if (!parent || !child || !old_child) return false;
  const dom::DomError e = dom::replace_child(parent, child, old_child);
  if (e != dom::DomError::None) return throw_dom_error(vm, e);
  *result = args[1];
  return true;
}

NATIVE(node_clone) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (n->type == dom::NodeType::Document) return throw_dom(vm, "NotSupportedError", "A document cannot be cloned.");
  dom::Node* copy = dom::clone_node(n, Vm::to_boolean(arg_at(args, argc, 0)));
  if (!copy) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(impl(vm), copy);
  return true;
}

NATIVE(node_contains) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  dom::Node* other = node_of(arg_at(args, argc, 0));
  *result = bool_value(other && dom::contains(n, other));
  return true;
}

NATIVE(node_has_child_nodes) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  *result = bool_value(n->first_child != nullptr);
  return true;
}

NATIVE(node_get_root) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  while (n->parent) n = n->parent;
  *result = node_value(impl(vm), n);
  return true;
}

NATIVE(node_compare_position) {
  dom::Node* a = this_node(vm, self);
  dom::Node* b = arg_node(vm, args, argc, 0);
  if (!a || !b) return false;
  double mask = 0;
  const dom::Node* ra = a;
  const dom::Node* rb = b;
  while (ra->parent) ra = ra->parent;
  while (rb->parent) rb = rb->parent;
  if (a == b) mask = 0;
  else if (ra != rb) mask = 1 | 32;
  else if (dom::contains(a, b)) mask = 16 | 4;
  else if (dom::contains(b, a)) mask = 8 | 2;
  else mask = dom::compare_order(a, b) < 0 ? 4 : 2;
  *result = num_value(mask);
  return true;
}

NATIVE(node_normalize) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  for (dom::Node* d = n->first_child; d;) {
    dom::Node* next = dom::next_in_tree(d, n);
    if (d->is_text()) {
      if (d->data.empty()) {
        if (next == d->next || true) next = d->next ? d->next : dom::next_in_tree(d, n);
        dom::detach(d);
      } else if (d->next && d->next->is_text()) {
        d->data.append(d->next->data.view());
        dom::detach(d->next);
        next = d;   // (look at it again: there may be more)
      }
    }
    d = next;
  }
  return true;
}

NATIVE(node_is_same) {
  dom::Node* n = this_node(vm, self);
  *result = bool_value(n && n == node_of(arg_at(args, argc, 0)));
  return n != nullptr;
}

NATIVE(node_to_string) {
  dom::Node* n = node_of(self);
  String text(vm.allocator());
  text.append("[object ");
  if (!n) text.append("Object");
  else if (n->is_element()) text.append("HTMLElement");
  else if (n->is_text()) text.append("Text");
  else if (n->type == dom::NodeType::Comment) text.append("Comment");
  else if (n->type == dom::NodeType::Document) text.append("HTMLDocument");
  else text.append("DocumentFragment");
  text.push_back(']');
  *result = str_value(vm, text.view());
  return true;
}

// ChildNode / ParentNode
NATIVE(node_append) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  dom::Node* f = nodes_to_fragment(vm, args, argc);
  if (!f) return false;
  const dom::DomError e = dom::append_child(n, f);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(node_prepend) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  dom::Node* f = nodes_to_fragment(vm, args, argc);
  if (!f) return false;
  const dom::DomError e = dom::insert_before(n, f, n->first_child);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(node_replace_children) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  dom::Node* f = nodes_to_fragment(vm, args, argc);
  if (!f) return false;
  while (n->first_child) dom::remove_child(n, n->first_child);
  const dom::DomError e = dom::append_child(n, f);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(node_before) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->parent) return true;
  dom::Node* f = nodes_to_fragment(vm, args, argc);
  if (!f) return false;
  const dom::DomError e = dom::insert_before(n->parent, f, n);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(node_after) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->parent) return true;
  dom::Node* f = nodes_to_fragment(vm, args, argc);
  if (!f) return false;
  const dom::DomError e = dom::insert_before(n->parent, f, n->next);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(node_replace_with) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->parent) return true;
  dom::Node* f = nodes_to_fragment(vm, args, argc);
  if (!f) return false;
  const dom::DomError e = dom::replace_child(n->parent, f, n);
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(node_remove) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  dom::detach(n);
  return true;
}

NATIVE(text_split) {
  dom::Node* n = this_node(vm, self);
  if (!n || !n->is_text()) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  double at = 0;
  if (!vm.to_number(arg_at(args, argc, 0), &at)) return false;
  if (at != at || at < 0 || at > static_cast<double>(n->data.size())) return throw_dom(vm, "IndexSizeError", "The offset is larger than the node's length.");
  const uint64_t i = static_cast<uint64_t>(at);
  dom::Node* tail = n->owner->create_text(n->data.view().substr(i));
  if (!tail) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  n->data.truncate(i);
  if (n->parent) dom::insert_before(n->parent, tail, n->next);
  *result = node_value(impl(vm), tail);
  return true;
}

// --- Methods: elements -----------------------------------------------------------------------------------------------------

dom::Node* this_element(Vm& vm, const Value& self) {
  dom::Node* n = this_node(vm, self);
  if (n && !n->is_element()) {
    vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
    return nullptr;
  }
  return n;
}

NATIVE(el_get_attribute) {
  dom::Node* n = this_element(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  StringView v;
  *result = dom::get_attribute(n, name.view(), &v) ? str_value(vm, v) : Value::make_null();
  return true;
}

NATIVE(el_set_attribute) {
  dom::Node* n = this_element(vm, self);
  String name(vm.allocator()), value(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name) || !arg_text(vm, arg_at(args, argc, 1), &value)) return false;
  for (uint64_t i = 0; i < name.size(); ++i) {
    const char ch = name[i];
    if (is_space(ch) || ch == '/' || ch == '=' || ch == '>' || ch == '"' || ch == '\'' || ch == '<') return throw_dom(vm, "InvalidCharacterError", "The attribute name is not valid.");
  }
  if (name.empty()) return throw_dom(vm, "InvalidCharacterError", "The attribute name is not valid.");
  const dom::DomError e = dom::set_attribute(n, name.view(), value.view());
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(el_remove_attribute) {
  dom::Node* n = this_element(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  dom::remove_attribute(n, name.view());
  return true;
}

NATIVE(el_has_attribute) {
  dom::Node* n = this_element(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  *result = bool_value(dom::has_attribute(n, name.view()));
  return true;
}

NATIVE(el_has_attributes) {
  dom::Node* n = this_element(vm, self);
  if (!n) return false;
  *result = bool_value(n->attrs.count() > 0);
  return true;
}

NATIVE(el_toggle_attribute) {
  dom::Node* n = this_element(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  const bool present = dom::has_attribute(n, name.view());
  const bool want = argc > 1 && !args[1].is_undefined() ? Vm::to_boolean(args[1]) : !present;
  if (want && !present) dom::set_attribute(n, name.view(), "");
  else if (!want && present) dom::remove_attribute(n, name.view());
  *result = bool_value(want);
  return true;
}

NATIVE(el_get_attribute_names) {
  dom::Node* n = this_element(vm, self);
  if (!n) return false;
  JsObject* list = vm.new_array();
  for (uint64_t i = 0; i < n->attrs.count(); ++i) vm.array_push(list, str_value(vm, n->attrs[i].name.view()));
  *result = Value::make_object(list);
  return true;
}

bool query_impl(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, bool all) {
  dom::Node* n = this_node(vm, self);
  String selector(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &selector)) return false;
  List<dom::Node*> found(vm.allocator());
  if (!select(vm, n, selector.view(), all, &found)) return false;
  if (all) *result = Value::make_object(make_collection(impl(vm), found, false));
  else *result = found.count() ? node_value(impl(vm), found[0]) : Value::make_null();
  return true;
}
NATIVE(node_query_selector) { return query_impl(vm, self, args, argc, result, false); }
NATIVE(node_query_selector_all) { return query_impl(vm, self, args, argc, result, true); }

NATIVE(el_matches) {
  dom::Node* n = this_element(vm, self);
  String selector(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &selector)) return false;
  dom::SelectorSet set(vm.allocator());
  uint32_t added = 0;
  if (!set.parse(selector.view(), 0, &added, false)) return throw_dom(vm, "SyntaxError", "The selector is not valid.");
  *result = bool_value(set.matches_any(n, n));
  return true;
}

NATIVE(el_closest) {
  dom::Node* n = this_element(vm, self);
  String selector(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &selector)) return false;
  dom::SelectorSet set(vm.allocator());
  uint32_t added = 0;
  if (!set.parse(selector.view(), 0, &added, false)) return throw_dom(vm, "SyntaxError", "The selector is not valid.");
  for (dom::Node* p = n; p && p->is_element(); p = p->parent) {
    if (set.matches_any(p, n)) {
      *result = node_value(impl(vm), p);
      return true;
    }
  }
  *result = Value::make_null();
  return true;
}

NATIVE(node_get_by_tag) {
  dom::Node* n = this_node(vm, self);
  String tag(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &tag)) return false;
  String lowered(vm.allocator());
  for (uint64_t i = 0; i < tag.size(); ++i) lowered.push_back(lower_char(tag[i]));
  List<dom::Node*> found(vm.allocator());
  const StringView t = lowered.view();
  descendants_with(n, &found, has_tag, &t);
  *result = Value::make_object(make_collection(impl(vm), found));
  return true;
}

NATIVE(node_get_by_class) {
  dom::Node* n = this_node(vm, self);
  String names(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &names)) return false;
  List<dom::Node*> found(vm.allocator());
  for (dom::Node* d = n->first_child; d; d = dom::next_in_tree(d, n)) {
    if (!d->is_element()) continue;
    bool all = false;
    uint64_t i = 0;
    const StringView v = names.view();
    all = true;
    bool any = false;
    while (i < v.size()) {
      while (i < v.size() && is_space(v[i])) ++i;
      const uint64_t start = i;
      while (i < v.size() && !is_space(v[i])) ++i;
      if (i > start) {
        any = true;
        if (!dom::has_class(d, v.substr(start, i - start))) all = false;
      }
    }
    if (any && all) found.push_back(d);
  }
  *result = Value::make_object(make_collection(impl(vm), found));
  return true;
}

NATIVE(node_get_by_name) {
  dom::Node* n = this_node(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  List<dom::Node*> found(vm.allocator());
  for (dom::Node* d = n->first_child; d; d = dom::next_in_tree(d, n)) {
    StringView v;
    if (d->is_element() && dom::get_attribute(d, "name", &v) && v.equals(name.view())) found.push_back(d);
  }
  *result = Value::make_object(make_collection(impl(vm), found));
  return true;
}

NATIVE(el_insert_adjacent_html) {
  dom::Node* n = this_element(vm, self);
  String where(vm.allocator()), html(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &where) || !arg_text(vm, arg_at(args, argc, 1), &html)) return false;
  const StringView w = where.view();
  dom::Node* parent = w.equals_ignore_case("beforebegin") || w.equals_ignore_case("afterend") ? n->parent : n;
  if (!parent) return throw_dom(vm, "NoModificationAllowedError", "The element has no parent.");
  dom::Node* f = dom::parse_fragment(*n->owner, html.view(), parent->name.view());
  if (!f) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  dom::DomError e;
  if (w.equals_ignore_case("beforebegin")) e = dom::insert_before(parent, f, n);
  else if (w.equals_ignore_case("afterbegin")) e = dom::insert_before(n, f, n->first_child);
  else if (w.equals_ignore_case("beforeend")) e = dom::append_child(n, f);
  else if (w.equals_ignore_case("afterend")) e = dom::insert_before(parent, f, n->next);
  else return throw_dom(vm, "SyntaxError", "The value provided is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'.");
  return e == dom::DomError::None ? true : throw_dom_error(vm, e);
}

NATIVE(el_insert_adjacent_element) {
  dom::Node* n = this_element(vm, self);
  String where(vm.allocator());
  dom::Node* e = arg_node(vm, args, argc, 1);
  if (!n || !e || !arg_text(vm, arg_at(args, argc, 0), &where)) return false;
  const StringView w = where.view();
  dom::DomError err;
  if (w.equals_ignore_case("beforebegin") && n->parent) err = dom::insert_before(n->parent, e, n);
  else if (w.equals_ignore_case("afterbegin")) err = dom::insert_before(n, e, n->first_child);
  else if (w.equals_ignore_case("beforeend")) err = dom::append_child(n, e);
  else if (w.equals_ignore_case("afterend") && n->parent) err = dom::insert_before(n->parent, e, n->next);
  else return throw_dom(vm, "SyntaxError", "The position is not valid.");
  if (err != dom::DomError::None) return throw_dom_error(vm, err);
  *result = args[1];
  return true;
}

NATIVE(el_insert_adjacent_text) {
  dom::Node* n = this_element(vm, self);
  String where(vm.allocator()), text(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &where) || !arg_text(vm, arg_at(args, argc, 1), &text)) return false;
  dom::Node* t = n->owner->create_text(text.view());
  if (!t) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  const StringView w = where.view();
  dom::DomError err;
  if (w.equals_ignore_case("beforebegin") && n->parent) err = dom::insert_before(n->parent, t, n);
  else if (w.equals_ignore_case("afterbegin")) err = dom::insert_before(n, t, n->first_child);
  else if (w.equals_ignore_case("beforeend")) err = dom::append_child(n, t);
  else if (w.equals_ignore_case("afterend") && n->parent) err = dom::insert_before(n->parent, t, n->next);
  else return throw_dom(vm, "SyntaxError", "The position is not valid.");
  return err == dom::DomError::None ? true : throw_dom_error(vm, err);
}

NATIVE(el_rect) {
  JsObject* r = vm.new_plain();
  for (const char* k : {"x", "y", "width", "height", "top", "left", "right", "bottom"}) vm.define_named(r, k, num_value(0));
  *result = Value::make_object(r);
  return true;
}

NATIVE(el_empty_list) {
  *result = Value::make_object(vm.new_array());
  return true;
}

NATIVE(el_true) {
  *result = bool_value(true);
  return true;
}

NATIVE(do_nothing) { return true; }


NATIVE(collection_item) {
  double d;
  if (!self.is_object() || self.as_object()->okind != ObjKind::Array || !vm.to_number(arg_at(args, argc, 0), &d)) {
    *result = Value::make_null();
    return true;
  }
  JsObject* a = self.as_object();
  *result = d >= 0 && d < a->length && d == static_cast<double>(static_cast<uint32_t>(d)) ? a->items[static_cast<uint32_t>(d)] : Value::make_null();
  return true;
}

NATIVE(collection_named) {
  String name(vm.allocator());
  *result = Value::make_null();
  if (!self.is_object() || self.as_object()->okind != ObjKind::Array || !arg_text(vm, arg_at(args, argc, 0), &name)) return true;
  JsObject* a = self.as_object();
  for (uint32_t i = 0; i < a->length; ++i) {
    dom::Node* n = node_of(a->items[i]);
    StringView v;
    if (n && n->is_element() && ((dom::get_attribute(n, "id", &v) && v.equals(name.view())) || (dom::get_attribute(n, "name", &v) && v.equals(name.view())))) {
      *result = a->items[i];
      return true;
    }
  }
  return true;
}

// --- Methods: documents ----------------------------------------------------------------------------------------------------

bool valid_tag_name(StringView t) {
  if (t.empty()) return false;
  for (uint64_t i = 0; i < t.size(); ++i) {
    const char ch = t[i];
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9' && i > 0) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    if (!ok) return false;
  }
  return true;
}

NATIVE(doc_create_element) {
  dom::Node* d = this_node(vm, self);
  String tag(vm.allocator());
  if (!d || !arg_text(vm, arg_at(args, argc, 0), &tag)) return false;
  if (!valid_tag_name(tag.view())) return throw_dom(vm, "InvalidCharacterError", "The tag name provided is not a valid name.");
  dom::Node* e = d->owner->create_element(tag.view());
  if (!e) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(impl(vm), e);
  return true;
}

NATIVE(doc_create_element_ns) {
  dom::Node* d = this_node(vm, self);
  String ns(vm.allocator()), qname(vm.allocator());
  if (!d || !arg_text(vm, arg_at(args, argc, 0), &ns) || !arg_text(vm, arg_at(args, argc, 1), &qname)) return false;
  const uint64_t colon = qname.view().find(':');
  const StringView local = colon == StringView::npos ? qname.view() : qname.view().substr(colon + 1);
  if (!valid_tag_name(local) || (colon != StringView::npos && !valid_tag_name(qname.view().substr(0, colon)))) return throw_dom(vm, "InvalidCharacterError", "The tag name provided is not a valid name.");
  const Value ns_arg = arg_at(args, argc, 0);
  const bool has_ns = !ns_arg.is_nullish() && !ns.empty();
  if (colon != StringView::npos && !has_ns) return throw_dom(vm, "NamespaceError", "The qualified name has a prefix but the namespace is null.");
  const StringView prefix = colon == StringView::npos ? StringView() : qname.view().substr(0, colon);
  if (prefix.equals("xml") && !ns.view().equals("http://www.w3.org/XML/1998/namespace")) return throw_dom(vm, "NamespaceError", "The prefix 'xml' is reserved for the XML namespace.");
  dom::Node* e = d->owner->create_element(local);
  if (!e) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  const bool html = has_ns && ns.view().equals("http://www.w3.org/1999/xhtml");
  const bool xml_doc = !d->owner->content_type.empty();
  if (!html || xml_doc || colon != StringView::npos) {   // (an element of HTML in a page of HTML is the same as one made by createElement)
    e->ns_set = true;
    e->ns.assign(has_ns ? ns.view() : StringView());
  }
  if (colon != StringView::npos || xml_doc || !html) e->qname.assign(qname.view());
  *result = node_value(impl(vm), e);
  return true;
}

NATIVE(doc_create_text) {
  dom::Node* d = this_node(vm, self);
  String text(vm.allocator());
  if (!d || !arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  dom::Node* t = d->owner->create_text(text.view());
  if (!t) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(impl(vm), t);
  return true;
}

NATIVE(doc_create_comment) {
  dom::Node* d = this_node(vm, self);
  String text(vm.allocator());
  if (!d || !arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  dom::Node* t = d->owner->create_comment(text.view());
  if (!t) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(impl(vm), t);
  return true;
}

NATIVE(doc_create_fragment) {
  dom::Node* d = this_node(vm, self);
  if (!d) return false;
  dom::Node* f = d->owner->create_fragment();
  if (!f) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(impl(vm), f);
  return true;
}

NATIVE(doc_get_by_id) {
  dom::Node* d = this_node(vm, self);
  String id(vm.allocator());
  if (!d || !arg_text(vm, arg_at(args, argc, 0), &id)) return false;
  dom::Node* found = nullptr;
  if (d->type == dom::NodeType::Fragment) {   // (a fragment or a shadow root looks in itself)
    for (dom::Node* n = d->first_child; n && !found; n = dom::next_in_tree(n, d)) {
      StringView value;
      if (n->is_element() && dom::get_attribute(n, "id", &value) && value.equals(id.view())) found = n;
    }
  } else {
    found = d->owner->element_by_id(id.view());
  }
  *result = node_value(impl(vm), found);
  return true;
}

NATIVE(doc_import_node) {
  dom::Node* n = arg_node(vm, args, argc, 0);
  if (!n) return false;
  if (n->type == dom::NodeType::Document) return throw_dom(vm, "NotSupportedError", "A document cannot be imported.");
  dom::Node* copy = dom::clone_node(n, Vm::to_boolean(arg_at(args, argc, 1)));
  if (!copy) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(impl(vm), copy);
  return true;
}

NATIVE(doc_adopt_node) {
  dom::Node* n = arg_node(vm, args, argc, 0);
  if (!n) return false;
  dom::detach(n);
  *result = args[0];
  return true;
}

// document.write: while the page is still being read the text goes into the page where the script is; afterwards it is added to the end of the body.
bool write_impl(Vm& vm, const Value* args, uint32_t argc, bool newline) {
  DomBindings::Impl& c = impl(vm);
  String text(vm.allocator());
  for (uint32_t i = 0; i < argc; ++i) {
    JsString* s;
    if (!vm.to_string(args[i], &s)) return false;
    text.append(s->view());
  }
  if (newline) text.push_back('\n');
  Value state;
  JsString* ready = vm.intern("readyState");
  Value doc_value = node_value(c, c.doc.root());
  bool loading = true;
  if (vm.get_plain(doc_value.as_object(), ready, &state) && state.is_string()) loading = state.as_string()->view().equals("loading");
  if (loading) {
    if (vm.host()) vm.host()->document_write(text.view(), false);
    return true;
  }
  dom::Node* body = c.doc.body();
  if (!body) return true;
  dom::Node* f = dom::parse_fragment(c.doc, text.view(), "body");
  if (!f) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  return dom::append_child(body, f) == dom::DomError::None ? true : throw_dom_error(vm, dom::DomError::HierarchyRequest);
}
NATIVE(doc_write) { return write_impl(vm, args, argc, false); }
NATIVE(doc_writeln) { return write_impl(vm, args, argc, true); }

NATIVE(doc_has_focus) {
  *result = bool_value(true);
  return true;
}

NATIVE(illegal_constructor) { return vm.throw_error(ErrorKind::TypeError, "Illegal constructor"); }

// new EventTarget(): an object that has listeners and can be dispatched to (and the base of the script classes that are event targets).
NATIVE(event_target_constructor) {
  if (!self.is_object() || self.as_object() == vm.global() || node_of(self)) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'EventTarget': Please use the 'new' operator.");
  *result = self;
  return true;
}

NATIVE(image_constructor) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* img = c.doc.create_element("img");
  if (!img) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  if (argc > 0 && args[0].is_number()) {
    String n(vm.allocator());
    n.append_uint(static_cast<uint64_t>(args[0].u.number));
    dom::set_attribute(img, "width", n.view());
  }
  if (argc > 1 && args[1].is_number()) {
    String n(vm.allocator());
    n.append_uint(static_cast<uint64_t>(args[1].u.number));
    dom::set_attribute(img, "height", n.view());
  }
  *result = node_value(c, img);
  return true;
}

NATIVE(audio_constructor) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* audio = c.doc.create_element("audio");
  if (!audio) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  if (argc > 0 && !args[0].is_undefined()) {
    String src(vm.allocator());
    if (!arg_text(vm, args[0], &src)) return false;
    dom::set_attribute(audio, "src", src.view());
  }
  dom::set_attribute(audio, "preload", "auto");
  *result = node_value(c, audio);
  return true;
}

NATIVE(dom_parser_parse) {
  DomBindings::Impl& c = impl(vm);
  String html(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &html)) return false;
  dom::Document* d = c.host.parse_document(html.view());
  if (!d) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  *result = node_value(c, d->root());
  return true;
}

NATIVE(dom_parser_constructor) { return true; }

NATIVE(exception_constructor) {
  // DOMException(message, name)
  JsObject* e = self.is_object() ? self.as_object() : nullptr;
  if (!e) return true;
  String message(vm.allocator()), name(vm.allocator());
  arg_text(vm, arg_at(args, argc, 0), &message);
  if (argc > 1) arg_text(vm, args[1], &name);
  else name.assign("Error");
  vm.define_named(e, "message", str_value(vm, message.view()), kWritable);
  vm.define_named(e, "name", str_value(vm, name.view()), kWritable);
  vm.define_named(e, "code", num_value(dom_exception_code(name.view())), kWritable);
  *result = self;
  return true;
}

JsObject* make_class(DomBindings::Impl& c, const char* name, NativeFn ctor, JsObject* proto) {
  Vm& vm = c.vm;
  JsObject* f = vm.new_native(ctor, name, 0);
  vm.define(f, vm.prototype_name(), Value::make_object(proto), 0);
  vm.define(proto, vm.intern("constructor"), Value::make_object(f), kWritable);
  vm.define_named(vm.global(), name, Value::make_object(f), kWritable);
  return f;
}

}  // namespace

const HostClass kNodeHostClass = {"Node", node_get, node_set, nullptr, nullptr};

// --- Shadow trees

// element.attachShadow({ mode })
NATIVE(el_attach_shadow) {
  dom::Node* n = this_node(vm, self);
  if (!n || !n->is_element()) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  const Value init = arg_at(args, argc, 0);
  Value mode;
  if (!init.is_object() || !vm.get_named(init, "mode", &mode) || !mode.is_string() || !(mode.as_string()->view().equals("open") || mode.as_string()->view().equals("closed")))
    return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'attachShadow' on 'Element': The provided value is not a valid enum value of type ShadowRootMode.");
  if (n->shadow) return throw_dom(vm, "InvalidStateError", "Failed to execute 'attachShadow' on 'Element': Shadow root cannot be created on a host which already hosts a shadow tree.");
  dom::Node* root = n->owner->create_fragment();
  if (!root) return throw_dom_error(vm, dom::DomError::OutOfMemory);
  root->shadow_host = n;
  root->shadow_closed = mode.as_string()->view().equals("closed");
  n->shadow = root;
  n->owner->touch();
  *result = node_value(impl(vm), root);
  return true;
}

NATIVE(slot_assigned_nodes) {
  dom::Node* n = this_node(vm, self);
  if (!n) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  List<dom::Node*> nodes(vm.allocator());
  dom::slot_assigned_nodes(n, &nodes);
  JsObject* out = vm.new_array(static_cast<uint32_t>(nodes.count()));
  for (uint64_t i = 0; i < nodes.count(); ++i) vm.array_push(out, node_value(impl(vm), nodes[i]));
  *result = Value::make_object(out);
  return true;
}

NATIVE(slot_assigned_elements) {
  dom::Node* n = this_node(vm, self);
  if (!n) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  List<dom::Node*> nodes(vm.allocator());
  dom::slot_assigned_nodes(n, &nodes);
  JsObject* out = vm.new_array(static_cast<uint32_t>(nodes.count()));
  for (uint64_t i = 0; i < nodes.count(); ++i)
    if (nodes[i]->is_element()) vm.array_push(out, node_value(impl(vm), nodes[i]));
  *result = Value::make_object(out);
  return true;
}

// --- Mutation records and custom elements: the script side is in WebPrelude.cpp (the DOM script)

void mutation_hook(void* context) {
  DomBindings::Impl& c = *static_cast<DomBindings::Impl*>(context);
  Value fn;
  if (c.vm.get_named(Value::make_object(c.vm.global()), "__deliver_mutations", &fn) && Vm::is_callable(fn)) c.vm.enqueue_microtask(fn);
  c.vm.request_epilogue();   // (custom element reactions run as soon as the call that made the change is over)
}

void mutation_epilogue(void* context) {
  DomBindings::Impl& c = *static_cast<DomBindings::Impl*>(context);
  Value fn;
  Value ignored;
  if (c.vm.get_named(Value::make_object(c.vm.global()), "__process_mutations", &fn) && Vm::is_callable(fn)) c.vm.call(fn, Value(), nullptr, 0, &ignored);
}

NATIVE(watch_node) {
  (void)self;
  (void)result;
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = DomBindings::unwrap(arg_at(args, argc, 0));
  if (n) c.doc.watch(n);
  return true;
}

NATIVE(mutations_on) {
  (void)self;
  (void)args;
  (void)argc;
  (void)result;
  DomBindings::Impl& c = impl(vm);
  c.doc.set_recording(true, mutation_hook, &c);
  vm.set_epilogue(mutation_epilogue, &c);
  return true;
}

// __take_mutations(): the changes since the last call, as an array of { type, target, addedNodes, removedNodes, previousSibling, nextSibling, attributeName, oldValue }
NATIVE(take_mutations) {
  (void)self;
  (void)args;
  (void)argc;
  DomBindings::Impl& c = impl(vm);
  List<dom::Mutation>& log = c.doc.log();
  JsObject* out = vm.new_array(static_cast<uint32_t>(log.count()));
  *result = Value::make_object(out);
  auto node_or_null = [&](dom::Node* n) { return n ? node_value(c, n) : Value::make_null(); };
  for (uint64_t i = 0; i < log.count(); ++i) {
    const dom::Mutation& m = log[i];
    JsObject* r = vm.new_plain();
    const char* type = m.kind == dom::Mutation::Kind::ChildList ? "childList" : m.kind == dom::Mutation::Kind::Attributes ? "attributes" : "characterData";
    vm.define_named(r, "type", str_value(vm, type));
    vm.define_named(r, "target", node_or_null(m.target));
    JsObject* added = vm.new_array(1);
    JsObject* removed = vm.new_array(1);
    if (m.added) vm.array_push(added, node_or_null(m.added));
    if (m.removed) vm.array_push(removed, node_or_null(m.removed));
    vm.define_named(r, "addedNodes", Value::make_object(added));
    vm.define_named(r, "removedNodes", Value::make_object(removed));
    vm.define_named(r, "previousSibling", node_or_null(m.previous));
    vm.define_named(r, "nextSibling", node_or_null(m.next));
    vm.define_named(r, "attributeName", m.kind == dom::Mutation::Kind::Attributes ? str_value(vm, m.attribute.view()) : Value::make_null());
    vm.define_named(r, "attributeNamespace", Value::make_null());
    vm.define_named(r, "oldValue", m.had_old_value ? str_value(vm, m.old_value.view()) : Value::make_null());
    JsObject* watchers = vm.new_array(2);   // the observed nodes this happened in
    uint64_t at = 0;
    while (at < m.watchers.size()) {
      uint64_t end = at, serial = 0;
      while (end < m.watchers.size() && m.watchers[end] != ',') serial = serial * 10 + static_cast<uint64_t>(m.watchers[end++] - '0');
      if (dom::Node* w = c.doc.by_serial(static_cast<uint32_t>(serial))) vm.array_push(watchers, node_or_null(w));
      at = end + 1;
    }
    vm.define_named(r, "watchers", Value::make_object(watchers));
    vm.array_push(out, Value::make_object(r));
  }
  log.clear();
  return true;
}

// __register_tag(name, prototype): elements with that tag name get this prototype (a custom element)
NATIVE(register_tag) {
  (void)self;
  DomBindings::Impl& c = impl(vm);
  JsString* name;
  if (!vm.to_string(arg_at(args, argc, 0), &name) || !arg_at(args, argc, 1).is_object()) return false;
  vm.define(c.tag_protos, vm.intern(name->view()), args[1], 0);
  *result = Value();
  return true;
}

// `new MyElement()` for a class that extends HTMLElement: a document node whose tag was registered for a prototype in the chain
JsObject* construct_element(void* user, JsObject* proto) {
  DomBindings::Impl& c = *static_cast<DomBindings::Impl*>(user);
  if (!c.tag_protos || !proto) return nullptr;
  for (JsObject* p = proto; p && p != c.html_proto; p = p->proto) {
    for (uint32_t i = 0; i < c.tag_protos->prop_count; ++i) {
      if (!c.tag_protos->props[i].value.is_object() || c.tag_protos->props[i].value.as_object() != p) continue;
      const StringView tag = c.tag_protos->props[i].key->view();
      if (tag.find('-') == StringView::npos) continue;
      dom::Node* e = c.doc.create_element(tag);
      if (!e) return nullptr;
      JsObject* w = c.owner.wrap(e);
      if (w) {
        w->proto = proto;
        c.vm.define_named(w, "__ce", Value::make_number(1), 0);   // (the constructor that is about to run is the upgrade)
      }
      return w;
    }
  }
  return nullptr;
}

bool install_nodes(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__mutations_on", mutations_on, 0);
  vm.define_native(g, "__watch_node", watch_node, 1);
  vm.define_native(g, "__take_mutations", take_mutations, 0);
  vm.define_native(g, "__register_tag", register_tag, 2);
  vm.set_construct_hook(construct_element, &c);
  vm.define_native(g, "__collection_item", collection_item, 1);
  vm.define_native(g, "__collection_named", collection_named, 1);
  {   // (they are used as values: kept, but not shown)
    Value v;
    vm.get_named(Value::make_object(g), "__collection_item", &v);
    vm.define_named(g, "__collection_item", v, 0);
    vm.get_named(Value::make_object(g), "__collection_named", &v);
    vm.define_named(g, "__collection_named", v, 0);
  }
  JsObject* event_target = vm.new_plain();
  c.node_proto = vm.new_object(event_target);
  c.element_proto = vm.new_object(c.node_proto);
  c.html_proto = vm.new_object(c.element_proto);
  c.character_data_proto = vm.new_object(c.node_proto);   // (CharacterData: what Text and Comment are)
  c.text_proto = vm.new_object(c.character_data_proto);
  c.comment_proto = vm.new_object(c.character_data_proto);
  c.document_proto = vm.new_object(c.node_proto);
  c.fragment_proto = vm.new_object(c.node_proto);
  vm.define_named(g, "__event_target_proto", Value::make_object(event_target), 0);

  JsObject* np = c.node_proto;
  vm.define_native(np, "appendChild", node_append_child, 1);
  vm.define_native(np, "insertBefore", node_insert_before, 2);
  vm.define_native(np, "removeChild", node_remove_child, 1);
  vm.define_native(np, "replaceChild", node_replace_child, 2);
  vm.define_native(np, "cloneNode", node_clone, 1);
  vm.define_native(np, "contains", node_contains, 1);
  vm.define_native(np, "hasChildNodes", node_has_child_nodes, 0);
  vm.define_native(np, "getRootNode", node_get_root, 0);
  vm.define_native(np, "compareDocumentPosition", node_compare_position, 1);
  vm.define_native(np, "normalize", node_normalize, 0);
  vm.define_native(np, "isSameNode", node_is_same, 1);
  vm.define_native(np, "toString", node_to_string, 0);
  vm.define_native(np, "append", node_append, 1);
  vm.define_native(np, "prepend", node_prepend, 1);
  vm.define_native(np, "replaceChildren", node_replace_children, 1);
  vm.define_native(np, "querySelector", node_query_selector, 1);
  vm.define_native(np, "querySelectorAll", node_query_selector_all, 1);
  vm.define_native(np, "getElementsByTagName", node_get_by_tag, 1);
  vm.define_native(np, "getElementsByClassName", node_get_by_class, 1);
  vm.define_native(np, "before", node_before, 1);
  vm.define_native(np, "after", node_after, 1);
  vm.define_native(np, "replaceWith", node_replace_with, 1);
  vm.define_native(np, "remove", node_remove, 0);
  vm.define_native(c.text_proto, "splitText", text_split, 1);

  JsObject* ep = c.element_proto;
  vm.define_native(ep, "getAttribute", el_get_attribute, 1);
  vm.define_native(ep, "setAttribute", el_set_attribute, 2);
  vm.define_native(ep, "removeAttribute", el_remove_attribute, 1);
  vm.define_native(ep, "hasAttribute", el_has_attribute, 1);
  vm.define_native(ep, "hasAttributes", el_has_attributes, 0);
  vm.define_native(ep, "toggleAttribute", el_toggle_attribute, 1);
  vm.define_native(ep, "getAttributeNames", el_get_attribute_names, 0);
  vm.define_native(ep, "matches", el_matches, 1);
  vm.define_native(ep, "webkitMatchesSelector", el_matches, 1);
  vm.define_native(ep, "closest", el_closest, 1);
  vm.define_native(ep, "insertAdjacentHTML", el_insert_adjacent_html, 2);
  vm.define_native(ep, "insertAdjacentElement", el_insert_adjacent_element, 2);
  vm.define_native(ep, "insertAdjacentText", el_insert_adjacent_text, 2);
  vm.define_native(ep, "getBoundingClientRect", el_rect, 0);
  vm.define_native(ep, "getClientRects", el_empty_list, 0);
  vm.define_native(ep, "checkVisibility", el_true, 0);
  vm.define_native(ep, "focus", do_nothing, 0);
  vm.define_native(ep, "blur", do_nothing, 0);
  vm.define_native(ep, "scrollIntoView", do_nothing, 0);
  vm.define_native(ep, "scrollTo", do_nothing, 0);
  vm.define_native(ep, "scrollBy", do_nothing, 0);
  vm.define_native(ep, "getElementsByName", node_get_by_name, 1);

  JsObject* dp = c.document_proto;
  vm.define_native(dp, "createElement", doc_create_element, 1);
  vm.define_native(dp, "createElementNS", doc_create_element_ns, 2);
  vm.define_native(dp, "createTextNode", doc_create_text, 1);
  vm.define_native(dp, "createComment", doc_create_comment, 1);
  vm.define_native(dp, "createDocumentFragment", doc_create_fragment, 0);
  vm.define_native(dp, "getElementById", doc_get_by_id, 1);
  vm.define_native(dp, "getElementsByName", node_get_by_name, 1);
  vm.define_native(dp, "importNode", doc_import_node, 2);
  vm.define_native(dp, "adoptNode", doc_adopt_node, 1);
  vm.define_native(dp, "write", doc_write, 1);
  vm.define_native(dp, "writeln", doc_writeln, 1);
  vm.define_native(dp, "open", do_nothing, 0);
  vm.define_native(dp, "close", do_nothing, 0);
  vm.define_native(dp, "hasFocus", doc_has_focus, 0);
  vm.define_native(dp, "elementFromPoint", el_empty_list, 2);
  vm.define_native(c.fragment_proto, "getElementById", doc_get_by_id, 1);

  // the constructors pages use for instanceof and constants
  JsObject* node_ctor = make_class(c, "Node", illegal_constructor, np);
  static const struct {
    const char* name;
    int value;
  } kConstants[] = {{"ELEMENT_NODE", 1}, {"ATTRIBUTE_NODE", 2}, {"TEXT_NODE", 3}, {"CDATA_SECTION_NODE", 4}, {"PROCESSING_INSTRUCTION_NODE", 7}, {"COMMENT_NODE", 8},
                    {"DOCUMENT_NODE", 9}, {"DOCUMENT_TYPE_NODE", 10}, {"DOCUMENT_FRAGMENT_NODE", 11}, {"DOCUMENT_POSITION_DISCONNECTED", 1}, {"DOCUMENT_POSITION_PRECEDING", 2},
                    {"DOCUMENT_POSITION_FOLLOWING", 4}, {"DOCUMENT_POSITION_CONTAINS", 8}, {"DOCUMENT_POSITION_CONTAINED_BY", 16}};
  for (const auto& k : kConstants) {
    vm.define_named(node_ctor, k.name, num_value(k.value), 0);
    vm.define_named(np, k.name, num_value(k.value), 0);
  }
  make_class(c, "EventTarget", event_target_constructor, event_target);
  vm.define_native(ep, "attachShadow", el_attach_shadow, 1);
  make_class(c, "Element", illegal_constructor, ep);
  make_class(c, "SVGElement", illegal_constructor, vm.new_object(ep));
  make_class(c, "HTMLUnknownElement", illegal_constructor, vm.new_object(c.html_proto));
  {   // `window` is a Window, which is an EventTarget: addEventListener and the rest are inherited, as pages that patch Window.prototype expect
    JsObject* window_proto = vm.new_object(event_target);
    make_class(c, "Window", illegal_constructor, window_proto);
    g->proto = window_proto;
  }
  make_class(c, "HTMLElement", illegal_constructor, c.html_proto);
  {   // one interface for each kind of element, so instanceof tells an <input> from a <div>
    static const struct {
      const char* interface;
      const char* tags;   // space separated
    } kInterfaces[] = {
        {"HTMLAnchorElement", "a"}, {"HTMLAreaElement", "area"}, {"HTMLBaseElement", "base"}, {"HTMLQuoteElement", "blockquote q"}, {"HTMLBodyElement", "body"},
        {"HTMLBRElement", "br"}, {"HTMLButtonElement", "button"}, {"HTMLCanvasElement", "canvas"}, {"HTMLDListElement", "dl"}, {"HTMLDataElement", "data"},
        {"HTMLDataListElement", "datalist"}, {"HTMLDetailsElement", "details"}, {"HTMLDialogElement", "dialog"}, {"HTMLDivElement", "div"}, {"HTMLEmbedElement", "embed"},
        {"HTMLFieldSetElement", "fieldset"}, {"HTMLFormElement", "form"}, {"HTMLHeadingElement", "h1 h2 h3 h4 h5 h6"}, {"HTMLHeadElement", "head"}, {"HTMLHRElement", "hr"},
        {"HTMLHtmlElement", "html"}, {"HTMLIFrameElement", "iframe"}, {"HTMLImageElement", "img"}, {"HTMLInputElement", "input"}, {"HTMLLabelElement", "label"},
        {"HTMLLegendElement", "legend"}, {"HTMLLIElement", "li"}, {"HTMLLinkElement", "link"}, {"HTMLMapElement", "map"}, {"HTMLMenuElement", "menu"},
        {"HTMLMetaElement", "meta"}, {"HTMLMeterElement", "meter"}, {"HTMLModElement", "ins del"}, {"HTMLObjectElement", "object"}, {"HTMLOListElement", "ol"},
        {"HTMLOptGroupElement", "optgroup"}, {"HTMLOptionElement", "option"}, {"HTMLOutputElement", "output"}, {"HTMLParagraphElement", "p"}, {"HTMLParamElement", "param"},
        {"HTMLPictureElement", "picture"}, {"HTMLPreElement", "pre"}, {"HTMLProgressElement", "progress"}, {"HTMLScriptElement", "script"}, {"HTMLSelectElement", "select"},
        {"HTMLSlotElement", "slot"}, {"HTMLSourceElement", "source"}, {"HTMLSpanElement", "span"}, {"HTMLStyleElement", "style"}, {"HTMLTableCaptionElement", "caption"},
        {"HTMLTableCellElement", "td th"}, {"HTMLTableColElement", "col colgroup"}, {"HTMLTableElement", "table"}, {"HTMLTableRowElement", "tr"},
        {"HTMLTableSectionElement", "tbody thead tfoot"}, {"HTMLTemplateElement", "template"}, {"HTMLTextAreaElement", "textarea"}, {"HTMLTimeElement", "time"},
        {"HTMLTitleElement", "title"}, {"HTMLTrackElement", "track"}, {"HTMLUListElement", "ul"}};
    c.tag_protos = vm.new_plain();
    vm.define_named(g, "__tag_protos", Value::make_object(c.tag_protos), 0);   // (kept alive; also what the page cannot overwrite)
    JsObject* media = vm.new_object(c.html_proto);
    JsObject* media_class = make_class(c, "HTMLMediaElement", illegal_constructor, media);
    {   // HTMLMediaElement.NETWORK_EMPTY ... HAVE_ENOUGH_DATA (on the class and on every element)
      const struct { const char* name; uint32_t value; } kStates[] = {{"NETWORK_EMPTY", 0}, {"NETWORK_IDLE", 1}, {"NETWORK_LOADING", 2}, {"NETWORK_NO_SOURCE", 3},
                                                                     {"HAVE_NOTHING", 0}, {"HAVE_METADATA", 1}, {"HAVE_CURRENT_DATA", 2}, {"HAVE_FUTURE_DATA", 3},
                                                                     {"HAVE_ENOUGH_DATA", 4}};
      for (const auto& state : kStates) {
        vm.define_named(media_class, state.name, Value::make_number(state.value), 0);
        vm.define_named(media, state.name, Value::make_number(state.value), 0);
      }
    }
    for (const auto& k : kInterfaces) {
      JsObject* proto = vm.new_object(c.html_proto);
      make_class(c, k.interface, illegal_constructor, proto);
      const StringView tags(k.tags);
      uint64_t at = 0;
      while (at < tags.size()) {
        uint64_t end = at;
        while (end < tags.size() && tags[end] != ' ') ++end;
        vm.define(c.tag_protos, vm.intern(tags.substr(at, end - at)), Value::make_object(proto), 0);
        at = end + 1;
      }
    }
    {   // <slot>: assignedNodes() / assignedElements()
      Value slot;
      if (vm.get_plain(c.tag_protos, vm.intern("slot"), &slot) && slot.is_object()) {
        vm.define_native(slot.as_object(), "assignedNodes", slot_assigned_nodes, 1);
        vm.define_native(slot.as_object(), "assignedElements", slot_assigned_elements, 1);
      }
    }
    for (const char* tag : {"audio", "video"}) {
      JsObject* proto = vm.new_object(media);
      make_class(c, tag[0] == 'a' ? "HTMLAudioElement" : "HTMLVideoElement", illegal_constructor, proto);
      vm.define(c.tag_protos, vm.intern(tag), Value::make_object(proto), 0);
    }
  }
  make_class(c, "CharacterData", illegal_constructor, c.character_data_proto);
  make_class(c, "Text", illegal_constructor, c.text_proto);
  make_class(c, "CDATASection", illegal_constructor, vm.new_object(c.text_proto));
  make_class(c, "ProcessingInstruction", illegal_constructor, vm.new_object(c.character_data_proto));
  make_class(c, "Comment", illegal_constructor, c.comment_proto);
  make_class(c, "Document", illegal_constructor, dp);
  make_class(c, "HTMLDocument", illegal_constructor, dp);
  make_class(c, "DocumentFragment", illegal_constructor, c.fragment_proto);
  c.shadow_root_proto = vm.new_object(c.fragment_proto);
  make_class(c, "ShadowRoot", illegal_constructor, c.shadow_root_proto);
  {
    JsObject* image = vm.new_native(image_constructor, "Image", 2);
    JsObject* image_proto = vm.new_object(c.html_proto);
    vm.define(image, vm.prototype_name(), Value::make_object(image_proto), 0);
    vm.define_named(g, "Image", Value::make_object(image), kWritable);
  }
  {   // new Audio(src)
    JsObject* audio = vm.new_native(audio_constructor, "Audio", 1);
    JsObject* audio_proto = vm.new_object(c.html_proto);
    vm.define(audio, vm.prototype_name(), Value::make_object(audio_proto), 0);
    vm.define_named(g, "Audio", Value::make_object(audio), kWritable);
  }
  {
    JsObject* parser_proto = vm.new_plain();
    vm.define_native(parser_proto, "parseFromString", dom_parser_parse, 2);
    make_class(c, "DOMParser", dom_parser_constructor, parser_proto);
  }
  {   // DOMException
    c.exception_proto = vm.new_object(vm.error_proto());
    vm.define_named(c.exception_proto, "name", str_value(vm, "Error"), kWritable);
    JsObject* f = make_class(c, "DOMException", exception_constructor, c.exception_proto);
    (void)f;
  }
  // the document and its wrapper
  vm.define_named(g, "document", node_value(c, c.doc.root()), kWritable);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
