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

constexpr uint32_t kMaxTemps = 1024;
constexpr uint32_t kMaxImms = 256;
constexpr uint32_t kMaxArray = 32;        // elements of a local array (indexed by a variable through a chain of selects)
constexpr uint32_t kMaxCallDepth = 16;    // inlined calls inside each other
const char* const kChan[4] = {"x", "y", "z", "w"};

// What the floats of a value hold. Integers and booleans live in float registers as their values (exact below 2^24, which is what shaders count and index
// with): the arithmetic is the float arithmetic, a division of integers is truncated, and a boolean is 0.0 or 1.0.
enum : uint8_t { kNumFloat = 0, kNumInt = 1, kNumUint = 2, kNumBool = 3 };
uint8_t num_of(Base b) { return b == Base::Int ? kNumInt : (b == Base::Uint ? kNumUint : (b == Base::Bool ? kNumBool : kNumFloat)); }

// A TGSI source operand: a register file and index, a swizzle, and a negate flag. 'T' temporary, 'I' input, 'C' constant, 'M' immediate.
struct Oper {
  char file = 'T';   // (and 'O': an output a control shader reads, OUT[dim][idx] or OUT[idx])
  uint16_t idx = 0;
  int16_t dim = -1;   // a two-dimensional register's vertex
  uint8_t swz[4] = {0, 1, 2, 3};
  bool neg = false;
};

// A value of the shader: a scalar, vector (cols == 1) or matrix, one operand per column. An integer constant is only usable as an index or under a cast.
struct Val {
  uint8_t cols = 1, rows = 1;
  Oper c[4];
  bool is_int = false;   // an integer constant (ival), usable as a constant index; to_float() makes it a value
  int32_t ival = 0;
  uint8_t kind = 0;    // 1: a structure (WGSL): its members are members_[first ...], of the structure type `sid`; 4: an array, elements elems_[first ...] (count)
  uint16_t first = 0, sid = 0;
  uint8_t num = kNumFloat;
  uint16_t count = 0;
};

// A variable: one temporary per column.
struct Var {
  bool valid = false;
  uint8_t cols = 1, rows = 1;
  uint16_t temp[4] = {0, 0, 0, 0};
  bool is_struct = false;   // a structure: members_[first ...]
  uint16_t first = 0, sid = 0;
  uint8_t num = kNumFloat;
  uint16_t count = 0;       // an array of `count` elements: elems_[first ...]
};

// A place that can be assigned to: channels of temporaries.
struct LV {
  uint8_t cols = 1, rows = 1;
  uint16_t temp[4] = {0, 0, 0, 0};
  uint8_t chan[4] = {0, 1, 2, 3};
  uint8_t n = 1;   // vector: how many channels
  bool agg = false;   // a whole structure: members_[first ...]
  uint16_t first = 0, sid = 0;
  uint16_t count = 0;   // a whole array: elems_[first ...]
};

struct Imm {
  float v[4];
  bool bits = false;   // written as UINT32 bit patterns: a value the decimal form cannot say exactly (very small or large, an infinity, NaN)
};

struct UniformBase {
  uint32_t global;
  uint32_t row;
};

// The stages, as TgsiConstant::stage numbers them.
enum : uint8_t { kStVertex = 0, kStFragment = 1, kStGeometry = 2, kStControl = 3, kStEval = 4 };

// What one GLSL stage puts out and the next one reads: each link takes one GENERIC register (between the control and the evaluation stage a PATCH register, for a patch
// varying) per column of each element - a mat3 three, a vec4[8] eight - from `base` on. Per vertex, element v of the consumer's array is what the producer put out for
// vertex v. (The positions and the tessellation levels are not links: every stage has them in registers of their own.)
struct IoLink {
  int32_t out_gi = -1, in_gi = -1;   // the globals of the producer and the consumer
  uint32_t base = 0, regs = 0;
  bool patch = false, flat = false, saturate = false;
};
struct Boundary {
  explicit Boundary(allocators::IAllocator& a) : links(a) {}
  List<IoLink> links;
  uint32_t registers = 0, patch_registers = 0;
  // Between the control and the evaluation stage, when the evaluation stage reads the tessellation levels: the control stage also writes them as the two PATCH
  // registers after the links' (outer, inner), which the evaluation stage reads (the host cannot take TESSOUTER / TESSINNER as its inputs).
  bool levels = false;
  const IoLink* by_in(int32_t gi) const {
    for (const IoLink& l : links) if (l.in_gi == gi) return &l;
    return nullptr;
  }
};

// What both stages share while they are translated.
struct Shared {
  Shared(TgsiProgram* p, ArrayView<const TgsiLink> l, const TgsiLimits& lim) : out(p), links(l), limits(lim), bounds{Boundary(p->vs.allocator()), Boundary(p->vs.allocator()),
                                                                                                                        Boundary(p->vs.allocator()), Boundary(p->vs.allocator())} {
    if (limits.const_rows > kTgsiMaxConstRows) limits.const_rows = kTgsiMaxConstRows;
    if (limits.attributes > kTgsiMaxAttributes) limits.attributes = kTgsiMaxAttributes;
    if (limits.varyings > kTgsiMaxVaryings) limits.varyings = kTgsiMaxVaryings;
    if (limits.samplers > kTgsiMaxSamplers) limits.samplers = kTgsiMaxSamplers;
  }
  TgsiProgram* out;
  ArrayView<const TgsiLink> links;
  TgsiLimits limits;
  const char* why = nullptr;
  static constexpr uint32_t kMaxLinks = 64;
  // GLSL: the interfaces between the stages of the program, in order (vertex -> control -> evaluation -> geometry -> fragment, the ones it has); the last is the
  // fragment shader's, made from `links`.
  Boundary bounds[4];
  uint32_t nbounds = 0;
  TgsiStages stages;
  uint32_t eval_vertices = 0;   // the vertices of the evaluation stage's input patch (the control stage's output, or the draw's patch)
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

// Whether put_float() says f exactly (read back, the decimal is the same float).
bool decimal_exact(float f) {
  if (!(f == f) || f > 1e9f || f < -1e9f) return false;
  const double d = f < 0 ? -static_cast<double>(f) : static_cast<double>(f);
  uint64_t ip = static_cast<uint64_t>(d);
  uint64_t fd = static_cast<uint64_t>((d - static_cast<double>(ip)) * 1e12 + 0.5);
  if (fd >= 1000000000000ull) {
    fd -= 1000000000000ull;
    ++ip;
  }
  const double back = static_cast<double>(ip) + static_cast<double>(fd) * 1e-12;
  return static_cast<float>(f < 0 ? -back : back) == f;
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
  // `in` / `out`: the interfaces the stage reads and writes (GLSL; nullptr for the vertex shader's input and the fragment shader's output); `last`: the stage whose
  // vertices are rasterized.
  Gen(Shared& sh, const Module& m, uint8_t stage, int32_t entry = -1, const Boundary* in = nullptr, const Boundary* out = nullptr, bool last = true)
      : body(sh.out->vs.allocator()), sh_(sh), m_(m), stage_(stage), fs_(stage == kStFragment), vs_(stage == kStVertex), entry_(entry), in_(in), out_(out), last_(last),
        stage_vars_(sh.out->vs.allocator()), locals_(sh.out->vs.allocator()), elems_(sh.out->vs.allocator()), globals_(sh.out->vs.allocator()), imms_(sh.out->vs.allocator()),
        ubase_(sh.out->vs.allocator()), members_(sh.out->vs.allocator()) {}

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
      case 'O': body.append("OUT"); break;
      default: body.append("IMM"); break;
    }
    if (o.dim >= 0) {
      body.push_back('[');
      body.append_uint(static_cast<uint32_t>(o.dim));
      body.push_back(']');
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
  void out_mov(uint32_t out, const char* mask, const Oper& src, bool saturate = false) {
    begin_inst(saturate ? "MOV_SAT" : "MOV");
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
    v->kind = 0;
    v->num = kNumFloat;
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
      for (uint32_t k = 0; k < 4; ++k) same = same && __builtin_bit_cast(uint32_t, imms_[i].v[k]) == __builtin_bit_cast(uint32_t, v[k]);
      if (same) {
        *o = filled('M', static_cast<uint16_t>(i), rows);
        return true;
      }
    }
    if (imms_.count() >= kMaxImms) return fail("too many constants");
    Imm im;
    for (uint32_t k = 0; k < 4; ++k) {
      im.v[k] = v[k];
      if (!decimal_exact(v[k])) im.bits = true;
    }
    if (!imms_.push_back(im)) return fail("out of memory");
    *o = filled('M', static_cast<uint16_t>(imms_.count() - 1), rows);
    return true;
  }
  bool scalar_imm(float x, Oper* o) {
    const float v[4] = {x, 0, 0, 0};
    return imm(v, 1, o);
  }
  // A float, integer or boolean scalar, vector or (float) matrix.
  static bool is_float_type(const Type& t) {
    if (t.array != 0 || t.rows < 1 || t.rows > 4 || t.cols < 1 || t.cols > 4) return false;
    if (t.base == Base::Float) return true;
    return (t.base == Base::Bool || t.base == Base::Int || t.base == Base::Uint) && t.cols == 1;
  }
  static void val_of(const Var& v, Val* out) {
    if (v.is_struct || v.count) {
      *out = Val();
      out->kind = v.count ? 4 : 1;
      out->first = v.first;
      out->sid = v.sid;
      out->count = v.count;
      return;
    }
    out->kind = 0;
    out->rows = v.rows;
    out->cols = v.cols;
    out->is_int = false;
    out->num = v.num;
    for (uint32_t c = 0; c < v.cols; ++c) out->c[c] = filled('T', v.temp[c], v.rows);
  }
  static void lv_of(const Var& v, LV* l) {
    l->agg = v.is_struct;
    l->count = v.count;
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
    if (t.array) {      // (each element a variable of its own)
      if (t.array > kMaxArray || t.base == Base::Struct || t.base == Base::Sampler) return fail("an array the device cannot hold");
      const Type et = t.element();
      if (!is_float_type(et)) return fail("an array the device cannot hold");
      const uint64_t first = elems_.count();
      if (first + t.array > 0xFFFF) return fail("too many variables");
      for (uint32_t i = 0; i < t.array; ++i) {
        Var e;
        if (!make_var(et, &e) || !elems_.push_back(e)) return fail("too many variables");
      }
      *v = Var();
      v->valid = true;
      v->first = static_cast<uint16_t>(first);
      v->count = static_cast<uint16_t>(t.array);
      return true;
    }
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
    v->num = num_of(t.base);
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
  bool alloc_const(uint32_t slot, uint32_t rows, uint32_t cols, uint32_t elems, uint32_t* row, uint8_t convert = kTgsiAsIs);
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
  bool tex_call(const Node& n, Val* out);
  bool call_args(const Node& n, Val* args, uint32_t max, uint32_t* count);

  // --- statements
  bool stmt(int32_t id, bool tail);
  bool block(const Node& n, bool tail);
  bool loop(const Node& n);
  bool switch_stmt(const Node& n);
  bool call(const Node& n, Val* out);
  bool assign_to(int32_t target, const Val& v);
  bool cond_store(const LV& l, const Val& v, const Oper& t);
  bool index_of(int32_t id, bool* constant, int32_t* k, Val* dynamic);
  bool clamp_index(const Val& idx, uint32_t count, Val* out);
  bool select(const Val& idx, uint32_t count, const Val* items, Val* out);
  bool flag_test(uint16_t flag, bool when_set);
  bool convert(const Val& in, uint8_t num, Val* out);
  bool atan2_of(const Val& y, const Val& x, Val* out);
  bool frag_input(bool face, Val* out);
  // what a statement may do that the statements after it must know of: return (from the function it is in) or continue (the loop it is in)
  static constexpr uint32_t kExitReturn = 1, kExitContinue = 2;
  uint32_t exits(int32_t id, bool in_loop) const;
  uint32_t count_returns(int32_t id) const;
  Var* local(int32_t i) {
    const uint64_t at = local_base_ + static_cast<uint64_t>(i < 0 ? 0 : i);
    return i >= 0 && at < locals_.count() ? &locals_[at] : nullptr;
  }

  // --- the geometry and tessellation stages
  bool stage_start();                    // their inputs and system values, into variables
  bool write_vertex_outputs();           // the position and the varyings of a stage that puts out vertices (vertex, evaluation, geometry at EmitVertex)
  bool write_clip_distances();           // ... and its clip distances (TgsiStages::clip)
  bool control_end();                    // a control stage's outputs (at the end, and at each barrier())
  bool control_read(const Node& index, Val* out);   // another invocation's output, as the last barrier() left it
  bool patch_read(uint64_t gi, Val* out);           // a control stage's patch output or level
  bool system_value(const char* semantic, bool integer, uint32_t rows, const Var& v);
  bool in2d(uint16_t temp, const char* mask, uint32_t vertex, uint32_t reg, const char* swizzle = "xyzw");
  bool control_output(const Node& index_node, Var* out);   // gl_out[gl_InvocationID].x / x[gl_InvocationID] of a control stage
  bool is_control_output(int32_t node) const;
  uint32_t input_vertices() const;

 public:
  // after run(): what the assembler declares beyond the shader's own registers
  int32_t position_in = -1, face_in = -1;   // a fragment shader's gl_FragCoord / gl_FrontFacing inputs
  int32_t primitive_in = -1;                 // a geometry shader's gl_PrimitiveIDIn input
  bool clip_out = false;                     // the stage writes CLIPDIST[0], CLIPDIST[1] after its varyings
  bool uses_addr = false;                    // ARL (a uniform array indexed by a variable)
  static constexpr uint32_t kMaxSystemValues = 4;
  const char* sv_names[kMaxSystemValues] = {};   // SV[i]'s semantic
  uint32_t nsv = 0;
  uint8_t stage() const { return stage_; }
  const Module& module() const { return m_; }
  const Boundary* in_boundary() const { return in_; }
  const Boundary* out_boundary() const { return out_; }

 private:

  Shared& sh_;
  const Module& m_;
  uint8_t stage_;
  bool fs_, vs_;
  int32_t entry_;   // WGSL: the entry point function; GLSL: -1 (main)
  const Boundary* in_;
  const Boundary* out_;
  bool last_;
  // A geometry or tessellation stage's own variables, by global: a control stage's output for its own vertex (`own`), the flag its writes to a patch output set
  // (`flag`), and whether the global is a system value or input made ready at the start (`ready`).
  struct StageVar {
    Var own;
    int32_t flag = -1;
    bool ready = false;
  };
  List<StageVar> stage_vars_;
  int32_t clip_row_ = -1;        // the user clip planes' first CONST row (write_clip_distances)
  int32_t invocation_gi_ = -1;   // a control stage's gl_InvocationID (always in a variable: its outputs are written by it)
  uint32_t ntemp_ = 0;
  uint32_t inst_ = 0;
  List<Var> locals_;
  uint64_t local_base_ = 0;   // where the variables of the function being translated begin in locals_ (an inlined call's are after its caller's)
  List<Var> elems_;           // the elements of array variables
  List<Var> globals_;
  // The function being translated (main, an entry point, or an inlined call): its result variable and the flag a return in the middle of it sets, which the
  // statements after one test (kExitReturn); `loops`: the loops open inside it.
  struct FnCtx {
    Var ret;
    bool has_ret = false;
    bool use_flag = false;
    uint16_t flag = 0;
    uint32_t loops = 0;
    bool wgsl_entry = false;
  };
  FnCtx fns_[kMaxCallDepth + 2];
  uint32_t nfns_ = 0;
  // The loops open: a for or do-while loop's continue sets a flag (its step or condition must still run), a while loop's is TGSI's CONT.
  struct LoopCtx {
    bool use_flag = false;
    uint16_t flag = 0;
    bool is_while = false;
    bool is_switch = false;   // the loop a switch is made of (its break leaves it; a continue in it would be the loop around's)
  };
  LoopCtx loops_[64];
  uint32_t nloops_ = 0;
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
bool Gen::alloc_const(uint32_t slot, uint32_t rows, uint32_t cols, uint32_t elems, uint32_t* row, uint8_t convert) {
  if (elems > sh_.limits.const_rows) return fail("too many uniforms");
  const uint32_t total = elems * cols;
  TgsiProgram& p = *sh_.out;
  if (p.const_rows + total > sh_.limits.const_rows) return fail("too many uniforms");
  const uint32_t first = p.const_rows;
  for (uint32_t e = 0; e < elems; ++e)
    for (uint32_t c = 0; c < cols; ++c) {
      TgsiConstant k;
      k.stage = stage_;
      k.row = static_cast<uint16_t>(first + e * cols + c);
      k.comps = static_cast<uint8_t>(rows);
      k.slot = slot + (e * cols + c) * rows;
      k.convert = convert;
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
  // (a uniform block's member is read like any uniform: the context copies the block's buffer into the stage's uniforms before each draw, load_uniform_blocks)
  if (!is_float_type(t.element())) return fail("a uniform of a type the device cannot take");
  const uint8_t convert = t.base == Base::Int ? kTgsiFromInt : (t.base == Base::Uint ? kTgsiFromUint : (t.base == Base::Bool ? kTgsiFromBool : kTgsiAsIs));
  uint32_t first;
  if (!alloc_const(g.slot, t.rows, t.cols, t.array ? t.array : 1, &first, convert)) return false;
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

// An integer constant as a value in a register (an immediate); the value keeps saying it is an integer.
bool Gen::to_float(Val& v) {
  if (!v.is_int) return true;
  Oper o;
  const float f = v.num == kNumUint ? static_cast<float>(static_cast<uint32_t>(v.ival)) : static_cast<float>(v.ival);
  if (!scalar_imm(f, &o)) return false;
  v.is_int = false;
  v.rows = v.cols = 1;
  v.c[0] = o;
  return true;
}

// A value made another kind of number (GLSL's constructors and conversions): to an integer, truncated; to a boolean, nonzero; from integers and booleans to float,
// the same floats.
bool Gen::convert(const Val& in_v, uint8_t num, Val* out) {
  Val in = in_v;
  if (!to_float(in)) return false;
  if (in.kind) return fail("a conversion of a structure or array");
  const bool to_int = num == kNumInt || num == kNumUint, to_bool = num == kNumBool;
  if ((to_int && in.num != kNumFloat) || (to_bool && in.num == kNumBool) || (!to_int && !to_bool)) {
    *out = in;
    out->num = num;
    return true;
  }
  if (!new_val(in.rows, in.cols, out)) return false;
  Oper zero;
  if (to_bool && !scalar_imm(0.0f, &zero)) return false;
  for (uint32_t c = 0; c < in.cols; ++c) {
    if (to_int) inst_rows("TRUNC", out->c[c].idx, in.rows, &in.c[c]);
    else inst_rows("SNE", out->c[c].idx, in.rows, &in.c[c], &zero);
  }
  out->num = num;
  return true;
}

bool Gen::attribute_index(const Global& g, uint32_t* index) {
  TgsiProgram& p = *sh_.out;
  for (uint64_t i = 0; i < p.attributes.count(); ++i)
    if (p.attributes[i].slot == g.slot) {
      *index = static_cast<uint32_t>(i);
      return true;
    }
  if (p.attributes.count() >= sh_.limits.attributes) return fail("too many attributes");
  const uint8_t convert = g.type.base == Base::Int ? kTgsiFromInt : (g.type.base == Base::Uint ? kTgsiFromUint : kTgsiAsIs);
  if (!p.attributes.push_back(TgsiAttribute{g.slot, g.type.rows, convert})) return fail("out of memory");
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
  if (p.attributes.count() >= sh_.limits.attributes) return fail("too many attributes");
  if (!p.attributes.push_back(TgsiAttribute{key, static_cast<uint8_t>(comps)})) return fail("out of memory");
  *index = static_cast<uint32_t>(p.attributes.count() - 1);
  return true;
}

// gl_FragCoord (TGSI's POSITION: the device's pixel centres, which are GL's window coordinates as the device keeps GL's rows in order, and 1/w) and gl_FrontFacing
// (FACE, +1 for the triangles the pipeline calls front-facing, which are GL's), declared after the varyings.
bool Gen::frag_input(bool face, Val* out) {
  int32_t& idx = face ? face_in : position_in;
  if (idx < 0) idx = static_cast<int32_t>((entry_ >= 0 ? sh_.var_count : (in_ ? in_->registers : 0)) + (position_in >= 0 || face_in >= 0 ? 1 : 0));
  if (!face) {
    *out = Val();
    out->rows = 4;
    out->c[0] = filled('I', static_cast<uint16_t>(idx), 4);
    sh_.out->uses_frag_coord = true;
    return true;
  }
  Oper zero;
  if (!scalar_imm(0.0f, &zero) || !new_val(1, 1, out)) return false;
  const Oper f = component(filled('I', static_cast<uint16_t>(idx), 1), 0);
  inst("SLT", out->c[0].idx, "x", &zero, &f);
  out->num = kNumBool;
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
      out->kind = 0;
      out->num = num_of(g.type.base);
      for (uint32_t c = 0; c < g.type.cols; ++c) out->c[c] = filled('C', static_cast<uint16_t>(row + c), g.type.rows);
      return true;
    }
    case GlobalKind::In: {
      if (g.builtin != kBvNone) return fail("a built-in input");
      if (g.type.base == Base::Bool || !is_float_type(g.type.element()) || (vs_ && (g.type.cols != 1 || g.type.array))) return fail("an input of a type the device cannot take");
      out->rows = g.type.rows;
      out->cols = 1;
      out->is_int = false;
      out->kind = 0;
      out->num = num_of(g.type.base);
      if (vs_) {
        uint32_t k;
        if (!attribute_index(g, &k)) return false;
        out->c[0] = filled('I', static_cast<uint16_t>(k), g.type.rows);
        return true;
      }
      if (!fs_) {      // a geometry or tessellation stage: its inputs were copied into variables at the start (stage_start)
        if (gi >= stage_vars_.count() || !stage_vars_[gi].ready) return fail("an input the stage before does not write");
        Var v;
        if (!global_var(gi, &v)) return false;
        val_of(v, out);
        return true;
      }
      const IoLink* l = in_ ? in_->by_in(static_cast<int32_t>(gi)) : nullptr;
      if (!l) return fail("a varying the vertex shader does not write");
      if (l->flat != g.flat) return fail("a varying the device cannot take");
      if (g.type.array) {      // (copied into variables at the start: see run())
        Var v;
        if (!global_var(gi, &v)) return false;
        val_of(v, out);
        return true;
      }
      out->cols = g.type.cols;
      for (uint32_t c = 0; c < g.type.cols; ++c) out->c[c] = filled('I', static_cast<uint16_t>(l->base + c), g.type.rows);
      return true;
    }
    case GlobalKind::Builtin:
    case GlobalKind::Out:
    case GlobalKind::Private: {
      if (g.kind == GlobalKind::Builtin && fs_ && (g.builtin == kBvFragCoord || g.builtin == kBvFrontFacing)) return frag_input(g.builtin == kBvFrontFacing, out);
      if (g.kind == GlobalKind::Builtin && g.builtin != kBvPosition && g.builtin != kBvPointSize && g.builtin != kBvFragColor && !(fs_ && g.builtin == kBvFragData) &&
          !(!fs_ && g.builtin == kBvClipDistance))
        return fail("a built-in variable");
      if ((g.builtin == kBvFragColor) != (fs_ && g.builtin == kBvFragColor) || (!fs_ && g.builtin == kBvFragColor)) return fail("gl_FragColor in a vertex shader");
      if (fs_ && (g.builtin == kBvPosition || g.builtin == kBvPointSize)) return fail("a vertex output in a fragment shader");
      if (stage_ == kStControl && g.kind == GlobalKind::Out && g.per_vertex) return fail("a control stage output of another invocation, or all of them");
      if (stage_ == kStControl && g.kind == GlobalKind::Out && g.patch) return patch_read(gi, out);
      if (entry_ < 0 && g.kind == GlobalKind::Private && g.is_read_only && (gi >= stage_vars_.count() || !stage_vars_[gi].ready)) return fail("a system value the device does not give");
      if (g.kind == GlobalKind::Out && g.name.view().equals("gl_ViewportIndex")) return fail("gl_ViewportIndex");
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
  const bool integer = t.base == Base::Int || t.base == Base::Uint;
  if (integer && lanes == 1) {      // (kept as a number: it may be an index)
    *out = Val();
    out->is_int = true;
    out->num = num_of(t.base);
    out->ival = static_cast<int32_t>(m_.consts[base]);
    return true;
  }
  if (t.base != Base::Float && t.base != Base::Bool && !integer) return fail("a constant of a type the device cannot take");
  if (t.rows > 4 || t.cols > 4) return fail("bad constant");
  out->rows = t.rows;
  out->cols = t.cols;
  out->is_int = false;
  out->kind = 0;
  out->num = num_of(t.base);
  for (uint32_t c = 0; c < t.cols; ++c) {
    float v[4] = {0, 0, 0, 0};
    for (uint32_t r = 0; r < t.rows; ++r) {
      const uint32_t bits = m_.consts[base + c * t.rows + r];
      if (t.base == Base::Bool) {
        v[r] = bits ? 1.0f : 0.0f;
      } else if (t.base == Base::Int) {
        v[r] = static_cast<float>(static_cast<int32_t>(bits));
      } else if (t.base == Base::Uint) {
        v[r] = static_cast<float>(bits);
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
  if (l.count || v.kind == 4) {      // a whole array: element by element
    if (!l.count || v.kind != 4 || v.count != l.count) return fail("assignment of arrays of other sizes");
    for (uint32_t e = 0; e < l.count; ++e) {
      LV el;
      Val ev;
      lv_of(elems_[static_cast<uint64_t>(l.first) + e], &el);
      val_of(elems_[static_cast<uint64_t>(v.first) + e], &ev);
      if (!store(el, ev)) return false;
    }
    return true;
  }
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
      const Var* v = local(n.ival);
      if (!v || !v->valid) return fail("a variable used before it is declared");
      lv_of(*v, out);
      return true;
    }
    case NodeKind::Global: {
      if (n.ival < 0 || static_cast<uint64_t>(n.ival) >= m_.globals.count()) return fail("bad global");
      const Global& g = *m_.globals[static_cast<uint64_t>(n.ival)];
      if ((g.kind != GlobalKind::Out && g.kind != GlobalKind::Private && g.kind != GlobalKind::Builtin) || g.is_read_only) return fail("assignment to an input or uniform");
      if (stage_ == kStControl && g.kind == GlobalKind::Out && g.patch) {      // a patch output: put out at the end if this invocation wrote it (control_end)
        const uint64_t gi = static_cast<uint64_t>(n.ival);
        Oper one;
        Var v;
        if (gi >= stage_vars_.count() || stage_vars_[gi].flag < 0 || !scalar_imm(1.0f, &one) || !global_var(gi, &v)) return fail("a patch output the device cannot take");
        inst("MOV", static_cast<uint16_t>(stage_vars_[gi].flag), "x", &one);
        lv_of(v, out);
        return true;
      }
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
      if (is_control_output(n.a)) {      // gl_out[gl_InvocationID].x = ...: the invocation's own control point
        Var v;
        if (!control_output(n, &v)) return false;
        lv_of(v, out);
        return true;
      }
      LV base;
      int32_t k;
      if (!lvalue(n.a, &base) || !const_index(n.b, &k) || k < 0) return fail("an indexed assignment the device cannot take");
      if (base.count) {      // an array element
        if (static_cast<uint32_t>(k) >= base.count) return fail("index out of range");
        lv_of(elems_[static_cast<uint64_t>(base.first) + static_cast<uint32_t>(k)], out);
        return true;
      }
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
  Val a = a_in, b = b_in;
  const bool b_const = b.is_int;
  const int32_t b_k = b.ival;
  if (!to_float(a) || !to_float(b)) return false;
  if (a.kind || b.kind) return fail("arithmetic on a structure or array");
  // the kind of number of the result: that of both, a float with a float operand (desktop GL converts implicitly), unsigned for int with uint
  const uint8_t rnum = a.num == b.num ? a.num : (a.num == kNumFloat || b.num == kNumFloat ? kNumFloat : kNumUint);
  const bool integer = rnum == kNumInt || rnum == kNumUint;
  const auto with_num = [&](bool ok) {
    if (ok) out->num = rnum;
    return ok;
  };
  switch (op) {
    case Op::Add: return with_num(elementwise("ADD", a, b, out));
    case Op::Sub: return with_num(elementwise("SUB", a, b, out));
    case Op::Div: {
      if (!integer) return elementwise("DIV", a, b, out);
      Val q;      // (integer division truncates)
      if (!elementwise("DIV", a, b, &q) || !new_val(q.rows, q.cols, out)) return false;
      for (uint32_t c = 0; c < q.cols; ++c) inst_rows("TRUNC", out->c[c].idx, q.rows, &q.c[c]);
      out->num = rnum;
      return true;
    }
    case Op::Mod: {      // a % b of integers: a - b * trunc(a / b)
      if (!integer) return fail("% of floats");
      Val q, t, m;
      if (!elementwise("DIV", a, b, &q) || !new_val(q.rows, q.cols, &t)) return false;
      for (uint32_t c = 0; c < q.cols; ++c) inst_rows("TRUNC", t.c[c].idx, q.rows, &q.c[c]);
      return elementwise("MUL", b, t, &m) && with_num(elementwise("SUB", a, m, out));
    }
    case Op::Shl: case Op::Shr: {      // a multiplication or a division by 2^b (rounded down, as the arithmetic shift does); the result is the left operand's type
      if (a.num != kNumInt && a.num != kNumUint) return fail("a shift the device cannot take");
      Val kv;
      if (b_const) {
        if (b_k < 0 || b_k > 23) return fail("a shift the device cannot take");
        Oper k;
        if (!scalar_imm(static_cast<float>(1u << b_k), &k)) return false;
        kv.c[0] = k;
      } else if (!per_component("EX2", b, nullptr, &kv)) {
        return false;
      }
      if (op == Op::Shl) {
        if (!elementwise("MUL", a, kv, out)) return false;
        out->num = a.num;
        return true;
      }
      Val q;
      if (!elementwise("DIV", a, kv, &q) || !new_val(q.rows, q.cols, out)) return false;
      for (uint32_t c = 0; c < q.cols; ++c) inst_rows("FLR", out->c[c].idx, q.rows, &q.c[c]);
      out->num = a.num;
      return true;
    }
    case Op::And: case Op::Or: case Op::Xor: return fail("bitwise integer operators");
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
      return with_num(elementwise("MUL", a, b, out));   // matrix * scalar
    }
    case Op::Eq: case Op::Ne:
      if (a.cols == 1 && b.cols == 1 && a.rows == b.rows && a.rows > 1) {      // vectors: all components equal (any different), one boolean
        Val e, d;
        Oper n;
        if (!elementwise(op == Op::Eq ? "SNE" : "SNE", a, b, &e) || !dot(e, e, &d) || !scalar_imm(0.0f, &n)) return false;
        Val z;
        z.c[0] = n;
        if (!elementwise(op == Op::Eq ? "SEQ" : "SNE", d, z, out)) return false;
        out->num = kNumBool;
        return true;
      }
      [[fallthrough]];
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
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
      if (!elementwise(opc, *x, *y, out)) return false;
      out->num = kNumBool;
      return true;
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
  // integer and boolean vectors: built like float ones, then converted (truncated, or made 0 / 1) unless every argument already is one
  const uint8_t num = num_of(n.type.base);
  if (num != kNumFloat && n.type.array == 0 && n.type.base != Base::Void) {
    Node fn = n;
    fn.type.base = Base::Float;
    Val f;
    if (!construct(fn, &f)) return false;
    bool already = true;
    for (int32_t i = 0; i < n.count && already; ++i) {
      Val a;
      const int32_t kid = m_.kids[static_cast<uint64_t>(n.first + i)];
      // (an argument's kind of number: its node again - evaluating has no effects worth repeating only for these simple forms)
      const NodeKind kk = m_.nodes[static_cast<uint64_t>(kid)].kind;
      if (kk != NodeKind::Local && kk != NodeKind::Global && kk != NodeKind::Const) { already = false; break; }
      if (!eval(kid, &a)) return false;
      already = (num == kNumBool) ? a.num == kNumBool : a.num != kNumFloat;
    }
    if (already) {
      *out = f;
      out->num = num;
      return true;
    }
    f.num = kNumFloat;
    return convert(f, num, out);
  }
  Type t = n.type;
  if (t.base == Base::Void && t.array == 0) t.base = Base::Float;   // WGSL vec3(1.0, 2.0, 3.0): the element type is the arguments'
  if (t.array && t.base != Base::Void) {      // float[3](a, b, c): a new array variable, element by element
    if (t.array == 0xFFFFFFFFu) t.array = static_cast<uint32_t>(n.count);
    if (t.array != static_cast<uint32_t>(n.count) || t.array > kMaxArray) return fail("an array constructor the device cannot take");
    Var v;
    if (!make_var(t, &v)) return false;
    for (int32_t i = 0; i < n.count; ++i) {
      Val e;
      LV l;
      if (!eval(m_.kids[static_cast<uint64_t>(n.first + i)], &e)) return false;
      lv_of(elems_[static_cast<uint64_t>(v.first) + static_cast<uint32_t>(i)], &l);
      if (!store(l, e)) return false;
    }
    val_of(v, out);
    return true;
  }
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
  if (n.count == 1 && args[0].cols > 1) {      // a matrix from another: its upper left part, the rest from the identity
    const Val& m = args[0];
    out->rows = static_cast<uint8_t>(rows);
    out->cols = static_cast<uint8_t>(cols);
    out->is_int = false;
    out->kind = 0;
    out->num = kNumFloat;
    for (uint32_t c = 0; c < cols; ++c) {
      Oper pieces[4];
      for (uint32_t r = 0; r < rows; ++r) {
        if (c < m.cols && r < m.rows) {
          pieces[r] = component(m.c[c], r);
        } else if (!scalar_imm(r == c ? 1.0f : 0.0f, &pieces[r])) {
          return false;
        }
      }
      Val col;
      if (!pieces_to_vec(pieces, rows, rows, &col)) return false;
      out->c[c] = col.c[0];
    }
    return true;
  }
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
  switch (static_cast<BuiltinFn>(n.ival)) {
    case BuiltinFn::EmitVertex:
    case BuiltinFn::EndPrimitive: {      // (stream 0: the x of an immediate zero)
      const bool emit = static_cast<BuiltinFn>(n.ival) == BuiltinFn::EmitVertex;
      if (stage_ != kStGeometry) return fail("EmitVertex() or EndPrimitive() outside a geometry shader");
      if (emit && !write_vertex_outputs()) return false;
      Oper zero;
      if (!scalar_imm(0.0f, &zero)) return false;
      begin_inst(emit ? "EMIT" : "ENDPRIM");
      body.push_back(' ');
      put_oper(component(zero, 0));
      end_inst();
      *out = Val();
      return true;
    }
    case BuiltinFn::EmitStreamVertex:
    case BuiltinFn::EndStreamPrimitive: return fail("vertex streams");
    case BuiltinFn::Barrier:
      // (a control stage's invocations put out what they wrote, then wait for each other: what they read of each other after it is in the output registers)
      if (stage_ != kStControl) return fail("barrier()");
      if (!control_end()) return false;
      begin_inst("BARRIER");
      end_inst();
      *out = Val();
      return true;
    default: break;
  }
  // modf(x, out i): the whole part goes to the second argument (an lvalue), the fraction is the result
  if (static_cast<BuiltinFn>(n.ival) == BuiltinFn::Modf) {
    Val x, ip;
    if (n.count != 2 || !eval(m_.kids[static_cast<uint64_t>(n.first)], &x) || !to_float(x)) return fail("modf of other arguments");
    if (x.kind || x.cols != 1 || !new_val(x.rows, 1, &ip)) return false;
    inst_rows("TRUNC", ip.c[0].idx, x.rows, &x.c[0]);
    if (!assign_to(m_.kids[static_cast<uint64_t>(n.first) + 1], ip)) return false;
    return elementwise("SUB", x, ip, out);
  }
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
    case BuiltinFn::Abs: return simple1("ABS") && ((out->num = a[0].num), true);
    case BuiltinFn::Floor: return simple1("FLR");
    case BuiltinFn::Ceil: return simple1("CEIL");
    case BuiltinFn::Fract: return simple1("FRC");
    case BuiltinFn::Sign: return simple1("SSG") && ((out->num = a[0].num), true);
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
    case BuiltinFn::Min: return need(2) && elementwise("MIN", a[0], a[1], out) && ((out->num = a[0].num), true);
    case BuiltinFn::Max: return need(2) && elementwise("MAX", a[0], a[1], out) && ((out->num = a[0].num), true);
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
      return need(3) && elementwise("MAX", a[0], a[1], &t) && elementwise("MIN", t, a[2], out) && ((out->num = a[0].num), true);
    }
    case BuiltinFn::Mix: {   // mix(x, y, a) = x * (1 - a) + y * a, which is LRP(a, y, x)
      if (!need(3) || a[0].cols != 1 || a[1].cols != 1 || a[0].rows != a[1].rows) return fail("mix of other shapes");
      if (!new_val(a[0].rows, 1, out)) return false;
      if (a[2].num == kNumBool) {      // mix(x, y, bvec): y where true - a choice, not arithmetic
        Oper nc = a[2].c[0];
        if (a[2].rows == 1) nc = component(nc, 0);
        nc.neg = !nc.neg;
        inst_rows("CMP", out->c[0].idx, a[0].rows, &nc, &a[1].c[0], &a[0].c[0]);
        return true;
      }
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
      Oper nc = component(a[2].c[0], 0);
      nc.neg = !nc.neg;
      for (uint32_t c = 0; c < a[0].cols; ++c) inst_rows("CMP", out->c[c].idx, a[0].rows, &nc, &a[1].c[c], &a[0].c[c]);
      out->num = a[0].num;
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
    case BuiltinFn::IsNan: case BuiltinFn::IsInf: {      // NaN is not itself; an infinity is its absolute value's
      if (!need(1) || a[0].cols != 1) return fail("isnan / isinf of other shapes");
      if (fn == BuiltinFn::IsNan) {
        if (!elementwise("SNE", a[0], a[0], out)) return false;
      } else {
        Val ab, inf;
        Oper k;
        if (!new_val(a[0].rows, 1, &ab) || !scalar_imm(__builtin_inff(), &k)) return false;
        inst_rows("ABS", ab.c[0].idx, a[0].rows, &a[0].c[0]);
        inf.c[0] = k;
        if (!elementwise("SEQ", ab, inf, out)) return false;
      }
      out->num = kNumBool;
      return true;
    }
    case BuiltinFn::Ldexp: {      // x * 2^e
      Val p2;
      return need(2) && per_component("EX2", a[1], nullptr, &p2) && elementwise("MUL", a[0], p2, out);
    }
    case BuiltinFn::Sinh: case BuiltinFn::Cosh: case BuiltinFn::Tanh: {      // from e^x and e^-x (tanh's argument kept in [-20, 20], where it is +-1 to the float)
      Val x = a[0], ep, en, num, den;
      Oper l2e, half, lo, hi;
      if (!need(1) || !scalar_imm(1.44269504f, &l2e) || !scalar_imm(0.5f, &half) || !scalar_imm(-20.0f, &lo) || !scalar_imm(20.0f, &hi)) return false;
      Val kl, kh, kv, hv;
      kl.c[0] = lo;
      kh.c[0] = hi;
      kv.c[0] = l2e;
      hv.c[0] = half;
      if (fn == BuiltinFn::Tanh) {
        Val t;
        if (!elementwise("MAX", a[0], kl, &t) || !elementwise("MIN", t, kh, &x)) return false;
      }
      Val xs, nx;
      if (!elementwise("MUL", x, kv, &xs)) return false;
      nx = xs;
      for (uint32_t c = 0; c < nx.cols; ++c) nx.c[c].neg = !nx.c[c].neg;
      if (!per_component("EX2", xs, nullptr, &ep) || !per_component("EX2", nx, nullptr, &en)) return false;
      if (fn == BuiltinFn::Tanh) return elementwise("SUB", ep, en, &num) && elementwise("ADD", ep, en, &den) && elementwise("DIV", num, den, out);
      return elementwise(fn == BuiltinFn::Sinh ? "SUB" : "ADD", ep, en, &num) && elementwise("MUL", num, hv, out);
    }
    case BuiltinFn::Asinh: case BuiltinFn::Acosh: case BuiltinFn::Atanh: {      // log(x + sqrt(x x +- 1)); atanh: log((1 + x) / (1 - x)) / 2
      Oper one, ln2, half;
      if (!need(1) || !scalar_imm(1.0f, &one) || !scalar_imm(0.693147181f, &ln2) || !scalar_imm(0.5f, &half)) return false;
      Val ov, lv, hv, arg, lg;
      ov.c[0] = one;
      lv.c[0] = ln2;
      hv.c[0] = half;
      if (fn == BuiltinFn::Atanh) {
        Val p1, m1;
        if (!elementwise("ADD", ov, a[0], &p1) || !elementwise("SUB", ov, a[0], &m1) || !elementwise("DIV", p1, m1, &arg)) return false;
      } else {
        Val sq, s1, r;
        if (!elementwise("MUL", a[0], a[0], &sq) || !elementwise(fn == BuiltinFn::Asinh ? "ADD" : "SUB", sq, ov, &s1) || !per_component("SQRT", s1, nullptr, &r) ||
            !elementwise("ADD", a[0], r, &arg))
          return false;
      }
      Val l;
      if (!per_component("LG2", arg, nullptr, &lg) || !elementwise("MUL", lg, lv, &l)) return false;
      if (fn == BuiltinFn::Atanh) return elementwise("MUL", l, hv, out);
      *out = l;
      return true;
    }
    case BuiltinFn::OuterProduct: {      // column j = c * r[j]
      if (!need(2) || a[0].cols != 1 || a[1].cols != 1 || a[0].rows < 2 || a[1].rows < 2) return fail("outerProduct of other shapes");
      if (!new_val(a[0].rows, a[1].rows, out)) return false;
      for (uint32_t j = 0; j < a[1].rows; ++j) {
        const Oper rj = component(a[1].c[0], j);
        inst_rows("MUL", out->c[j].idx, a[0].rows, &a[0].c[0], &rj);
      }
      return true;
    }
    case BuiltinFn::Determinant: case BuiltinFn::Inverse: {
      if (!need(1) || a[0].cols != a[0].rows || a[0].cols < 2 || a[0].cols > 3) return fail("a determinant or inverse the device cannot take");
      const Val& m = a[0];
      Val det;
      if (m.cols == 2) {      // a d - c b
        Val p, q;
        Val m00, m11, m10, m01;
        m00.c[0] = component(m.c[0], 0);
        m11.c[0] = component(m.c[1], 1);
        m10.c[0] = component(m.c[1], 0);
        m01.c[0] = component(m.c[0], 1);
        if (!elementwise("MUL", m00, m11, &p) || !elementwise("MUL", m10, m01, &q) || !elementwise("SUB", p, q, &det)) return false;
        if (fn == BuiltinFn::Determinant) {
          *out = det;
          return true;
        }
        // (1 / det) [d -b; -c a], by columns: (d, -b), (-c, a)
        Val r;
        if (!per_component("RCP", det, nullptr, &r) || !new_val(2, 2, out)) return false;
        Oper pieces[2][2] = {{component(m.c[1], 1), component(m.c[0], 1)}, {component(m.c[1], 0), component(m.c[0], 0)}};
        pieces[0][1].neg = !pieces[0][1].neg;
        pieces[1][0].neg = !pieces[1][0].neg;
        for (uint32_t c = 0; c < 2; ++c) {
          Val col, sc;
          if (!pieces_to_vec(pieces[c], 2, 2, &col) || !elementwise("MUL", col, r, &sc)) return false;
          inst_rows("MOV", out->c[c].idx, 2, &sc.c[0]);
        }
        return true;
      }
      // 3 x 3: det = c0 . (c1 x c2); the inverse's rows are c1 x c2, c2 x c0, c0 x c1 over det
      Val x12, x20, x01;
      if (!new_val(3, 1, &x12) || !new_val(3, 1, &x20) || !new_val(3, 1, &x01)) return false;
      inst_rows("XPD", x12.c[0].idx, 3, &m.c[1], &m.c[2]);
      inst_rows("XPD", x20.c[0].idx, 3, &m.c[2], &m.c[0]);
      inst_rows("XPD", x01.c[0].idx, 3, &m.c[0], &m.c[1]);
      Val c0;
      c0.rows = 3;
      c0.c[0] = m.c[0];
      if (!dot(c0, x12, &det)) return false;
      if (fn == BuiltinFn::Determinant) {
        *out = det;
        return true;
      }
      Val r;
      if (!per_component("RCP", det, nullptr, &r) || !new_val(3, 3, out)) return false;
      const Val* rowv[3] = {&x12, &x20, &x01};
      for (uint32_t c = 0; c < 3; ++c) {
        Oper pieces[3] = {component(rowv[0]->c[0], c), component(rowv[1]->c[0], c), component(rowv[2]->c[0], c)};
        Val col, sc;
        if (!pieces_to_vec(pieces, 3, 3, &col) || !elementwise("MUL", col, r, &sc)) return false;
        inst_rows("MOV", out->c[c].idx, 3, &sc.c[0]);
      }
      return true;
    }
    case BuiltinFn::Tan: {
      Val sn, cs;
      return need(1) && per_component("SIN", a[0], nullptr, &sn) && per_component("COS", a[0], nullptr, &cs) && elementwise("DIV", sn, cs, out);
    }
    case BuiltinFn::Atan: {
      if (argc == 2) return atan2_of(a[0], a[1], out);      // atan(y, x)
      Oper one;
      if (!need(1) || !scalar_imm(1.0f, &one)) return false;
      Val ov;
      ov.c[0] = one;
      return atan2_of(a[0], ov, out);
    }
    case BuiltinFn::Atan2: return need(2) && atan2_of(a[0], a[1], out);
    case BuiltinFn::Asin: case BuiltinFn::Acos: {      // asin(x) = atan2(x, sqrt(1 - x x)), acos(x) = atan2(sqrt(1 - x x), x)
      Val sq, om, r;
      Oper one;
      if (!need(1) || !scalar_imm(1.0f, &one)) return false;
      Val ov;
      ov.c[0] = one;
      if (!elementwise("MUL", a[0], a[0], &sq) || !elementwise("SUB", ov, sq, &om)) return false;
      Oper zero;
      Val zv;
      if (!scalar_imm(0.0f, &zero)) return false;
      zv.c[0] = zero;
      Val cl;
      if (!elementwise("MAX", om, zv, &cl) || !per_component("SQRT", cl, nullptr, &r)) return false;
      return fn == BuiltinFn::Asin ? atan2_of(a[0], r, out) : atan2_of(r, a[0], out);
    }
    case BuiltinFn::LessThan: case BuiltinFn::LessThanEqual: case BuiltinFn::GreaterThan: case BuiltinFn::GreaterThanEqual: case BuiltinFn::Equal: case BuiltinFn::NotEqual: {
      if (!need(2) || a[0].cols != 1 || a[1].cols != 1 || a[0].rows != a[1].rows) return fail("a comparison of other shapes");
      bool ok;
      switch (fn) {
        case BuiltinFn::LessThan: ok = elementwise("SLT", a[0], a[1], out); break;
        case BuiltinFn::LessThanEqual: ok = elementwise("SGE", a[1], a[0], out); break;
        case BuiltinFn::GreaterThan: ok = elementwise("SLT", a[1], a[0], out); break;
        case BuiltinFn::GreaterThanEqual: ok = elementwise("SGE", a[0], a[1], out); break;
        case BuiltinFn::Equal: ok = elementwise("SEQ", a[0], a[1], out); break;
        default: ok = elementwise("SNE", a[0], a[1], out); break;
      }
      if (ok) out->num = kNumBool;
      return ok;
    }
    case BuiltinFn::Any: case BuiltinFn::All: {      // the number of true components against 0 / against all
      Val d;
      Oper k;
      if (!need(1) || a[0].cols != 1) return fail("any / all of other shapes");
      if (a[0].rows == 1) d = a[0];
      else if (!dot(a[0], a[0], &d)) return false;      // (0 / 1 components: the sum of their squares is how many)
      if (!scalar_imm(fn == BuiltinFn::Any ? 0.0f : static_cast<float>(a[0].rows), &k)) return false;
      Val kv;
      kv.c[0] = k;
      if (!elementwise(fn == BuiltinFn::Any ? "SNE" : "SEQ", d, kv, out)) return false;
      out->num = kNumBool;
      return true;
    }
    case BuiltinFn::Not: {
      Oper zero;
      if (!need(1) || !scalar_imm(0.0f, &zero)) return false;
      Val zv;
      zv.c[0] = zero;
      if (!elementwise("SEQ", a[0], zv, out)) return false;
      out->num = kNumBool;
      return true;
    }
    case BuiltinFn::FaceForward: {      // faceforward(N, I, Nref) = dot(Nref, I) < 0 ? N : -N
      Val d;
      if (!need(3) || !dot(a[2], a[1], &d) || a[0].cols != 1) return false;
      if (!new_val(a[0].rows, 1, out)) return false;
      Oper nn = a[0].c[0];
      nn.neg = !nn.neg;
      inst_rows("CMP", out->c[0].idx, a[0].rows, &d.c[0], &a[0].c[0], &nn);
      return true;
    }
    case BuiltinFn::Refract: {      // k = 1 - eta^2 (1 - dot(N, I)^2); k < 0 ? 0 : eta I - (eta dot(N, I) + sqrt(k)) N
      Val d, d2, om, e2, k2, kk, sq, ei, ed, s2, sn, r;
      Oper one, zero;
      if (!need(3) || !scalar_imm(1.0f, &one) || !scalar_imm(0.0f, &zero) || !dot(a[1], a[0], &d)) return false;
      Val ov, zv;
      ov.c[0] = one;
      zv.c[0] = zero;
      if (!elementwise("MUL", d, d, &d2) || !elementwise("SUB", ov, d2, &om) || !elementwise("MUL", a[2], a[2], &e2) || !elementwise("MUL", e2, om, &k2) ||
          !elementwise("SUB", ov, k2, &kk) || !elementwise("MAX", kk, zv, &sq) || !per_component("SQRT", sq, nullptr, &s2))
        return false;
      if (!elementwise("MUL", a[2], a[0], &ei) || !elementwise("MUL", a[2], d, &ed) || !elementwise("ADD", ed, s2, &sn) || !elementwise("MUL", sn, a[1], &r)) return false;
      Val res;
      if (!elementwise("SUB", ei, r, &res) || !new_val(res.rows, 1, out)) return false;
      inst_rows("CMP", out->c[0].idx, res.rows, &kk.c[0], &zv.c[0], &res.c[0]);
      return true;
    }
    default: return fail("a built-in function the device cannot take");
  }
}

// atan2(y, x) of each component: the angle of (x, y) in (-pi, pi]. An odd polynomial for the angle of the smaller over the larger of |x| and |y| (in [0, 1]:
// error below 2e-6), then the octant: pi/2 - a where |y| > |x|, pi - a where x < 0, the sign of y.
bool Gen::atan2_of(const Val& y_in, const Val& x_in, Val* out) {
  Val y = y_in, x = x_in;
  if (!to_float(y) || !to_float(x)) return false;
  if (y.cols != 1 || x.cols != 1 || y.kind || x.kind) return fail("atan of other shapes");
  const uint32_t rows = y.rows > x.rows ? y.rows : x.rows;
  Val ax, ay, mn, mx, t, t2, p, r;
  if (!new_val(x.rows, 1, &ax) || !new_val(y.rows, 1, &ay)) return false;
  inst_rows("ABS", ax.c[0].idx, x.rows, &x.c[0]);
  inst_rows("ABS", ay.c[0].idx, y.rows, &y.c[0]);
  Oper tiny;
  if (!scalar_imm(1e-30f, &tiny)) return false;
  Val tv;
  tv.c[0] = tiny;
  Val mx0;
  if (!elementwise("MIN", ax, ay, &mn) || !elementwise("MAX", ax, ay, &mx0) || !elementwise("MAX", mx0, tv, &mx) || !elementwise("DIV", mn, mx, &t) ||
      !elementwise("MUL", t, t, &t2))
    return false;
  // a(t) = t (c0 + t2 (c1 + t2 (c2 + t2 (c3 + t2 (c4 + t2 c5)))))
  static const float kC[6] = {0.99997726f, -0.33262347f, 0.19354346f, -0.11643287f, 0.05265332f, -0.01172120f};
  Oper c[6];
  for (int i = 0; i < 6; ++i)
    if (!scalar_imm(kC[i], &c[i])) return false;
  if (!new_val(rows, 1, &p)) return false;
  inst_rows("MAD", p.c[0].idx, rows, &t2.c[0], &c[5], &c[4]);
  for (int i = 3; i >= 0; --i) inst_rows("MAD", p.c[0].idx, rows, &t2.c[0], &p.c[0], &c[i]);
  if (!elementwise("MUL", p, t, &r)) return false;
  Oper half_pi, pi;
  if (!scalar_imm(1.57079632679f, &half_pi) || !scalar_imm(3.14159265359f, &pi)) return false;
  Val r1, r2, sw, neg_x, hv, pv, alt;
  hv.c[0] = half_pi;
  pv.c[0] = pi;
  // where |y| > |x|: pi/2 - r
  if (!elementwise("SUB", hv, r, &alt) || !elementwise("SLT", ax, ay, &sw) || !new_val(rows, 1, &r1)) return false;
  Oper nsw = sw.c[0];
  nsw.neg = true;
  inst_rows("CMP", r1.c[0].idx, rows, &nsw, &alt.c[0], &r.c[0]);
  // where x < 0: pi - r
  Val alt2;
  if (!elementwise("SUB", pv, r1, &alt2) || !new_val(rows, 1, &r2)) return false;
  inst_rows("CMP", r2.c[0].idx, rows, &x.c[0], &alt2.c[0], &r1.c[0]);
  // the sign of y
  Oper nr = r2.c[0];
  nr.neg = true;
  if (!new_val(rows, 1, out)) return false;
  inst_rows("CMP", out->c[0].idx, rows, &y.c[0], &nr, &r2.c[0]);
  return true;
}

bool Gen::eval(int32_t id, Val* out) {
  if (id < 0) return fail("missing expression");
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  switch (n.kind) {
    case NodeKind::Const: return const_val(n, out);
    case NodeKind::Local: {
      const Var* v = local(n.ival);
      if (!v || !v->valid) return fail("a variable used before it is declared");
      val_of(*v, out);
      return true;
    }
    case NodeKind::Global: return n.ival >= 0 && global_val(static_cast<uint64_t>(n.ival), out);
    case NodeKind::Unary: {
      Val a;
      if (!eval(n.a, &a)) return false;
      if (a.is_int && (n.op == Op::Neg || n.op == Op::Plus)) {      // (a constant stays one)
        *out = a;
        if (n.op == Op::Neg) out->ival = -a.ival;
        return true;
      }
      if (!to_float(a)) return false;
      if (a.kind) return fail("an operator on a structure or array");
      if (n.op == Op::Plus) {
        *out = a;
        return true;
      }
      if (n.op == Op::Neg) {
        *out = a;
        for (uint32_t c = 0; c < a.cols; ++c) out->c[c].neg = !a.c[c].neg;
        return true;
      }
      if (n.op == Op::Not) {
        Oper zero;
        if (!scalar_imm(0.0f, &zero)) return false;
        Val z;
        z.c[0] = zero;
        if (!elementwise("SEQ", a, z, out)) return false;
        out->num = kNumBool;
        return true;
      }
      return fail("an operator the device cannot take");
    }
    case NodeKind::Binary: {
      Val a, b;
      return eval(n.a, &a) && eval(n.b, &b) && binary(n.op, a, b, out);
    }
    case NodeKind::Logical: {      // (both sides are evaluated: they have no effects the device would see twice)
      Val a, b;
      if (!eval(n.a, &a) || !eval(n.b, &b) || !to_float(a) || !to_float(b)) return false;
      if (a.rows != 1 || a.cols != 1 || b.rows != 1 || b.cols != 1 || a.kind || b.kind) return fail("a logical operator on vectors");
      if (!elementwise(n.op == Op::LogAnd ? "MUL" : (n.op == Op::LogOr ? "MAX" : "SNE"), a, b, out)) return false;
      out->num = kNumBool;
      return true;
    }
    case NodeKind::Assign: {
      Val r;
      if (!eval(n.b, &r)) return false;
      if (n.op != Op::None) {
        Val cur, res;
        if (!eval(n.a, &cur) || !binary(n.op, cur, r, &res)) return false;
        r = res;
      }
      if (!assign_to(n.a, r)) return false;
      if (stage_ == kStControl) {      // (the value of an assignment to a patch output is what was assigned: the output itself cannot be read)
        int32_t base = n.a;
        while (base >= 0 && (m_.nodes[static_cast<uint64_t>(base)].kind == NodeKind::Index || m_.nodes[static_cast<uint64_t>(base)].kind == NodeKind::Field ||
                             m_.nodes[static_cast<uint64_t>(base)].kind == NodeKind::Swizzle))
          base = m_.nodes[static_cast<uint64_t>(base)].a;
        const Node* bn = base >= 0 ? &m_.nodes[static_cast<uint64_t>(base)] : nullptr;
        if (bn && bn->kind == NodeKind::Global && bn->ival >= 0 && static_cast<uint64_t>(bn->ival) < m_.globals.count() && m_.globals[static_cast<uint64_t>(bn->ival)]->patch) {
          *out = r;
          return true;
        }
      }
      return eval(n.a, out);
    }
    case NodeKind::PreInc:
    case NodeKind::PostInc: {
      Val cur, old, res, one;
      if (!eval(n.a, &cur) || !to_float(cur)) return false;
      if (cur.kind) return fail("++ of a structure or array");
      if (n.kind == NodeKind::PostInc) {      // (the value before)
        if (!new_val(cur.rows, cur.cols, &old)) return false;
        for (uint32_t c = 0; c < cur.cols; ++c) inst_rows("MOV", old.c[c].idx, cur.rows, &cur.c[c]);
        old.num = cur.num;
      }
      one.is_int = true;
      one.num = cur.num;
      one.ival = 1;
      if (!binary(n.op, cur, one, &res) || !assign_to(n.a, res)) return false;
      if (n.kind == NodeKind::PostInc) {
        *out = old;
        return true;
      }
      return eval(n.a, out);
    }
    case NodeKind::Call: return call(n, out);
    case NodeKind::Ternary: {
      Val c, x, y;
      if (!eval(n.a, &c) || !eval(n.b, &x) || !eval(n.c, &y)) return false;
      if (!to_float(c) || !to_float(x) || !to_float(y)) return false;
      if (c.rows != 1 || c.cols != 1 || c.kind || x.kind || y.kind || x.cols != y.cols || x.rows != y.rows) return fail("a conditional of other shapes");
      if (!new_val(x.rows, x.cols, out)) return false;
      // CMP(-c, x, y): x where c is 1 (-c < 0), y where it is 0 - without arithmetic on the side not taken (an infinity there stays out)
      Oper nc = component(c.c[0], 0);
      nc.neg = !nc.neg;
      for (uint32_t col = 0; col < x.cols; ++col) inst_rows("CMP", out->c[col].idx, x.rows, &nc, &x.c[col], &y.c[col]);
      out->num = x.num;
      return true;
    }
    case NodeKind::Comma: {
      Val ignored;
      return eval(n.a, &ignored) && eval(n.b, out);
    }
    case NodeKind::Index: {
      if (is_control_output(n.a)) {      // a control stage reading its own control point's output, or another's
        const bool own = invocation_gi_ >= 0 && n.b >= 0 && m_.nodes[static_cast<uint64_t>(n.b)].kind == NodeKind::Global && m_.nodes[static_cast<uint64_t>(n.b)].ival == invocation_gi_;
        if (!own) return control_read(n, out);
        Var v;
        if (!control_output(n, &v)) return false;
        val_of(v, out);
        return true;
      }
      bool constant;
      int32_t k;
      Val iv;
      if (!index_of(n.b, &constant, &k, &iv)) return false;
      if (n.a >= 0 && m_.nodes[static_cast<uint64_t>(n.a)].kind == NodeKind::Global) {
        const int32_t gi = m_.nodes[static_cast<uint64_t>(n.a)].ival;
        if (gi >= 0 && static_cast<uint64_t>(gi) < m_.globals.count() && m_.globals[static_cast<uint64_t>(gi)]->type.array &&
            m_.globals[static_cast<uint64_t>(gi)]->kind == GlobalKind::Uniform) {
          // a uniform array: its rows in CONST, by a constant index or through the address register (the index clamped to the array, as the CPU does)
          const Global& g = *m_.globals[static_cast<uint64_t>(gi)];
          uint32_t row;
          if (!uniform_base(static_cast<uint64_t>(gi), &row)) return false;
          *out = Val();
          out->rows = g.type.rows;
          out->cols = g.type.cols;
          out->num = num_of(g.type.base);
          if (constant) {
            if (k < 0 || static_cast<uint32_t>(k) >= g.type.array) return fail("index out of range");
            for (uint32_t c = 0; c < g.type.cols; ++c) out->c[c] = filled('C', static_cast<uint16_t>(row + static_cast<uint32_t>(k) * g.type.cols + c), g.type.rows);
            return true;
          }
          Val ci, scaled;
          if (!clamp_index(iv, g.type.array, &ci)) return false;
          scaled = ci;
          if (g.type.cols > 1) {
            Oper m;
            Val mv;
            if (!scalar_imm(static_cast<float>(g.type.cols), &m)) return false;
            mv.c[0] = m;
            if (!elementwise("MUL", ci, mv, &scaled)) return false;
          }
          uses_addr = true;
          begin_inst("ARL");
          body.append(" ADDR[0].x, ");
          put_oper(component(scaled.c[0], 0));
          end_inst();
          for (uint32_t c = 0; c < g.type.cols; ++c) {
            uint16_t t;
            if (!new_temp(&t)) return false;
            begin_inst("MOV");
            body.append(" TEMP[");
            body.append_uint(t);
            body.append("].");
            body.append(mask_of(g.type.rows));
            body.append(", CONST[ADDR[0].x+");
            body.append_uint(row + c);
            body.append("]");
            end_inst();
            out->c[c] = filled('T', t, g.type.rows);
          }
          return true;
        }
      }
      Val b;
      if (!eval(n.a, &b)) return false;
      if (b.kind == 4) {      // an array variable
        if (constant) {
          if (k < 0 || static_cast<uint32_t>(k) >= b.count) return fail("index out of range");
          val_of(elems_[static_cast<uint64_t>(b.first) + static_cast<uint32_t>(k)], out);
          return true;
        }
        // (a per-vertex input holds as many vertices as the primitive or patch has, whatever its declared size: only those are chosen from)
        uint32_t count = b.count;
        if (!vs_ && !fs_ && n.a >= 0 && m_.nodes[static_cast<uint64_t>(n.a)].kind == NodeKind::Global) {
          const int32_t gi = m_.nodes[static_cast<uint64_t>(n.a)].ival;
          if (gi >= 0 && static_cast<uint64_t>(gi) < m_.globals.count() && m_.globals[static_cast<uint64_t>(gi)]->kind == GlobalKind::In &&
              m_.globals[static_cast<uint64_t>(gi)]->per_vertex && input_vertices() < count)
            count = input_vertices();
        }
        Val items[kMaxArray];
        for (uint32_t e = 0; e < count; ++e) val_of(elems_[static_cast<uint64_t>(b.first) + e], &items[e]);
        if (items[0].kind) return fail("an array of structures");
        return select(iv, count, items, out);
      }
      if (b.kind || b.is_int) return fail("an index the device cannot take");
      if (b.cols > 1) {      // a matrix: a column
        if (constant) {
          if (k < 0 || k >= b.cols) return fail("index out of range");
          *out = Val();
          out->rows = b.rows;
          out->c[0] = b.c[k];
          return true;
        }
        Val items[4];
        for (uint32_t c = 0; c < b.cols; ++c) {
          items[c] = Val();
          items[c].rows = b.rows;
          items[c].c[0] = b.c[c];
        }
        return select(iv, b.cols, items, out);
      }
      if (b.rows < 2) return fail("index out of range");
      if (constant) {
        if (k < 0 || k >= b.rows) return fail("index out of range");
        *out = Val();
        out->num = b.num;
        out->c[0] = component(b.c[0], static_cast<uint32_t>(k));
        return true;
      }
      Val items[4];
      for (uint32_t r = 0; r < b.rows; ++r) {
        items[r] = Val();
        items[r].num = b.num;
        items[r].c[0] = component(b.c[0], r);
      }
      return select(iv, b.rows, items, out);
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
      if (b.is_int && !to_float(b)) return false;
      if (b.cols != 1 || b.kind) return fail("a swizzle of a matrix");
      out->rows = static_cast<uint8_t>(count);
      out->cols = 1;
      out->is_int = false;
      out->kind = 0;
      out->num = b.num;
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
      if (!is_float_type(n.type)) return fail("a conversion the device cannot take");
      if (!to_float(a)) return false;
      if (a.kind || a.rows != n.type.rows || a.cols != n.type.cols) return fail("a conversion of another shape");
      return convert(a, num_of(n.type.base), out);
    }
    case NodeKind::Builtin: return builtin(n, out);
    case NodeKind::TexCall: return tex_call(n, out);
    default: return fail("an expression the device cannot take");
  }
}

// texture2D(sampler, coord) / texture(sampler, coord) in a fragment shader: a 2D float sampler uniform and a vec2 coordinate. The device has no mipmaps, so the
// implicit level of detail does not matter (the context only takes the draw when the bound textures have none).
bool Gen::tex_call(const Node& n, Val* out) {
  if (!fs_) return fail("a texture lookup in a vertex shader");
  const bool wgsl = n.flags == 1;
  // The device's textures have no mipmaps (GlContext::hw_texture: one level, one filter): a level, bias or gradients choose nothing, so those lookups are plain ones;
  // a projective lookup divides the coordinate by its last component first. Offsets and gathers are the CPU's.
  const TexOp top = static_cast<TexOp>(n.op);
  const bool proj = top == TexOp::Proj || top == TexOp::ProjBias || top == TexOp::ProjLod;
  const bool plain = top == TexOp::Sample || top == TexOp::Bias || top == TexOp::Lod || top == TexOp::Grad || proj;
  if (n.flags > 1 || !plain || static_cast<uint64_t>(n.first) + 3 > m_.kids.count()) return fail("a texture lookup the device cannot take");
  if (!wgsl && (n.count < 9 || static_cast<uint64_t>(n.first) + 9 > m_.kids.count() || m_.kids[static_cast<uint64_t>(n.first) + 7] >= 0 || m_.kids[static_cast<uint64_t>(n.first) + 8] >= 0))
    return fail("a texture lookup with an offset");
  if (wgsl && top != TexOp::Sample) return fail("a texture lookup the device cannot take");
  const int32_t sampler_id = m_.kids[static_cast<uint64_t>(n.first)], coord_id = m_.kids[static_cast<uint64_t>(n.first) + 2];
  auto global_of = [&](int32_t id, const Global** out) {
    if (id < 0 || m_.nodes[static_cast<uint64_t>(id)].kind != NodeKind::Global) return fail("a sampler that is not a global");
    const int32_t gi = m_.nodes[static_cast<uint64_t>(id)].ival;
    if (gi < 0 || static_cast<uint64_t>(gi) >= m_.globals.count()) return fail("bad global");
    *out = m_.globals[static_cast<uint64_t>(gi)].get();
    return true;
  };
  const Global* g = nullptr;
  const Global* s = nullptr;   // WGSL: the sampler binding
  if (!global_of(sampler_id, &g)) return false;
  if (wgsl) {
    // textureSample(texture, sampler, coords): both are bindings, whose handles the draw reads from the bound groups
    if (!global_of(m_.kids[static_cast<uint64_t>(n.first) + 1], &s)) return false;
    if (g->kind != GlobalKind::Handle || g->type.base != Base::Texture || g->type.array || g->type.dim != kDim2D || g->type.shadow || g->type.texel != Base::Float ||
        s->kind != GlobalKind::Handle || s->type.base != Base::SamplerState || s->type.array)
      return fail("a texture the device cannot take");
  } else if (g->kind != GlobalKind::Uniform || g->type.base != Base::Sampler || g->type.array || g->type.dim != kDim2D || g->type.shadow || g->type.texel != Base::Float) {
    return fail("a sampler the device cannot take");
  }
  TgsiProgram& p = *sh_.out;
  const uint32_t sampler_slot = wgsl ? s->slot : 0;
  uint32_t index = 0;
  while (index < p.samplers.count() && !(p.samplers[index].slot == g->slot && p.samplers[index].sampler_slot == sampler_slot && p.samplers[index].wgsl == wgsl)) ++index;
  if (index == p.samplers.count()) {
    if (index >= sh_.limits.samplers) return fail("too many samplers");
    if (!p.samplers.push_back(TgsiSampler{g->slot, sampler_slot, wgsl})) return fail("out of memory");
  }
  Val coord;
  if (!eval(coord_id, &coord)) return false;
  if (proj) {      // (s, t, q) or (s, t, r, q): s / q, t / q
    if (coord.is_int || coord.kind || coord.cols != 1 || coord.rows < 3) return fail("a projective coordinate the device cannot take");
    Val q, xy;
    q.c[0] = component(coord.c[0], coord.rows - 1u);
    xy.rows = 2;
    xy.c[0] = coord.c[0];
    if (!elementwise("DIV", xy, q, &coord)) return false;
  }
  if (coord.is_int || coord.cols != 1 || coord.rows != 2) return fail("a texture coordinate that is not a vec2");
  if (!new_val(4, 1, out)) return false;
  begin_inst("TEX");
  body.append(" TEMP[");
  body.append_uint(out->c[0].idx);
  body.append("].xyzw, ");
  put_oper(coord.c[0]);
  body.append(", SAMP[");
  body.append_uint(index);
  body.append("], 2D");
  end_inst();
  return true;
}

// --- statements ----------------------------------------------------------------------------------------------------------------------------------------------

// --- helpers of statements and indexing ---------------------------------------------------------------------------------------------------------------------

// An index: a constant (k) or a value computed by the shader (dynamic).
bool Gen::index_of(int32_t id, bool* constant, int32_t* k, Val* dynamic) {
  Val v;
  if (!eval(id, &v)) return false;
  if (v.is_int) {
    *constant = true;
    *k = v.ival;
    return true;
  }
  if (v.kind || v.cols != 1 || v.rows != 1) return fail("an index that is not a scalar");
  *constant = false;
  *dynamic = v;
  return true;
}

// An index kept inside an array of `count` (the CPU interpreter clamps them too).
bool Gen::clamp_index(const Val& idx, uint32_t count, Val* out) {
  Oper zero, last;
  if (!scalar_imm(0.0f, &zero) || !scalar_imm(static_cast<float>(count - 1), &last)) return false;
  Val zv, lv, t;
  zv.c[0] = zero;
  lv.c[0] = last;
  return elementwise("MAX", idx, zv, &t) && elementwise("MIN", t, lv, out);
}

// items[idx] for an index computed by the shader: each item compared with the (clamped) index and the one that matches chosen.
bool Gen::select(const Val& idx, uint32_t count, const Val* items, Val* out) {
  if (count == 0) return fail("an empty array");
  Val ci;
  if (!clamp_index(idx, count, &ci)) return false;
  const Val& first = items[0];
  if (!new_val(first.rows, first.cols, out)) return false;
  out->num = first.num;
  for (uint32_t c = 0; c < first.cols; ++c) inst_rows("MOV", out->c[c].idx, first.rows, &first.c[c]);
  for (uint32_t k = 1; k < count; ++k) {
    Oper kv;
    uint16_t t;
    if (items[k].cols != first.cols || items[k].rows != first.rows) return fail("array elements of other shapes");
    if (!scalar_imm(static_cast<float>(k), &kv) || !new_temp(&t)) return false;
    const Oper cx = component(ci.c[0], 0);
    inst("SEQ", t, "x", &cx, &kv);
    Oper nt = component(filled('T', t, 1), 0);
    nt.neg = true;
    for (uint32_t c = 0; c < first.cols; ++c) inst_rows("CMP", out->c[c].idx, first.rows, &nt, &items[k].c[c], &out->c[c]);
  }
  return true;
}

// l = t ? v : l (t a 0 / 1 scalar operand), for a store through an index computed by the shader.
bool Gen::cond_store(const LV& l, const Val& v_in, const Oper& t) {
  Val v = v_in;
  if (!to_float(v)) return false;
  if (l.agg || l.count || v.kind) return fail("an assignment through a variable index the device cannot take");
  Oper nt = component(t, 0);
  nt.neg = !nt.neg;
  if (l.cols > 1) {
    if (v.cols != l.cols || v.rows != l.rows) return fail("matrix assignment of another shape");
    for (uint32_t c = 0; c < l.cols; ++c) {
      const Oper cur = filled('T', l.temp[c], l.rows);
      inst_rows("CMP", l.temp[c], l.rows, &nt, &v.c[c], &cur);
    }
    return true;
  }
  if (v.cols != 1 || v.rows != l.n) return fail("assignment of another shape");
  Oper src = v.c[0];
  bool used[4] = {false, false, false, false};
  for (uint32_t j = 0; j < l.n; ++j) {
    src.swz[l.chan[j]] = v.c[0].swz[j];
    used[l.chan[j]] = true;
  }
  char mask[5];
  uint32_t n = 0;
  for (uint32_t k = 0; k < 4; ++k)
    if (used[k]) mask[n++] = "xyzw"[k];
  mask[n] = 0;
  const Oper cur = filled('T', l.temp[0], 4);
  inst("CMP", l.temp[0], mask, &nt, &src, &cur);
  return true;
}

// target = v: an lvalue, or an element of an array, matrix or vector chosen by an index the shader computes (each element takes v where the index is its own).
bool Gen::assign_to(int32_t target, const Val& v) {
  if (target < 0) return fail("missing expression");
  const Node& n = m_.nodes[static_cast<uint64_t>(target)];
  if (n.kind == NodeKind::Index && !is_control_output(n.a)) {
    bool constant;
    int32_t k;
    Val iv;
    if (!index_of(n.b, &constant, &k, &iv)) return false;
    if (!constant) {
      LV base;
      if (!lvalue(n.a, &base)) return false;
      const uint32_t count = base.count ? base.count : (base.cols > 1 ? base.cols : base.n);
      if (count < 1 || count > kMaxArray) return fail("an index the device cannot take");
      Val ci;
      if (!clamp_index(iv, count, &ci)) return false;
      for (uint32_t e = 0; e < count; ++e) {
        LV el;
        if (base.count) {
          lv_of(elems_[static_cast<uint64_t>(base.first) + e], &el);
        } else if (base.cols > 1) {
          el = LV();
          el.rows = base.rows;
          el.n = base.rows;
          el.temp[0] = base.temp[e];
        } else {
          el = base;
          el.n = 1;
          el.chan[0] = base.chan[e];
        }
        Oper kv;
        uint16_t t;
        if (!scalar_imm(static_cast<float>(e), &kv) || !new_temp(&t)) return false;
        const Oper cx = component(ci.c[0], 0);
        inst("SEQ", t, "x", &cx, &kv);
        if (!cond_store(el, v, component(filled('T', t, 1), 0))) return false;
      }
      return true;
    }
  }
  LV l;
  return lvalue(target, &l) && store(l, v);
}

// IF flag (when_set) or IF !flag: the flags are 0 / 1 in .x.
bool Gen::flag_test(uint16_t flag, bool when_set) {
  if (when_set) {
    begin_inst("IF");
    body.push_back(' ');
    put_oper(component(filled('T', flag, 1), 0));
    end_inst();
    return true;
  }
  uint16_t t;
  Oper zero;
  if (!new_temp(&t) || !scalar_imm(0.0f, &zero)) return false;
  const Oper f = component(filled('T', flag, 1), 0);
  inst("SEQ", t, "x", &f, &zero);
  begin_inst("IF");
  body.push_back(' ');
  put_oper(component(filled('T', t, 1), 0));
  end_inst();
  return true;
}

uint32_t Gen::count_returns(int32_t id) const {
  if (id < 0) return 0;
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  switch (n.kind) {
    case NodeKind::Return: return 1;
    case NodeKind::Block: {
      uint32_t c = 0;
      for (int32_t i = 0; i < n.count; ++i) c += count_returns(m_.kids[static_cast<uint64_t>(n.first + i)]);
      return c;
    }
    case NodeKind::If: return count_returns(n.b) + count_returns(n.c);
    case NodeKind::For: return count_returns(n.d);
    case NodeKind::While: return count_returns(n.b);
    case NodeKind::DoWhile: return count_returns(n.a);
    case NodeKind::Switch: {
      uint32_t c = 0;
      for (int32_t i = 0; i < n.count; ++i) c += count_returns(m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(n.first + i)])].d);
      return c;
    }
    default: return 0;
  }
}

// What a statement may do that the statements after it in its block must not miss: kExitReturn if it may return, kExitContinue if it may continue the loop
// that contains it (a continue of a loop inside it is that loop's).
uint32_t Gen::exits(int32_t id, bool in_loop) const {
  if (id < 0) return 0;
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  switch (n.kind) {
    case NodeKind::Return: return kExitReturn;
    case NodeKind::Continue: return in_loop ? kExitContinue : 0;
    case NodeKind::Block: {
      uint32_t m = 0;
      for (int32_t i = 0; i < n.count; ++i) m |= exits(m_.kids[static_cast<uint64_t>(n.first + i)], in_loop);
      return m;
    }
    case NodeKind::If: return exits(n.b, in_loop) | exits(n.c, in_loop);
    case NodeKind::For: return exits(n.d, false) & kExitReturn;
    case NodeKind::While: return exits(n.b, false) & kExitReturn;
    case NodeKind::DoWhile: return exits(n.a, false) & kExitReturn;
    case NodeKind::Switch: {
      uint32_t m = 0;
      for (int32_t i = 0; i < n.count; ++i) m |= exits(m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(n.first + i)])].d, in_loop);
      return m;
    }
    default: return 0;
  }
}

// switch (sel) { case ...: ... }: a loop run once, so that break leaves it. A case's body runs once it or a case before it (GLSL falls through) matched; default
// matches when no case does.
bool Gen::switch_stmt(const Node& n) {
  FnCtx& f = fns_[nfns_ - 1];
  if (nloops_ >= sizeof loops_ / sizeof loops_[0]) return fail("loops nested too deep");
  for (int32_t i = 0; i < n.count; ++i)
    if (exits(m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(n.first + i)])].d, true) & kExitContinue) return fail("continue inside a switch");
  Val sel;
  if (!eval(n.a, &sel) || !to_float(sel)) return false;
  if (sel.kind || sel.cols != 1 || sel.rows != 1) return fail("a switch on a value that is not a scalar");
  const bool fallthrough = (n.flags & 1) != 0;
  // which case matches (before the loop: a case value is a constant expression)
  uint16_t hit[64];
  uint16_t any;
  Oper zero, one;
  if (n.count > 64 || !new_temp(&any) || !scalar_imm(0.0f, &zero) || !scalar_imm(1.0f, &one)) return fail("a switch the device cannot take");
  inst("MOV", any, "x", &zero);
  const Oper sx = component(sel.c[0], 0);
  for (int32_t i = 0; i < n.count; ++i) {
    const Node& cs = m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(n.first + i)])];
    if (!new_temp(&hit[i])) return false;
    inst("MOV", hit[i], "x", &zero);
    for (int32_t j = 0; j < cs.count; ++j) {
      Val v;
      uint16_t t;
      if (!eval(m_.kids[static_cast<uint64_t>(cs.first + j)], &v) || !to_float(v) || !new_temp(&t)) return false;
      const Oper vx = component(v.c[0], 0);
      inst("SEQ", t, "x", &sx, &vx);
      const Oper tx = component(filled('T', t, 1), 0), hx = component(filled('T', hit[i], 1), 0), ax = component(filled('T', any, 1), 0);
      inst("MAX", hit[i], "x", &hx, &tx);
      inst("MAX", any, "x", &ax, &tx);
    }
  }
  uint16_t matched;
  if (!new_temp(&matched)) return false;
  inst("MOV", matched, "x", &zero);
  begin_inst("BGNLOOP");
  end_inst();
  LoopCtx lc;
  lc.is_switch = true;
  loops_[nloops_++] = lc;
  ++f.loops;
  bool ok = true;
  for (int32_t i = 0; i < n.count && ok; ++i) {
    const Node& cs = m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(n.first + i)])];
    const Oper mx = component(filled('T', matched, 1), 0), hx = component(filled('T', hit[i], 1), 0);
    if (cs.ival == 1) {      // default: no case matched (or, falling through, it did before)
      uint16_t none;
      if (!new_temp(&none)) return false;
      const Oper ax = component(filled('T', any, 1), 0);
      inst("SEQ", none, "x", &ax, &zero);
      const Oper nx = component(filled('T', none, 1), 0);
      inst("MAX", hit[i], "x", &hx, &nx);
    }
    if (fallthrough) inst("MAX", matched, "x", &mx, &hx);
    else inst("MOV", matched, "x", &hx);
    if (!flag_test(matched, true)) return false;
    ok = stmt(cs.d, false);
    begin_inst("ENDIF");
    end_inst();
    if (ok && f.use_flag && (exits(cs.d, true) & kExitReturn)) {      // (a return out of a loop inside the case)
      ok = flag_test(f.flag, true);
      begin_inst("BRK");
      end_inst();
      begin_inst("ENDIF");
      end_inst();
    }
  }
  --f.loops;
  --nloops_;
  if (!ok) return false;
  begin_inst("BRK");
  end_inst();
  begin_inst("ENDLOOP");
  end_inst();
  return true;
}

// The statements of a block; after one that may return or continue, the rest runs only while the flag is clear (a return inside a loop also leaves it).
bool Gen::block(const Node& n, bool tail) {
  uint32_t open = 0;
  FnCtx& f = fns_[nfns_ - 1];
  for (int32_t i = 0; i < n.count; ++i) {
    const int32_t kid = m_.kids[static_cast<uint64_t>(n.first + i)];
    if (!stmt(kid, tail && i == n.count - 1)) return false;
    if (i == n.count - 1) break;
    const uint32_t m = exits(kid, f.loops > 0);
    if ((m & kExitReturn) && f.use_flag) {
      if (f.loops) {      // (out of the loop; the blocks around the loop skip what follows it)
        if (!flag_test(f.flag, true)) return false;
        begin_inst("BRK");
        end_inst();
        begin_inst("ENDIF");
        end_inst();
      } else {
        if (!flag_test(f.flag, false)) return false;
        ++open;
      }
    }
    if ((m & kExitContinue) && nloops_ && loops_[nloops_ - 1].use_flag) {
      if (!flag_test(loops_[nloops_ - 1].flag, false)) return false;
      ++open;
    }
  }
  for (uint32_t k = 0; k < open; ++k) {
    begin_inst("ENDIF");
    end_inst();
  }
  return true;
}

// for (init; cond; step) body / while (cond) body / do body while (cond): BGNLOOP ... ENDLOOP, the condition tested at the top (a do loop: at the bottom).
bool Gen::loop(const Node& n) {
  FnCtx& f = fns_[nfns_ - 1];
  if (nloops_ >= sizeof loops_ / sizeof loops_[0]) return fail("loops nested too deep");
  const bool is_for = n.kind == NodeKind::For, is_while = n.kind == NodeKind::While;
  const int32_t body_id = is_for ? n.d : (is_while ? n.b : n.a);
  const int32_t cond_id = is_for ? n.b : (is_while ? n.a : n.b);
  LoopCtx lc;
  lc.is_while = is_while;
  lc.use_flag = !is_while && (exits(body_id, true) & kExitContinue) != 0;
  if (lc.use_flag && !new_temp(&lc.flag)) return false;
  if (is_for && !stmt(n.a, false)) return false;
  const auto test = [&]() {      // IF !cond BRK
    if (cond_id < 0) return true;
    Val c;
    if (!eval(cond_id, &c) || !to_float(c)) return false;
    if (c.kind || c.cols != 1 || c.rows != 1) return fail("a condition that is not a scalar");
    uint16_t t;
    Oper zero;
    if (!new_temp(&t) || !scalar_imm(0.0f, &zero)) return false;
    const Oper cx = component(c.c[0], 0);
    inst("SEQ", t, "x", &cx, &zero);
    begin_inst("IF");
    body.push_back(' ');
    put_oper(component(filled('T', t, 1), 0));
    end_inst();
    begin_inst("BRK");
    end_inst();
    begin_inst("ENDIF");
    end_inst();
    return true;
  };
  begin_inst("BGNLOOP");
  end_inst();
  if (n.kind != NodeKind::DoWhile && !test()) return false;
  if (lc.use_flag) {
    Oper zero;
    if (!scalar_imm(0.0f, &zero)) return false;
    inst("MOV", lc.flag, "x", &zero);
  }
  loops_[nloops_++] = lc;
  ++f.loops;
  bool ok = stmt(body_id, false);
  // a return in the body that came out of a loop inside it leaves this loop too (the body's blocks test the flag only between their statements)
  if (ok && f.use_flag && (exits(body_id, true) & kExitReturn)) {
    ok = flag_test(f.flag, true);
    begin_inst("BRK");
    end_inst();
    begin_inst("ENDIF");
    end_inst();
  }
  --f.loops;
  --nloops_;
  if (!ok) return false;
  if (is_for && n.c >= 0) {
    Val ignored;
    if (!eval(n.c, &ignored)) return false;
  }
  if (n.kind == NodeKind::DoWhile && !test()) return false;
  begin_inst("ENDLOOP");
  end_inst();
  return true;
}

// A user function, inlined: its parameters and variables are new registers (in / inout parameters copied in, out / inout ones copied back after), its returns
// write its result variable.
bool Gen::call(const Node& n, Val* out) {
  if (n.ival < 0 || static_cast<uint64_t>(n.ival) >= m_.functions.count()) return fail("unknown function");
  const Function& F = *m_.functions[static_cast<uint64_t>(n.ival)];
  if (F.body < 0) return fail("a function without a body");
  if (nfns_ > kMaxCallDepth) return fail("calls nested too deep (or recursion)");
  const uint32_t np = static_cast<uint32_t>(F.params.count());
  if (np != static_cast<uint32_t>(n.count) || np > 16 || np > F.locals) return fail("a call the device cannot take");
  Val args[16];
  for (uint32_t i = 0; i < np; ++i) {
    if (F.params[i].dir == 1) continue;
    if (!eval(m_.kids[static_cast<uint64_t>(n.first) + i], &args[i]) || !to_float(args[i])) return false;
  }
  // the callee's variables, after the caller's
  const uint64_t base = locals_.count();
  for (uint32_t i = 0; i < F.locals; ++i)
    if (!locals_.push_back(Var())) return fail("out of memory");
  for (uint32_t i = 0; i < np; ++i) {
    Var pv;
    if (!make_var(F.params[i].type, &pv)) return false;
    locals_[base + i] = pv;
    if (F.params[i].dir == 1) continue;
    LV l;
    lv_of(pv, &l);
    if (!store(l, args[i])) return false;
  }
  FnCtx ctx;
  if (F.ret.base != Base::Void) {
    if (!make_var(F.ret, &ctx.ret)) return false;
    ctx.has_ret = true;
  }
  {      // (only a function whose one return is its last statement does without the flag)
    const uint32_t r = count_returns(F.body);
    const Node& b = m_.nodes[static_cast<uint64_t>(F.body)];
    const bool last_is_return = b.kind == NodeKind::Block && b.count > 0 && m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(b.first + b.count - 1)])].kind == NodeKind::Return;
    ctx.use_flag = r > 1 || (r == 1 && !last_is_return);
  }
  if (ctx.use_flag) {
    Oper zero;
    if (!new_temp(&ctx.flag) || !scalar_imm(0.0f, &zero)) return false;
    inst("MOV", ctx.flag, "x", &zero);
  }
  const uint64_t saved_base = local_base_;
  const uint32_t saved_loops = nloops_;
  fns_[nfns_++] = ctx;
  local_base_ = base;
  nloops_ = 0;      // (a break or continue in the callee is about its own loops)
  const bool ok = stmt(F.body, true);
  nloops_ = saved_loops;
  local_base_ = saved_base;
  const FnCtx done = fns_[--nfns_];
  if (!ok) return false;
  // out / inout parameters back into the arguments (in the caller's frame again)
  for (uint32_t i = 0; i < np; ++i) {
    if (F.params[i].dir == 0) continue;
    Val pv;
    val_of(locals_[base + i], &pv);
    if (!assign_to(m_.kids[static_cast<uint64_t>(n.first) + i], pv)) return false;
  }
  if (done.has_ret) {
    val_of(done.ret, out);
  } else {
    *out = Val();
    out->kind = 2;      // (void: a value that fails where it is used)
  }
  return true;
}

// --- statements ----------------------------------------------------------------------------------------------------------------------------------------------

bool Gen::stmt(int32_t id, bool tail) {
  if (id < 0) return true;
  const Node& n = m_.nodes[static_cast<uint64_t>(id)];
  FnCtx& f = fns_[nfns_ - 1];
  switch (n.kind) {
    case NodeKind::Empty: return true;
    case NodeKind::Block: return block(n, tail);
    case NodeKind::Decl: {
      Var* slot = local(n.ival);
      if (!slot) return fail("bad variable");
      Var v;
      if (n.type.base == Base::Void) {   // WGSL let/var without a type: it is the initializer's
        Val init;
        if (n.a < 0 || !eval(n.a, &init) || !to_float(init)) return fail("a variable without a type");
        if (init.kind == 1) {
          v.valid = v.is_struct = true;
          v.first = init.first;
          v.sid = init.sid;
          *local(n.ival) = v;
          return true;
        }
        Type t = make_type(Base::Float, init.rows, init.cols);
        LV l;
        if (!make_var(t, &v)) return false;
        v.num = init.num;
        *local(n.ival) = v;
        lv_of(v, &l);
        return store(l, init);
      }
      if (!make_var(n.type, &v)) return false;
      *local(n.ival) = v;
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
      if (!eval(n.a, &c) || !to_float(c)) return false;
      if (c.kind || c.cols != 1 || c.rows != 1) return fail("a condition that is not a scalar");
      begin_inst("IF");
      body.push_back(' ');
      put_oper(component(c.c[0], 0));
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
    case NodeKind::For: case NodeKind::While: case NodeKind::DoWhile: return loop(n);
    case NodeKind::Switch: return switch_stmt(n);
    case NodeKind::Break:
      if (!f.loops || !nloops_) return fail("break outside a loop");
      begin_inst("BRK");
      end_inst();
      return true;
    case NodeKind::Continue:
      if (!f.loops || !nloops_ || loops_[nloops_ - 1].is_switch) return fail("continue outside a loop");
      if (loops_[nloops_ - 1].use_flag) {
        Oper one;
        if (!scalar_imm(1.0f, &one)) return false;
        inst("MOV", loops_[nloops_ - 1].flag, "x", &one);
      } else {
        begin_inst("CONT");
        end_inst();
      }
      return true;
    case NodeKind::Return:
      if (f.wgsl_entry) {      // (an entry point's result is the outputs: one return, at the end)
        if (!tail || (n.a >= 0 && have_ret_)) return fail("an early return");
        if (n.a >= 0) {
          if (!eval(n.a, &ret_)) return false;
          have_ret_ = true;
        }
        return true;
      }
      if (n.a >= 0) {
        Val v;
        LV l;
        if (!f.has_ret || !eval(n.a, &v)) return fail("a return value the device cannot take");
        lv_of(f.ret, &l);
        if (!store(l, v)) return false;
      }
      if (f.use_flag) {
        Oper one;
        if (!scalar_imm(1.0f, &one)) return false;
        inst("MOV", f.flag, "x", &one);
        if (f.loops) {
          begin_inst("BRK");
          end_inst();
        }
      } else if (!tail) {
        return fail("an early return");
      }
      return true;
    case NodeKind::Discard:
      if (!fs_) return fail("discard in a vertex shader");
      begin_inst("KILL");
      end_inst();
      return true;
    default: return fail("a statement the device cannot take (switch)");
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
  fns_[0] = FnCtx();
  fns_[0].wgsl_entry = true;
  nfns_ = 1;
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
        if (o.flat || sh_.var_count >= sh_.limits.varyings) return fail("a varying the device cannot take");
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
    if (m_.uses_derivatives || m_.writes_frag_depth) return fail("derivatives or gl_FragDepth");
    return run_entry();
  }
  if (m_.main_function < 0 || static_cast<uint64_t>(m_.main_function) >= m_.functions.count()) return fail("no main function");
  const Function& f = *m_.functions[static_cast<uint64_t>(m_.main_function)];
  if (m_.uses_derivatives || m_.writes_frag_depth) return fail("derivatives or gl_FragDepth");
  for (uint32_t i = 0; i < f.locals; ++i)
    if (!locals_.push_back(Var())) return fail("out of memory");
  for (uint64_t i = 0; i < m_.globals.count(); ++i)
    if (!globals_.push_back(Var()) || !stage_vars_.push_back(StageVar())) return fail("out of memory");
  // main's returns end the shader: the outputs written so far are the shader's (a return in the middle sets a flag the rest is skipped by)
  fns_[0] = FnCtx();
  {
    const uint32_t r = count_returns(f.body);
    const Node& b = m_.nodes[static_cast<uint64_t>(f.body)];
    const bool last_is_return = b.kind == NodeKind::Block && b.count > 0 && m_.nodes[static_cast<uint64_t>(m_.kids[static_cast<uint64_t>(b.first + b.count - 1)])].kind == NodeKind::Return;
    fns_[0].use_flag = r > 1 || (r == 1 && !last_is_return);
  }
  nfns_ = 1;
  if (fns_[0].use_flag) {
    Oper zero;
    if (!new_temp(&fns_[0].flag) || !scalar_imm(0.0f, &zero)) return false;
    inst("MOV", fns_[0].flag, "x", &zero);
  }
  if (!vs_ && !fs_ && !stage_start()) return false;
  // globals with an initializer (`float g = 1.0;` at file scope): the initializers run first, in their order
  for (uint64_t gi = 0; gi < m_.globals.count(); ++gi) {
    const Global& g = *m_.globals[gi];
    if (g.kind != GlobalKind::Private || g.init < 0) continue;
    Var v;
    Val init;
    LV l;
    if (!global_var(gi, &v) || !eval(g.init, &init)) return false;
    lv_of(v, &l);
    if (!store(l, init)) return false;
  }
  // a fragment shader's array varyings: copied from their registers into variables before anything (an index by a variable needs them as variables)
  if (fs_) {
    for (uint64_t gi = 0; gi < m_.globals.count(); ++gi) {
      const Global& g = *m_.globals[gi];
      if (g.kind != GlobalKind::In || !g.type.array || !g.referenced || g.builtin != kBvNone) continue;
      const IoLink* l = in_ ? in_->by_in(static_cast<int32_t>(gi)) : nullptr;
      if (!l) return fail("a varying the vertex shader does not write");
      Var v;
      if (!global_var(gi, &v) || !v.count) return fail("a varying the device cannot take");
      uint32_t reg = l->base;
      for (uint32_t e = 0; e < v.count; ++e) {
        const Var el = elems_[static_cast<uint64_t>(v.first) + e];
        for (uint32_t c = 0; c < el.cols; ++c) {
          const Oper in = filled('I', static_cast<uint16_t>(reg++), el.rows);
          inst_rows("MOV", el.temp[c], el.rows, &in);
        }
      }
    }
  }
  if (!stmt(f.body, true)) return false;

  if (vs_ || stage_ == kStEval) return write_vertex_outputs();
  if (stage_ == kStGeometry) return true;      // (its vertices went out at each EmitVertex())
  if (stage_ == kStControl) return control_end();
  // fragment: the first color output
  for (uint64_t gi = 0; gi < m_.globals.count(); ++gi) {
    const Global& g = *m_.globals[gi];
    const bool legacy = (g.builtin == kBvFragColor || g.builtin == kBvFragData) && g.referenced;
    const bool modern = g.kind == GlobalKind::Out && g.builtin == kBvNone && !g.type.array && (g.location <= 0);
    if (!legacy && !modern) continue;
    if (g.type.base != Base::Float || g.type.rows != 4 || g.type.cols != 1) return fail("a color output that is not a vec4");
    Var v;
    if (!global_var(gi, &v)) return false;
    // (gl_FragData: the first draw buffer's, the only one the device's target has)
    const Var& c = v.count ? elems_[v.first] : v;
    out_mov(0, "xyzw", filled('T', c.temp[0], 4));
    return true;
  }
  return fail("no color output");
}

// --- geometry and tessellation stages ------------------------------------------------------------------------------------------------------------------------

// The vertices of the stage's input primitive or patch (what its per-vertex input arrays hold).
uint32_t Gen::input_vertices() const {
  if (stage_ == kStGeometry) {
    switch (m_.gs_input) {
      case 0: return 1;
      case 1: return 2;
      case 0xA: return 4;
      case 0xC: return 6;
      default: return 3;
    }
  }
  return stage_ == kStControl ? sh_.stages.patch_vertices : sh_.eval_vertices;
}

// MOV TEMP[temp].mask, IN[vertex][reg].swizzle: an input of one vertex of the primitive or patch.
bool Gen::in2d(uint16_t temp, const char* mask, uint32_t vertex, uint32_t reg, const char* swizzle) {
  begin_inst("MOV");
  body.append(" TEMP[");
  body.append_uint(temp);
  body.append("].");
  body.append(mask);
  body.append(", IN[");
  body.append_uint(vertex);
  body.append("][");
  body.append_uint(reg);
  body.append("].");
  body.append(swizzle);
  end_inst();
  return true;
}

// A system value, SV[n] of `semantic`, into its variable (an integer one as its float value, as the translation keeps integers).
bool Gen::system_value(const char* semantic, bool integer, uint32_t rows, const Var& v) {
  if (nsv >= kMaxSystemValues || v.count || v.is_struct) return fail("a system value the device cannot take");
  const uint32_t k = nsv++;
  sv_names[k] = semantic;
  begin_inst(integer ? "I2F" : "MOV");
  body.append(" TEMP[");
  body.append_uint(v.temp[0]);
  body.append("].");
  body.append(mask_of(rows));
  body.append(", SV[");
  body.append_uint(k);
  body.append(rows >= 3 ? "].xyzz" : "].xxxx");
  end_inst();
  return true;
}

// A geometry or tessellation stage's start: the inputs the stage before wrote, copied from their registers into the variables the shader reads (an input is then
// indexed like any array, by a variable too), the system values it reads, and for a control stage the flags its patch outputs are written with.
bool Gen::stage_start() {
  static const char* const kSwizzles[4] = {"xxxx", "yyyy", "zzzz", "wwww"};
  const uint32_t nv = input_vertices();
  if (nv < 1 || nv > kMaxArray) return fail("a patch size the device cannot take");
  const uint32_t patch_in = 1 + (in_ ? in_->registers : 0);      // (the evaluation stage's per-patch inputs: after the per-vertex ones)
  for (uint64_t gi = 0; gi < m_.globals.count(); ++gi) {
    const Global& g = *m_.globals[gi];
    if (g.kind != GlobalKind::In || !g.referenced) continue;
    const StringView name = g.name.view();
    const bool position = name.equals("gl_in.gl_Position");
    const bool outer = name.equals("gl_TessLevelOuter"), inner = name.equals("gl_TessLevelInner");
    const IoLink* l = in_ ? in_->by_in(static_cast<int32_t>(gi)) : nullptr;
    if (!position && !outer && !inner && !l) continue;      // (not written by the stage before: reading it fails in global_val)
    if (g.inner) return fail("an array of arrays per vertex");
    Var v;
    if (!global_var(gi, &v)) return false;
    if (outer || inner) {      // the evaluation stage's tessellation levels: the x, y, z, w of TESSOUTER, the x, y of TESSINNER
      if (stage_ != kStEval || !v.count || !in_ || !in_->levels) return fail("a tessellation level the device cannot take");
      const uint32_t reg = patch_in + in_->patch_registers + (inner ? 1 : 0);
      for (uint32_t e = 0; e < v.count && e < 4; ++e) {
        const Var& el = elems_[static_cast<uint64_t>(v.first) + e];
        begin_inst("MOV");
        body.append(" TEMP[");
        body.append_uint(el.temp[0]);
        body.append("].x, IN[");
        body.append_uint(reg);
        body.append("].");
        body.append(kSwizzles[e]);
        end_inst();
      }
    } else if (g.per_vertex) {
      if (!v.count) return fail("a per-vertex input the device cannot take");
      const uint32_t reg = position ? 0 : 1 + l->base;
      for (uint32_t vtx = 0; vtx < nv && vtx < v.count; ++vtx) {
        const Var& el = elems_[static_cast<uint64_t>(v.first) + vtx];
        if (el.count || el.is_struct) return fail("a per-vertex input the device cannot take");
        for (uint32_t c = 0; c < el.cols; ++c) in2d(el.temp[c], mask_of(el.rows), vtx, reg + c);
      }
    } else {      // a per-patch input of the evaluation stage: PATCH registers
      if (stage_ != kStEval || !l->patch) return fail("an input the device cannot take");
      uint32_t reg = patch_in + l->base;
      const uint32_t elements = v.count ? v.count : 1;
      for (uint32_t e = 0; e < elements; ++e) {
        const Var& el = v.count ? elems_[static_cast<uint64_t>(v.first) + e] : v;
        if (el.count || el.is_struct) return fail("a patch input the device cannot take");
        for (uint32_t c = 0; c < el.cols; ++c) {
          begin_inst("MOV");
          body.append(" TEMP[");
          body.append_uint(el.temp[c]);
          body.append("].");
          body.append(mask_of(el.rows));
          body.append(", IN[");
          body.append_uint(reg++);
          body.append("].xyzw");
          end_inst();
        }
      }
    }
    stage_vars_[gi].ready = true;
  }
  // The system values each stage has (TGSI's are integers but for the coordinate), and the patch size, which the program was made for.
  struct Sys {
    const char* name;
    const char* semantic;
    bool integer;
    uint32_t rows;
    uint8_t stage;
  };
  static const Sys kSys[] = {{"gl_InvocationID", "INVOCATIONID", true, 1, kStGeometry},
                             {"gl_InvocationID", "INVOCATIONID", true, 1, kStControl},  {"gl_PrimitiveID", "PRIM_ID", true, 1, kStControl},
                             {"gl_TessCoord", "TESSCOORD", false, 3, kStEval},          {"gl_PrimitiveID", "PRIM_ID", true, 1, kStEval}};
  for (const Sys& s : kSys) {
    if (s.stage != stage_) continue;
    const int32_t gi = m_.find_global(StringView(s.name));
    if (gi < 0) continue;
    const Global& g = *m_.globals[static_cast<uint64_t>(gi)];
    if (g.kind != GlobalKind::Private || !g.is_read_only) continue;
    const bool invocation = stage_ == kStControl && StringView(s.name).equals("gl_InvocationID");
    if (!g.referenced && !invocation) continue;      // (a control stage's outputs are written by its invocation's number)
    Var v;
    if (!global_var(static_cast<uint64_t>(gi), &v) || !system_value(s.semantic, s.integer, s.rows, v)) return false;
    stage_vars_[static_cast<uint64_t>(gi)].ready = true;
    if (invocation) invocation_gi_ = gi;
  }
  if (stage_ == kStControl && invocation_gi_ < 0) return fail("a control stage without gl_InvocationID");
  // (the geometry stage's primitive number is an input there, PRIM_ID, after the per-vertex ones: as Gallium has it)
  const int32_t pid = stage_ == kStGeometry ? m_.find_global(StringView("gl_PrimitiveIDIn")) : -1;
  if (pid >= 0 && m_.globals[static_cast<uint64_t>(pid)]->referenced) {
    Var v;
    if (!global_var(static_cast<uint64_t>(pid), &v) || v.count || v.is_struct) return fail("gl_PrimitiveIDIn");
    primitive_in = static_cast<int32_t>(1 + (in_ ? in_->registers : 0));
    begin_inst("I2F");
    body.append(" TEMP[");
    body.append_uint(v.temp[0]);
    body.append("].x, IN[");
    body.append_uint(static_cast<uint32_t>(primitive_in));
    body.append("].xxxx");
    end_inst();
    stage_vars_[static_cast<uint64_t>(pid)].ready = true;
  }
  const int32_t pv = stage_ != kStGeometry ? m_.find_global(StringView("gl_PatchVerticesIn")) : -1;
  if (pv >= 0 && m_.globals[static_cast<uint64_t>(pv)]->referenced) {
    Var v;
    Oper k;
    if (!global_var(static_cast<uint64_t>(pv), &v) || v.count || v.is_struct || !scalar_imm(static_cast<float>(nv), &k)) return fail("gl_PatchVerticesIn");
    inst("MOV", v.temp[0], "x", &k);
    stage_vars_[static_cast<uint64_t>(pv)].ready = true;
  }
  if (stage_ == kStControl) {
    Oper zero;
    if (!scalar_imm(0.0f, &zero)) return false;
    for (uint64_t gi = 0; gi < m_.globals.count(); ++gi) {
      const Global& g = *m_.globals[gi];
      if (g.kind != GlobalKind::Out || !g.patch || !g.referenced) continue;
      uint16_t t;
      if (!new_temp(&t)) return false;
      inst("MOV", t, "x", &zero);
      stage_vars_[gi].flag = t;
    }
  }
  return true;
}

bool Gen::is_control_output(int32_t node) const {
  if (stage_ != kStControl || node < 0 || static_cast<uint64_t>(node) >= m_.nodes.count()) return false;
  const Node& n = m_.nodes[static_cast<uint64_t>(node)];
  if (n.kind != NodeKind::Global || n.ival < 0 || static_cast<uint64_t>(n.ival) >= m_.globals.count()) return false;
  const Global& g = *m_.globals[static_cast<uint64_t>(n.ival)];
  return g.kind == GlobalKind::Out && g.per_vertex;
}

// x[gl_InvocationID] of a per-vertex output x of the control stage: the variable of the invocation's own control point (any other index is refused).
bool Gen::control_output(const Node& index, Var* out) {
  const int32_t gi = m_.nodes[static_cast<uint64_t>(index.a)].ival;
  const Global& g = *m_.globals[static_cast<uint64_t>(gi)];
  const bool own = invocation_gi_ >= 0 && index.b >= 0 && static_cast<uint64_t>(index.b) < m_.nodes.count() &&
                   m_.nodes[static_cast<uint64_t>(index.b)].kind == NodeKind::Global && m_.nodes[static_cast<uint64_t>(index.b)].ival == invocation_gi_;
  if (!own) return fail("a control stage output of another invocation");
  if (g.inner || static_cast<uint64_t>(gi) >= stage_vars_.count()) return fail("an array of arrays per vertex");
  if (!stage_vars_[static_cast<uint64_t>(gi)].own.valid) {
    Var v;
    if (!make_var(g.type.element(), &v)) return false;
    stage_vars_[static_cast<uint64_t>(gi)].own = v;
  }
  *out = stage_vars_[static_cast<uint64_t>(gi)].own;
  return true;
}

// gl_out[j].x / x[j] of a control stage, j not its own invocation: OUT[k][r] of control point k (what invocation k put out at its last barrier()), chosen by j.
bool Gen::control_read(const Node& index, Val* out) {
  const int32_t gi = m_.nodes[static_cast<uint64_t>(index.a)].ival;
  const Global& g = *m_.globals[static_cast<uint64_t>(gi)];
  if (g.inner) return fail("an array of arrays per vertex");
  uint32_t reg = 0;
  const IoLink* l = nullptr;
  if (out_)
    for (const IoLink& k : out_->links) if (k.out_gi == gi && !k.patch) l = &k;
  if (l) reg = 1 + l->base;      // (the position: its copy, see stage_boundary)
  else if (!g.name.view().equals("gl_out.gl_Position")) return fail("a control stage output of another invocation");
  const Type et = g.type.element();
  if (!is_float_type(et)) return fail("a control stage output of another invocation");
  const uint32_t n = m_.tcs_vertices > 0 ? static_cast<uint32_t>(m_.tcs_vertices) : 0;
  if (n < 1 || n > kMaxArray) return fail("a patch size the device cannot take");
  Val items[kMaxArray];
  for (uint32_t k = 0; k < n; ++k) {
    items[k] = Val();
    items[k].rows = et.rows;
    items[k].cols = et.cols;
    items[k].num = num_of(et.base);
    for (uint32_t c = 0; c < et.cols; ++c) {
      items[k].c[c] = filled('O', static_cast<uint16_t>(reg + c), et.rows);
      items[k].c[c].dim = static_cast<int16_t>(k);
    }
  }
  bool constant;
  int32_t k;
  Val iv;
  if (!index_of(index.b, &constant, &k, &iv)) return false;
  if (!constant) return select(iv, n, items, out);
  if (k < 0 || static_cast<uint32_t>(k) >= n) return fail("index out of range");
  if (!new_val(et.rows, et.cols, out)) return false;      // (a copy: an output register is not an operand everywhere)
  out->num = items[k].num;
  for (uint32_t c = 0; c < et.cols; ++c) inst_rows("MOV", out->c[c].idx, et.rows, &items[k].c[c]);
  return true;
}

// A control stage's patch output or tessellation level read: its own value when it wrote one, else the output register (what an invocation put out at a barrier()).
bool Gen::patch_read(uint64_t gi, Val* out) {
  const Global& g = *m_.globals[gi];
  if (gi >= stage_vars_.count() || stage_vars_[gi].flag < 0 || !out_) return fail("a patch output the device cannot take");
  const StringView name = g.name.view();
  const bool outer = name.equals("gl_TessLevelOuter"), inner = name.equals("gl_TessLevelInner");
  const uint32_t patch_out = 1 + out_->registers;
  uint32_t reg;
  if (outer || inner) {
    reg = patch_out + out_->patch_registers + (inner ? 1 : 0);
  } else {
    const IoLink* l = nullptr;
    for (const IoLink& k : out_->links) if (k.out_gi == static_cast<int32_t>(gi) && k.patch) l = &k;
    if (!l) return fail("a patch output the device cannot take");
    reg = patch_out + l->base;
  }
  Var var, copy;
  if (!global_var(gi, &var) || !make_var(g.type, &copy)) return false;
  Oper flag = component(filled('T', static_cast<uint16_t>(stage_vars_[gi].flag), 1), 0);
  flag.neg = true;
  const uint32_t elements = var.count ? var.count : 1;
  for (uint32_t e = 0; e < elements; ++e) {
    const Var& el = var.count ? elems_[static_cast<uint64_t>(var.first) + e] : var;
    const Var& cl = copy.count ? elems_[static_cast<uint64_t>(copy.first) + e] : copy;
    for (uint32_t c = 0; c < el.cols; ++c) {
      Oper src = filled('O', static_cast<uint16_t>(outer || inner ? reg : reg++), el.rows);
      if (outer || inner)      // (a level: a component of TESSOUTER / TESSINNER)
        for (uint8_t& k : src.swz) k = static_cast<uint8_t>(e & 3);
      const Oper own = filled('T', el.temp[c], el.rows);
      inst_rows("CMP", cl.temp[c], el.rows, &flag, &own, &src);
    }
  }
  val_of(copy, out);
  return true;
}

// The position and the varyings of a vertex: at the end of a vertex or evaluation shader, at each EmitVertex() of a geometry shader. The last stage before the
// rasterizer converts the position to the device's clip space; the others pass it on as it is.
bool Gen::write_vertex_outputs() {
  const int32_t pi = m_.find_builtin_global(kBvPosition);
  if (pi < 0) return fail("no gl_Position");
  Var pos;
  if (!global_var(static_cast<uint64_t>(pi), &pos)) return false;
  const Oper p = filled('T', pos.temp[0], 4);
  if (!last_) {
    out_mov(0, "xyzw", p);
  } else {
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
  }
  if (!out_) return fail("a stage without its outputs");
  if (last_ && sh_.stages.clip && !write_clip_distances()) return false;
  for (const IoLink& l : out_->links) {
    Var v;
    if (l.out_gi < 0 || !global_var(static_cast<uint64_t>(l.out_gi), &v)) return fail("a varying the stage does not declare");
    // each element's columns, one register each
    uint32_t reg = 1 + l.base;
    const uint32_t elements = v.count ? v.count : 1;
    for (uint32_t e = 0; e < elements; ++e) {
      const Var& el = v.count ? elems_[static_cast<uint64_t>(v.first) + e] : v;
      for (uint32_t c = 0; c < el.cols; ++c) out_mov(reg++, mask_of(el.rows), filled('T', el.temp[c], el.rows), l.saturate);
    }
  }
  return true;
}

// The clip distances of the vertex, the eight of them in OUT[1 + varyings] and the one after (CLIPDIST[0], [1]): what the CPU path computes (GlProgram.cpp, the
// last stage's clip_distance_slot / clip_vertex_slot).
bool Gen::write_clip_distances() {
  const uint32_t first = 1 + out_->registers;
  clip_out = true;
  // a gl_ClipDistance the shader writes (its own redeclaration, or the built-in)
  for (uint64_t k = m_.globals.count(); k > 0; --k) {
    const Global& g = *m_.globals[k - 1];
    const bool distance = g.name.view().equals("gl_ClipDistance") && (g.kind == GlobalKind::Out || (g.kind == GlobalKind::Builtin && g.builtin == kBvClipDistance));
    if (!distance || !g.referenced || !g.type.array) continue;
    Var v;
    Oper one;
    if (!global_var(k - 1, &v) || !v.count || !scalar_imm(1.0f, &one)) return fail("clip distances the device cannot take");
    for (uint32_t i = 0; i < 8; ++i) {
      const Oper src = i < v.count ? filled('T', elems_[static_cast<uint64_t>(v.first) + i].temp[0], 1) : one;
      out_mov(first + i / 4, kChan[i % 4], src);
    }
    return true;
  }
  // else the user planes against gl_ClipVertex, or the compatibility profile's gl_Position
  int32_t src_gi = m_.find_global(StringView("gl_ClipVertex"));
  if (src_gi >= 0 && !m_.globals[static_cast<uint64_t>(src_gi)]->referenced) src_gi = -1;
  if (src_gi < 0 && m_.compatibility) src_gi = m_.find_builtin_global(kBvPosition);
  if (src_gi < 0) {      // (nothing to clip with: inside)
    Oper one;
    if (!scalar_imm(1.0f, &one)) return false;
    out_mov(first, "xyzw", one);
    out_mov(first + 1, "xyzw", one);
    return true;
  }
  Var src;
  if (!global_var(static_cast<uint64_t>(src_gi), &src) || src.count || src.is_struct || src.rows != 4) return fail("clip distances the device cannot take");
  if (clip_row_ < 0) {      // (the planes' rows, once: a geometry shader comes here at each EmitVertex())
    if (sh_.out->const_rows + 8 > sh_.limits.const_rows) return fail("too many uniforms");
    clip_row_ = static_cast<int32_t>(sh_.out->const_rows);
    sh_.out->const_rows += 8;
    for (uint32_t i = 0; i < 8; ++i)
      if (!sh_.out->constants.push_back(TgsiConstant{kTgsiClipPlanes, static_cast<uint16_t>(clip_row_ + static_cast<int32_t>(i)), 4, i * 4})) return fail("out of memory");
  }
  const uint32_t row = static_cast<uint32_t>(clip_row_);
  uint16_t t;
  if (!new_temp(&t)) return false;
  const Oper sv = filled('T', src.temp[0], 4);
  for (uint32_t i = 0; i < 8; ++i) {
    const Oper plane = filled('C', static_cast<uint16_t>(row + i), 4);
    inst("DP4", t, "x", &plane, &sv);
    out_mov(first + i / 4, kChan[i % 4], component(filled('T', t, 1), 0));
  }
  return true;
}

// The end of a control shader: invocation i writes the outputs of control point i (OUT[i][...]: the register's vertex cannot be a variable, so there is a test for
// each), and the patch outputs and tessellation levels it wrote.
bool Gen::control_end() {
  if (!out_ || invocation_gi_ < 0) return fail("a control stage without its outputs");
  const uint32_t n = m_.tcs_vertices > 0 ? static_cast<uint32_t>(m_.tcs_vertices) : 0;
  const Var& inv = globals_[static_cast<uint64_t>(invocation_gi_)];
  const Oper id = component(filled('T', inv.temp[0], 1), 0);
  const int32_t pos_gi = m_.find_global(StringView("gl_out.gl_Position"));
  const auto out2d = [&](uint32_t cp, uint32_t reg, uint32_t rows, uint16_t temp) {
    begin_inst("MOV");
    body.append(" OUT[");
    body.append_uint(cp);
    body.append("][");
    body.append_uint(reg);
    body.append("].");
    body.append(mask_of(rows));
    body.append(", ");
    put_oper(filled('T', temp, rows));
    end_inst();
  };
  for (uint32_t cp = 0; cp < n; ++cp) {
    Oper k;
    uint16_t t;
    if (!scalar_imm(static_cast<float>(cp), &k) || !new_temp(&t)) return false;
    inst("SEQ", t, "x", &id, &k);
    if (!flag_test(t, true)) return false;
    if (pos_gi >= 0 && stage_vars_[static_cast<uint64_t>(pos_gi)].own.valid) out2d(cp, 0, 4, stage_vars_[static_cast<uint64_t>(pos_gi)].own.temp[0]);
    for (const IoLink& l : out_->links) {
      if (l.patch || l.out_gi < 0) continue;
      const Var& own = stage_vars_[static_cast<uint64_t>(l.out_gi)].own;
      if (!own.valid) continue;      // (never written)
      for (uint32_t c = 0; c < own.cols; ++c) out2d(cp, 1 + l.base + c, own.rows, own.temp[c]);
    }
    begin_inst("ENDIF");
    end_inst();
  }
  const uint32_t patch_out = 1 + out_->registers;
  for (const IoLink& l : out_->links) {
    if (!l.patch || l.out_gi < 0 || stage_vars_[static_cast<uint64_t>(l.out_gi)].flag < 0) continue;
    Var v;
    if (!global_var(static_cast<uint64_t>(l.out_gi), &v) || !flag_test(static_cast<uint16_t>(stage_vars_[static_cast<uint64_t>(l.out_gi)].flag), true)) return false;
    uint32_t reg = patch_out + l.base;
    const uint32_t elements = v.count ? v.count : 1;
    for (uint32_t e = 0; e < elements; ++e) {
      const Var& el = v.count ? elems_[static_cast<uint64_t>(v.first) + e] : v;
      for (uint32_t c = 0; c < el.cols; ++c) out_mov(reg++, mask_of(el.rows), filled('T', el.temp[c], el.rows));
    }
    begin_inst("ENDIF");
    end_inst();
  }
  for (uint32_t lv = 0; lv < 2; ++lv) {
    const int32_t gi = m_.find_global(StringView(lv ? "gl_TessLevelInner" : "gl_TessLevelOuter"));
    if (gi < 0 || stage_vars_[static_cast<uint64_t>(gi)].flag < 0) continue;
    Var v;
    if (!global_var(static_cast<uint64_t>(gi), &v) || !v.count || !flag_test(static_cast<uint16_t>(stage_vars_[static_cast<uint64_t>(gi)].flag), true)) return false;
    for (uint32_t e = 0; e < v.count && e < 4; ++e) {
      out_mov(patch_out + out_->patch_registers + lv, kChan[e], filled('T', elems_[static_cast<uint64_t>(v.first) + e].temp[0], 1));
      if (out_->levels) out_mov(patch_out + out_->patch_registers + 2 + lv, kChan[e], filled('T', elems_[static_cast<uint64_t>(v.first) + e].temp[0], 1));
    }
    begin_inst("ENDIF");
    end_inst();
  }
  return true;
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

// DCL file[index], semantic[semantic_index] (semantic_index < 0: none); `per_vertex`: file[][index], an array with an element per vertex.
void put_decl(String& out, const char* file, bool per_vertex, uint32_t index, const char* semantic, int32_t semantic_index = -1) {
  out.append("DCL ");
  out.append(file);
  out.append(per_vertex ? "[][" : "[");
  out.append_uint(index);
  out.append("], ");
  out.append(semantic);
  if (semantic_index >= 0) {
    out.push_back('[');
    out.append_uint(static_cast<uint32_t>(semantic_index));
    out.push_back(']');
  }
  out.push_back('\n');
}

// The registers a geometry or tessellation stage reads and writes (see translate_program_to_tgsi), and its properties.
void stage_header(String& out, const Gen& g, const Shared& sh) {
  const Module& m = g.module();
  const Boundary* in = g.in_boundary();
  const Boundary* ob = g.out_boundary();
  const uint32_t in_regs = in ? in->registers : 0, out_regs = ob ? ob->registers : 0;
  switch (g.stage()) {
    case kStGeometry: {
      out.append("GEOM\nPROPERTY GS_INPUT_PRIMITIVE ");
      out.append(m.gs_input == 0 ? "POINTS" : m.gs_input == 1 ? "LINES" : m.gs_input == 0xA ? "LINES_ADJACENCY" : m.gs_input == 0xC ? "TRIANGLES_ADJACENCY" : "TRIANGLES");
      out.append("\nPROPERTY GS_OUTPUT_PRIMITIVE ");
      out.append(m.gs_output == 0 ? "POINTS" : m.gs_output == 3 ? "LINE_STRIP" : "TRIANGLE_STRIP");
      out.append("\nPROPERTY GS_MAX_OUTPUT_VERTICES ");
      out.append_uint(m.gs_max_vertices > 0 ? static_cast<uint32_t>(m.gs_max_vertices) : 0);
      out.append("\nPROPERTY GS_INVOCATIONS ");
      out.append_uint(m.gs_invocations);
      out.push_back('\n');
      break;
    }
    case kStControl:
      out.append("TESS_CTRL\nPROPERTY TCS_VERTICES_OUT ");
      out.append_uint(m.tcs_vertices > 0 ? static_cast<uint32_t>(m.tcs_vertices) : 0);
      out.push_back('\n');
      break;
    default: {
      // (the values are Gallium's numbers: PIPE_PRIM_LINES 1, TRIANGLES 4, QUADS 7; PIPE_TESS_SPACING_FRACTIONAL_ODD 0, FRACTIONAL_EVEN 1, EQUAL 2)
      out.append("TESS_EVAL\nPROPERTY TES_PRIM_MODE ");
      out.append(m.tes_mode == 0x8E7A ? "1" : m.tes_mode == 7 ? "7" : "4");
      out.append("\nPROPERTY TES_SPACING ");
      out.append(m.tes_spacing == 0x8E7B ? "0" : m.tes_spacing == 0x8E7C ? "1" : "2");
      out.append("\nPROPERTY TES_VERTEX_ORDER_CW ");
      out.append(m.tes_order == 0x0900 ? "1" : "0");
      out.append("\nPROPERTY TES_POINT_MODE ");
      out.append(m.tes_point_mode ? "1" : "0");
      out.push_back('\n');
      break;
    }
  }
  if (g.clip_out) out.append("PROPERTY NUM_CLIPDIST_ENABLED 8\n");
  put_decl(out, "IN", true, 0, "POSITION");
  for (uint32_t i = 0; i < in_regs; ++i) put_decl(out, "IN", true, 1 + i, "GENERIC", static_cast<int32_t>(i));
  if (g.primitive_in >= 0) put_decl(out, "IN", false, static_cast<uint32_t>(g.primitive_in), "PRIM_ID");
  if (g.stage() == kStEval && in) {
    for (uint32_t i = 0; i < in->patch_registers + (in->levels ? 2 : 0); ++i) put_decl(out, "IN", false, 1 + in_regs + i, "PATCH", static_cast<int32_t>(i));
  }
  const bool control = g.stage() == kStControl;
  put_decl(out, "OUT", control, 0, "POSITION");
  for (uint32_t i = 0; i < out_regs; ++i) put_decl(out, "OUT", control, 1 + i, "GENERIC", static_cast<int32_t>(i));
  if (g.clip_out)
    for (uint32_t i = 0; i < 2; ++i) put_decl(out, "OUT", false, 1 + out_regs + i, "CLIPDIST", static_cast<int32_t>(i));
  if (control && ob) {
    for (uint32_t i = 0; i < ob->patch_registers; ++i) put_decl(out, "OUT", false, 1 + out_regs + i, "PATCH", static_cast<int32_t>(i));
    put_decl(out, "OUT", false, 1 + out_regs + ob->patch_registers, "TESSOUTER");
    put_decl(out, "OUT", false, 2 + out_regs + ob->patch_registers, "TESSINNER");
    if (ob->levels)
      for (uint32_t i = 0; i < 2; ++i) put_decl(out, "OUT", false, 3 + out_regs + ob->patch_registers + i, "PATCH", static_cast<int32_t>(ob->patch_registers + i));
  }
  (void)sh;
}

bool assemble(String& out, const Gen& g, const Shared& sh, uint32_t attributes, uint32_t varyings, uint32_t const_rows, uint32_t samplers, uint32_t flat_mask) {
  out.clear();
  const bool fragment = g.stage() == kStFragment;
  if (g.stage() >= kStGeometry) {
    stage_header(out, g, sh);
  } else if (fragment) {
    out.append("FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n");
    for (uint32_t i = 0; i < varyings; ++i) {
      out.append("DCL IN[");
      out.append_uint(i);
      out.append("], GENERIC[");
      out.append_uint(i);
      out.append((flat_mask >> i) & 1 ? "], CONSTANT\n" : "], PERSPECTIVE\n");
    }
    // gl_FragCoord and gl_FrontFacing, after the varyings (see Gen::frag_input)
    if (g.position_in >= 0) {
      out.append("DCL IN[");
      out.append_uint(static_cast<uint32_t>(g.position_in));
      out.append("], POSITION, LINEAR\n");
    }
    if (g.face_in >= 0) {
      out.append("DCL IN[");
      out.append_uint(static_cast<uint32_t>(g.face_in));
      out.append("], FACE, CONSTANT\n");
    }
    out.append("DCL OUT[0], COLOR\n");
  } else {
    out.append("VERT\n");
    if (g.clip_out) out.append("PROPERTY NUM_CLIPDIST_ENABLED 8\n");
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
    if (g.clip_out)
      for (uint32_t i = 0; i < 2; ++i) put_decl(out, "OUT", false, 1 + varyings + i, "CLIPDIST", static_cast<int32_t>(i));
  }
  for (uint32_t i = 0; i < g.nsv; ++i) {
    out.append("DCL SV[");
    out.append_uint(i);
    out.append("], ");
    out.append(g.sv_names[i]);
    out.push_back('\n');
  }
  if (fragment)
    for (uint32_t i = 0; i < samplers; ++i) {
      out.append("DCL SAMP[");
      out.append_uint(i);
      out.append("]\nDCL SVIEW[");
      out.append_uint(i);
      out.append("], 2D, FLOAT\n");
    }
  write_header_decl(out, "CONST", const_rows);
  if (g.uses_addr) out.append("DCL ADDR[0]\n");
  write_header_decl(out, "TEMP", g.ntemp());
  for (uint64_t i = 0; i < g.imms().count(); ++i) {
    out.append("IMM[");
    out.append_uint(i);
    const Imm& im = g.imms()[i];
    out.append(im.bits ? "] UINT32 {" : "] FLT32 {");
    for (uint32_t k = 0; k < 4; ++k) {
      if (k) out.append(", ");
      if (im.bits) out.append_uint(__builtin_bit_cast(uint32_t, im.v[k]));
      else if (!put_float(out, im.v[k])) return false;
    }
    out.append("}\n");
  }
  out.append(g.body.view());
  out.append("  ");
  out.append_uint(g.instructions());
  return out.append(": END\n");
}

}  // namespace

bool translate_wgsl_to_tgsi(allocators::IAllocator& a, const Module& vs, int32_t vs_fn, const Module& fs, int32_t fs_fn, TgsiProgram* out, const char** why,
                            const TgsiLimits& limits) {
  (void)a;
  out->vs.clear();
  out->fs.clear();
  out->constants.clear();
  out->attributes.clear();
  out->samplers.clear();
  out->const_rows = 0;
  out->varyings = 0;
  out->uses_frag_coord = false;
  Shared sh(out, ArrayView<const TgsiLink>(), limits);
  bool ok = vs.language == Language::Wgsl && fs.language == Language::Wgsl && vs_fn >= 0 && fs_fn >= 0;
  if (!ok) sh.why = "not a WGSL pipeline";
  Gen vg(sh, vs, kStVertex, vs_fn);
  Gen fg(sh, fs, kStFragment, fs_fn);
  ok = ok && vg.run() && fg.run();
  if (ok && out->attributes.count() == 0) {
    ok = false;
    sh.why = "no vertex attributes";
  }
  if (ok) {
    out->varyings = sh.var_count;
    ok = assemble(out->vs, vg, sh, static_cast<uint32_t>(out->attributes.count()), out->varyings, out->const_rows, 0, 0) &&
         assemble(out->fs, fg, sh, static_cast<uint32_t>(out->attributes.count()), out->varyings, out->const_rows, static_cast<uint32_t>(out->samplers.count()), 0);
    if (!ok) sh.why = "out of memory";
  }
  if (why) *why = ok ? nullptr : (sh.why ? sh.why : "unsupported shader");
  return ok;
}

bool translate_to_tgsi(allocators::IAllocator& a, const Module& vs, const Module& fs, ArrayView<const TgsiLink> links, TgsiProgram* out, const char** why,
                       const TgsiLimits& limits) {
  return translate_program_to_tgsi(a, vs, fs, TgsiStages(), links, out, why, limits);
}

namespace {

// A value a varying register holds: a float, integer or boolean scalar or vector, or a float matrix.
bool varying_value(const Type& t) {
  return t.array == 0 && t.rows >= 1 && t.rows <= 4 && t.cols >= 1 && t.cols <= 4 && (t.base == Base::Float || ((t.base == Base::Int || t.base == Base::Uint) && t.cols == 1));
}

bool refuse(Shared& sh, const char* why) {
  if (!sh.why) sh.why = why;
  return false;
}

// The fragment shader's interface, from the links the program made: the last stage's output global and the fragment shader's input global of each.
bool final_boundary(Shared& sh, const Module& last, const Module& fs, Boundary& b) {
  for (const TgsiLink& link : sh.links) {
    IoLink l;
    for (uint64_t gi = 0; gi < last.globals.count(); ++gi) {
      const Global& c = *last.globals[gi];
      if (c.kind == GlobalKind::Out && c.builtin == kBvNone && c.slot == link.vs_slot) l.out_gi = static_cast<int32_t>(gi);
    }
    for (uint64_t gi = 0; gi < fs.globals.count(); ++gi) {
      const Global& c = *fs.globals[gi];
      if (c.kind == GlobalKind::In && c.builtin == kBvNone && c.slot == link.fs_slot) l.in_gi = static_cast<int32_t>(gi);
    }
    if (l.out_gi < 0) return refuse(sh, "a varying the vertex shader does not declare");
    const Global& g = *last.globals[static_cast<uint64_t>(l.out_gi)];
    if (!varying_value(g.type.element()) || g.type.array > 32) return refuse(sh, "a varying the device cannot take");
    if (g.flat != link.flat && !(link.flat && g.type.base != Base::Float)) return refuse(sh, "a varying the device cannot take");
    l.base = b.registers;
    l.regs = (g.type.array ? g.type.array : 1) * g.type.element().cols;
    l.flat = link.flat;
    l.saturate = link.saturate;
    b.registers += l.regs;
    if (!b.links.push_back(l)) return refuse(sh, "out of memory");
  }
  return b.registers <= sh.limits.varyings || refuse(sh, "too many varyings");
}

// The interface between two stages that are not the fragment shader's: what the consumer reads, matched by name to what the producer writes (gl_in[].x is the
// producer's x, or the control stage's gl_out[].x).
bool stage_boundary(Shared& sh, const Module& producer, bool producer_is_control, const Module& consumer, Boundary& b) {
  constexpr uint32_t kMaxPatchRegisters = 30;
  for (uint64_t ci = 0; ci < consumer.globals.count(); ++ci) {
    const Global& cg = *consumer.globals[ci];
    if (cg.kind != GlobalKind::In || !cg.referenced || cg.builtin != kBvNone) continue;
    const StringView cn = cg.name.view();
    if (cn.equals("gl_TessLevelOuter") || cn.equals("gl_TessLevelInner")) {
      b.levels = true;
      continue;
    }
    if (cn.equals("gl_in.gl_Position")) continue;      // (registers of their own)
    if (cn.starts_with("gl_")) return refuse(sh, "a built-in input the device cannot take");
    int32_t pi = -1;
    for (uint64_t k = producer.globals.count(); k > 0 && pi < 0; --k) {
      const Global& g = *producer.globals[k - 1];
      if (g.kind == GlobalKind::Out && g.name.view().equals(cn)) pi = static_cast<int32_t>(k - 1);
    }
    if (pi < 0) return refuse(sh, "an input the stage before does not write");
    const Global& pg = *producer.globals[static_cast<uint64_t>(pi)];
    if (cg.inner || pg.inner) return refuse(sh, "an array of arrays per vertex");
    IoLink l;
    l.out_gi = pi;
    l.in_gi = static_cast<int32_t>(ci);
    if (cg.patch) {
      if (!producer_is_control || !pg.patch) return refuse(sh, "a patch input the stage before does not write");
      if (!varying_value(cg.type.element()) || cg.type.array > 32) return refuse(sh, "a patch varying the device cannot take");
      l.patch = true;
      l.regs = (cg.type.array ? cg.type.array : 1) * cg.type.element().cols;
      l.base = b.patch_registers;
      b.patch_registers += l.regs;
    } else {
      if (!cg.per_vertex || (producer_is_control && !pg.per_vertex)) return refuse(sh, "an input the device cannot take");
      const Type et = cg.type.element();
      if (!varying_value(et)) return refuse(sh, "a varying the device cannot take");
      l.regs = et.cols;
      l.base = b.registers;
      b.registers += l.regs;
    }
    if (!b.links.push_back(l)) return refuse(sh, "out of memory");
  }
  // (a control stage that reads other invocations' positions reads them from a copy in a GENERIC register: the host does not give back the POSITION output of another)
  const int32_t pos_gi = producer_is_control ? producer.find_global(StringView("gl_out.gl_Position")) : -1;
  bool position_read = false;
  for (const Node& nd : producer.nodes)
    if (pos_gi >= 0 && nd.kind == NodeKind::Index && nd.a >= 0 && producer.nodes[static_cast<uint64_t>(nd.a)].kind == NodeKind::Global &&
        producer.nodes[static_cast<uint64_t>(nd.a)].ival == pos_gi) {
      const int32_t inv = producer.find_global(StringView("gl_InvocationID"));
      const bool own = nd.b >= 0 && producer.nodes[static_cast<uint64_t>(nd.b)].kind == NodeKind::Global && producer.nodes[static_cast<uint64_t>(nd.b)].ival == inv;
      position_read = position_read || !own;
    }
  if (position_read) {
    IoLink l;
    l.out_gi = pos_gi;
    l.regs = 1;
    l.base = b.registers++;
    if (!b.links.push_back(l)) return refuse(sh, "out of memory");
  }
  // (a control stage's outputs the evaluation stage does not read have registers too: its invocations may read each other's)
  for (uint64_t pi = 0; producer_is_control && pi < producer.globals.count(); ++pi) {
    const Global& pg = *producer.globals[pi];
    if (pg.kind != GlobalKind::Out || !pg.referenced || pg.builtin != kBvNone || pg.inner || pg.name.view().starts_with("gl_") || (!pg.per_vertex && !pg.patch)) continue;
    bool linked = false;
    for (const IoLink& l : b.links) linked = linked || l.out_gi == static_cast<int32_t>(pi);
    const Type et = pg.type.element();
    if (linked || !varying_value(et) || (pg.patch && pg.type.array > 32)) continue;
    IoLink l;
    l.out_gi = static_cast<int32_t>(pi);
    l.patch = pg.patch;
    l.regs = pg.patch ? (pg.type.array ? pg.type.array : 1) * et.cols : et.cols;
    l.base = pg.patch ? b.patch_registers : b.registers;
    (pg.patch ? b.patch_registers : b.registers) += l.regs;
    if (!b.links.push_back(l)) return refuse(sh, "out of memory");
  }
  if (b.registers > sh.limits.varyings) return refuse(sh, "too many varyings");
  return b.patch_registers + (b.levels ? 2 : 0) <= kMaxPatchRegisters || refuse(sh, "too many patch varyings");
}

// The control stage of a program that has an evaluation stage but none in its GLSL: each invocation passes its vertex on, and the tessellation levels are the
// default ones (glPatchParameterfv), in two CONST rows.
bool passthrough_control(Shared& sh, const Boundary& b, uint32_t vertices, uint32_t levels_row, String& out) {
  out.clear();
  out.append("TESS_CTRL\nPROPERTY TCS_VERTICES_OUT ");
  out.append_uint(vertices);
  out.push_back('\n');
  put_decl(out, "IN", true, 0, "POSITION");
  for (uint32_t i = 0; i < b.registers; ++i) put_decl(out, "IN", true, 1 + i, "GENERIC", static_cast<int32_t>(i));
  put_decl(out, "OUT", true, 0, "POSITION");
  for (uint32_t i = 0; i < b.registers; ++i) put_decl(out, "OUT", true, 1 + i, "GENERIC", static_cast<int32_t>(i));
  put_decl(out, "OUT", false, 1 + b.registers, "TESSOUTER");
  put_decl(out, "OUT", false, 2 + b.registers, "TESSINNER");
  if (b.levels)
    for (uint32_t i = 0; i < 2; ++i) put_decl(out, "OUT", false, 3 + b.registers + i, "PATCH", static_cast<int32_t>(i));
  out.append("DCL SV[0], INVOCATIONID\n");
  write_header_decl(out, "CONST", sh.out->const_rows);
  out.append("DCL TEMP[0]\n");
  for (uint32_t cp = 0; cp < vertices; ++cp) {
    out.append("IMM[");
    out.append_uint(cp);
    out.append("] UINT32 {");
    for (int k = 0; k < 4; ++k) {
      if (k) out.append(", ");
      out.append_uint(cp);
    }
    out.append("}\n");
  }
  uint32_t n = 0;
  const auto line = [&](const char* text) {
    out.append("  ");
    out.append_uint(n++);
    out.append(": ");
    out.append(text);
  };
  for (uint32_t cp = 0; cp < vertices; ++cp) {
    line("USEQ TEMP[0].x, SV[0].xxxx, IMM[");
    out.append_uint(cp);
    out.append("].xxxx\n");
    line("UIF TEMP[0].xxxx\n");
    for (uint32_t r = 0; r <= b.registers; ++r) {
      line("MOV OUT[");
      out.append_uint(cp);
      out.append("][");
      out.append_uint(r);
      out.append("].xyzw, IN[");
      out.append_uint(cp);
      out.append("][");
      out.append_uint(r);
      out.append("].xyzw\n");
    }
    line("ENDIF\n");
  }
  // (a component at a time: the host makes each a float of gl_TessLevelOuter / Inner)
  static const char* const kChannels[4] = {"x", "y", "z", "w"};
  for (uint32_t lv = 0; lv < 2; ++lv)
    for (uint32_t c = 0; c < (lv ? 2u : 4u); ++c) {
      line("MOV OUT[");
      out.append_uint(1 + lv + b.registers);
      out.append("].");
      out.append(kChannels[c]);
      out.append(", CONST[");
      out.append_uint(levels_row + lv);
      out.append("].");
      for (int k = 0; k < 4; ++k) out.append(kChannels[c]);
      out.append("\n");
    }
  for (uint32_t lv = 0; b.levels && lv < 2; ++lv) {      // (the copies the evaluation stage reads)
    line("MOV OUT[");
    out.append_uint(3 + lv + b.registers);
    out.append("].xyzw, CONST[");
    out.append_uint(levels_row + lv);
    out.append("].xyzw\n");
  }
  line("END\n");
  return true;
}

}  // namespace

bool translate_program_to_tgsi(allocators::IAllocator& a, const Module& vs, const Module& fs, const TgsiStages& stages, ArrayView<const TgsiLink> links,
                               TgsiProgram* out, const char** why, const TgsiLimits& limits) {
  String* const texts[5] = {&out->vs, &out->fs, &out->gs, &out->tcs, &out->tes};
  for (String* s : texts) s->clear();
  out->constants.clear();
  out->attributes.clear();
  out->samplers.clear();
  out->const_rows = 0;
  out->varyings = 0;
  out->uses_frag_coord = false;
  Shared sh(out, links, limits);
  sh.stages = stages;
  bool ok = vs.stage == Stage::Vertex && fs.stage == Stage::Fragment && links.size() <= Shared::kMaxLinks;
  if (!ok) sh.why = "unsupported stage or too many varyings";
  if (ok && ((stages.gs && stages.gs->stage != Stage::Geometry) || (stages.tcs && stages.tcs->stage != Stage::TessControl) || (stages.tes && stages.tes->stage != Stage::TessEval) ||
             (stages.tcs && !stages.tes))) {
    ok = false;
    sh.why = "unsupported stage";
  }
  if (ok && stages.gs && (stages.gs->gs_streams || stages.gs->gs_max_vertices <= 0)) {
    ok = false;
    sh.why = stages.gs->gs_streams ? "vertex streams" : "a geometry shader that emits nothing";
  }
  sh.eval_vertices = stages.tcs ? (stages.tcs->tcs_vertices > 0 ? static_cast<uint32_t>(stages.tcs->tcs_vertices) : 0) : stages.patch_vertices;
  if (ok && stages.tes && (stages.patch_vertices < 1 || stages.patch_vertices > kMaxArray || sh.eval_vertices < 1 || sh.eval_vertices > kMaxArray)) {
    ok = false;
    sh.why = "a patch size the device cannot take";
  }
  // the stages in order, and the interfaces between them
  const Module* chain[5];
  uint8_t kinds[5];
  uint32_t n = 0;
  chain[n] = &vs, kinds[n++] = kStVertex;
  if (stages.tcs) chain[n] = stages.tcs, kinds[n++] = kStControl;
  if (stages.tes) chain[n] = stages.tes, kinds[n++] = kStEval;
  if (stages.gs) chain[n] = stages.gs, kinds[n++] = kStGeometry;
  chain[n] = &fs, kinds[n++] = kStFragment;
  sh.nbounds = n - 1;
  for (uint32_t i = 0; ok && i + 2 < n; ++i) ok = stage_boundary(sh, *chain[i], kinds[i] == kStControl, *chain[i + 1], sh.bounds[i]);
  ok = ok && final_boundary(sh, *chain[n - 2], fs, sh.bounds[n - 2]);
  UniquePointer<Gen> gens[5];
  for (uint32_t i = 0; ok && i < n; ++i) {
    gens[i] = UniquePointer<Gen>::make(a, sh, *chain[i], kinds[i], -1, i > 0 ? &sh.bounds[i - 1] : nullptr, i + 1 < n ? &sh.bounds[i] : nullptr, i + 2 == n);
    if (!gens[i]) {
      ok = false;
      sh.why = "out of memory";
    }
  }
  for (uint32_t i = 0; ok && i < n; ++i) ok = gens[i]->run();
  if (ok && out->attributes.count() == 0 && !out->attributes.push_back(TgsiAttribute{kTgsiNoAttribute, 4})) {      // (a placeholder: see kTgsiNoAttribute)
    ok = false;
    sh.why = "out of memory";
  }
  // the default tessellation levels of a program without a control stage: two CONST rows
  uint32_t levels_row = 0;
  if (ok && stages.tes && !stages.tcs) {
    if (out->const_rows + 2 > sh.limits.const_rows) {
      ok = false;
      sh.why = "too many uniforms";
    } else {
      levels_row = out->const_rows;
      out->const_rows += 2;
      ok = out->constants.push_back(TgsiConstant{kTgsiPatchLevels, static_cast<uint16_t>(levels_row), 4, 0}) &&
           out->constants.push_back(TgsiConstant{kTgsiPatchLevels, static_cast<uint16_t>(levels_row + 1), 2, 4});
      if (!ok) sh.why = "out of memory";
    }
  }
  if (ok) {
    const Boundary& to_fs = sh.bounds[n - 2];
    out->varyings = to_fs.registers;
    uint32_t flat_mask = 0;
    for (const IoLink& l : to_fs.links)
      if (l.flat)
        for (uint32_t r = 0; r < l.regs; ++r)
          if (l.base + r < 32) flat_mask |= 1u << (l.base + r);
    const uint32_t attributes = static_cast<uint32_t>(out->attributes.count());
    for (uint32_t i = 0; ok && i < n; ++i) {
      String* text = kinds[i] == kStVertex ? &out->vs : kinds[i] == kStFragment ? &out->fs : kinds[i] == kStGeometry ? &out->gs : kinds[i] == kStControl ? &out->tcs : &out->tes;
      const uint32_t varyings = kinds[i] == kStFragment ? to_fs.registers : (i < sh.nbounds ? sh.bounds[i].registers : 0);
      ok = assemble(*text, *gens[i], sh, attributes, varyings, out->const_rows, kinds[i] == kStFragment ? static_cast<uint32_t>(out->samplers.count()) : 0,
                    kinds[i] == kStFragment ? flat_mask : 0);
    }
    if (ok && stages.tes && !stages.tcs) ok = passthrough_control(sh, sh.bounds[0], sh.eval_vertices, levels_row, out->tcs);
    if (!ok) sh.why = "out of memory";
  }
  if (ok) {
    if (stages.gs) {
      const Module& g = *stages.gs;
      out->gs_input = g.gs_input == 0 ? 0 : g.gs_input == 1 ? 1 : g.gs_input == 0xA ? 3 : g.gs_input == 0xC ? 4 : 2;
      out->gs_output = g.gs_output == 0 ? 0 : g.gs_output == 3 ? 1 : 2;
      out->gs_max_vertices = static_cast<uint32_t>(g.gs_max_vertices);
      out->gs_invocations = g.gs_invocations;
    }
    if (stages.tes) {
      const Module& e = *stages.tes;
      out->patch_vertices = stages.patch_vertices;
      out->tcs_vertices = sh.eval_vertices;
      out->tess_domain = e.tes_mode == 7 ? 2 : e.tes_mode == 0x8E7A ? 0 : 1;
      out->tess_partitioning = e.tes_spacing == 0x8E7B ? 2 : e.tes_spacing == 0x8E7C ? 3 : 0;
      out->tess_output = e.tes_point_mode ? 2 : e.tes_mode == 0x8E7A ? 3 : e.tes_order == 0x0900 ? 0 : 1;
    }
  }
  if (why) *why = ok ? nullptr : (sh.why ? sh.why : "unsupported shader");
  return ok;
}

}  // namespace webgl
