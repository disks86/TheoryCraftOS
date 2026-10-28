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

// Forms for scripts: the properties and methods of <form> and its controls (value, checked, selectedIndex, validity, form.elements, submit(),
// requestSubmit(), reset(), checkValidity(), focus() ...) and FormData. What the controls hold and how they are checked is Dom/Form.hpp; focus, submission
// and the clicks are the page's (Page.cpp, reached through IDomHost).
#include <initializer_list>
#include "Browser/DomBindingsInternal.hpp"
#include "Dom/Form.hpp"
#include "Utility/Move.hpp"

namespace browser {
namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)
#define IS(text) k.equals(text)

double nan_value() {
  const uint64_t bits = 0x7FF8000000000000ull;
  double d;
  __builtin_memcpy(&d, &bits, sizeof d);
  return d;
}

StringView attr_of(const dom::Node* n, const char* name) {
  StringView v;
  return dom::get_attribute(n, name, &v) ? v : StringView();
}

dom::ValidityHooks hooks_for(DomBindings::Impl& c, bool (**fn)(void*, const dom::Node*)) {
  (void)fn;
  dom::ValidityHooks h;
  h.has_custom_error = [](void* context, const dom::Node* control) { return static_cast<DomBindings::Impl*>(context)->owner.custom_validity(control, nullptr); };
  h.context = &c;
  return h;
}


Value make_validity(DomBindings::Impl& c, dom::Node* n) {
  Vm& vm = c.vm;
  const dom::ValidityHooks hooks = hooks_for(c, nullptr);
  const uint32_t f = dom::will_validate(n) || c.owner.custom_validity(n, nullptr) ? dom::validity(n, &hooks) : 0;
  JsObject* v = vm.new_plain();
  static const struct {
    const char* name;
    uint32_t bit;
  } kBits[] = {{"valueMissing", dom::kValueMissing}, {"typeMismatch", dom::kTypeMismatch}, {"patternMismatch", dom::kPatternMismatch}, {"tooLong", dom::kTooLong},
               {"tooShort", dom::kTooShort}, {"rangeUnderflow", dom::kRangeUnderflow}, {"rangeOverflow", dom::kRangeOverflow}, {"stepMismatch", dom::kStepMismatch},
               {"badInput", dom::kBadInput}, {"customError", dom::kCustomError}};
  for (const auto& b : kBits) vm.define_named(v, b.name, bool_value(f & b.bit));
  vm.define_named(v, "valid", bool_value(f == 0));
  return Value::make_object(v);
}

const char* type_name(const dom::Node* n) {
  if (n->is_tag("select")) return dom::has_attribute(n, "multiple") ? "select-multiple" : "select-one";
  if (n->is_tag("textarea")) return "textarea";
  if (n->is_tag("fieldset")) return "fieldset";
  if (n->is_tag("output")) return "output";
  if (n->is_tag("button")) return dom::is_reset_button(n) ? "reset" : dom::is_submit_button(n) ? "submit" : "button";
  switch (dom::input_type(n)) {
    case dom::InputType::Search: return "search";
    case dom::InputType::Tel: return "tel";
    case dom::InputType::Url: return "url";
    case dom::InputType::Email: return "email";
    case dom::InputType::Password: return "password";
    case dom::InputType::Number: return "number";
    case dom::InputType::Range: return "range";
    case dom::InputType::Date: return "date";
    case dom::InputType::Time: return "time";
    case dom::InputType::DateTimeLocal: return "datetime-local";
    case dom::InputType::Month: return "month";
    case dom::InputType::Week: return "week";
    case dom::InputType::Color: return "color";
    case dom::InputType::Checkbox: return "checkbox";
    case dom::InputType::Radio: return "radio";
    case dom::InputType::File: return "file";
    case dom::InputType::Hidden: return "hidden";
    case dom::InputType::Image: return "image";
    case dom::InputType::Button: return "button";
    case dom::InputType::Submit: return "submit";
    case dom::InputType::Reset: return "reset";
    default: return "text";
  }
}

// The labels that name a control.
void labels_of(const dom::Node* control, List<dom::Node*>* out) {
  StringView id;
  const bool has_id = dom::get_attribute(control, "id", &id) && !id.empty();
  const dom::Node* root = control->owner->root();
  for (dom::Node* l = root->first_child; l; l = dom::next_in_tree(l, root)) {
    if (!l->is_tag("label")) continue;
    StringView f;
    if (dom::get_attribute(l, "for", &f)) {
      if (has_id && f.equals(id)) out->push_back(l);
    } else if (dom::contains(l, control)) {
      out->push_back(l);
    }
  }
}

Value elements_value(DomBindings::Impl& c, dom::Node* form) {
  Vm& vm = c.vm;
  List<dom::Node*> list(vm.allocator());
  if (form->is_tag("form")) {
    dom::form_controls(form, &list);
  } else {   // a fieldset: the controls inside
    for (dom::Node* d = form->first_child; d; d = dom::next_in_tree(d, form))
      if (d->is_element() && dom::is_listed_control(d)) list.push_back(d);
  }
  JsObject* collection = make_collection(c, list);
  if (form->is_tag("form")) set_interface(vm, collection, "HTMLFormControlsCollection");
  for (uint64_t i = 0; i < list.count(); ++i) {   // form.elements.name
    StringView v;
    for (const char* attribute : {"id", "name"}) {
      if (dom::get_attribute(list[i], attribute, &v) && !v.empty() && !(v[0] >= '0' && v[0] <= '9')) {
        Value existing;
        if (!vm.get_named(Value::make_object(collection), v, &existing) || existing.is_undefined()) {
          List<dom::Node*> group(vm.allocator());   // several controls with the name: a list of them (a RadioNodeList: its value is the checked radio button's)
          for (uint64_t k = 0; k < list.count(); ++k) {
            StringView other;
            if (dom::get_attribute(list[k], "name", &other) && other.equals(v)) group.push_back(list[k]);
          }
          if (group.count() > 1) {
            JsObject* nodes = make_collection(c, group);
            String chosen(vm.allocator());
            for (uint64_t k = 0; k < group.count(); ++k)
              if (group[k]->is_tag("input") && dom::input_type(group[k]) == dom::InputType::Radio && dom::is_checked(group[k])) {
                dom::get_value(group[k], &chosen);
                break;
              }
            vm.define_named(nodes, "value", str_value(vm, chosen.view()), kWritable);
            vm.define_named(collection, v, Value::make_object(nodes), kWritable);
          } else {
            vm.define_named(collection, v, node_value(c, list[i]), kWritable);
          }
        }
      }
    }
  }
  return Value::make_object(collection);
}

}  // namespace

bool is_form_element(const dom::Node* n) {
  return n->is_element() && (dom::is_listed_control(n) || n->is_tag("form") || n->is_tag("option") || n->is_tag("label") || n->is_tag("progress") || n->is_tag("meter") ||
                             n->is_tag("optgroup") || n->is_tag("legend") || n->is_tag("dialog") || n->is_tag("iframe"));
}

bool form_get(DomBindings::Impl& c, dom::Node* n, StringView k, Value* out) {
  Vm& vm = c.vm;
  allocators::IAllocator& a = vm.allocator();
  const bool control = dom::is_listed_control(n);
  if (n->is_tag("form")) {
    if (IS("elements")) { *out = elements_value(c, n); return true; }
    if (IS("length")) {
      List<dom::Node*> list(a);
      dom::form_controls(n, &list);
      *out = num_value(static_cast<double>(list.count()));
      return true;
    }
    if (IS("method")) {
      StringView m;
      const bool post = dom::get_attribute(n, "method", &m) && m.trimmed().equals_ignore_case("post");
      const bool dialog = dom::get_attribute(n, "method", &m) && m.trimmed().equals_ignore_case("dialog");
      *out = str_value(vm, dialog ? "dialog" : post ? "post" : "get");
      return true;
    }
    if (IS("enctype") || IS("encoding")) {
      StringView e;
      const bool multipart = dom::get_attribute(n, "enctype", &e) && e.trimmed().equals_ignore_case("multipart/form-data");
      const bool plain = dom::get_attribute(n, "enctype", &e) && e.trimmed().equals_ignore_case("text/plain");
      *out = str_value(vm, multipart ? "multipart/form-data" : plain ? "text/plain" : "application/x-www-form-urlencoded");
      return true;
    }
    if (IS("acceptCharset")) {
      StringView v;
      *out = str_value(vm, dom::get_attribute(n, "accept-charset", &v) ? v : StringView());
      return true;
    }
    return false;
  }
  if (n->is_tag("iframe")) {
    if (IS("contentWindow")) {   // (a stand-in for the window of the page in the frame: postMessage goes to it; made by the page's built-in script)
      const uint32_t id = c.host.frame_window_id(n->serial);
      Value make;
      *out = Value::make_null();
      if (id && vm.get_named(Value::make_object(vm.global()), "__window_proxy", &make) && Vm::is_callable(make)) {
        const Value arg = num_value(id);
        if (!vm.call(make, Value(), &arg, 1, out)) {
          String ignored(vm.allocator());
          vm.take_exception(&ignored);
          *out = Value::make_null();
        }
      }
      return true;
    }
    if (IS("contentDocument")) {   // (the page of a frame has a script engine of its own: its document is not reachable from here)
      *out = Value::make_null();
      return true;
    }
    return false;
  }
  if (n->is_tag("dialog")) {
    if (IS("returnValue")) {
      *out = str_value(vm, n->data.view());
      return true;
    }
    return false;
  }
  if (n->is_tag("label")) {
    if (IS("control")) {
      StringView f;
      dom::Node* target = nullptr;
      if (dom::get_attribute(n, "for", &f)) {
        dom::Node* e = n->owner->element_by_id(f);
        if (e && dom::is_listed_control(e) && !e->is_tag("fieldset") && !e->is_tag("output")) target = e;
      } else {
        for (dom::Node* d = n->first_child; d && !target; d = dom::next_in_tree(d, n))
          if (d->is_element() && dom::is_submittable(d) && !(d->is_tag("input") && dom::input_type(d) == dom::InputType::Hidden)) target = d;
      }
      *out = node_value(c, target);
      return true;
    }
    if (IS("form")) {
      dom::Node* f = nullptr;
      for (dom::Node* p = n->parent; p && !f; p = p->parent)
        if (p->is_tag("form")) f = p;
      *out = node_value(c, f);
      return true;
    }
    return false;
  }
  if (n->is_tag("option")) {
    if (IS("selected")) { *out = bool_value(dom::is_checked(n)); return true; }
    if (IS("defaultSelected")) { *out = bool_value(dom::has_attribute(n, "selected")); return true; }
    if (IS("value")) {
      String v(a);
      dom::option_value(n, &v);
      *out = str_value(vm, v.view());
      return true;
    }
    if (IS("label")) {
      StringView l;
      String t(a);
      if (dom::get_attribute(n, "label", &l) && !l.empty()) t.assign(l);
      else dom::option_text(n, &t);
      *out = str_value(vm, t.view());
      return true;
    }
    if (IS("text")) {
      String t(a);
      dom::option_text(n, &t);
      *out = str_value(vm, t.view());
      return true;
    }
    if (IS("index")) {
      dom::Node* s = n->parent;
      while (s && !s->is_tag("select")) s = s->parent;
      int32_t index = 0;
      if (s) {
        List<dom::Node*> options(a);
        dom::select_options(s, &options);
        for (uint64_t i = 0; i < options.count(); ++i)
          if (options[i] == n) index = static_cast<int32_t>(i);
      }
      *out = num_value(index);
      return true;
    }
    if (IS("form")) {
      dom::Node* s = n->parent;
      while (s && !s->is_tag("select")) s = s->parent;
      *out = node_value(c, s ? dom::form_owner(s) : nullptr);
      return true;
    }
    return false;
  }
  if (n->is_tag("progress") || n->is_tag("meter")) {
    double v = 0, mx = 1, mn = 0;
    StringView s;
    const bool known = dom::get_attribute(n, "value", &s) && dom::parse_number(s.trimmed(), &v);
    if (!dom::get_attribute(n, "max", &s) || !dom::parse_number(s.trimmed(), &mx) || (n->is_tag("progress") && mx <= 0)) mx = n->is_tag("progress") ? 1 : 1;
    if (n->is_tag("meter") && dom::get_attribute(n, "min", &s)) dom::parse_number(s.trimmed(), &mn);
    if (IS("value")) { *out = num_value(v < mn ? mn : v > mx ? mx : v); return true; }
    if (IS("max")) { *out = num_value(mx); return true; }
    if (IS("min") && n->is_tag("meter")) { *out = num_value(mn); return true; }
    if (IS("position")) { *out = num_value(known && mx > mn ? (v - mn) / (mx - mn) : -1); return true; }
    return false;
  }
  if (!control) return false;
  const bool input = n->is_tag("input"), textarea = n->is_tag("textarea"), select = n->is_tag("select"), button = n->is_tag("button");
  if (IS("form")) { *out = node_value(c, dom::form_owner(n)); return true; }
  if (IS("type")) {
    *out = str_value(vm, type_name(n));
    return true;
  }
  if (IS("willValidate")) { *out = bool_value(dom::will_validate(n)); return true; }
  if (IS("validity")) { *out = make_validity(c, n); return true; }
  if (IS("validationMessage")) {
    String m(a);
    if (dom::will_validate(n)) {
      const dom::ValidityHooks hooks = hooks_for(c, nullptr);
      const uint32_t f = dom::validity(n, &hooks);
      if (!(f & dom::kCustomError) || !c.owner.custom_validity(n, &m)) dom::validation_message(n, f, &m);
    }
    *out = str_value(vm, m.view());
    return true;
  }
  if (IS("labels")) {
    List<dom::Node*> list(a);
    labels_of(n, &list);
    *out = Value::make_object(make_collection(c, list));
    return true;
  }
  if (IS("minLength")) {
    StringView v;
    uint64_t u;
    *out = num_value(dom::get_attribute(n, "minlength", &v) && v.trimmed().parse_uint(&u) ? static_cast<double>(u) : -1);
    return true;
  }
  if (IS("elements") && n->is_tag("fieldset")) { *out = elements_value(c, n); return true; }
  if (IS("value") && (input || textarea || select || button || n->is_tag("output"))) {
    String v(a);
    dom::get_value(n, &v);
    *out = str_value(vm, v.view());
    return true;
  }
  if (IS("defaultValue") && (input || textarea || n->is_tag("output"))) {
    String v(a);
    dom::default_value(n, &v);
    *out = str_value(vm, v.view());
    return true;
  }
  if (IS("textLength") && (input || textarea)) {
    String v(a);
    dom::get_value(n, &v);
    *out = num_value(static_cast<double>(utf16_length(v.view())));
    return true;
  }
  if ((IS("selectionStart") || IS("selectionEnd")) && (input || textarea)) {
    uint32_t s, e, caret;
    dom::get_selection(n, &s, &e, &caret);
    String v(a);
    dom::get_value(n, &v);
    *out = num_value(static_cast<double>(byte_to_utf16(v.view(), IS("selectionStart") ? s : e)));
    return true;
  }
  if (IS("selectionDirection") && (input || textarea)) { *out = str_value(vm, "none"); return true; }
  if (input) {
    if (IS("checked")) { *out = bool_value(dom::is_checked(n)); return true; }
    if (IS("defaultChecked")) { *out = bool_value(dom::has_attribute(n, "checked")); return true; }
    if (IS("indeterminate")) { *out = bool_value(n->form_state & dom::kFormIndeterminate); return true; }
    if (IS("files")) {
      JsObject* list = vm.new_array();
      if (dom::input_type(n) == dom::InputType::File) {
        StringView name, type, bytes;
        for (uint32_t i = 0; dom::file_at(n, i, &name, &type, &bytes); ++i) {
          JsObject* f = vm.new_plain();
          vm.define_named(f, "name", str_value(vm, name));
          vm.define_named(f, "type", str_value(vm, type));
          vm.define_named(f, "size", num_value(static_cast<double>(bytes.size())));
          vm.define_named(f, "lastModified", num_value(0));
          vm.array_push(list, Value::make_object(f));
        }
      }
      set_interface(vm, list, "FileList");
      *out = Value::make_object(list);
      return true;
    }
    if (IS("list")) {
      const dom::Node* l = n->owner->element_by_id(attr_of(n, "list"));
      *out = node_value(c, l && l->is_tag("datalist") ? const_cast<dom::Node*>(l) : nullptr);
      return true;
    }
    if (IS("valueAsNumber")) {
      String v(a);
      dom::get_value(n, &v);
      double d;
      *out = num_value(dom::parse_number(v.view(), &d) && (dom::input_type(n) == dom::InputType::Number || dom::input_type(n) == dom::InputType::Range) ? d : nan_value());
      return true;
    }
    if (IS("valueAsDate")) { *out = Value::make_null(); return true; }
  }
  if (select) {
    if (IS("selectedIndex")) { *out = num_value(dom::selected_index(n)); return true; }
    if (IS("length")) {
      List<dom::Node*> options(a);
      dom::select_options(n, &options);
      *out = num_value(static_cast<double>(options.count()));
      return true;
    }
    if (IS("selectedOptions")) {
      List<dom::Node*> options(a), chosen(a);
      dom::select_options(n, &options);
      const int32_t one = dom::has_attribute(n, "multiple") ? -2 : dom::selected_index(n);
      for (uint64_t i = 0; i < options.count(); ++i)
        if (one == -2 ? dom::is_checked(options[i]) : static_cast<int32_t>(i) == one) chosen.push_back(options[i]);
      *out = Value::make_object(make_collection(c, chosen));
      return true;
    }
  }
  return false;
}

// form[name] and the like: the control of the form with that id or name.
static bool form_named(DomBindings::Impl& c, dom::Node* form, StringView k, Value* out) {
  List<dom::Node*> list(c.vm.allocator());
  dom::form_controls(form, &list);
  dom::Node* found = nullptr;
  for (uint64_t i = 0; i < list.count() && !found; ++i) {
    StringView v;
    if ((dom::get_attribute(list[i], "id", &v) && v.equals(k)) || (dom::get_attribute(list[i], "name", &v) && v.equals(k))) found = list[i];
  }
  if (!found) return false;
  *out = node_value(c, found);
  return true;
}

bool form_lookup_named(DomBindings::Impl& c, dom::Node* n, StringView k, Value* out) { return n->is_tag("form") && form_named(c, n, k, out); }

HostClass::Set form_set(DomBindings::Impl& c, dom::Node* n, StringView k, const Value& value) {
  Vm& vm = c.vm;
  allocators::IAllocator& a = vm.allocator();
  String text(a);
  auto text_arg = [&]() { return arg_text(vm, value, &text); };
  if (n->is_tag("form")) {
    if (IS("elements") || IS("length")) return HostClass::Set::Handled;
    if (IS("method") || IS("enctype") || IS("encoding")) {
      if (!text_arg()) return HostClass::Set::Threw;
      dom::set_attribute(n, IS("method") ? "method" : "enctype", text.view());
      return HostClass::Set::Handled;
    }
    return HostClass::Set::NotHandled;
  }
  if (n->is_tag("dialog")) {
    if (IS("returnValue")) {
      if (!text_arg()) return HostClass::Set::Threw;
      n->data.assign(text.view());
      return HostClass::Set::Handled;
    }
    return HostClass::Set::NotHandled;
  }
  if (n->is_tag("option")) {
    if (IS("selected")) {
      dom::Node* s = n->parent;
      while (s && !s->is_tag("select")) s = s->parent;
      const bool on = Vm::to_boolean(value);
      if (s && on && !dom::has_attribute(s, "multiple")) {
        List<dom::Node*> options(a);
        dom::select_options(s, &options);
        for (uint64_t i = 0; i < options.count(); ++i)
          if (options[i] == n) dom::set_selected_index(s, static_cast<int32_t>(i));
      } else {
        dom::set_checked(n, on);
      }
      return HostClass::Set::Handled;
    }
    if (IS("defaultSelected")) {
      if (Vm::to_boolean(value)) dom::set_attribute(n, "selected", "");
      else dom::remove_attribute(n, "selected");
      return HostClass::Set::Handled;
    }
    if (IS("value") || IS("label")) {
      if (!text_arg()) return HostClass::Set::Threw;
      dom::set_attribute(n, k, text.view());
      return HostClass::Set::Handled;
    }
    if (IS("index") || IS("form")) return HostClass::Set::Handled;
    return HostClass::Set::NotHandled;
  }
  if (n->is_tag("progress") || n->is_tag("meter")) {
    if (IS("value") || IS("max") || IS("min")) {
      double d;
      if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
      String s(a);
      dom::format_number(d, &s);
      dom::set_attribute(n, k, s.view());
      return HostClass::Set::Handled;
    }
    return HostClass::Set::NotHandled;
  }
  if (!dom::is_listed_control(n)) return HostClass::Set::NotHandled;
  if (IS("form") || IS("willValidate") || IS("validity") || IS("validationMessage") || IS("labels") || IS("textLength") || IS("selectionDirection") || IS("files") ||
      IS("list") || IS("length") || IS("selectedOptions") || IS("elements") || IS("valueAsDate"))
    return HostClass::Set::Handled;
  if (IS("type")) {
    if (!text_arg()) return HostClass::Set::Threw;
    dom::set_attribute(n, "type", text.view());
    return HostClass::Set::Handled;
  }
  if (IS("minLength")) {
    double d;
    if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
    String s(a);
    s.append_uint(d > 0 ? static_cast<uint64_t>(d) : 0);
    dom::set_attribute(n, "minlength", s.view());
    return HostClass::Set::Handled;
  }
  if (IS("value")) {
    if (!text_arg()) return HostClass::Set::Threw;
    if (n->is_tag("input") && dom::input_type(n) == dom::InputType::File && !text.empty()) {
      throw_dom(vm, "InvalidStateError", "This input element accepts a filename, which may only be programmatically set to the empty string.");
      return HostClass::Set::Threw;
    }
    n->form_state &= static_cast<uint8_t>(~dom::kFormEdited);
    if (n->is_tag("input") && dom::input_type(n) == dom::InputType::File) {
      dom::clear_files(n);
      return HostClass::Set::Handled;
    }
    dom::set_value(n, text.view());
    return HostClass::Set::Handled;
  }
  if (IS("defaultValue")) {
    if (!text_arg()) return HostClass::Set::Threw;
    if (n->is_tag("textarea") || n->is_tag("output")) dom::set_text_content(n, text.view());
    else dom::set_attribute(n, "value", text.view());
    return HostClass::Set::Handled;
  }
  if (IS("valueAsNumber")) {
    double d;
    if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
    String s(a);
    dom::format_number(d, &s);
    dom::set_value(n, s.view());
    return HostClass::Set::Handled;
  }
  if (IS("checked")) {
    if (n->is_tag("input")) dom::set_checked(n, Vm::to_boolean(value));
    return HostClass::Set::Handled;
  }
  if (IS("indeterminate")) {
    if (Vm::to_boolean(value)) n->form_state |= dom::kFormIndeterminate;
    else n->form_state &= static_cast<uint8_t>(~dom::kFormIndeterminate);
    n->owner->touch();
    return HostClass::Set::Handled;
  }
  if ((IS("selectionStart") || IS("selectionEnd")) && (n->is_tag("input") || n->is_tag("textarea"))) {
    double d;
    if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
    uint32_t s, e, caret;
    dom::get_selection(n, &s, &e, &caret);
    String text(vm.allocator());
    dom::get_value(n, &text);
    const uint32_t v = d > 0 ? static_cast<uint32_t>(utf16_to_byte(text.view(), static_cast<uint64_t>(d))) : 0;
    if (IS("selectionStart")) dom::set_selection(n, v, e < v ? v : e);
    else dom::set_selection(n, s > v ? v : s, v);
    return HostClass::Set::Handled;
  }
  if (IS("defaultChecked")) {
    if (Vm::to_boolean(value)) dom::set_attribute(n, "checked", "");
    else dom::remove_attribute(n, "checked");
    return HostClass::Set::Handled;
  }
  if (IS("selectedIndex") && n->is_tag("select")) {
    double d;
    if (!vm.to_number(value, &d)) return HostClass::Set::Threw;
    dom::set_selected_index(n, static_cast<int32_t>(d));
    return HostClass::Set::Handled;
  }
  return HostClass::Set::NotHandled;
}

namespace {

// --- Methods -------------------------------------------------------------------------------------------------------------------

NATIVE(el_submit) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("form")) return vm.throw_error(ErrorKind::TypeError, "submit is not a function");
  impl(vm).host.submit_form(n, nullptr, false);
  return true;
}

NATIVE(el_request_submit) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("form")) return vm.throw_error(ErrorKind::TypeError, "requestSubmit is not a function");
  dom::Node* submitter = argc > 0 && !args[0].is_nullish() ? node_of(args[0]) : nullptr;
  if (argc > 0 && !args[0].is_nullish()) {
    if (!submitter) return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'requestSubmit' on 'HTMLFormElement': parameter 1 is not of type 'HTMLElement'.");
    if (!dom::is_submit_button(submitter) || dom::form_owner(submitter) != n) return throw_dom(vm, "NotFoundError", "The specified element is not a submit button of this form.");
  }
  c.host.submit_form(n, submitter, true);
  return true;
}

NATIVE(el_reset) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("form")) return vm.throw_error(ErrorKind::TypeError, "reset is not a function");
  impl(vm).host.reset_form(n);
  return true;
}

NATIVE(el_check_validity) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  const dom::ValidityHooks hooks = hooks_for(c, nullptr);
  bool valid = true;
  auto check = [&](dom::Node* control) {
    if (!dom::will_validate(control) && !c.owner.custom_validity(control, nullptr)) return;
    if (dom::validity(control, &hooks) == 0) return;
    valid = false;
    EventInit init;
    init.bubbles = false;
    c.owner.dispatch(control, "invalid", init);
  };
  if (n->is_tag("form")) {
    List<dom::Node*> list(vm.allocator());
    dom::form_controls(n, &list);
    for (uint64_t i = 0; i < list.count(); ++i) check(list[i]);
  } else if (dom::is_listed_control(n)) {
    check(n);
  }
  *result = bool_value(valid);
  return true;
}

NATIVE(el_report_validity) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  const dom::ValidityHooks hooks = hooks_for(c, nullptr);
  bool valid = true;
  if (n->is_tag("form")) {
    List<dom::Node*> list(vm.allocator());
    dom::form_controls(n, &list);
    for (uint64_t i = 0; i < list.count() && valid; ++i)
      if ((dom::will_validate(list[i]) || c.owner.custom_validity(list[i], nullptr)) && dom::validity(list[i], &hooks) != 0) valid = c.host.report_validity(list[i]);
  } else if (dom::is_listed_control(n)) {
    valid = c.host.report_validity(n);
  }
  *result = bool_value(valid);
  return true;
}

NATIVE(el_set_custom_validity) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  String message(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &message)) return false;
  for (uint64_t i = 0; i < c.custom_validity.count(); ++i)
    if (c.custom_validity[i].serial == n->serial) {
      c.custom_validity[i].message.assign(message.view());
      return true;
    }
  if (!message.empty()) {
    DomBindings::Impl::CustomValidity entry(vm.allocator());
    entry.serial = n->serial;
    entry.message.assign(message.view());
    c.custom_validity.push_back(util::move(entry));
  }
  return true;
}

NATIVE(el_focus) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (dom::is_focusable(n)) impl(vm).host.set_focus(n);
  return true;
}

NATIVE(el_blur) {
  DomBindings::Impl& c = impl(vm);
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (c.host.focused() == n) c.host.set_focus(nullptr);
  return true;
}

NATIVE(el_step_up) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  double d = 1;
  if (argc > 0 && !args[0].is_undefined() && !vm.to_number(args[0], &d)) return false;
  if (!dom::step_value(n, static_cast<int32_t>(d))) return throw_dom(vm, "InvalidStateError", "This form element is not steppable.");
  return true;
}

NATIVE(el_step_down) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  double d = 1;
  if (argc > 0 && !args[0].is_undefined() && !vm.to_number(args[0], &d)) return false;
  if (!dom::step_value(n, static_cast<int32_t>(-d))) return throw_dom(vm, "InvalidStateError", "This form element is not steppable.");
  return true;
}

NATIVE(el_dialog_show) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("dialog")) return vm.throw_error(ErrorKind::TypeError, "show is not a function");
  impl(vm).host.show_dialog(n, false);
  return true;
}

NATIVE(el_dialog_show_modal) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("dialog")) return vm.throw_error(ErrorKind::TypeError, "showModal is not a function");
  if (dom::has_attribute(n, "open")) return throw_dom(vm, "InvalidStateError", "The element already has an 'open' attribute, and therefore cannot be opened modally.");
  impl(vm).host.show_dialog(n, true);
  return true;
}

NATIVE(el_dialog_close) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (!n->is_tag("dialog")) return vm.throw_error(ErrorKind::TypeError, "close is not a function");
  String value(vm.allocator());
  const bool given = argc > 0 && !args[0].is_undefined();
  if (given && !arg_text(vm, args[0], &value)) return false;
  impl(vm).host.close_dialog(n, given ? value.c_str() : nullptr);
  return true;
}

NATIVE(el_select_text) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  if (n->is_tag("input") || n->is_tag("textarea")) {
    String v(vm.allocator());
    dom::get_value(n, &v);
    if (dom::is_focusable(n)) impl(vm).host.set_focus(n);
    dom::set_selection(n, 0, static_cast<uint32_t>(v.size()));
  }
  return true;
}

NATIVE(el_set_selection_range) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  double a = 0, b = 0;
  if (argc > 0 && !vm.to_number(args[0], &a)) return false;
  if (argc > 1 && !vm.to_number(args[1], &b)) return false;
  if (n->is_tag("input") || n->is_tag("textarea")) {
    String text(vm.allocator());
    dom::get_value(n, &text);
    dom::set_selection(n, a > 0 ? static_cast<uint32_t>(utf16_to_byte(text.view(), static_cast<uint64_t>(a))) : 0, b > 0 ? static_cast<uint32_t>(utf16_to_byte(text.view(), static_cast<uint64_t>(b))) : 0);
  }
  return true;
}

// setRangeText(text) replaces the selection; setRangeText(text, start, end) a range.
NATIVE(el_set_range_text) {
  dom::Node* n = this_node(vm, self);
  if (!n) return false;
  String text(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &text)) return false;
  if (!(n->is_tag("input") || n->is_tag("textarea"))) return true;
  if (argc >= 3) {
    double a = 0, b = 0;
    if (!vm.to_number(args[1], &a) || !vm.to_number(args[2], &b)) return false;
    String current(vm.allocator());
    dom::get_value(n, &current);
    dom::set_selection(n, a > 0 ? static_cast<uint32_t>(utf16_to_byte(current.view(), static_cast<uint64_t>(a))) : 0, b > 0 ? static_cast<uint32_t>(utf16_to_byte(current.view(), static_cast<uint64_t>(b))) : 0);
  }
  dom::replace_selection(n, text.view(), false);
  return true;
}

NATIVE(el_nothing) {
  (void)vm; (void)self; (void)args; (void)argc; (void)result;
  return true;
}

// --- FormData: an object whose entries are an array of [name, value] pairs ----------------------------------------------------------------

JsObject* entries_of(Vm& vm, const Value& self) {
  Value e;
  if (!self.is_object() || !vm.get_named(self, "__entries", &e) || !e.is_object()) return nullptr;
  return e.as_object();
}

Value pair(Vm& vm, JsString* name, const Value& value) {
  JsObject* p = vm.new_array(2);
  vm.array_push(p, Value::make_string(name));
  vm.array_push(p, value);
  return Value::make_object(p);
}

bool entry_name(Vm& vm, const Value& entry, String* out) {
  Value n;
  if (!entry.is_object() || !vm.get(entry, vm.intern("0"), &n)) return false;
  return arg_text(vm, n, out);
}

NATIVE(formdata_constructor) {
  DomBindings::Impl& c = impl(vm);
  if (!self.is_object()) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'FormData': Please use the 'new' operator.");
  JsObject* entries = vm.new_array();
  vm.define_named(self.as_object(), "__entries", Value::make_object(entries), kWritable);
  if (argc > 0 && !args[0].is_nullish()) {
    dom::Node* form = node_of(args[0]);
    if (!form || !form->is_tag("form")) return vm.throw_error(ErrorKind::TypeError, "Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'.");
    dom::Node* submitter = argc > 1 ? node_of(args[1]) : nullptr;
    List<dom::Entry> list(vm.allocator());
    if (!dom::form_entries(form, submitter, &list)) return vm.throw_error(ErrorKind::Error, "out of memory");
    for (uint64_t i = 0; i < list.count(); ++i) {
      Value v = str_value(vm, list[i].value.view());
      if (list[i].file) {   // (a file is an object with its name, type and size)
        JsObject* f = vm.new_plain();
        vm.define_named(f, "name", str_value(vm, list[i].filename.view()));
        vm.define_named(f, "type", str_value(vm, list[i].content_type.view()));
        vm.define_named(f, "size", num_value(static_cast<double>(list[i].value.size())));
        vm.define_named(f, "lastModified", num_value(0));
        v = Value::make_object(f);
      }
      vm.array_push(entries, pair(vm, vm.new_string(list[i].name.view()), v));
    }
  }
  (void)c;
  *result = self;
  return true;
}

// A File or Blob is kept as the object it is (it carries `__bytes`); anything else is turned into text.
bool entry_value(Vm& vm, const Value& v, Value* out) {
  Value bytes;
  if (v.is_object() && vm.get_named(v, "__bytes", &bytes) && bytes.is_string()) {
    *out = v;
    return true;
  }
  String text(vm.allocator());
  if (!arg_text(vm, v, &text)) return false;
  *out = str_value(vm, text.view());
  return true;
}

NATIVE(formdata_append) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  String name(vm.allocator());
  Value value;
  if (!arg_text(vm, arg_at(args, argc, 0), &name) || !entry_value(vm, arg_at(args, argc, 1), &value)) return false;
  vm.array_push(entries, pair(vm, vm.new_string(name.view()), value));
  return true;
}

// Which entries have this name: the indexes.
bool find_all(Vm& vm, JsObject* entries, StringView name, List<uint32_t>* at) {
  for (uint32_t i = 0; i < entries->length; ++i) {
    String n(vm.allocator());
    if (entry_name(vm, entries->items[i], &n) && n.view().equals(name)) at->push_back(i);
  }
  return true;
}

NATIVE(formdata_get) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  List<uint32_t> at(vm.allocator());
  find_all(vm, entries, name.view(), &at);
  *result = Value::make_null();
  if (at.count()) return vm.get(entries->items[at[0]], vm.intern("1"), result);
  return true;
}

NATIVE(formdata_get_all) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  List<uint32_t> at(vm.allocator());
  find_all(vm, entries, name.view(), &at);
  JsObject* list = vm.new_array();
  for (uint64_t i = 0; i < at.count(); ++i) {
    Value v;
    if (!vm.get(entries->items[at[i]], vm.intern("1"), &v)) return false;
    vm.array_push(list, v);
  }
  *result = Value::make_object(list);
  return true;
}

NATIVE(formdata_has) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  List<uint32_t> at(vm.allocator());
  find_all(vm, entries, name.view(), &at);
  *result = bool_value(at.count() > 0);
  return true;
}

// Rebuilds the entries without those at the indexes `drop` (and with `replacement` put where the first of them was, if given).
bool rebuild(Vm& vm, const Value& self, JsObject* entries, const List<uint32_t>& drop, const Value* replacement) {
  JsObject* fresh = vm.new_array();
  bool placed = replacement == nullptr;
  for (uint32_t i = 0; i < entries->length; ++i) {
    bool dropped = false;
    for (uint64_t k = 0; k < drop.count(); ++k) dropped = dropped || drop[k] == i;
    if (!dropped) {
      vm.array_push(fresh, entries->items[i]);
    } else if (!placed) {
      vm.array_push(fresh, *replacement);
      placed = true;
    }
  }
  if (!placed) vm.array_push(fresh, *replacement);
  vm.define_named(self.as_object(), "__entries", Value::make_object(fresh), kWritable);
  return true;
}

NATIVE(formdata_delete) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  String name(vm.allocator());
  if (!arg_text(vm, arg_at(args, argc, 0), &name)) return false;
  List<uint32_t> at(vm.allocator());
  find_all(vm, entries, name.view(), &at);
  return rebuild(vm, self, entries, at, nullptr);
}

NATIVE(formdata_set) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  String name(vm.allocator());
  Value value;
  if (!arg_text(vm, arg_at(args, argc, 0), &name) || !entry_value(vm, arg_at(args, argc, 1), &value)) return false;
  List<uint32_t> at(vm.allocator());
  find_all(vm, entries, name.view(), &at);
  const Value p = pair(vm, vm.new_string(name.view()), value);
  return rebuild(vm, self, entries, at, &p);
}

// entries(), keys() and values() give arrays (which a for...of reads); forEach(callback) calls it with (value, name, formData).
NATIVE(formdata_entries) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* list = vm.new_array();
  for (uint32_t i = 0; i < entries->length; ++i) vm.array_push(list, entries->items[i]);
  *result = Value::make_object(list);
  return true;
}

NATIVE(formdata_keys) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* list = vm.new_array();
  for (uint32_t i = 0; i < entries->length; ++i) {
    Value v;
    if (!vm.get(entries->items[i], vm.intern("0"), &v)) return false;
    vm.array_push(list, v);
  }
  *result = Value::make_object(list);
  return true;
}

NATIVE(formdata_values) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  JsObject* list = vm.new_array();
  for (uint32_t i = 0; i < entries->length; ++i) {
    Value v;
    if (!vm.get(entries->items[i], vm.intern("1"), &v)) return false;
    vm.array_push(list, v);
  }
  *result = Value::make_object(list);
  return true;
}

NATIVE(formdata_for_each) {
  JsObject* entries = entries_of(vm, self);
  if (!entries) return vm.throw_error(ErrorKind::TypeError, "Illegal invocation");
  const Value callback = arg_at(args, argc, 0);
  if (!Vm::is_callable(callback)) return vm.throw_error(ErrorKind::TypeError, "Failed to execute 'forEach' on 'FormData': parameter 1 is not of type 'Function'.");
  for (uint32_t i = 0; i < entries->length; ++i) {
    Value n, v, ignored;
    if (!vm.get(entries->items[i], vm.intern("0"), &n) || !vm.get(entries->items[i], vm.intern("1"), &v)) return false;
    const Value call_args[3] = {v, n, self};
    if (!vm.call(callback, arg_at(args, argc, 1), call_args, 3, &ignored)) return false;
  }
  return true;
}

}  // namespace

bool install_forms(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* ep = c.element_proto;
  vm.define_native(ep, "submit", el_submit, 0);
  vm.define_native(ep, "requestSubmit", el_request_submit, 0);
  vm.define_native(ep, "reset", el_reset, 0);
  vm.define_native(ep, "checkValidity", el_check_validity, 0);
  vm.define_native(ep, "reportValidity", el_report_validity, 0);
  vm.define_native(ep, "setCustomValidity", el_set_custom_validity, 1);
  vm.define_native(ep, "focus", el_focus, 0);
  vm.define_native(ep, "blur", el_blur, 0);
  vm.define_native(ep, "select", el_select_text, 0);
  vm.define_native(ep, "setSelectionRange", el_set_selection_range, 2);
  vm.define_native(ep, "setRangeText", el_set_range_text, 1);
  vm.define_native(ep, "showPicker", el_nothing, 0);
  vm.define_native(ep, "show", el_dialog_show, 0);
  vm.define_native(ep, "showModal", el_dialog_show_modal, 0);
  vm.define_native(ep, "close", el_dialog_close, 0);
  vm.define_native(ep, "stepUp", el_step_up, 0);
  vm.define_native(ep, "stepDown", el_step_down, 0);

  JsObject* proto = vm.new_plain();
  vm.define_native(proto, "append", formdata_append, 2);
  vm.define_native(proto, "get", formdata_get, 1);
  vm.define_native(proto, "getAll", formdata_get_all, 1);
  vm.define_native(proto, "has", formdata_has, 1);
  vm.define_native(proto, "delete", formdata_delete, 1);
  vm.define_native(proto, "set", formdata_set, 2);
  vm.define_native(proto, "entries", formdata_entries, 0);
  vm.define_native(proto, "keys", formdata_keys, 0);
  vm.define_native(proto, "values", formdata_values, 0);
  vm.define_native(proto, "forEach", formdata_for_each, 1);
  JsObject* ctor = vm.new_native(formdata_constructor, "FormData", 0);
  vm.define(ctor, vm.prototype_name(), Value::make_object(proto), kNonConfigurable);
  vm.define(proto, vm.intern("constructor"), Value::make_object(ctor), kWritable);
  vm.define_named(vm.global(), "FormData", Value::make_object(ctor), kWritable);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
