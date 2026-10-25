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

// The shader interpreter: values, operators, constructors, statements (see Shader.hpp).
#include "WebGl/ShaderInternal.hpp"
#include "Math/FloatMath.hpp"

namespace shader {

// --- Module -----------------------------------------------------------------------------------------------------------------------------------------------

uint32_t Module::slots_of(const Type& t) const {
  uint32_t one;
  switch (t.base) {
    case Base::Void: one = 0; break;
    case Base::Struct: one = t.sid < structs.count() ? structs[t.sid]->slots : 0; break;
    case Base::Sampler: case Base::Texture: case Base::SamplerState: one = 1; break;
    default: one = static_cast<uint32_t>(t.cols) * t.rows; break;
  }
  return t.array ? (t.array == kRuntimeArray ? one : one * t.array) : one;
}

int32_t Module::find_global(StringView name) const {
  for (uint64_t i = 0; i < globals.count(); ++i)
    if (globals[i]->name.view().equals(name)) return static_cast<int32_t>(i);
  return -1;
}

int32_t Module::find_function(StringView name) const {
  for (uint64_t i = 0; i < functions.count(); ++i)
    if (functions[i]->name.view().equals(name)) return static_cast<int32_t>(i);
  return -1;
}

int32_t Module::find_builtin_global(int32_t builtin) const {
  for (uint64_t i = 0; i < globals.count(); ++i)
    if (globals[i]->builtin == builtin) return static_cast<int32_t>(i);
  return -1;
}

// --- Scalars ----------------------------------------------------------------------------------------------------------------------------------------------

uint32_t convert_scalar(uint32_t v, Base from, Base to) {
  if (from == to) return v;
  switch (from) {
    case Base::Float: {
      const float f = u2f(v);
      switch (to) {
        case Base::Int:
          if (f != f) return 0;
          if (f >= 2147483647.0f) return 0x7FFFFFFFu;
          if (f <= -2147483648.0f) return 0x80000000u;
          return static_cast<uint32_t>(static_cast<int32_t>(f));
        case Base::Uint:
          if (f != f || f <= 0) return 0;
          if (f >= 4294967295.0f) return 0xFFFFFFFFu;
          return static_cast<uint32_t>(f);
        case Base::Bool: return f != 0;
        default: return v;
      }
    }
    case Base::Int:
      if (to == Base::Float) return f2u(static_cast<float>(u2i(v)));
      if (to == Base::Bool) return v != 0;
      return v;
    case Base::Uint:
      if (to == Base::Float) return f2u(static_cast<float>(v));
      if (to == Base::Bool) return v != 0;
      return v;
    case Base::Bool:
      if (to == Base::Float) return f2u(v ? 1.0f : 0.0f);
      return v ? 1 : 0;
    default: return v;
  }
}

namespace {

Base promote(Base a, Base b) {
  if (a == b) return a;
  if (a == Base::Float || b == Base::Float) return Base::Float;
  if (a == Base::Uint || b == Base::Uint) return Base::Uint;
  if (a == Base::Int || b == Base::Int) return Base::Int;
  return a;
}

bool is_compare(Op op) { return op >= Op::Eq && op <= Op::Ge; }

uint32_t scalar_op(Op op, Base k, uint32_t x, uint32_t y) {
  if (k == Base::Float) {
    const float a = u2f(x), b = u2f(y);
    switch (op) {
      case Op::Add: return f2u(a + b);
      case Op::Sub: return f2u(a - b);
      case Op::Mul: return f2u(a * b);
      case Op::Div: return f2u(a / b);
      case Op::Mod: {
        if (b == 0) return f2u(0.0f / 0.0f);
        const float q = a / b;
        const float t = q < 0 ? -ffloor(-q) : ffloor(q);
        return f2u(a - b * t);
      }
      case Op::Eq: return a == b;
      case Op::Ne: return a != b;
      case Op::Lt: return a < b;
      case Op::Le: return a <= b;
      case Op::Gt: return a > b;
      case Op::Ge: return a >= b;
      default: return 0;
    }
  }
  if (k == Base::Bool) {
    switch (op) {
      case Op::And: case Op::LogAnd: return x && y;
      case Op::Or: case Op::LogOr: return x || y;
      case Op::Xor: case Op::LogXor: return (x != 0) != (y != 0);
      case Op::Eq: return (x != 0) == (y != 0);
      case Op::Ne: return (x != 0) != (y != 0);
      default: return 0;
    }
  }
  const bool sgn = k == Base::Int;
  const int32_t sa = u2i(x), sb = u2i(y);
  switch (op) {
    case Op::Add: return x + y;
    case Op::Sub: return x - y;
    case Op::Mul: return x * y;
    case Op::Div:
      if (y == 0) return x;
      if (sgn) return (sa == -2147483647 - 1 && sb == -1) ? x : static_cast<uint32_t>(sa / sb);
      return x / y;
    case Op::Mod:
      if (y == 0) return 0;
      if (sgn) return (sb == -1) ? 0 : static_cast<uint32_t>(sa % sb);
      return x % y;
    case Op::And: return x & y;
    case Op::Or: return x | y;
    case Op::Xor: return x ^ y;
    case Op::Shl: return x << (y & 31);
    case Op::Shr: return sgn ? static_cast<uint32_t>(sa >> (y & 31)) : x >> (y & 31);
    case Op::Eq: return x == y;
    case Op::Ne: return x != y;
    case Op::Lt: return sgn ? sa < sb : x < y;
    case Op::Le: return sgn ? sa <= sb : x <= y;
    case Op::Gt: return sgn ? sa > sb : x > y;
    case Op::Ge: return sgn ? sa >= sb : x >= y;
    default: return 0;
  }
}

}  // namespace

// --- Machine ----------------------------------------------------------------------------------------------------------------------------------------------

Machine::Machine(const Module& module, allocators::IAllocator& allocator, uint32_t arena_slots) : module_(module), allocator_(allocator) {
  arena_size_ = arena_slots;
  arena_ = static_cast<uint32_t*>(allocator.allocate(uint64_t{arena_slots} * 4));
  const uint32_t pn = module.private_slots ? module.private_slots : 1;
  priv_ = static_cast<uint32_t*>(allocator.allocate(uint64_t{pn} * 4));
  vars_ = static_cast<Var*>(allocator.allocate(sizeof(Var) * kMaxVars));
  frames_ = static_cast<Frame*>(allocator.allocate(sizeof(Frame) * kMaxFrames));
  valid_ = arena_ && priv_ && vars_ && frames_;
  if (valid_)
    for (uint32_t i = 0; i < pn; ++i) priv_[i] = 0;
}

Machine::~Machine() {
  allocator_.deallocate(arena_);
  allocator_.deallocate(priv_);
  allocator_.deallocate(vars_);
  allocator_.deallocate(frames_);
}

bool Machine::reset_privates() {
  if (!valid_) return false;
  for (uint32_t i = 0; i < module_.private_slots; ++i) priv_[i] = 0;
  top_ = 0;
  failure_ = nullptr;
  Interp in(*this);
  for (uint64_t i = 0; i < module_.globals.count(); ++i) {
    const Global& g = *module_.globals[i];
    if (g.shared || g.init < 0) continue;
    top_ = 0;
    Ref r;
    if (!in.eval(g.init, &r)) return false;
    LRef l;
    l.p = priv_ + g.slot;
    l.t = g.type;
    if (!in.store(l, r)) return false;
  }
  top_ = 0;
  return true;
}

bool Machine::init_shared(uint32_t* shared) {
  if (!valid_) return false;
  for (uint32_t i = 0; i < module_.shared_slots; ++i) shared[i] = 0;
  shared_ = shared;
  failure_ = nullptr;
  Interp in(*this);
  for (uint64_t i = 0; i < module_.globals.count(); ++i) {
    const Global& g = *module_.globals[i];
    if (!g.shared || g.init < 0) continue;
    top_ = 0;
    Ref r;
    if (!in.eval(g.init, &r)) return false;
    LRef l;
    l.p = shared + g.slot;
    l.t = g.type;
    if (!in.store(l, r)) return false;
  }
  top_ = 0;
  return true;
}

// --- Interp: memory ---------------------------------------------------------------------------------------------------------------------------------------

bool Interp::fail(const char* why) {
  if (!m.failure_) m.failure_ = why;
  return false;
}

uint32_t* Interp::alloc(uint32_t n) {
  if (n > m.arena_size_ - m.top_) {
    fail("shader ran out of stack");
    return nullptr;
  }
  uint32_t* p = m.arena_ + m.top_;
  m.top_ += n;
  for (uint32_t i = 0; i < n; ++i) p[i] = 0;
  return p;
}

bool Interp::new_value(const Type& t, Ref* out, uint32_t** writable) {
  uint32_t* p = alloc(slots(t));
  if (!p) return false;
  out->p = p;
  out->t = t;
  *writable = p;
  return true;
}

bool Interp::convert_to(const Ref& src, Base to, Ref* out) {
  if (src.t.base == to || src.t.base == Base::Struct) {
    *out = src;
    return true;
  }
  Type t = src.t;
  t.base = to;
  uint32_t* w;
  if (!new_value(t, out, &w)) return false;
  const uint32_t n = slots(t);
  for (uint32_t i = 0; i < n; ++i) w[i] = convert_scalar(src.p[i], src.t.base, to);
  return true;
}

// Writes a value, converting number kinds (WGSL's abstract numbers) and broadcasting a scalar over a vector.
bool Interp::store(const LRef& dst, const Ref& src) {
  if (dst.n > 0) {
    const uint32_t srcn = count_of(src.t);
    for (uint32_t i = 0; i < dst.n; ++i) dst.p[dst.swz[i]] = convert_scalar(src.p[srcn == 1 ? 0 : (i < srcn ? i : 0)], src.t.base, dst.t.base);
    return true;
  }
  const uint32_t n = slots(dst.t);
  if (src.t.base == Base::Struct || dst.t.base == Base::Struct || dst.t.base == Base::Sampler || dst.t.base == Base::Texture || dst.t.base == Base::SamplerState) {
    const uint32_t sn = slots(src.t);
    for (uint32_t i = 0; i < n && i < sn; ++i) dst.p[i] = src.p[i];
    return true;
  }
  const uint32_t sn = slots(src.t);
  if (sn == n) {
    if (src.t.base == dst.t.base) {
      if (dst.p != src.p) __builtin_memmove(dst.p, src.p, uint64_t{n} * 4);
    } else {
      for (uint32_t i = 0; i < n; ++i) dst.p[i] = convert_scalar(src.p[i], src.t.base, dst.t.base);
    }
    return true;
  }
  if (sn == 1) {   // broadcast
    const uint32_t v = convert_scalar(src.p[0], src.t.base, dst.t.base);
    for (uint32_t i = 0; i < n; ++i) dst.p[i] = v;
    return true;
  }
  // unsized or mismatched arrays: copy what fits
  for (uint32_t i = 0; i < n && i < sn; ++i) dst.p[i] = convert_scalar(src.p[i], src.t.base, dst.t.base);
  return true;
}

// --- Interp: operators --------------------------------------------------------------------------------------------------------------------------------------

bool Interp::unary(Op op, const Ref& a, Ref* out) {
  Type t = a.t;
  uint32_t* w;
  if (op == Op::Plus) {
    *out = a;
    return true;
  }
  if (!new_value(t, out, &w)) return false;
  const uint32_t n = slots(t);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t v = a.p[i];
    switch (op) {
      case Op::Neg: w[i] = t.base == Base::Float ? (v ^ 0x80000000u) : (0u - v); break;
      case Op::Not: w[i] = v == 0; break;
      case Op::BitNot: w[i] = ~v; break;
      default: w[i] = v; break;
    }
  }
  return true;
}

bool Interp::binary(Op op, const Ref& a, const Ref& b, Ref* out) {
  const Type &ta = a.t, &tb = b.t;
  const bool cmp = is_compare(op);
  if (ta.base == Base::Struct || ta.array || tb.array || tb.base == Base::Struct) {
    if (op != Op::Eq && op != Op::Ne) return fail("operator on a structure or array");
    const uint32_t n = slots(ta);
    bool eq = n == slots(tb);
    for (uint32_t i = 0; eq && i < n; ++i) eq = a.p[i] == b.p[i];
    uint32_t* w;
    if (!new_value(make_type(Base::Bool), out, &w)) return false;
    w[0] = (op == Op::Eq) == eq;
    return true;
  }
  const Base k = promote(ta.base, tb.base);
  const uint32_t na = count_of(ta), nb = count_of(tb);
  // linear algebra products
  if (op == Op::Mul && (ta.cols > 1 || tb.cols > 1) && na > 1 && nb > 1) {
    uint32_t* w;
    Ref fa, fb;
    if (!convert_to(a, Base::Float, &fa) || !convert_to(b, Base::Float, &fb)) return false;
    if (ta.cols > 1 && tb.cols == 1) {   // mat * vec
      if (tb.rows != ta.cols) return fail("matrix and vector sizes do not match");
      if (!new_value(make_type(Base::Float, ta.rows), out, &w)) return false;
      for (uint32_t r = 0; r < ta.rows; ++r) {
        float s = 0;
        for (uint32_t c = 0; c < ta.cols; ++c) s += u2f(fa.p[c * ta.rows + r]) * u2f(fb.p[c]);
        w[r] = f2u(s);
      }
      return true;
    }
    if (ta.cols == 1 && tb.cols > 1) {   // vec * mat
      if (ta.rows != tb.rows) return fail("vector and matrix sizes do not match");
      if (!new_value(make_type(Base::Float, tb.cols), out, &w)) return false;
      for (uint32_t c = 0; c < tb.cols; ++c) {
        float s = 0;
        for (uint32_t r = 0; r < tb.rows; ++r) s += u2f(fa.p[r]) * u2f(fb.p[c * tb.rows + r]);
        w[c] = f2u(s);
      }
      return true;
    }
    if (ta.cols != tb.rows) return fail("matrix sizes do not match");
    if (!new_value(make_type(Base::Float, ta.rows, tb.cols), out, &w)) return false;
    for (uint32_t c = 0; c < tb.cols; ++c)
      for (uint32_t r = 0; r < ta.rows; ++r) {
        float s = 0;
        for (uint32_t i = 0; i < ta.cols; ++i) s += u2f(fa.p[i * ta.rows + r]) * u2f(fb.p[c * tb.rows + i]);
        w[c * ta.rows + r] = f2u(s);
      }
    return true;
  }
  // component-wise, with a scalar broadcast
  Type shape = ta;
  if (na == 1 && nb != 1) shape = tb;
  else if (na != 1 && nb != 1 && (na != nb || ta.cols != tb.cols)) return fail("operand sizes do not match");
  const uint32_t n = count_of(shape);
  const bool glsl_whole = cmp && (op == Op::Eq || op == Op::Ne) && mod.language != Language::Wgsl && n > 1;
  Type rt = shape;
  rt.base = cmp ? Base::Bool : k;
  uint32_t* w;
  if (glsl_whole) rt = make_type(Base::Bool);
  if (!new_value(rt, out, &w)) return false;
  bool all = true;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t x = convert_scalar(a.p[na == 1 ? 0 : i], ta.base, k);
    const uint32_t y = convert_scalar(b.p[nb == 1 ? 0 : i], tb.base, k);
    const uint32_t r = scalar_op(op, k, x, y);
    if (glsl_whole) all = all && (r != 0);
    else w[i] = r;
  }
  if (glsl_whole) w[0] = (op == Op::Eq) ? all : !all;
  // (GLSL's != on vectors is "not all equal", which `all` computed for Ne as the componentwise result)
  if (glsl_whole && op == Op::Ne) {
    bool any = false;
    for (uint32_t i = 0; i < n; ++i) any = any || scalar_op(Op::Ne, k, convert_scalar(a.p[na == 1 ? 0 : i], ta.base, k), convert_scalar(b.p[nb == 1 ? 0 : i], tb.base, k));
    w[0] = any;
  }
  return true;
}

// --- Interp: constructors -----------------------------------------------------------------------------------------------------------------------------------

bool Interp::construct(const Type& t, const Ref* args, uint32_t n, Ref* out) {
  uint32_t* w;
  if (!new_value(t, out, &w)) return false;
  const uint32_t total = slots(t);
  if (t.array) {   // array constructor: the elements one after the other
    const Type et = t.element();
    const uint32_t es = slots(et);
    for (uint32_t i = 0; i < n && i < t.array; ++i) {
      LRef l;
      l.p = w + i * es;
      l.t = et;
      if (!store(l, args[i])) return false;
    }
    return true;
  }
  if (t.base == Base::Struct) {
    const StructDef* sd = mod.struct_of(t);
    if (!sd) return fail("unknown structure");
    for (uint32_t i = 0; i < n && i < sd->fields.count(); ++i) {
      LRef l;
      l.p = w + sd->fields[i].offset;
      l.t = sd->fields[i].type;
      if (!store(l, args[i])) return false;
    }
    return true;
  }
  if (n == 0) return true;   // zero value
  // flatten the arguments' components
  uint32_t flat[64];
  uint32_t fn = 0;
  const Base to = t.base;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t c = slots(args[i].t);
    for (uint32_t j = 0; j < c && fn < 64; ++j) flat[fn++] = convert_scalar(args[i].p[j], args[i].t.base, to);
  }
  if (t.cols > 1) {   // matrix
    if (n == 1 && args[0].t.cols == 1 && args[0].t.rows == 1) {   // diagonal
      for (uint32_t c = 0; c < t.cols; ++c)
        if (c < t.rows) w[c * t.rows + c] = flat[0];
    } else if (n == 1 && args[0].t.cols > 1) {   // from another matrix
      const Type& st = args[0].t;
      for (uint32_t c = 0; c < t.cols; ++c)
        for (uint32_t r = 0; r < t.rows; ++r) {
          if (c < st.cols && r < st.rows) w[c * t.rows + r] = convert_scalar(args[0].p[c * st.rows + r], st.base, to);
          else w[c * t.rows + r] = (c == r) ? f2u(1.0f) : 0;
        }
    } else {
      for (uint32_t i = 0; i < total && i < fn; ++i) w[i] = flat[i];
    }
    return true;
  }
  if (total == 1) {
    w[0] = fn ? flat[0] : 0;
    return true;
  }
  if (fn == 1) {
    for (uint32_t i = 0; i < total; ++i) w[i] = flat[0];
    return true;
  }
  for (uint32_t i = 0; i < total && i < fn; ++i) w[i] = flat[i];
  return true;
}

// --- Interp: fields and swizzles ----------------------------------------------------------------------------------------------------------------------------

bool Interp::read_swizzle(const Node& node, uint32_t* idx, uint32_t* n) const {
  if (node.c < 1 || node.c > 4) return false;
  *n = static_cast<uint32_t>(node.c);
  for (int32_t i = 0; i < node.c; ++i) {
    const char ch = static_cast<char>(mod.consts[static_cast<uint64_t>(node.b + i)]);
    switch (ch) {
      case 'x': case 'r': case 's': idx[i] = 0; break;
      case 'y': case 'g': case 't': idx[i] = 1; break;
      case 'z': case 'b': case 'p': idx[i] = 2; break;
      case 'w': case 'a': case 'q': idx[i] = 3; break;
      default: return false;
    }
  }
  return true;
}

static bool name_is(const Module& mod, const Node& node, StringView name) {
  if (static_cast<uint64_t>(node.c) != name.size()) return false;
  for (int32_t i = 0; i < node.c; ++i)
    if (static_cast<char>(mod.consts[static_cast<uint64_t>(node.b + i)]) != name[static_cast<uint64_t>(i)]) return false;
  return true;
}

bool Interp::field_of(const Ref& base, const Node& node, Ref* out) {
  if (base.t.base == Base::Struct && base.t.array == 0) {
    const StructDef* sd = mod.struct_of(base.t);
    if (!sd) return fail("unknown structure");
    for (uint64_t i = 0; i < sd->fields.count(); ++i)
      if (name_is(mod, node, sd->fields[i].name.view())) {
        out->p = base.p + sd->fields[i].offset;
        out->t = sd->fields[i].type;
        return true;
      }
    return fail("no such field");
  }
  uint32_t idx[4], n;
  if (!read_swizzle(node, idx, &n) || base.t.array) return fail("bad field or swizzle");
  const uint32_t have = count_of(base.t);
  for (uint32_t i = 0; i < n; ++i)
    if (idx[i] >= have) return fail("swizzle out of range");
  Type rt = base.t;
  rt.cols = 1;
  rt.rows = static_cast<uint8_t>(n);
  bool contiguous = true;
  for (uint32_t i = 1; i < n; ++i) contiguous = contiguous && idx[i] == idx[i - 1] + 1;
  if (contiguous) {
    out->p = base.p + idx[0];
    out->t = rt;
    return true;
  }
  uint32_t* w;
  if (!new_value(rt, out, &w)) return false;
  for (uint32_t i = 0; i < n; ++i) w[i] = base.p[idx[i]];
  return true;
}

// --- Interp: expressions ------------------------------------------------------------------------------------------------------------------------------------

bool Interp::lvalue(int32_t id, LRef* out) {
  const Node& node = mod.nodes[static_cast<uint64_t>(id)];
  switch (node.kind) {
    case NodeKind::Local: {
      const Machine::Var& v = m.vars_[m.var_base_ + static_cast<uint32_t>(node.ival)];
      out->p = v.p;
      out->t = v.t;
      out->n = 0;
      return v.p != nullptr || fail("variable not initialised");
    }
    case NodeKind::Global: {
      const Global& g = *mod.globals[static_cast<uint64_t>(node.ival)];
      uint32_t* region = g.shared ? m.shared_ : m.priv_;
      if (!region) return fail("no storage for a global");
      out->p = region + g.slot;
      out->t = g.type;
      out->n = 0;
      return true;
    }
    case NodeKind::Index: {
      LRef base;
      if (!lvalue(node.a, &base) || base.n) return false;
      Ref idx;
      if (!eval(node.b, &idx)) return false;
      int64_t i = idx.t.base == Base::Float ? static_cast<int64_t>(u2f(idx.p[0])) : (idx.t.base == Base::Int ? u2i(idx.p[0]) : static_cast<int64_t>(idx.p[0]));
      Type et;
      uint32_t es, count;
      if (base.t.array) {
        et = base.t.element();
        es = slots(et);
        count = base.t.array;
      } else if (base.t.cols > 1) {
        et = make_type(base.t.base, base.t.rows);
        es = base.t.rows;
        count = base.t.cols;
      } else {
        et = make_type(base.t.base);
        es = 1;
        count = base.t.rows;
      }
      if (i < 0) i = 0;
      if (i >= count) i = count ? count - 1 : 0;
      out->p = base.p + static_cast<uint64_t>(i) * es;
      out->t = et;
      out->n = 0;
      return true;
    }
    case NodeKind::Field: {
      LRef base;
      if (!lvalue(node.a, &base) || base.n) return false;
      if (base.t.base == Base::Struct && base.t.array == 0) {
        Ref r, f;
        r.p = base.p;
        r.t = base.t;
        if (!field_of(r, node, &f)) return false;
        out->p = base.p + (f.p - r.p);
        out->t = f.t;
        out->n = 0;
        return true;
      }
      uint32_t idx[4], n;
      if (!read_swizzle(node, idx, &n)) return fail("bad swizzle");
      out->p = base.p;
      out->t = base.t;
      out->n = static_cast<uint8_t>(n);
      for (uint32_t i = 0; i < n; ++i) out->swz[i] = static_cast<uint8_t>(idx[i]);
      if (n == 1) {   // a single component is an ordinary place
        out->p = base.p + idx[0];
        out->t = make_type(base.t.base);
        out->n = 0;
      }
      return true;
    }
    default: return fail("not assignable");
  }
}

bool Interp::eval(int32_t id, Ref* out) {
  if (id < 0) return fail("missing expression");
  const Node& node = mod.nodes[static_cast<uint64_t>(id)];
  switch (node.kind) {
    case NodeKind::Const:
      out->p = &mod.consts[static_cast<uint64_t>(node.a)];
      out->t = node.type;
      return true;
    case NodeKind::Zero: {
      uint32_t* w;
      return new_value(node.type, out, &w);
    }
    case NodeKind::Local: {
      const Machine::Var& v = m.vars_[m.var_base_ + static_cast<uint32_t>(node.ival)];
      if (!v.p) return fail("variable not initialised");
      out->p = v.p;
      out->t = v.t;
      return true;
    }
    case NodeKind::Global: {
      const Global& g = *mod.globals[static_cast<uint64_t>(node.ival)];
      const uint32_t* region = g.shared ? m.shared_ : m.priv_;
      if (!region) return fail("no storage for a global");
      out->p = region + g.slot;
      out->t = g.type;
      return true;
    }
    case NodeKind::Unary: {
      Ref a;
      return eval(node.a, &a) && unary(node.op, a, out);
    }
    case NodeKind::Binary: {
      Ref a, b;
      return eval(node.a, &a) && eval(node.b, &b) && binary(node.op, a, b, out);
    }
    case NodeKind::Logical: {
      Ref a, b;
      uint32_t* w;
      if (!eval(node.a, &a)) return false;
      if (a.t.rows > 1) {   // WGSL vectors of bool are not short-circuited
        return eval(node.b, &b) && binary(node.op == Op::LogAnd ? Op::And : Op::Or, a, b, out);
      }
      const bool av = a.p[0] != 0;
      if (node.op == Op::LogAnd && !av) {
        if (!new_value(make_type(Base::Bool), out, &w)) return false;
        return true;
      }
      if (node.op == Op::LogOr && av) {
        if (!new_value(make_type(Base::Bool), out, &w)) return false;
        w[0] = 1;
        return true;
      }
      if (!eval(node.b, &b)) return false;
      if (!new_value(make_type(Base::Bool), out, &w)) return false;
      if (node.op == Op::LogXor) w[0] = (av != (b.p[0] != 0));
      else w[0] = b.p[0] != 0;
      return true;
    }
    case NodeKind::Assign: {
      LRef l;
      Ref r;
      if (!eval(node.b, &r)) return false;
      if (node.op != Op::None) {
        Ref cur, res;
        if (!eval(node.a, &cur) || !binary(node.op, cur, r, &res)) return false;
        r = res;
      }
      if (!lvalue(node.a, &l) || !store(l, r)) return false;
      return eval(node.a, out);
    }
    case NodeKind::Ternary: {
      Ref c;
      if (!eval(node.a, &c)) return false;
      return eval(c.p[0] != 0 ? node.b : node.c, out);
    }
    case NodeKind::Comma: {
      Ref ignored;
      return eval(node.a, &ignored) && eval(node.b, out);
    }
    case NodeKind::PreInc:
    case NodeKind::PostInc: {
      LRef l;
      if (!lvalue(node.a, &l)) return false;
      Ref cur, old, res, one;
      if (!eval(node.a, &cur)) return false;
      const Type t = cur.t;
      uint32_t* w;
      if (!new_value(t, &old, &w)) return false;
      const uint32_t n = slots(t);
      for (uint32_t i = 0; i < n; ++i) w[i] = cur.p[i];
      uint32_t* ow;
      if (!new_value(make_type(t.base), &one, &ow)) return false;
      ow[0] = t.base == Base::Float ? f2u(1.0f) : 1;
      if (!binary(node.op, old, one, &res) || !store(l, res)) return false;
      if (node.kind == NodeKind::PreInc) return eval(node.a, out);
      *out = old;
      return true;
    }
    case NodeKind::Index: {
      Ref base, idx;
      if (!eval(node.a, &base) || !eval(node.b, &idx)) return false;
      int64_t i = idx.t.base == Base::Float ? static_cast<int64_t>(u2f(idx.p[0])) : (idx.t.base == Base::Int ? u2i(idx.p[0]) : static_cast<int64_t>(idx.p[0]));
      Type et;
      uint32_t es, count;
      if (base.t.array) {
        et = base.t.element();
        es = slots(et);
        count = base.t.array;
      } else if (base.t.cols > 1) {
        et = make_type(base.t.base, base.t.rows);
        es = base.t.rows;
        count = base.t.cols;
      } else if (base.t.base == Base::Struct) {
        return fail("cannot index a structure");
      } else {
        et = make_type(base.t.base);
        es = 1;
        count = base.t.rows;
      }
      if (i < 0) i = 0;
      if (i >= count) i = count ? count - 1 : 0;
      out->p = base.p + static_cast<uint64_t>(i) * es;
      out->t = et;
      return true;
    }
    case NodeKind::Field: {
      Ref base;
      return eval(node.a, &base) && field_of(base, node, out);
    }
    case NodeKind::Length: {
      Ref base;
      uint32_t* w;
      if (!eval(node.a, &base) || !new_value(make_type(Base::Int), out, &w)) return false;
      w[0] = base.t.array ? base.t.array : (base.t.cols > 1 ? base.t.cols : base.t.rows);
      return true;
    }
    case NodeKind::Construct: {
      Ref args[16];
      if (node.count > 16) return fail("too many constructor arguments");
      for (int32_t i = 0; i < node.count; ++i)
        if (!eval(mod.kids[static_cast<uint64_t>(node.first + i)], &args[i])) return false;
      Type ct = node.type;
      if (ct.base == Base::Void && node.count > 0) {   // WGSL vec3(1.0, 2.0, 3.0), array(a, b): the element type comes from the arguments
        if (ct.array == 0xFFFFFFFFu) {
          ct = args[0].t;
          ct.array = static_cast<uint32_t>(node.count);
          if (args[0].t.array) return fail("array of arrays needs a type");
        } else {
          Base b = args[0].t.base;
          for (int32_t i = 1; i < node.count; ++i) b = (b == Base::Float || args[i].t.base == Base::Float) ? Base::Float : (b == Base::Uint || args[i].t.base == Base::Uint ? Base::Uint : b);
          ct.base = b;
        }
      } else if (ct.base == Base::Void) {
        ct.base = Base::Float;
      }
      return construct(ct, args, static_cast<uint32_t>(node.count), out);
    }
    case NodeKind::Cast: {
      Ref a;
      return eval(node.a, &a) && construct(node.type, &a, 1, out);
    }
    case NodeKind::Bitcast: {
      Ref a;
      uint32_t* w;
      if (!eval(node.a, &a) || !new_value(node.type, out, &w)) return false;
      const uint32_t n = slots(node.type);
      for (uint32_t i = 0; i < n && i < slots(a.t); ++i) w[i] = a.p[i];
      return true;
    }
    case NodeKind::Call: return call_user(node.ival, node, out);
    case NodeKind::Builtin: return call_builtin(node, out);
    case NodeKind::TexCall: return tex_call(node, out);
    default: return fail("bad expression");
  }
}

// --- Interp: calls ----------------------------------------------------------------------------------------------------------------------------------------

bool Interp::push_frame(int32_t node) {
  if (m.frame_top_ >= Machine::kMaxFrames) return fail("shader nesting too deep");
  m.frames_[m.frame_top_++] = Machine::Frame{node, 0, 0, m.top_, 0};
  return true;
}

bool Interp::call_user(int32_t fi, const Node& node, Ref* out) {
  if (fi < 0 || static_cast<uint64_t>(fi) >= mod.functions.count()) return fail("unknown function");
  const Function& F = *mod.functions[static_cast<uint64_t>(fi)];
  if (F.body < 0) return fail("function has no body");
  if (static_cast<uint64_t>(node.count) != F.params.count()) return fail("wrong number of arguments");
  if (m.depth_ > 64) return fail("function calls nested too deep");
  const uint32_t np = static_cast<uint32_t>(F.params.count());
  Ref vals[16];
  LRef places[16];
  if (np > 16) return fail("too many parameters");
  for (uint32_t i = 0; i < np; ++i) {
    const int32_t arg = mod.kids[static_cast<uint64_t>(node.first) + i];
    if (F.params[i].dir == 0) {
      if (!eval(arg, &vals[i])) return false;
    } else {
      if (!lvalue(arg, &places[i])) return false;
      if (F.params[i].dir == 2 && !eval(arg, &vals[i])) return false;
    }
  }
  // the result goes below the callee's temporaries
  const Type rt = F.ret;
  uint32_t* retp = alloc(slots(rt));
  if (!retp && slots(rt)) return false;
  const uint32_t mark = m.top_;
  if (m.var_top_ + F.locals > Machine::kMaxVars) return fail("too many shader variables");
  const uint32_t new_base = m.var_top_;
  m.var_top_ += F.locals;
  for (uint32_t i = 0; i < F.locals; ++i) m.vars_[new_base + i] = Machine::Var{nullptr, Type()};
  for (uint32_t i = 0; i < np; ++i) {
    const Type pt = F.params[i].type;
    uint32_t* p = alloc(slots(pt));
    if (!p && slots(pt)) {
      m.var_top_ = new_base;
      return false;
    }
    if (F.params[i].dir != 1) {
      LRef l;
      l.p = p;
      l.t = pt;
      store(l, vals[i]);
    }
    m.vars_[new_base + i] = Machine::Var{p, pt};
  }
  const uint32_t saved_base = m.var_base_;
  m.var_base_ = new_base;
  ++m.depth_;
  const uint32_t boundary = m.frame_top_;
  bool ok = true;
  if (m.frame_top_ + 2 > Machine::kMaxFrames) ok = fail("shader nesting too deep");
  Machine::Status st = Machine::Status::Done;
  if (ok) {
    m.frames_[m.frame_top_++] = Machine::Frame{-1, -1, fi, m.top_, static_cast<uint32_t>(retp - m.arena_)};
    ok = push_frame(F.body);
    if (ok) st = run(boundary);
  }
  --m.depth_;
  m.frame_top_ = boundary;
  m.var_base_ = saved_base;
  if (!ok || st == Machine::Status::Failed || st == Machine::Status::Discarded) {
    m.var_top_ = new_base;
    if (st == Machine::Status::Discarded) m.discarded_ = true;
    return false;
  }
  for (uint32_t i = 0; i < np; ++i) {
    if (F.params[i].dir == 0) continue;
    Ref pv;
    pv.p = m.vars_[new_base + i].p;
    pv.t = m.vars_[new_base + i].t;
    store(places[i], pv);
  }
  m.var_top_ = new_base;
  m.top_ = mark;
  out->p = retp;
  out->t = rt;
  return true;
}

bool Interp::call_builtin(const Node& node, Ref* out) {
  const BuiltinFn fn = static_cast<BuiltinFn>(node.ival);
  Ref args[12];
  uint32_t* places[12] = {};
  if (node.count > 12) return fail("too many arguments");
  for (int32_t i = 0; i < node.count; ++i) {
    const int32_t arg = mod.kids[static_cast<uint64_t>(node.first + i)];
    const bool pointer = (i == 0 && fn >= BuiltinFn::AtomicLoad && fn <= BuiltinFn::AtomicCompareExchange) || (fn == BuiltinFn::Modf && i == 1) ||
                         (fn == BuiltinFn::Frexp && i == 1) || ((fn == BuiltinFn::UaddCarry || fn == BuiltinFn::UsubBorrow) && i >= 2);
    if (pointer) {   // the place itself
      LRef l;
      if (!lvalue(arg, &l)) return false;
      args[i].p = l.p;
      args[i].t = l.t;
      places[i] = l.p;
    } else if (!eval(arg, &args[i])) {
      return false;
    }
  }
  for (int32_t i = 0; i < 12; ++i) wr[i] = places[i];
  return builtin(fn, args, static_cast<uint32_t>(node.count), out, node);
}

// --- Interp: textures -------------------------------------------------------------------------------------------------------------------------------------

bool Interp::tex_call(const Node& node, Ref* out) {
  if (!m.host_) return fail("no texture host");
  const TexOp op = static_cast<TexOp>(node.op);
  Ref kid[9];
  bool have[9];
  for (int i = 0; i < 9; ++i) have[i] = false;
  for (int32_t i = 0; i < node.count && i < 9; ++i) {
    const int32_t arg = mod.kids[static_cast<uint64_t>(node.first + i)];
    if (arg < 0) continue;
    if (!eval(arg, &kid[i])) return false;
    have[i] = true;
  }
  if (!have[0]) return fail("texture call without a texture");
  const Type tt = kid[0].t;
  SampleRequest rq;
  rq.texture = kid[0].p[0];
  if (have[1]) rq.sampler = kid[1].p[0];
  rq.dim = tt.dim;
  rq.shadow = tt.shadow;
  rq.texel = tt.texel;
  const bool wgsl = (node.flags & 1) != 0;
  auto fcomp = [&](const Ref& r, uint32_t i) -> float {
    return r.t.base == Base::Float ? u2f(r.p[i]) : (r.t.base == Base::Int ? static_cast<float>(u2i(r.p[i])) : static_cast<float>(r.p[i]));
  };
  auto icomp = [&](const Ref& r, uint32_t i) -> int32_t {
    return r.t.base == Base::Float ? static_cast<int32_t>(u2f(r.p[i])) : u2i(r.p[i]);
  };
  uint32_t dims;   // spatial components of the coordinate
  switch (tt.dim) {
    case kDim1D: dims = 1; break;
    case kDim3D: case kDimCube: case kDimCubeArray: dims = 3; break;
    default: dims = 2; break;
  }
  const bool layered = tt.dim == kDim2DArray || tt.dim == kDimCubeArray;
  if (op == TexOp::Size || op == TexOp::Levels || op == TexOp::Samples) {
    rq.mode = op == TexOp::Size ? SampleMode::Size : (op == TexOp::Levels ? SampleMode::Levels : SampleMode::Samples);
    if (have[4]) rq.level = icomp(kid[4], 0);
    uint32_t raw[4];
    m.host_->sample(rq, raw);
    Type rt = op == TexOp::Size ? make_type(Base::Int, static_cast<uint8_t>(dims + (layered && tt.dim != kDimCubeArray ? 1 : 0))) : make_type(Base::Int);
    if (tt.dim == kDimCube) rt = make_type(Base::Int, 2);
    if (wgsl) rt.base = Base::Uint;
    if (wgsl && op == TexOp::Size && rt.rows == 1) rt = make_type(Base::Uint);
    uint32_t* w;
    if (!new_value(rt, out, &w)) return false;
    for (uint32_t i = 0; i < rt.rows; ++i) w[i] = raw[i];
    return true;
  }
  if (!have[2]) return fail("texture call without coordinates");
  const Ref& co = kid[2];
  if (op == TexOp::Fetch || op == TexOp::Store) {
    rq.mode = op == TexOp::Store ? SampleMode::Store : SampleMode::Fetch;
    for (uint32_t i = 0; i < 3 && i < count_of(co.t); ++i) rq.icoord[i] = icomp(co, i);
    if (wgsl && have[3]) rq.icoord[dims] = icomp(kid[3], 0);   // array index
    if (have[4]) rq.level = icomp(kid[4], 0);
    if (have[7]) for (uint32_t i = 0; i < 3 && i < count_of(kid[7].t); ++i) rq.offset[i] = icomp(kid[7], i);
    if (op == TexOp::Store) {
      if (!have[5]) return fail("textureStore needs a value");
      for (uint32_t i = 0; i < 4 && i < count_of(kid[5].t); ++i) rq.value[i] = kid[5].t.base == tt.texel || tt.texel != Base::Float ? kid[5].p[i] : kid[5].p[i];
      uint32_t ignored[4];
      m.host_->sample(rq, ignored);
      uint32_t* wv;
      return new_value(make_type(Base::Void), out, &wv);
    }
    uint32_t raw[4];
    m.host_->sample(rq, raw);
    uint32_t* w;
    if (!new_value(make_type(tt.shadow ? Base::Float : tt.texel, tt.shadow ? 1 : 4), out, &w)) return false;
    for (uint32_t i = 0; i < out->t.rows; ++i) w[i] = raw[i];
    return true;
  }
  float c[4] = {0, 0, 0, 0};
  const uint32_t have_c = count_of(co.t);
  for (uint32_t i = 0; i < 4 && i < have_c; ++i) c[i] = fcomp(co, i);
  float layer = 0, ref = 0;
  uint32_t used = dims;
  if (layered) {
    if (wgsl) layer = have[3] ? fcomp(kid[3], 0) : 0;
    else {
      layer = c[dims];
      used = dims + 1;
    }
  }
  const bool proj = op == TexOp::Proj || op == TexOp::ProjBias || op == TexOp::ProjLod;
  if (tt.shadow && !wgsl) ref = c[used];
  if (proj) {
    const float q = c[have_c - 1];
    for (uint32_t i = 0; i < dims; ++i) c[i] /= q;
    if (tt.shadow) ref /= q;
  }
  rq.layer = layer;
  rq.reference = ref;
  for (uint32_t i = 0; i < 3; ++i) rq.coord[i] = c[i];
  if (tt.dim == kDim2DArray) rq.coord[2] = layer;
  if (have[7]) for (uint32_t i = 0; i < 3 && i < count_of(kid[7].t); ++i) rq.offset[i] = icomp(kid[7], i);
  switch (op) {
    case TexOp::Sample: case TexOp::Proj: rq.mode = SampleMode::Implicit; break;
    case TexOp::Bias: case TexOp::ProjBias: rq.mode = SampleMode::Bias; rq.bias = have[4] ? fcomp(kid[4], 0) : 0; break;
    case TexOp::Lod: case TexOp::ProjLod: rq.mode = SampleMode::Lod; rq.lod = have[4] ? fcomp(kid[4], 0) : 0; break;
    case TexOp::Grad:
      rq.mode = SampleMode::Grad;
      rq.has_derivatives = true;
      for (uint32_t i = 0; i < 3; ++i) {
        rq.ddx[i] = have[5] && i < count_of(kid[5].t) ? fcomp(kid[5], i) : 0;
        rq.ddy[i] = have[6] && i < count_of(kid[6].t) ? fcomp(kid[6], i) : 0;
      }
      break;
    case TexOp::Compare: rq.mode = SampleMode::Implicit; rq.compare = true; if (wgsl) rq.reference = have[4] ? fcomp(kid[4], 0) : 0; break;
    case TexOp::CompareLevel: rq.mode = SampleMode::Lod; rq.compare = true; rq.lod = 0; if (wgsl) rq.reference = have[4] ? fcomp(kid[4], 0) : 0; break;
    case TexOp::Gather: case TexOp::GatherCompare:
      rq.mode = SampleMode::Gather;
      rq.component = have[8] ? icomp(kid[8], 0) : 0;
      if (op == TexOp::GatherCompare) { rq.compare = true; if (wgsl) rq.reference = have[4] ? fcomp(kid[4], 0) : 0; }
      break;
    default: break;
  }
  if (tt.shadow && !wgsl) rq.compare = true;
  // implicit derivatives: from the neighbouring pixels' recorded coordinates
  if (rq.mode == SampleMode::Implicit || rq.mode == SampleMode::Bias) {
    if (m.record_) {
      for (uint32_t i = 0; i < dims; ++i) m.record_->values.push_back(c[i]);
      m.site_ += dims;
    } else if (m.replay_x_ && m.replay_y_) {
      const uint32_t s = m.site_;
      if (s + dims <= m.replay_x_->values.count() && s + dims <= m.replay_y_->values.count()) {
        for (uint32_t i = 0; i < dims; ++i) {
          rq.ddx[i] = m.replay_x_->values[s + i] - c[i];
          rq.ddy[i] = m.replay_y_->values[s + i] - c[i];
        }
        rq.has_derivatives = true;
      }
      m.site_ += dims;
    }
  }
  uint32_t raw[4];
  m.host_->sample(rq, raw);
  const bool depth_result = tt.shadow && rq.mode != SampleMode::Gather;
  uint32_t* w;
  if (depth_result) {
    if (!new_value(make_type(Base::Float), out, &w)) return false;
    w[0] = raw[0];
    return true;
  }
  if (!new_value(make_type(tt.texel, 4), out, &w)) return false;
  for (uint32_t i = 0; i < 4; ++i) w[i] = raw[i];
  return true;
}

// --- Statements -------------------------------------------------------------------------------------------------------------------------------------------

// Pops frames for `break` (to the loop or switch, popping it too) or `continue` (to the loop, which stays and goes on at its step).
bool Interp::unwind(bool is_continue) {
  uint32_t lowest = m.top_;
  while (m.frame_top_ > 0) {
    const Machine::Frame& f = m.frames_[m.frame_top_ - 1];
    if (f.node < 0) return fail("break or continue outside a loop");
    const Node& n = mod.nodes[static_cast<uint64_t>(f.node)];
    const bool loop = n.kind == NodeKind::For || n.kind == NodeKind::While || n.kind == NodeKind::DoWhile || n.kind == NodeKind::Loop;
    if (loop || (!is_continue && n.kind == NodeKind::Switch)) {
      if (is_continue) {
        Machine::Frame& lf = m.frames_[m.frame_top_ - 1];
        switch (n.kind) {
          case NodeKind::For: lf.state = 2; break;
          case NodeKind::While: lf.state = 0; break;
          case NodeKind::DoWhile: lf.state = 1; break;
          case NodeKind::Loop: lf.state = 1; break;
          default: break;
        }
        m.top_ = lowest;
      } else {
        m.top_ = f.mark;
        --m.frame_top_;
      }
      return true;
    }
    lowest = f.mark;
    --m.frame_top_;
  }
  return fail("break or continue outside a loop");
}

Machine::Status Interp::exec_step(uint32_t base) {
  using S = Machine::Status;
  Machine::Frame& f = m.frames_[m.frame_top_ - 1];
  const uint32_t fi = m.frame_top_ - 1;
  if (f.node < 0) {   // the end of a function that was called without recursing: go back to the caller
    m.var_top_ = f.saved_top;
    m.var_base_ = f.saved_base;
    m.top_ = f.mark;
    --m.frame_top_;
    return S::Done;
  }
  const Node& node = mod.nodes[static_cast<uint64_t>(f.node)];
  auto pop = [&]() { --m.frame_top_; };
  switch (node.kind) {
    case NodeKind::Empty: pop(); return S::Done;
    case NodeKind::Block: {
      if (f.index < node.count) {
        const int32_t child = mod.kids[static_cast<uint64_t>(node.first + f.index)];
        ++m.frames_[fi].index;
        return push_frame(child) ? S::Done : S::Failed;
      }
      if (!(node.flags & 1)) m.top_ = f.mark;
      pop();
      return S::Done;
    }
    case NodeKind::Decl: {
      const uint32_t mark = m.top_;
      Type t = node.type;
      const uint32_t slot = static_cast<uint32_t>(node.ival);
      if (t.base != Base::Void) {
        const uint32_t n = slots(t);
        uint32_t* p = alloc(n);
        if (!p && n) return S::Failed;
        if (node.a >= 0) {
          Ref r;
          if (!eval(node.a, &r)) return m.discarded_ ? S::Discarded : S::Failed;
          LRef l;
          l.p = p;
          l.t = t;
          store(l, r);
        }
        m.top_ = mark + n;
        m.vars_[m.var_base_ + slot] = Machine::Var{p, t};
      } else {   // the type of the initializer
        Ref r;
        if (node.a < 0 || !eval(node.a, &r)) return m.discarded_ ? S::Discarded : (fail("cannot tell the type of a variable"), S::Failed);
        t = r.t;
        const uint32_t n = slots(t);
        uint32_t* dst = m.arena_ + mark;
        if (r.p != dst) __builtin_memmove(dst, r.p, uint64_t{n} * 4);
        m.top_ = mark + n;
        m.vars_[m.var_base_ + slot] = Machine::Var{dst, t};
      }
      pop();
      return S::Done;
    }
    case NodeKind::ExprStmt: {
      if (node.a >= 0) {
        const Node& e = mod.nodes[static_cast<uint64_t>(node.a)];
        if (e.kind == NodeKind::Builtin && static_cast<BuiltinFn>(e.ival) == BuiltinFn::Barrier && m.depth_ == 0 && base == 0) {
          pop();
          return S::Yield;
        }
        // A function with a barrier in it is entered by pushing its frames, so that the invocation can stop at the barrier with the call still open.
        if (e.kind == NodeKind::Call && m.depth_ == 0 && base == 0 && e.ival >= 0 && static_cast<uint64_t>(e.ival) < mod.functions.count() &&
            mod.functions[static_cast<uint64_t>(e.ival)]->has_barrier) {
          const Function& F = *mod.functions[static_cast<uint64_t>(e.ival)];
          bool simple = static_cast<uint64_t>(e.count) == F.params.count() && F.body >= 0 && F.params.count() <= 16;
          for (uint64_t i = 0; i < F.params.count(); ++i) simple = simple && F.params[i].dir == 0;
          if (simple) {
            const uint32_t mark = m.top_;
            Ref vals[16];
            for (uint32_t i = 0; i < F.params.count(); ++i)
              if (!eval(mod.kids[static_cast<uint64_t>(e.first) + i], &vals[i])) return m.discarded_ ? S::Discarded : S::Failed;
            uint32_t* retp = alloc(slots(F.ret));
            if (!retp && slots(F.ret)) return S::Failed;
            if (m.var_top_ + F.locals > Machine::kMaxVars || m.frame_top_ + 2 > Machine::kMaxFrames) return fail("shader nesting too deep"), S::Failed;
            const uint32_t new_base = m.var_top_;
            for (uint32_t i = 0; i < F.locals; ++i) m.vars_[new_base + i] = Machine::Var{nullptr, Type()};
            for (uint32_t i = 0; i < F.params.count(); ++i) {
              const Type pt = F.params[i].type;
              uint32_t* p = alloc(slots(pt));
              if (!p && slots(pt)) return S::Failed;
              LRef l;
              l.p = p;
              l.t = pt;
              store(l, vals[i]);
              m.vars_[new_base + i] = Machine::Var{p, pt};
            }
            Machine::Frame bf{-1, -1, e.ival, mark, static_cast<uint32_t>(retp - m.arena_)};
            bf.saved_base = m.var_base_;
            bf.saved_top = m.var_top_;
            m.var_top_ += F.locals;
            m.var_base_ = new_base;
            pop();   // the statement is over once the call returns
            m.frames_[m.frame_top_++] = bf;
            return push_frame(F.body) ? S::Done : S::Failed;
          }
        }
        const uint32_t mark = m.top_;
        Ref r;
        if (!eval(node.a, &r)) return m.discarded_ ? S::Discarded : S::Failed;
        m.top_ = mark;
      }
      pop();
      return S::Done;
    }
    case NodeKind::If: {
      const uint32_t mark = m.top_;
      Ref c;
      if (!eval(node.a, &c)) return m.discarded_ ? S::Discarded : S::Failed;
      m.top_ = mark;
      const int32_t child = c.p[0] != 0 ? node.b : node.c;
      pop();
      if (child >= 0) return push_frame(child) ? S::Done : S::Failed;
      return S::Done;
    }
    case NodeKind::For: {
      switch (f.state) {
        case 0:
          f.state = 1;
          if (node.a >= 0) return push_frame(node.a) ? S::Done : S::Failed;
          return S::Done;
        case 1: {
          bool go = true;
          if (node.b >= 0) {
            const uint32_t mark = m.top_;
            Ref c;
            if (!eval(node.b, &c)) return m.discarded_ ? S::Discarded : S::Failed;
            go = c.p[0] != 0;
            m.top_ = mark;
          }
          if (!go) {
            m.top_ = f.mark;
            pop();
            return S::Done;
          }
          f.state = 2;
          return push_frame(node.d) ? S::Done : S::Failed;
        }
        default: {
          if (node.c >= 0) {
            const uint32_t mark = m.top_;
            Ref r;
            if (!eval(node.c, &r)) return m.discarded_ ? S::Discarded : S::Failed;
            m.top_ = mark;
          }
          f.state = 1;
          return S::Done;
        }
      }
    }
    case NodeKind::While: {
      const uint32_t mark = m.top_;
      Ref c;
      if (!eval(node.a, &c)) return m.discarded_ ? S::Discarded : S::Failed;
      const bool go = c.p[0] != 0;
      m.top_ = mark;
      if (!go) {
        pop();
        return S::Done;
      }
      return push_frame(node.b) ? S::Done : S::Failed;
    }
    case NodeKind::DoWhile: {
      if (f.state == 0) {
        f.state = 1;
        return push_frame(node.a) ? S::Done : S::Failed;
      }
      const uint32_t mark = m.top_;
      Ref c;
      if (!eval(node.b, &c)) return m.discarded_ ? S::Discarded : S::Failed;
      const bool go = c.p[0] != 0;
      m.top_ = mark;
      if (!go) {
        pop();
        return S::Done;
      }
      f.state = 0;
      return S::Done;
    }
    case NodeKind::Loop: {
      if (f.state == 0) {
        f.state = 1;
        return push_frame(node.a) ? S::Done : S::Failed;
      }
      if (f.state == 1 && node.b >= 0) {
        f.state = 2;
        return push_frame(node.b) ? S::Done : S::Failed;
      }
      f.state = 0;
      return S::Done;
    }
    case NodeKind::Switch: {
      if (f.state == 0) {
        const uint32_t mark = m.top_;
        Ref sel;
        if (!eval(node.a, &sel)) return m.discarded_ ? S::Discarded : S::Failed;
        const uint32_t v = sel.p[0];
        int32_t found = -1, def = -1;
        for (int32_t i = 0; i < node.count && found < 0; ++i) {
          const Node& cs = mod.nodes[static_cast<uint64_t>(mod.kids[static_cast<uint64_t>(node.first + i)])];
          if (cs.ival == 1) def = i;
          for (int32_t j = 0; j < cs.count; ++j) {
            Ref cv;
            if (!eval(mod.kids[static_cast<uint64_t>(cs.first + j)], &cv)) return S::Failed;
            if (convert_scalar(cv.p[0], cv.t.base, sel.t.base) == v) {
              found = i;
              break;
            }
          }
        }
        m.top_ = mark;
        if (found < 0) found = def;
        if (found < 0) {
          pop();
          return S::Done;
        }
        f.state = 1;
        f.index = found;
        const Node& cs = mod.nodes[static_cast<uint64_t>(mod.kids[static_cast<uint64_t>(node.first + found)])];
        if (cs.d >= 0) return push_frame(cs.d) ? S::Done : S::Failed;
        return S::Done;
      }
      // after a case body: GLSL falls through, WGSL leaves
      if ((node.flags & 1) && f.index + 1 < node.count) {
        ++f.index;
        const Node& cs = mod.nodes[static_cast<uint64_t>(mod.kids[static_cast<uint64_t>(node.first + f.index)])];
        if (cs.d >= 0) return push_frame(cs.d) ? S::Done : S::Failed;
        return S::Done;
      }
      m.top_ = f.mark;
      pop();
      return S::Done;
    }
    case NodeKind::BreakIf: {
      const uint32_t mark = m.top_;
      Ref c;
      if (!eval(node.a, &c)) return m.discarded_ ? S::Discarded : S::Failed;
      m.top_ = mark;
      if (c.p[0] == 0) {
        pop();
        return S::Done;
      }
      return unwind(false) ? S::Done : S::Failed;
    }
    case NodeKind::Break: return unwind(false) ? S::Done : S::Failed;
    case NodeKind::Continue: return unwind(true) ? S::Done : S::Failed;
    case NodeKind::Discard:
      m.discarded_ = true;
      return S::Discarded;
    case NodeKind::Return: {
      // find the boundary of the function
      int32_t b = static_cast<int32_t>(m.frame_top_) - 1;
      while (b >= 0 && m.frames_[b].node >= 0) --b;
      if (b < 0) return fail("return outside a function"), S::Failed;
      const Machine::Frame bf = m.frames_[b];
      if (node.a >= 0) {
        const uint32_t mark = m.top_;
        Ref r;
        if (!eval(node.a, &r)) return m.discarded_ ? S::Discarded : S::Failed;
        const Function& F = *mod.functions[static_cast<uint64_t>(bf.index)];
        LRef l;
        l.p = m.arena_ + bf.aux;
        l.t = F.ret;
        store(l, r);
        m.top_ = mark;
      }
      m.frame_top_ = static_cast<uint32_t>(b) + 1;
      return S::Done;
    }
    default: return fail("bad statement"), S::Failed;
  }
}

Machine::Status Interp::run(uint32_t base) {
  using S = Machine::Status;
  while (m.frame_top_ > base + 1) {
    if (++m.steps_ > m.limit_) {
      fail("shader ran too long");
      return S::Failed;
    }
    const S s = exec_step(base);
    if (s != S::Done) return s;
  }
  return S::Done;
}

Machine::Status Machine::start(int32_t index, ArrayView<const uint32_t* const> args) {
  failure_ = nullptr;
  discarded_ = false;
  result_ = nullptr;
  if (!valid_ || index < 0 || static_cast<uint64_t>(index) >= module_.functions.count()) return Status::Failed;
  const Function& F = *module_.functions[static_cast<uint64_t>(index)];
  Interp in(*this);
  top_ = 0;
  var_top_ = 0;
  frame_top_ = 0;
  var_base_ = 0;
  depth_ = 0;
  steps_ = 0;
  site_ = 0;
  if (F.locals > kMaxVars) return Status::Failed;
  var_top_ = F.locals;
  for (uint32_t i = 0; i < F.locals; ++i) vars_[i] = Var{nullptr, Type()};
  const Type rt = F.ret;
  uint32_t* retp = in.alloc(module_.slots_of(rt));
  if (!retp) return Status::Failed;
  const uint32_t np = static_cast<uint32_t>(F.params.count());
  for (uint32_t i = 0; i < np; ++i) {
    const Type pt = F.params[i].type;
    uint32_t* p = in.alloc(module_.slots_of(pt));
    if (!p && module_.slots_of(pt)) return Status::Failed;
    if (i < args.size() && args[i]) {
      const uint32_t n = module_.slots_of(pt);
      for (uint32_t j = 0; j < n; ++j) p[j] = args[i][j];
    }
    vars_[i] = Var{p, pt};
  }
  result_type_ = rt;
  result_ = retp;
  frames_[0] = Frame{-1, -1, index, top_, static_cast<uint32_t>(retp - arena_)};
  frame_top_ = 1;
  if (F.body >= 0 && !in.push_frame(F.body)) return Status::Failed;
  return resume();
}

Machine::Status Machine::resume() {
  Interp in(*this);
  const Status s = in.run(0);
  if (s == Status::Done) frame_top_ = 0;
  if (discarded_ && s != Status::Failed) return Status::Discarded;
  return s;
}

}  // namespace shader
