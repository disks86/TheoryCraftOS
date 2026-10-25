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

// The WGSL front end: lexer and recursive-descent parser that build a shader::Module (the same node tree the GLSL front end makes).
#include "WebGl/ShaderInternal.hpp"

namespace shader {

namespace {

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

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
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++i; continue; }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {   // block comments nest
      int depth = 1;
      i += 2;
      while (i < src.size() && depth > 0) {
        if (src[i] == '\n') ++line;
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') { ++depth; i += 2; }
        else if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') { --depth; i += 2; }
        else ++i;
      }
      continue;
    }
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
        t.kind = Tok::Int;
        t.ival = v;
      } else {
        bool is_float = false;
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
        if (is_float || (j < src.size() && (src[j] == 'f' || src[j] == 'h'))) {
          t.kind = Tok::Float;
          t.fval = parse_float(num);
          if (j < src.size() && (src[j] == 'f' || src[j] == 'h')) ++j;
        } else {
          uint32_t v = 0;
          for (uint64_t k = 0; k < num.size(); ++k) v = v * 10 + static_cast<uint32_t>(num[k] - '0');
          t.kind = Tok::Int;
          t.ival = v;
        }
      }
      if (t.kind == Tok::Int && j < src.size() && src[j] == 'u') { t.kind = Tok::Uint; ++j; }
      else if (t.kind == Tok::Int && j < src.size() && src[j] == 'i') ++j;
      t.text = src.substr(i, j - i);
      i = j;
    } else {
      static const char* const kThree[] = {"<<=", ">>="};
      static const char* const kTwo[] = {"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
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

struct BuiltinName {
  const char* name;
  BuiltinFn fn;
};

const BuiltinName kBuiltins[] = {
    {"abs", BuiltinFn::Abs}, {"acos", BuiltinFn::Acos}, {"acosh", BuiltinFn::Acosh}, {"all", BuiltinFn::All}, {"any", BuiltinFn::Any}, {"arrayLength", BuiltinFn::ArrayLength},
    {"asin", BuiltinFn::Asin}, {"asinh", BuiltinFn::Asinh}, {"atan", BuiltinFn::Atan}, {"atanh", BuiltinFn::Atanh}, {"atan2", BuiltinFn::Atan2}, {"ceil", BuiltinFn::Ceil},
    {"clamp", BuiltinFn::Clamp}, {"cos", BuiltinFn::Cos}, {"cosh", BuiltinFn::Cosh}, {"countLeadingZeros", BuiltinFn::CountLeadingZeros}, {"countOneBits", BuiltinFn::BitCount},
    {"countTrailingZeros", BuiltinFn::CountTrailingZeros}, {"cross", BuiltinFn::Cross}, {"degrees", BuiltinFn::Degrees}, {"determinant", BuiltinFn::Determinant},
    {"distance", BuiltinFn::Distance}, {"dot", BuiltinFn::Dot}, {"dpdx", BuiltinFn::Dfdx}, {"dpdxCoarse", BuiltinFn::Dfdx}, {"dpdxFine", BuiltinFn::Dfdx}, {"dpdy", BuiltinFn::Dfdy},
    {"dpdyCoarse", BuiltinFn::Dfdy}, {"dpdyFine", BuiltinFn::Dfdy}, {"exp", BuiltinFn::Exp}, {"exp2", BuiltinFn::Exp2}, {"extractBits", BuiltinFn::ExtractBits},
    {"faceForward", BuiltinFn::FaceForward}, {"firstLeadingBit", BuiltinFn::FirstLeadingBit}, {"firstTrailingBit", BuiltinFn::FirstTrailingBit}, {"floor", BuiltinFn::Floor},
    {"fma", BuiltinFn::Fma}, {"fract", BuiltinFn::Fract}, {"fwidth", BuiltinFn::Fwidth}, {"fwidthCoarse", BuiltinFn::Fwidth}, {"fwidthFine", BuiltinFn::Fwidth},
    {"insertBits", BuiltinFn::InsertBits}, {"inverseSqrt", BuiltinFn::InverseSqrt}, {"ldexp", BuiltinFn::Ldexp}, {"length", BuiltinFn::Length}, {"log", BuiltinFn::Log},
    {"log2", BuiltinFn::Log2}, {"max", BuiltinFn::Max}, {"min", BuiltinFn::Min}, {"mix", BuiltinFn::Mix}, {"normalize", BuiltinFn::Normalize},
    {"pack4x8snorm", BuiltinFn::PackSnorm4x8}, {"pack4x8unorm", BuiltinFn::PackUnorm4x8}, {"pack2x16snorm", BuiltinFn::PackSnorm2x16}, {"pack2x16unorm", BuiltinFn::PackUnorm2x16},
    {"pack2x16float", BuiltinFn::PackHalf2x16}, {"pow", BuiltinFn::Pow}, {"quantizeToF16", BuiltinFn::Step1}, {"radians", BuiltinFn::Radians}, {"reflect", BuiltinFn::Reflect},
    {"refract", BuiltinFn::Refract}, {"reverseBits", BuiltinFn::ReverseBits}, {"round", BuiltinFn::Round}, {"saturate", BuiltinFn::Saturate}, {"select", BuiltinFn::Select},
    {"sign", BuiltinFn::Sign}, {"sin", BuiltinFn::Sin}, {"sinh", BuiltinFn::Sinh}, {"smoothstep", BuiltinFn::Smoothstep}, {"sqrt", BuiltinFn::Sqrt}, {"step", BuiltinFn::Step},
    {"tan", BuiltinFn::Tan}, {"tanh", BuiltinFn::Tanh}, {"transpose", BuiltinFn::Transpose}, {"trunc", BuiltinFn::Trunc}, {"unpack4x8snorm", BuiltinFn::UnpackSnorm4x8},
    {"unpack4x8unorm", BuiltinFn::UnpackUnorm4x8}, {"unpack2x16snorm", BuiltinFn::UnpackSnorm2x16}, {"unpack2x16unorm", BuiltinFn::UnpackUnorm2x16},
    {"unpack2x16float", BuiltinFn::UnpackHalf2x16}, {"workgroupBarrier", BuiltinFn::Barrier}, {"storageBarrier", BuiltinFn::Barrier}, {"textureBarrier", BuiltinFn::Barrier},
    {"atomicLoad", BuiltinFn::AtomicLoad}, {"atomicStore", BuiltinFn::AtomicStore}, {"atomicAdd", BuiltinFn::AtomicAdd}, {"atomicSub", BuiltinFn::AtomicSub},
    {"atomicMax", BuiltinFn::AtomicMax}, {"atomicMin", BuiltinFn::AtomicMin}, {"atomicAnd", BuiltinFn::AtomicAnd}, {"atomicOr", BuiltinFn::AtomicOr},
    {"atomicXor", BuiltinFn::AtomicXor}, {"atomicExchange", BuiltinFn::AtomicExchange}, {"atomicCompareExchangeWeak", BuiltinFn::AtomicCompareExchange},
};

struct Attrs {
  int32_t group = -1, binding = -1, location = -1;
  int32_t builtin = kBvNone;
  int32_t stage = -1;   // 0 vertex, 1 fragment, 2 compute
  int32_t wg[3] = {1, 1, 1};
  bool flat = false;
  bool has_wg = false;
};

struct Sym {
  explicit Sym(allocators::IAllocator& a) : name(a) {}
  String name;
  Type type;
  uint8_t kind = 0;   // 0 local, 1 global
  int32_t index = 0;
};

struct Pending {
  explicit Pending(allocators::IAllocator& a) : name(a) {}
  String name;
  int32_t node = -1;
};

class Parser {
 public:
  Parser(Module& m, List<Token>& toks) : m_(m), a_(m.allocator), toks_(toks), syms_(m.allocator), scopes_(m.allocator), pending_(m.allocator), aliases_(m.allocator) {}

  bool run() {
    push_scope();
    while (cur().kind != Tok::End && !failed_) top_level();
    pop_scope();
    if (failed_) return false;
    // calls to functions declared later
    for (uint64_t i = 0; i < pending_.count(); ++i) {
      const int32_t f = m_.find_function(pending_[i]->name.view());
      if (f < 0 || m_.functions[static_cast<uint64_t>(f)]->body < 0) {
        m_.error.assign("unknown function '");
        m_.error.append(pending_[i]->name.view());
        m_.error.append("'");
        return false;
      }
      m_.nodes[static_cast<uint64_t>(pending_[i]->node)].ival = f;
    }
    return true;
  }

 private:
  Module& m_;
  allocators::IAllocator& a_;
  List<Token>& toks_;
  List<UniquePointer<Sym>> syms_;
  List<uint64_t> scopes_;
  List<UniquePointer<Pending>> pending_;
  struct Alias { String name; Type type; explicit Alias(allocators::IAllocator& a) : name(a) {} };
  List<UniquePointer<Alias>> aliases_;
  uint64_t pos_ = 0;
  bool failed_ = false;
  uint32_t nlocals_ = 0;
  uint32_t template_depth_ = 0;   // inside <...>: '>' ends the list instead of comparing

  const Token& cur() const { return toks_[pos_]; }
  const Token& peek(uint64_t n = 1) const { return toks_[pos_ + n < toks_.count() ? pos_ + n : toks_.count() - 1]; }
  bool is(const char* p) const { return cur().kind == Tok::Punct && cur().text.equals(p); }
  bool is_id(const char* w) const { return cur().kind == Tok::Ident && cur().text.equals(w); }
  bool accept(const char* p) { if (is(p)) { ++pos_; return true; } return false; }
  bool accept_id(const char* w) { if (is_id(w)) { ++pos_; return true; } return false; }
  bool expect(const char* p) {
    if (accept(p)) return true;
    String msg(a_);
    msg.assign("expected '");
    msg.append(p);
    msg.append("'");
    return err(msg.c_str());
  }
  bool err(const char* why) {
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
  // the closing '>' of a template list, splitting '>>' and '>='
  bool expect_gt() {
    if (cur().kind == Tok::Punct) {
      Token& t = toks_[pos_];
      if (t.text.equals(">")) { ++pos_; return true; }
      if (t.text.equals(">>") || t.text.equals(">=") || t.text.equals(">>=")) {
        t.text = t.text.substr(1);
        return true;
      }
    }
    return err("expected '>'");
  }

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
      case NodeKind::Construct:
      case NodeKind::Cast:
        return n.count == 1 ? fold_int(m_.kids[static_cast<uint64_t>(n.first)], v) : (n.kind == NodeKind::Cast && fold_int(n.a, v));
      default: return false;
    }
  }

  // --- attributes
  static int32_t builtin_id(StringView n) {
    if (n.equals("position")) return kBvPosition;
    if (n.equals("vertex_index")) return kBvVertexIndex;
    if (n.equals("instance_index")) return kBvInstanceIndex;
    if (n.equals("front_facing")) return kBvFrontFacing;
    if (n.equals("frag_depth")) return kBvFragDepth;
    if (n.equals("local_invocation_id")) return kBvLocalInvocationId;
    if (n.equals("local_invocation_index")) return kBvLocalInvocationIndex;
    if (n.equals("global_invocation_id")) return kBvGlobalInvocationId;
    if (n.equals("workgroup_id")) return kBvWorkgroupId;
    if (n.equals("num_workgroups")) return kBvNumWorkgroups;
    if (n.equals("sample_index")) return kBvSampleIndex;
    if (n.equals("sample_mask")) return kBvSampleMask;
    return kBvNone;
  }

  bool const_int(int64_t* v) {
    ++template_depth_;
    const int32_t e = ternary();
    --template_depth_;
    return e >= 0 && fold_int(e, v);
  }

  bool parse_attrs(Attrs* a) {
    while (is("@")) {
      ++pos_;
      if (cur().kind != Tok::Ident) return err("expected an attribute name");
      const StringView name = cur().text;
      ++pos_;
      if (name.equals("vertex")) a->stage = 0;
      else if (name.equals("fragment")) a->stage = 1;
      else if (name.equals("compute")) a->stage = 2;
      else if (name.equals("group") || name.equals("binding") || name.equals("location") || name.equals("align") || name.equals("size") || name.equals("id")) {
        int64_t v = 0;
        if (!expect("(") || !const_int(&v)) return err("bad attribute value");
        accept(",");
        if (!expect(")")) return false;
        if (name.equals("group")) a->group = static_cast<int32_t>(v);
        else if (name.equals("binding")) a->binding = static_cast<int32_t>(v);
        else if (name.equals("location")) a->location = static_cast<int32_t>(v);
      } else if (name.equals("builtin")) {
        if (!expect("(") || cur().kind != Tok::Ident) return err("bad builtin attribute");
        a->builtin = builtin_id(cur().text);
        if (a->builtin == kBvNone) return err("unknown builtin");
        ++pos_;
        accept(",");
        if (!expect(")")) return false;
      } else if (name.equals("interpolate")) {
        if (!expect("(")) return false;
        while (!is(")") && cur().kind != Tok::End) {
          if (is_id("flat")) a->flat = true;
          ++pos_;
        }
        if (!expect(")")) return false;
      } else if (name.equals("workgroup_size")) {
        if (!expect("(")) return false;
        a->has_wg = true;
        for (int i = 0; i < 3 && !is(")"); ++i) {
          int64_t v = 1;
          if (!const_int(&v)) return err("workgroup size must be constant");
          a->wg[i] = static_cast<int32_t>(v);
          if (!accept(",")) break;
        }
        if (!expect(")")) return false;
      } else if (name.equals("invariant") || name.equals("must_use") || name.equals("const") || name.equals("diagnostic")) {
        if (is("(")) {
          int depth = 0;
          do {
            if (is("(")) ++depth;
            else if (is(")")) --depth;
            ++pos_;
          } while (depth > 0 && cur().kind != Tok::End);
        }
      } else {
        return err("unknown attribute");
      }
    }
    return true;
  }

  // --- types
  static bool vec_n(StringView n, uint8_t* rows, Base* b, bool* shorthand) {
    if (n.size() >= 4 && n.substr(0, 3).equals("vec") && n[3] >= '2' && n[3] <= '4') {
      *rows = static_cast<uint8_t>(n[3] - '0');
      if (n.size() == 4) { *shorthand = false; *b = Base::Float; return true; }
      if (n.size() == 5) {
        *shorthand = true;
        switch (n[4]) {
          case 'f': case 'h': *b = Base::Float; return true;
          case 'i': *b = Base::Int; return true;
          case 'u': *b = Base::Uint; return true;
          default: return false;
        }
      }
    }
    return false;
  }
  static bool mat_n(StringView n, uint8_t* cols, uint8_t* rows, bool* shorthand) {
    if (n.size() >= 6 && n.substr(0, 3).equals("mat") && n[3] >= '2' && n[3] <= '4' && n[4] == 'x' && n[5] >= '2' && n[5] <= '4') {
      *cols = static_cast<uint8_t>(n[3] - '0');
      *rows = static_cast<uint8_t>(n[5] - '0');
      *shorthand = n.size() == 7 && (n[6] == 'f' || n[6] == 'h');
      return n.size() == 6 || *shorthand;
    }
    return false;
  }
  bool scalar_type(StringView n, Type* t) const {
    if (n.equals("f32") || n.equals("f16")) { *t = make_type(Base::Float); return true; }
    if (n.equals("i32")) { *t = make_type(Base::Int); return true; }
    if (n.equals("u32")) { *t = make_type(Base::Uint); return true; }
    if (n.equals("bool")) { *t = make_type(Base::Bool); return true; }
    return false;
  }
  bool is_type_start() const {
    if (cur().kind != Tok::Ident) return false;
    Type t;
    return type_name_known(cur().text, &t);
  }
  bool type_name_known(StringView n, Type* t) const {
    uint8_t r, c;
    Base b;
    bool sh;
    if (scalar_type(n, t)) return true;
    if (vec_n(n, &r, &b, &sh) || mat_n(n, &c, &r, &sh)) return true;
    if (n.equals("array") || n.equals("atomic") || n.equals("ptr") || n.equals("sampler") || n.equals("sampler_comparison")) return true;
    if (n.starts_with("texture_")) return true;
    for (uint64_t i = 0; i < m_.structs.count(); ++i)
      if (m_.structs[i]->name.view().equals(n)) return true;
    for (uint64_t i = 0; i < aliases_.count(); ++i)
      if (aliases_[i]->name.view().equals(n)) return true;
    return false;
  }

  bool parse_type(Type* out) {
    if (cur().kind != Tok::Ident) return err("expected a type");
    const StringView n = cur().text;
    ++pos_;
    uint8_t r, c;
    Base b;
    bool shorthand;
    if (scalar_type(n, out)) return true;
    if (vec_n(n, &r, &b, &shorthand)) {
      *out = make_type(b, r);
      if (!shorthand && is("<")) {
        ++pos_;
        Type e;
        if (!parse_type(&e) || !expect_gt()) return false;
        out->base = e.base;
      }
      return true;
    }
    if (mat_n(n, &c, &r, &shorthand)) {
      *out = make_type(Base::Float, r, c);
      if (!shorthand && is("<")) {
        ++pos_;
        Type e;
        if (!parse_type(&e) || !expect_gt()) return false;
      }
      return true;
    }
    if (n.equals("array")) {
      if (!expect("<")) return false;
      Type e;
      if (!parse_type(&e)) return false;
      uint32_t count = kRuntimeArray;
      if (accept(",")) {
        int64_t v;
        if (!const_int(&v) || v <= 0) return err("array size must be a positive constant");
        count = static_cast<uint32_t>(v);
      }
      if (!expect_gt()) return false;
      *out = e;
      out->array = count;
      return true;
    }
    if (n.equals("atomic")) {
      Type e;
      if (!expect("<") || !parse_type(&e) || !expect_gt()) return false;
      *out = e;
      return true;
    }
    if (n.equals("ptr")) {
      Type e;
      if (!expect("<")) return false;
      ++pos_;   // address space
      if (!expect(",") || !parse_type(&e)) return false;
      if (accept(",")) ++pos_;   // access mode
      if (!expect_gt()) return false;
      *out = e;
      return true;
    }
    if (n.equals("sampler") || n.equals("sampler_comparison")) {
      *out = make_type(Base::SamplerState);
      return true;
    }
    if (n.starts_with("texture_")) {
      Type t = make_type(Base::Texture);
      t.texel = Base::Float;
      const StringView k = n.substr(8);
      if (k.starts_with("depth_")) {
        t.shadow = true;
        const StringView d = k.substr(6);
        t.dim = d.equals("2d") ? kDim2D : d.equals("cube") ? kDimCube : d.equals("2d_array") ? kDim2DArray : d.equals("cube_array") ? kDimCubeArray : kDim2D;
        if (d.equals("multisampled_2d")) t.dim = kDim2D;
      } else if (k.starts_with("storage_")) {
        const StringView d = k.substr(8);
        t.dim = d.equals("1d") ? kDim1D : d.equals("3d") ? kDim3D : d.equals("2d_array") ? kDim2DArray : kDim2D;
        if (!expect("<")) return false;
        ++pos_;   // texel format
        if (!expect(",")) return false;
        ++pos_;   // access
        if (!expect_gt()) return false;
        *out = t;
        return true;
      } else {
        t.dim = k.starts_with("1d") ? kDim1D : k.starts_with("2d_array") ? kDim2DArray : k.starts_with("2d") ? kDim2D : k.starts_with("3d") ? kDim3D :
                k.starts_with("cube_array") ? kDimCubeArray : k.starts_with("cube") ? kDimCube : k.starts_with("multisampled_2d") ? kDim2D : kDim2D;
        if (k.starts_with("multisampled")) t.dim = kDim2D;
        if (is("<")) {
          ++pos_;
          Type e;
          if (!parse_type(&e) || !expect_gt()) return false;
          t.texel = e.base;
        }
      }
      *out = t;
      return true;
    }
    for (uint64_t i = 0; i < m_.structs.count(); ++i)
      if (m_.structs[i]->name.view().equals(n)) {
        *out = make_type(Base::Struct);
        out->sid = static_cast<uint16_t>(i);
        return true;
      }
    for (uint64_t i = 0; i < aliases_.count(); ++i)
      if (aliases_[i]->name.view().equals(n)) {
        *out = aliases_[i]->type;
        return true;
      }
    --pos_;
    return err("unknown type");
  }

  // --- declarations
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

  void top_level() {
    if (accept(";")) return;
    Attrs at;
    if (!parse_attrs(&at)) return;
    if (is_id("struct")) { struct_decl(); return; }
    if (is_id("alias")) {
      ++pos_;
      if (cur().kind != Tok::Ident) { err("expected a name"); return; }
      UniquePointer<Alias> al = UniquePointer<Alias>::make(a_, a_);
      al->name.assign(cur().text);
      ++pos_;
      if (!expect("=") || !parse_type(&al->type) || !expect(";")) return;
      aliases_.push_back(static_cast<UniquePointer<Alias>&&>(al));
      return;
    }
    if (is_id("enable") || is_id("requires") || is_id("diagnostic")) {
      while (!is(";") && cur().kind != Tok::End) ++pos_;
      expect(";");
      return;
    }
    if (is_id("const_assert")) {
      while (!is(";") && cur().kind != Tok::End) ++pos_;
      expect(";");
      return;
    }
    if (is_id("fn")) { function_decl(at); return; }
    if (is_id("var")) { global_var(at); return; }
    if (is_id("const") || is_id("override")) {
      const bool is_override = is_id("override");
      ++pos_;
      if (cur().kind != Tok::Ident) { err("expected a name"); return; }
      const StringView name = cur().text;
      ++pos_;
      Type t;
      bool typed = false;
      if (accept(":")) { if (!parse_type(&t)) return; typed = true; }
      int32_t init = -1;
      if (accept("=")) init = expression();
      if (!expect(";")) return;
      if (!typed) {
        if (init >= 0 && m_.nodes[static_cast<uint64_t>(init)].kind == NodeKind::Const) t = m_.nodes[static_cast<uint64_t>(init)].type;
        else t = make_type(Base::Float);
      }
      if (typed && init >= 0 && m_.nodes[static_cast<uint64_t>(init)].kind == NodeKind::Const) {
        // a typed constant takes the type of its declaration
        Node& n = m_.nodes[static_cast<uint64_t>(init)];
        if (n.type.is_scalar() && n.type.base != t.base && t.is_scalar()) {
          uint32_t v = m_.consts[static_cast<uint64_t>(n.a)];
          v = convert_scalar(v, n.type.base, t.base);
          init = const_node(t, &v, 1);
        }
      }
      const int32_t g = new_global(name, t, GlobalKind::Const, false);
      if (g >= 0) {
        m_.globals[static_cast<uint64_t>(g)]->init = init;
        m_.globals[static_cast<uint64_t>(g)]->is_read_only = true;
        if (is_override) m_.globals[static_cast<uint64_t>(g)]->storage = true;   // (marks an override)
      }
      return;
    }
    err("unexpected declaration");
  }

  void struct_decl() {
    ++pos_;
    UniquePointer<StructDef> sd = UniquePointer<StructDef>::make(a_, a_);
    if (!sd || cur().kind != Tok::Ident) { err("expected a struct name"); return; }
    sd->name.assign(cur().text);
    ++pos_;
    if (!expect("{")) return;
    uint32_t offset = 0;
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      Attrs fa;
      if (!parse_attrs(&fa)) return;
      if (cur().kind != Tok::Ident) { err("expected a member name"); return; }
      Field f(a_);
      f.name.assign(cur().text);
      ++pos_;
      if (!expect(":") || !parse_type(&f.type)) return;
      f.offset = offset;
      f.location = fa.location;
      f.builtin = fa.builtin;
      f.flat = fa.flat;
      offset += m_.slots_of(f.type);
      sd->fields.push_back(static_cast<Field&&>(f));
      if (!accept(",") && !is("}")) { err("expected ',' or '}'"); return; }
    }
    if (!expect("}")) return;
    sd->slots = offset;
    m_.structs.push_back(static_cast<UniquePointer<StructDef>&&>(sd));
  }

  void global_var(const Attrs& at) {
    ++pos_;   // var
    StringView space, access;
    if (accept("<")) {
      if (cur().kind == Tok::Ident) { space = cur().text; ++pos_; }
      if (accept(",") && cur().kind == Tok::Ident) { access = cur().text; ++pos_; }
      if (!expect_gt()) return;
    }
    if (cur().kind != Tok::Ident) { err("expected a name"); return; }
    const StringView name = cur().text;
    ++pos_;
    Type t;
    if (!expect(":") || !parse_type(&t)) return;
    int32_t init = -1;
    if (accept("=")) init = expression();
    if (!expect(";")) return;
    GlobalKind kind = GlobalKind::Private;
    bool shared = false;
    if (space.equals("uniform") || space.equals("storage")) { kind = GlobalKind::Buffer; shared = true; }
    else if (space.equals("workgroup")) { kind = GlobalKind::Workgroup; shared = true; }
    else if (t.base == Base::Texture || t.base == Base::SamplerState) { kind = GlobalKind::Handle; shared = true; }
    const int32_t g = new_global(name, t, kind, shared);
    if (g < 0) return;
    Global& gv = *m_.globals[static_cast<uint64_t>(g)];
    gv.group = at.group;
    gv.binding = at.binding;
    gv.storage = space.equals("storage");
    gv.is_read_only = space.equals("uniform") || (gv.storage && !access.equals("read_write"));
    gv.init = init;
    if (kind == GlobalKind::Workgroup || kind == GlobalKind::Private) gv.init = init;
    if (kind == GlobalKind::Workgroup) m_.uses_barrier = m_.uses_barrier;
  }

  void function_decl(const Attrs& at) {
    ++pos_;
    if (cur().kind != Tok::Ident) { err("expected a function name"); return; }
    UniquePointer<Function> fn = UniquePointer<Function>::make(a_, a_);
    fn->name.assign(cur().text);
    ++pos_;
    if (!expect("(")) return;
    List<UniquePointer<String>> pnames(a_);
    List<bool> is_ptr(a_);
    while (!is(")") && !failed_ && cur().kind != Tok::End) {
      Attrs pa;
      if (!parse_attrs(&pa)) return;
      if (cur().kind != Tok::Ident) { err("expected a parameter name"); return; }
      UniquePointer<String> pn = UniquePointer<String>::make(a_, a_);
      pn->assign(cur().text);
      ++pos_;
      if (!expect(":")) return;
      const bool ptr = is_id("ptr");
      Param p;
      if (!parse_type(&p.type)) return;
      p.dir = ptr ? 2 : 0;
      p.location = pa.location;
      p.builtin = pa.builtin;
      p.flat = pa.flat;
      fn->params.push_back(p);
      pnames.push_back(static_cast<UniquePointer<String>&&>(pn));
      if (!accept(",")) break;
    }
    if (!expect(")")) return;
    if (accept("->")) {
      Attrs ra;
      if (!parse_attrs(&ra)) return;
      if (!parse_type(&fn->ret)) return;
      fn->ret_location = ra.location;
      fn->ret_builtin = ra.builtin;
    }
    if (at.stage >= 0) {
      fn->is_entry = true;
      fn->stage = at.stage == 0 ? Stage::Vertex : (at.stage == 1 ? Stage::Fragment : Stage::Compute);
      fn->wg_x = static_cast<uint32_t>(at.wg[0]);
      fn->wg_y = static_cast<uint32_t>(at.wg[1]);
      fn->wg_z = static_cast<uint32_t>(at.wg[2]);
    }
    m_.functions.push_back(static_cast<UniquePointer<Function>&&>(fn));
    const int32_t index = static_cast<int32_t>(m_.functions.count() - 1);
    nlocals_ = static_cast<uint32_t>(m_.functions[static_cast<uint64_t>(index)]->params.count());
    push_scope();
    for (uint64_t i = 0; i < pnames.count(); ++i) add_sym(pnames[i]->view(), m_.functions[static_cast<uint64_t>(index)]->params[i].type, 0, static_cast<int32_t>(i));
    const int32_t body = block_stmt();
    pop_scope();
    m_.functions[static_cast<uint64_t>(index)]->body = body;
    m_.functions[static_cast<uint64_t>(index)]->locals = nlocals_;
  }

  // --- statements
  int32_t block_stmt() {
    if (!expect("{")) return -1;
    push_scope();
    List<int32_t> kids(a_);
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      const int32_t s = statement();
      if (s < 0) break;
      kids.push_back(s);
    }
    pop_scope();
    if (failed_ || !expect("}")) return -1;
    Node n;
    n.kind = NodeKind::Block;
    n.first = add_kids(kids.view().data(), static_cast<uint32_t>(kids.count()));
    n.count = static_cast<int32_t>(kids.count());
    return add(n);
  }

  int32_t decl_stmt(bool is_var) {
    ++pos_;   // let / var / const
    if (is_var && is("<")) {   // var<function>
      while (!is(">") && cur().kind != Tok::End) ++pos_;
      ++pos_;
    }
    if (cur().kind != Tok::Ident) return err("expected a variable name"), -1;
    const StringView name = cur().text;
    ++pos_;
    Type t;
    bool typed = false;
    if (accept(":")) {
      if (!parse_type(&t)) return -1;
      typed = true;
    }
    int32_t init = -1;
    if (accept("=")) {
      init = expression();
      if (init < 0) return -1;
    }
    Node d;
    d.kind = NodeKind::Decl;
    d.type = typed ? t : make_type(Base::Void);
    d.a = init;
    d.ival = static_cast<int32_t>(nlocals_);
    d.line = cur().line;
    add_sym(name, typed ? t : make_type(Base::Void), 0, static_cast<int32_t>(nlocals_));
    ++nlocals_;
    return add(d);
  }

  // A statement made of an expression, with an assignment operator maybe: `x = 1`, `a[i] += 2`, `i++`, `f(x)`, `_ = e`.
  int32_t simple_stmt() {
    Node n;
    n.line = cur().line;
    if (is_id("_") && peek().kind == Tok::Punct && peek().text.equals("=")) {
      pos_ += 2;
      n.kind = NodeKind::ExprStmt;
      n.a = expression();
      return n.a < 0 ? -1 : add(n);
    }
    const int32_t lhs = unary_expr();
    if (lhs < 0) return -1;
    n.kind = NodeKind::ExprStmt;
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
        Node a;
        a.kind = NodeKind::Assign;
        a.op = op;
        a.a = lhs;
        a.b = expression();
        if (a.b < 0) return -1;
        n.a = add(a);
        return add(n);
      }
      if (t.equals("++") || t.equals("--")) {
        Node a;
        a.kind = NodeKind::PostInc;
        a.op = t.equals("++") ? Op::Add : Op::Sub;
        a.a = lhs;
        ++pos_;
        n.a = add(a);
        return add(n);
      }
    }
    n.a = lhs;
    return add(n);
  }

  int32_t statement() {
    if (failed_) return -1;
    Node n;
    n.line = cur().line;
    Attrs ignored;
    if (is("@") && !parse_attrs(&ignored)) return -1;
    if (is("{")) return block_stmt();
    if (accept(";")) { n.kind = NodeKind::Empty; return add(n); }
    if (is_id("let") || is_id("const")) {
      const int32_t d = decl_stmt(false);
      return d >= 0 && expect(";") ? d : -1;
    }
    if (is_id("var")) {
      const int32_t d = decl_stmt(true);
      return d >= 0 && expect(";") ? d : -1;
    }
    if (is_id("if")) return if_stmt();
    if (is_id("for")) {
      ++pos_;
      if (!expect("(")) return -1;
      push_scope();
      n.kind = NodeKind::For;
      if (!is(";")) {
        if (is_id("var")) n.a = decl_stmt(true);
        else if (is_id("let") || is_id("const")) n.a = decl_stmt(false);
        else n.a = simple_stmt();
        if (n.a < 0) return -1;
      }
      if (!expect(";")) return -1;
      if (!is(";")) n.b = expression();
      if (!expect(";")) return -1;
      if (!is(")")) {
        const int32_t u = simple_stmt();
        if (u < 0) return -1;
        n.c = m_.nodes[static_cast<uint64_t>(u)].a;   // the update is the expression of that statement
      }
      if (!expect(")")) return -1;
      n.d = block_stmt();
      pop_scope();
      return failed_ ? -1 : add(n);
    }
    if (is_id("while")) {
      ++pos_;
      n.kind = NodeKind::While;
      n.a = expression();
      n.b = block_stmt();
      return failed_ ? -1 : add(n);
    }
    if (is_id("loop")) {
      ++pos_;
      n.kind = NodeKind::Loop;
      if (!expect("{")) return -1;
      push_scope();
      List<int32_t> body(a_);
      int32_t cont = -1;
      while (!is("}") && !failed_ && cur().kind != Tok::End) {
        if (is_id("continuing")) {
          ++pos_;
          if (!expect("{")) return -1;
          List<int32_t> ck(a_);
          while (!is("}") && !failed_ && cur().kind != Tok::End) {
            if (is_id("break") && peek().kind == Tok::Ident && peek().text.equals("if")) {
              pos_ += 2;
              Node bi;
              bi.kind = NodeKind::BreakIf;
              bi.a = expression();
              if (bi.a < 0 || !expect(";")) return -1;
              ck.push_back(add(bi));
              continue;
            }
            const int32_t s = statement();
            if (s < 0) return -1;
            ck.push_back(s);
          }
          if (!expect("}")) return -1;
          Node cb;
          cb.kind = NodeKind::Block;
          cb.first = add_kids(ck.view().data(), static_cast<uint32_t>(ck.count()));
          cb.count = static_cast<int32_t>(ck.count());
          cont = add(cb);
          break;
        }
        const int32_t s = statement();
        if (s < 0) return -1;
        body.push_back(s);
      }
      pop_scope();
      if (!expect("}")) return -1;
      Node bb;
      bb.kind = NodeKind::Block;
      bb.flags = 0;
      bb.first = add_kids(body.view().data(), static_cast<uint32_t>(body.count()));
      bb.count = static_cast<int32_t>(body.count());
      n.a = add(bb);
      n.b = cont;
      return failed_ ? -1 : add(n);
    }
    if (is_id("switch")) return switch_stmt();
    if (is_id("return")) {
      ++pos_;
      n.kind = NodeKind::Return;
      if (!is(";")) n.a = expression();
      return expect(";") && !failed_ ? add(n) : -1;
    }
    if (is_id("break")) { ++pos_; n.kind = NodeKind::Break; return expect(";") ? add(n) : -1; }
    if (is_id("continue")) { ++pos_; n.kind = NodeKind::Continue; return expect(";") ? add(n) : -1; }
    if (is_id("discard")) { ++pos_; n.kind = NodeKind::Discard; m_.uses_discard = true; return expect(";") ? add(n) : -1; }
    if (is_id("const_assert")) {
      while (!is(";") && cur().kind != Tok::End) ++pos_;
      n.kind = NodeKind::Empty;
      return expect(";") ? add(n) : -1;
    }
    const int32_t s = simple_stmt();
    return s >= 0 && expect(";") ? s : -1;
  }

  int32_t if_stmt() {
    ++pos_;
    Node n;
    n.kind = NodeKind::If;
    n.a = expression();
    n.b = block_stmt();
    if (accept_id("else")) {
      if (is_id("if")) n.c = if_stmt();
      else n.c = block_stmt();
    }
    return failed_ ? -1 : add(n);
  }

  int32_t switch_stmt() {
    ++pos_;
    Node n;
    n.kind = NodeKind::Switch;
    n.a = expression();
    if (!expect("{")) return -1;
    List<int32_t> cases(a_);
    while (!is("}") && !failed_ && cur().kind != Tok::End) {
      Node c;
      c.kind = NodeKind::Case;
      List<int32_t> sel(a_);
      if (accept_id("default")) {
        c.ival = 1;
      } else if (accept_id("case")) {
        for (;;) {
          if (accept_id("default")) c.ival = 1;
          else {
            const int32_t v = expression();
            if (v < 0) return -1;
            sel.push_back(v);
          }
          if (!accept(",")) break;
          if (is(":") || is("{")) break;
        }
      } else {
        return err("expected case or default"), -1;
      }
      accept(":");
      c.d = block_stmt();
      if (c.d < 0) return -1;
      c.first = add_kids(sel.view().data(), static_cast<uint32_t>(sel.count()));
      c.count = static_cast<int32_t>(sel.count());
      cases.push_back(add(c));
    }
    if (!expect("}")) return -1;
    n.first = add_kids(cases.view().data(), static_cast<uint32_t>(cases.count()));
    n.count = static_cast<int32_t>(cases.count());
    return failed_ ? -1 : add(n);
  }

  // --- expressions
  int32_t expression() { return ternary(); }
  int32_t ternary() { return binary_expr(0); }

  static int precedence(StringView t, Op* op, bool* logical) {
    *logical = false;
    if (t.equals("||")) { *op = Op::LogOr; *logical = true; return 1; }
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
      if (template_depth_ > 0 && (op == Op::Gt || op == Op::Ge || op == Op::Shr)) break;
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
      if (t.equals("-") || t.equals("!") || t.equals("~")) {
        ++pos_;
        n.kind = NodeKind::Unary;
        n.op = t.equals("-") ? Op::Neg : t.equals("!") ? Op::Not : Op::BitNot;
        n.a = unary_expr();
        if (n.a < 0) return -1;
        const Node& x = m_.nodes[static_cast<uint64_t>(n.a)];
        if (n.op == Op::Neg && x.kind == NodeKind::Const && x.type.is_scalar() && x.type.base != Base::Bool) {
          uint32_t v = m_.consts[static_cast<uint64_t>(x.a)];
          v = x.type.base == Base::Float ? (v ^ 0x80000000u) : (0u - v);
          return const_node(x.type, &v, 1);
        }
        return add(n);
      }
      if (t.equals("*") || t.equals("&")) {   // pointers are the places themselves
        ++pos_;
        return unary_expr();
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
        if (cur().kind != Tok::Ident) return err("expected a member name"), -1;
        const StringView name = cur().text;
        ++pos_;
        Node n;
        n.kind = NodeKind::Field;
        n.a = e;
        n.b = name_chars(name);
        n.c = static_cast<int32_t>(name.size());
        e = add(n);
      } else {
        break;
      }
    }
    return e;
  }

  int32_t call_args(List<int32_t>* args) {
    if (!expect("(")) return -1;
    while (!is(")") && !failed_ && cur().kind != Tok::End) {
      const int32_t a = expression();
      if (a < 0) return -1;
      args->push_back(a);
      if (!accept(",")) break;
    }
    return expect(")") ? 0 : -1;
  }

  bool static_type(int32_t id, Type* t) const {
    if (id < 0) return false;
    const Node& n = m_.nodes[static_cast<uint64_t>(id)];
    if (n.kind == NodeKind::Global) { *t = m_.globals[static_cast<uint64_t>(n.ival)]->type; return true; }
    if (n.kind == NodeKind::Local) {
      for (uint64_t i = syms_.count(); i > 0; --i)
        if (syms_[i - 1]->kind == 0 && syms_[i - 1]->index == n.ival) { *t = syms_[i - 1]->type; return true; }
    }
    return false;
  }

  int32_t primary() {
    const Token tk = cur();
    Node n;
    n.line = tk.line;
    switch (tk.kind) {
      case Tok::Int: { ++pos_; return int_node(static_cast<int32_t>(tk.ival)); }
      case Tok::Uint: { ++pos_; const uint32_t v = tk.ival; return const_node(make_type(Base::Uint), &v, 1); }
      case Tok::Float: { ++pos_; const uint32_t v = __builtin_bit_cast(uint32_t, tk.fval); return const_node(make_type(Base::Float), &v, 1); }
      case Tok::Punct:
        if (tk.text.equals("(")) {
          ++pos_;
          const int32_t e = expression();
          return expect(")") ? e : -1;
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
    if (w.equals("bitcast")) {
      ++pos_;
      Type t;
      if (!expect("<") || !parse_type(&t) || !expect_gt() || !expect("(")) return -1;
      n.kind = NodeKind::Bitcast;
      n.type = t;
      n.a = expression();
      return n.a >= 0 && expect(")") ? add(n) : -1;
    }
    // local symbols shadow type and function names
    Sym* sym = find_sym(w);
    const bool call_next = peek().kind == Tok::Punct && peek().text.equals("(");
    const bool templ_next = peek().kind == Tok::Punct && peek().text.equals("<");
    // constructors and conversions
    Type ct;
    const bool known_type = type_name_known(w, &ct);
    if (known_type && !sym && (call_next || templ_next)) {
      const bool bare_array = w.equals("array") && call_next;
      const bool bare_vec = !bare_array && call_next;   // vec3(...) / f32(...) / mat2x2f(...)
      if (bare_array) {
        ++pos_;
        ct = make_type(Base::Void);
        ct.array = 0xFFFFFFFFu;
      } else if (bare_vec) {
        uint8_t r, c;
        Base b;
        bool sh;
        const StringView nm = w;
        if (vec_n(nm, &r, &b, &sh) && !sh) { ++pos_; ct = make_type(Base::Void, r); }
        else if (mat_n(nm, &c, &r, &sh) && !sh) { ++pos_; ct = make_type(Base::Float, r, c); }
        else if (!parse_type(&ct)) return -1;
      } else if (!parse_type(&ct)) {
        return -1;
      }
      List<int32_t> args(a_);
      if (call_args(&args) < 0) return -1;
      if (ct.array == kRuntimeArray) ct.array = static_cast<uint32_t>(args.count());
      n.kind = NodeKind::Construct;
      n.type = ct;
      n.first = add_kids(args.view().data(), static_cast<uint32_t>(args.count()));
      n.count = static_cast<int32_t>(args.count());
      return add(n);
    }
    if (call_next && !sym) {
      ++pos_;
      List<int32_t> args(a_);
      if (call_args(&args) < 0) return -1;
      return make_call(w, args);
    }
    ++pos_;
    if (!sym) {
      String msg(a_);
      msg.assign("undeclared identifier '");
      msg.append(w);
      msg.append("'");
      --pos_;
      err(msg.c_str());
      return -1;
    }
    n.kind = sym->kind == 0 ? NodeKind::Local : NodeKind::Global;
    n.ival = sym->index;
    if (sym->kind == 1) m_.globals[static_cast<uint64_t>(sym->index)]->referenced = true;
    return add(n);
  }

  int32_t make_call(StringView name, List<int32_t>& args) {
    Node n;
    n.line = cur().line;
    // texture functions
    if (name.starts_with("texture")) {
      const int32_t t = make_tex(name, args);
      if (t != -2) return t;
    }
    for (const BuiltinName& b : kBuiltins)
      if (name.equals(b.name)) {
        n.first = add_kids(args.view().data(), static_cast<uint32_t>(args.count()));
        n.count = static_cast<int32_t>(args.count());
        n.kind = NodeKind::Builtin;
        n.ival = static_cast<int32_t>(b.fn);
        if (b.fn == BuiltinFn::Dfdx || b.fn == BuiltinFn::Dfdy || b.fn == BuiltinFn::Fwidth) m_.uses_derivatives = true;
        if (b.fn == BuiltinFn::Barrier) m_.uses_barrier = true;
        return add(n);
      }
    // a user function, maybe declared further down
    n.first = add_kids(args.view().data(), static_cast<uint32_t>(args.count()));
    n.count = static_cast<int32_t>(args.count());
    n.kind = NodeKind::Call;
    const int32_t f = m_.find_function(name);
    n.ival = f;
    const int32_t id = add(n);
    if (f < 0) {
      UniquePointer<Pending> p = UniquePointer<Pending>::make(a_, a_);
      p->name.assign(name);
      p->node = id;
      pending_.push_back(static_cast<UniquePointer<Pending>&&>(p));
    }
    return id;
  }

  // Returns -2 if the name is not a texture function.
  int32_t make_tex(StringView name, List<int32_t>& args) {
    int32_t kid[9];
    for (int i = 0; i < 9; ++i) kid[i] = -1;
    TexOp op = TexOp::Sample;
    bool compare_first = false;   // textureGather(component, t, s, ...)
    uint64_t at = 0;
    bool needs_s = true;
    uint32_t extra = 0;   // scalar arguments before the offset
    enum { Ref, Lod, Bias, Grad, None } kind = None;
    if (name.equals("textureSample")) { op = TexOp::Sample; }
    else if (name.equals("textureSampleBias")) { op = TexOp::Bias; kind = Bias; extra = 1; }
    else if (name.equals("textureSampleLevel")) { op = TexOp::Lod; kind = Lod; extra = 1; }
    else if (name.equals("textureSampleGrad")) { op = TexOp::Grad; kind = Grad; extra = 2; }
    else if (name.equals("textureSampleCompare")) { op = TexOp::Compare; kind = Ref; extra = 1; }
    else if (name.equals("textureSampleCompareLevel")) { op = TexOp::CompareLevel; kind = Ref; extra = 1; }
    else if (name.equals("textureLoad")) { op = TexOp::Fetch; needs_s = false; }
    else if (name.equals("textureStore")) { op = TexOp::Store; needs_s = false; }
    else if (name.equals("textureDimensions")) { op = TexOp::Size; needs_s = false; }
    else if (name.equals("textureNumLevels")) { op = TexOp::Levels; needs_s = false; }
    else if (name.equals("textureNumSamples")) { op = TexOp::Samples; needs_s = false; }
    else if (name.equals("textureNumLayers")) { op = TexOp::Size; needs_s = false; }
    else if (name.equals("textureGather") || name.equals("textureGatherCompare")) { op = name.equals("textureGather") ? TexOp::Gather : TexOp::GatherCompare; compare_first = true; }
    else return -2;
    if (compare_first && op == TexOp::Gather && args.count() > 0) {
      Type t0;
      static_type(args[0], &t0);
      if (m_.nodes[static_cast<uint64_t>(args[0])].kind == NodeKind::Const) { kid[8] = args[0]; at = 1; }   // component given
      // (a depth texture's gather has no component argument)
    }
    auto arg = [&](uint64_t i) { return i < args.count() ? args[i] : -1; };
    kid[0] = arg(at++);
    Type tt;
    const bool have_type = static_type(kid[0], &tt);
    const bool array = have_type && (tt.dim == kDim2DArray || tt.dim == kDimCubeArray);
    if (needs_s) kid[1] = arg(at++);
    if (op == TexOp::Size || op == TexOp::Levels || op == TexOp::Samples) {
      kid[4] = arg(at);
    } else if (op == TexOp::Fetch) {
      kid[2] = arg(at++);
      if (array) kid[3] = arg(at++);
      kid[4] = arg(at++);   // level or sample index
    } else if (op == TexOp::Store) {
      kid[2] = arg(at++);
      if (array) kid[3] = arg(at++);
      kid[5] = arg(at++);   // the value
    } else {
      kid[2] = arg(at++);
      if (array) kid[3] = arg(at++);
      if (kind == Lod || kind == Bias || kind == Ref) kid[4] = arg(at++);
      else if (kind == Grad) { kid[5] = arg(at++); kid[6] = arg(at++); }
      kid[7] = arg(at);   // the offset, when there is one
    }
    (void)extra;
    Node n;
    n.kind = NodeKind::TexCall;
    n.op = static_cast<Op>(op);
    n.flags = 1;   // WGSL argument layout
    n.first = add_kids(kid, 9);
    n.count = 9;
    n.line = cur().line;
    if (op == TexOp::Sample || op == TexOp::Bias) m_.uses_implicit_lod = true;
    const int32_t id = add(n);
    // textureDimensions of an array texture does not report the layers
    if ((name.equals("textureDimensions")) && array) {
      Node f;
      f.kind = NodeKind::Field;
      f.a = id;
      f.b = name_chars("xy");
      f.c = 2;
      return add(f);
    }
    if (name.equals("textureNumLayers")) {
      Node f;
      f.kind = NodeKind::Field;
      f.a = id;
      f.b = name_chars("z");
      f.c = 1;
      return add(f);
    }
    return id;
  }
};

}  // namespace

bool parse_wgsl(Module& module, StringView source) {
  module.language = Language::Wgsl;
  List<Token> toks(module.allocator);
  if (!lex(source, &toks)) {
    module.error.assign("out of memory");
    return false;
  }
  Parser p(module, toks);
  return p.run();
}

bool set_override(Module& module, StringView name, double value) {
  const int32_t gi = module.find_global(name);
  if (gi < 0) return false;
  Global& g = *module.globals[static_cast<uint64_t>(gi)];
  if (g.kind != GlobalKind::Const) return false;
  uint32_t lane;
  switch (g.type.base) {
    case Base::Float: lane = __builtin_bit_cast(uint32_t, static_cast<float>(value)); break;
    case Base::Bool: lane = value != 0; break;
    case Base::Uint: lane = static_cast<uint32_t>(static_cast<int64_t>(value)); break;
    default: lane = static_cast<uint32_t>(static_cast<int32_t>(value)); break;
  }
  const int32_t off = static_cast<int32_t>(module.consts.count());
  module.consts.push_back(lane);
  Node nd;
  nd.kind = NodeKind::Const;
  nd.a = off;
  nd.type = g.type.is_scalar() ? g.type : make_type(Base::Float);
  module.nodes.push_back(nd);
  g.init = static_cast<int32_t>(module.nodes.count() - 1);
  return true;
}

void relayout_shared(Module& module) {
  uint32_t shared = 0;
  for (uint64_t i = 0; i < module.globals.count(); ++i) {
    Global& g = *module.globals[i];
    if (!g.shared) continue;
    g.slot = shared;
    shared += module.slots_of(g.type);
  }
  module.shared_slots = shared;
}

void set_runtime_length(Module& module, uint32_t global, uint32_t count) {
  if (global >= module.globals.count()) return;
  Global& g = *module.globals[global];
  if (g.type.array == kRuntimeArray) {
    g.type.array = count;
  } else if (g.type.base == Base::Struct) {
    StructDef* sd = module.structs[g.type.sid].get();
    if (sd && sd->fields.count() > 0) {
      Field& last = sd->fields[sd->fields.count() - 1];
      if (last.type.array == kRuntimeArray) {
        last.type.array = count;
        uint32_t off = 0;
        for (uint64_t i = 0; i < sd->fields.count(); ++i) {
          sd->fields[i].offset = off;
          off += module.slots_of(sd->fields[i].type);
        }
        sd->slots = off;
      }
    }
  }
  relayout_shared(module);
}

}  // namespace shader
