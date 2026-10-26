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


#include "Script/RegExp.hpp"
#include <new>
#include "Regex/Regex.hpp"

namespace script {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

Value arg_at(const Value* args, uint32_t argc, uint32_t i) { return i < argc ? args[i] : Value(); }
Value str_value(Vm& vm, StringView text) { return Value::make_string(vm.new_string(text)); }

// What a RegExp object holds: the compiled expression and the flags it was made with.
struct Compiled {
  explicit Compiled(allocators::IAllocator& allocator) : regex(allocator), source(allocator), flags(allocator) {}
  regex::Regex regex;
  String source, flags;
  bool global = false, sticky = false;
};

Compiled* compiled_of(const Value& v) { return v.is_object() && v.as_object()->okind == ObjKind::RegExp ? static_cast<Compiled*>(v.as_object()->host_data) : nullptr; }

bool valid_flags(StringView flags) {
  bool seen[128] = {};
  for (uint64_t i = 0; i < flags.size(); ++i) {
    const char c = flags[i];
    if (c != 'd' && c != 'g' && c != 'i' && c != 'm' && c != 's' && c != 'u' && c != 'v' && c != 'y') return false;
    if (seen[static_cast<uint8_t>(c)]) return false;
    seen[static_cast<uint8_t>(c)] = true;
  }
  return true;
}

// The source as it shows in /.../ : an empty pattern is (?:), a slash outside a class is escaped, a line break is written out.
void escaped_source(StringView source, String* out) {
  if (source.empty()) {
    out->append("(?:)");
    return;
  }
  bool in_class = false;
  for (uint64_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '\\' && i + 1 < source.size()) {
      out->push_back(c);
      out->push_back(source[++i]);
      continue;
    }
    if (c == '[') in_class = true;
    else if (c == ']') in_class = false;
    if (c == '/' && !in_class) out->append("\\/");
    else if (c == '\n') out->append("\\n");
    else if (c == '\r') out->append("\\r");
    else out->push_back(c);
  }
}

bool make_regexp(Vm& vm, StringView source, StringView flags, Value* out) {
  if (!valid_flags(flags)) {
    String message(vm.allocator());
    message.append("Invalid flags supplied to RegExp constructor '");
    message.append(flags);
    message.append("'");
    return vm.throw_error(ErrorKind::SyntaxError, message.view());
  }
  void* mem = vm.allocator().allocate(sizeof(Compiled));
  if (!mem) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  Compiled* c = new (mem) Compiled(vm.allocator());
  regex::Flags f = regex::Flags::Utf8;
  for (uint64_t i = 0; i < flags.size(); ++i) {
    switch (flags[i]) {
      case 'i': f = f | regex::Flags::IgnoreCase; break;
      case 'm': f = f | regex::Flags::Multiline; break;
      case 's': f = f | regex::Flags::DotAll; break;
      case 'u': case 'v': f = f | regex::Flags::Unicode; break;
      case 'g': c->global = true; break;
      case 'y': c->sticky = true; break;
      default: break;
    }
  }
  const regex::Error error = c->regex.compile(source, f);
  if (error != regex::Error::None) {
    String message(vm.allocator());
    message.append("Invalid regular expression: /");
    message.append(source);
    message.append("/: ");
    message.append(regex::error_text(error));
    regexp_release(vm.allocator(), c);
    return vm.throw_error(ErrorKind::SyntaxError, message.view());
  }
  c->source.assign(source);
  c->flags.assign(flags);
  Value proto;
  vm.get_named(Value::make_object(vm.global()), "__regexp_proto", &proto);
  JsObject* o = vm.new_object(proto.is_object() ? proto.as_object() : nullptr);
  o->okind = ObjKind::RegExp;
  o->host_data = c;
  String shown(vm.allocator());
  escaped_source(source, &shown);
  vm.define_named(o, "lastIndex", Value::make_number(0), kWritable);
  vm.define_named(o, "source", str_value(vm, shown.view()), 0);
  String canonical(vm.allocator());   // (the flags property lists the flags in a fixed order whatever order they were written in)
  for (const char* f = "dgimsuvy"; *f; ++f) {
    if (flags.contains(*f)) canonical.push_back(*f);
  }
  vm.define_named(o, "flags", str_value(vm, canonical.view()), 0);
  auto has = [&](char ch) { return flags.contains(ch); };
  vm.define_named(o, "global", Value::make_bool(has('g')), 0);
  vm.define_named(o, "ignoreCase", Value::make_bool(has('i')), 0);
  vm.define_named(o, "multiline", Value::make_bool(has('m')), 0);
  vm.define_named(o, "dotAll", Value::make_bool(has('s')), 0);
  vm.define_named(o, "unicode", Value::make_bool(has('u')), 0);
  vm.define_named(o, "sticky", Value::make_bool(has('y')), 0);
  vm.define_named(o, "hasIndices", Value::make_bool(has('d')), 0);
  *out = Value::make_object(o);
  return true;
}

// The position after the character at `at` (a UTF-8 sequence is one step).
uint64_t next_char(StringView s, uint64_t at) {
  if (at >= s.size()) return at + 1;
  ++at;
  while (at < s.size() && (static_cast<uint8_t>(s[at]) & 0xC0) == 0x80) ++at;
  return at;
}

bool limit_error(Vm& vm) { return vm.throw_error(ErrorKind::RangeError, "Regular expression is too complex (it ran out of steps)"); }

// One search from `from`. *found tells whether there was a match; false is returned only when an exception was thrown.
bool search_from(Vm& vm, Compiled* c, StringView s, uint64_t from, bool prefix_only, regex::Match* m, bool* found) {
  *found = false;
  if (from > s.size()) return true;
  const regex::Result r = prefix_only ? c->regex.match_prefix(s, m, from) : c->regex.search(s, m, from);
  if (r == regex::Result::Matched) {
    *found = true;
    return true;
  }
  if (r == regex::Result::NoMatch) return true;
  return limit_error(vm);
}

bool read_last_index(Vm& vm, JsObject* re, double* out) {
  Value v;
  if (!vm.get_named(Value::make_object(re), "lastIndex", &v)) return false;
  if (!vm.to_number(v, out)) return false;
  if (*out != *out || *out < 0) *out = 0;
  return true;
}

void write_last_index(Vm& vm, JsObject* re, double v) { vm.define_named(re, "lastIndex", Value::make_number(v), kWritable); }

// The groups object for the named groups of a match: a name used by several groups (in different alternatives) gets the one that took part.
Value named_groups(Vm& vm, const regex::Program& p, StringView subject, const regex::Match& m, bool indices, JsString* s) {
  JsObject* groups = vm.new_plain();
  for (int pass = 0; pass < 2; ++pass) {   // (first every name as undefined, in order, then the groups that matched)
    for (uint64_t i = 0; i < p.names.size(); ++i) {
      const regex::NameEntry& e = p.names[i];
      const StringView name(e.text, e.length);
      if (pass == 0) {
        vm.define_named(groups, name, Value());
      } else if (m.has(e.group)) {
        if (!indices) {
          vm.define_named(groups, name, str_value(vm, m.text(subject, e.group)));
        } else {
          JsObject* pair = vm.new_array(2);
          vm.array_push(pair, Value::make_number(static_cast<double>(vm.string_unit_of_byte(s, m.begins[e.group]))));
          vm.array_push(pair, Value::make_number(static_cast<double>(vm.string_unit_of_byte(s, m.ends[e.group]))));
          vm.define_named(groups, name, Value::make_object(pair));
        }
      }
    }
  }
  return Value::make_object(groups);
}

// The groups of a match as the array exec() returns: the texts, and index, input and groups (and indices with the d flag).
Value match_array(Vm& vm, Compiled* c, JsString* s, const regex::Match& m) {
  JsObject* a = vm.new_array(m.count);
  for (uint32_t g = 0; g < m.count; ++g) vm.array_push(a, m.has(g) ? str_value(vm, m.text(s->view(), g)) : Value());
  vm.define_named(a, "index", Value::make_number(static_cast<double>(vm.string_unit_of_byte(s, m.begin()))));
  vm.define_named(a, "input", Value::make_string(s));
  const regex::Program p = c->regex.program();
  if (p.names.size() == 0) vm.define_named(a, "groups", Value());
  else vm.define_named(a, "groups", named_groups(vm, p, s->view(), m, false, s));
  if (c->flags.view().contains('d')) {   // hasIndices: [start, end] of each group, in characters of the subject
    JsObject* indices = vm.new_array(m.count);
    for (uint32_t g = 0; g < m.count; ++g) {
      if (!m.has(g)) {
        vm.array_push(indices, Value());
        continue;
      }
      JsObject* pair = vm.new_array(2);
      vm.array_push(pair, Value::make_number(static_cast<double>(vm.string_unit_of_byte(s, m.begins[g]))));
      vm.array_push(pair, Value::make_number(static_cast<double>(vm.string_unit_of_byte(s, m.ends[g]))));
      vm.array_push(indices, Value::make_object(pair));
    }
    vm.define_named(indices, "groups", p.names.size() == 0 ? Value() : named_groups(vm, p, s->view(), m, true, s));
    vm.define_named(a, "indices", Value::make_object(indices));
  }
  return Value::make_object(a);
}

// RegExp.prototype.exec's search: honours lastIndex for global and sticky expressions.
bool exec_once(Vm& vm, JsObject* re, JsString* s, regex::Match* m, bool* found) {
  Compiled* c = static_cast<Compiled*>(re->host_data);
  const bool uses_last = c->global || c->sticky;
  double last = 0;
  if (uses_last && !read_last_index(vm, re, &last)) return false;
  if (last > static_cast<double>(vm.string_length(s))) {
    write_last_index(vm, re, 0);
    *found = false;
    return true;
  }
  if (!search_from(vm, c, s->view(), vm.string_byte_of_unit(s, static_cast<uint64_t>(last)), c->sticky, m, found)) return false;
  if (uses_last) write_last_index(vm, re, *found ? static_cast<double>(vm.string_unit_of_byte(s, m->end())) : 0);
  return true;
}

bool this_regexp(Vm& vm, const Value& self, JsObject** out) {
  if (!compiled_of(self)) return vm.throw_error(ErrorKind::TypeError, "Receiver is not a RegExp");
  *out = self.as_object();
  return true;
}

NATIVE(regexp_exec) {
  JsObject* re;
  JsString* s;
  if (!this_regexp(vm, self, &re) || !vm.to_string(arg_at(args, argc, 0), &s)) return false;
  regex::Match m;
  bool found;
  if (!exec_once(vm, re, s, &m, &found)) return false;
  *result = found ? match_array(vm, static_cast<Compiled*>(re->host_data), s, m) : Value::make_null();
  return true;
}

NATIVE(regexp_test) {
  JsObject* re;
  JsString* s;
  if (!this_regexp(vm, self, &re) || !vm.to_string(arg_at(args, argc, 0), &s)) return false;
  regex::Match m;
  bool found;
  if (!exec_once(vm, re, s, &m, &found)) return false;
  *result = Value::make_bool(found);
  return true;
}

NATIVE(regexp_to_string) {
  JsObject* re;
  if (!this_regexp(vm, self, &re)) return false;
  Compiled* c = static_cast<Compiled*>(re->host_data);
  String text(vm.allocator());
  text.push_back('/');
  escaped_source(c->source.view(), &text);
  text.push_back('/');
  text.append(c->flags.view());
  *result = str_value(vm, text.view());
  return true;
}

NATIVE(regexp_constructor) {
  Value pattern = arg_at(args, argc, 0);
  const Value flags_arg = arg_at(args, argc, 1);
  String source(vm.allocator()), flags(vm.allocator());
  if (Compiled* other = compiled_of(pattern)) {
    source.assign(other->source.view());
    flags.assign(other->flags.view());
  } else if (!pattern.is_undefined()) {
    JsString* s;
    if (!vm.to_string(pattern, &s)) return false;
    source.assign(s->view());
  }
  if (!flags_arg.is_undefined()) {
    JsString* f;
    if (!vm.to_string(flags_arg, &f)) return false;
    flags.assign(f->view());
  }
  return make_regexp(vm, source.view(), flags.view(), result);
}

// $-patterns of a replacement string.
void expand_template(StringView repl, StringView subject, Compiled* c, const regex::Match& m, String* out) {
  const regex::Program p = c->regex.program();
  for (uint64_t i = 0; i < repl.size(); ++i) {
    const char ch = repl[i];
    if (ch != '$' || i + 1 >= repl.size()) {
      out->push_back(ch);
      continue;
    }
    const char n = repl[i + 1];
    if (n == '$') {
      out->push_back('$');
      ++i;
    } else if (n == '&') {
      out->append(m.text(subject, 0));
      ++i;
    } else if (n == '`') {
      out->append(subject.first(m.begin()));
      ++i;
    } else if (n == '\'') {
      out->append(subject.substr(m.end()));
      ++i;
    } else if (n >= '0' && n <= '9') {
      uint32_t g = static_cast<uint32_t>(n - '0');
      uint64_t used = 1;
      if (i + 2 < repl.size() && repl[i + 2] >= '0' && repl[i + 2] <= '9') {
        const uint32_t two = g * 10 + static_cast<uint32_t>(repl[i + 2] - '0');
        if (two > 0 && two < m.count) {
          g = two;
          used = 2;
        }
      }
      if (g == 0 || g >= m.count) {
        out->push_back('$');
        continue;
      }
      out->append(m.text(subject, g));
      i += used;
    } else if (n == '<' && p.names.size() > 0) {
      const uint64_t close = repl.find('>', i + 2);
      int32_t g = close == StringView::npos ? -1 : p.group_index(repl.substr(i + 2, close - i - 2));
      if (g >= 0) {   // (a name used by several groups: the one that took part)
        const StringView wanted = repl.substr(i + 2, close - i - 2);
        for (uint64_t k = 0; k < p.names.size(); ++k) {
          const regex::NameEntry& e = p.names[k];
          if (StringView(e.text, e.length).equals(wanted) && m.has(e.group)) g = static_cast<int32_t>(e.group);
        }
      }
      if (close == StringView::npos) {
        out->push_back('$');
        continue;
      }
      if (g >= 0) out->append(m.text(subject, static_cast<uint32_t>(g)));
      i = close;
    } else {
      out->push_back('$');
    }
  }
}

}  // namespace

void regexp_release(allocators::IAllocator& allocator, void* compiled) {
  Compiled* c = static_cast<Compiled*>(compiled);
  c->~Compiled();
  allocator.deallocate(c);
}

bool regexp_is(const Value& v) { return compiled_of(v) != nullptr; }

bool regexp_from_argument(Vm& vm, const Value& argument, const char* flags, Value* out) {
  if (regexp_is(argument)) {
    *out = argument;
    return true;
  }
  String source(vm.allocator());
  if (!argument.is_undefined()) {
    JsString* s;
    if (!vm.to_string(argument, &s)) return false;
    source.assign(s->view());
  }
  return make_regexp(vm, source.view(), StringView(flags), out);
}

bool regexp_match(Vm& vm, JsString* s, const Value& regexp, Value* result) {
  JsObject* re = regexp.as_object();
  Compiled* c = static_cast<Compiled*>(re->host_data);
  regex::Match m;
  bool found;
  if (!c->global) {
    if (!exec_once(vm, re, s, &m, &found)) return false;
    *result = found ? match_array(vm, c, s, m) : Value::make_null();
    return true;
  }
  write_last_index(vm, re, 0);
  JsObject* all = vm.new_array();
  uint64_t at = 0;
  while (at <= s->length) {
    if (!search_from(vm, c, s->view(), at, c->sticky, &m, &found)) return false;
    if (!found) break;
    vm.array_push(all, str_value(vm, m.text(s->view(), 0)));
    at = m.end() > m.begin() ? m.end() : next_char(s->view(), m.end());
  }
  *result = all->length == 0 ? Value::make_null() : Value::make_object(all);
  return true;
}

bool regexp_match_all(Vm& vm, JsString* s, const Value& regexp, Value* result) {
  JsObject* re = regexp.as_object();
  Compiled* c = static_cast<Compiled*>(re->host_data);
  if (!c->global) return vm.throw_error(ErrorKind::TypeError, "String.prototype.matchAll called with a non-global RegExp argument");
  JsObject* all = vm.new_array();
  regex::Match m;
  bool found;
  uint64_t at = 0;
  while (at <= s->length) {
    if (!search_from(vm, c, s->view(), at, c->sticky, &m, &found)) return false;
    if (!found) break;
    vm.array_push(all, match_array(vm, c, s, m));
    at = m.end() > m.begin() ? m.end() : next_char(s->view(), m.end());
  }
  Value factory;
  if (!vm.get_named(Value::make_object(all), "values", &factory) || !Vm::is_callable(factory)) {
    *result = Value::make_object(all);
    return true;
  }
  return vm.call(factory, Value::make_object(all), nullptr, 0, result);
}

bool regexp_search(Vm& vm, JsString* s, const Value& regexp, Value* result) {
  Compiled* c = static_cast<Compiled*>(regexp.as_object()->host_data);
  regex::Match m;
  bool found;
  if (!search_from(vm, c, s->view(), 0, c->sticky, &m, &found)) return false;
  *result = Value::make_number(found ? static_cast<double>(vm.string_unit_of_byte(s, m.begin())) : -1.0);
  return true;
}

bool regexp_replace(Vm& vm, JsString* s, const Value& regexp, const Value& replacement, bool replace_all, Value* result) {
  JsObject* re = regexp.as_object();
  Compiled* c = static_cast<Compiled*>(re->host_data);
  if (replace_all && !c->global) return vm.throw_error(ErrorKind::TypeError, "replaceAll must be called with a global RegExp");
  const bool function = Vm::is_callable(replacement);
  JsString* fixed = nullptr;
  if (!function && !vm.to_string(replacement, &fixed)) return false;
  const regex::Program p = c->regex.program();
  String out(vm.allocator());
  const StringView subject = s->view();
  uint64_t copied = 0, at = 0;
  if (c->global) write_last_index(vm, re, 0);
  while (at <= subject.size()) {
    regex::Match m;
    bool found;
    if (!search_from(vm, c, subject, at, c->sticky, &m, &found)) return false;
    if (!found) break;
    out.append(subject.substr(copied, m.begin() - copied));
    if (function) {
      List<Value> call_args(vm.allocator());
      for (uint32_t g = 0; g < m.count; ++g) call_args.push_back(m.has(g) ? str_value(vm, m.text(subject, g)) : Value());
      call_args.push_back(Value::make_number(static_cast<double>(vm.string_unit_of_byte(s, m.begin()))));
      call_args.push_back(Value::make_string(s));
      if (p.names.size() > 0) {
        call_args.push_back(named_groups(vm, p, subject, m, false, s));
      }
      Value r;
      JsString* rs;
      if (!vm.call(replacement, Value(), &call_args[0], static_cast<uint32_t>(call_args.count()), &r) || !vm.to_string(r, &rs)) return false;
      out.append(rs->view());
    } else {
      expand_template(fixed->view(), subject, c, m, &out);
    }
    copied = m.end();
    if (!c->global) break;
    at = m.end() > m.begin() ? m.end() : next_char(subject, m.end());
  }
  if (copied < subject.size()) out.append(subject.substr(copied));
  *result = str_value(vm, out.view());
  return true;
}

bool regexp_split(Vm& vm, JsString* s, const Value& regexp, int64_t limit, Value* result) {
  Compiled* c = static_cast<Compiled*>(regexp.as_object()->host_data);
  const StringView subject = s->view();
  JsObject* out = vm.new_array();
  *result = Value::make_object(out);
  if (limit <= 0) return true;
  regex::Match m;
  bool found;
  if (subject.empty()) {
    if (!search_from(vm, c, subject, 0, true, &m, &found)) return false;
    if (!found) vm.array_push(out, Value::make_string(s));
    return true;
  }
  uint64_t piece = 0, q = 0;
  while (q < subject.size()) {
    if (!search_from(vm, c, subject, q, false, &m, &found)) return false;
    if (!found || m.begin() >= subject.size()) break;
    if (m.end() == piece || (m.end() == m.begin() && m.begin() == piece)) {   // an empty match where the piece starts: one character further
      q = next_char(subject, m.begin() > q ? m.begin() : q);
      continue;
    }
    vm.array_push(out, str_value(vm, subject.substr(piece, m.begin() - piece)));
    if (static_cast<int64_t>(out->length) >= limit) return true;
    for (uint32_t g = 1; g < m.count; ++g) {
      vm.array_push(out, m.has(g) ? str_value(vm, m.text(subject, g)) : Value());
      if (static_cast<int64_t>(out->length) >= limit) return true;
    }
    piece = m.end();
    q = m.end() > m.begin() ? m.end() : next_char(subject, m.end());
  }
  vm.array_push(out, str_value(vm, subject.substr(piece)));
  return true;
}

bool install_regexp(Vm& vm) {
  JsObject* proto = vm.new_plain();
  vm.define_native(proto, "exec", regexp_exec, 1);
  vm.define_native(proto, "test", regexp_test, 1);
  vm.define_native(proto, "toString", regexp_to_string, 0);
  JsObject* ctor = vm.new_native(regexp_constructor, "RegExp", 2);
  vm.define(ctor, vm.prototype_name(), Value::make_object(proto), 0);
  vm.define(proto, vm.intern("constructor"), Value::make_object(ctor), kWritable);
  vm.define_named(vm.global(), "RegExp", Value::make_object(ctor), kWritable);
  vm.define_named(vm.global(), "__regexp_proto", Value::make_object(proto), 0);
  return true;
}

}  // namespace script
