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

#include "WebGl/TgsiTranslator.hpp"

namespace webgl {
namespace {

using namespace shader;

constexpr uint32_t kMaxTemps = 96;
constexpr uint32_t kMaxImms = 64;
const char* const kChan[4] = {"x", "y", "z", "w"};

// A TGSI source operand: a register file and index, a swizzle, and a negate flag. 'T' temporary, 'I' input, 'C' constant, 'M' immediate.
struct Oper {
  char file = 'T';
  uint16_t idx = 0;
  uint8_t swz[4] = {0, 1, 2, 3};
  bool neg = false;
};

// A value of the shader: a scalar, vector (cols == 1) or matrix, one operand per column. An integer constant is only usable as an index or under a cast.
struct Val {
  uint8_t cols = 1, rows = 1;
  Oper c[4];
  bool is_int = false;
  int32_t ival = 0;
  uint8_t kind = 0;    // 1: a structure (WGSL): its members are members_[first ...], of the structure type `sid`
  uint16_t first = 0, sid = 0;
};

// A variable: one temporary per column.
struct Var {
  bool valid = false;
  uint8_t cols = 1, rows = 1;
  uint16_t temp[4] = {0, 0, 0, 0};
  bool is_struct = false;   // a structure: members_[first ...]
  uint16_t first = 0, sid = 0;
};

// A place that can be assigned to: channels of temporaries.
struct LV {
  uint8_t cols = 1, rows = 1;
  uint16_t temp[4] = {0, 0, 0, 0};
  uint8_t chan[4] = {0, 1, 2, 3};
  uint8_t n = 1;   // vector: how many channels
  bool agg = false;   // a whole structure: members_[first ...]
  uint16_t first = 0, sid = 0;
};

struct Imm {
  float v[4];
};

struct UniformBase {
  uint32_t global;
  uint32_t row;
};

// What both stages share while they are translated.
struct Shared {
  explicit Shared(TgsiProgram* p, ArrayView<const TgsiLink> l) : out(p), links(l) {}
  TgsiProgram* out;
  ArrayView<const TgsiLink> links;
  const char* why = nullptr;
  // WGSL: the locations of the varyings the vertex shader wrote, in the order of the device's GENERIC registers
  uint32_t var_loc[kTgsiMaxVaryings] = {};
  uint32_t var_rows[kTgsiMaxVaryings] = {};
  uint32_t var_count = 0;
};

Oper filled(char file, uint16_t idx, uint32_t rows) {
  Oper o;
  o.file = file;
  o.idx = idx;
  for (uint32_t k = 0; k < 4; ++k) o.swz[k] = static_cast<uint8_t>(k < rows ? k : rows - 1);
  return o;
}

Oper component(const Oper& o, uint32_t j) {
  Oper r = o;
  const uint8_t s = o.swz[j & 3];
  for (uint32_t k = 0; k < 4; ++k) r.swz[k] = s;
  return r;
}

const char* mask_of(uint32_t rows) {
  static const char* const kMasks[] = {"x", "x", "xy", "xyz", "xyzw"};
  return kMasks[rows > 4 ? 4 : rows];
}

// A float as TGSI text: decimal, twelve fractional digits (a float32 is exact in a double, and this is enough digits for the small ones).
bool put_float(String& s, float f) {
  if (!(f == f) || f > 1e9f || f < -1e9f) return false;
  double d = static_cast<double>(f);
  if (d < 0) {
    s.push_back('-');
    d = -d;
  }
  uint64_t ip = static_cast<uint64_t>(d);
  double frac = d - static_cast<double>(ip);
  uint64_t fd = static_cast<uint64_t>(frac * 1e12 + 0.5);
  if (fd >= 1000000000000ull) {
    fd -= 1000000000000ull;
    ++ip;
  }
  s.append_uint(ip);
  s.push_back('.');
  char digits[12];
  for (int i = 11; i >= 0; --i) {
    digits[i] = static_cast<char>('0' + fd % 10);
    fd /= 10;
  }
  return s.append(StringView(digits, 12));
}

class Gen {
 public:
  Gen(Shared& sh, const Module& m, bool fragment, int32_t entry = -1)
      : body(sh.out->vs.allocator()), sh_(sh), m_(m), fs_(fragment), entry_(entry), locals_(sh.out->vs.allocator()), globals_(sh.out->vs.allocator()),
        imms_(sh.out->vs.allocator()), ubase_(sh.out->vs.allocator()), members_(sh.out->vs.allocator()) {}

  String body;
  uint32_t ntemp() const { return ntemp_; }
  uint32_t instructions() const { return inst_; }
  const List<Imm>& imms() const { return imms_; }

  // Translates main() and writes the outputs of the stage.
  bool run();

 private:
  bool fail(const char* why) {
    if (!sh_.why) sh_.why = why;
    return false;
  }

  // --- text
  void put_oper(const Oper& o) {
    if (o.neg) body.push_back('-');
    switch (o.file) {
      case 'T': body.append("TEMP"); break;
      case 'I': body.append("IN"); break;
      case 'C': body.append("CONST"); break;
      default: body.append("IMM"); break;
    }
    body.push_back('[');
    body.append_uint(o.idx);
    body.append("].");
    for (uint32_t k = 0; k < 4; ++k) body.push_back("xyzw"[o.swz[k] & 3]);
  }
  void begin_inst(const char* op) {
    body.append("  ");
    body.append_uint(inst_++);
    body.append(": ");
    body.append(op);
  }
  void end_inst() { body.push_back('\n'); }
  void inst(const char* op, uint16_t dst, const char* mask, const Oper* s0, const Oper* s1 = nullptr, const Oper* s2 = nullptr) {
    begin_inst(op);
    body.append(" TEMP[");
    body.append_uint(dst);
    body.append("].");
    body.append(mask);
    const Oper* src[3] = {s0, s1, s2};
    for (const Oper* s : src) {
      if (!s) break;
      body.append(", ");
      put_oper(*s);
    }
    end_inst();
  }
  void inst_rows(const char* op, uint16_t dst, uint32_t rows, const Oper* s0, const Oper* s1 = nullptr, const Oper* s2 = nullptr) { inst(op, dst, mask_of(rows), s0, s1, s2); }
  void out_mov(uint32_t out, const char* mask, const Oper& src) {
    begin_inst("MOV");
    body.append(" OUT[");
    body.append_uint(out);
    body.append("].");
    body.append(mask);
    body.append(", ");
    put_oper(src);
    end_inst();
  }

  // --- registers and values
  bool new_temp(uint16_t* t) {
    if (ntemp_ >= kMaxTemps) return fail("too many temporaries");
    *t = static_cast<uint16_t>(ntemp_++);
    return true;
  }
  bool new_val(uint32_t rows, uint32_t cols, Val* v) {
    v->rows = static_cast<uint8_t>(rows);
    v->cols = static_cast<uint8_t>(cols);
    v->is_int = false;
    for (uint32_t c = 0; c < cols; ++c) {
      uint16_t t;
      if (!new_temp(&t)) return false;
      v->c[c] = filled('T', t, rows);
    }
    return true;
  }
  bool imm(const float v[4], uint32_t rows, Oper* o) {
    for (uint64_t i = 0; i < imms_.count(); ++i) {
      bool same = true;
      for (uint32_t k = 0; k < 4; ++k) same = same && imms_[i].v[k] == v[k];
      if (same) {
        *o = filled('M', static_cast<uint16_t>(i), rows);
        return true;
      }
    }
    if (imms_.count() >= kMaxImms) return fail("too many constants");
    Imm im;
    String probe(body.allocator());
    for (uint32_t k = 0; k < 4; ++k) {
      if (!put_float(probe, v[k])) return fail("constant out of range");
      im.v[k] = v[k];
    }
    if (!imms_.push_back(im)) return fail("out of memory");
    *o = filled('M', static_cast<uint16_t>(imms_.count() - 1), rows);
    return true;
  }
  bool scalar_imm(float x, Oper* o) {
    const float v[4] = {x, 0, 0, 0};
    return imm(v, 1, o);
  }
  static bool is_float_type(const Type& t) {
    return t.array == 0 && (t.base == Base::Float || t.base == Base::Bool) && t.rows >= 1 && t.rows <= 4 && t.cols >= 1 && t.cols <= 4;
  }
  static void val_of(const Var& v, Val* out) {
    if (v.is_struct) {
      *out = Val();
      out->kind = 1;
      out->first = v.first;
      out->sid = v.sid;
      return;
    }
    out->kind = 0;
    out->rows = v.rows;
    out->cols = v.cols;
    out->is_int = false;
    for (uint32_t c = 0; c < v.cols; ++c) out->c[c] = filled('T', v.temp[c], v.rows);
  }
  static void lv_of(const Var& v, LV* l) {
    l->agg = v.is_struct;
    l->first = v.first;
    l->sid = v.sid;
    l->rows = v.rows;
    l->cols = v.cols;
    l->n = v.rows;
    for (uint32_t k = 0; k < 4; ++k) {
      l->temp[k] = v.temp[k];
      l->chan[k] = static_cast<uint8_t>(k);
    }
  }
  bool make_var(const Type& t, Var* v) {
    if (t.base == Base::Struct && t.array == 0) {
      Val sv;
      if (!make_struct(t, &sv)) return false;
      v->valid = true;
      v->is_struct = true;
      v->first = sv.first;
      v->sid = sv.sid;
      return true;
    }
    if (!is_float_type(t)) return fail("a variable of a type the device cannot hold");
    v->valid = true;
    v->rows = t.rows;
    v->cols = t.cols;
    for (uint32_t c = 0; c < t.cols; ++c)
      if (!new_temp(&v->temp[c])) return false;
    return true;
  }

  // --- globals
  bool global_var(uint64_t gi, Var* out);
  bool global_val(uint64_t gi, Val* out);
  bool uniform_base(uint64_t gi, uint32_t* row);
  bool attribute_index(const Global& g, uint32_t* index);
  bool attribute_at(uint32_t key, uint32_t comps, uint32_t* index);
  bool alloc_const(uint32_t slot, uint32_t rows, uint32_t cols, uint32_t elems, uint32_t* row);
  bool uniform_struct(uint32_t slot, const Type& t, Val* out);
  bool make_struct(const Type& t, Val* out);
  bool find_field(uint32_t sid, const Node& n, uint32_t* index);
  bool lv_from_val(const Val& v, LV* out);
  bool to_float(Val& v);
  bool run_entry();
  bool fill_input(int32_t location, int32_t builtin, bool flat, const Val& dst);

  // --- expressions
  bool const_val(const Node& n, Val* out);
  bool const_index(int32_t id, int32_t* out);
  bool swizzle_indices(const Node& n, uint32_t* idx, uint32_t* count);
  bool eval(int32_t id, Val* out);
  bool lvalue(int32_t id, LV* out);
  bool read_lv(const LV& l, Val* out);
  bool store(const LV& l, const Val& v);
  bool binary(Op op, const Val& a, const Val& b, Val* out);
  bool elementwise(const char* opc, const Val& a, const Val& b, Val* out);
  bool per_component(const char* opc, const Val& a, const Val* b, Val* out);
  bool mat_vec(const Val& m, const Val& v, Val* out);
  bool dot(const Val& a, const Val& b, Val* out);
  bool pieces_to_vec(const Oper* comps, uint32_t count, uint32_t rows, Val* out);
  bool construct(const Node& n, Val* out);
  bool builtin(const Node& n, Val* out);
  bool call_args(const Node& n, Val* args, uint32_t max, uint32_t* count);

  // --- statements
  bool stmt(int32_t id, bool tail);

  Shared& sh_;
  const Module& m_;
  bool fs_;
  int32_t entry_;   // WGSL: the entry point function; GLSL: -1 (main)
  uint32_t ntemp_ = 0;
  uint32_t inst_ = 0;
  List<Var> locals_;
  List<Var> globals_;
  List<Imm> imms_;
  List<UniformBase> ubase_;
  List<Val> members_;
  Val ret_;
  bool have_ret_ = false;
};

// --- globals -------------------------------------------------------------------------------------------------------------------------------------------------

bool Gen::global_var(uint64_t gi, Var* out) {
  if (gi >= globals_.count()) return fail("bad global");
  Var& v = globals_[gi];
  if (!v.valid && !make_var(m_.globals[gi]->type, &v)) return false;
  *out = v;
  return true;
}

// `elems` x `cols` consecutive CONST rows of `rows` floats each, read from the shared region of this stage from `slot` on.
bool Gen::alloc_const(uint32_t slot, uint32_t rows, uint32_t cols, uint32_t elems, uint32_t* row) {
  if (elems > kTgsiMaxConstRows) return fail("too many uniforms");
  const uint32_t total = elems * cols;
  TgsiProgram& p = *sh_.out;
  if (p.const_rows + total > kTgsiMaxConstRows) return fail("too many uniforms");
  const uint32_t first = p.const_rows;
  for (uint32_t e = 0; e < elems; ++e)
    for (uint32_t c = 0; c < cols; ++c) {
      TgsiConstant k;
      k.stage = fs_ ? 1 : 0;
      k.row = static_cast<uint8_t>(first + e * cols + c);
      k.comps = static_cast<uint8_t>(rows);
      k.slot = slot + (e * cols + c) * rows;
      if (!p.constants.push_back(k)) return fail("out of memory");
    }
  p.const_rows += total;
  *row = first;
  return true;
}

bool Gen::uniform_base(uint64_t gi, uint32_t* row) {
  for (uint64_t i = 0; i < ubase_.count(); ++i)
    if (ubase_[i].global == gi) {
      *row = ubase_[i].row;
      return true;
    }
  const Global& g = *m_.globals[gi];
  const Type& t = g.type;
  if (g.block >= 0) return fail("uniform blocks");
  if (t.base != Base::Float || t.rows < 1 || t.rows > 4 || t.cols < 1 || t.cols > 4) return fail("a uniform of a type the device cannot take");
  uint32_t first;
  if (!alloc_const(g.slot, t.rows, t.cols, t.array ? t.array : 1, &first)) return false;
  if (!ubase_.push_back(UniformBase{static_cast<uint32_t>(gi), first})) return fail("out of memory");
  *row = first;
  return true;
}

// A uniform structure (WGSL): every member gets its CONST rows; the members' slots are the structure's slot plus the field offset.
bool Gen::uniform_struct(uint32_t slot, const Type& t, Val* out) {
  const StructDef* sd = m_.struct_of(t);
  if (!sd || t.array || sd->fields.count() > 16) return fail("a uniform structure the device cannot take");
  const uint32_t n = static_cast<uint32_t>(sd->fields.count());
  const uint32_t first = static_cast<uint32_t>(members_.count());
  for (uint32_t i = 0; i < n; ++i)
    if (!members_.push_back(Val())) return fail("out of memory");
  for (uint32_t i = 0; i < n; ++i) {
    const Type ft = sd->fields[i].type;
    const uint32_t fslot = slot + sd->fields[i].offset;
    Val mv;
    if (ft.base == Base::Struct && ft.array == 0) {
      if (!uniform_struct(fslot, ft, &mv)) return false;
    } else {
      if (ft.base != Base::Float || ft.array || ft.rows < 1 || ft.rows > 4 || ft.cols < 1 || ft.cols > 4) return fail("a uniform member the device cannot take");
      mv.kind = 3;   // the CONST rows are taken when the member is first read (a shader rarely uses every member, and there are only a few rows)
      mv.rows = ft.rows;
      mv.cols = ft.cols;
      mv.ival = static_cast<int32_t>(fslot);
    }
    members_[first + i] = mv;
  }
  *out = Val();
  out->kind = 1;
  out->first = static_cast<uint16_t>(first);
  out->sid = t.sid;
  return true;
}

bool Gen::make_struct(const Type& t, Val* out) {
  const StructDef* sd = m_.struct_of(t);
  if (!sd || t.array || sd->fields.count() > 16) return fail("a structure the device cannot hold");
  const uint32_t n = static_cast<uint32_t>(sd->fields.count());
  const uint32_t first = static_cast<uint32_t>(members_.count());
  for (uint32_t i = 0; i < n; ++i)
    if (!members_.push_back(Val())) return fail("out of memory");
  for (uint32_t i = 0; i < n; ++i) {
    const Type ft = sd->fields[i].type;
    Val mv;
    if (ft.base == Base::Struct && ft.array == 0) {
      if (!make_struct(ft, &mv)) return false;
    } else {
      if (!is_float_type(ft) || !new_val(ft.rows, ft.cols, &mv)) return fail("a member of a type the device cannot hold");
    }
    members_[first + i] = mv;
  }
  *out = Val();
  out->kind = 1;
  out->first = static_cast<uint16_t>(first);
  out->sid = t.sid;
  return true;
}

bool Gen::find_field(uint32_t sid, const Node& n, uint32_t* index) {
  if (sid >= m_.structs.count() || n.c < 1 || n.b < 0 || uint64_t{static_cast<uint32_t>(n.b)} + static_cast<uint32_t>(n.c) > m_.consts.count()) return fail("bad field");
  const StructDef& sd = *m_.structs[sid];
  for (uint64_t i = 0; i < sd.fields.count(); ++i) {
    const String& name = sd.fields[i].name;
    if (name.size() != static_cast<uint64_t>(n.c)) continue;
    bool same = true;
    for (int32_t k = 0; k < n.c && same; ++k) same = static_cast<char>(m_.consts[static_cast<uint64_t>(n.b + k)]) == name[static_cast<uint64_t>(k)];
    if (same) {
      *index = static_cast<uint32_t>(i);
      return true;
    }
  }
  return fail("no such field");
}

bool Gen::lv_from_val(const Val& v, LV* out) {
  if (v.kind == 3 || v.kind == 2) return fail("assignment to a uniform or built-in");
  if (v.kind == 1) {
    *out = LV();
    out->agg = true;
    out->first = v.first;
    out->sid = v.sid;
    return true;
  }
  *out = LV();
  out->cols = v.cols;
  out->rows = v.rows;
  out->n = v.rows;
  for (uint32_t c = 0; c < v.cols; ++c) {
    if (v.c[c].file != 'T') return fail("assignment to something that is not a variable");
    out->temp[c] = v.c[c].idx;
  }
  return true;
}

bool Gen::to_float(Val& v) {
  if (!v.is_int) return true;
  Oper o;
  if (!scalar_imm(static_cast<float>(v.ival), &o)) return false;
  v.is_int = false;
  v.rows = v.cols = 1;
  v.c[0] = o;
  return true;
}

bool Gen::attribute_index(const Global& g, uint32_t* index) {
  TgsiProgram& p = *sh_.out;
  for (uint64_t i = 0; i < p.attributes.count(); ++i)
    if (p.attributes[i].slot == g.slot) {
      *index = static_cast<uint32_t>(i);
      return true;
    }
  if (p.attributes.count() >= kTgsiMaxAttributes) return fail("too many attributes");
  if (!p.attributes.push_back(TgsiAttribute{g.slot, g.type.rows})) return fail("out of memory");
  *index = static_cast<uint32_t>(p.attributes.count() - 1);
  return true;
}

bool Gen::attribute_at(uint32_t key, uint32_t comps, uint32_t* index) {
  TgsiProgram& p = *sh_.out;
  for (uint64_t i = 0; i < p.attributes.count(); ++i)
    if (p.attributes[i].slot == key) {
      *index = static_cast<uint32_t>(i);
      return true;
    }
  if (p.attributes.count() >= kTgsiMaxAttributes) return fail("too many attributes");
  if (!p.attributes.push_back(TgsiAttribute{key, static_cast<uint8_t>(comps)})) return fail("out of memory");
  *index = static_cast<uint32_t>(p.attributes.count() - 1);
  return true;
}

bool Gen::global_val(uint64_t gi, Val* out) {
  if (gi >= m_.globals.count()) return fail("bad global");
  const Global& g = *m_.globals[gi];
  switch (g.kind) {
    case GlobalKind::Const: {
      if (g.init < 0 || g.type.array) return fail("a constant the device cannot take");
      return eval(g.init, out);
    }
    case GlobalKind::Buffer:   // (a WGSL var<uniform> is a buffer binding; a storage buffer is not for the device)
      if (g.storage) return fail("a storage buffer");
      [[fallthrough]];
    case GlobalKind::Uniform: {
      if (g.type.array) return fail("a uniform array used as a whole");
      if (g.type.base == Base::Struct) {
        if (g.block >= 0) return fail("uniform blocks");
        Var& cached = globals_[gi];
        if (!cached.valid) {
          Val sv;
          if (!uniform_struct(g.slot, g.type, &sv)) return false;
          cached.valid = cached.is_struct = true;
          cached.first = sv.first;
          cached.sid = sv.sid;
        }
        val_of(cached, out);
        return true;
      }
      uint32_t row;
      if (!uniform_base(gi, &row)) return false;
      out->rows = g.type.rows;
      out->cols = g.type.cols;
      out->is_int = false;
      for (uint32_t c = 0; c < g.type.cols; ++c) out->c[c] = filled('C', static_cast<uint16_t>(row + c), g.type.rows);
      return true;
    }
    case GlobalKind::In: {
      if (g.builtin != kBvNone) return fail("a built-in input");
      if (g.type.base != Base::Float || g.type.array || g.type.cols != 1) return fail("an input of a type the device cannot take");
      out->rows = g.type.rows;
      out->cols = 1;
      out->is_int = false;
      if (!fs_) {
        uint32_t k;
        if (!attribute_index(g, &k)) return false;
        out->c[0] = filled('I', static_cast<uint16_t>(k), g.type.rows);
        return true;
      }
      if (g.flat) return fail("a flat varying");
      for (uint64_t i = 0; i < sh_.links.size(); ++i)
        if (sh_.links[i].fs_slot == g.slot) {
          if (i >= kTgsiMaxVaryings || sh_.links[i].count != g.type.rows) return fail("a varying the device cannot take");
          out->c[0] = filled('I', static_cast<uint16_t>(i), g.type.rows);
          return true;
        }
      return fail("a varying the vertex shader does not write");
    }
    case GlobalKind::Builtin:
    case GlobalKind::Out:
    case GlobalKind::Private: {
      if (g.kind == GlobalKind::Private && g.init >= 0) return fail("an initialised global");
      if (g.kind == GlobalKind::Builtin && g.builtin != kBvPosition && g.builtin != kBvPointSize && g.builtin != kBvFragColor) return fail("a built-in variable");
      if ((g.builtin == kBvFragColor) != (fs_ && g.builtin == kBvFragColor) || (!fs_ && g.builtin == kBvFragColor)) return fail("gl_FragColor in a vertex shader");
      if (fs_ && (g.builtin == kBvPosition || g.builtin == kBvPointSize)) return fail("a vertex output in a fragment shader");
      Var v;
      if (!global_var(gi, &v)) return false;
      val_of(v, out);
      return true;
    }
    default: return fail("a global the device cannot take");
  }
}

// --- expressions ---------------------------------------------------------------------------------------------------------------------------------------------

bool Gen::const_val(const Node& n, Val* out) {
  const Type& t = n.type;
  if (t.array || t.base == Base::Struct) return fail("a constant array or structure");
  const uint32_t lanes = static_cast<uint32_t>(t.rows) * t.cols;
  if (n.a < 0 || uint64_t{static_cast<uint32_t>(n.a)} + lanes > m_.consts.count()) return fail("bad constant");
  const uint32_t base = static_cast<uint32_t>(n.a);
  if (t.base == Base::Int || t.base == Base::Uint) {
    if (lanes != 1) return fail("an integer vector");
    out->rows = out->cols = 1;
    out->is_int = true;
    out->ival = static_cast<int32_t>(m_.consts[base]);
    return true;
  }
  if (t.base != Base::Float && t.base != Base::Bool) return fail("a constant of a type the device cannot take");
  if (t.rows > 4 || t.cols > 4) return fail("bad constant");
  out->rows = t.rows;
  out->cols = t.cols;
  out->is_int = false;
  for (uint32_t c = 0; c < t.cols; ++c) {
    float v[4] = {0, 0, 0, 0};
    for (uint32_t r = 0; r < t.rows; ++r) {
      const uint32_t bits = m_.consts[base + c * t.rows + r];
      if (t.base == Base::Bool) {
        v[r] = bits ? 1.0f : 0.0f;
      } else {
        __builtin_memcpy(&v[r], &bits, 4);
      }
    }
    if (!imm(v, t.rows, &out->c[c])) return false;
  }
  return true;
}

bool Gen::const_index(int32_t id, int32_t* out) {
  Val v;
  if (id < 0 || m_.nodes[static_cast<uint64_t>(id)].kind != NodeKind::Const || !eval(id, &v) || !v.is_int) return fail("an index that is not a constant");
  *out = v.ival;
  return true;
}

bool Gen::swizzle_indices(const Node& n, uint32_t* idx, uint32_t* count) {
  if (n.c < 1 || n.c > 4 || n.b < 0 || uint64_t{static_cast<uint32_t>(n.b)} + static_cast<uint32_t>(n.c) > m_.consts.count()) return fail("a structure field");
  *count = static_cast<uint32_t>(n.c);
  for (int32_t i = 0; i < n.c; ++i) {
    switch (static_cast<char>(m_.consts[static_cast<uint64_t>(n.b + i)])) {
      case 'x': case 'r': case 's': idx[i] = 0; break;
      case 'y': case 'g': case 't': idx[i] = 1; break;
      case 'z': case 'b': case 'p': idx[i] = 2; break;
      case 'w': case 'a': case 'q': idx[i] = 3; break;
      default: return fail("a structure field");
    }
  }
  return true;
}

bool Gen::read_lv(const LV& l, Val* out) {
  out->cols = l.cols;
  out->is_int = false;
  if (l.cols > 1) {
    out->rows = l.rows;
    for (uint32_t c = 0; c < l.cols; ++c) out->c[c] = filled('T', l.temp[c], l.rows);
    return true;
  }
  out->rows = l.n;
  Oper o;
  o.file = 'T';
  o.idx = l.temp[0];
  for (uint32_t k = 0; k < 4; ++k) o.swz[k] = l.chan[k < l.n ? k : l.n - 1];
  out->c[0] = o;
  return true;
}

bool Gen::store(const LV& l, const Val& v_in) {
  Val v = v_in;
  if (!to_float(v)) return false;
  if (l.agg || v.kind) return fail("assignment of a whole structure");
  if (l.cols > 1) {
    if (v.cols != l.cols || v.rows != l.rows) return fail("matrix assignment of another shape");
    for (uint32_t c = 0; c < l.cols; ++c) inst_rows("MOV", l.temp[c], l.rows, &v.c[c]);
    return true;
  }
  if (v.cols != 1 || v.rows != l.n) return fail("assignment of another shape");
  Oper s = v.c[0];
  bool used[4] = {false, false, false, false};
  for (uint32_t j = 0; j < l.n; ++j) {
    s.swz[l.chan[j]] = v.c[0].swz[j];
    used[l.chan[j]] = true;
  }
  char mask[5];
  uint32_t n = 0;
  for (uint32_t k = 0; k < 4; ++k)
    if (used[k]) mask[n++] = "xyzw"[k];
  mask[n] = 0;
  inst("MOV", l.temp[0], mask, &s);
  return true;
}

bool Gen::lvalue(int32_t id, LV* out) {
  if (id < 0) return fail("missing expression");
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  switch (n.kind) {
    case NodeKind::Local: {
      if (n.ival < 0 || static_cast<uint64_t>(n.ival) >= locals_.count() || !locals_[static_cast<uint64_t>(n.ival)].valid) return fail("a variable used before it is declared");
      lv_of(locals_[static_cast<uint64_t>(n.ival)], out);
      return true;
    }
    case NodeKind::Global: {
      if (n.ival < 0 || static_cast<uint64_t>(n.ival) >= m_.globals.count()) return fail("bad global");
      const Global& g = *m_.globals[static_cast<uint64_t>(n.ival)];
      if ((g.kind != GlobalKind::Out && g.kind != GlobalKind::Private && g.kind != GlobalKind::Builtin) || g.is_read_only) return fail("assignment to an input or uniform");
      Val ignored;
      Var v;
      if (!global_val(static_cast<uint64_t>(n.ival), &ignored) || !global_var(static_cast<uint64_t>(n.ival), &v)) return false;
      lv_of(v, out);
      return true;
    }
    case NodeKind::Field: {
      LV base;
      uint32_t idx[4], count;
      if (!lvalue(n.a, &base)) return false;
      if (base.agg) {
        uint32_t f;
        if (!find_field(base.sid, n, &f)) return false;
        return lv_from_val(members_[static_cast<uint64_t>(base.first) + f], out);
      }
      if (!swizzle_indices(n, idx, &count)) return false;
      if (base.cols != 1) return fail("a swizzle of a matrix");
      LV r = base;
      r.n = static_cast<uint8_t>(count);
      for (uint32_t j = 0; j < count; ++j) {
        if (idx[j] >= base.n) return fail("swizzle out of range");
        r.chan[j] = base.chan[idx[j]];
      }
      *out = r;
      return true;
    }
    case NodeKind::Index: {
      LV base;
      int32_t k;
      if (!lvalue(n.a, &base) || !const_index(n.b, &k) || k < 0) return fail("an indexed assignment the device cannot take");
      if (base.cols > 1) {
        if (k >= base.cols) return fail("index out of range");
        LV r;
        r.cols = 1;
        r.rows = base.rows;
        r.n = base.rows;
        r.temp[0] = base.temp[k];
        *out = r;
        return true;
      }
      if (k >= base.n) return fail("index out of range");
      LV r = base;
      r.n = 1;
      r.chan[0] = base.chan[k];
      *out = r;
      return true;
    }
    default: return fail("not assignable");
  }
}

bool Gen::elementwise(const char* opc, const Val& a, const Val& b, Val* out) {
  if (a.is_int || b.is_int || a.kind || b.kind) return fail("integer arithmetic");
  const bool a_scalar = a.cols == 1 && a.rows == 1, b_scalar = b.cols == 1 && b.rows == 1;
  if (!a_scalar && !b_scalar && (a.cols != b.cols || a.rows != b.rows)) return fail("operands of different shapes");
  const uint32_t cols = a.cols > b.cols ? a.cols : b.cols;
  const uint32_t rows = a.rows > b.rows ? a.rows : b.rows;
  if (!new_val(rows, cols, out)) return false;
  for (uint32_t c = 0; c < cols; ++c) {
    const Oper& sa = a.cols > 1 ? a.c[c] : a.c[0];
    const Oper& sb = b.cols > 1 ? b.c[c] : b.c[0];
    inst_rows(opc, out->c[c].idx, rows, &sa, &sb);
  }
  return true;
}

// OP applied to each component separately, for TGSI's scalar instructions (RCP, SQRT, POW, ...): each reads x of its source and fills the channels written.
bool Gen::per_component(const char* opc, const Val& a, const Val* b, Val* out) {
  if (a.is_int || (b && b->is_int) || a.cols != 1 || (b && b->cols != 1)) return fail("a scalar function of a matrix or integer");
  const uint32_t rows = b && b->rows > a.rows ? b->rows : a.rows;
  if (!new_val(rows, 1, out)) return false;
  for (uint32_t j = 0; j < rows; ++j) {
    const Oper sa = component(a.c[0], j < a.rows ? j : 0);
    if (b) {
      const Oper sb = component(b->c[0], j < b->rows ? j : 0);
      inst(opc, out->c[0].idx, kChan[j], &sa, &sb);
    } else {
      inst(opc, out->c[0].idx, kChan[j], &sa);
    }
  }
  return true;
}

bool Gen::mat_vec(const Val& m, const Val& v, Val* out) {
  if (v.cols != 1 || v.rows != m.cols) return fail("matrix and vector sizes do not match");
  if (!new_val(m.rows, 1, out)) return false;
  const Oper s0 = component(v.c[0], 0);
  inst_rows("MUL", out->c[0].idx, m.rows, &m.c[0], &s0);
  for (uint32_t i = 1; i < m.cols; ++i) {
    const Oper si = component(v.c[0], i);
    inst_rows("MAD", out->c[0].idx, m.rows, &m.c[i], &si, &out->c[0]);
  }
  return true;
}

bool Gen::dot(const Val& a, const Val& b, Val* out) {
  if (a.cols != 1 || b.cols != 1 || a.rows != b.rows || a.is_int || b.is_int) return fail("dot of other shapes");
  if (!new_val(1, 1, out)) return false;
  static const char* const kDp[] = {"MUL", "MUL", "DP2", "DP3", "DP4"};
  inst_rows(kDp[a.rows], out->c[0].idx, 1, &a.c[0], &b.c[0]);
  return true;
}

bool Gen::binary(Op op, const Val& a_in, const Val& b_in, Val* out) {
  Val a = a_in, b = b_in;   // (an integer constant meets a float: WGSL's abstract numbers)
  if (a.is_int != b.is_int && (!to_float(a) || !to_float(b))) return false;
  if (a.is_int || a.kind || b.kind) return fail("integer arithmetic");
  switch (op) {
    case Op::Add: return elementwise("ADD", a, b, out);
    case Op::Sub: return elementwise("SUB", a, b, out);
    case Op::Div: return elementwise("DIV", a, b, out);
    case Op::Mul: {
      if (a.cols == 1 && b.cols == 1) return elementwise("MUL", a, b, out);
      if (a.cols > 1 && b.cols > 1) {   // matrix * matrix: each column of the right one through the left one
        if (b.rows != a.cols) return fail("matrix sizes do not match");
        if (!new_val(a.rows, b.cols, out)) return false;
        for (uint32_t j = 0; j < b.cols; ++j) {
          Val col;
          col.rows = b.rows;
          col.c[0] = b.c[j];
          Val r;
          if (!mat_vec(a, col, &r)) return false;
          inst_rows("MOV", out->c[j].idx, a.rows, &r.c[0]);
        }
        return true;
      }
      if (a.cols > 1 && b.rows > 1) return mat_vec(a, b, out);
      if (b.cols > 1 && a.rows > 1) {   // row vector * matrix: a dot product per column
        if (a.rows != b.rows) return fail("matrix and vector sizes do not match");
        if (!new_val(b.cols, 1, out)) return false;
        static const char* const kDp[] = {"MUL", "MUL", "DP2", "DP3", "DP4"};
        for (uint32_t j = 0; j < b.cols; ++j) inst(kDp[a.rows], out->c[0].idx, kChan[j], &a.c[0], &b.c[j]);
        return true;
      }
      return elementwise("MUL", a, b, out);   // matrix * scalar
    }
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne: {
      if (a.cols != 1 || b.cols != 1 || a.rows != 1 || b.rows != 1) return fail("a comparison of vectors");
      const Val* x = &a;
      const Val* y = &b;
      const char* opc = "SLT";
      switch (op) {
        case Op::Lt: break;
        case Op::Gt: x = &b; y = &a; break;
        case Op::Le: x = &b; y = &a; opc = "SGE"; break;
        case Op::Ge: opc = "SGE"; break;
        case Op::Eq: opc = "SEQ"; break;
        default: opc = "SNE"; break;
      }
      return elementwise(opc, *x, *y, out);
    }
    default: return fail("an operator the device cannot take");
  }
}

// Builds a vector of `rows` components from single-component operands (see component()), one MOV each.
bool Gen::pieces_to_vec(const Oper* comps, uint32_t count, uint32_t rows, Val* out) {
  if (count < rows) return fail("too few components");
  if (!new_val(rows, 1, out)) return false;
  for (uint32_t k = 0; k < rows; ++k) inst("MOV", out->c[0].idx, kChan[k], &comps[k]);   // (each operand has one component in every swizzle slot)
  return true;
}

bool Gen::construct(const Node& n, Val* out) {
  Type t = n.type;
  if (t.base == Base::Void && t.array == 0) t.base = Base::Float;   // WGSL vec3(1.0, 2.0, 3.0): the element type is the arguments'
  if (t.array == 0xFFFFFFFFu) return fail("an array constructor");
  if (!is_float_type(t) || n.count < 1 || n.count > 16) return fail("a constructor the device cannot take");
  Val args[16];
  uint32_t total = 0;
  Oper comps[16];
  for (int32_t i = 0; i < n.count; ++i) {
    Val& a = args[i];
    if (!eval(m_.kids[static_cast<uint64_t>(n.first + i)], &a) || !to_float(a)) return false;
    if (a.kind) return fail("a structure in a constructor");
    for (uint32_t c = 0; c < a.cols; ++c)
      for (uint32_t r = 0; r < a.rows; ++r) {
        if (total >= 16) return fail("too many components");
        comps[total++] = component(a.c[c], r);
      }
  }
  const uint32_t rows = t.rows, cols = t.cols;
  if (cols == 1) {
    if (total == 1 && args[0].rows == 1 && args[0].cols == 1) {   // one scalar fills every component: no instruction
      out->rows = static_cast<uint8_t>(rows);
      out->cols = 1;
      out->is_int = false;
      out->c[0] = filled(args[0].c[0].file, args[0].c[0].idx, 1);
      out->c[0].swz[0] = out->c[0].swz[1] = out->c[0].swz[2] = out->c[0].swz[3] = args[0].c[0].swz[0];
      out->c[0].neg = args[0].c[0].neg;
      return true;
    }
    if (n.count == 1 && args[0].cols == 1 && args[0].rows == rows) {
      *out = args[0];
      return true;
    }
    if (n.count == 1 && args[0].cols > 1) return fail("a vector from a matrix");
    return pieces_to_vec(comps, total, rows, out);
  }
  // matrix
  if (n.count == 1 && args[0].cols == 1 && args[0].rows == 1) {   // a diagonal
    if (!new_val(rows, cols, out)) return false;
    for (uint32_t c = 0; c < cols; ++c) {
      float unit[4] = {0, 0, 0, 0};
      if (c < rows) unit[c] = 1.0f;
      Oper u;
      if (!imm(unit, rows, &u)) return false;
      const Oper s = component(args[0].c[0], 0);
      inst_rows("MUL", out->c[c].idx, rows, &u, &s);
    }
    return true;
  }
  if (n.count == 1 && args[0].cols > 1) return fail("a matrix from another matrix");
  bool columns = n.count == static_cast<int32_t>(cols);
  for (int32_t i = 0; i < n.count && columns; ++i) columns = args[i].cols == 1 && args[i].rows == rows;
  out->rows = static_cast<uint8_t>(rows);
  out->cols = static_cast<uint8_t>(cols);
  out->is_int = false;
  if (columns) {
    for (uint32_t c = 0; c < cols; ++c) out->c[c] = args[c].c[0];
    return true;
  }
  if (total < rows * cols) return fail("too few components");
  for (uint32_t c = 0; c < cols; ++c) {
    Val col;
    if (!pieces_to_vec(comps + c * rows, total - c * rows, rows, &col)) return false;
    out->c[c] = col.c[0];
  }
  return true;
}

bool Gen::call_args(const Node& n, Val* args, uint32_t max, uint32_t* count) {
  if (n.count < 0 || static_cast<uint32_t>(n.count) > max) return fail("too many arguments");
  *count = static_cast<uint32_t>(n.count);
  for (uint32_t i = 0; i < *count; ++i) {
    if (!eval(m_.kids[static_cast<uint64_t>(n.first) + i], &args[i])) return false;
    if (!to_float(args[i])) return false;
    if (args[i].kind) return fail("a structure argument");
  }
  return true;
}

bool Gen::builtin(const Node& n, Val* out) {
  Val a[3];
  uint32_t argc;
  if (!call_args(n, a, 3, &argc)) return false;
  const BuiltinFn fn = static_cast<BuiltinFn>(n.ival);
  auto need = [&](uint32_t k) { return argc == k || fail("a built-in called with other arguments"); };
  // one-argument component-wise instructions read a single source
  auto simple1 = [&](const char* opc) {
    if (!need(1) || a[0].cols != 1) return false;
    if (!new_val(a[0].rows, 1, out)) return false;
    inst_rows(opc, out->c[0].idx, a[0].rows, &a[0].c[0]);
    return true;
  };
  Oper k;
  switch (fn) {
    case BuiltinFn::Abs: return simple1("ABS");
    case BuiltinFn::Floor: return simple1("FLR");
    case BuiltinFn::Ceil: return simple1("CEIL");
    case BuiltinFn::Fract: return simple1("FRC");
    case BuiltinFn::Sign: return simple1("SSG");
    case BuiltinFn::Trunc: return simple1("TRUNC");
    case BuiltinFn::Round: case BuiltinFn::RoundEven: return simple1("ROUND");
    case BuiltinFn::Sqrt: return need(1) && per_component("SQRT", a[0], nullptr, out);
    case BuiltinFn::InverseSqrt: return need(1) && per_component("RSQ", a[0], nullptr, out);
    case BuiltinFn::Sin: return need(1) && per_component("SIN", a[0], nullptr, out);
    case BuiltinFn::Cos: return need(1) && per_component("COS", a[0], nullptr, out);
    case BuiltinFn::Exp2: return need(1) && per_component("EX2", a[0], nullptr, out);
    case BuiltinFn::Log2: return need(1) && per_component("LG2", a[0], nullptr, out);
    case BuiltinFn::Pow: return need(2) && per_component("POW", a[0], &a[1], out);
    case BuiltinFn::Exp: case BuiltinFn::Log: {   // exp(x) = exp2(x * log2(e)), log(x) = log2(x) * ln(2)
      Val t;
      if (!need(1) || !scalar_imm(fn == BuiltinFn::Exp ? 1.44269504f : 0.693147181f, &k)) return false;
      Val kv;
      kv.c[0] = k;
      if (fn == BuiltinFn::Exp) return elementwise("MUL", a[0], kv, &t) && per_component("EX2", t, nullptr, out);
      return per_component("LG2", a[0], nullptr, &t) && elementwise("MUL", t, kv, out);
    }
    case BuiltinFn::Radians: case BuiltinFn::Degrees: {
      if (!need(1) || !scalar_imm(fn == BuiltinFn::Radians ? 0.0174532925f : 57.2957795f, &k)) return false;
      Val kv;
      kv.c[0] = k;
      return elementwise("MUL", a[0], kv, out);
    }
    case BuiltinFn::Min: return need(2) && elementwise("MIN", a[0], a[1], out);
    case BuiltinFn::Max: return need(2) && elementwise("MAX", a[0], a[1], out);
    case BuiltinFn::Step: return need(2) && elementwise("SGE", a[1], a[0], out);   // step(edge, x) = x >= edge
    case BuiltinFn::Mod: {   // x - y * floor(x / y)
      Val q, f, m;
      if (!need(2) || !elementwise("DIV", a[0], a[1], &q)) return false;
      if (!new_val(q.rows, 1, &f)) return false;
      inst_rows("FLR", f.c[0].idx, q.rows, &q.c[0]);
      return elementwise("MUL", a[1], f, &m) && elementwise("SUB", a[0], m, out);
    }
    case BuiltinFn::Clamp: {
      Val t;
      return need(3) && elementwise("MAX", a[0], a[1], &t) && elementwise("MIN", t, a[2], out);
    }
    case BuiltinFn::Mix: {   // mix(x, y, a) = x * (1 - a) + y * a, which is LRP(a, y, x)
      if (!need(3) || a[0].cols != 1 || a[1].cols != 1 || a[0].rows != a[1].rows) return fail("mix of other shapes");
      if (!new_val(a[0].rows, 1, out)) return false;
      inst_rows("LRP", out->c[0].idx, a[0].rows, &a[2].c[0], &a[1].c[0], &a[0].c[0]);
      return true;
    }
    case BuiltinFn::Smoothstep: {
      Val u, v;
      Oper m2, p3, zero, one;
      if (!need(3) || !scalar_imm(-2.0f, &m2) || !scalar_imm(3.0f, &p3) || !scalar_imm(0.0f, &zero) || !scalar_imm(1.0f, &one)) return false;
      Val zv, ov;
      zv.c[0] = zero;
      ov.c[0] = one;
      Val x = a[2], num, den, q, c0, c1;
      if (!elementwise("SUB", x, a[0], &num) || !elementwise("SUB", a[1], a[0], &den) || !elementwise("DIV", num, den, &q)) return false;
      if (!elementwise("MAX", q, zv, &c0) || !elementwise("MIN", c0, ov, &c1)) return false;
      if (!new_val(c1.rows, 1, &u) || !new_val(c1.rows, 1, &v)) return false;
      inst_rows("MAD", u.c[0].idx, c1.rows, &c1.c[0], &m2, &p3);   // 3 - 2t
      inst_rows("MUL", v.c[0].idx, c1.rows, &c1.c[0], &c1.c[0]);   // t * t
      return elementwise("MUL", v, u, out);
    }
    case BuiltinFn::Fma: return need(3) && a[0].cols == 1 && a[1].cols == 1 && a[2].cols == 1 && a[0].rows == a[1].rows && a[1].rows == a[2].rows && new_val(a[0].rows, 1, out) &&
                                 (inst_rows("MAD", out->c[0].idx, a[0].rows, &a[0].c[0], &a[1].c[0], &a[2].c[0]), true);
    case BuiltinFn::Saturate: {
      Val zv, ov, t;
      Oper zero, one;
      if (!need(1) || !scalar_imm(0.0f, &zero) || !scalar_imm(1.0f, &one)) return false;
      zv.c[0] = zero;
      ov.c[0] = one;
      return elementwise("MAX", a[0], zv, &t) && elementwise("MIN", t, ov, out);
    }
    case BuiltinFn::Select: {   // select(f, t, cond)
      if (!need(3) || a[2].rows != 1 || a[2].cols != 1 || a[0].cols != a[1].cols || a[0].rows != a[1].rows) return fail("select of other shapes");
      if (!new_val(a[0].rows, a[0].cols, out)) return false;
      for (uint32_t c = 0; c < a[0].cols; ++c) inst_rows("LRP", out->c[c].idx, a[0].rows, &a[2].c[0], &a[1].c[c], &a[0].c[c]);
      return true;
    }
    case BuiltinFn::Dot: return need(2) && dot(a[0], a[1], out);
    case BuiltinFn::Length: {
      Val d;
      if (!need(1)) return false;
      if (a[0].cols == 1 && a[0].rows == 1) return simple1("ABS");
      return dot(a[0], a[0], &d) && per_component("SQRT", d, nullptr, out);
    }
    case BuiltinFn::Distance: {
      Val diff, d;
      return need(2) && elementwise("SUB", a[0], a[1], &diff) && dot(diff, diff, &d) && per_component("SQRT", d, nullptr, out);
    }
    case BuiltinFn::Normalize: {
      Val d, r;
      if (!need(1) || a[0].cols != 1 || a[0].rows < 2) return fail("normalize of other shapes");
      if (!dot(a[0], a[0], &d) || !per_component("RSQ", d, nullptr, &r)) return false;
      return elementwise("MUL", a[0], r, out);
    }
    case BuiltinFn::Cross: {
      if (!need(2) || a[0].cols != 1 || a[0].rows != 3 || a[1].rows != 3 || a[1].cols != 1) return fail("cross of other shapes");
      if (!new_val(3, 1, out)) return false;
      inst_rows("XPD", out->c[0].idx, 3, &a[0].c[0], &a[1].c[0]);
      return true;
    }
    case BuiltinFn::Reflect: {   // I - 2 dot(N, I) N
      Val d, d2, t;
      Oper two;
      if (!need(2) || !scalar_imm(2.0f, &two) || !dot(a[1], a[0], &d)) return false;
      Val tv;
      tv.c[0] = two;
      return elementwise("MUL", d, tv, &d2) && elementwise("MUL", a[1], d2, &t) && elementwise("SUB", a[0], t, out);
    }
    case BuiltinFn::MatrixCompMult: return need(2) && elementwise("MUL", a[0], a[1], out);
    case BuiltinFn::Transpose: {
      if (!need(1) || a[0].cols == 1) return fail("transpose of other shapes");
      out->rows = a[0].cols;
      out->cols = a[0].rows;
      out->is_int = false;
      for (uint32_t j = 0; j < a[0].rows; ++j) {
        Oper pieces[4];
        for (uint32_t i = 0; i < a[0].cols; ++i) pieces[i] = component(a[0].c[i], j);
        Val col;
        if (!pieces_to_vec(pieces, a[0].cols, a[0].cols, &col)) return false;
        out->c[j] = col.c[0];
      }
      return true;
    }
    default: return fail("a built-in function the device cannot take");
  }
}

bool Gen::eval(int32_t id, Val* out) {
  if (id < 0) return fail("missing expression");
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  switch (n.kind) {
    case NodeKind::Const: return const_val(n, out);
    case NodeKind::Local: {
      if (n.ival < 0 || static_cast<uint64_t>(n.ival) >= locals_.count() || !locals_[static_cast<uint64_t>(n.ival)].valid) return fail("a variable used before it is declared");
      val_of(locals_[static_cast<uint64_t>(n.ival)], out);
      return true;
    }
    case NodeKind::Global: return n.ival >= 0 && global_val(static_cast<uint64_t>(n.ival), out);
    case NodeKind::Unary: {
      Val a;
      if (!eval(n.a, &a)) return false;
      if (a.is_int) return fail("integer arithmetic");
      if (n.op == Op::Plus) {
        *out = a;
        return true;
      }
      if (n.op == Op::Neg) {
        *out = a;
        for (uint32_t c = 0; c < a.cols; ++c) out->c[c].neg = !a.c[c].neg;
        return true;
      }
      if (n.op == Op::Not && a.cols == 1 && a.rows == 1) {
        Oper zero;
        if (!scalar_imm(0.0f, &zero)) return false;
        Val z;
        z.c[0] = zero;
        return elementwise("SEQ", a, z, out);
      }
      return fail("an operator the device cannot take");
    }
    case NodeKind::Binary: {
      Val a, b;
      return eval(n.a, &a) && eval(n.b, &b) && binary(n.op, a, b, out);
    }
    case NodeKind::Logical: {
      Val a, b;
      if (!eval(n.a, &a) || !eval(n.b, &b)) return false;
      if (a.rows != 1 || a.cols != 1 || b.rows != 1 || b.cols != 1 || a.is_int || b.is_int) return fail("a logical operator on vectors");
      return elementwise(n.op == Op::LogAnd ? "MUL" : (n.op == Op::LogOr ? "MAX" : "SNE"), a, b, out);
    }
    case NodeKind::Assign: {
      Val r;
      LV l;
      if (!eval(n.b, &r)) return false;
      if (n.op != Op::None) {
        Val cur, res;
        if (!eval(n.a, &cur) || !binary(n.op, cur, r, &res)) return false;
        r = res;
      }
      return lvalue(n.a, &l) && store(l, r) && read_lv(l, out);
    }
    case NodeKind::Ternary: {
      Val c, x, y;
      if (!eval(n.a, &c) || !eval(n.b, &x) || !eval(n.c, &y)) return false;
      if (!to_float(x) || !to_float(y)) return false;
      if (c.rows != 1 || c.cols != 1 || c.is_int || x.kind || y.kind || x.cols != y.cols || x.rows != y.rows) return fail("a conditional of other shapes");
      if (!new_val(x.rows, x.cols, out)) return false;
      for (uint32_t col = 0; col < x.cols; ++col) inst_rows("LRP", out->c[col].idx, x.rows, &c.c[0], &x.c[col], &y.c[col]);
      return true;
    }
    case NodeKind::Comma: {
      Val ignored;
      return eval(n.a, &ignored) && eval(n.b, out);
    }
    case NodeKind::Index: {
      int32_t k;
      if (n.a >= 0 && m_.nodes[static_cast<uint64_t>(n.a)].kind == NodeKind::Global) {
        const int32_t gi = m_.nodes[static_cast<uint64_t>(n.a)].ival;
        if (gi >= 0 && static_cast<uint64_t>(gi) < m_.globals.count() && m_.globals[static_cast<uint64_t>(gi)]->type.array) {
          const Global& g = *m_.globals[static_cast<uint64_t>(gi)];
          if (g.kind != GlobalKind::Uniform || !const_index(n.b, &k) || k < 0 || static_cast<uint32_t>(k) >= g.type.array) return fail("an array access the device cannot take");
          uint32_t row;
          if (!uniform_base(static_cast<uint64_t>(gi), &row)) return false;
          out->rows = g.type.rows;
          out->cols = g.type.cols;
          out->is_int = false;
          for (uint32_t c = 0; c < g.type.cols; ++c) out->c[c] = filled('C', static_cast<uint16_t>(row + static_cast<uint32_t>(k) * g.type.cols + c), g.type.rows);
          return true;
        }
      }
      Val b;
      if (!eval(n.a, &b) || !const_index(n.b, &k) || k < 0) return fail("an index the device cannot take");
      if (b.cols > 1) {
        if (k >= b.cols) return fail("index out of range");
        out->rows = b.rows;
        out->cols = 1;
        out->is_int = false;
        out->c[0] = b.c[k];
        return true;
      }
      if (b.rows < 2 || k >= b.rows) return fail("index out of range");
      out->rows = out->cols = 1;
      out->is_int = false;
      out->c[0] = component(b.c[0], static_cast<uint32_t>(k));
      return true;
    }
    case NodeKind::Field: {
      Val b;
      uint32_t idx[4], count;
      if (!eval(n.a, &b)) return false;
      if (b.kind == 1) {
        uint32_t f;
        if (!find_field(b.sid, n, &f)) return false;
        Val& mem = members_[static_cast<uint64_t>(b.first) + f];
        if (mem.kind == 3) {
          uint32_t row;
          if (!alloc_const(static_cast<uint32_t>(mem.ival), mem.rows, mem.cols, 1, &row)) return false;
          mem.kind = 0;
          for (uint32_t c = 0; c < mem.cols; ++c) mem.c[c] = filled('C', static_cast<uint16_t>(row + c), mem.rows);
        }
        *out = mem;
        return out->kind != 2 || fail("a built-in input read by the shader");
      }
      if (!swizzle_indices(n, idx, &count)) return false;
      if (b.cols != 1 || b.is_int) return fail("a swizzle of a matrix");
      out->rows = static_cast<uint8_t>(count);
      out->cols = 1;
      out->is_int = false;
      out->c[0] = b.c[0];
      for (uint32_t j = 0; j < 4; ++j) {
        const uint32_t from = idx[j < count ? j : count - 1];
        if (from >= b.rows) return fail("swizzle out of range");
        out->c[0].swz[j] = b.c[0].swz[from];
      }
      return true;
    }
    case NodeKind::Construct: return construct(n, out);
    case NodeKind::Cast: {
      Val a;
      if (!eval(n.a, &a)) return false;
      if (n.type.base == Base::Float && n.type.rows == 1 && n.type.cols == 1 && n.type.array == 0) {
        if (a.is_int) {
          Oper o;
          if (!scalar_imm(static_cast<float>(a.ival), &o)) return false;
          out->rows = out->cols = 1;
          out->is_int = false;
          out->c[0] = o;
          return true;
        }
        if (a.rows == 1 && a.cols == 1) {
          *out = a;
          return true;
        }
      }
      return fail("a conversion the device cannot take");
    }
    case NodeKind::Builtin: return builtin(n, out);
    default: return fail("an expression the device cannot take");
  }
}

// --- statements ----------------------------------------------------------------------------------------------------------------------------------------------

bool Gen::stmt(int32_t id, bool tail) {
  if (id < 0) return true;
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  switch (n.kind) {
    case NodeKind::Empty: return true;
    case NodeKind::Block:
      for (int32_t i = 0; i < n.count; ++i)
        if (!stmt(m_.kids[static_cast<uint64_t>(n.first + i)], tail && i == n.count - 1)) return false;
      return true;
    case NodeKind::Decl: {
      if (n.ival < 0 || static_cast<uint64_t>(n.ival) >= locals_.count()) return fail("bad variable");
      Var v;
      if (n.type.base == Base::Void) {   // WGSL let/var without a type: it is the initializer's
        Val init;
        if (n.a < 0 || !eval(n.a, &init) || !to_float(init)) return fail("a variable without a type");
        if (init.kind == 1) {
          v.valid = v.is_struct = true;
          v.first = init.first;
          v.sid = init.sid;
          locals_[static_cast<uint64_t>(n.ival)] = v;
          return true;
        }
        Type t = make_type(Base::Float, init.rows, init.cols);
        LV l;
        if (!make_var(t, &v)) return false;
        locals_[static_cast<uint64_t>(n.ival)] = v;
        lv_of(v, &l);
        return store(l, init);
      }
      if (!make_var(n.type, &v)) return false;
      locals_[static_cast<uint64_t>(n.ival)] = v;
      if (n.a >= 0) {
        Val init;
        LV l;
        lv_of(v, &l);
        return eval(n.a, &init) && store(l, init);
      }
      return true;
    }
    case NodeKind::ExprStmt: {
      if (n.a < 0) return true;
      Val ignored;
      return eval(n.a, &ignored);
    }
    case NodeKind::If: {
      Val c;
      if (!eval(n.a, &c)) return false;
      if (c.is_int || c.cols != 1 || c.rows != 1) return fail("a condition that is not a scalar");
      begin_inst("IF");
      body.push_back(' ');
      put_oper(c.c[0]);
      end_inst();
      if (!stmt(n.b, false)) return false;
      if (n.c >= 0) {
        begin_inst("ELSE");
        end_inst();
        if (!stmt(n.c, false)) return false;
      }
      begin_inst("ENDIF");
      end_inst();
      return true;
    }
    case NodeKind::Return:
      if (!tail || (n.a >= 0 && (entry_ < 0 || have_ret_))) return fail("an early return");
      if (n.a >= 0) {
        if (!eval(n.a, &ret_)) return false;
        have_ret_ = true;
      }
      return true;
    case NodeKind::Discard:
      if (!fs_) return fail("discard in a vertex shader");
      begin_inst("KILL");
      end_inst();
      return true;
    default: return fail("a statement the device cannot take (loops, switch)");
  }
}

// An input of the entry point (WGSL): a vertex attribute or a varying, copied into the variable the shader reads.
bool Gen::fill_input(int32_t location, int32_t builtin, bool flat, const Val& dst) {
  if (builtin != kBvNone) return fail("a built-in input");
  if (location < 0 || flat || dst.cols != 1 || dst.kind) return fail("an input the device cannot take");
  Oper in;
  if (!fs_) {
    uint32_t k;
    if (!attribute_at(static_cast<uint32_t>(location), dst.rows, &k)) return false;
    in = filled('I', static_cast<uint16_t>(k), dst.rows);
  } else {
    uint32_t k = sh_.var_count;
    for (uint32_t i = 0; i < sh_.var_count; ++i)
      if (sh_.var_loc[i] == static_cast<uint32_t>(location)) k = i;
    if (k == sh_.var_count || sh_.var_rows[k] != dst.rows) return fail("a varying the vertex shader does not write");
    in = filled('I', static_cast<uint16_t>(k), dst.rows);
  }
  inst_rows("MOV", dst.c[0].idx, dst.rows, &in);
  return true;
}

bool Gen::run_entry() {
  if (static_cast<uint64_t>(entry_) >= m_.functions.count()) return fail("no entry point");
  const Function& f = *m_.functions[static_cast<uint64_t>(entry_)];
  if (f.stage != (fs_ ? Stage::Fragment : Stage::Vertex)) return fail("an entry point of the wrong stage");
  for (uint32_t i = 0; i < f.locals; ++i)
    if (!locals_.push_back(Var())) return fail("out of memory");
  for (uint64_t i = 0; i < m_.globals.count(); ++i)
    if (!globals_.push_back(Var())) return fail("out of memory");
  for (uint64_t i = 0; i < f.params.count(); ++i) {
    const Param& p = f.params[i];
    if (p.dir != 0 || i >= locals_.count()) return fail("a parameter the device cannot take");
    // A built-in input (vertex_index, position, front_facing ...) is not available on the device: it is fine as long as the shader does not read it, so it is
    // left as a variable that fails when it is used.
    if (p.type.base != Base::Struct && p.builtin != kBvNone) continue;
    Var v;
    if (!make_var(p.type, &v)) return false;
    locals_[i] = v;
    Val vv;
    val_of(v, &vv);
    if (vv.kind == 1) {
      const StructDef* sd = m_.struct_of(p.type);
      for (uint64_t k = 0; sd && k < sd->fields.count(); ++k) {
        const Field& fld = sd->fields[k];
        if (fld.builtin != kBvNone) {
          Val poison;
          poison.kind = 2;
          members_[static_cast<uint64_t>(vv.first) + k] = poison;
        } else if (!fill_input(fld.location, fld.builtin, fld.flat, members_[static_cast<uint64_t>(vv.first) + k])) {
          return false;
        }
      }
    } else if (!fill_input(p.location, p.builtin, p.flat, vv)) {
      return false;
    }
  }
  if (!stmt(f.body, true)) return false;
  if (!have_ret_) return fail("an entry point that returns nothing");

  // the outputs: (builtin, location, flat, value) of each result
  struct Out {
    int32_t builtin, location;
    bool flat;
    Val v;
  };
  Out outs[17];
  uint32_t nout = 0;
  if (ret_.kind == 1) {
    const StructDef* sd = m_.struct_of(f.ret);
    if (!sd || sd->fields.count() > 16) return fail("a result structure the device cannot take");
    for (uint64_t k = 0; k < sd->fields.count(); ++k)
      outs[nout++] = Out{sd->fields[k].builtin, sd->fields[k].location, sd->fields[k].flat, members_[static_cast<uint64_t>(ret_.first) + k]};
  } else {
    outs[nout++] = Out{f.ret_builtin, f.ret_location, false, ret_};
  }
  bool have_color = false;
  for (uint32_t k = 0; k < nout; ++k) {
    const Out& o = outs[k];
    if (o.v.kind || o.v.cols != 1 || o.v.is_int) return fail("a result the device cannot take");
    if (!fs_) {
      if (o.builtin == kBvPosition) {
        if (o.v.rows != 4) return fail("a position that is not a vec4");
        // WebGPU's clip space already has 0 <= z <= w; y is negated because the device's row 0 is the top of the picture and its +y is down
        out_mov(0, "xzw", o.v.c[0]);
        Oper ny = component(o.v.c[0], 1);
        ny.neg = !ny.neg;
        out_mov(0, "y", ny);
        have_color = true;
      } else if (o.builtin == kBvNone && o.location >= 0) {
        if (o.flat || sh_.var_count >= kTgsiMaxVaryings) return fail("a varying the device cannot take");
        const uint32_t idx = sh_.var_count++;
        sh_.var_loc[idx] = static_cast<uint32_t>(o.location);
        sh_.var_rows[idx] = o.v.rows;
        out_mov(1 + idx, mask_of(o.v.rows), o.v.c[0]);
      } else {
        return fail("a vertex output the device cannot take");
      }
    } else {
      if (o.builtin != kBvNone || o.location != 0 || o.v.rows != 4 || have_color) return fail("a fragment output the device cannot take");
      out_mov(0, "xyzw", o.v.c[0]);
      have_color = true;
    }
  }
  return have_color || fail("no output");
}

bool Gen::run() {
  if (entry_ >= 0) {
    if (m_.uses_derivatives || m_.uses_implicit_lod || m_.writes_frag_depth) return fail("derivatives, texture lookups or gl_FragDepth");
    return run_entry();
  }
  if (m_.main_function < 0 || static_cast<uint64_t>(m_.main_function) >= m_.functions.count()) return fail("no main function");
  const Function& f = *m_.functions[static_cast<uint64_t>(m_.main_function)];
  if (m_.uses_derivatives || m_.uses_implicit_lod || m_.writes_frag_depth) return fail("derivatives, texture lookups or gl_FragDepth");
  for (uint32_t i = 0; i < f.locals; ++i)
    if (!locals_.push_back(Var())) return fail("out of memory");
  for (uint64_t i = 0; i < m_.globals.count(); ++i)
    if (!globals_.push_back(Var())) return fail("out of memory");
  if (!stmt(f.body, true)) return false;

  if (!fs_) {
    const int32_t pi = m_.find_builtin_global(kBvPosition);
    if (pi < 0) return fail("no gl_Position");
    Var pos;
    if (!global_var(static_cast<uint64_t>(pi), &pos)) return false;
    const Oper p = filled('T', pos.temp[0], 4);
    // GL clip space has -w <= z <= w, the device 0 <= z <= w: z' = (z + w) / 2. x and y stay.
    out_mov(0, "xyw", p);
    uint16_t t;
    Oper half;
    if (!new_temp(&t) || !scalar_imm(0.5f, &half)) return false;
    const Oper pz = component(p, 2), pw = component(p, 3);
    inst("ADD", t, "x", &pz, &pw);
    const Oper tx = component(filled('T', t, 1), 0);
    begin_inst("MUL");
    body.append(" OUT[0].z, ");
    put_oper(tx);
    body.append(", ");
    put_oper(half);
    end_inst();
    for (uint64_t k = 0; k < sh_.links.size(); ++k) {
      const TgsiLink& link = sh_.links[k];
      if (k >= kTgsiMaxVaryings || link.count < 1 || link.count > 4) return fail("a varying the device cannot take");
      bool found = false;
      for (uint64_t gi = 0; gi < m_.globals.count() && !found; ++gi) {
        const Global& g = *m_.globals[gi];
        if (g.kind != GlobalKind::Out || g.builtin != kBvNone || g.slot != link.vs_slot) continue;
        if (g.type.base != Base::Float || g.type.array || g.type.cols != 1 || g.type.rows != link.count || g.flat) return fail("a varying the device cannot take");
        Var v;
        if (!global_var(gi, &v)) return false;
        out_mov(static_cast<uint32_t>(1 + k), mask_of(link.count), filled('T', v.temp[0], link.count));
        found = true;
      }
      if (!found) return fail("a varying the vertex shader does not declare");
    }
    return true;
  }
  // fragment: the first color output
  for (uint64_t gi = 0; gi < m_.globals.count(); ++gi) {
    const Global& g = *m_.globals[gi];
    const bool legacy = g.builtin == kBvFragColor && g.referenced;
    const bool modern = g.kind == GlobalKind::Out && g.builtin == kBvNone && !g.type.array && (g.location <= 0);
    if (!legacy && !modern) continue;
    if (g.type.base != Base::Float || g.type.rows != 4 || g.type.cols != 1) return fail("a color output that is not a vec4");
    Var v;
    if (!global_var(gi, &v)) return false;
    out_mov(0, "xyzw", filled('T', v.temp[0], 4));
    return true;
  }
  return fail("no color output");
}

void write_header_decl(String& s, const char* kind, uint32_t count) {
  if (!count) return;
  s.append("DCL ");
  s.append(kind);
  s.append("[0");
  if (count > 1) {
    s.append("..");
    s.append_uint(count - 1);
  }
  s.append("]\n");
}

bool assemble(String& out, const Gen& g, bool fragment, uint32_t attributes, uint32_t varyings, uint32_t const_rows) {
  out.clear();
  if (fragment) {
    out.append("FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n");
    for (uint32_t i = 0; i < varyings; ++i) {
      out.append("DCL IN[");
      out.append_uint(i);
      out.append("], GENERIC[");
      out.append_uint(i);
      out.append("], PERSPECTIVE\n");
    }
    out.append("DCL OUT[0], COLOR\n");
  } else {
    out.append("VERT\n");
    for (uint32_t i = 0; i < attributes; ++i) {
      out.append("DCL IN[");
      out.append_uint(i);
      out.append("]\n");
    }
    out.append("DCL OUT[0], POSITION\n");
    for (uint32_t i = 0; i < varyings; ++i) {
      out.append("DCL OUT[");
      out.append_uint(i + 1);
      out.append("], GENERIC[");
      out.append_uint(i);
      out.append("]\n");
    }
  }
  write_header_decl(out, "CONST", const_rows);
  write_header_decl(out, "TEMP", g.ntemp());
  for (uint64_t i = 0; i < g.imms().count(); ++i) {
    out.append("IMM[");
    out.append_uint(i);
    out.append("] FLT32 {");
    for (uint32_t k = 0; k < 4; ++k) {
      if (k) out.append(", ");
      if (!put_float(out, g.imms()[i].v[k])) return false;
    }
    out.append("}\n");
  }
  out.append(g.body.view());
  out.append("  ");
  out.append_uint(g.instructions());
  return out.append(": END\n");
}

}  // namespace

bool translate_wgsl_to_tgsi(allocators::IAllocator& a, const Module& vs, int32_t vs_fn, const Module& fs, int32_t fs_fn, TgsiProgram* out, const char** why) {
  (void)a;
  out->vs.clear();
  out->fs.clear();
  out->constants.clear();
  out->attributes.clear();
  out->const_rows = 0;
  out->varyings = 0;
  Shared sh(out, ArrayView<const TgsiLink>());
  bool ok = vs.language == Language::Wgsl && fs.language == Language::Wgsl && vs_fn >= 0 && fs_fn >= 0;
  if (!ok) sh.why = "not a WGSL pipeline";
  Gen vg(sh, vs, false, vs_fn);
  Gen fg(sh, fs, true, fs_fn);
  ok = ok && vg.run() && fg.run();
  if (ok && out->attributes.count() == 0) {
    ok = false;
    sh.why = "no vertex attributes";
  }
  if (ok) {
    out->varyings = sh.var_count;
    ok = assemble(out->vs, vg, false, static_cast<uint32_t>(out->attributes.count()), out->varyings, out->const_rows) &&
         assemble(out->fs, fg, true, static_cast<uint32_t>(out->attributes.count()), out->varyings, out->const_rows);
    if (!ok) sh.why = "out of memory";
  }
  if (why) *why = ok ? nullptr : (sh.why ? sh.why : "unsupported shader");
  return ok;
}

bool translate_to_tgsi(allocators::IAllocator& a, const Module& vs, const Module& fs, ArrayView<const TgsiLink> links, TgsiProgram* out, const char** why) {
  (void)a;
  out->vs.clear();
  out->fs.clear();
  out->constants.clear();
  out->attributes.clear();
  out->const_rows = 0;
  out->varyings = 0;
  Shared sh(out, links);
  bool ok = vs.stage == Stage::Vertex && fs.stage == Stage::Fragment && links.size() <= kTgsiMaxVaryings;
  if (!ok) sh.why = "unsupported stage or too many varyings";
  Gen vg(sh, vs, false);
  Gen fg(sh, fs, true);
  ok = ok && vg.run() && fg.run();
  if (ok && out->attributes.count() == 0) {
    ok = false;
    sh.why = "no vertex attributes";
  }
  if (ok) {
    out->varyings = static_cast<uint32_t>(links.size());
    ok = assemble(out->vs, vg, false, static_cast<uint32_t>(out->attributes.count()), out->varyings, out->const_rows) &&
         assemble(out->fs, fg, true, static_cast<uint32_t>(out->attributes.count()), out->varyings, out->const_rows);
    if (!ok) sh.why = "out of memory";
  }
  if (why) *why = ok ? nullptr : (sh.why ? sh.why : "unsupported shader");
  return ok;
}

}  // namespace webgl
