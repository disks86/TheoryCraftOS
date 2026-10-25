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

// The GLSL ES 1.00 and 3.00 front end: a small preprocessor, a lexer and a recursive-descent parser that builds a shader::Module.
#include "WebGl/ShaderInternal.hpp"

namespace shader {

namespace {

// --- Preprocessor ----------------------------------------------------------------------------------------------------------------------------------------

struct Macro {
  explicit Macro(allocators::IAllocator& a) : name(a), body(a), params(a) {}
  String name, body;
  bool function = false;
  List<UniquePointer<String>> params;
};

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

class Preprocessor {
 public:
  Preprocessor(allocators::IAllocator& a, String* error) : alloc_(a), macros_(a), cond_(a), error_(error) {}

  bool run(StringView src, String* out, uint32_t* version) {
    define_simple("GL_ES", "1");
    define_simple("GL_FRAGMENT_PRECISION_HIGH", "1");
    define_simple("GL_OES_standard_derivatives", "1");
    define_simple("GL_EXT_shader_texture_lod", "1");
    define_simple("GL_EXT_frag_depth", "1");
    define_simple("GL_EXT_draw_buffers", "1");
    // strip comments and join continued lines
    String clean(alloc_);
    for (uint64_t i = 0; i < src.size();) {
      const char c = src[i];
      if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
        while (i < src.size() && src[i] != '\n') ++i;
      } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
        i += 2;
        while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
          if (src[i] == '\n') clean.push_back('\n');
          ++i;
        }
        i += 2;
        clean.push_back(' ');
      } else if (c == '\\' && i + 1 < src.size() && (src[i + 1] == '\n' || (src[i + 1] == '\r' && i + 2 < src.size() && src[i + 2] == '\n'))) {
        i += src[i + 1] == '\r' ? 3 : 2;
      } else {
        clean.push_back(c);
        ++i;
      }
    }
    uint32_t lineno = 0;
    StringView rest = clean.view();
    while (!rest.empty() || lineno == 0) {
      ++lineno;
      const uint64_t nl = rest.find('\n');
      StringView line = nl == StringView::npos ? rest : rest.substr(0, nl);
      rest = nl == StringView::npos ? StringView() : rest.substr(nl + 1);
      if (!handle_line(line, out, version)) return false;
      out->push_back('\n');
      if (nl == StringView::npos) break;
    }
    if (cond_.count() != 0) return fail("unterminated #if");
    return true;
  }

 private:
  struct Cond { bool active; bool taken; bool parent; };

  bool fail(const char* why) {
    if (error_->empty()) error_->assign(why);
    return false;
  }

  bool active() const { return cond_.count() == 0 || cond_[cond_.count() - 1].active; }

  void define_simple(const char* name, const char* body) {
    UniquePointer<Macro> m = UniquePointer<Macro>::make(alloc_, alloc_);
    if (!m) return;
    m->name.assign(name);
    m->body.assign(body);
    macros_.push_back(static_cast<UniquePointer<Macro>&&>(m));
  }

  Macro* find(StringView name) {
    for (uint64_t i = 0; i < macros_.count(); ++i)
      if (macros_[i]->name.view().equals(name)) return macros_[i].get();
    return nullptr;
  }

  void undef(StringView name) {
    for (uint64_t i = 0; i < macros_.count(); ++i)
      if (macros_[i]->name.view().equals(name)) {
        macros_.remove_at(i);
        return;
      }
  }

  static StringView trim(StringView s) {
    uint64_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
  }

  bool handle_line(StringView line, String* out, uint32_t* version) {
    StringView t = trim(line);
    if (t.empty() || t[0] != '#') {
      if (!active()) return true;
      return expand(line, out, 0);
    }
    t = trim(t.substr(1));
    uint64_t e = 0;
    while (e < t.size() && is_ident(t[e])) ++e;
    const StringView dir = t.substr(0, e);
    const StringView arg = trim(t.substr(e));
    if (dir.equals("ifdef") || dir.equals("ifndef")) {
      const bool def = find(first_word(arg)) != nullptr;
      const bool cond = dir.equals("ifdef") ? def : !def;
      const bool par = active();
      cond_.push_back(Cond{par && cond, cond, par});
      return true;
    }
    if (dir.equals("if")) {
      const bool par = active();
      bool v = false;
      if (par && !eval_if(arg, &v)) return false;
      cond_.push_back(Cond{par && v, v, par});
      return true;
    }
    if (dir.equals("elif")) {
      if (cond_.count() == 0) return fail("#elif without #if");
      Cond& c = cond_[cond_.count() - 1];
      if (c.taken || !c.parent) {
        c.active = false;
      } else {
        bool v = false;
        if (!eval_if(arg, &v)) return false;
        c.active = v;
        c.taken = v;
      }
      return true;
    }
    if (dir.equals("else")) {
      if (cond_.count() == 0) return fail("#else without #if");
      Cond& c = cond_[cond_.count() - 1];
      c.active = c.parent && !c.taken;
      c.taken = true;
      return true;
    }
    if (dir.equals("endif")) {
      if (cond_.count() == 0) return fail("#endif without #if");
      cond_.remove_at(cond_.count() - 1);
      return true;
    }
    if (!active()) return true;
    if (dir.equals("define")) return define(arg);
    if (dir.equals("undef")) {
      undef(first_word(arg));
      return true;
    }
    if (dir.equals("version")) {
      uint32_t v = 0;
      for (uint64_t i = 0; i < arg.size() && is_digit(arg[i]); ++i) v = v * 10 + static_cast<uint32_t>(arg[i] - '0');
      *version = v;
      undef("__VERSION__");
      define_simple("__VERSION__", v == 300 ? "300" : "100");
      return true;
    }
    if (dir.equals("error")) {
      error_->assign("#error ");
      error_->append(arg);
      return false;
    }
    return true;   // extension, pragma, line
  }

  static StringView first_word(StringView s) {
    uint64_t e = 0;
    while (e < s.size() && is_ident(s[e])) ++e;
    return s.substr(0, e);
  }

  bool define(StringView arg) {
    const StringView name = first_word(arg);
    if (name.empty()) return fail("bad #define");
    UniquePointer<Macro> m = UniquePointer<Macro>::make(alloc_, alloc_);
    if (!m) return fail("out of memory");
    m->name.assign(name);
    uint64_t i = name.size();
    if (i < arg.size() && arg[i] == '(') {
      m->function = true;
      ++i;
      while (i < arg.size() && arg[i] != ')') {
        while (i < arg.size() && (is_space(arg[i]) || arg[i] == ',')) ++i;
        uint64_t s = i;
        while (i < arg.size() && is_ident(arg[i])) ++i;
        if (i > s) {
          UniquePointer<String> p = UniquePointer<String>::make(alloc_, alloc_);
          if (!p) return fail("out of memory");
          p->assign(arg.substr(s, i - s));
          m->params.push_back(static_cast<UniquePointer<String>&&>(p));
        } else if (i < arg.size() && arg[i] != ')') {
          return fail("bad macro parameter");
        }
      }
      ++i;
    }
    m->body.assign(trim(arg.substr(i < arg.size() ? i : arg.size())));
    undef(name);
    macros_.push_back(static_cast<UniquePointer<Macro>&&>(m));
    return true;
  }

  // Replaces the macros in `text` and appends the result.
  bool expand(StringView text, String* out, uint32_t depth) {
    if (depth > 16) return fail("macro expansion too deep");
    for (uint64_t i = 0; i < text.size();) {
      const char c = text[i];
      if (is_ident_start(c)) {
        uint64_t j = i;
        while (j < text.size() && is_ident(text[j])) ++j;
        const StringView word = text.substr(i, j - i);
        Macro* mac = find(word);
        if (!mac) {
          out->append(word);
          i = j;
          continue;
        }
        if (!mac->function) {
          if (!expand(mac->body.view(), out, depth + 1)) return false;
          i = j;
          continue;
        }
        uint64_t k = j;
        while (k < text.size() && is_space(text[k])) ++k;
        if (k >= text.size() || text[k] != '(') {
          out->append(word);
          i = j;
          continue;
        }
        // gather the arguments
        List<UniquePointer<String>> args(alloc_);
        ++k;
        int level = 0;
        uint64_t s = k;
        for (; k < text.size(); ++k) {
          const char ch = text[k];
          if (ch == '(') ++level;
          else if (ch == ')') {
            if (level == 0) break;
            --level;
          } else if (ch == ',' && level == 0) {
            UniquePointer<String> a = UniquePointer<String>::make(alloc_, alloc_);
            if (!a) return fail("out of memory");
            a->assign(trim(text.substr(s, k - s)));
            args.push_back(static_cast<UniquePointer<String>&&>(a));
            s = k + 1;
          }
        }
        if (k >= text.size()) return fail("unterminated macro call");
        {
          UniquePointer<String> a = UniquePointer<String>::make(alloc_, alloc_);
          if (!a) return fail("out of memory");
          a->assign(trim(text.substr(s, k - s)));
          if (!(args.count() == 0 && a->empty() && mac->params.count() == 0)) args.push_back(static_cast<UniquePointer<String>&&>(a));
        }
        // substitute
        String body(alloc_);
        const StringView b = mac->body.view();
        for (uint64_t p = 0; p < b.size();) {
          if (is_ident_start(b[p])) {
            uint64_t q = p;
            while (q < b.size() && is_ident(b[q])) ++q;
            const StringView id = b.substr(p, q - p);
            bool replaced = false;
            for (uint64_t pi = 0; pi < mac->params.count(); ++pi)
              if (mac->params[pi]->view().equals(id)) {
                if (pi < args.count()) body.append(args[pi]->view());
                replaced = true;
                break;
              }
            if (!replaced) body.append(id);
            p = q;
          } else {
            body.push_back(b[p++]);
          }
        }
        if (!expand(body.view(), out, depth + 1)) return false;
        i = k + 1;
        continue;
      }
      if (is_digit(c)) {   // keep numbers (and their suffixes) whole
        uint64_t j = i;
        while (j < text.size() && (is_ident(text[j]) || text[j] == '.')) ++j;
        out->append(text.substr(i, j - i));
        i = j;
        continue;
      }
      out->push_back(c);
      ++i;
    }
    return true;
  }

  // #if expressions
  struct Cur {
    StringView s;
    uint64_t i = 0;
  };

  bool eval_if(StringView arg, bool* result) {
    // replace defined(X) / defined X first, expand the rest, then evaluate
    String pre(alloc_);
    for (uint64_t i = 0; i < arg.size();) {
      if (is_ident_start(arg[i])) {
        uint64_t j = i;
        while (j < arg.size() && is_ident(arg[j])) ++j;
        if (arg.substr(i, j - i).equals("defined")) {
          while (j < arg.size() && is_space(arg[j])) ++j;
          bool paren = false;
          if (j < arg.size() && arg[j] == '(') {
            paren = true;
            ++j;
          }
          while (j < arg.size() && is_space(arg[j])) ++j;
          uint64_t s = j;
          while (j < arg.size() && is_ident(arg[j])) ++j;
          pre.append(find(arg.substr(s, j - s)) ? "1" : "0");
          if (paren) {
            while (j < arg.size() && arg[j] != ')') ++j;
            ++j;
          }
          i = j;
          continue;
        }
        pre.append(arg.substr(i, j - i));
        i = j;
        continue;
      }
      pre.push_back(arg[i++]);
    }
    String ex(alloc_);
    if (!expand(pre.view(), &ex, 0)) return false;
    Cur c;
    c.s = ex.view();
    int64_t v = 0;
    if (!parse_or(c, &v)) return fail("bad #if expression");
    *result = v != 0;
    return true;
  }

  static void skip(Cur& c) {
    while (c.i < c.s.size() && is_space(c.s[c.i])) ++c.i;
  }
  static bool eat(Cur& c, const char* op) {
    skip(c);
    const StringView o(op);
    if (c.s.size() - c.i >= o.size() && c.s.substr(c.i, o.size()).equals(o)) {
      c.i += o.size();
      return true;
    }
    return false;
  }
  bool parse_or(Cur& c, int64_t* v) {
    if (!parse_and(c, v)) return false;
    while (eat(c, "||")) {
      int64_t r;
      if (!parse_and(c, &r)) return false;
      *v = (*v || r);
    }
    return true;
  }
  bool parse_and(Cur& c, int64_t* v) {
    if (!parse_eq(c, v)) return false;
    while (eat(c, "&&")) {
      int64_t r;
      if (!parse_eq(c, &r)) return false;
      *v = (*v && r);
    }
    return true;
  }
  bool parse_eq(Cur& c, int64_t* v) {
    if (!parse_rel(c, v)) return false;
    for (;;) {
      int64_t r;
      if (eat(c, "==")) { if (!parse_rel(c, &r)) return false; *v = *v == r; }
      else if (eat(c, "!=")) { if (!parse_rel(c, &r)) return false; *v = *v != r; }
      else return true;
    }
  }
  bool parse_rel(Cur& c, int64_t* v) {
    if (!parse_add(c, v)) return false;
    for (;;) {
      int64_t r;
      if (eat(c, "<=")) { if (!parse_add(c, &r)) return false; *v = *v <= r; }
      else if (eat(c, ">=")) { if (!parse_add(c, &r)) return false; *v = *v >= r; }
      else if (eat(c, "<")) { if (!parse_add(c, &r)) return false; *v = *v < r; }
      else if (eat(c, ">")) { if (!parse_add(c, &r)) return false; *v = *v > r; }
      else return true;
    }
  }
  bool parse_add(Cur& c, int64_t* v) {
    if (!parse_mul(c, v)) return false;
    for (;;) {
      int64_t r;
      if (eat(c, "+")) { if (!parse_mul(c, &r)) return false; *v += r; }
      else if (eat(c, "-")) { if (!parse_mul(c, &r)) return false; *v -= r; }
      else return true;
    }
  }
  bool parse_mul(Cur& c, int64_t* v) {
    if (!parse_unary(c, v)) return false;
    for (;;) {
      int64_t r;
      if (eat(c, "*")) { if (!parse_unary(c, &r)) return false; *v *= r; }
      else if (eat(c, "/")) { if (!parse_unary(c, &r)) return false; *v = r ? *v / r : 0; }
      else if (eat(c, "%")) { if (!parse_unary(c, &r)) return false; *v = r ? *v % r : 0; }
      else return true;
    }
  }
  bool parse_unary(Cur& c, int64_t* v) {
    skip(c);
    if (eat(c, "!")) { if (!parse_unary(c, v)) return false; *v = !*v; return true; }
    if (eat(c, "-")) { if (!parse_unary(c, v)) return false; *v = -*v; return true; }
    if (eat(c, "+")) return parse_unary(c, v);
    if (eat(c, "(")) {
      if (!parse_or(c, v)) return false;
      return eat(c, ")");
    }
    if (c.i < c.s.size() && is_digit(c.s[c.i])) {
      int64_t n = 0;
      if (c.s[c.i] == '0' && c.i + 1 < c.s.size() && (c.s[c.i + 1] == 'x' || c.s[c.i + 1] == 'X')) {
        c.i += 2;
        while (c.i < c.s.size()) {
          const char ch = c.s[c.i];
          int d = is_digit(ch) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
          if (d < 0) break;
          n = n * 16 + d;
          ++c.i;
        }
      } else {
        while (c.i < c.s.size() && is_digit(c.s[c.i])) n = n * 10 + (c.s[c.i++] - '0');
      }
      while (c.i < c.s.size() && (c.s[c.i] == 'u' || c.s[c.i] == 'U')) ++c.i;
      *v = n;
      return true;
    }
    if (c.i < c.s.size() && is_ident_start(c.s[c.i])) {   // an undefined name is 0
      while (c.i < c.s.size() && is_ident(c.s[c.i])) ++c.i;
      *v = 0;
      return true;
    }
    return false;
  }

  allocators::IAllocator& alloc_;
  List<UniquePointer<Macro>> macros_;
  List<Cond> cond_;
  String* error_;
};

// --- Lexer -----------------------------------------------------------------------------------------------------------------------------------------------

enum class Tok : uint8_t { End, Ident, Int, Uint, Float, Punct };

struct Token {
  Tok kind = Tok::End;
  StringView text;
  uint32_t line = 0;
  uint32_t ival = 0;
  float fval = 0;
};

float parse_float(StringView s) {
  double v = 0;
  uint64_t i = 0;
  while (i < s.size() && is_digit(s[i])) v = v * 10 + (s[i++] - '0');
  if (i < s.size() && s[i] == '.') {
    ++i;
    double scale = 0.1;
    while (i < s.size() && is_digit(s[i])) {
      v += (s[i++] - '0') * scale;
      scale *= 0.1;
    }
  }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    int e = 0;
    while (i < s.size() && is_digit(s[i])) e = e * 10 + (s[i++] - '0');
    double p = 1;
    for (int k = 0; k < e && k < 80; ++k) p *= 10;
    v = neg ? v / p : v * p;
  }
  return static_cast<float>(v);
}

bool lex(StringView src, List<Token>* out) {
  uint32_t line = 1;
  for (uint64_t i = 0; i < src.size();) {
    const char c = src[i];
    if (c == '\n') { ++line; ++i; continue; }
    if (is_space(c)) { ++i; continue; }
    Token t;
    t.line = line;
    if (is_ident_start(c)) {
      uint64_t j = i;
      while (j < src.size() && is_ident(src[j])) ++j;
      t.kind = Tok::Ident;
      t.text = src.substr(i, j - i);
      i = j;
    } else if (is_digit(c) || (c == '.' && i + 1 < src.size() && is_digit(src[i + 1]))) {
      uint64_t j = i;
      bool is_float = false;
      if (c == '0' && j + 1 < src.size() && (src[j + 1] == 'x' || src[j + 1] == 'X')) {
        j += 2;
        uint32_t v = 0;
        while (j < src.size()) {
          const char ch = src[j];
          int d = is_digit(ch) ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
          if (d < 0) break;
          v = v * 16 + static_cast<uint32_t>(d);
          ++j;
        }
        t.ival = v;
        t.kind = Tok::Int;
      } else {
        while (j < src.size() && is_digit(src[j])) ++j;
        if (j < src.size() && src[j] == '.') {
          is_float = true;
          ++j;
          while (j < src.size() && is_digit(src[j])) ++j;
        }
        if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
          uint64_t k = j + 1;
          if (k < src.size() && (src[k] == '+' || src[k] == '-')) ++k;
          if (k < src.size() && is_digit(src[k])) {
            is_float = true;
            j = k;
            while (j < src.size() && is_digit(src[j])) ++j;
          }
        }
        const StringView num = src.substr(i, j - i);
        if (is_float) {
          t.kind = Tok::Float;
          t.fval = parse_float(num);
          if (j < src.size() && (src[j] == 'f' || src[j] == 'F')) ++j;
        } else {
          uint32_t v = 0;
          const bool octal = num.size() > 1 && num[0] == '0';
          for (uint64_t k = 0; k < num.size(); ++k) v = v * (octal ? 8 : 10) + static_cast<uint32_t>(num[k] - '0');
          t.ival = v;
          t.kind = Tok::Int;
        }
      }
      if (j < src.size() && (src[j] == 'u' || src[j] == 'U') && t.kind == Tok::Int) {
        t.kind = Tok::Uint;
        ++j;
      }
      t.text = src.substr(i, j - i);
      i = j;
    } else {
      static const char* const kThree[] = {"<<=", ">>="};
      static const char* const kTwo[] = {"++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
      uint64_t len = 1;
      for (const char* op : kThree)
        if (src.size() - i >= 3 && src.substr(i, 3).equals(op)) len = 3;
      if (len == 1)
        for (const char* op : kTwo)
          if (src.size() - i >= 2 && src.substr(i, 2).equals(op)) len = 2;
      t.kind = Tok::Punct;
      t.text = src.substr(i, len);
      i += len;
    }
    if (!out->push_back(t)) return false;
  }
  Token e;
  e.kind = Tok::End;
  e.line = line;
  return out->push_back(e);
}

// --- Names ----------------------------------------------------------------------------------------------------------------------------------------------

struct TypeName {
  const char* name;
  Base base;
  uint8_t rows, cols;
  uint8_t dim;
  bool shadow;
  Base texel;
};

const TypeName kTypes[] = {
    {"void", Base::Void, 1, 1, 0, false, Base::Float},
    {"bool", Base::Bool, 1, 1, 0, false, Base::Float},
    {"int", Base::Int, 1, 1, 0, false, Base::Float},
    {"uint", Base::Uint, 1, 1, 0, false, Base::Float},
    {"float", Base::Float, 1, 1, 0, false, Base::Float},
    {"vec2", Base::Float, 2, 1, 0, false, Base::Float}, {"vec3", Base::Float, 3, 1, 0, false, Base::Float}, {"vec4", Base::Float, 4, 1, 0, false, Base::Float},
    {"ivec2", Base::Int, 2, 1, 0, false, Base::Float}, {"ivec3", Base::Int, 3, 1, 0, false, Base::Float}, {"ivec4", Base::Int, 4, 1, 0, false, Base::Float},
    {"uvec2", Base::Uint, 2, 1, 0, false, Base::Float}, {"uvec3", Base::Uint, 3, 1, 0, false, Base::Float}, {"uvec4", Base::Uint, 4, 1, 0, false, Base::Float},
    {"bvec2", Base::Bool, 2, 1, 0, false, Base::Float}, {"bvec3", Base::Bool, 3, 1, 0, false, Base::Float}, {"bvec4", Base::Bool, 4, 1, 0, false, Base::Float},
    {"mat2", Base::Float, 2, 2, 0, false, Base::Float}, {"mat3", Base::Float, 3, 3, 0, false, Base::Float}, {"mat4", Base::Float, 4, 4, 0, false, Base::Float},
    {"mat2x2", Base::Float, 2, 2, 0, false, Base::Float}, {"mat2x3", Base::Float, 3, 2, 0, false, Base::Float}, {"mat2x4", Base::Float, 4, 2, 0, false, Base::Float},
    {"mat3x2", Base::Float, 2, 3, 0, false, Base::Float}, {"mat3x3", Base::Float, 3, 3, 0, false, Base::Float}, {"mat3x4", Base::Float, 4, 3, 0, false, Base::Float},
    {"mat4x2", Base::Float, 2, 4, 0, false, Base::Float}, {"mat4x3", Base::Float, 3, 4, 0, false, Base::Float}, {"mat4x4", Base::Float, 4, 4, 0, false, Base::Float},
    {"sampler2D", Base::Sampler, 1, 1, kDim2D, false, Base::Float}, {"samplerCube", Base::Sampler, 1, 1, kDimCube, false, Base::Float},
    {"sampler3D", Base::Sampler, 1, 1, kDim3D, false, Base::Float}, {"sampler2DArray", Base::Sampler, 1, 1, kDim2DArray, false, Base::Float},
    {"sampler2DShadow", Base::Sampler, 1, 1, kDim2D, true, Base::Float}, {"samplerCubeShadow", Base::Sampler, 1, 1, kDimCube, true, Base::Float},
    {"sampler2DArrayShadow", Base::Sampler, 1, 1, kDim2DArray, true, Base::Float},
    {"isampler2D", Base::Sampler, 1, 1, kDim2D, false, Base::Int}, {"isamplerCube", Base::Sampler, 1, 1, kDimCube, false, Base::Int},
    {"isampler3D", Base::Sampler, 1, 1, kDim3D, false, Base::Int}, {"isampler2DArray", Base::Sampler, 1, 1, kDim2DArray, false, Base::Int},
    {"usampler2D", Base::Sampler, 1, 1, kDim2D, false, Base::Uint}, {"usamplerCube", Base::Sampler, 1, 1, kDimCube, false, Base::Uint},
    {"usampler3D", Base::Sampler, 1, 1, kDim3D, false, Base::Uint}, {"usampler2DArray", Base::Sampler, 1, 1, kDim2DArray, false, Base::Uint},
};

struct BuiltinName {
  const char* name;
  BuiltinFn fn;
};

const BuiltinName kBuiltins[] = {
    {"radians", BuiltinFn::Radians}, {"degrees", BuiltinFn::Degrees}, {"sin", BuiltinFn::Sin}, {"cos", BuiltinFn::Cos}, {"tan", BuiltinFn::Tan}, {"asin", BuiltinFn::Asin},
    {"acos", BuiltinFn::Acos}, {"atan", BuiltinFn::Atan}, {"sinh", BuiltinFn::Sinh}, {"cosh", BuiltinFn::Cosh}, {"tanh", BuiltinFn::Tanh}, {"asinh", BuiltinFn::Asinh},
    {"acosh", BuiltinFn::Acosh}, {"atanh", BuiltinFn::Atanh}, {"pow", BuiltinFn::Pow}, {"exp", BuiltinFn::Exp}, {"log", BuiltinFn::Log}, {"exp2", BuiltinFn::Exp2},
    {"log2", BuiltinFn::Log2}, {"sqrt", BuiltinFn::Sqrt}, {"inversesqrt", BuiltinFn::InverseSqrt}, {"abs", BuiltinFn::Abs}, {"sign", BuiltinFn::Sign},
    {"floor", BuiltinFn::Floor}, {"ceil", BuiltinFn::Ceil}, {"fract", BuiltinFn::Fract}, {"mod", BuiltinFn::Mod}, {"min", BuiltinFn::Min}, {"max", BuiltinFn::Max},
    {"clamp", BuiltinFn::Clamp}, {"mix", BuiltinFn::Mix}, {"step", BuiltinFn::Step}, {"smoothstep", BuiltinFn::Smoothstep}, {"length", BuiltinFn::Length},
    {"distance", BuiltinFn::Distance}, {"dot", BuiltinFn::Dot}, {"cross", BuiltinFn::Cross}, {"normalize", BuiltinFn::Normalize}, {"faceforward", BuiltinFn::FaceForward},
    {"reflect", BuiltinFn::Reflect}, {"refract", BuiltinFn::Refract}, {"matrixCompMult", BuiltinFn::MatrixCompMult}, {"outerProduct", BuiltinFn::OuterProduct},
    {"transpose", BuiltinFn::Transpose}, {"determinant", BuiltinFn::Determinant}, {"inverse", BuiltinFn::Inverse}, {"lessThan", BuiltinFn::LessThan},
    {"lessThanEqual", BuiltinFn::LessThanEqual}, {"greaterThan", BuiltinFn::GreaterThan}, {"greaterThanEqual", BuiltinFn::GreaterThanEqual}, {"equal", BuiltinFn::Equal},
    {"notEqual", BuiltinFn::NotEqual}, {"any", BuiltinFn::Any}, {"all", BuiltinFn::All}, {"not", BuiltinFn::Not}, {"isnan", BuiltinFn::IsNan}, {"isinf", BuiltinFn::IsInf},
    {"trunc", BuiltinFn::Trunc}, {"round", BuiltinFn::Round}, {"roundEven", BuiltinFn::RoundEven}, {"modf", BuiltinFn::Modf},
    {"floatBitsToInt", BuiltinFn::FloatBitsToInt}, {"floatBitsToUint", BuiltinFn::FloatBitsToUint}, {"intBitsToFloat", BuiltinFn::IntBitsToFloat},
    {"uintBitsToFloat", BuiltinFn::UintBitsToFloat}, {"packSnorm2x16", BuiltinFn::PackSnorm2x16}, {"packUnorm2x16", BuiltinFn::PackUnorm2x16},
    {"packHalf2x16", BuiltinFn::PackHalf2x16}, {"unpackSnorm2x16", BuiltinFn::UnpackSnorm2x16}, {"unpackUnorm2x16", BuiltinFn::UnpackUnorm2x16},
    {"unpackHalf2x16", BuiltinFn::UnpackHalf2x16}, {"packUnorm4x8", BuiltinFn::PackUnorm4x8}, {"packSnorm4x8", BuiltinFn::PackSnorm4x8},
    {"unpackUnorm4x8", BuiltinFn::UnpackUnorm4x8}, {"unpackSnorm4x8", BuiltinFn::UnpackSnorm4x8}, {"bitCount", BuiltinFn::BitCount}, {"findLSB", BuiltinFn::FindLsb},
    {"findMSB", BuiltinFn::FindMsb}, {"bitfieldExtract", BuiltinFn::BitfieldExtract}, {"bitfieldInsert", BuiltinFn::BitfieldInsert},
    {"bitfieldReverse", BuiltinFn::BitfieldReverse}, {"uaddCarry", BuiltinFn::UaddCarry}, {"usubBorrow", BuiltinFn::UsubBorrow},
    {"dFdx", BuiltinFn::Dfdx}, {"dFdy", BuiltinFn::Dfdy}, {"fwidth", BuiltinFn::Fwidth}, {"dFdxCoarse", BuiltinFn::Dfdx}, {"dFdyCoarse", BuiltinFn::Dfdy},
    {"dFdxFine", BuiltinFn::Dfdx}, {"dFdyFine", BuiltinFn::Dfdy}, {"fwidthCoarse", BuiltinFn::Fwidth}, {"fwidthFine", BuiltinFn::Fwidth},
};

struct TexName {
  const char* name;
  TexOp op;
  uint8_t form;   // 0: (s,P) +opt bias  1: (s,P,lod)  2: (s,P,dx,dy)  3: (s,P,off)+opt bias  4: (s,P,lod,off)  5: (s,P,dx,dy,off)  6: fetch(s,P,lod)  7: fetchOffset  8: size  9: gather  10: levels
};

const TexName kTex[] = {
    {"texture", TexOp::Sample, 0}, {"texture2D", TexOp::Sample, 0}, {"textureCube", TexOp::Sample, 0}, {"texture3D", TexOp::Sample, 0}, {"texture2DArray", TexOp::Sample, 0},
    {"textureProj", TexOp::Proj, 0}, {"texture2DProj", TexOp::Proj, 0}, {"texture3DProj", TexOp::Proj, 0},
    {"textureLod", TexOp::Lod, 1}, {"texture2DLod", TexOp::Lod, 1}, {"textureCubeLod", TexOp::Lod, 1}, {"texture3DLod", TexOp::Lod, 1},
    {"texture2DLodEXT", TexOp::Lod, 1}, {"textureCubeLodEXT", TexOp::Lod, 1}, {"textureProjLod", TexOp::ProjLod, 1}, {"texture2DProjLod", TexOp::ProjLod, 1},
    {"textureGrad", TexOp::Grad, 2}, {"texture2DGradEXT", TexOp::Grad, 2}, {"textureCubeGradEXT", TexOp::Grad, 2},
    {"textureOffset", TexOp::Sample, 3}, {"textureProjOffset", TexOp::Proj, 3}, {"textureLodOffset", TexOp::Lod, 4}, {"textureGradOffset", TexOp::Grad, 5},
    {"texelFetch", TexOp::Fetch, 6}, {"texelFetchOffset", TexOp::Fetch, 7}, {"textureSize", TexOp::Size, 8}, {"textureGather", TexOp::Gather, 9},
    {"textureQueryLevels", TexOp::Levels, 10},
};

// --- Parser ----------------------------------------------------------------------------------------------------------------------------------------------

struct Sym {
  explicit Sym(allocators::IAllocator& a) : name(a) {}
  String name;
  Type type;
  uint8_t kind = 0;    // 0 local, 1 global, 2 uniform block instance
  int32_t index = 0;
};

class Parser {
 public:
  Parser(Module& m, List<Token>& toks, Stage stage) : m_(m), a_(m.allocator), toks_(toks), stage_(stage), syms_(m.allocator), scopes_(m.allocator) {}

  bool run() {
    declare_builtin_vars();
    push_scope();
    while (cur().kind != Tok::End && !failed_) top_level();
    pop_scope();
    if (failed_) return false;
    m_.main_function = m_.find_function("main");
    if (m_.main_function < 0 || m_.functions[static_cast<uint64_t>(m_.main_function)]->body < 0) return err("no main function");
    m_.functions[static_cast<uint64_t>(m_.main_function)]->is_entry = true;
    m_.functions[static_cast<uint64_t>(m_.main_function)]->stage = stage_;
    return true;
  }

 private:
  Module& m_;
  allocators::IAllocator& a_;
  List<Token>& toks_;
  Stage stage_;
  List<UniquePointer<Sym>> syms_;
  List<uint64_t> scopes_;
  uint64_t pos_ = 0;
  bool failed_ = false;
  int32_t cur_fn_ = -1;
  uint32_t nlocals_ = 0;
  uint32_t anon_ = 0;

  // --- tokens
  const Token& cur() const { return toks_[pos_]; }
  const Token& peek(uint64_t n = 1) const { return toks_[pos_ + n < toks_.count() ? pos_ + n : toks_.count() - 1]; }
  bool is(const char* p) const { return cur().kind == Tok::Punct && cur().text.equals(p); }
  bool is_id(const char* w) const { return cur().kind == Tok::Ident && cur().text.equals(w); }
  bool accept(const char* p) {
    if (is(p)) { ++pos_; return true; }
    return false;
  }
  bool accept_id(const char* w) {
    if (is_id(w)) { ++pos_; return true; }
    return false;
  }
  bool expect(const char* p) {
    if (accept(p)) return true;
    String msg(a_);
    msg.assign("expected '");
    msg.append(p);
    msg.append("'");
    return err_at(msg.c_str());
  }
  bool err(const char* why) { return err_at(why); }
  bool err_at(const char* why) {
    if (!failed_) {
      failed_ = true;
      m_.error.assign("line ");
      m_.error.append_uint(cur().line);
      m_.error.append(": ");
      m_.error.append(why);
      if (cur().kind != Tok::End) {
        m_.error.append(" (at '");
        m_.error.append(cur().text);
        m_.error.append("')");
      }
    }
    return false;
  }

  // --- scopes
  void push_scope() { scopes_.push_back(syms_.count()); }
  void pop_scope() {
    const uint64_t at = scopes_[scopes_.count() - 1];
    scopes_.remove_at(scopes_.count() - 1);
    while (syms_.count() > at) syms_.remove_at(syms_.count() - 1);
  }
  Sym* add_sym(StringView name, const Type& t, uint8_t kind, int32_t index) {
    UniquePointer<Sym> s = UniquePointer<Sym>::make(a_, a_);
    if (!s) { err("out of memory"); return nullptr; }
    s->name.assign(name);
    s->type = t;
    s->kind = kind;
    s->index = index;
    Sym* raw = s.get();
    syms_.push_back(static_cast<UniquePointer<Sym>&&>(s));
    return raw;
  }
  Sym* find_sym(StringView name) {
    for (uint64_t i = syms_.count(); i > 0; --i)
      if (syms_[i - 1]->name.view().equals(name)) return syms_[i - 1].get();
    return nullptr;
  }

  // --- nodes
  int32_t add(const Node& n) {
    if (!m_.nodes.push_back(n)) { err("out of memory"); return -1; }
    return static_cast<int32_t>(m_.nodes.count() - 1);
  }
  int32_t add_kids(const int32_t* ids, uint32_t n) {
    const int32_t first = static_cast<int32_t>(m_.kids.count());
    for (uint32_t i = 0; i < n; ++i)
      if (!m_.kids.push_back(ids[i])) { err("out of memory"); return 0; }
    return first;
  }
  int32_t const_node(const Type& t, const uint32_t* vals, uint32_t n) {
    const int32_t off = static_cast<int32_t>(m_.consts.count());
    for (uint32_t i = 0; i < n; ++i) m_.consts.push_back(vals[i]);
    Node nd;
    nd.kind = NodeKind::Const;
    nd.a = off;
    nd.type = t;
    nd.line = cur().line;
    return add(nd);
  }
  int32_t int_node(int32_t v) { const uint32_t u = static_cast<uint32_t>(v); return const_node(make_type(Base::Int), &u, 1); }
  int32_t zero_node(const Type& t) {
    Node nd;
    nd.kind = NodeKind::Zero;
    nd.type = t;
    return add(nd);
  }
  int32_t name_chars(StringView s) {
    const int32_t off = static_cast<int32_t>(m_.consts.count());
    for (char c : s) m_.consts.push_back(static_cast<uint8_t>(c));
    return off;
  }

  bool fold_int(int32_t id, int64_t* v) {
    if (id < 0) return false;
    const Node& n = m_.nodes[static_cast<uint64_t>(id)];
    switch (n.kind) {
      case NodeKind::Const:
        if (n.type.base == Base::Int || n.type.base == Base::Uint) { *v = n.type.base == Base::Int ? u2i(m_.consts[static_cast<uint64_t>(n.a)]) : m_.consts[static_cast<uint64_t>(n.a)]; return true; }
        return false;
      case NodeKind::Global: {
        const Global& g = *m_.globals[static_cast<uint64_t>(n.ival)];
        return g.kind == GlobalKind::Const && g.init >= 0 && fold_int(g.init, v);
      }
      case NodeKind::Unary: {
        int64_t x;
        if (!fold_int(n.a, &x)) return false;
        *v = n.op == Op::Neg ? -x : x;
        return true;
      }
      case NodeKind::Binary: {
        int64_t x, y;
        if (!fold_int(n.a, &x) || !fold_int(n.b, &y)) return false;
        switch (n.op) {
          case Op::Add: *v = x + y; return true;
          case Op::Sub: *v = x - y; return true;
          case Op::Mul: *v = x * y; return true;
          case Op::Div: *v = y ? x / y : 0; return true;
          case Op::Mod: *v = y ? x % y : 0; return true;
          case Op::Shl: *v = x << (y & 31); return true;
          default: return false;
        }
      }
      default: return false;
    }
  }

  // --- types
  bool find_type_name(StringView w, Type* t) const {
    for (const TypeName& tn : kTypes)
      if (w.equals(tn.name)) {
        Type r = make_type(tn.base, tn.rows, tn.cols);
        r.dim = tn.dim;
        r.shadow = tn.shadow;
        r.texel = tn.texel;
        *t = r;
        return true;
      }
    for (uint64_t i = 0; i < m_.structs.count(); ++i)
      if (m_.structs[i]->name.view().equals(w)) {
        Type r = make_type(Base::Struct);
        r.sid = static_cast<uint16_t>(i);
        *t = r;
        return true;
      }
    return false;
  }
  bool at_type() const {
    if (cur().kind != Tok::Ident) return false;
    Type t;
    return find_type_name(cur().text, &t);
  }
  bool at_qualifier() const {
    if (cur().kind != Tok::Ident) return false;
    static const char* const q[] = {"const", "uniform", "attribute", "varying", "in", "out", "inout", "highp", "mediump", "lowp", "flat", "smooth", "centroid", "invariant", "layout", "precision", "shared", "readonly", "writeonly", "coherent", "volatile", "restrict"};
    for (const char* w : q)
      if (cur().text.equals(w)) return true;
    return false;
  }

  bool parse_array_suffix(Type* t) {
    while (accept("[")) {
      if (is("]")) {
        ++pos_;
        t->array = 0xFFFFFFFFu;   // unsized: the initializer says
        continue;
      }
      const int32_t e = expression();
      int64_t v;
      if (e < 0 || !fold_int(e, &v) || v <= 0) return err("array size must be a positive constant");
      if (!expect("]")) return false;
      t->array = static_cast<uint32_t>(v);
    }
    return true;
  }

  bool parse_type(Type* t) {
    if (cur().kind != Tok::Ident || !find_type_name(cur().text, t)) return err("expected a type");
    ++pos_;
    if (is("[")) return parse_array_suffix(t);
    return true;
  }

  // --- globals
  void declare_builtin_vars() {
    struct B { const char* name; Type type; GlobalKind kind; int32_t id; };
    const Type f1 = make_type(Base::Float), f4 = make_type(Base::Float, 4), f2 = make_type(Base::Float, 2), i1 = make_type(Base::Int), b1 = make_type(Base::Bool);
    Type frag_data = f4;
    frag_data.array = 4;
    if (stage_ == Stage::Vertex) {
      add_builtin("gl_Position", f4, GlobalKind::Out, kBvPosition);
      add_builtin("gl_PointSize", f1, GlobalKind::Out, kBvPointSize);
      add_builtin("gl_VertexID", i1, GlobalKind::In, kBvVertexIndex);
      add_builtin("gl_InstanceID", i1, GlobalKind::In, kBvInstanceIndex);
    } else {
      add_builtin("gl_FragCoord", f4, GlobalKind::In, kBvFragCoord);
      add_builtin("gl_FrontFacing", b1, GlobalKind::In, kBvFrontFacing);
      add_builtin("gl_PointCoord", f2, GlobalKind::In, kBvPointCoord);
      add_builtin("gl_FragColor", f4, GlobalKind::Out, kBvFragColor);
      add_builtin("gl_FragData", frag_data, GlobalKind::Out, kBvFragData);
      add_builtin("gl_FragDepth", f1, GlobalKind::Out, kBvFragDepth);
      add_builtin("gl_FragDepthEXT", f1, GlobalKind::Out, kBvFragDepth);
    }
    const struct { const char* name; int v; } consts[] = {{"gl_MaxVertexAttribs", 16}, {"gl_MaxDrawBuffers", 4}, {"gl_MaxVertexUniformVectors", 1024},
                                                          {"gl_MaxFragmentUniformVectors", 1024}, {"gl_MaxVaryingVectors", 30}, {"gl_MaxTextureImageUnits", 16}};
    for (const auto& c : consts) {
      const int32_t n = int_node(c.v);
      const int32_t g = new_global(c.name, i1, GlobalKind::Const, false);
      if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->init = n;
    }
  }
  void add_builtin(const char* name, const Type& t, GlobalKind kind, int32_t id) {
    const int32_t g = new_global(name, t, kind, false);
    if (g >= 0) {
      m_.globals[static_cast<uint64_t>(g)]->kind = GlobalKind::Builtin;
      m_.globals[static_cast<uint64_t>(g)]->builtin = id;
      m_.globals[static_cast<uint64_t>(g)]->is_read_only = kind == GlobalKind::In;
    }
  }

  int32_t new_global(StringView name, const Type& t, GlobalKind kind, bool shared) {
    UniquePointer<Global> g = UniquePointer<Global>::make(a_, a_);
    if (!g) { err("out of memory"); return -1; }
    g->name.assign(name);
    g->type = t;
    g->kind = kind;
    g->shared = shared;
    const uint32_t n = m_.slots_of(t);
    if (shared) { g->slot = m_.shared_slots; m_.shared_slots += n; }
    else { g->slot = m_.private_slots; m_.private_slots += n; }
    m_.globals.push_back(static_cast<UniquePointer<Global>&&>(g));
    const int32_t idx = static_cast<int32_t>(m_.globals.count() - 1);
    add_sym(name, t, 1, idx);
    return idx;
  }

  struct Quals {
    bool is_const = false, uniform = false, attribute = false, varying = false, in = false, out = false, inout = false, flat = false, layout_std140 = false;
    int32_t location = -1, binding = -1;
  };

  bool parse_layout(Quals* q) {
    if (!expect("(")) return false;
    while (!is(")") && !failed_) {
      if (cur().kind != Tok::Ident) return err("bad layout qualifier");
      const StringView key = cur().text;
      ++pos_;
      int32_t value = 0;
      if (accept("=")) {
        if (cur().kind != Tok::Int) return err("bad layout value");
        value = static_cast<int32_t>(cur().ival);
        ++pos_;
      }
      if (key.equals("location")) q->location = value;
      else if (key.equals("binding")) q->binding = value;
      else if (key.equals("std140")) q->layout_std140 = true;
      if (!accept(",")) break;
    }
    return expect(")");
  }

  bool parse_quals(Quals* q) {
    while (at_qualifier() && !is_id("precision")) {
      const StringView w = cur().text;
      ++pos_;
      if (w.equals("const")) q->is_const = true;
      else if (w.equals("uniform")) q->uniform = true;
      else if (w.equals("attribute")) q->attribute = true;
      else if (w.equals("varying")) q->varying = true;
      else if (w.equals("in")) q->in = true;
      else if (w.equals("out")) q->out = true;
      else if (w.equals("inout")) q->inout = true;
      else if (w.equals("flat")) q->flat = true;
      else if (w.equals("layout")) { if (!parse_layout(q)) return false; }
    }
    return !failed_;
  }

  void top_level() {
    if (accept(";")) return;
    if (is_id("precision")) {
      while (!is(";") && cur().kind != Tok::End) ++pos_;
      expect(";");
      return;
    }
    if (is_id("struct")) {
      struct_decl(nullptr);
      if (!failed_) {
        // a declaration like `struct S {...} s;` or just `struct S {...};`
        global_declarators_after_struct();
      }
      return;
    }
    Quals q;
    if (!parse_quals(&q)) return;
    // uniform block
    if ((q.uniform || q.in || q.out) && cur().kind == Tok::Ident && !at_type() && peek().kind == Tok::Punct && peek().text.equals("{")) {
      uniform_block(q);
      return;
    }
    Type t;
    if (is_id("struct")) {
      Type st;
      struct_decl(&st);
      t = st;
    } else if (!parse_type(&t)) {
      return;
    }
    if (cur().kind != Tok::Ident) {
      err("expected a name");
      return;
    }
    const Token nameTok = cur();
    if (peek().kind == Tok::Punct && peek().text.equals("(")) {
      function_decl(t);
      return;
    }
    // variables
    for (;;) {
      if (cur().kind != Tok::Ident) { err("expected a name"); return; }
      const StringView name = cur().text;
      ++pos_;
      Type vt = t;
      if (is("[") && !parse_array_suffix(&vt)) return;
      int32_t init = -1;
      if (accept("=")) {
        init = assignment();
        if (init < 0) return;
        if (vt.array == 0xFFFFFFFFu) vt.array = init_array_size(init);
      }
      declare_global_variable(name, vt, q, init);
      if (failed_) return;
      if (!accept(",")) break;
    }
    expect(";");
    (void)nameTok;
  }

  uint32_t init_array_size(int32_t init) {
    const Node& n = m_.nodes[static_cast<uint64_t>(init)];
    if (n.kind == NodeKind::Construct) return n.type.array ? n.type.array : static_cast<uint32_t>(n.count);
    return 1;
  }

  void declare_global_variable(StringView name, Type t, const Quals& q, int32_t init) {
    GlobalKind kind = GlobalKind::Private;
    bool shared = false;
    if (q.is_const && !q.uniform) kind = GlobalKind::Const;
    else if (q.uniform) { kind = GlobalKind::Uniform; shared = true; }
    else if (q.attribute) kind = GlobalKind::In;
    else if (q.varying) kind = stage_ == Stage::Vertex ? GlobalKind::Out : GlobalKind::In;
    else if (q.in) kind = GlobalKind::In;
    else if (q.out) kind = GlobalKind::Out;
    const int32_t g = new_global(name, t, kind, shared);
    if (g < 0) return;
    Global& gv = *m_.globals[static_cast<uint64_t>(g)];
    gv.init = init;
    gv.location = q.location;
    gv.binding = q.binding;
    gv.flat = q.flat;
    if (kind == GlobalKind::Uniform) gv.uniform_index = 0;
  }

  void global_declarators_after_struct() {
    // `struct S {...};` was parsed by struct_decl up to the closing brace
    if (is(";")) { ++pos_; return; }
    if (cur().kind == Tok::Ident) {
      Type t = make_type(Base::Struct);
      t.sid = static_cast<uint16_t>(m_.structs.count() - 1);
      Quals q;
      for (;;) {
        if (cur().kind != Tok::Ident) { err("expected a name"); return; }
        const StringView name = cur().text;
        ++pos_;
        Type vt = t;
        if (is("[") && !parse_array_suffix(&vt)) return;
        int32_t init = -1;
        if (accept("=")) init = assignment();
        declare_global_variable(name, vt, q, init);
        if (!accept(",")) break;
      }
    }
    expect(";");
  }

  bool struct_decl(Type* out) {
    ++pos_;   // struct
    UniquePointer<StructDef> sd = UniquePointer<StructDef>::make(a_, a_);
    if (!sd) return err("out of memory");
    if (cur().kind == Tok::Ident) {
      sd->name.assign(cur().text);
      ++pos_;
    } else {
      sd->name.assign("anon");
      sd->name.append_uint(anon_++);
    }
    if (!expect("{")) return false;
    uint32_t offset = 0;
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      Quals q;
      parse_quals(&q);
      Type ft;
      if (!parse_type(&ft)) return false;
      for (;;) {
        if (cur().kind != Tok::Ident) return err("expected a field name");
        Field f(a_);
        f.name.assign(cur().text);
        ++pos_;
        Type t = ft;
        if (is("[") && !parse_array_suffix(&t)) return false;
        f.type = t;
        f.offset = offset;
        offset += m_.slots_of(t);
        sd->fields.push_back(static_cast<Field&&>(f));
        if (!accept(",")) break;
      }
      if (!expect(";")) return false;
    }
    if (!expect("}")) return false;
    sd->slots = offset;
    m_.structs.push_back(static_cast<UniquePointer<StructDef>&&>(sd));
    if (out) {
      *out = make_type(Base::Struct);
      out->sid = static_cast<uint16_t>(m_.structs.count() - 1);
    }
    return true;
  }

  void uniform_block(const Quals& q) {
    UniquePointer<UniformBlock> blk = UniquePointer<UniformBlock>::make(a_, a_);
    if (!blk) { err("out of memory"); return; }
    blk->name.assign(cur().text);
    blk->binding = q.binding;
    ++pos_;
    if (!expect("{")) return;
    const int32_t bi = static_cast<int32_t>(m_.blocks.count());
    m_.blocks.push_back(static_cast<UniquePointer<UniformBlock>&&>(blk));
    const bool is_uniform = q.uniform;
    uint32_t offset = 0;
    List<int32_t> members(a_);
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      Quals mq;
      parse_quals(&mq);
      Type mt;
      if (!parse_type(&mt)) return;
      for (;;) {
        if (cur().kind != Tok::Ident) { err("expected a member name"); return; }
        const StringView name = cur().text;
        ++pos_;
        Type t = mt;
        if (is("[") && !parse_array_suffix(&t)) return;
        const int32_t g = new_global(name, t, is_uniform ? GlobalKind::Uniform : (q.in ? GlobalKind::In : GlobalKind::Out), is_uniform);
        if (g < 0) return;
        Global& gv = *m_.globals[static_cast<uint64_t>(g)];
        gv.block = bi;
        members.push_back(g);
        (void)offset;
        if (!accept(",")) break;
      }
      if (!expect(";")) return;
    }
    if (!expect("}")) return;
    if (cur().kind == Tok::Ident) {   // instance name
      m_.blocks[static_cast<uint64_t>(bi)]->instance.assign(cur().text);
      add_sym(cur().text, make_type(Base::Void), 2, bi);
      ++pos_;
    }
    expect(";");
  }

  // --- functions
  void function_decl(const Type& ret) {
    const StringView name = cur().text;
    ++pos_;
    expect("(");
    List<UniquePointer<Sym>> unused(a_);
    UniquePointer<Function> fn = UniquePointer<Function>::make(a_, a_);
    if (!fn) { err("out of memory"); return; }
    fn->name.assign(name);
    fn->ret = ret;
    List<UniquePointer<String>> pnames(a_);
    if (is_id("void") && peek().kind == Tok::Punct && peek().text.equals(")")) ++pos_;
    while (!is(")") && !failed_ && cur().kind != Tok::End) {
      Param p;
      for (;;) {
        if (accept_id("in")) p.dir = 0;
        else if (accept_id("out")) p.dir = 1;
        else if (accept_id("inout")) p.dir = 2;
        else if (accept_id("const") || accept_id("highp") || accept_id("mediump") || accept_id("lowp")) {}
        else break;
      }
      Type pt;
      if (!parse_type(&pt)) return;
      UniquePointer<String> pn = UniquePointer<String>::make(a_, a_);
      if (cur().kind == Tok::Ident) {
        pn->assign(cur().text);
        ++pos_;
        if (is("[") && !parse_array_suffix(&pt)) return;
      }
      p.type = pt;
      fn->params.push_back(p);
      pnames.push_back(static_cast<UniquePointer<String>&&>(pn));
      if (!accept(",")) break;
    }
    if (!expect(")")) return;
    // find a prototype with the same signature
    int32_t index = -1;
    for (uint64_t i = 0; i < m_.functions.count(); ++i) {
      const Function& f = *m_.functions[i];
      if (!f.name.view().equals(name) || f.params.count() != fn->params.count()) continue;
      bool same = true;
      for (uint64_t k = 0; k < f.params.count(); ++k) same = same && f.params[k].type == fn->params[k].type;
      if (same) { index = static_cast<int32_t>(i); break; }
    }
    if (index < 0) {
      m_.functions.push_back(static_cast<UniquePointer<Function>&&>(fn));
      index = static_cast<int32_t>(m_.functions.count() - 1);
    }
    if (accept(";")) return;   // a prototype
    if (!is("{")) { err("expected a function body"); return; }
    Function& F = *m_.functions[static_cast<uint64_t>(index)];
    cur_fn_ = index;
    nlocals_ = static_cast<uint32_t>(F.params.count());
    push_scope();
    for (uint64_t i = 0; i < pnames.count(); ++i)
      if (!pnames[i]->empty()) add_sym(pnames[i]->view(), F.params[i].type, 0, static_cast<int32_t>(i));
    const int32_t body = block_stmt(false);
    pop_scope();
    m_.functions[static_cast<uint64_t>(index)]->body = body;
    m_.functions[static_cast<uint64_t>(index)]->locals = nlocals_;
    cur_fn_ = -1;
  }

  // --- statements
  int32_t block_stmt(bool scope = true) {
    if (!expect("{")) return -1;
    if (scope) push_scope();
    List<int32_t> kids(a_);
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      const int32_t s = statement();
      if (s < 0) break;
      kids.push_back(s);
    }
    if (scope) pop_scope();
    if (failed_) return -1;
    if (!expect("}")) return -1;
    Node n;
    n.kind = NodeKind::Block;
    n.first = add_kids(kids.view().data(), static_cast<uint32_t>(kids.count()));
    n.count = static_cast<int32_t>(kids.count());
    return add(n);
  }

  int32_t statement() {
    if (failed_) return -1;
    const uint32_t line = cur().line;
    Node n;
    n.line = line;
    if (is("{")) return block_stmt();
    if (accept(";")) { n.kind = NodeKind::Empty; return add(n); }
    if (is_id("if")) {
      ++pos_;
      if (!expect("(")) return -1;
      n.kind = NodeKind::If;
      n.a = expression();
      if (!expect(")")) return -1;
      n.b = scoped_statement();
      if (accept_id("else")) n.c = scoped_statement();
      return failed_ ? -1 : add(n);
    }
    if (is_id("for")) {
      ++pos_;
      if (!expect("(")) return -1;
      push_scope();
      n.kind = NodeKind::For;
      if (is(";")) { ++pos_; }
      else if (starts_decl()) n.a = declaration_stmt(true);
      else {
        Node e;
        e.kind = NodeKind::ExprStmt;
        e.a = expression();
        if (!expect(";")) return -1;
        n.a = add(e);
      }
      if (!is(";")) n.b = expression();
      if (!expect(";")) return -1;
      if (!is(")")) n.c = expression();
      if (!expect(")")) return -1;
      n.d = scoped_statement();
      pop_scope();
      return failed_ ? -1 : add(n);
    }
    if (is_id("while")) {
      ++pos_;
      if (!expect("(")) return -1;
      push_scope();
      n.kind = NodeKind::While;
      if (starts_decl()) {   // while (int i = f()) is rare; treat as an expression only
        err("declaration in while condition is not supported");
        return -1;
      }
      n.a = expression();
      pop_scope();
      if (!expect(")")) return -1;
      n.b = scoped_statement();
      return failed_ ? -1 : add(n);
    }
    if (is_id("do")) {
      ++pos_;
      n.kind = NodeKind::DoWhile;
      n.a = scoped_statement();
      if (!accept_id("while") || !expect("(")) return err("expected while"), -1;
      n.b = expression();
      if (!expect(")") || !expect(";")) return -1;
      return failed_ ? -1 : add(n);
    }
    if (is_id("switch")) return switch_stmt();
    if (is_id("return")) {
      ++pos_;
      n.kind = NodeKind::Return;
      if (!is(";")) n.a = expression();
      if (!expect(";")) return -1;
      return failed_ ? -1 : add(n);
    }
    if (is_id("break")) { ++pos_; n.kind = NodeKind::Break; return expect(";") ? add(n) : -1; }
    if (is_id("continue")) { ++pos_; n.kind = NodeKind::Continue; return expect(";") ? add(n) : -1; }
    if (is_id("discard")) { ++pos_; n.kind = NodeKind::Discard; m_.uses_discard = true; return expect(";") ? add(n) : -1; }
    if (starts_decl()) return declaration_stmt(false);
    n.kind = NodeKind::ExprStmt;
    n.a = expression();
    if (!expect(";")) return -1;
    return failed_ ? -1 : add(n);
  }

  int32_t scoped_statement() {
    push_scope();
    const int32_t s = statement();
    pop_scope();
    return s;
  }

  bool starts_decl() {
    if (is_id("const") || is_id("highp") || is_id("mediump") || is_id("lowp") || is_id("precision")) return true;
    if (!at_type()) return false;
    Type t;
    find_type_name(cur().text, &t);
    // `float(1.0)` is a constructor expression; `float x` or `float[3] x` is a declaration
    if (peek().kind == Tok::Ident) return true;
    if (peek().kind == Tok::Punct && peek().text.equals("[")) {
      uint64_t k = 1;
      while (peek(k).kind != Tok::End && !(peek(k).kind == Tok::Punct && peek(k).text.equals("]"))) ++k;
      return peek(k + 1).kind == Tok::Ident;
    }
    return false;
  }

  int32_t declaration_stmt(bool for_init) {
    while (is_id("const") || is_id("highp") || is_id("mediump") || is_id("lowp")) ++pos_;
    if (is_id("precision")) {
      while (!is(";") && cur().kind != Tok::End) ++pos_;
      expect(";");
      Node e;
      e.kind = NodeKind::Empty;
      return add(e);
    }
    Type base;
    if (!parse_type(&base)) return -1;
    List<int32_t> decls(a_);
    for (;;) {
      if (cur().kind != Tok::Ident) return err("expected a variable name"), -1;
      const StringView name = cur().text;
      ++pos_;
      Type t = base;
      if (is("[") && !parse_array_suffix(&t)) return -1;
      int32_t init = -1;
      if (accept("=")) {
        init = assignment();
        if (init < 0) return -1;
        if (t.array == 0xFFFFFFFFu) t.array = init_array_size(init);
      }
      if (t.array == 0xFFFFFFFFu) return err("unsized array without an initializer"), -1;
      Node d;
      d.kind = NodeKind::Decl;
      d.type = t;
      d.a = init;
      d.ival = static_cast<int32_t>(nlocals_);
      d.line = cur().line;
      add_sym(name, t, 0, static_cast<int32_t>(nlocals_));
      ++nlocals_;
      decls.push_back(add(d));
      if (!accept(",")) break;
    }
    if (!expect(";")) return -1;
    (void)for_init;
    if (decls.count() == 1) return decls[0];
    Node b;
    b.kind = NodeKind::Block;
    b.flags = 1;   // no scope of its own: the variables live in the enclosing block
    b.first = add_kids(decls.view().data(), static_cast<uint32_t>(decls.count()));
    b.count = static_cast<int32_t>(decls.count());
    return add(b);
  }

  int32_t switch_stmt() {
    ++pos_;
    Node n;
    n.kind = NodeKind::Switch;
    n.flags = 1;   // GLSL falls through
    if (!expect("(")) return -1;
    n.a = expression();
    if (!expect(")") || !expect("{")) return -1;
    List<int32_t> cases(a_);
    push_scope();
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      Node c;
      c.kind = NodeKind::Case;
      if (accept_id("default")) {
        c.ival = 1;
        if (!expect(":")) return -1;
      } else if (accept_id("case")) {
        const int32_t v = expression();
        if (v < 0 || !expect(":")) return -1;
        c.first = add_kids(&v, 1);
        c.count = 1;
      } else {
        return err("expected case or default"), -1;
      }
      List<int32_t> body(a_);
      while (!is_id("case") && !is_id("default") && !is("}") && !failed_ && cur().kind != Tok::End) {
        const int32_t s = statement();
        if (s < 0) return -1;
        body.push_back(s);
      }
      if (body.count() > 0) {
        Node b;
        b.kind = NodeKind::Block;
        b.flags = 1;
        b.first = add_kids(body.view().data(), static_cast<uint32_t>(body.count()));
        b.count = static_cast<int32_t>(body.count());
        c.d = add(b);
      }
      cases.push_back(add(c));
    }
    pop_scope();
    if (!expect("}")) return -1;
    n.first = add_kids(cases.view().data(), static_cast<uint32_t>(cases.count()));
    n.count = static_cast<int32_t>(cases.count());
    return failed_ ? -1 : add(n);
  }

  // --- expressions
  int32_t expression() {
    int32_t l = assignment();
    while (l >= 0 && is(",")) {
      ++pos_;
      Node n;
      n.kind = NodeKind::Comma;
      n.a = l;
      n.b = assignment();
      if (n.b < 0) return -1;
      l = add(n);
    }
    return l;
  }

  int32_t assignment() {
    const int32_t l = ternary();
    if (l < 0) return -1;
    if (cur().kind == Tok::Punct) {
      Op op = Op::None;
      bool is_assign = true;
      const StringView t = cur().text;
      if (t.equals("=")) op = Op::None;
      else if (t.equals("+=")) op = Op::Add;
      else if (t.equals("-=")) op = Op::Sub;
      else if (t.equals("*=")) op = Op::Mul;
      else if (t.equals("/=")) op = Op::Div;
      else if (t.equals("%=")) op = Op::Mod;
      else if (t.equals("<<=")) op = Op::Shl;
      else if (t.equals(">>=")) op = Op::Shr;
      else if (t.equals("&=")) op = Op::And;
      else if (t.equals("|=")) op = Op::Or;
      else if (t.equals("^=")) op = Op::Xor;
      else is_assign = false;
      if (is_assign) {
        ++pos_;
        Node n;
        n.kind = NodeKind::Assign;
        n.op = op;
        n.a = l;
        n.b = assignment();
        if (n.b < 0) return -1;
        n.line = cur().line;
        return add(n);
      }
    }
    return l;
  }

  int32_t ternary() {
    const int32_t c = binary_expr(0);
    if (c < 0 || !is("?")) return c;
    ++pos_;
    Node n;
    n.kind = NodeKind::Ternary;
    n.a = c;
    n.b = expression();
    if (!expect(":")) return -1;
    n.c = assignment();
    return n.b < 0 || n.c < 0 ? -1 : add(n);
  }

  static int precedence(StringView t, Op* op, bool* logical) {
    *logical = false;
    if (t.equals("||")) { *op = Op::LogOr; *logical = true; return 1; }
    if (t.equals("^^")) { *op = Op::LogXor; *logical = true; return 2; }
    if (t.equals("&&")) { *op = Op::LogAnd; *logical = true; return 3; }
    if (t.equals("|")) { *op = Op::Or; return 4; }
    if (t.equals("^")) { *op = Op::Xor; return 5; }
    if (t.equals("&")) { *op = Op::And; return 6; }
    if (t.equals("==")) { *op = Op::Eq; return 7; }
    if (t.equals("!=")) { *op = Op::Ne; return 7; }
    if (t.equals("<")) { *op = Op::Lt; return 8; }
    if (t.equals(">")) { *op = Op::Gt; return 8; }
    if (t.equals("<=")) { *op = Op::Le; return 8; }
    if (t.equals(">=")) { *op = Op::Ge; return 8; }
    if (t.equals("<<")) { *op = Op::Shl; return 9; }
    if (t.equals(">>")) { *op = Op::Shr; return 9; }
    if (t.equals("+")) { *op = Op::Add; return 10; }
    if (t.equals("-")) { *op = Op::Sub; return 10; }
    if (t.equals("*")) { *op = Op::Mul; return 11; }
    if (t.equals("/")) { *op = Op::Div; return 11; }
    if (t.equals("%")) { *op = Op::Mod; return 11; }
    return 0;
  }

  int32_t binary_expr(int min_prec) {
    int32_t l = unary_expr();
    while (l >= 0 && cur().kind == Tok::Punct) {
      Op op;
      bool logical;
      const int prec = precedence(cur().text, &op, &logical);
      if (prec == 0 || prec <= min_prec) break;
      ++pos_;
      const int32_t r = binary_expr(prec);
      if (r < 0) return -1;
      Node n;
      n.kind = logical ? NodeKind::Logical : NodeKind::Binary;
      n.op = op;
      n.a = l;
      n.b = r;
      n.line = cur().line;
      l = add(n);
    }
    return l;
  }

  int32_t unary_expr() {
    if (cur().kind == Tok::Punct) {
      const StringView t = cur().text;
      Node n;
      n.line = cur().line;
      if (t.equals("-") || t.equals("+") || t.equals("!") || t.equals("~")) {
        ++pos_;
        n.kind = NodeKind::Unary;
        n.op = t.equals("-") ? Op::Neg : t.equals("+") ? Op::Plus : t.equals("!") ? Op::Not : Op::BitNot;
        n.a = unary_expr();
        if (n.a < 0) return -1;
        // fold negative literals
        const Node& x = m_.nodes[static_cast<uint64_t>(n.a)];
        if (n.op == Op::Neg && x.kind == NodeKind::Const && x.type.is_scalar() && x.type.base != Base::Bool) {
          uint32_t v = m_.consts[static_cast<uint64_t>(x.a)];
          v = x.type.base == Base::Float ? (v ^ 0x80000000u) : (0u - v);
          return const_node(x.type, &v, 1);
        }
        return add(n);
      }
      if (t.equals("++") || t.equals("--")) {
        ++pos_;
        n.kind = NodeKind::PreInc;
        n.op = t.equals("++") ? Op::Add : Op::Sub;
        n.a = unary_expr();
        return n.a < 0 ? -1 : add(n);
      }
    }
    return postfix_expr();
  }

  int32_t postfix_expr() {
    int32_t e = primary();
    while (e >= 0 && !failed_) {
      if (is("[")) {
        ++pos_;
        Node n;
        n.kind = NodeKind::Index;
        n.a = e;
        n.b = expression();
        if (n.b < 0 || !expect("]")) return -1;
        e = add(n);
      } else if (is(".")) {
        ++pos_;
        if (cur().kind != Tok::Ident) return err("expected a field name"), -1;
        const StringView name = cur().text;
        ++pos_;
        if (name.equals("length") && is("(")) {
          ++pos_;
          if (!expect(")")) return -1;
          Node n;
          n.kind = NodeKind::Length;
          n.a = e;
          e = add(n);
          continue;
        }
        Node n;
        n.kind = NodeKind::Field;
        n.a = e;
        n.b = name_chars(name);
        n.c = static_cast<int32_t>(name.size());
        e = add(n);
      } else if (is("++") || is("--")) {
        Node n;
        n.kind = NodeKind::PostInc;
        n.op = is("++") ? Op::Add : Op::Sub;
        n.a = e;
        ++pos_;
        e = add(n);
      } else {
        break;
      }
    }
    return e;
  }

  int32_t call_args(List<int32_t>* args) {
    if (!expect("(")) return -1;
    if (is_id("void") && peek().kind == Tok::Punct && peek().text.equals(")")) ++pos_;
    while (!is(")") && !failed_ && cur().kind != Tok::End) {
      const int32_t a = assignment();
      if (a < 0) return -1;
      args->push_back(a);
      if (!accept(",")) break;
    }
    return expect(")") ? 0 : -1;
  }

  // The type an expression has, when it can be told without running it (for choosing between overloads).
  bool static_type(int32_t id, Type* t) const {
    if (id < 0) return false;
    const Node& n = m_.nodes[static_cast<uint64_t>(id)];
    switch (n.kind) {
      case NodeKind::Const: case NodeKind::Construct: case NodeKind::Zero: *t = n.type; return true;
      case NodeKind::Global: *t = m_.globals[static_cast<uint64_t>(n.ival)]->type; return true;
      default: return false;
    }
  }

  int32_t primary() {
    const Token tk = cur();
    Node n;
    n.line = tk.line;
    switch (tk.kind) {
      case Tok::Int: { ++pos_; return int_node(static_cast<int32_t>(tk.ival)); }
      case Tok::Uint: { ++pos_; const uint32_t v = tk.ival; return const_node(make_type(Base::Uint), &v, 1); }
      case Tok::Float: { ++pos_; const uint32_t v = f2u(tk.fval); return const_node(make_type(Base::Float), &v, 1); }
      case Tok::Punct:
        if (tk.text.equals("(")) {
          ++pos_;
          const int32_t e = expression();
          if (!expect(")")) return -1;
          return e;
        }
        return err("unexpected token"), -1;
      case Tok::Ident: break;
      default: return err("unexpected end of shader"), -1;
    }
    const StringView w = tk.text;
    if (w.equals("true") || w.equals("false")) {
      ++pos_;
      const uint32_t v = w.equals("true");
      return const_node(make_type(Base::Bool), &v, 1);
    }
    // constructors (including arrays: float[3](...))
    Type ct;
    if (find_type_name(w, &ct) && (peek().kind == Tok::Punct && (peek().text.equals("(") || peek().text.equals("[")))) {
      ++pos_;
      if (is("[") && !parse_array_suffix(&ct)) return -1;
      List<int32_t> args(a_);
      if (call_args(&args) < 0) return -1;
      if (ct.array == 0xFFFFFFFFu) ct.array = static_cast<uint32_t>(args.count());
      n.kind = NodeKind::Construct;
      n.type = ct;
      n.first = add_kids(args.view().data(), static_cast<uint32_t>(args.count()));
      n.count = static_cast<int32_t>(args.count());
      return add(n);
    }
    if (peek().kind == Tok::Punct && peek().text.equals("(")) {
      ++pos_;
      List<int32_t> args(a_);
      if (call_args(&args) < 0) return -1;
      return make_call(w, args);
    }
    ++pos_;
    Sym* s = find_sym(w);
    if (!s) {
      String msg(a_);
      msg.assign("undeclared identifier '");
      msg.append(w);
      msg.append("'");
      pos_--;
      err(msg.c_str());
      ++pos_;
      return -1;
    }
    if (s->kind == 2) {   // block instance: inst.member
      if (!accept(".") || cur().kind != Tok::Ident) return err("expected a member of the block"), -1;
      const StringView member = cur().text;
      ++pos_;
      for (uint64_t i = m_.globals.count(); i > 0; --i) {
        const Global& g = *m_.globals[i - 1];
        if (g.block == s->index && g.name.view().equals(member)) {
          n.kind = NodeKind::Global;
          n.ival = static_cast<int32_t>(i - 1);
          return add(n);
        }
      }
      return err("no such member in the block"), -1;
    }
    n.kind = s->kind == 0 ? NodeKind::Local : NodeKind::Global;
    n.ival = s->index;
    if (s->kind == 1) m_.globals[static_cast<uint64_t>(s->index)]->referenced = true;
    if (s->kind == 1 && m_.globals[static_cast<uint64_t>(s->index)]->builtin == kBvFragDepth) m_.writes_frag_depth = true;
    return add(n);
  }

  int32_t make_call(StringView name, List<int32_t>& args) {
    Node n;
    n.line = cur().line;
    n.first = add_kids(args.view().data(), static_cast<uint32_t>(args.count()));
    n.count = static_cast<int32_t>(args.count());
    // user functions first (they can shadow built-ins only by overloading)
    int32_t best = -1;
    for (uint64_t i = 0; i < m_.functions.count(); ++i) {
      const Function& f = *m_.functions[i];
      if (!f.name.view().equals(name) || f.params.count() != args.count()) continue;
      bool ok = true;
      for (uint64_t k = 0; k < args.count(); ++k) {
        Type st;
        if (static_type(args[k], &st) && st.base != f.params[k].type.base && !(st.base == Base::Int && f.params[k].type.base == Base::Float)) ok = false;
        else if (static_type(args[k], &st) && st.base == f.params[k].type.base && (st.rows != f.params[k].type.rows || st.cols != f.params[k].type.cols)) ok = false;
      }
      if (ok) { best = static_cast<int32_t>(i); if (f.body >= 0) break; }
    }
    if (best >= 0) {
      n.kind = NodeKind::Call;
      n.ival = best;
      return add(n);
    }
    for (const BuiltinName& b : kBuiltins)
      if (name.equals(b.name)) {
        n.kind = NodeKind::Builtin;
        n.ival = static_cast<int32_t>(b.fn);
        if (b.fn == BuiltinFn::Dfdx || b.fn == BuiltinFn::Dfdy || b.fn == BuiltinFn::Fwidth) m_.uses_derivatives = true;
        return add(n);
      }
    for (const TexName& t : kTex)
      if (name.equals(t.name)) return make_tex(t, args);
    String msg(a_);
    msg.assign("unknown function '");
    msg.append(name);
    msg.append("'");
    err(msg.c_str());
    return -1;
  }

  int32_t make_tex(const TexName& t, List<int32_t>& args) {
    int32_t kid[9];
    for (int i = 0; i < 9; ++i) kid[i] = -1;
    auto arg = [&](uint64_t i) { return i < args.count() ? args[i] : -1; };
    TexOp op = t.op;
    kid[0] = arg(0);
    kid[2] = arg(1);
    switch (t.form) {
      case 0:
        if (args.count() >= 3) { kid[4] = arg(2); op = op == TexOp::Proj ? TexOp::ProjBias : TexOp::Bias; }
        break;
      case 1: kid[4] = arg(2); break;
      case 2: kid[5] = arg(2); kid[6] = arg(3); break;
      case 3:
        kid[7] = arg(2);
        if (args.count() >= 4) { kid[4] = arg(3); op = op == TexOp::Proj ? TexOp::ProjBias : TexOp::Bias; }
        break;
      case 4: kid[4] = arg(2); kid[7] = arg(3); break;
      case 5: kid[5] = arg(2); kid[6] = arg(3); kid[7] = arg(4); break;
      case 6: kid[4] = arg(2); break;
      case 7: kid[4] = arg(2); kid[7] = arg(3); break;
      case 8: kid[2] = -1; kid[4] = arg(1); break;
      case 9: kid[8] = arg(2); break;
      case 10: kid[2] = -1; break;
      default: break;
    }
    if (t.form == 9 && args.count() >= 3) {   // textureGather(shadowSampler, P, refZ)
      kid[4] = arg(2);
    }
    Node n;
    n.kind = NodeKind::TexCall;
    n.op = static_cast<Op>(op);
    n.first = add_kids(kid, 9);
    n.count = 9;
    n.line = cur().line;
    if (op == TexOp::Sample || op == TexOp::Bias || op == TexOp::Proj || op == TexOp::ProjBias) m_.uses_implicit_lod = true;
    return add(n);
  }
};

}  // namespace

bool parse_glsl(Module& module, StringView source, Stage stage) {
  module.stage = stage;
  module.language = Language::Glsl100;
  String pp(module.allocator);
  uint32_t version = 100;
  Preprocessor pre(module.allocator, &module.error);
  if (!pre.run(source, &pp, &version)) {
    if (module.error.empty()) module.error.assign("preprocessor error");
    return false;
  }
  module.glsl_version = version;
  if (version >= 300) module.language = Language::Glsl300;
  List<Token> toks(module.allocator);
  if (!lex(pp.view(), &toks)) {
    module.error.assign("out of memory");
    return false;
  }
  Parser parser(module, toks, stage);
  return parser.run();
}

}  // namespace shader
