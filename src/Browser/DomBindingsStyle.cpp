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
#include "Browser/Css.hpp"
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

// --- Shorthands read back as longhands: "margin: 1px 2px" gives margin-left 2px ---------------------------------------------------------------

// The space separated items of a value (parentheses keep their insides together).
void split_items(StringView v, List<StringView>* out) {
  uint64_t i = 0;
  int depth = 0;
  while (i < v.size()) {
    while (i < v.size() && is_space(v[i])) ++i;
    const uint64_t start = i;
    while (i < v.size() && (depth > 0 || !is_space(v[i]))) {
      if (v[i] == '(') ++depth;
      else if (v[i] == ')') --depth;
      ++i;
    }
    if (i > start) out->push_back(v.substr(start, i - start));
  }
}

bool border_style_word(StringView t) {
  static const char* const kStyles[] = {"none", "hidden", "dotted", "dashed", "solid", "double", "groove", "ridge", "inset", "outset"};
  for (const char* w : kStyles)
    if (t.equals_ignore_case(w)) return true;
  return false;
}

bool border_width_word(StringView t) {
  if (t.empty()) return false;
  const char c = t[0];
  return (c >= '0' && c <= '9') || c == '.' || c == '+' || t.starts_with("calc(") || t.equals_ignore_case("thin") || t.equals_ignore_case("medium") || t.equals_ignore_case("thick");
}

bool side_index(StringView side, uint32_t* index) {   // top right bottom left
  static const char* const kSides[] = {"top", "right", "bottom", "left"};
  for (uint32_t i = 0; i < 4; ++i)
    if (side.equals(kSides[i])) {
      *index = i;
      return true;
    }
  return false;
}

// 1 to 4 values apply to the sides the way margin and padding do.
bool box_item(const List<StringView>& items, uint32_t side, StringView* out) {
  if (items.count() == 0 || items.count() > 4) return false;
  static const uint32_t kPick[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
  *out = items[kPick[items.count() - 1][side]];
  return true;
}

// The value `name` has through the declaration `decl: value`, if that is a shorthand covering it.
bool through_shorthand(allocators::IAllocator& a, StringView decl, StringView value, StringView name, String* out) {
  value = strip_important(value);
  List<StringView> items(a);
  split_items(value, &items);
  StringView picked;
  uint32_t side = 0;
  // margin / padding: margin-top ...
  for (const char* box : {"margin", "padding"}) {
    const StringView b(box);
    if (decl.equals(b) && name.starts_with(b) && name.size() > b.size() + 1 && name[b.size()] == '-' && side_index(name.substr(b.size() + 1), &side) && box_item(items, side, &picked)) {
      out->assign(picked);
      return true;
    }
  }
  // border-width / -style / -color: border-top-width ...
  if (name.starts_with("border-") ) {
    const StringView rest = name.substr(7);
    StringView part;
    StringView sidename;
    for (const char* suffix : {"width", "style", "color"}) {
      const StringView sf(suffix);
      if (rest.equals(sf)) { part = sf; break; }
      if (rest.size() > sf.size() + 1 && rest.ends_with(sf) && rest[rest.size() - sf.size() - 1] == '-') {
        part = sf;
        sidename = rest.first(rest.size() - sf.size() - 1);
        break;
      }
    }
    if (!part.empty() && !sidename.empty() && side_index(sidename, &side)) {   // a side's own longhand
      String group(a);
      group.append("border-");
      group.append(part);
      if (decl.equals(group.view()) && box_item(items, side, &picked)) {
        out->assign(picked);
        return true;
      }
      if (decl.equals("border") || (decl.starts_with("border-") && decl.substr(7).equals(sidename))) {
        for (uint64_t i = 0; i < items.count(); ++i) {
          const bool is_width = border_width_word(items[i]), is_style = border_style_word(items[i]);
          if ((part.equals("width") && is_width) || (part.equals("style") && is_style) || (part.equals("color") && !is_width && !is_style)) {
            out->assign(items[i]);
            return true;
          }
        }
        return false;
      }
    }
  }
  if (decl.equals("background") && name.equals("background-color")) {
    for (uint64_t i = 0; i < items.count(); ++i) {
      uint32_t color;
      if (parse_css_color(items[i], &color) || items[i].equals_ignore_case("currentcolor")) {
        out->assign(items[i]);
        return true;
      }
    }
    return false;
  }
  if (decl.equals("font") && (name.equals("font-size") || name.equals("font-weight") || name.equals("font-style"))) {
    for (uint64_t i = 0; i < items.count(); ++i) {
      StringView t = items[i];
      const bool is_size = border_width_word(t) && !(t.size() > 0 && t[0] >= '0' && t[0] <= '9' && t.find('.') == StringView::npos && t.find('p') == StringView::npos && t.find('%') == StringView::npos && t.find('e') == StringView::npos && t.find('r') == StringView::npos);
      if (is_size) {
        if (name.equals("font-size")) {
          const uint64_t slash = t.find('/');
          out->assign(slash == StringView::npos ? t : t.first(slash));
          return true;
        }
        return false;
      }
      const bool weight = t.equals_ignore_case("bold") || t.equals_ignore_case("bolder") || t.equals_ignore_case("lighter") || (t.size() == 3 && t[0] >= '1' && t[0] <= '9' && t[1] == '0' && t[2] == '0');
      const bool style = t.equals_ignore_case("italic") || t.equals_ignore_case("oblique");
      if (name.equals("font-weight") && weight) { out->assign(t); return true; }
      if (name.equals("font-style") && style) { out->assign(t); return true; }
    }
    if (name.equals("font-weight") || name.equals("font-style")) { out->assign("normal"); return true; }
    return false;
  }
  if (decl.equals("overflow") && (name.equals("overflow-x") || name.equals("overflow-y")) && items.count() >= 1 && items.count() <= 2) {
    out->assign(items[name.equals("overflow-x") || items.count() == 1 ? 0 : 1]);
    return true;
  }
  if (decl.equals("gap") && (name.equals("row-gap") || name.equals("column-gap")) && items.count() >= 1 && items.count() <= 2) {
    out->assign(items[name.equals("row-gap") || items.count() == 1 ? 0 : 1]);
    return true;
  }
  return false;
}

// The value of property `name` in the declarations: its own declaration or a shorthand that covers it, whichever comes last.
bool declared_value(allocators::IAllocator& a, const List<Pair>& pairs, StringView name, String* out) {
  for (uint64_t i = pairs.count(); i > 0; --i) {
    if (pairs[i - 1].name.view().equals(name)) {
      out->assign(strip_important(pairs[i - 1].value.view()));
      return true;
    }
    if (through_shorthand(a, pairs[i - 1].name.view(), pairs[i - 1].value.view(), name, out)) return true;
  }
  return false;
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
  String found(vm.allocator());
  *out = str_value(vm, declared_value(vm.allocator(), pairs, css.view(), &found) ? found.view() : StringView());
  return true;
}

// Sets (or, for "", removes) one property of a style attribute.
bool style_write(Vm& vm, dom::Node* n, StringView css_name, StringView value, StringView priority) {
  if (!value.empty() && !css_declaration_valid(css_name, value)) return true;   // (a value the property does not take is ignored: the declaration stays as it was)
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
  String lowered(vm.allocator()), found(vm.allocator());
  for (uint64_t i = 0; i < name.size(); ++i) lowered.push_back(lower_char(name[i]));
  *result = str_value(vm, declared_value(vm.allocator(), pairs, lowered.view(), &found) ? found.view() : StringView());
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

// rgb(r, g, b), or rgba(r, g, b, a) with the alpha to three places for a colour that is not opaque.
void rgba_text(uint32_t color, String* out) {
  const uint32_t alpha = color >> 24;
  if (alpha == 0xFF) {
    out->append("rgb(");
  } else {
    out->append("rgba(");
  }
  out->append_uint((color >> 16) & 0xFF);
  out->append(", ");
  out->append_uint((color >> 8) & 0xFF);
  out->append(", ");
  out->append_uint(color & 0xFF);
  if (alpha != 0xFF) {
    out->append(", ");
    uint32_t thousandths = (alpha * 1000 + 127) / 255;   // (two places if they give the same byte back, else three: 0.25, 0.502)
    const uint32_t hundredths = (alpha * 100 + 127) / 255;
    if ((hundredths * 255 + 50) / 100 == alpha) thousandths = hundredths * 10;
    out->append_uint(thousandths / 1000);
    if (thousandths % 1000) {
      out->push_back('.');
      out->push_back(static_cast<char>('0' + thousandths / 100 % 10));
      if (thousandths % 100) out->push_back(static_cast<char>('0' + thousandths / 10 % 10));
      if (thousandths % 10) out->push_back(static_cast<char>('0' + thousandths % 10));
    }
  }
  out->push_back(')');
}

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

// The parts of `text` separated by `sep` outside brackets and quotes (trimmed).
bool next_top_level(StringView text, uint64_t* at, char sep, StringView* part) {
  if (*at > text.size()) return false;
  int depth = 0;
  char quote = 0;
  const uint64_t start = *at;
  uint64_t i = start;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (quote) {
      if (c == '\\') ++i;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (c == sep && depth == 0) {
      break;
    }
  }
  *part = text.substr(start, i - start).trimmed();
  *at = i + 1;
  return true;
}

// A colour written any way, as the computed value shows it.
bool colour_serialized(StringView text, String* out) {
  uint32_t argb;
  if (!parse_css_color_alpha(text, &argb)) return false;
  rgba_text(argb, out);
  return true;
}

// A list of images (background-image): url() quoted, the colours of gradients as rgb().
void image_list_text(StringView value, String* out) {
  uint64_t at = 0;
  StringView item;
  bool first = true;
  while (next_top_level(value, &at, ',', &item)) {
    if (!first) out->append(", ");
    first = false;
    const uint64_t open = item.find('(');
    const uint64_t close = item.rfind(')');
    if (open == StringView::npos || close == StringView::npos || close < open) {
      out->append(item);
      continue;
    }
    const StringView name = item.first(open);
    StringView inner = item.substr(open + 1, close - open - 1).trimmed();
    if (name.equals_ignore_case("url")) {
      if (inner.size() >= 2 && (inner[0] == '"' || inner[0] == '\'') && inner[inner.size() - 1] == inner[0]) inner = inner.substr(1, inner.size() - 2);
      out->append("url(\"");
      for (uint64_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '"' || inner[i] == '\\') out->push_back('\\');
        out->push_back(inner[i]);
      }
      out->append("\")");
      continue;
    }
    if (!name.contains("gradient")) {
      out->append(item);
      continue;
    }
    out->append(name);
    out->push_back('(');
    uint64_t k = 0;
    StringView arg;
    bool first_arg = true;
    while (next_top_level(inner, &k, ',', &arg)) {
      if (!first_arg) out->append(", ");
      first_arg = false;
      uint64_t m = 0;
      next_top_level(arg, &m, ' ', &item);   // (the colour comes first in a colour stop)
      String colour(out->allocator());
      if (!item.empty() && colour_serialized(item, &colour)) {
        out->append(colour.view());
        if (m <= arg.size()) {
          out->push_back(' ');
          out->append(arg.substr(m).trimmed());
        }
      } else {
        out->append(arg);
      }
    }
    out->push_back(')');
  }
}

// background-position: keywords become percentages ("right bottom" is "100% 100%").
void position_list_text(StringView value, String* out) {
  uint64_t at = 0;
  StringView item;
  bool first = true;
  while (next_top_level(value, &at, ',', &item)) {
    if (!first) out->append(", ");
    first = false;
    StringView words[4];
    uint32_t n = 0;
    uint64_t k = 0;
    StringView w;
    while (n < 4 && next_top_level(item, &k, ' ', &w)) {
      if (!w.empty()) words[n++] = w;
    }
    auto percent_of = [](StringView word, int* percent) {
      if (word.equals_ignore_case("left") || word.equals_ignore_case("top")) *percent = 0;
      else if (word.equals_ignore_case("center")) *percent = 50;
      else if (word.equals_ignore_case("right") || word.equals_ignore_case("bottom")) *percent = 100;
      else return false;
      return true;
    };
    bool simple = n >= 1 && n <= 2;
    int x = 50, y = 50;
    if (simple) {
      const bool a_vertical = words[0].equals_ignore_case("top") || words[0].equals_ignore_case("bottom");
      const bool b_horizontal = n == 2 && (words[1].equals_ignore_case("left") || words[1].equals_ignore_case("right"));
      int pa = 0, pb = 50;
      const bool a_key = percent_of(words[0], &pa);
      const bool b_key = n == 2 ? percent_of(words[1], &pb) : true;
      if (n == 1) {
        if (a_vertical) { x = 50; y = pa; }
        else if (a_key) { x = pa; y = 50; }
        else simple = false;
      } else if (a_key && b_key) {
        if (a_vertical || b_horizontal) { y = pa; x = pb; }
        else { x = pa; y = pb; }
      } else {
        simple = false;
      }
    }
    if (!simple) {
      out->append(item);
      continue;
    }
    out->append_uint(static_cast<uint32_t>(x));
    out->append("% ");
    out->append_uint(static_cast<uint32_t>(y));
    out->push_back('%');
  }
}

NATIVE(computed_get_property_value) {
  String name(vm.allocator());
  if (!self.is_object() || !arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  String js(vm.allocator());
  if (name.view().starts_with("--")) js.assign(name.view());   // (a custom property keeps its name)
  else js_name_from_css(name.view(), &js);
  if (name.view().equals("float")) js.assign("cssFloat");
  Value v;
  *result = vm.get_plain(self.as_object(), vm.intern(js.view()), &v) ? v : str_value(vm, "");
  return true;
}

NATIVE(computed_item) {   // the name of the i-th property the style lists
  double d;
  *result = str_value(vm, "");
  if (!self.is_object() || !vm.to_number(arg_at(args, argc, 0), &d) || d < 0) return true;
  JsObject* keys = vm.own_keys(self.as_object());
  uint64_t seen = 0;
  for (uint32_t i = 0; i < keys->length; ++i) {
    const StringView k = keys->items[i].as_string()->view();
    if (k.empty() || (k[0] >= '0' && k[0] <= '9') || k.equals("length")) continue;
    if (seen++ == static_cast<uint64_t>(d)) {
      String css(vm.allocator());
      if (k.starts_with("--")) css.assign(k);
      else css_name_from_js(k, &css);
      *result = str_value(vm, css.equals("css-float") ? StringView("float") : css.view());
      return true;
    }
  }
  return true;
}

}  // namespace

Value make_style_object(DomBindings::Impl& c, dom::Node* element) { return Value::make_object(c.vm.new_host_object(c.style_proto, &kStyleClass, element)); }

namespace {

void fixed_px(int32_t px100, String* out) {
  if (px100 < 0) {
    out->push_back('-');
    px100 = -px100;
  }
  out->append_uint(static_cast<uint32_t>(px100 / 100));
  if (px100 % 100) {
    out->push_back('.');
    out->push_back(static_cast<char>('0' + (px100 % 100) / 10));
    if (px100 % 10) out->push_back(static_cast<char>('0' + px100 % 10));
  }
  out->append("px");
}

void length_text(const Length& l, String* out) {
  switch (l.kind) {
    case Length::Px: fixed_px(l.px100, out); break;
    case Length::MinContent: out->append("min-content"); break;
    case Length::MaxContent: out->append("max-content"); break;
    case Length::FitContent: out->append("fit-content"); break;
    case Length::None: out->append("none"); break;
    default: out->append("auto"); break;
  }
}

const char* place_text(Place p) {
  switch (p) {
    case Place::Start: return "start";
    case Place::End: return "end";
    case Place::Center: return "center";
    case Place::Stretch: return "stretch";
    case Place::Baseline: return "baseline";
    case Place::SpaceBetween: return "space-between";
    case Place::SpaceAround: return "space-around";
    case Place::SpaceEvenly: return "space-evenly";
    case Place::FlexStart: return "flex-start";
    case Place::FlexEnd: return "flex-end";
    case Place::Left: return "left";
    case Place::Right: return "right";
    case Place::Auto: return "auto";
    default: return "normal";
  }
}

const char* border_style_text(BorderStyle b) {
  switch (b) {
    case BorderStyle::Hidden: return "hidden";
    case BorderStyle::Dotted: return "dotted";
    case BorderStyle::Dashed: return "dashed";
    case BorderStyle::Solid: return "solid";
    case BorderStyle::Double: return "double";
    case BorderStyle::Groove: return "groove";
    case BorderStyle::Ridge: return "ridge";
    case BorderStyle::Inset: return "inset";
    case BorderStyle::Outset: return "outset";
    default: return "none";
  }
}

const char* overflow_text(Overflow o) { return o == Overflow::Hidden ? "hidden" : o == Overflow::Scroll ? "scroll" : o == Overflow::Auto ? "auto" : "visible"; }

// A number with up to 3 decimals ("0.5", "1", "12.25").
void number_text(double v, String* out) {
  const bool negative = v < 0;
  if (negative) v = -v;
  if (v > 1e12) v = 1e12;
  int64_t milli = static_cast<int64_t>(v * 1000 + 0.5);
  if (negative && milli != 0) out->push_back('-');
  out->append_uint(static_cast<uint64_t>(milli / 1000));
  if (milli % 1000) {
    out->push_back('.');
    out->push_back(static_cast<char>('0' + (milli / 100) % 10));
    if (milli % 100) {
      out->push_back(static_cast<char>('0' + (milli / 10) % 10));
      if (milli % 10) out->push_back(static_cast<char>('0' + milli % 10));
    }
  }
}

// "rgb(1, 2, 3)" or "rgba(1, 2, 3, 0.5)".
void color_text(uint32_t argb, String* out) {
  const uint32_t alpha = argb >> 24;
  if (alpha == 255) {
    out->append("rgb(");
  } else {
    out->append("rgba(");
  }
  out->append_uint((argb >> 16) & 0xFF);
  out->append(", ");
  out->append_uint((argb >> 8) & 0xFF);
  out->append(", ");
  out->append_uint(argb & 0xFF);
  if (alpha != 255) {
    out->append(", ");
    number_text(alpha / 255.0, out);
  }
  out->push_back(')');
}

void dimension_text(const Dimension& d, String* out) {
  if (d.kind == Dimension::Auto) out->append("auto");
  else if (d.kind == Dimension::Percent) {
    number_text(d.x100 / 100.0, out);
    out->push_back('%');
  } else {
    fixed_px(d.x100, out);
  }
}

void axis_text(const AxisPos& a, String* out) {
  if (a.px100 == 0) {
    number_text(a.percent_x100 / 100.0, out);
    out->push_back('%');
  } else if (a.percent_x100 == 0) {
    fixed_px(a.px100, out);
  } else {
    out->append("calc(");
    number_text(a.percent_x100 / 100.0, out);
    out->append(a.px100 < 0 ? "% - " : "% + ");
    fixed_px(a.px100 < 0 ? -a.px100 : a.px100, out);
    out->push_back(')');
  }
}

void shadows_text(const StyleSheet& sheet, TextRef list, const Style& s, bool text_shadow, allocators::IAllocator& a, String* out) {
  List<Shadow> shadows(a);
  if (list.empty() || !sheet.shadows(list, s, &shadows) || shadows.count() == 0) {
    out->append("none");
    return;
  }
  for (uint64_t i = 0; i < shadows.count(); ++i) {
    const Shadow& sh = shadows[i];
    if (i) out->append(", ");
    color_text(sh.current_color ? (s.color ? s.color : 0xFF000000u) : sh.color, out);
    out->push_back(' ');
    fixed_px(sh.x_px100, out);
    out->push_back(' ');
    fixed_px(sh.y_px100, out);
    out->push_back(' ');
    fixed_px(sh.blur_px100, out);
    if (!text_shadow) {
      out->push_back(' ');
      fixed_px(sh.spread_px100, out);
      if (sh.inset) out->append(" inset");
    }
  }
}

const char* cursor_text(Cursor c) {
  static const char* const kNames[] = {"auto", "default", "none", "context-menu", "help", "pointer", "progress", "wait", "cell", "crosshair", "text", "vertical-text", "alias", "copy",
                                       "move", "no-drop", "not-allowed", "grab", "grabbing", "all-scroll", "col-resize", "row-resize", "n-resize", "e-resize", "s-resize",
                                       "w-resize", "ne-resize", "nw-resize", "se-resize", "sw-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize", "zoom-in", "zoom-out"};
  const uint32_t i = static_cast<uint32_t>(c);
  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "auto";
}

const char* overscroll_text(OverscrollBehavior o) { return o == OverscrollBehavior::Contain ? "contain" : o == OverscrollBehavior::None ? "none" : "auto"; }

}  // namespace

Value make_computed_style(DomBindings::Impl& c, dom::Node* element) {
  Vm& vm = c.vm;
  JsObject* o = vm.new_object(c.computed_proto);
  Style s;
  const bool known = element && element->is_element() && c.host.computed_style(element, &s);
  auto set = [&](const char* name, StringView value) { vm.define_named(o, name, str_value(vm, value)); };
  String t(vm.allocator());
  if (!known) {
    set("display", "");
    return Value::make_object(o);
  }
  const bool dark = c.host.media().dark;
  const uint32_t text_color = s.color ? s.color : (dark ? 0xE8E8E8 : 0x000000);
  if (s.color_argb_set && (s.color_argb >> 24) != 0xFF) rgba_text(s.color_argb, &t);
  else rgb_text(text_color, &t);
  if (element->is_tag("a") && dom::has_attribute(element, "href") && !s.color && !s.color_argb_set) t.assign("rgb(0, 0, 238)");
  set("color", t.view());
  t.clear();
  if (s.background_argb != 0 && (s.background_argb >> 24) != 0xFF) rgba_text(s.background_argb, &t);
  else if (s.background) rgb_text(s.background, &t);
  else t.assign("rgba(0, 0, 0, 0)");
  set("backgroundColor", t.view());
  t.clear();
  const char* display = s.display == Display::None ? "none" : s.contents ? "contents" : "inline";
  if (s.display != Display::None && !s.contents) {
    switch (s.layout) {
      case Layout::Flex: display = s.inline_box ? "inline-flex" : "flex"; break;
      case Layout::Grid: display = s.inline_box ? "inline-grid" : "grid"; break;
      case Layout::Table: display = s.inline_box ? "inline-table" : "table"; break;
      case Layout::TableRowGroup: display = "table-row-group"; break;
      case Layout::TableRow: display = "table-row"; break;
      case Layout::TableCell: display = "table-cell"; break;
      case Layout::TableCaption: display = "table-caption"; break;
      default: display = s.display == Display::ListItem ? "list-item" : s.display == Display::Block ? (s.inline_box ? "inline-block" : "block") : s.inline_box ? "inline-block" : "inline"; break;
    }
  }
  // controls are inline-blocks; a floated or absolutely positioned box is blockified
  if (display[0] == 'i' && element->is_element() &&
      (element->is_tag("button") || element->is_tag("input") || element->is_tag("select") || element->is_tag("textarea") || element->is_tag("meter") || element->is_tag("progress")))
    display = "inline-block";
  if (s.display != Display::None && !s.contents && (s.float_side != Float::None || s.position == Position::Absolute || s.position == Position::Fixed)) {
    if (StringView(display).equals("inline") || StringView(display).equals("inline-block") || StringView(display).equals("table-row-group") ||
        StringView(display).equals("table-row") || StringView(display).equals("table-cell") || StringView(display).equals("table-caption")) display = "block";
    else if (StringView(display).equals("inline-flex")) display = "flex";
    else if (StringView(display).equals("inline-grid")) display = "grid";
    else if (StringView(display).equals("inline-table")) display = "table";
  }
  set("display", display);
  set("visibility", s.visibility == Visibility::Hidden ? "hidden" : s.visibility == Visibility::Collapse ? "collapse" : "visible");
  t.clear();
  t.append_uint(s.font_weight);
  set("fontWeight", t.view());
  if (s.oblique_x10 >= 0) {
    t.clear();
    t.append("oblique");
    if (s.oblique_x10 != 140) {
      t.push_back(' ');
      number_text(s.oblique_x10 / 10.0, &t);
      t.append("deg");
    }
    set("fontStyle", t.view());
  } else {
    set("fontStyle", s.italic ? "italic" : "normal");
  }
  set("textAlign", s.text_align_kw == 1 ? "start" : s.text_align_kw == 2 ? "end" : s.text_align_kw == 3 ? "match-parent" : s.justify ? "justify" : s.align == 1 ? "center" : s.align == 2 ? "right" : s.text_align_kw == 4 ? "left" : "start");
  const char* decoration = s.underline && s.strike ? "underline line-through" : s.underline ? "underline" : s.strike ? "line-through" : "none";
  set("textDecoration", decoration);
  set("textDecorationLine", decoration);
  set("textTransform", s.transform == 1 ? "uppercase" : s.transform == 2 ? "lowercase" : s.transform == 3 ? "capitalize" : "none");
  static const char* const kWhite[] = {"normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces"};
  set("whiteSpace", kWhite[s.white_space < 6 ? s.white_space : 0]);
  static const char* const kList[] = {"disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha", "lower-roman", "upper-roman", "none", "decimal-leading-zero"};
  set("listStyleType", s.list_none ? "none" : kList[s.list_type < 10 ? s.list_type : 0]);
  t.clear();
  fixed_px(static_cast<int32_t>(s.font_px) * 100, &t);
  set("fontSize", t.view());
  t.clear();
  if (s.line_height_px100 >= 0) fixed_px(s.line_height_px100, &t);
  else t.append("normal");
  set("lineHeight", t.view());
  const StyleSheet* sheet = c.host.computed_style_sheet();
  {
    StringView family = sheet ? sheet->text(s.font_family) : StringView();
    set("fontFamily", family.empty() ? StringView("monospace") : family);
  }
  set("verticalAlign", s.vertical_align == 0 && element->is_tag("sub") ? "sub" : s.vertical_align == 0 && element->is_tag("sup") ? "super" : s.vertical_align == 1 ? "top" : s.vertical_align == 2 ? "middle" : s.vertical_align == 3 ? "bottom" : "baseline");
  // the box
  static const char* const kMargin[4] = {"marginTop", "marginRight", "marginBottom", "marginLeft"};
  static const char* const kPadding[4] = {"paddingTop", "paddingRight", "paddingBottom", "paddingLeft"};
  static const char* const kWidth[4] = {"borderTopWidth", "borderRightWidth", "borderBottomWidth", "borderLeftWidth"};
  static const char* const kStyle[4] = {"borderTopStyle", "borderRightStyle", "borderBottomStyle", "borderLeftStyle"};
  static const char* const kColor[4] = {"borderTopColor", "borderRightColor", "borderBottomColor", "borderLeftColor"};
  static const char* const kInset[4] = {"top", "right", "bottom", "left"};
  for (int i = 0; i < 4; ++i) {
    t.clear();
    length_text(s.margin[i], &t);
    set(kMargin[i], t.view());
    t.clear();
    length_text(s.padding[i], &t);
    set(kPadding[i], t.view());
    t.clear();
    fixed_px(s.border_style[i] == BorderStyle::None || s.border_style[i] == BorderStyle::Hidden ? 0 : s.border_px100[i], &t);
    set(kWidth[i], t.view());
    set(kStyle[i], border_style_text(s.border_style[i]));
    t.clear();
    if (s.border_color[i] == kTransparentBorder) t.append("rgba(0, 0, 0, 0)");
    else rgb_text(s.border_color[i] ? s.border_color[i] : text_color, &t);
    set(kColor[i], t.view());
    t.clear();
    length_text(s.inset[i], &t);
    set(kInset[i], t.view());
  }
  const struct {
    const char* name;
    const Length* length;
  } kSizes[] = {{"width", &s.width}, {"height", &s.height}, {"minWidth", &s.min_width}, {"minHeight", &s.min_height}, {"maxWidth", &s.max_width}, {"maxHeight", &s.max_height},
                {"flexBasis", &s.flex_basis}, {"rowGap", &s.row_gap}, {"columnGap", &s.column_gap}};
  for (const auto& z : kSizes) {
    t.clear();
    length_text(*z.length, &t);
    if (t.view().equals("auto") && (z.name[0] == 'r' || z.name[0] == 'c')) t.assign("normal");
    if (z.length == &s.flex_basis && s.flex_basis_zero_percent && s.flex_basis.kind == Length::Px && s.flex_basis.px100 == 0) t.assign("0%");
    set(z.name, t.view());
  }
  set("boxSizing", s.sizing == BoxSizing::BorderBox ? "border-box" : "content-box");
  static const char* const kPosition[] = {"static", "relative", "absolute", "fixed", "sticky"};
  set("position", kPosition[static_cast<int>(s.position)]);
  const char* side = s.float_side == Float::Left ? "left" : s.float_side == Float::Right ? "right" : "none";
  set("float", side);
  set("cssFloat", side);
  set("clear", s.clear == Clear::Left ? "left" : s.clear == Clear::Right ? "right" : s.clear == Clear::Both ? "both" : "none");
  {
    // one axis that scrolls or clips makes a visible other axis auto
    Overflow ox = s.overflow_x, oy = s.overflow_y;
    if (ox == Overflow::Visible && oy != Overflow::Visible) ox = Overflow::Auto;
    else if (oy == Overflow::Visible && ox != Overflow::Visible) oy = Overflow::Auto;
    set("overflowX", overflow_text(ox));
    set("overflowY", overflow_text(oy));
    set("overflow", ox == oy ? overflow_text(ox) : "visible");
  }
  t.clear();
  number_text(s.opacity_x1000 / 1000.0, &t);
  set("opacity", t.view());
  t.clear();
  if (s.z_index == 0 && s.position == Position::Static) t.assign("auto");
  else {
    if (s.z_index < 0) t.push_back('-');
    t.append_uint(static_cast<uint32_t>(s.z_index < 0 ? -static_cast<int64_t>(s.z_index) : s.z_index));
  }
  set("zIndex", t.view());
  set("cursor", cursor_text(s.cursor));
  // flex and grid
  static const char* const kDirection[] = {"row", "row-reverse", "column", "column-reverse"};
  set("flexDirection", kDirection[static_cast<int>(s.flex_direction)]);
  set("flexWrap", s.flex_wrap == FlexWrap::Wrap ? "wrap" : s.flex_wrap == FlexWrap::WrapReverse ? "wrap-reverse" : "nowrap");
  set("justifyContent", place_text(s.justify_content));
  set("alignItems", place_text(s.align_items));
  set("alignContent", place_text(s.align_content));
  set("alignSelf", place_text(s.align_self));
  set("justifyItems", place_text(s.justify_items));
  set("justifySelf", place_text(s.justify_self));
  t.clear();
  t.append_uint(static_cast<uint32_t>(s.flex_grow_x100 / 100));
  if (s.flex_grow_x100 % 100) {
    t.push_back('.');
    t.append_uint(static_cast<uint32_t>(s.flex_grow_x100 % 100));
  }
  set("flexGrow", t.view());
  t.clear();
  t.append_uint(static_cast<uint32_t>(s.flex_shrink_x100 / 100));
  if (s.flex_shrink_x100 % 100) {
    t.push_back('.');
    t.append_uint(static_cast<uint32_t>(s.flex_shrink_x100 % 100));
  }
  set("flexShrink", t.view());
  t.clear();
  if (s.order < 0) t.push_back('-');
  t.append_uint(static_cast<uint32_t>(s.order < 0 ? -static_cast<int64_t>(s.order) : s.order));
  set("order", t.view());
  set("gridTemplateColumns", s.grid_template_columns.empty() ? "none" : "");
  set("gridTemplateRows", s.grid_template_rows.empty() ? "none" : "");
  // the pixel engine's properties
  allocators::IAllocator& alloc = vm.allocator();
  auto text_of = [&](TextRef ref, const char* fallback) { StringView v = sheet ? sheet->text(ref) : StringView(); return v.empty() ? StringView(fallback) : v; };
  t.clear();
  number_text(s.font_stretch_x10 / 10.0, &t);
  t.push_back('%');
  set("fontStretch", t.view());
  {
    const uint16_t v = s.font_variant;
    const char* caps = v & kVariantSmallCaps ? "small-caps" : v & kVariantAllSmallCaps ? "all-small-caps" : v & kVariantPetiteCaps ? "petite-caps" : v & kVariantAllPetiteCaps ? "all-petite-caps"
                       : v & kVariantUnicase ? "unicase" : v & kVariantTitlingCaps ? "titling-caps" : "normal";
    set("fontVariantCaps", caps);
    set("fontVariant", caps);
    t.clear();
    static const struct { uint16_t bit; const char* name; } kNumeric[] = {{kVariantLiningNums, "lining-nums"}, {kVariantOldstyleNums, "oldstyle-nums"}, {kVariantProportionalNums, "proportional-nums"},
                                                                         {kVariantTabularNums, "tabular-nums"}, {kVariantDiagonalFractions, "diagonal-fractions"},
                                                                         {kVariantStackedFractions, "stacked-fractions"}, {kVariantOrdinal, "ordinal"}, {kVariantSlashedZero, "slashed-zero"}};
    for (const auto& n : kNumeric)
      if (v & n.bit) {
        if (!t.empty()) t.push_back(' ');
        t.append(n.name);
      }
    set("fontVariantNumeric", t.empty() ? StringView("normal") : t.view());
    set("fontVariantLigatures", v & kVariantNoLigatures ? "none" : "normal");
  }
  t.clear();
  if (s.letter_spacing_px100 == 0) t.append("normal");
  else fixed_px(s.letter_spacing_px100, &t);
  set("letterSpacing", t.view());
  t.clear();
  if (s.word_spacing_px100 == 0) t.append("normal");
  else fixed_px(s.word_spacing_px100, &t);
  set("wordSpacing", t.view());
  {
    static const char* const kCorner[4] = {"borderTopLeftRadius", "borderTopRightRadius", "borderBottomRightRadius", "borderBottomLeftRadius"};
    String all(alloc);
    for (int i = 0; i < 4; ++i) {
      t.clear();
      dimension_text(s.radius[i].h, &t);
      if (s.radius[i].v.kind != s.radius[i].h.kind || s.radius[i].v.x100 != s.radius[i].h.x100) {
        t.push_back(' ');
        dimension_text(s.radius[i].v, &t);
      }
      set(kCorner[i], t.view());
      if (i) all.push_back(' ');
      all.append(t.view());
    }
    set("borderRadius", all.view());
  }
  if (sheet) {
    t.clear();
    shadows_text(*sheet, s.box_shadow, s, false, alloc, &t);
    set("boxShadow", t.view());
    t.clear();
    shadows_text(*sheet, s.text_shadow, s, true, alloc, &t);
    set("textShadow", t.view());
  } else {
    set("boxShadow", "none");
    set("textShadow", "none");
  }
  {
    t.clear();
    const StringView image = text_of(s.background_image, "none");
    image_list_text(image, &t);
    set("backgroundImage", t.view());
  }
  set("backgroundSize", text_of(s.background_size, "auto"));
  {
    t.clear();
    position_list_text(text_of(s.background_position, "0% 0%"), &t);
    set("backgroundPosition", t.view());
  }
  set("backgroundRepeat", text_of(s.background_repeat, "repeat"));
  set("backgroundClip", text_of(s.background_clip, "border-box"));
  set("backgroundOrigin", text_of(s.background_origin, "padding-box"));
  set("backgroundAttachment", text_of(s.background_attachment, "scroll"));
  // transform: the matrix of the transform functions (translate, rotate and scale first), or the text
  {
    double m[16];
    t.clear();
    if (sheet && sheet->transform_matrix(s, 0, 0, m, false)) {
      const bool flat = m[2] == 0 && m[3] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 0 && m[9] == 0 && m[10] == 1 && m[11] == 0 && m[14] == 0 && m[15] == 1;
      t.append(flat ? "matrix(" : "matrix3d(");
      const int order2d[6] = {0, 1, 4, 5, 12, 13};
      for (int i = 0; i < (flat ? 6 : 16); ++i) {
        if (i) t.append(", ");
        number_text(m[flat ? order2d[i] : i], &t);
      }
      t.push_back(')');
    } else {
      t.append("none");
    }
    set("transform", t.view());
  }
  set("translate", text_of(s.translate, "none"));
  set("rotate", text_of(s.rotate, "none"));
  set("scale", text_of(s.scale, "none"));
  t.clear();
  // (a percentage is of the box, which has no size until the page is rendered: like a browser that has not laid the element out, the result is in pixels)
  fixed_px(s.transform_origin_x.px100, &t);
  t.push_back(' ');
  fixed_px(s.transform_origin_y.px100, &t);
  if (s.transform_origin_z_px100) {
    t.push_back(' ');
    fixed_px(s.transform_origin_z_px100, &t);
  }
  set("transformOrigin", t.view());
  t.clear();
  fixed_px(s.outline_style == BorderStyle::None ? 0 : s.outline_width_px100, &t);
  set("outlineWidth", t.view());
  set("outlineStyle", s.outline_auto ? "auto" : border_style_text(s.outline_style));
  t.clear();
  if (s.outline_color == kTransparentBorder) t.append("rgba(0, 0, 0, 0)");
  else rgb_text(s.outline_color ? s.outline_color : text_color, &t);
  set("outlineColor", t.view());
  t.clear();
  fixed_px(s.outline_offset_px100, &t);
  set("outlineOffset", t.view());
  set("pointerEvents", s.pointer_events == PointerEvents::None ? "none" : "auto");
  {
    static const char* const kSelect[] = {"auto", "none", "text", "all", "contain"};
    set("userSelect", kSelect[static_cast<int>(s.user_select) < 5 ? static_cast<int>(s.user_select) : 0]);
  }
  t.clear();
  if (s.aspect_auto) t.append("auto");
  if (s.aspect_ratio_x1000) {
    if (!t.empty()) t.push_back(' ');
    if (s.aspect_w_x1000 && s.aspect_h_x1000) {   // as written: "16 / 9"
      number_text(s.aspect_w_x1000 / 1000.0, &t);
      t.append(" / ");
      number_text(s.aspect_h_x1000 / 1000.0, &t);
    } else {
      number_text(s.aspect_ratio_x1000 / 1000.0, &t);
      t.append(" / 1");
    }
  } else if (!s.aspect_auto) {
    t.append("auto");
  }
  set("aspectRatio", t.view());
  {
    static const char* const kFit[] = {"fill", "contain", "cover", "none", "scale-down"};
    set("objectFit", kFit[static_cast<int>(s.object_fit) < 5 ? static_cast<int>(s.object_fit) : 0]);
  }
  t.clear();
  axis_text(s.object_position_x, &t);
  t.push_back(' ');
  axis_text(s.object_position_y, &t);
  set("objectPosition", t.view());
  set("listStyleImage", text_of(s.list_style_image, "none"));
  t.clear();
  fixed_px(s.column_rule_style == BorderStyle::None ? 0 : s.column_rule_width_px100, &t);
  set("columnRuleWidth", t.view());
  set("columnRuleStyle", border_style_text(s.column_rule_style));
  t.clear();
  if (s.column_rule_color == kTransparentBorder) t.append("rgba(0, 0, 0, 0)");
  else rgb_text(s.column_rule_color ? s.column_rule_color : text_color, &t);
  set("columnRuleColor", t.view());
  set("transitionProperty", text_of(s.transition_property, "all"));
  set("transitionDuration", text_of(s.transition_duration, "0s"));
  set("transitionTimingFunction", text_of(s.transition_timing, "ease"));
  set("transitionDelay", text_of(s.transition_delay, "0s"));
  set("transitionBehavior", text_of(s.transition_behavior, "normal"));
  set("animationName", text_of(s.animation_name, "none"));
  set("animationDuration", text_of(s.animation_duration, "0s"));
  set("animationTimingFunction", text_of(s.animation_timing, "ease"));
  set("animationDelay", text_of(s.animation_delay, "0s"));
  set("animationIterationCount", text_of(s.animation_iteration_count, "1"));
  set("animationDirection", text_of(s.animation_direction, "normal"));
  set("animationFillMode", text_of(s.animation_fill_mode, "none"));
  set("animationPlayState", text_of(s.animation_play_state, "running"));
  set("filter", text_of(s.filter, "none"));
  set("backdropFilter", text_of(s.backdrop_filter, "none"));
  set("clipPath", text_of(s.clip_path, "none"));
  {
    static const char* const kWriting[] = {"horizontal-tb", "vertical-rl", "vertical-lr", "sideways-rl", "sideways-lr"};
    set("writingMode", kWriting[static_cast<int>(s.writing_mode) < 5 ? static_cast<int>(s.writing_mode) : 0]);
    set("direction", s.direction == Direction::Rtl ? "rtl" : "ltr");
    set("colorScheme", s.color_scheme == ColorScheme::Dark ? "dark" : s.color_scheme == ColorScheme::Light ? "light" : s.color_scheme == ColorScheme::LightDark ? "light dark" : "normal");
  }
  set("scrollSnapType", text_of(s.scroll_snap_type, "none"));
  set("scrollSnapAlign", text_of(s.scroll_snap_align, "none"));
  set("scrollSnapStop", s.scroll_snap_stop_always ? "always" : "normal");
  set("overscrollBehaviorX", overscroll_text(s.overscroll_x));
  set("overscrollBehaviorY", overscroll_text(s.overscroll_y));
  set("scrollBehavior", s.scroll_smooth ? "smooth" : "auto");
  set("containerType", s.container_type == 1 ? "inline-size" : s.container_type == 2 ? "size" : "normal");
  set("containerName", text_of(s.container_name, "none"));
  t.clear();
  if (s.text_indent.kind == Length::Px) fixed_px(s.text_indent.px100 ? s.text_indent.px100 : s.text_indent_px100, &t);
  else fixed_px(s.text_indent_px100, &t);
  set("textIndent", t.view());
  {
    const uint8_t w = s.white_space;   // 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line, 5 break-spaces
    set("textWrapMode", w == 1 || w == 2 ? "nowrap" : "wrap");
    set("whiteSpaceCollapse", w == 2 || w == 3 ? "preserve" : w == 4 ? "preserve-breaks" : w == 5 ? "break-spaces" : "collapse");
  }
  set("wordBreak", s.word_break_kind == 1 ? "break-all" : s.word_break_kind == 2 ? "keep-all" : s.word_break_kind == 3 ? "break-word" : "normal");
  set("overflowWrap", s.overflow_wrap_kind == 1 ? "anywhere" : s.overflow_wrap_kind == 2 ? "break-word" : "normal");
  set("wordWrap", s.overflow_wrap_kind == 1 ? "anywhere" : s.overflow_wrap_kind == 2 ? "break-word" : "normal");
  set("textOverflow", s.text_ellipsis ? "ellipsis" : "clip");
  set("quotes", text_of(s.quotes, "auto"));
  set("tabSize", text_of(s.tab_size, "8"));
  set("listStylePosition", s.list_inside ? "inside" : "outside");
  set("counterReset", text_of(s.counter_reset, "none"));
  set("counterIncrement", text_of(s.counter_increment, "none"));
  set("content", s.has_content ? text_of(s.content, "normal") : StringView(s.content_kind == 1 ? "none" : "normal"));
  set("gridColumnStart", text_of(s.grid_column_start, "auto"));
  set("gridColumnEnd", text_of(s.grid_column_end, "auto"));
  set("gridRowStart", text_of(s.grid_row_start, "auto"));
  set("gridRowEnd", text_of(s.grid_row_end, "auto"));
  if (s.column_count) {
    t.clear();
    t.append_uint(s.column_count);
    set("columnCount", t.view());
  } else {
    set("columnCount", "auto");
  }
  set("mixBlendMode", text_of(s.mix_blend_mode, "normal"));
  set("contain", text_of(s.contain, "none"));
  set("appearance", text_of(s.appearance, "none"));
  set("willChange", text_of(s.will_change, "auto"));
  if (s.accent_color_argb) {
    t.clear();
    if ((s.accent_color_argb >> 24) != 0xFF && (s.accent_color_argb >> 24) != 0) rgba_text(s.accent_color_argb, &t);
    else rgb_text(s.accent_color_argb & 0xFFFFFF, &t);
    set("accentColor", t.view());
  } else {
    set("accentColor", "auto");
  }
  set("borderCollapse", s.border_collapse ? "collapse" : "separate");
  t.clear();
  if (s.border_spacing.kind == Length::Px) fixed_px(s.border_spacing.px100, &t);
  else t.append("0px");
  set("borderSpacing", t.view());
  set("tableLayout", s.table_fixed ? "fixed" : "auto");
  set("captionSide", s.caption_bottom ? "bottom" : "top");
  if (sheet) {   // the custom properties that apply (the nearest declaration of a name wins)
    StringView cname, cvalue;
    uint32_t at = s.custom_scope;
    for (uint32_t guard = 0; guard < 4096 && sheet->custom_property_at(at, &cname, &cvalue, &at); ++guard) {
      Value existing;
      if (!vm.get_plain(o, vm.intern(cname), &existing)) vm.define(o, vm.intern(cname), str_value(vm, cvalue.trimmed()), kEnumerable);
    }
  }
  {   // length and the indexes list the properties (the names the item() method gives)
    JsObject* keys = vm.own_keys(o);
    uint32_t count = 0;
    for (uint32_t i = 0; i < keys->length; ++i) {
      const StringView k = keys->items[i].as_string()->view();
      if (k.empty() || (k[0] >= '0' && k[0] <= '9') || k.equals("length")) continue;
      String css(vm.allocator()), index(vm.allocator());
      if (k.starts_with("--")) css.assign(k);
      else css_name_from_js(k, &css);
      index.append_uint(count++);
      vm.define(o, vm.intern(index.view()), str_value(vm, css.equals("css-float") ? StringView("float") : css.view()), 0);
    }
    vm.define(o, vm.intern("length"), num_value(static_cast<double>(count)), 0);
  }
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
  if (!dom::get_attribute(data_node(o), attribute.view(), &v)) return false;   // (no such data attribute: the prototype chain answers, so `in` is false and undefined comes out)
  *out = str_value(vm, v);
  return true;
}

HostClass::Set dataset_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  const StringView k = key->view();
  if (k.empty() || (k.size() > 1 && k[0] == '_' && k[1] == '_')) return HostClass::Set::NotHandled;
  for (uint64_t i = 0; i + 1 < k.size(); ++i) {   // (a name with a hyphen before a lower case letter would not survive the round trip)
    if (k[i] == '-' && k[i + 1] >= 'a' && k[i + 1] <= 'z') {
      throw_dom(vm, "SyntaxError", "The property name contains a hyphen followed by a lower case letter.");
      return HostClass::Set::Threw;
    }
  }
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

NATIVE(css_supports_native) {   // CSS.supports(property, value) and CSS.supports(condition)
  String first(vm.allocator()), second(vm.allocator());
  if (argc == 0) return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'supports' on 'CSS': 1 argument required, but only 0 present.");
  if (!arg_text(vm, args[0], &first)) return false;
  if (argc >= 2) {
    if (!arg_text(vm, args[1], &second)) return false;
    String lowered(vm.allocator());
    for (uint64_t i = 0; i < first.size(); ++i) lowered.push_back(lower_char(first[i]));
    *result = bool_value(css_declaration_valid(lowered.view(), second.view()));
    return true;
  }
  const StringView t = first.view().trimmed();
  if (t.starts_with("(") || t.starts_with("not") || t.starts_with("selector(")) {
    *result = bool_value(css_supports(t));
  } else {
    String wrapped(vm.allocator());
    wrapped.push_back('(');
    wrapped.append(t);
    wrapped.push_back(')');
    *result = bool_value(css_supports(wrapped.view()));
  }
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
  vm.define_native(css, "supports", css_supports_native, 2);
  vm.define_named(vm.global(), "CSS", Value::make_object(css), kWritable);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
