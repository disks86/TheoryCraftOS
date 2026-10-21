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

// Inline styles (element.style, over the style attribute), computed styles (getComputedStyle), class lists (classList) and dataset.
#include <initializer_list>
#include "Browser/DomBindingsInternal.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)
#define IS(text) k.equals(text)

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower_char(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

struct Pair {
  explicit Pair(allocators::IAllocator& a) : name(a), value(a) {}
  String name, value;
};

// "a: b; c: d !important" -> pairs (the names lowercased, the values trimmed; !important kept in the value).
void parse_declarations(allocators::IAllocator& a, StringView text, List<Pair>* out) {
  uint64_t i = 0;
  while (i < text.size()) {
    const uint64_t start = i;
    int depth = 0;
    char quote = 0;
    while (i < text.size() && (depth > 0 || quote || text[i] != ';')) {
      const char c = text[i];
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')' && depth > 0) {
        --depth;
      }
      ++i;
    }
    const StringView declaration = text.substr(start, i - start);
    if (i < text.size()) ++i;
    const uint64_t colon = declaration.find(':');
    if (colon == StringView::npos) continue;
    const StringView name = declaration.first(colon).trimmed(), value = declaration.substr(colon + 1).trimmed();
    if (name.empty()) continue;
    Pair p(a);
    for (uint64_t k = 0; k < name.size(); ++k) p.name.push_back(lower_char(name[k]));
    p.value.assign(value);
    out->push_back(util::move(p));
  }
}

void serialize_declarations(const List<Pair>& pairs, String* out) {
  out->clear();
  for (uint64_t i = 0; i < pairs.count(); ++i) {
    if (i) out->push_back(' ');
    out->append(pairs[i].name.view());
    out->append(": ");
    out->append(pairs[i].value.view());
    out->push_back(';');
  }
}

StringView strip_important(StringView value) {
  const uint64_t bang = value.rfind('!');
  if (bang != StringView::npos && value.substr(bang + 1).trimmed().equals_ignore_case("important")) return value.first(bang).trimmed();
  return value;
}

}  // namespace

void css_name_from_js(StringView js, String* out) {
  out->clear();
  if (js.equals("cssFloat")) {
    out->assign("float");
    return;
  }
  for (uint64_t i = 0; i < js.size(); ++i) {
    const char c = js[i];
    if (c >= 'A' && c <= 'Z') {
      out->push_back('-');
      out->push_back(lower_char(c));
    } else {
      out->push_back(c);
    }
  }
}

void js_name_from_css(StringView css, String* out) {
  out->clear();
  bool up = false;
  for (uint64_t i = 0; i < css.size(); ++i) {
    const char c = css[i];
    if (c == '-' && i + 1 < css.size()) {
      up = true;
      continue;
    }
    out->push_back(up && c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
    up = false;
  }
}

namespace {

// --- Inline style ----------------------------------------------------------------------------------------------------------

dom::Node* style_node(JsObject* o) { return static_cast<dom::Node*>(o->host_data); }

bool style_method_name(StringView k) { return IS("getPropertyValue") || IS("setProperty") || IS("removeProperty") || IS("getPropertyPriority") || IS("item") || IS("constructor") || IS("toString"); }

bool style_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  dom::Node* n = style_node(o);
  const StringView k = key->view();
  if (k.empty() || (k.size() > 1 && k[0] == '_' && k[1] == '_') || style_method_name(k)) return false;
  StringView text;
  dom::get_attribute(n, "style", &text);
  if (IS("cssText")) {
    *out = str_value(vm, text);
    return true;
  }
  List<Pair> pairs(vm.allocator());
  parse_declarations(vm.allocator(), text, &pairs);
  if (IS("length")) {
    *out = num_value(static_cast<double>(pairs.count()));
    return true;
  }
  if (IS("parentRule")) {
    *out = Value::make_null();
    return true;
  }
  if (k[0] >= '0' && k[0] <= '9') {
    uint64_t index = 0;
    if (k.parse_uint(&index)) {
      *out = index < pairs.count() ? str_value(vm, pairs[index].name.view()) : str_value(vm, "");
      return true;
    }
  }
  String css(vm.allocator());
  css_name_from_js(k, &css);
  for (uint64_t i = pairs.count(); i > 0; --i) {
    if (pairs[i - 1].name.view().equals(css.view())) {
      *out = str_value(vm, strip_important(pairs[i - 1].value.view()));
      return true;
    }
  }
  *out = str_value(vm, "");
  return true;
}

// Sets (or, for "", removes) one property of a style attribute.
bool style_write(Vm& vm, dom::Node* n, StringView css_name, StringView value, StringView priority) {
  StringView text;
  dom::get_attribute(n, "style", &text);
  List<Pair> pairs(vm.allocator());
  parse_declarations(vm.allocator(), text, &pairs);
  for (uint64_t i = pairs.count(); i > 0; --i)
    if (pairs[i - 1].name.view().equals(css_name)) pairs.remove_at(i - 1);
  if (!value.empty()) {
    Pair p(vm.allocator());
    p.name.assign(css_name);
    p.value.assign(value);
    if (!priority.empty()) p.value.append(" !important");
    pairs.push_back(util::move(p));
  }
  String out(vm.allocator());
  serialize_declarations(pairs, &out);
  if (out.empty() && !dom::has_attribute(n, "style")) return true;
  return dom::set_attribute(n, "style", out.view()) == dom::DomError::None;
}

HostClass::Set style_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  dom::Node* n = style_node(o);
  const StringView k = key->view();
  if (k.empty() || (k.size() > 1 && k[0] == '_' && k[1] == '_') || style_method_name(k) || IS("length") || IS("parentRule")) return HostClass::Set::NotHandled;
  String text(vm.allocator());
  if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
  if (IS("cssText")) {
    dom::set_attribute(n, "style", text.view());
    return HostClass::Set::Handled;
  }
  String css(vm.allocator());
  css_name_from_js(k, &css);
  style_write(vm, n, css.view(), text.view().trimmed(), StringView());
  return HostClass::Set::Handled;
}

bool style_remove(Vm& vm, JsObject* o, JsString* key, bool* deleted) {
  String css(vm.allocator());
  css_name_from_js(key->view(), &css);
  style_write(vm, style_node(o), css.view(), StringView(), StringView());
  *deleted = true;
  return true;
}

void style_keys(Vm& vm, JsObject* o, JsObject* keys) {
  StringView text;
  dom::get_attribute(style_node(o), "style", &text);
  List<Pair> pairs(vm.allocator());
  parse_declarations(vm.allocator(), text, &pairs);
  for (uint64_t i = 0; i < pairs.count(); ++i) {
    String js(vm.allocator());
    js_name_from_css(pairs[i].name.view(), &js);
    vm.array_push(keys, str_value(vm, js.view()));
  }
}

const HostClass kStyleClass = {"CSSStyleDeclaration", style_get, style_set, style_remove, style_keys};

dom::Node* this_style_node(Vm& vm, const Value& self) {
  if (self.is_object() && self.as_object()->host == &kStyleClass) return style_node(self.as_object());
  vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  return nullptr;
}

NATIVE(style_get_property_value) {
  dom::Node* n = this_style_node(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  StringView text;
  dom::get_attribute(n, "style", &text);
  List<Pair> pairs(vm.allocator());
  parse_declarations(vm.allocator(), text, &pairs);
  *result = str_value(vm, "");
  for (uint64_t i = pairs.count(); i > 0; --i)
    if (pairs[i - 1].name.view().equals_ignore_case(name.view())) {
      *result = str_value(vm, strip_important(pairs[i - 1].value.view()));
      break;
    }
  return true;
}

NATIVE(style_get_property_priority) {
  dom::Node* n = this_style_node(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  StringView text;
  dom::get_attribute(n, "style", &text);
  List<Pair> pairs(vm.allocator());
  parse_declarations(vm.allocator(), text, &pairs);
  *result = str_value(vm, "");
  for (uint64_t i = pairs.count(); i > 0; --i)
    if (pairs[i - 1].name.view().equals_ignore_case(name.view())) {
      if (strip_important(pairs[i - 1].value.view()).size() != pairs[i - 1].value.size()) *result = str_value(vm, "important");
      break;
    }
  return true;
}

NATIVE(style_set_property) {
  dom::Node* n = this_style_node(vm, self);
  String name(vm.allocator()), value(vm.allocator()), priority(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name) || !arg_text(vm, arg_at(args, argc, 1), &value) || !arg_text(vm, arg_at(args, argc, 2), &priority)) return false;
  String lowered(vm.allocator());
  for (uint64_t i = 0; i < name.size(); ++i) lowered.push_back(lower_char(name[i]));
  style_write(vm, n, lowered.view(), value.view().trimmed(), priority.view());
  return true;
}

NATIVE(style_remove_property) {
  dom::Node* n = this_style_node(vm, self);
  String name(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  Value old;
  style_get_property_value(vm, self, args, argc, &old);
  style_write(vm, n, name.view(), StringView(), StringView());
  *result = old;
  return true;
}

NATIVE(style_item) {
  dom::Node* n = this_style_node(vm, self);
  double d;
  if (!n || !vm.to_number(arg_at(args, argc, 0), &d)) return false;
  StringView text;
  dom::get_attribute(n, "style", &text);
  List<Pair> pairs(vm.allocator());
  parse_declarations(vm.allocator(), text, &pairs);
  *result = d >= 0 && d < pairs.count() ? str_value(vm, pairs[static_cast<uint64_t>(d)].name.view()) : str_value(vm, "");
  return true;
}

NATIVE(style_to_string) {
  *result = str_value(vm, "[object CSSStyleDeclaration]");
  return true;
}

// --- Computed style --------------------------------------------------------------------------------------------------------

void rgb_text(uint32_t color, String* out) {
  out->append("rgb(");
  out->append_uint((color >> 16) & 0xFF);
  out->append(", ");
  out->append_uint((color >> 8) & 0xFF);
  out->append(", ");
  out->append_uint(color & 0xFF);
  out->push_back(')');
}

void px_text(uint32_t px, String* out) {
  out->append_uint(px);
  out->append("px");
}

NATIVE(computed_get_property_value) {
  String name(vm.allocator());
  if (!self.is_object() || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  String js(vm.allocator());
  js_name_from_css(name.view(), &js);
  if (name.view().equals("float")) js.assign("cssFloat");
  Value v;
  *result = vm.get_plain(self.as_object(), vm.intern(js.view()), &v) ? v : str_value(vm, "");
  return true;
}

NATIVE(computed_item) {
  *result = str_value(vm, "");
  return true;
}

}  // namespace

Value make_style_object(DomBindings::Impl& c, dom::Node* element) { return Value::make_object(c.vm.new_host_object(c.style_proto, &kStyleClass, element)); }

Value make_computed_style(DomBindings::Impl& c, dom::Node* element) {
  Vm& vm = c.vm;
  JsObject* o = vm.new_object(c.computed_proto);
  Style s;
  const bool known = element && element->is_element() && c.host.computed_style(element, &s);
  auto set = [&](const char* name, StringView value) { vm.define_named(o, name, str_value(vm, value)); };
  String t(vm.allocator());
  if (known) {
    const bool dark = c.host.media().dark;
    rgb_text(s.color ? s.color : (dark ? 0xE8E8E8 : 0x000000), &t);
    set("color", t.view());
    t.clear();
    if (s.background) rgb_text(s.background, &t);
    else t.assign("rgba(0, 0, 0, 0)");
    set("backgroundColor", t.view());
    set("display", s.display == Display::None ? "none" : s.display == Display::Block ? "block" : s.display == Display::ListItem ? "list-item" : "inline");
    set("visibility", s.hidden ? "hidden" : "visible");
    set("fontWeight", s.bold ? "700" : "400");
    set("textAlign", s.align == 1 ? "center" : s.align == 2 ? "right" : "left");
    set("textDecoration", s.underline && s.strike ? "underline line-through" : s.underline ? "underline" : s.strike ? "line-through" : "none");
    set("textDecorationLine", s.underline && s.strike ? "underline line-through" : s.underline ? "underline" : s.strike ? "line-through" : "none");
    set("textTransform", s.transform == 1 ? "uppercase" : s.transform == 2 ? "lowercase" : "none");
    set("whiteSpace", s.pre ? "pre" : "normal");
    set("listStyleType", s.list_none ? "none" : "disc");
    const struct {
      const char* name;
      uint32_t px;
    } kBox[] = {{"marginTop", s.margin_top * 16u}, {"marginBottom", s.margin_bottom * 16u}, {"marginLeft", s.margin_left * 8u}, {"paddingTop", s.padding_top * 16u},
                {"paddingBottom", s.padding_bottom * 16u}, {"paddingLeft", s.padding_left * 8u}};
    for (const auto& b : kBox) {
      t.clear();
      px_text(b.px, &t);
      set(b.name, t.view());
    }
  } else {
    set("display", "");
  }
  for (const char* k : {"marginRight", "paddingRight", "borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth", "top", "left", "right", "bottom"}) set(k, "0px");
  set("width", "auto");
  set("height", "auto");
  set("position", "static");
  set("float", "none");
  set("cssFloat", "none");
  set("overflow", "visible");
  set("opacity", "1");
  set("zIndex", "auto");
  set("cursor", "auto");
  set("boxSizing", "content-box");
  set("fontSize", "16px");
  set("fontStyle", "normal");
  set("fontFamily", "monospace");
  set("lineHeight", "16px");
  set("verticalAlign", "baseline");
  return Value::make_object(o);
}

// --- Class list ----------------------------------------------------------------------------------------------------------------

namespace {

dom::Node* token_node(JsObject* o) { return static_cast<dom::Node*>(o->host_data); }

void tokens_of(allocators::IAllocator& a, const dom::Node* n, List<String>* out) {
  StringView classes;
  dom::get_attribute(n, "class", &classes);
  uint64_t i = 0;
  while (i < classes.size()) {
    while (i < classes.size() && is_space(classes[i])) ++i;
    const uint64_t start = i;
    while (i < classes.size() && !is_space(classes[i])) ++i;
    if (i > start) {
      String t(a);
      t.assign(classes.substr(start, i - start));
      bool seen = false;
      for (uint64_t k = 0; k < out->count() && !seen; ++k) seen = (*out)[k].view().equals(t.view());
      if (!seen) out->push_back(util::move(t));
    }
  }
}

bool write_tokens(dom::Node* n, const List<String>& tokens) {
  String joined(n->owner->allocator());
  for (uint64_t i = 0; i < tokens.count(); ++i) {
    if (i) joined.push_back(' ');
    joined.append(tokens[i].view());
  }
  return dom::set_attribute(n, "class", joined.view()) == dom::DomError::None;
}

bool token_list_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  dom::Node* n = token_node(o);
  const StringView k = key->view();
  if (k.empty() || (k.size() > 1 && k[0] == '_' && k[1] == '_')) return false;
  if (IS("length")) {
    List<String> tokens(vm.allocator());
    tokens_of(vm.allocator(), n, &tokens);
    *out = num_value(static_cast<double>(tokens.count()));
    return true;
  }
  if (IS("value")) {
    StringView v;
    dom::get_attribute(n, "class", &v);
    *out = str_value(vm, v);
    return true;
  }
  if (k[0] >= '0' && k[0] <= '9') {
    uint64_t index = 0;
    if (k.parse_uint(&index)) {
      List<String> tokens(vm.allocator());
      tokens_of(vm.allocator(), n, &tokens);
      *out = index < tokens.count() ? str_value(vm, tokens[index].view()) : Value();
      return true;
    }
  }
  return false;
}

HostClass::Set token_list_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  if (!key->view().equals("value")) return HostClass::Set::NotHandled;
  String text(vm.allocator());
  if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
  dom::set_attribute(token_node(o), "class", text.view());
  return HostClass::Set::Handled;
}

const HostClass kTokenListClass = {"DOMTokenList", token_list_get, token_list_set, nullptr, nullptr};

dom::Node* this_tokens(Vm& vm, const Value& self) {
  if (self.is_object() && self.as_object()->host == &kTokenListClass) return token_node(self.as_object());
  vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  return nullptr;
}

bool token_arg(Vm& vm, const Value& v, String* out) {
  if (!arg_text(vm, v, out)) return false;
  if (out->empty()) return throw_dom(vm, "SyntaxError", "The token provided must not be empty.");
  for (uint64_t i = 0; i < out->size(); ++i)
    if (is_space((*out)[i])) return throw_dom(vm, "InvalidCharacterError", "The token provided contains HTML space characters, which are not valid in tokens.");
  return true;
}

NATIVE(tokens_add) {
  dom::Node* n = this_tokens(vm, self);
  if (!n) return false;
  List<String> tokens(vm.allocator());
  tokens_of(vm.allocator(), n, &tokens);
  for (uint32_t i = 0; i < argc; ++i) {
    String t(vm.allocator());
    if (!token_arg(vm, args[i], &t)) return false;
    bool have = false;
    for (uint64_t k = 0; k < tokens.count() && !have; ++k) have = tokens[k].view().equals(t.view());
    if (!have) tokens.push_back(util::move(t));
  }
  write_tokens(n, tokens);
  return true;
}

NATIVE(tokens_remove) {
  dom::Node* n = this_tokens(vm, self);
  if (!n) return false;
  List<String> tokens(vm.allocator());
  tokens_of(vm.allocator(), n, &tokens);
  for (uint32_t i = 0; i < argc; ++i) {
    String t(vm.allocator());
    if (!token_arg(vm, args[i], &t)) return false;
    for (uint64_t k = tokens.count(); k > 0; --k)
      if (tokens[k - 1].view().equals(t.view())) tokens.remove_at(k - 1);
  }
  if (dom::has_attribute(n, "class") || tokens.count()) write_tokens(n, tokens);
  return true;
}

NATIVE(tokens_toggle) {
  dom::Node* n = this_tokens(vm, self);
  String t(vm.allocator());
  if (!n || !token_arg(vm, arg_at(args, argc, 0), &t)) return false;
  List<String> tokens(vm.allocator());
  tokens_of(vm.allocator(), n, &tokens);
  int64_t at = -1;
  for (uint64_t k = 0; k < tokens.count(); ++k)
    if (tokens[k].view().equals(t.view())) at = static_cast<int64_t>(k);
  const bool want = argc > 1 && !args[1].is_undefined() ? Vm::to_boolean(args[1]) : at < 0;
  if (want && at < 0) tokens.push_back(util::move(t));
  else if (!want && at >= 0) tokens.remove_at(static_cast<uint64_t>(at));
  write_tokens(n, tokens);
  *result = bool_value(want);
  return true;
}

NATIVE(tokens_contains) {
  dom::Node* n = this_tokens(vm, self);
  String t(vm.allocator());
  if (!n || !arg_text(vm, arg_at(args, argc, 0), &t)) return false;
  *result = bool_value(dom::has_class(n, t.view()));
  return true;
}

NATIVE(tokens_item) {
  dom::Node* n = this_tokens(vm, self);
  double d;
  if (!n || !vm.to_number(arg_at(args, argc, 0), &d)) return false;
  List<String> tokens(vm.allocator());
  tokens_of(vm.allocator(), n, &tokens);
  *result = d >= 0 && d < tokens.count() ? str_value(vm, tokens[static_cast<uint64_t>(d)].view()) : Value::make_null();
  return true;
}

NATIVE(tokens_replace) {
  dom::Node* n = this_tokens(vm, self);
  String from(vm.allocator()), to(vm.allocator());
  if (!n || !token_arg(vm, arg_at(args, argc, 0), &from) || !token_arg(vm, arg_at(args, argc, 1), &to)) return false;
  List<String> tokens(vm.allocator());
  tokens_of(vm.allocator(), n, &tokens);
  int64_t at = -1;
  for (uint64_t k = 0; k < tokens.count(); ++k)
    if (tokens[k].view().equals(from.view())) at = static_cast<int64_t>(k);
  if (at < 0) {
    *result = bool_value(false);
    return true;
  }
  tokens[static_cast<uint64_t>(at)].assign(to.view());
  write_tokens(n, tokens);
  *result = bool_value(true);
  return true;
}

NATIVE(tokens_for_each) {
  dom::Node* n = this_tokens(vm, self);
  if (!n) return false;
  const Value fn = arg_at(args, argc, 0);
  if (!Vm::is_callable(fn)) return vm.throw_error(ErrorKind::TypeError, "parameter 1 is not of type 'Function'.");
  List<String> tokens(vm.allocator());
  tokens_of(vm.allocator(), n, &tokens);
  for (uint64_t i = 0; i < tokens.count(); ++i) {
    Value call_args[3] = {str_value(vm, tokens[i].view()), num_value(static_cast<double>(i)), self};
    Value ignored;
    if (!vm.call(fn, arg_at(args, argc, 1), call_args, 3, &ignored)) return false;
  }
  return true;
}

NATIVE(tokens_to_string) {
  dom::Node* n = this_tokens(vm, self);
  if (!n) return false;
  StringView v;
  dom::get_attribute(n, "class", &v);
  *result = str_value(vm, v);
  return true;
}

NATIVE(tokens_supports) {
  *result = bool_value(false);
  return true;
}

// --- Dataset --------------------------------------------------------------------------------------------------------------------

dom::Node* data_node(JsObject* o) { return static_cast<dom::Node*>(o->host_data); }

void data_attribute(StringView js, String* out) {
  out->assign("data-");
  for (uint64_t i = 0; i < js.size(); ++i) {
    if (js[i] >= 'A' && js[i] <= 'Z') {
      out->push_back('-');
      out->push_back(lower_char(js[i]));
    } else {
      out->push_back(js[i]);
    }
  }
}

bool dataset_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  const StringView k = key->view();
  if (k.empty() || (k.size() > 1 && k[0] == '_' && k[1] == '_')) return false;
  String attribute(vm.allocator());
  data_attribute(k, &attribute);
  StringView v;
  if (!dom::get_attribute(data_node(o), attribute.view(), &v)) {
    *out = Value();
    return true;   // (undefined, and not a property of Object either)
  }
  *out = str_value(vm, v);
  return true;
}

HostClass::Set dataset_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  const StringView k = key->view();
  if (k.empty() || (k.size() > 1 && k[0] == '_' && k[1] == '_')) return HostClass::Set::NotHandled;
  String attribute(vm.allocator()), text(vm.allocator());
  data_attribute(k, &attribute);
  if (!arg_text(vm, value, &text)) return HostClass::Set::Threw;
  dom::set_attribute(data_node(o), attribute.view(), text.view());
  return HostClass::Set::Handled;
}

bool dataset_remove(Vm& vm, JsObject* o, JsString* key, bool* deleted) {
  String attribute(vm.allocator());
  data_attribute(key->view(), &attribute);
  dom::remove_attribute(data_node(o), attribute.view());
  *deleted = true;
  return true;
}

void dataset_keys(Vm& vm, JsObject* o, JsObject* keys) {
  const dom::Node* n = data_node(o);
  for (uint64_t i = 0; i < n->attrs.count(); ++i) {
    const StringView name = n->attrs[i].name.view();
    if (!name.starts_with("data-")) continue;
    String js(vm.allocator());
    js_name_from_css(name.substr(5), &js);
    vm.array_push(keys, str_value(vm, js.view()));
  }
}

const HostClass kDatasetClass = {"DOMStringMap", dataset_get, dataset_set, dataset_remove, dataset_keys};

// --- CSS ------------------------------------------------------------------------------------------------------------------------

NATIVE(css_escape) {
  String text(vm.allocator()), out(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    const bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_' || static_cast<uint8_t>(ch) >= 0x80 || (ch >= '0' && ch <= '9' && i > 0);
    if (!plain) {
      if (ch >= '0' && ch <= '9') {   // a digit at the start: as a code point escape
        out.push_back('\\');
        out.push_back('3');
        out.push_back(ch);
        out.push_back(' ');
        continue;
      }
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  *result = str_value(vm, out.view());
  return true;
}

NATIVE(css_supports) {
  *result = bool_value(false);
  return true;
}

}  // namespace

Value make_token_list(DomBindings::Impl& c, dom::Node* element) { return Value::make_object(c.vm.new_host_object(c.token_list_proto, &kTokenListClass, element)); }

Value make_dataset(DomBindings::Impl& c, dom::Node* element) { return Value::make_object(c.vm.new_host_object(c.vm.object_proto(), &kDatasetClass, element)); }

bool install_styles(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  c.style_proto = vm.new_plain();
  vm.define_native(c.style_proto, "getPropertyValue", style_get_property_value, 1);
  vm.define_native(c.style_proto, "getPropertyPriority", style_get_property_priority, 1);
  vm.define_native(c.style_proto, "setProperty", style_set_property, 3);
  vm.define_native(c.style_proto, "removeProperty", style_remove_property, 1);
  vm.define_native(c.style_proto, "item", style_item, 1);
  vm.define_native(c.style_proto, "toString", style_to_string, 0);
  c.computed_proto = vm.new_plain();
  vm.define_native(c.computed_proto, "getPropertyValue", computed_get_property_value, 1);
  vm.define_native(c.computed_proto, "item", computed_item, 1);
  c.token_list_proto = vm.new_plain();
  vm.define_native(c.token_list_proto, "add", tokens_add, 1);
  vm.define_native(c.token_list_proto, "remove", tokens_remove, 1);
  vm.define_native(c.token_list_proto, "toggle", tokens_toggle, 2);
  vm.define_native(c.token_list_proto, "contains", tokens_contains, 1);
  vm.define_native(c.token_list_proto, "item", tokens_item, 1);
  vm.define_native(c.token_list_proto, "replace", tokens_replace, 2);
  vm.define_native(c.token_list_proto, "forEach", tokens_for_each, 1);
  vm.define_native(c.token_list_proto, "toString", tokens_to_string, 0);
  vm.define_native(c.token_list_proto, "supports", tokens_supports, 1);
  JsObject* css = vm.new_plain();
  vm.define_native(css, "escape", css_escape, 1);
  vm.define_native(css, "supports", css_supports, 2);
  vm.define_named(vm.global(), "CSS", Value::make_object(css), kWritable);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
