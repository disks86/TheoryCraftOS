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

#include "Dom/Form.hpp"
#include "Utility/Move.hpp"

namespace dom {

namespace {

StringView attr(const Node* n, const char* name) {
  StringView v;
  return get_attribute(n, name, &v) ? v : StringView();
}

bool is_space_char(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// The text of an element with white space collapsed and trimmed.
void collapsed_text(const Node* n, String* out) {
  String raw(out->allocator());
  text_content(n, &raw);
  out->clear();
  bool space = false;
  for (uint64_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (is_space_char(c)) {
      space = out->size() > 0;
      continue;
    }
    if (space) out->push_back(' ');
    space = false;
    out->push_back(c);
  }
}

bool type_is(const Node* n, InputType t) { return n->is_tag("input") && input_type(n) == t; }

// A number or range field, or a date / time one: the types min and max mean something for.
bool has_range(InputType t) {
  return t == InputType::Number || t == InputType::Range || t == InputType::Date || t == InputType::Time || t == InputType::DateTimeLocal || t == InputType::Month ||
         t == InputType::Week;
}

bool is_numeric(InputType t) { return t == InputType::Number || t == InputType::Range; }

double step_of(const Node* n, bool* any) {
  *any = false;
  const StringView s = attr(n, "step").trimmed();
  if (s.equals_ignore_case("any")) {
    *any = true;
    return 1;
  }
  double v;
  if (!s.empty() && parse_number(s, &v) && v > 0) return v;
  return 1;
}

bool min_max(const Node* n, double* lo, double* hi) {
  const InputType t = input_type(n);
  bool has_lo = false, has_hi = false;
  double v;
  if (parse_number(attr(n, "min").trimmed(), &v)) *lo = v, has_lo = true;
  if (parse_number(attr(n, "max").trimmed(), &v)) *hi = v, has_hi = true;
  if (t == InputType::Range) {
    if (!has_lo) *lo = 0;
    if (!has_hi) *hi = 100;
    if (*hi < *lo) *hi = *lo;
    return true;
  }
  return has_lo || has_hi;
}

// What a value of this type is made to be.
void sanitize(const Node* n, StringView raw, String* out) {
  const InputType t = input_type(n);
  out->clear();
  switch (t) {
    case InputType::Text:
    case InputType::Search:
    case InputType::Tel:
    case InputType::Password:
      for (uint64_t i = 0; i < raw.size(); ++i)
        if (raw[i] != '\n' && raw[i] != '\r') out->push_back(raw[i]);
      return;
    case InputType::Url:
    case InputType::Email: {
      String stripped(out->allocator());
      for (uint64_t i = 0; i < raw.size(); ++i)
        if (raw[i] != '\n' && raw[i] != '\r') stripped.push_back(raw[i]);
      out->assign(stripped.view().trimmed());
      return;
    }
    case InputType::Number: {
      double v;
      if (parse_number(raw, &v)) out->assign(raw);
      return;
    }
    case InputType::Range: {
      double lo, hi, v;
      min_max(n, &lo, &hi);
      if (!parse_number(raw, &v)) v = lo + (hi - lo) / 2;
      if (v < lo) v = lo;
      if (v > hi) v = hi;
      bool any;
      const double step = step_of(n, &any);
      if (!any) {
        double q = (v - lo) / step;
        q = static_cast<double>(static_cast<int64_t>(q + 0.5));
        v = lo + q * step;
        if (v > hi) v -= step;
        if (v < lo) v = lo;
      }
      format_number(v, out);
      return;
    }
    case InputType::Color: {
      // #rrggbb, lowercase; anything else is black
      bool ok = raw.size() == 7 && raw[0] == '#';
      for (uint64_t i = 1; ok && i < 7; ++i) ok = (raw[i] >= '0' && raw[i] <= '9') || (raw[i] >= 'a' && raw[i] <= 'f') || (raw[i] >= 'A' && raw[i] <= 'F');
      if (!ok) {
        out->assign("#000000");
        return;
      }
      for (uint64_t i = 0; i < 7; ++i) out->push_back(raw[i] >= 'A' && raw[i] <= 'F' ? static_cast<char>(raw[i] + 32) : raw[i]);
      return;
    }
    default:
      for (uint64_t i = 0; i < raw.size(); ++i)
        if (raw[i] != '\n' && raw[i] != '\r') out->push_back(raw[i]);
      return;
  }
}

// A value that the markup and the types agree on, from the attribute.
void markup_value(const Node* n, String* out) { sanitize(n, attr(n, "value"), out); }

void normalize_newlines(StringView in, bool crlf, String* out) {
  for (uint64_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '\r') {
      if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
      out->append(crlf ? "\r\n" : "\n");
    } else if (c == '\n') {
      out->append(crlf ? "\r\n" : "\n");
    } else {
      out->push_back(c);
    }
  }
}

bool in_first_legend(const Node* control, const Node* fieldset) {
  const Node* legend = nullptr;
  for (const Node* c = fieldset->first_child; c; c = c->next)
    if (c->is_tag("legend")) {
      legend = c;
      break;
    }
  return legend && contains(legend, control);
}

}  // namespace

// --- Kinds -------------------------------------------------------------------------------------------------------------------

InputType input_type(const Node* input) {
  const StringView t = attr(input, "type").trimmed();
  static const struct {
    const char* name;
    InputType type;
  } kTypes[] = {{"search", InputType::Search}, {"tel", InputType::Tel}, {"url", InputType::Url}, {"email", InputType::Email}, {"password", InputType::Password},
                {"number", InputType::Number}, {"range", InputType::Range}, {"date", InputType::Date}, {"time", InputType::Time},
                {"datetime-local", InputType::DateTimeLocal}, {"month", InputType::Month}, {"week", InputType::Week}, {"color", InputType::Color},
                {"checkbox", InputType::Checkbox}, {"radio", InputType::Radio}, {"file", InputType::File}, {"hidden", InputType::Hidden}, {"image", InputType::Image},
                {"button", InputType::Button}, {"submit", InputType::Submit}, {"reset", InputType::Reset}};
  for (const auto& k : kTypes)
    if (t.equals_ignore_case(k.name)) return k.type;
  return InputType::Text;
}

bool is_text_field(InputType t) {
  switch (t) {
    case InputType::Text: case InputType::Search: case InputType::Tel: case InputType::Url: case InputType::Email: case InputType::Password: case InputType::Number:
    case InputType::Date: case InputType::Time: case InputType::DateTimeLocal: case InputType::Month: case InputType::Week: return true;
    default: return false;
  }
}

bool is_listed_control(const Node* n) {
  return n->is_element() && (n->is_tag("input") || n->is_tag("button") || n->is_tag("select") || n->is_tag("textarea") || n->is_tag("fieldset") || n->is_tag("output"));
}

bool is_submittable(const Node* n) { return n->is_element() && (n->is_tag("input") || n->is_tag("button") || n->is_tag("select") || n->is_tag("textarea")); }

bool is_button(const Node* n) {
  if (n->is_tag("button")) return true;
  if (!n->is_tag("input")) return false;
  const InputType t = input_type(n);
  return t == InputType::Submit || t == InputType::Reset || t == InputType::Button || t == InputType::Image;
}

bool is_submit_button(const Node* n) {
  if (n->is_tag("button")) {
    const StringView t = attr(n, "type").trimmed();
    return !t.equals_ignore_case("button") && !t.equals_ignore_case("reset");
  }
  if (!n->is_tag("input")) return false;
  const InputType t = input_type(n);
  return t == InputType::Submit || t == InputType::Image;
}

bool is_reset_button(const Node* n) {
  if (n->is_tag("button")) return attr(n, "type").trimmed().equals_ignore_case("reset");
  return n->is_tag("input") && input_type(n) == InputType::Reset;
}

Node* form_owner(const Node* control) {
  if (!is_listed_control(control)) return nullptr;
  StringView id;
  if (get_attribute(control, "form", &id)) {
    Node* f = control->owner->element_by_id(id);
    return f && f->is_tag("form") ? f : nullptr;
  }
  for (Node* p = control->parent; p; p = p->parent)
    if (p->is_tag("form")) return p;
  return nullptr;
}

void form_controls(const Node* form, List<Node*>* out) {
  const Node* root = form->owner->root();
  for (Node* n = root->first_child; n; n = next_in_tree(n, root))
    if (n->is_element() && is_listed_control(n) && form_owner(n) == form) out->push_back(n);
}

bool is_disabled(const Node* control) {
  if (control->is_tag("option")) {
    if (has_attribute(control, "disabled")) return true;
    return control->parent && control->parent->is_tag("optgroup") && has_attribute(control->parent, "disabled");
  }
  if (!is_listed_control(control) || control->is_tag("output")) return false;
  if (has_attribute(control, "disabled") && !control->is_tag("fieldset")) return true;
  if (control->is_tag("fieldset") && has_attribute(control, "disabled")) return true;
  for (const Node* p = control->parent; p; p = p->parent)
    if (p->is_tag("fieldset") && has_attribute(p, "disabled") && !in_first_legend(control, p)) return true;
  return false;
}

bool is_read_only(const Node* control) {
  if (control->is_tag("textarea")) return has_attribute(control, "readonly");
  if (!control->is_tag("input")) return false;
  const InputType t = input_type(control);
  return (is_text_field(t) || t == InputType::Color) && has_attribute(control, "readonly");
}

bool is_focusable(const Node* n) {
  if (!n->is_element()) return false;
  if (n->is_tag("input")) {
    const InputType t = input_type(n);
    if (t == InputType::Hidden || t == InputType::File || t == InputType::Color) return false;
  } else if (!(n->is_tag("button") || n->is_tag("select") || n->is_tag("textarea"))) {
    return false;
  }
  return !is_disabled(n);
}

// --- Values ------------------------------------------------------------------------------------------------------------------

void select_options(const Node* select, List<Node*>* out) {
  for (Node* n = select->first_child; n; n = next_in_tree(n, select)) {
    if (n->is_tag("option")) out->push_back(n);
  }
}

void option_text(const Node* option, String* out) { collapsed_text(option, out); }

void option_value(const Node* option, String* out) {
  StringView v;
  if (get_attribute(option, "value", &v)) out->assign(v);
  else collapsed_text(option, out);
}

bool select_is_list(const Node* select) {
  if (has_attribute(select, "multiple")) return true;
  uint64_t size = 0;
  return attr(select, "size").trimmed().parse_uint(&size) && size > 1;
}

bool is_checked(const Node* n) {
  if (n->form_state & kFormCheckedDirty) return n->form_state & kFormChecked;
  return has_attribute(n, n->is_tag("option") ? "selected" : "checked");
}

void set_checked(Node* n, bool checked) {
  n->form_state = static_cast<uint8_t>((n->form_state & ~kFormChecked) | kFormCheckedDirty | (checked ? kFormChecked : 0));
  if (checked && type_is(n, InputType::Radio)) {
    const StringView name = attr(n, "name");
    if (!name.empty()) {
      Node* owner = form_owner(n);
      const Node* root = owner ? owner : n->owner->root();
      for (Node* d = root->first_child; d; d = next_in_tree(d, root))
        if (d != n && type_is(d, InputType::Radio) && attr(d, "name").equals(name) && form_owner(d) == owner) {
          d->form_state = static_cast<uint8_t>((d->form_state & ~kFormChecked) | kFormCheckedDirty);
        }
    }
  }
  n->owner->touch();
}

int32_t selected_index(const Node* select) {
  List<Node*> options(select->owner->allocator());
  select_options(select, &options);
  int32_t last = -1, first_enabled = -1;
  for (uint64_t i = 0; i < options.count(); ++i) {
    if (is_checked(options[i])) {
      if (has_attribute(select, "multiple")) return static_cast<int32_t>(i);
      last = static_cast<int32_t>(i);
    }
    if (first_enabled < 0 && !is_disabled(options[i])) first_enabled = static_cast<int32_t>(i);
  }
  if (last >= 0) return last;
  if (select_is_list(select)) return -1;
  return first_enabled;
}

void set_selected_index(Node* select, int32_t index) {
  List<Node*> options(select->owner->allocator());
  select_options(select, &options);
  for (uint64_t i = 0; i < options.count(); ++i) {
    Node* o = options[i];
    o->form_state = static_cast<uint8_t>((o->form_state & ~kFormChecked) | kFormCheckedDirty | (static_cast<int32_t>(i) == index ? kFormChecked : 0));
  }
  select->form_state |= kFormValueDirty;
  select->owner->touch();
}

void select_all_off(Node* select) { set_selected_index(select, -1); }

void get_value(const Node* n, String* out) {
  out->clear();
  if (!n->is_element()) return;
  if (n->is_tag("input")) {
    const InputType t = input_type(n);
    switch (t) {
      case InputType::Checkbox:
      case InputType::Radio: {
        StringView v;
        out->assign(get_attribute(n, "value", &v) ? v : StringView("on"));
        return;
      }
      case InputType::File: return;
      case InputType::Hidden:
      case InputType::Button:
      case InputType::Submit:
      case InputType::Reset:
      case InputType::Image: out->assign(attr(n, "value")); return;
      default: break;
    }
    if (n->form_state & kFormValueDirty) {
      if (t == InputType::Range) sanitize(n, n->data.view(), out);
      else out->assign(n->data.view());
    } else {
      markup_value(n, out);
    }
    return;
  }
  if (n->is_tag("textarea")) {
    if (n->form_state & kFormValueDirty) {
      out->assign(n->data.view());
      return;
    }
    String raw(out->allocator());
    text_content(n, &raw);
    normalize_newlines(raw.view(), false, out);
    return;
  }
  if (n->is_tag("select")) {
    List<Node*> options(n->owner->allocator());
    select_options(n, &options);
    const int32_t i = selected_index(n);
    if (i >= 0) option_value(options[static_cast<uint64_t>(i)], out);
    return;
  }
  if (n->is_tag("option")) {
    option_value(n, out);
    return;
  }
  if (n->is_tag("button")) {
    out->assign(attr(n, "value"));
    return;
  }
  if (n->is_tag("output")) text_content(n, out);
}

bool set_value(Node* n, StringView value, bool do_sanitize) {
  if (!n->is_element()) return false;
  if (n->is_tag("input")) {
    const InputType t = input_type(n);
    switch (t) {
      case InputType::Checkbox:
      case InputType::Radio:
      case InputType::Hidden:
      case InputType::Button:
      case InputType::Submit:
      case InputType::Reset:
      case InputType::Image: return set_attribute(n, "value", value) == DomError::None;
      case InputType::File: return true;
      default: break;
    }
    String s(n->owner->allocator());
    if (do_sanitize) sanitize(n, value, &s);
    else s.assign(value);
    if (!n->data.assign(s.view())) return false;
    n->form_state |= kFormValueDirty;
    n->owner->touch();
    return true;
  }
  if (n->is_tag("textarea")) {
    String s(n->owner->allocator());
    normalize_newlines(value, false, &s);
    if (!n->data.assign(s.view())) return false;
    n->form_state |= kFormValueDirty;
    n->owner->touch();
    return true;
  }
  if (n->is_tag("select")) {
    List<Node*> options(n->owner->allocator());
    select_options(n, &options);
    int32_t found = -1;
    for (uint64_t i = 0; i < options.count() && found < 0; ++i) {
      String v(n->owner->allocator());
      option_value(options[i], &v);
      if (v.view().equals(value)) found = static_cast<int32_t>(i);
    }
    set_selected_index(n, found);
    return true;
  }
  if (n->is_tag("option") || n->is_tag("button")) return set_attribute(n, "value", value) == DomError::None;
  if (n->is_tag("output")) return set_text_content(n, value) == DomError::None;
  return false;
}

void default_value(const Node* n, String* out) {
  out->clear();
  if (n->is_tag("textarea")) {
    String raw(out->allocator());
    text_content(n, &raw);
    normalize_newlines(raw.view(), false, out);
  } else if (n->is_tag("output")) {
    text_content(n, out);
  } else {
    out->assign(attr(n, "value"));
  }
}

void reset_control(Node* n) {
  n->form_state &= static_cast<uint8_t>(kFormFocused);
  n->data.clear();
  if (n->is_tag("select")) {
    List<Node*> options(n->owner->allocator());
    select_options(n, &options);
    for (uint64_t i = 0; i < options.count(); ++i) options[i]->form_state = 0;
  }
  n->owner->touch();
}

void reset_form(Node* form) {
  List<Node*> controls(form->owner->allocator());
  form_controls(form, &controls);
  for (uint64_t i = 0; i < controls.count(); ++i) reset_control(controls[i]);
  form->owner->touch();
}

// --- Submission --------------------------------------------------------------------------------------------------------------

bool form_entries(const Node* form, const Node* submitter, List<Entry>* out) {
  allocators::IAllocator& a = form->owner->allocator();
  List<Node*> controls(a);
  form_controls(form, &controls);
  for (uint64_t i = 0; i < controls.count(); ++i) {
    const Node* n = controls[i];
    if (!is_submittable(n) || is_disabled(n)) continue;
    bool in_datalist = false;
    for (const Node* p = n->parent; p; p = p->parent) in_datalist = in_datalist || p->is_tag("datalist");
    if (in_datalist) continue;
    const StringView name = attr(n, "name");
    auto add = [&](StringView entry_name, StringView value) {
      Entry e(a);
      e.name.assign(entry_name);
      e.value.assign(value);
      return out->push_back(util::move(e));
    };
    if (n->is_tag("input")) {
      const InputType t = input_type(n);
      if (t == InputType::Image) {
        if (n != submitter) continue;
        String x(a), y(a);
        x.assign(name);
        y.assign(name);
        x.append(name.empty() ? "x" : ".x");
        y.append(name.empty() ? "y" : ".y");
        if (!add(x.view(), "0") || !add(y.view(), "0")) return false;
        continue;
      }
      if (name.empty()) continue;
      if (t == InputType::Submit || t == InputType::Button || t == InputType::Reset) {
        if (n != submitter || t != InputType::Submit) continue;
      }
      if ((t == InputType::Checkbox || t == InputType::Radio) && !is_checked(n)) continue;
      if (t == InputType::Hidden && name.equals_ignore_case("_charset_")) {
        if (!add(name, "UTF-8")) return false;
        continue;
      }
      String v(a);
      get_value(n, &v);
      if (t == InputType::Submit && !has_attribute(n, "value")) v.assign("Submit");
      if (!add(name, v.view())) return false;
      continue;
    }
    if (name.empty()) continue;
    if (n->is_tag("button")) {
      if (n != submitter) continue;
      if (!add(name, attr(n, "value"))) return false;
      continue;
    }
    if (n->is_tag("textarea")) {
      String v(a);
      get_value(n, &v);
      if (!add(name, v.view())) return false;
      continue;
    }
    if (n->is_tag("select")) {
      List<Node*> options(a);
      select_options(n, &options);
      const bool multiple = has_attribute(n, "multiple");
      const int32_t chosen = multiple ? -1 : selected_index(n);
      for (uint64_t k = 0; k < options.count(); ++k) {
        if (is_disabled(options[k])) continue;
        const bool on = multiple ? is_checked(options[k]) : static_cast<int32_t>(k) == chosen;
        if (!on) continue;
        String v(a);
        option_value(options[k], &v);
        if (!add(name, v.view())) return false;
      }
    }
  }
  return true;
}

bool url_encode(StringView text, String* out) {
  static const char kHex[] = "0123456789ABCDEF";
  for (uint64_t i = 0; i < text.size(); ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    bool ok;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') ok = out->push_back(static_cast<char>(c));
    else if (c == ' ') ok = out->push_back('+');
    else ok = out->push_back('%') && out->push_back(kHex[c >> 4]) && out->push_back(kHex[c & 15]);
    if (!ok) return false;
  }
  return true;
}

bool encode_urlencoded(const List<Entry>& entries, String* out) {
  out->clear();
  String tmp(out->allocator());
  for (uint64_t i = 0; i < entries.count(); ++i) {
    if (i > 0 && !out->push_back('&')) return false;
    tmp.clear();
    normalize_newlines(entries[i].name.view(), true, &tmp);
    if (!url_encode(tmp.view(), out) || !out->push_back('=')) return false;
    tmp.clear();
    normalize_newlines(entries[i].value.view(), true, &tmp);
    if (!url_encode(tmp.view(), out)) return false;
  }
  return true;
}

bool encode_multipart(const List<Entry>& entries, StringView boundary, String* out) {
  out->clear();
  String tmp(out->allocator());
  for (uint64_t i = 0; i < entries.count(); ++i) {
    out->append("--");
    out->append(boundary);
    out->append("\r\nContent-Disposition: form-data; name=\"");
    tmp.clear();
    normalize_newlines(entries[i].name.view(), true, &tmp);
    for (uint64_t k = 0; k < tmp.size(); ++k) {
      if (tmp[k] == '"') out->append("%22");
      else if (tmp[k] == '\r') out->append("%0D");
      else if (tmp[k] == '\n') out->append("%0A");
      else out->push_back(tmp[k]);
    }
    out->append("\"\r\n\r\n");
    tmp.clear();
    normalize_newlines(entries[i].value.view(), true, &tmp);
    out->append(tmp.view());
    if (!out->append("\r\n")) return false;
  }
  out->append("--");
  out->append(boundary);
  return out->append("--\r\n");
}

bool encode_plain(const List<Entry>& entries, String* out) {
  out->clear();
  String tmp(out->allocator());
  for (uint64_t i = 0; i < entries.count(); ++i) {
    tmp.clear();
    normalize_newlines(entries[i].name.view(), true, &tmp);
    out->append(tmp.view());
    out->push_back('=');
    tmp.clear();
    normalize_newlines(entries[i].value.view(), true, &tmp);
    out->append(tmp.view());
    if (!out->append("\r\n")) return false;
  }
  return true;
}

// --- Numbers -----------------------------------------------------------------------------------------------------------------

bool parse_number(StringView s, double* out) {
  uint64_t i = 0;
  bool negative = false;
  if (i < s.size() && s[i] == '-') {
    negative = true;
    ++i;
  }
  double mantissa = 0;
  uint64_t digits = 0, frac = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    mantissa = mantissa * 10 + (s[i] - '0');
    ++i;
    ++digits;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    uint64_t fd = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      mantissa = mantissa * 10 + (s[i] - '0');
      ++i;
      ++fd;
    }
    if (fd == 0) return false;
    frac = fd;
    digits += fd;
  }
  if (digits == 0) return false;
  int64_t exponent = 0;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool eneg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) eneg = s[i++] == '-';
    uint64_t ed = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && exponent < 400) {
      exponent = exponent * 10 + (s[i] - '0');
      ++i;
      ++ed;
    }
    if (ed == 0) return false;
    if (eneg) exponent = -exponent;
  }
  if (i != s.size()) return false;
  int64_t scale = exponent - static_cast<int64_t>(frac);
  while (scale > 0 && scale < 400) {
    mantissa *= 10;
    --scale;
  }
  while (scale < 0 && scale > -400) {
    mantissa /= 10;
    ++scale;
  }
  *out = negative ? -mantissa : mantissa;
  return true;
}

void format_number(double v, String* out) {
  if (!(v == v) || v > 1e15 || v < -1e15) {
    out->assign("0");
    return;
  }
  out->clear();
  if (v < 0) {
    out->push_back('-');
    v = -v;
  }
  uint64_t whole = static_cast<uint64_t>(v);
  uint64_t frac = static_cast<uint64_t>((v - static_cast<double>(whole)) * 1000000 + 0.5);
  if (frac >= 1000000) {
    ++whole;
    frac = 0;
  }
  if (out->size() == 1 && whole == 0 && frac == 0) out->clear();   // (no "-0")
  out->append_uint(whole);
  if (frac) {
    char digits[6];
    for (int k = 5; k >= 0; --k) {
      digits[k] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    int n = 6;
    while (n > 0 && digits[n - 1] == '0') --n;
    out->push_back('.');
    out->append(StringView(digits, static_cast<uint64_t>(n)));
  }
}

bool step_value(Node* n, int32_t steps) {
  if (!n->is_tag("input") || !is_numeric(input_type(n))) return false;
  String current(n->owner->allocator());
  get_value(n, &current);
  double v = 0, lo = 0, hi = 0;
  parse_number(current.view(), &v);
  bool any;
  const double step = step_of(n, &any);
  v += steps * step;
  bool has_lo = parse_number(attr(n, "min").trimmed(), &lo), has_hi = parse_number(attr(n, "max").trimmed(), &hi);
  if (input_type(n) == InputType::Range) min_max(n, &lo, &hi), has_lo = has_hi = true;
  if (has_lo && v < lo) v = lo;
  if (has_hi && v > hi) v = hi;
  String text(n->owner->allocator());
  format_number(v, &text);
  n->form_state |= kFormEdited;
  return set_value(n, text.view(), false);
}

// --- Regular expressions -----------------------------------------------------------------------------------------------------

namespace {

struct Cont {
  bool (*fn)(void*, uint64_t);
  void* ctx;
  bool operator()(uint64_t s) const { return fn(ctx, s); }
};

template <class F>
Cont make_cont(F& f) {
  return Cont{[](void* c, uint64_t s) { return (*static_cast<F*>(c))(s); }, &f};
}

class Regex {
 public:
  Regex(StringView pattern, StringView text) : p_(pattern), s_(text) {}

  bool match() {
    auto done = [&](uint64_t si) { return si == s_.size(); };
    Cont k = make_cont(done);
    const bool r = alt(0, p_.size(), 0, k);
    return gave_up_ ? true : r;
  }

 private:
  // Where the atom starting at `i` ends (an escape, a class, a group or one character).
  uint64_t atom_end(uint64_t i, uint64_t end) const {
    if (p_[i] == '\\') return i + 2 <= end ? i + 2 : end;
    if (p_[i] == '[') {
      uint64_t j = i + 1;
      if (j < end && p_[j] == '^') ++j;
      if (j < end && p_[j] == ']') ++j;
      while (j < end && p_[j] != ']') j += p_[j] == '\\' ? 2 : 1;
      return j < end ? j + 1 : end;
    }
    if (p_[i] == '(') {
      int depth = 0;
      uint64_t j = i;
      while (j < end) {
        if (p_[j] == '\\') {
          j += 2;
          continue;
        }
        if (p_[j] == '[') {
          j = atom_end(j, end);
          continue;
        }
        if (p_[j] == '(') ++depth;
        if (p_[j] == ')' && --depth == 0) return j + 1;
        ++j;
      }
      return end;
    }
    return i + 1;
  }

  bool class_has(char c, uint64_t i, uint64_t end) const {   // i..end is the inside of [...]
    bool negate = false;
    if (i < end && p_[i] == '^') {
      negate = true;
      ++i;
    }
    bool found = false;
    while (i < end) {
      char lo = p_[i];
      if (lo == '\\' && i + 1 < end) {
        ++i;
        if (escape_class(p_[i], c)) found = true;
        lo = escape_char(p_[i]);
      }
      if (i + 2 < end && p_[i + 1] == '-' && p_[i + 2] != ']') {
        char hi = p_[i + 2];
        i += 2;
        if (hi == '\\' && i + 1 < end) hi = escape_char(p_[++i]);
        if (c >= lo && c <= hi) found = true;
      } else if (c == lo) {
        found = true;
      }
      ++i;
    }
    return found != negate;
  }

  static char escape_char(char e) { return e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e; }

  static bool escape_class(char e, char c) {
    const bool digit = c >= '0' && c <= '9', word = digit || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'), space = is_space_char(c);
    switch (e) {
      case 'd': return digit;
      case 'D': return !digit;
      case 'w': return word;
      case 'W': return !word;
      case 's': return space;
      case 'S': return !space;
      default: return false;
    }
  }

  // Does the single-character atom at i..e match `c`?
  bool one(uint64_t i, uint64_t e, char c) const {
    const char a = p_[i];
    if (a == '.') return c != '\n';
    if (a == '[') return class_has(c, i + 1, e > i + 1 && p_[e - 1] == ']' ? e - 1 : e);
    if (a == '\\' && i + 1 < e) {
      const char x = p_[i + 1];
      if (x == 'd' || x == 'D' || x == 'w' || x == 'W' || x == 's' || x == 'S') return escape_class(x, c);
      return c == escape_char(x);
    }
    return c == a;
  }

  bool alt(uint64_t ps, uint64_t pe, uint64_t si, const Cont& k) {
    uint64_t start = ps;
    int depth = 0;
    for (uint64_t i = ps; i <= pe; ++i) {
      if (i < pe && p_[i] == '\\') {
        ++i;
        continue;
      }
      if (i < pe && p_[i] == '[') {
        i = atom_end(i, pe) - 1;
        continue;
      }
      if (i < pe && p_[i] == '(') ++depth;
      if (i < pe && p_[i] == ')') --depth;
      if (i == pe || (p_[i] == '|' && depth == 0)) {
        if (seq(start, i, si, k)) return true;
        start = i + 1;
      }
    }
    return false;
  }

  bool seq(uint64_t ps, uint64_t pe, uint64_t si, const Cont& k) {
    if (++steps_ > 400000) {
      gave_up_ = true;
      return true;
    }
    if (ps >= pe) return k(si);
    const char a = p_[ps];
    if (a == '^') return si == 0 && seq(ps + 1, pe, si, k);
    if (a == '$') return si == s_.size() && seq(ps + 1, pe, si, k);
    const uint64_t ae = atom_end(ps, pe);
    uint64_t qe = ae;
    int64_t lo = 1, hi = 1;
    if (ae < pe) {
      const char q = p_[ae];
      if (q == '*') lo = 0, hi = -1, qe = ae + 1;
      else if (q == '+') lo = 1, hi = -1, qe = ae + 1;
      else if (q == '?') lo = 0, hi = 1, qe = ae + 1;
      else if (q == '{') {
        uint64_t j = ae + 1;
        int64_t n = 0, m = -1;
        bool digits = false;
        while (j < pe && p_[j] >= '0' && p_[j] <= '9') n = n * 10 + (p_[j++] - '0'), digits = true;
        if (digits && j < pe && p_[j] == '}') {
          lo = hi = n;
          qe = j + 1;
        } else if (digits && j < pe && p_[j] == ',') {
          ++j;
          bool d2 = false;
          m = 0;
          while (j < pe && p_[j] >= '0' && p_[j] <= '9') m = m * 10 + (p_[j++] - '0'), d2 = true;
          if (j < pe && p_[j] == '}') {
            lo = n;
            hi = d2 ? m : -1;
            qe = j + 1;
          }
        }
      }
      if (qe > ae && qe < pe && p_[qe] == '?') ++qe;   // (lazy: tried as greedy)
    }
    return repeat(ps, ae, qe, pe, lo, hi, 0, si, k);
  }

  bool repeat(uint64_t as, uint64_t ae, uint64_t qe, uint64_t pe, int64_t lo, int64_t hi, int64_t count, uint64_t si, const Cont& k) {
    if (++steps_ > 400000) {
      gave_up_ = true;
      return true;
    }
    if (hi < 0 || count < hi) {
      auto next = [&](uint64_t ni) { return (ni != si || count < lo) && repeat(as, ae, qe, pe, lo, hi, count + 1, ni, k); };
      Cont nk = make_cont(next);
      if (p_[as] == '(') {
        uint64_t inner = as + 1;
        if (inner + 1 < ae && p_[inner] == '?' && p_[inner + 1] == ':') inner += 2;
        if (alt(inner, ae - 1, si, nk)) return true;
      } else if (si < s_.size() && one(as, ae, s_[si])) {
        if (next(si + 1)) return true;
      }
    }
    if (count >= lo) {
      auto rest = [&](uint64_t ni) { return seq(qe, pe, ni, k); };
      return rest(si);
    }
    return false;
  }

  StringView p_, s_;
  uint64_t steps_ = 0;
  bool gave_up_ = false;
};

}  // namespace

bool regex_match(StringView pattern, StringView text) {
  int depth = 0;
  for (uint64_t i = 0; i < pattern.size(); ++i) {   // (brackets must balance, or the pattern cannot be read)
    if (pattern[i] == '\\') ++i;
    else if (pattern[i] == '(') ++depth;
    else if (pattern[i] == ')' && --depth < 0) return true;
  }
  if (depth != 0) return true;
  Regex r(pattern, text);
  return r.match();
}

// --- Validity ----------------------------------------------------------------------------------------------------------------

bool will_validate(const Node* n) {
  if (!is_submittable(n) || is_disabled(n)) return false;
  for (const Node* p = n->parent; p; p = p->parent)
    if (p->is_tag("datalist")) return false;
  if (n->is_tag("input")) {
    const InputType t = input_type(n);
    if (t == InputType::Hidden || t == InputType::Button || t == InputType::Reset) return false;
    return !is_read_only(n);
  }
  if (n->is_tag("button")) return is_submit_button(n);
  if (n->is_tag("textarea")) return !is_read_only(n);
  return true;
}

namespace {

bool valid_email(StringView v) {
  const uint64_t at = v.find('@');
  if (at == StringView::npos || at == 0 || at + 1 >= v.size() || v.find('@', at + 1) != StringView::npos) return false;
  for (uint64_t i = 0; i < v.size(); ++i)
    if (static_cast<uint8_t>(v[i]) <= ' ' || v[i] == ',' || v[i] == ';') return false;
  const StringView domain = v.substr(at + 1);
  if (domain.starts_with('.') || domain.ends_with('.') || domain.starts_with('-')) return false;
  for (uint64_t i = 0; i < domain.size(); ++i) {
    const char c = domain[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    if (c == '.' && i + 1 < domain.size() && domain[i + 1] == '.') return false;
  }
  return true;
}

bool valid_url(StringView v) {
  if (v.empty() || !((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= 'A' && v[0] <= 'Z'))) return false;
  uint64_t i = 1;
  while (i < v.size() && ((v[i] >= 'a' && v[i] <= 'z') || (v[i] >= 'A' && v[i] <= 'Z') || (v[i] >= '0' && v[i] <= '9') || v[i] == '+' || v[i] == '-' || v[i] == '.')) ++i;
  if (i >= v.size() || v[i] != ':' || i + 1 >= v.size()) return false;
  for (uint64_t k = 0; k < v.size(); ++k)
    if (static_cast<uint8_t>(v[k]) <= ' ') return false;
  return true;
}

bool radio_group_has_checked(const Node* radio) {
  const StringView name = attr(radio, "name");
  if (name.empty()) return is_checked(radio);
  Node* owner = form_owner(radio);
  const Node* root = owner ? owner : radio->owner->root();
  for (Node* d = root->first_child; d; d = next_in_tree(d, root))
    if (type_is(d, InputType::Radio) && attr(d, "name").equals(name) && form_owner(d) == owner && is_checked(d)) return true;
  return false;
}

}  // namespace

uint32_t validity(const Node* n, const ValidityHooks* hooks) {
  uint32_t flags = 0;
  if (hooks && hooks->has_custom_error && hooks->has_custom_error(hooks->context, n)) flags |= kCustomError;
  if (!will_validate(n)) return flags;
  allocators::IAllocator& a = n->owner->allocator();
  const bool required = has_attribute(n, "required");
  if (n->is_tag("select")) {
    if (required) {
      List<Node*> options(a);
      select_options(n, &options);
      bool any = false;
      if (has_attribute(n, "multiple")) {
        for (uint64_t i = 0; i < options.count(); ++i) any = any || (is_checked(options[i]) && !is_disabled(options[i]));
      } else {
        const int32_t i = selected_index(n);
        if (i >= 0) {
          String v(a);
          option_value(options[static_cast<uint64_t>(i)], &v);
          // (the placeholder: the first option, with an empty value, in a select that shows one line)
          any = !(v.empty() && i == 0 && !select_is_list(n));
        }
      }
      if (!any) flags |= kValueMissing;
    }
    return flags;
  }
  String value(a);
  get_value(n, &value);
  if (n->is_tag("textarea")) {
    if (required && value.empty()) flags |= kValueMissing;
    uint64_t limit;
    if (attr(n, "maxlength").trimmed().parse_uint(&limit) && value.size() > limit && (n->form_state & kFormEdited)) flags |= kTooLong;
    if (attr(n, "minlength").trimmed().parse_uint(&limit) && !value.empty() && value.size() < limit && (n->form_state & kFormEdited)) flags |= kTooShort;
    return flags;
  }
  if (!n->is_tag("input")) return flags;
  const InputType t = input_type(n);
  if (t == InputType::Checkbox) {
    if (required && !is_checked(n)) flags |= kValueMissing;
    return flags;
  }
  if (t == InputType::Radio) {
    if (required && !radio_group_has_checked(n)) flags |= kValueMissing;
    return flags;
  }
  if (t == InputType::File) {
    if (required) flags |= kValueMissing;
    return flags;
  }
  if (t == InputType::Submit || t == InputType::Image) return flags;
  if (required && value.empty()) flags |= kValueMissing;
  if (is_text_field(t) || t == InputType::Color) {
    uint64_t limit;
    if (attr(n, "maxlength").trimmed().parse_uint(&limit) && value.size() > limit && (n->form_state & kFormEdited)) flags |= kTooLong;
    if (attr(n, "minlength").trimmed().parse_uint(&limit) && !value.empty() && value.size() < limit && (n->form_state & kFormEdited)) flags |= kTooShort;
  }
  if (value.empty()) {
    if (t == InputType::Number && (n->form_state & kFormValueDirty) && !n->data.empty()) flags |= kBadInput;
    return flags;
  }
  if (t == InputType::Email) {
    bool ok = true;
    if (has_attribute(n, "multiple")) {
      uint64_t start = 0;
      for (uint64_t i = 0; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == ',') {
          ok = ok && valid_email(value.view().substr(start, i - start).trimmed());
          start = i + 1;
        }
      }
    } else {
      ok = valid_email(value.view());
    }
    if (!ok) flags |= kTypeMismatch;
  } else if (t == InputType::Url) {
    if (!valid_url(value.view())) flags |= kTypeMismatch;
  }
  const StringView pattern = attr(n, "pattern");
  if (!pattern.empty() && (t == InputType::Text || t == InputType::Search || t == InputType::Tel || t == InputType::Url || t == InputType::Email || t == InputType::Password) &&
      !regex_match(pattern, value.view()))
    flags |= kPatternMismatch;
  if (has_range(t)) {
    if (is_numeric(t)) {
      double v;
      if (!parse_number(value.view(), &v)) {
        flags |= kBadInput;
        return flags;
      }
      double lo = 0, hi = 0;
      bool has_lo = parse_number(attr(n, "min").trimmed(), &lo), has_hi = parse_number(attr(n, "max").trimmed(), &hi);
      if (t == InputType::Range) min_max(n, &lo, &hi), has_lo = has_hi = true;
      if (has_lo && v < lo) flags |= kRangeUnderflow;
      if (has_hi && v > hi) flags |= kRangeOverflow;
      bool any;
      const double step = step_of(n, &any);
      if (!any) {
        double base = has_lo ? lo : 0;
        if (!has_lo) parse_number(attr(n, "value").trimmed(), &base);
        const double q = (v - base) / step;
        const double nearest = static_cast<double>(static_cast<int64_t>(q < 0 ? q - 0.5 : q + 0.5));
        double diff = q - nearest;
        if (diff < 0) diff = -diff;
        if (diff > 1e-7 * (q < 0 ? -q : q < 1 ? 1 : q)) flags |= kStepMismatch;
      }
    } else {   // dates and times compare as text (they are written so that they sort)
      const StringView lo = attr(n, "min").trimmed(), hi = attr(n, "max").trimmed();
      if (!lo.empty() && value.view().compare(lo) < 0) flags |= kRangeUnderflow;
      if (!hi.empty() && value.view().compare(hi) > 0) flags |= kRangeOverflow;
    }
  }
  return flags;
}

bool validation_message(const Node* n, uint32_t flags, String* out) {
  out->clear();
  if (flags == 0) return true;
  const InputType t = n->is_tag("input") ? input_type(n) : InputType::Text;
  if (flags & kBadInput) return out->assign("Please enter a number.");
  if (flags & kValueMissing) {
    if (t == InputType::Checkbox) return out->assign("Please check this box if you want to proceed.");
    if (t == InputType::Radio) return out->assign("Please select one of these options.");
    if (t == InputType::File) return out->assign("Please select a file.");
    if (n->is_tag("select")) return out->assign("Please select an item in the list.");
    return out->assign("Please fill out this field.");
  }
  if (flags & kTypeMismatch) {
    if (t == InputType::Url) return out->assign("Please enter a URL.");
    String value(n->owner->allocator());
    get_value(n, &value);
    return out->assign(value.view().contains('@') ? "Please enter a valid email address." : "Please include an '@' in the email address.");
  }
  if (flags & kPatternMismatch) {
    const StringView title = attr(n, "title");
    out->assign("Please match the requested format.");
    if (!title.empty()) {
      out->push_back(' ');
      out->append(title);
    }
    return true;
  }
  if (flags & kTooLong) {
    out->assign("Please shorten this text to ");
    out->append(attr(n, "maxlength").trimmed());
    return out->append(" characters or less.");
  }
  if (flags & kTooShort) {
    out->assign("Please lengthen this text to ");
    out->append(attr(n, "minlength").trimmed());
    return out->append(" characters or more.");
  }
  if (flags & kRangeUnderflow) {
    out->assign("Value must be greater than or equal to ");
    out->append(attr(n, "min").trimmed());
    return out->push_back('.');
  }
  if (flags & kRangeOverflow) {
    out->assign("Value must be less than or equal to ");
    out->append(attr(n, "max").trimmed());
    return out->push_back('.');
  }
  if (flags & kStepMismatch) return out->assign("Please enter a valid value.");
  return out->assign("Invalid value.");
}

Node* first_invalid(const Node* form, const ValidityHooks* hooks) {
  List<Node*> controls(form->owner->allocator());
  form_controls(form, &controls);
  for (uint64_t i = 0; i < controls.count(); ++i)
    if (validity(controls[i], hooks) != 0) return controls[i];
  return nullptr;
}

}  // namespace dom
