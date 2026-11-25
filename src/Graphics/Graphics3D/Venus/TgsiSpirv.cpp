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

#include "Graphics/Graphics3D/Venus/TgsiSpirv.hpp"
#include "Graphics/Graphics3D/Venus/Spirv.hpp"

namespace graphics::tgsi {
namespace {

using namespace spirv;

// How an opcode reads its sources.
enum class Ty : uint8_t { F, I, U };

constexpr uint32_t kMaxIn = 64, kMaxOut = 64, kMaxSv = 16, kMaxAddr = 4;

// What an input or output register is.
enum class IoKind : uint8_t {
  None,
  Generic,         // a location (vec4 of `type`)
  Position,        // gl_PerVertex.Position (an output; GS/TCS/TES inputs per vertex)
  FragCoord, Face, PrimId, SampleId, SamplePos, SampleMask, Layer, ViewportIndex, ClipDist, PointSize, FragDepth, Stencil,
  Color,           // a fragment colour (location semantic_index)
  TessOuter, TessInner, Patch, InvocationId, VertexId, InstanceId, TessCoord, BlockId, ThreadId, GridSize, BlockSize, VerticesIn, Helper,
};

struct Io {
  IoKind kind = IoKind::None;
  uint32_t var = 0;           // the variable
  uint32_t type = 0;          // its value type (a vector or scalar; for per-vertex arrays the element's)
  ValueType value = ValueType::Float;
  uint32_t index = 0;         // a semantic index (clip distance block, colour, patch)
  bool per_vertex = false;    // an array of vertices / control points
  uint32_t extra_var = 0;     // FS_COLOR0_WRITES_ALL_CBUFS: (the other colour outputs are kept in color_vars_)
};

struct TempArray {
  uint32_t first = 0, last = 0, id = 0;
  uint32_t var = 0;
};

// A structured construct being emitted.
struct Construct {
  bool loop = false;
  uint32_t merge = 0, cont = 0, header = 0, else_label = 0;
  bool has_else = false;
};

// An output register's stream-output copies (a register captured in part, or twice).
struct XfbCopy {
  uint32_t reg = 0, var = 0, type = 0;
  uint8_t first = 0, count = 0;
};

class Compiler {
 public:
  Compiler(allocators::IAllocator& allocator, const Program& program, const CompileOptions& options, CompileResult& result)
      : b_(allocator), p_(program), o_(options), r_(result), temp_vars_(allocator), temp_arrays_(allocator), constructs_(allocator), xfb_(allocator),
        allocator_(allocator) {}
  bool run();

 private:
  // ---- errors ----
  bool fail(const char* message, uint32_t line = 0) {
    if (r_.error.empty()) {
      r_.error.assign(StringView(message));
      r_.line = line ? line : line_;
    }
    ok_ = false;
    return false;
  }

  // ---- code ----
  uint32_t emit(uint16_t op, uint32_t type, std::initializer_list<uint32_t> args) {
    const uint32_t r = b_.id();
    uint32_t words[16];
    uint32_t n = 0;
    words[n++] = type;
    words[n++] = r;
    for (uint32_t a : args)
      if (n < 16) words[n++] = a;
    b_.op_list(Builder::kCode, op, words, n);
    return r;
  }
  void emit_void(uint16_t op, std::initializer_list<uint32_t> args) { b_.op(Builder::kCode, op, args); }
  uint32_t ext(uint32_t type, uint32_t inst, std::initializer_list<uint32_t> args) {
    const uint32_t r = b_.id();
    uint32_t words[12];
    uint32_t n = 0;
    words[n++] = type;
    words[n++] = r;
    words[n++] = glsl_;
    words[n++] = inst;
    for (uint32_t a : args)
      if (n < 12) words[n++] = a;
    b_.op_list(Builder::kCode, OpExtInst, words, n);
    return r;
  }
  uint32_t label() { return b_.id(); }
  void place(uint32_t l) {
    b_.op(Builder::kCode, OpLabel, {l});
    terminated_ = false;
  }
  void branch(uint32_t to) {
    if (terminated_) return;
    emit_void(OpBranch, {to});
    terminated_ = true;
  }
  // After a terminator in the middle of TGSI code: the code after it is in a block of its own (never reached).
  void resume() {
    if (terminated_) place(label());
  }
  uint32_t load(uint32_t type, uint32_t ptr) { return emit(OpLoad, type, {ptr}); }
  void store(uint32_t ptr, uint32_t value) { emit_void(OpStore, {ptr, value}); }
  uint32_t chain(uint32_t storage, uint32_t type, uint32_t base, std::initializer_list<uint32_t> indexes) {
    const uint32_t r = b_.id();
    uint32_t words[8];
    uint32_t n = 0;
    words[n++] = b_.t_pointer(storage, type);
    words[n++] = r;
    words[n++] = base;
    for (uint32_t i : indexes)
      if (n < 8) words[n++] = i;
    b_.op_list(Builder::kCode, OpAccessChain, words, n);
    return r;
  }
  uint32_t bitcast(uint32_t type, uint32_t v) { return emit(OpBitcast, type, {v}); }
  uint32_t extract(uint32_t type, uint32_t v, uint32_t i) { return emit(OpCompositeExtract, type, {v, i}); }
  uint32_t splat(uint32_t vtype, uint32_t s) { return emit(OpCompositeConstruct, vtype, {s, s, s, s}); }
  uint32_t shuffle(uint32_t type, uint32_t a, uint32_t c, const uint32_t sel[4], uint32_t n) {
    const uint32_t r = b_.id();
    uint32_t words[8] = {type, r, a, c};
    for (uint32_t i = 0; i < n; ++i) words[4 + i] = sel[i];
    b_.op_list(Builder::kCode, OpVectorShuffle, words, 4 + n);
    return r;
  }
  uint32_t select(uint32_t type, uint32_t cond, uint32_t a, uint32_t c) { return emit(OpSelect, type, {cond, a, c}); }
  uint32_t vec_of(Ty t) { return t == Ty::F ? f4_ : t == Ty::I ? i4_ : u4_; }
  uint32_t scalar_of(Ty t) { return t == Ty::F ? f_ : t == Ty::I ? i_ : u_; }
  uint32_t zero4() { return b_.c_null(u4_); }
  uint32_t fconst4(float v) {
    const uint32_t c = b_.c_float(v);
    return b_.c_composite(f4_, {c, c, c, c});
  }
  uint32_t uconst4(uint32_t v) {
    const uint32_t c = b_.c_uint(v);
    return b_.c_composite(u4_, {c, c, c, c});
  }

  // ---- registers ----
  uint32_t temp_var(uint32_t index);
  uint32_t temp_pointer(const Operand& o);
  uint32_t index_value(const Index& x);   // an int
  uint32_t raw(const Operand& o);         // the register's four words (no swizzle or modifiers)
  uint32_t src(const Instruction& ins, uint32_t n, Ty t);
  uint32_t swizzled(const Operand& o);
  bool interpolate(const Instruction& ins);
  uint32_t scalar(const Instruction& ins, uint32_t n, Ty t) { return extract(scalar_of(t), src(ins, n, t), 0); }
  void write(const Instruction& ins, uint32_t value, Ty t);   // the destination under its mask (value: a vec4 of type t)
  void write_bits(const Operand& d, uint32_t bits);           // value: a uvec4
  void write_scalar(const Instruction& ins, uint32_t value, Ty t) { write(ins, splat(vec_of(t), value), t); }
  uint32_t load_input(const Operand& o);
  uint32_t load_output(const Operand& o);
  uint32_t load_sv(const Operand& o);
  void store_output(const Operand& o, uint32_t bits);
  uint32_t vertex_index(const Operand& o);   // a per-vertex register's vertex (an int)
  uint32_t to_bits(uint32_t value, uint32_t type, ValueType vt, uint32_t components);   // a vector/scalar of `type` to uvec4

  // ---- declarations ----
  bool declare();
  bool declare_input(const Declaration& d);
  bool declare_output(const Declaration& d);
  bool declare_sv(const Declaration& d);
  void declare_resources();
  uint32_t per_vertex_block(uint32_t storage, uint32_t array_length);
  uint32_t make_io(uint32_t storage, uint32_t type, uint32_t array_length, uint32_t location, uint32_t builtin, bool flat_int);
  uint32_t texture_image_type(uint32_t unit, Target target);
  uint32_t storage_image_type(uint32_t k, Target target, ImageFormat format);

  // ---- instructions ----
  bool instruction(const Instruction& ins);
  bool alu(const Instruction& ins);
  bool doubles(const Instruction& ins);
  bool texture(const Instruction& ins);
  bool memory(const Instruction& ins);
  bool control(const Instruction& ins);
  uint32_t sampled_image(uint32_t unit, Target target);
  uint32_t texture_image(uint32_t unit);
  uint32_t buffer_word_pointer(const Operand& res, uint32_t word_index);
  uint32_t image_coordinate(Target t, uint32_t coord_bits);
  uint32_t image_texel_type(uint32_t k);
  uint32_t f2i(uint32_t v, bool is_unsigned);
  uint32_t condition(const Instruction& ins, bool integer);
  void finish_outputs();
  void emit_vertex(uint32_t stream);
  uint32_t stream_of(const Instruction& ins);
  bool out_has_kind(IoKind kind) const {
    for (const Io& io : out_)
      if (io.kind == kind) return true;
    return false;
  }
  bool out_has_color(uint32_t index) const {
    for (const Io& io : out_)
      if (io.kind == IoKind::Color && io.index == index) return true;
    return false;
  }

  Builder b_;
  const Program& p_;
  const CompileOptions& o_;
  CompileResult& r_;
  bool ok_ = true;
  uint32_t line_ = 0;
  bool terminated_ = false;

  // types
  uint32_t f_ = 0, u_ = 0, i_ = 0, bool_ = 0, d_ = 0, f2_ = 0, f3_ = 0, f4_ = 0, u2_ = 0, u3_ = 0, u4_ = 0, i2_ = 0, i3_ = 0, i4_ = 0, b4_ = 0, glsl_ = 0;

  // registers
  List<uint32_t> temp_vars_;          // TEMP[i] (0: not made yet)
  List<TempArray> temp_arrays_;
  uint32_t addr_[kMaxAddr] = {};
  Io in_[kMaxIn];
  Io out_[kMaxOut];
  Io sv_[kMaxSv];
  uint32_t color_vars_[8] = {};       // FS_COLOR0_WRITES_ALL_CBUFS: colour 0 into every target
  uint32_t color_types_[8] = {};
  uint32_t in_block_ = 0, out_block_ = 0, in_block_type_ = 0, out_block_type_ = 0, block_struct_ = 0;
  uint32_t in_vertices_ = 0;          // the length of per-vertex input arrays
  uint32_t out_vertices_ = 0;         // TCS outputs
  uint32_t invocation_var_ = 0;       // TCS: InvocationId (where literal control points are written)
  uint32_t clip_count_ = 0;
  // resources
  uint32_t constants_ = 0, constants_type_ = 0;
  uint32_t sview_var_[kMaxSviews] = {}, sview_type_[kMaxSviews] = {};
  Target sview_target_[kMaxSviews] = {};
  uint32_t samp_var_[kMaxSviews] = {};
  uint32_t storage_var_[kMaxStorage] = {};
  uint32_t image_var_[kMaxImages] = {}, image_type_[kMaxImages] = {}, image_decl_type_[kMaxImages] = {};
  Target image_target_[kMaxImages] = {};
  uint32_t shared_var_ = 0;
  uint32_t shared_words_ = 0;
  List<Construct> constructs_;
  List<XfbCopy> xfb_;
  allocators::IAllocator& allocator_;
  bool multi_stream_ = false;
  bool writes_depth_ = false;
};

// ---- small helpers --------------------------------------------------------------------------------------------------------------------------------------
bool is_texture_op(Op op) {
  switch (op) {
    case Op::Tex: case Op::Txp: case Op::Txb: case Op::Txd: case Op::Txl: case Op::Txl2: case Op::Txb2: case Op::Tex2: case Op::Txf: case Op::Txq:
    case Op::Tg4: case Op::Lodq: case Op::Txqs:
      return true;
    default: return false;
  }
}

bool is_memory_op(Op op) {
  switch (op) {
    case Op::Load: case Op::Store: case Op::Resq: case Op::AtomUadd: case Op::AtomAnd: case Op::AtomOr: case Op::AtomXor: case Op::AtomImax:
    case Op::AtomImin: case Op::AtomUmax: case Op::AtomUmin: case Op::AtomXchg: case Op::AtomCas:
      return true;
    default: return false;
  }
}

bool is_double_op(Op op) { return static_cast<uint32_t>(op) >= static_cast<uint32_t>(Op::Dadd) && static_cast<uint32_t>(op) <= static_cast<uint32_t>(Op::U2d); }

uint32_t spirv_format(ImageFormat f) {
  switch (f) {
    case ImageFormat::R8G8B8A8Unorm: case ImageFormat::B8G8R8A8Unorm: return FmtRgba8;
    case ImageFormat::R8G8B8A8Snorm: return FmtRgba8Snorm;
    case ImageFormat::R8G8B8A8Uint: return FmtRgba8ui;
    case ImageFormat::R8G8B8A8Sint: return FmtRgba8i;
    case ImageFormat::R16G16B16A16Float: return FmtRgba16f;
    case ImageFormat::R16G16B16A16Unorm: return FmtRgba16;
    case ImageFormat::R16G16B16A16Snorm: return FmtRgba16Snorm;
    case ImageFormat::R16G16B16A16Uint: return FmtRgba16ui;
    case ImageFormat::R16G16B16A16Sint: return FmtRgba16i;
    case ImageFormat::R32G32B32A32Float: return FmtRgba32f;
    case ImageFormat::R32G32B32A32Uint: return FmtRgba32ui;
    case ImageFormat::R32G32B32A32Sint: return FmtRgba32i;
    case ImageFormat::R32G32Float: return FmtRg32f;
    case ImageFormat::R32G32Uint: return FmtRg32ui;
    case ImageFormat::R32G32Sint: return FmtRg32i;
    case ImageFormat::R16G16Float: return FmtRg16f;
    case ImageFormat::R16G16Unorm: return FmtRg16;
    case ImageFormat::R16G16Snorm: return FmtRg16Snorm;
    case ImageFormat::R16G16Uint: return FmtRg16ui;
    case ImageFormat::R16G16Sint: return FmtRg16i;
    case ImageFormat::R8G8Unorm: return FmtRg8;
    case ImageFormat::R8G8Snorm: return FmtRg8Snorm;
    case ImageFormat::R8G8Uint: return FmtRg8ui;
    case ImageFormat::R8G8Sint: return FmtRg8i;
    case ImageFormat::R32Float: return FmtR32f;
    case ImageFormat::R32Uint: return FmtR32ui;
    case ImageFormat::R32Sint: return FmtR32i;
    case ImageFormat::R16Float: return FmtR16f;
    case ImageFormat::R16Unorm: return FmtR16;
    case ImageFormat::R16Snorm: return FmtR16Snorm;
    case ImageFormat::R16Uint: return FmtR16ui;
    case ImageFormat::R16Sint: return FmtR16i;
    case ImageFormat::R8Unorm: return FmtR8;
    case ImageFormat::R8Snorm: return FmtR8Snorm;
    case ImageFormat::R8Uint: return FmtR8ui;
    case ImageFormat::R8Sint: return FmtR8i;
    case ImageFormat::R10G10B10A2Unorm: return FmtRgb10A2;
    case ImageFormat::R10G10B10A2Uint: return FmtRgb10a2ui;
    case ImageFormat::R11G11B10Float: return FmtR11fG11fB10f;
    default: return FmtUnknown;
  }
}

ValueType format_value(ImageFormat f) {
  switch (f) {
    case ImageFormat::R8G8B8A8Uint: case ImageFormat::R16G16B16A16Uint: case ImageFormat::R32G32B32A32Uint: case ImageFormat::R32G32Uint:
    case ImageFormat::R16G16Uint: case ImageFormat::R8G8Uint: case ImageFormat::R32Uint: case ImageFormat::R16Uint: case ImageFormat::R8Uint:
    case ImageFormat::R10G10B10A2Uint:
      return ValueType::Uint;
    case ImageFormat::R8G8B8A8Sint: case ImageFormat::R16G16B16A16Sint: case ImageFormat::R32G32B32A32Sint: case ImageFormat::R32G32Sint:
    case ImageFormat::R16G16Sint: case ImageFormat::R8G8Sint: case ImageFormat::R32Sint: case ImageFormat::R16Sint: case ImageFormat::R8Sint:
      return ValueType::Sint;
    default: return ValueType::Float;
  }
}

// The image dimension, whether arrayed and multisampled, and how many coordinates address a texel (not the array layer) of a target.
struct TargetShape {
  uint32_t dim = Dim2D;
  bool arrayed = false, ms = false, shadow = false;
  uint32_t coords = 2;   // address coordinates (without the layer)
};
bool shape_of(Target t, TargetShape* s) {
  *s = TargetShape{};
  switch (t) {
    case Target::Buffer: s->dim = DimBuffer, s->coords = 1; return true;
    case Target::T1D: s->dim = Dim1D, s->coords = 1; return true;
    case Target::T2D: case Target::Rect: return true;
    case Target::T3D: s->dim = Dim3D, s->coords = 3; return true;
    case Target::Cube: s->dim = DimCube, s->coords = 3; return true;
    case Target::T1DArray: s->dim = Dim1D, s->arrayed = true, s->coords = 1; return true;
    case Target::T2DArray: s->arrayed = true; return true;
    case Target::CubeArray: s->dim = DimCube, s->arrayed = true, s->coords = 3; return true;
    case Target::T2DMsaa: s->ms = true; return true;
    case Target::T2DArrayMsaa: s->ms = true, s->arrayed = true; return true;
    case Target::Shadow1D: s->dim = Dim1D, s->coords = 1, s->shadow = true; return true;
    case Target::Shadow2D: case Target::ShadowRect: s->shadow = true; return true;
    case Target::Shadow1DArray: s->dim = Dim1D, s->coords = 1, s->arrayed = true, s->shadow = true; return true;
    case Target::Shadow2DArray: s->arrayed = true, s->shadow = true; return true;
    case Target::ShadowCube: s->dim = DimCube, s->coords = 3, s->shadow = true; return true;
    case Target::ShadowCubeArray: s->dim = DimCube, s->coords = 3, s->arrayed = true, s->shadow = true; return true;
    default: return false;
  }
}

// ---- declarations -----------------------------------------------------------------------------------------------------------------------------------
uint32_t Compiler::per_vertex_block(uint32_t storage, uint32_t array_length) {
  if (!block_struct_) {
    // gl_PerVertex: the same members in every stage of the pipeline (Position, PointSize, the clip distances when the pipeline has them).
    const uint32_t members = clip_count_ ? 3 : 2;
    const uint32_t clip_array = clip_count_ ? b_.t_array(f_, clip_count_) : 0;
    block_struct_ = b_.id();
    if (clip_count_) b_.op(Builder::kGlobals, OpTypeStruct, {block_struct_, f4_, f_, clip_array});
    else b_.op(Builder::kGlobals, OpTypeStruct, {block_struct_, f4_, f_});
    b_.decorate(block_struct_, DecBlock);
    b_.member_decorate(block_struct_, 0, DecBuiltIn, {BiPosition});
    b_.member_decorate(block_struct_, 1, DecBuiltIn, {BiPointSize});
    if (members == 3) b_.member_decorate(block_struct_, 2, DecBuiltIn, {BiClipDistance});
    if (p_.stage == Stage::Geometry) b_.capability(CapGeometryPointSize);
    if (p_.stage == Stage::TessControl || p_.stage == Stage::TessEval) b_.capability(CapTessellationPointSize);
  }
  const uint32_t type = array_length ? b_.t_array(block_struct_, array_length) : block_struct_;
  return b_.global(storage, type);
}

uint32_t Compiler::make_io(uint32_t storage, uint32_t type, uint32_t array_length, uint32_t location, uint32_t builtin, bool flat_int) {
  const uint32_t t = array_length ? b_.t_array(type, array_length) : type;
  const uint32_t v = b_.global(storage, t);
  if (builtin != ~0u) b_.decorate(v, DecBuiltIn, {builtin});
  else b_.decorate(v, DecLocation, {location});
  if (flat_int && storage == Input && p_.stage == Stage::Fragment) b_.decorate(v, DecFlat);
  return v;
}

bool Compiler::declare_input(const Declaration& d) {
  if (d.last >= kMaxIn) return fail("too many input registers", d.line);
  const bool per_vertex = d.two_dimensional;
  if (per_vertex && !in_vertices_) return fail("a per-vertex input outside the geometry and tessellation stages", d.line);
  for (uint32_t r = d.first; r <= d.last; ++r) {
    Io& io = in_[r];
    io.per_vertex = per_vertex;
    io.index = d.semantic_index + (r - d.first);
    const uint32_t n = per_vertex ? in_vertices_ : 0;
    switch (d.semantic) {
      case Semantic::None:
      case Semantic::Generic:
      case Semantic::TexCoord:
      case Semantic::Color:
      case Semantic::BColor:
      case Semantic::Fog: {
        // (VS: attribute r; other stages: GENERIC[i] at location i; colours and texture coordinates of legacy producers follow the generics)
        uint32_t location = d.semantic == Semantic::None ? r : io.index;
        if (d.semantic == Semantic::TexCoord) location = 8 + io.index;
        if (d.semantic == Semantic::Color || d.semantic == Semantic::BColor) location = 24 + io.index;
        if (d.semantic == Semantic::Fog) location = 30;
        io.value = p_.stage == Stage::Vertex && r < 16 ? o_.attribute_type[r] : ValueType::Float;
        io.type = io.value == ValueType::Float ? f4_ : io.value == ValueType::Sint ? i4_ : u4_;
        io.kind = IoKind::Generic;
        // (a fragment input that is flat, or not of floats, is decorated Flat by make_io)
        io.var = make_io(Input, io.type, n, location, ~0u, d.interpolation == Interpolation::Constant || io.value != ValueType::Float);
        if (p_.stage == Stage::Fragment) {
          if (d.interpolation == Interpolation::Constant) {
          } else if (d.interpolation == Interpolation::Linear) {
            b_.decorate(io.var, DecNoPerspective);
          }
          if (d.location == InterpolationLocation::Centroid) b_.decorate(io.var, DecCentroid);
          if (d.location == InterpolationLocation::Sample) {
            b_.capability(CapSampleRateShading);
            b_.decorate(io.var, DecSample);
          }
        }
        break;
      }
      case Semantic::Position:
        if (p_.stage == Stage::Fragment) {
          io.kind = IoKind::FragCoord, io.type = f4_;
          io.var = make_io(Input, f4_, 0, 0, BiFragCoord, false);
        } else if (per_vertex) {
          io.kind = IoKind::Position, io.type = f4_;
          if (!in_block_) in_block_ = per_vertex_block(Input, in_vertices_);
          io.var = in_block_;
        } else {
          return fail("a position input outside the fragment, geometry and tessellation stages", d.line);
        }
        break;
      case Semantic::Face:
        io.kind = IoKind::Face, io.type = bool_;
        io.var = make_io(Input, bool_, 0, 0, BiFrontFacing, false);
        break;
      case Semantic::PrimId:
      case Semantic::PrimitiveId:
        io.kind = IoKind::PrimId, io.type = i_, io.value = ValueType::Sint;
        if (p_.stage == Stage::Fragment) b_.capability(CapGeometry);
        io.var = make_io(Input, i_, 0, 0, BiPrimitiveId, true);
        break;
      case Semantic::SampleId:
        b_.capability(CapSampleRateShading);
        io.kind = IoKind::SampleId, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiSampleId, true);
        break;
      case Semantic::SamplePos:
        b_.capability(CapSampleRateShading);
        io.kind = IoKind::SamplePos, io.type = f2_;
        io.var = make_io(Input, f2_, 0, 0, BiSamplePosition, false);
        break;
      case Semantic::SampleMask:
        io.kind = IoKind::SampleMask, io.type = i_;
        io.var = make_io(Input, b_.t_array(i_, 1), 0, 0, BiSampleMask, true);
        break;
      case Semantic::Patch:
        io.kind = IoKind::Patch, io.type = f4_;
        io.var = make_io(Input, f4_, 0, o_.patch_base + io.index, ~0u, false);
        b_.decorate(io.var, DecPatch);
        break;
      case Semantic::TessOuter:
        io.kind = IoKind::TessOuter, io.type = f_;
        io.var = make_io(Input, b_.t_array(f_, 4), 0, 0, BiTessLevelOuter, false);
        b_.decorate(io.var, DecPatch);
        break;
      case Semantic::TessInner:
        io.kind = IoKind::TessInner, io.type = f_;
        io.var = make_io(Input, b_.t_array(f_, 2), 0, 0, BiTessLevelInner, false);
        b_.decorate(io.var, DecPatch);
        break;
      case Semantic::ClipDist:
        io.kind = IoKind::ClipDist, io.type = f_;
        if (!per_vertex) return fail("a clip distance input outside the geometry and tessellation stages", d.line);
        if (!in_block_) in_block_ = per_vertex_block(Input, in_vertices_);
        io.var = in_block_;
        break;
      default: return fail("an input semantic this compiler does not take", d.line);
    }
  }
  return true;
}

bool Compiler::declare_output(const Declaration& d) {
  if (d.last >= kMaxOut) return fail("too many output registers", d.line);
  const bool tcs = p_.stage == Stage::TessControl;
  const bool per_vertex = d.two_dimensional;
  if (per_vertex && !tcs) return fail("per-vertex outputs outside the tessellation control stage", d.line);
  const uint32_t n = per_vertex ? out_vertices_ : 0;
  for (uint32_t r = d.first; r <= d.last; ++r) {
    Io& io = out_[r];
    io.per_vertex = per_vertex;
    io.index = d.semantic_index + (r - d.first);
    switch (d.semantic) {
      case Semantic::Position:
        if (p_.stage == Stage::Fragment) {
          io.kind = IoKind::FragDepth, io.type = f_;
          io.var = make_io(Output, f_, 0, 0, BiFragDepth, false);
          writes_depth_ = true;
        } else {
          io.kind = IoKind::Position, io.type = f4_;
          if (!out_block_) out_block_ = per_vertex_block(Output, n);
          io.var = out_block_;
        }
        break;
      case Semantic::Psize:
        io.kind = IoKind::PointSize, io.type = f_;
        if (!out_block_) out_block_ = per_vertex_block(Output, n);
        io.var = out_block_;
        break;
      case Semantic::ClipDist:
        if (io.index > 1 || clip_count_ < 4 * (io.index + 1)) return fail("a clip distance output the pipeline has no room for", d.line);
        io.kind = IoKind::ClipDist, io.type = f_;
        if (!out_block_) out_block_ = per_vertex_block(Output, n);
        io.var = out_block_;
        break;
      case Semantic::Generic:
      case Semantic::TexCoord:
      case Semantic::Fog:
      case Semantic::BColor:
      case Semantic::Color:
        if (p_.stage == Stage::Fragment && d.semantic == Semantic::Color) {
          if (io.index >= 8) return fail("colour output past the eighth", d.line);
          io.kind = IoKind::Color;
          io.value = o_.color_type[io.index];
          io.type = io.value == ValueType::Float ? f4_ : io.value == ValueType::Sint ? i4_ : u4_;
          const bool second = o_.dual_source && io.index == 1;
          io.var = make_io(Output, io.type, 0, second ? 0 : io.index, ~0u, false);
          if (o_.dual_source && io.index <= 1) b_.decorate(io.var, DecIndex, {io.index});
          color_vars_[io.index] = io.var;
          color_types_[io.index] = io.type;
          if (io.index == 0 && p_.has(Property::FsColor0WritesAllCbufs) && p_.get(Property::FsColor0WritesAllCbufs))
            for (uint32_t t = 1; t < o_.color_targets && t < 8; ++t) {
              color_types_[t] = o_.color_type[t] == ValueType::Float ? f4_ : o_.color_type[t] == ValueType::Sint ? i4_ : u4_;
              color_vars_[t] = make_io(Output, color_types_[t], 0, t, ~0u, false);
            }
        } else {
          uint32_t location = io.index;
          if (d.semantic == Semantic::TexCoord) location = 8 + io.index;
          if (d.semantic == Semantic::Color || d.semantic == Semantic::BColor) location = 24 + io.index;
          if (d.semantic == Semantic::Fog) location = 30;
          io.kind = IoKind::Generic, io.type = f4_;
          io.var = make_io(Output, f4_, n, location, ~0u, false);
        }
        break;
      case Semantic::Layer:
      case Semantic::ViewportIndex: {
        const bool layer = d.semantic == Semantic::Layer;
        io.kind = layer ? IoKind::Layer : IoKind::ViewportIndex, io.type = i_;
        if (p_.stage == Stage::Fragment) return fail("a layer output from a fragment shader", d.line);
        if (p_.stage != Stage::Geometry) {
          if (!o_.output_layer_any_stage) return fail("a layer or viewport from a vertex or domain shader (the device lacks shaderOutputLayer)", d.line);
          b_.capability(layer ? CapShaderLayer : CapShaderViewportIndex);
        } else {
          b_.capability(layer ? CapGeometry : CapMultiViewport);
        }
        if (!layer) b_.capability(CapMultiViewport);
        io.var = make_io(Output, i_, 0, 0, layer ? BiLayer : BiViewportIndex, false);
        break;
      }
      case Semantic::TessOuter:
        io.kind = IoKind::TessOuter, io.type = f_;
        io.var = make_io(Output, b_.t_array(f_, 4), 0, 0, BiTessLevelOuter, false);
        b_.decorate(io.var, DecPatch);
        break;
      case Semantic::TessInner:
        io.kind = IoKind::TessInner, io.type = f_;
        io.var = make_io(Output, b_.t_array(f_, 2), 0, 0, BiTessLevelInner, false);
        b_.decorate(io.var, DecPatch);
        break;
      case Semantic::Patch:
        io.kind = IoKind::Patch, io.type = f4_;
        io.var = make_io(Output, f4_, 0, o_.patch_base + io.index, ~0u, false);
        b_.decorate(io.var, DecPatch);
        break;
      case Semantic::SampleMask:
        io.kind = IoKind::SampleMask, io.type = i_;
        io.var = make_io(Output, b_.t_array(i_, 1), 0, 0, BiSampleMask, false);
        break;
      default: return fail("an output semantic this compiler does not take", d.line);
    }
  }
  return true;
}

bool Compiler::declare_sv(const Declaration& d) {
  if (d.last >= kMaxSv) return fail("too many system values", d.line);
  for (uint32_t r = d.first; r <= d.last; ++r) {
    Io& io = sv_[r];
    switch (d.semantic) {
      case Semantic::VertexId:
      case Semantic::VertexIdNoBase:
        io.kind = IoKind::VertexId, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiVertexIndex, false);
        if (d.semantic == Semantic::VertexIdNoBase) {
          b_.capability(CapDrawParameters);
          io.extra_var = make_io(Input, i_, 0, 0, BiBaseVertex, false);
        }
        break;
      case Semantic::InstanceId:
        // (TGSI's instance id counts from 0 whatever the draw's first instance is: Vulkan's InstanceIndex minus BaseInstance)
        b_.capability(CapDrawParameters);
        io.kind = IoKind::InstanceId, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiInstanceIndex, false);
        io.extra_var = make_io(Input, i_, 0, 0, BiBaseInstance, false);
        break;
      case Semantic::BaseVertex:
        b_.capability(CapDrawParameters);
        io.kind = IoKind::VertexId, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiBaseVertex, false);
        break;
      case Semantic::BaseInstance:
        b_.capability(CapDrawParameters);
        io.kind = IoKind::VertexId, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiBaseInstance, false);
        break;
      case Semantic::InvocationId:
        io.kind = IoKind::InvocationId, io.type = i_;
        io.var = invocation_var_ ? invocation_var_ : make_io(Input, i_, 0, 0, BiInvocationId, false);
        invocation_var_ = io.var;
        break;
      case Semantic::PrimId:
      case Semantic::PrimitiveId:
        io.kind = IoKind::PrimId, io.type = i_;
        if (p_.stage == Stage::Fragment) b_.capability(CapGeometry);
        io.var = make_io(Input, i_, 0, 0, BiPrimitiveId, true);
        break;
      case Semantic::TessCoord:
        io.kind = IoKind::TessCoord, io.type = f3_;
        io.var = make_io(Input, f3_, 0, 0, BiTessCoord, false);
        break;
      case Semantic::VerticesIn:
        io.kind = IoKind::VerticesIn, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiPatchVertices, false);
        break;
      case Semantic::BlockId:
        io.kind = IoKind::BlockId, io.type = u3_;
        io.var = make_io(Input, u3_, 0, 0, BiWorkgroupId, false);
        break;
      case Semantic::ThreadId:
        io.kind = IoKind::ThreadId, io.type = u3_;
        io.var = make_io(Input, u3_, 0, 0, BiLocalInvocationId, false);
        break;
      case Semantic::GridSize:
        io.kind = IoKind::GridSize, io.type = u3_;
        io.var = make_io(Input, u3_, 0, 0, BiNumWorkgroups, false);
        break;
      case Semantic::BlockSize: io.kind = IoKind::BlockSize, io.type = u3_; break;   // (a constant: the fixed block size)
      case Semantic::SampleId:
        b_.capability(CapSampleRateShading);
        io.kind = IoKind::SampleId, io.type = i_;
        io.var = make_io(Input, i_, 0, 0, BiSampleId, true);
        break;
      case Semantic::SamplePos:
        b_.capability(CapSampleRateShading);
        io.kind = IoKind::SamplePos, io.type = f2_;
        io.var = make_io(Input, f2_, 0, 0, BiSamplePosition, false);
        break;
      case Semantic::SampleMask:
        io.kind = IoKind::SampleMask, io.type = i_;
        io.var = make_io(Input, b_.t_array(i_, 1), 0, 0, BiSampleMask, true);
        break;
      case Semantic::Face:
        io.kind = IoKind::Face, io.type = bool_;
        io.var = make_io(Input, bool_, 0, 0, BiFrontFacing, false);
        break;
      case Semantic::HelperInvocation:
        io.kind = IoKind::Helper, io.type = bool_;
        io.var = make_io(Input, bool_, 0, 0, BiHelperInvocation, false);
        break;
      case Semantic::Position:
        io.kind = IoKind::FragCoord, io.type = f4_;
        io.var = make_io(Input, f4_, 0, 0, BiFragCoord, false);
        break;
      default: return fail("a system value this compiler does not take", d.line);
    }
  }
  return true;
}

uint32_t Compiler::texture_image_type(uint32_t unit, Target target) {
  TargetShape s;
  if (!shape_of(target, &s)) return 0;
  const ValueType vt = o_.sview_type[unit];
  const uint32_t sampled = vt == ValueType::Float ? f_ : vt == ValueType::Sint ? i_ : u_;
  if (s.dim == Dim1D) b_.capability(CapSampled1D);
  if (s.dim == DimBuffer) b_.capability(CapSampledBuffer);
  if (s.dim == DimCube && s.arrayed) b_.capability(CapSampledCubeArray);
  return b_.t_image(sampled, s.dim, s.arrayed, s.ms, 1, FmtUnknown);
}

uint32_t Compiler::storage_image_type(uint32_t k, Target target, ImageFormat format) {
  TargetShape s;
  if (!shape_of(target, &s)) return 0;
  ImageFormat f = format != ImageFormat::None ? format : o_.image_format[k];
  const uint32_t sf = spirv_format(f);
  ValueType vt = f != ImageFormat::None ? format_value(f) : o_.image_type[k];
  const uint32_t sampled = vt == ValueType::Float ? f_ : vt == ValueType::Sint ? i_ : u_;
  if (s.dim == Dim1D) b_.capability(CapImage1D);
  if (s.dim == DimBuffer) b_.capability(CapImageBuffer);
  if (s.dim == DimCube && s.arrayed) b_.capability(CapImageCubeArray);
  if (s.ms) b_.capability(CapStorageImageMultisample);
  if (sf == FmtUnknown) {
    b_.capability(CapStorageImageReadWithoutFormat);
    b_.capability(CapStorageImageWriteWithoutFormat);
  } else if (sf != FmtRgba32f && sf != FmtRgba16f && sf != FmtR32f && sf != FmtRgba8 && sf != FmtRgba8Snorm && sf != FmtRgba32i && sf != FmtRgba16i &&
             sf != FmtRgba8i && sf != FmtR32i && sf != FmtRgba32ui && sf != FmtRgba16ui && sf != FmtRgba8ui && sf != FmtR32ui) {
    b_.capability(CapStorageImageExtendedFormats);
  }
  image_type_[k] = sampled;
  return b_.t_image(sampled, s.dim, s.arrayed, s.ms, 2, sf);
}

bool Compiler::declare() {
  // the per-vertex array lengths
  if (p_.stage == Stage::Geometry) {
    switch (p_.get(Property::GsInputPrimitive)) {
      case 0: in_vertices_ = 1; break;
      case 1: case 3: in_vertices_ = 2; break;
      case 10: case 11: in_vertices_ = 4; break;
      case 12: case 13: in_vertices_ = 6; break;
      default: in_vertices_ = 3; break;
    }
  } else if (p_.stage == Stage::TessControl || p_.stage == Stage::TessEval) {
    in_vertices_ = o_.patch_vertices && p_.stage == Stage::TessControl ? o_.patch_vertices : 32;
    out_vertices_ = p_.get(Property::TcsVerticesOut);
    if (p_.stage == Stage::TessControl && (out_vertices_ == 0 || out_vertices_ > 32)) return fail("a tessellation control shader needs TCS_VERTICES_OUT 1 .. 32");
  }
  for (const Declaration& d : p_.declarations) {
    switch (d.file) {
      case File::Temp:
        if (d.last >= kMaxConstantRows * 2) return fail("too many temporaries", d.line);
        if (d.array) {
          TempArray a;
          a.first = d.first, a.last = d.last, a.id = d.array;
          if (!temp_arrays_.push_back(a)) return fail("out of memory");
        }
        break;
      case File::Addr:
        if (d.last >= kMaxAddr) return fail("too many address registers", d.line);
        break;
      case File::In:
        if (!declare_input(d)) return false;
        break;
      case File::Out:
        if (!declare_output(d)) return false;
        break;
      case File::Sv:
        if (!declare_sv(d)) return false;
        break;
      case File::Const:
        if (d.dimension != 0) return fail("constant buffers other than 0", d.line);
        if (d.last >= o_.constant_rows) return fail("constants past the constant buffer", d.line);
        break;
      case File::Sview:
        if (d.last >= kMaxSviews) return fail("SVIEW past the last unit", d.line);
        for (uint32_t u = d.first; u <= d.last; ++u) {
          sview_target_[u] = d.target;
          if (d.target == Target::Buffer) r_.sview_buffer[u] = true;
          if (d.target == Target::T2DMsaa || d.target == Target::T2DArrayMsaa) r_.sview_multisampled[u] = true;
        }
        break;
      case File::Samp:
        if (d.last >= kMaxSviews) return fail("SAMP past the last unit", d.line);
        break;
      case File::Image:
        if (d.last >= kMaxImages) return fail("IMAGE past the last slot", d.line);
        for (uint32_t k = d.first; k <= d.last; ++k) {
          image_target_[k] = d.target;
          if (d.target == Target::Buffer) r_.image_buffer[k] = true;
          const uint32_t t = storage_image_type(k, d.target, d.format);
          if (!t) return fail("an image target this compiler does not take", d.line);
          image_var_[k] = b_.global(UniformConstant, t);
          image_decl_type_[k] = t;
          b_.decorate(image_var_[k], DecDescriptorSet, {0});
          b_.decorate(image_var_[k], DecBinding, {binding_image(o_.stage_slot, k)});
          r_.image_mask |= uint64_t{1} << k;
        }
        break;
      case File::Buffer:
        if (d.last >= kMaxStorage) return fail("BUFFER past the last slot", d.line);
        for (uint32_t k = d.first; k <= d.last; ++k) {
          const uint32_t rt = b_.id();
          b_.op(Builder::kGlobals, OpTypeRuntimeArray, {rt, u_});
          b_.decorate(rt, DecArrayStride, {4});
          const uint32_t st = b_.id();
          b_.op(Builder::kGlobals, OpTypeStruct, {st, rt});
          b_.decorate(st, DecBlock);
          b_.member_decorate(st, 0, DecOffset, {0});
          storage_var_[k] = b_.global(StorageBuffer, st);
          b_.decorate(storage_var_[k], DecDescriptorSet, {0});
          b_.decorate(storage_var_[k], DecBinding, {binding_storage(o_.stage_slot, k)});
          r_.storage_mask |= uint64_t{1} << k;
        }
        break;
      case File::Memory:
        if (!d.shared || d.first != 0 || d.last != 0) return fail("only MEMORY[0], SHARED is taken", d.line);
        shared_words_ = o_.shared_bytes / 4 ? o_.shared_bytes / 4 : 1;
        shared_var_ = b_.global(Workgroup, b_.t_array(u_, shared_words_));
        break;
      default: return fail("a declaration of a file this compiler does not take", d.line);
    }
  }
  // the constant buffer (made whether or not CONST is declared: an IMM-only shader may still be given one; it is cheap)
  {
    const uint32_t arr = b_.id();
    b_.op(Builder::kGlobals, OpTypeArray, {arr, u4_, b_.c_uint(o_.constant_rows)});
    b_.decorate(arr, DecArrayStride, {16});
    constants_type_ = b_.id();
    b_.op(Builder::kGlobals, OpTypeStruct, {constants_type_, arr});
    b_.decorate(constants_type_, DecBlock);
    b_.member_decorate(constants_type_, 0, DecOffset, {0});
    constants_ = b_.global(Uniform, constants_type_);
    b_.decorate(constants_, DecDescriptorSet, {0});
    b_.decorate(constants_, DecBinding, {kBindingConstants});
  }
  // samplers and texture views: one variable each for the units the shader names (SAMP without SVIEW: the view the instruction's target says)
  for (const Declaration& d : p_.declarations)
    if (d.file == File::Samp)
      for (uint32_t u = d.first; u <= d.last && u < kMaxUnits; ++u) {   // (the typed buffer views' SAMPs are not used: a buffer is fetched, not sampled)
        samp_var_[u] = b_.global(UniformConstant, b_.t_sampler());
        b_.decorate(samp_var_[u], DecDescriptorSet, {0});
        b_.decorate(samp_var_[u], DecBinding, {kBindingSampler + u});
        r_.sampler_mask |= 1u << u;
      }
  for (uint32_t u = 0; u < kMaxSviews; ++u)
    if (sview_target_[u] != Target::None) {
      sview_type_[u] = texture_image_type(u, sview_target_[u]);
      if (!sview_type_[u]) return fail("an SVIEW target this compiler does not take");
      sview_var_[u] = b_.global(UniformConstant, sview_type_[u]);
      b_.decorate(sview_var_[u], DecDescriptorSet, {0});
      b_.decorate(sview_var_[u], DecBinding, {u < kMaxUnits ? kBindingTexture + u : binding_texel(o_.stage_slot, u - kMaxUnits)});
      r_.sview_mask |= 1u << u;
    }
  // an output block for the stages before the rasterizer (the position is always written there) and the point size
  const bool last_vertex_stage = p_.stage == Stage::Vertex || p_.stage == Stage::TessEval || p_.stage == Stage::Geometry;
  if (last_vertex_stage && !out_block_) out_block_ = per_vertex_block(Output, 0);
  return ok_;
}

// ---- registers --------------------------------------------------------------------------------------------------------------------------------------
uint32_t Compiler::temp_var(uint32_t index) {
  while (temp_vars_.count() <= index)
    if (!temp_vars_.push_back(0)) return fail("out of memory"), 0;
  if (!temp_vars_[index]) {
    const uint32_t v = b_.id();
    b_.op(Builder::kFunctionVariables, OpVariable, {b_.t_pointer(Function, u4_), v, Function, zero4()});
    temp_vars_[index] = v;
  }
  return temp_vars_[index];
}

uint32_t Compiler::index_value(const Index& x) {
  if (!x.indirect) return b_.c_int(x.value);
  uint32_t reg = 0;
  if (x.file == File::Addr) {
    if (x.index >= kMaxAddr) return fail("ADDR past the last"), b_.c_int(0);
    if (!addr_[x.index]) {
      addr_[x.index] = b_.id();
      b_.op(Builder::kFunctionVariables, OpVariable, {b_.t_pointer(Function, i4_), addr_[x.index], Function, b_.c_null(i4_)});
    }
    reg = extract(i_, load(i4_, addr_[x.index]), x.component);
  } else if (x.file == File::Temp) {
    reg = bitcast(i_, extract(u_, load(u4_, temp_var(x.index)), x.component));
  } else {
    return fail("an indirect index of a file this compiler does not take"), b_.c_int(0);
  }
  return x.value ? emit(OpIAdd, i_, {reg, b_.c_int(x.value)}) : reg;
}

uint32_t Compiler::temp_pointer(const Operand& o) {
  const int32_t at = o.index.value;
  for (TempArray& a : temp_arrays_) {
    const bool named = o.array ? o.array == a.id : (!o.index.indirect && at >= static_cast<int32_t>(a.first) && at <= static_cast<int32_t>(a.last));
    if (!named) continue;
    if (!a.var) {
      a.var = b_.id();
      const uint32_t t = b_.t_array(u4_, a.last - a.first + 1);
      b_.op(Builder::kFunctionVariables, OpVariable, {b_.t_pointer(Function, t), a.var, Function, b_.c_null(t)});
    }
    uint32_t idx;
    if (o.index.indirect) {
      // (TGSI's index: the register plus the offset, which names the element as a temporary number; out of range is clamped as the producers clamp before)
      idx = emit(OpISub, i_, {index_value(o.index), b_.c_int(static_cast<int32_t>(a.first))});
      idx = ext(i_, GlslSMax, {idx, b_.c_int(0)});
      idx = ext(i_, GlslSMin, {idx, b_.c_int(static_cast<int32_t>(a.last - a.first))});
    } else {
      idx = b_.c_int(at - static_cast<int32_t>(a.first));
    }
    return chain(Function, u4_, a.var, {idx});
  }
  if (o.index.indirect) return fail("an indirect temporary outside a declared array"), temp_var(0);
  if (at < 0) return fail("a negative temporary"), temp_var(0);
  return temp_var(static_cast<uint32_t>(at));
}

uint32_t Compiler::to_bits(uint32_t value, uint32_t type, ValueType vt, uint32_t components) {
  if (components == 4) return vt == ValueType::Uint ? value : bitcast(u4_, value);
  if (components == 1) {
    const uint32_t s = type == u_ ? value : bitcast(u_, value);
    return splat(u4_, s);
  }
  uint32_t parts[4] = {b_.c_uint(0), b_.c_uint(0), b_.c_uint(0), b_.c_uint(0)};
  const uint32_t comp = vt == ValueType::Float ? f_ : vt == ValueType::Sint ? i_ : u_;
  for (uint32_t c = 0; c < components; ++c) {
    const uint32_t e = extract(comp, value, c);
    parts[c] = comp == u_ ? e : bitcast(u_, e);
  }
  return emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]});
}

uint32_t Compiler::vertex_index(const Operand& o) {
  if (!o.two_dimensional) return b_.c_int(0);
  return index_value(o.dimension);
}

uint32_t Compiler::load_input(const Operand& o) {
  if (o.index.indirect || o.index.value < 0 || o.index.value >= static_cast<int32_t>(kMaxIn)) return fail("an input index this compiler does not take"), zero4();
  const Io& io = in_[o.index.value];
  if (io.kind == IoKind::None) return fail("an undeclared input"), zero4();
  if (io.per_vertex != o.two_dimensional) return fail("a per-vertex input read without its vertex (or the other way)"), zero4();
  const uint32_t v = io.per_vertex ? vertex_index(o) : 0;
  switch (io.kind) {
    case IoKind::Generic:
    case IoKind::Patch: {
      const uint32_t ptr = io.per_vertex ? chain(Input, io.type, io.var, {v}) : io.var;
      uint32_t bits = to_bits(load(io.type, ptr), io.type, io.value, 4);
      if (p_.stage == Stage::Vertex && o.index.value < 16 && (o_.attribute_one_w >> o.index.value & 1))   // (w is 1: 1.0, or the integer 1)
        bits = emit(OpCompositeInsert, u4_, {b_.c_uint(io.value == ValueType::Float ? 0x3F800000u : 1u), bits, 3});
      return bits;
    }
    case IoKind::Position: return bitcast(u4_, load(f4_, chain(Input, f4_, io.var, {v, b_.c_int(0)})));
    case IoKind::ClipDist: {
      uint32_t parts[4];
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t el = io.index * 4 + c;
        parts[c] = el < clip_count_ ? bitcast(u_, load(f_, chain(Input, f_, io.var, {v, b_.c_int(2), b_.c_int(static_cast<int32_t>(el))}))) : b_.c_uint(0);
      }
      return emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]});
    }
    case IoKind::FragCoord: return bitcast(u4_, load(f4_, io.var));
    case IoKind::Face: {
      const uint32_t f = select(f_, load(bool_, io.var), b_.c_float(1.0f), b_.c_float(-1.0f));
      return bitcast(u4_, splat(f4_, f));
    }
    case IoKind::PrimId:
    case IoKind::SampleId: return splat(u4_, bitcast(u_, load(i_, io.var)));
    case IoKind::SamplePos: return to_bits(load(f2_, io.var), f2_, ValueType::Float, 2);
    case IoKind::SampleMask: return splat(u4_, bitcast(u_, load(i_, chain(Input, i_, io.var, {b_.c_int(0)}))));
    case IoKind::TessOuter:
    case IoKind::TessInner: {
      const uint32_t n = io.kind == IoKind::TessOuter ? 4 : 2;
      uint32_t parts[4] = {b_.c_uint(0), b_.c_uint(0), b_.c_uint(0), b_.c_uint(0)};
      for (uint32_t c = 0; c < n; ++c) parts[c] = bitcast(u_, load(f_, chain(Input, f_, io.var, {b_.c_int(static_cast<int32_t>(c))})));
      return emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]});
    }
    default: return fail("an input this compiler does not read"), zero4();
  }
}

uint32_t Compiler::load_output(const Operand& o) {
  if (o.index.indirect || o.index.value < 0 || o.index.value >= static_cast<int32_t>(kMaxOut)) return fail("an output index this compiler does not take"), zero4();
  const Io& io = out_[o.index.value];
  if (io.kind == IoKind::None) return fail("an undeclared output read"), zero4();
  const uint32_t v = io.per_vertex ? vertex_index(o) : 0;
  switch (io.kind) {
    case IoKind::Generic:
    case IoKind::Patch:
    case IoKind::Color: {
      const uint32_t ptr = io.per_vertex ? chain(Output, io.type, io.var, {v}) : io.var;
      return to_bits(load(io.type, ptr), io.type, io.value, 4);
    }
    case IoKind::Position: {
      const uint32_t ptr = io.per_vertex ? chain(Output, f4_, io.var, {v, b_.c_int(0)}) : chain(Output, f4_, io.var, {b_.c_int(0)});
      return bitcast(u4_, load(f4_, ptr));
    }
    case IoKind::TessOuter:
    case IoKind::TessInner: {
      const uint32_t n = io.kind == IoKind::TessOuter ? 4 : 2;
      uint32_t parts[4] = {b_.c_uint(0), b_.c_uint(0), b_.c_uint(0), b_.c_uint(0)};
      for (uint32_t c = 0; c < n; ++c) parts[c] = bitcast(u_, load(f_, chain(Output, f_, io.var, {b_.c_int(static_cast<int32_t>(c))})));
      return emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]});
    }
    case IoKind::FragDepth: {
      const uint32_t z = bitcast(u_, load(f_, io.var));
      return emit(OpCompositeConstruct, u4_, {b_.c_uint(0), b_.c_uint(0), z, b_.c_uint(0)});
    }
    case IoKind::Layer:
    case IoKind::ViewportIndex: return splat(u4_, bitcast(u_, load(i_, io.var)));
    default: return fail("an output this compiler does not read back"), zero4();
  }
}

uint32_t Compiler::load_sv(const Operand& o) {
  if (o.index.indirect || o.index.value < 0 || o.index.value >= static_cast<int32_t>(kMaxSv)) return fail("a system value index this compiler does not take"), zero4();
  const Io& io = sv_[o.index.value];
  switch (io.kind) {
    case IoKind::VertexId: {
      uint32_t v = load(i_, io.var);
      if (io.extra_var) v = emit(OpISub, i_, {v, load(i_, io.extra_var)});
      return splat(u4_, bitcast(u_, v));
    }
    case IoKind::InstanceId: return splat(u4_, bitcast(u_, emit(OpISub, i_, {load(i_, io.var), load(i_, io.extra_var)})));
    case IoKind::InvocationId:
    case IoKind::PrimId:
    case IoKind::VerticesIn:
    case IoKind::SampleId: return splat(u4_, bitcast(u_, load(i_, io.var)));
    case IoKind::TessCoord: return to_bits(load(f3_, io.var), f3_, ValueType::Float, 3);
    case IoKind::BlockId:
    case IoKind::ThreadId:
    case IoKind::GridSize: return to_bits(load(u3_, io.var), u3_, ValueType::Uint, 3);
    case IoKind::BlockSize: {
      const uint32_t parts[4] = {p_.get(Property::CsFixedBlockWidth), p_.get(Property::CsFixedBlockHeight), p_.get(Property::CsFixedBlockDepth), 0};
      return b_.c_uvec4(parts);
    }
    case IoKind::SamplePos: return to_bits(load(f2_, io.var), f2_, ValueType::Float, 2);
    case IoKind::SampleMask: return splat(u4_, bitcast(u_, load(i_, chain(Input, i_, io.var, {b_.c_int(0)}))));
    case IoKind::Face: return bitcast(u4_, splat(f4_, select(f_, load(bool_, io.var), b_.c_float(1.0f), b_.c_float(-1.0f))));
    case IoKind::Helper: return splat(u4_, select(u_, load(bool_, io.var), b_.c_uint(~0u), b_.c_uint(0)));
    case IoKind::FragCoord: return bitcast(u4_, load(f4_, io.var));
    default: return fail("an undeclared system value"), zero4();
  }
}

uint32_t Compiler::raw(const Operand& o) {
  switch (o.file) {
    case File::Temp: return load(u4_, temp_pointer(o));
    case File::Imm: {
      if (o.index.indirect || o.index.value < 0 || static_cast<uint64_t>(o.index.value) >= p_.immediates.count()) return fail("an immediate that is not declared"), zero4();
      return b_.c_uvec4(p_.immediates[static_cast<uint64_t>(o.index.value)].bits);
    }
    case File::Const: {
      if (o.two_dimensional && (o.dimension.indirect || o.dimension.value != 0)) return fail("constant buffers other than 0"), zero4();
      r_.constants = true;
      const uint32_t idx = index_value(o.index);
      if (!o.index.indirect && (o.index.value < 0 || static_cast<uint32_t>(o.index.value) >= o_.constant_rows)) return fail("a constant past the buffer"), zero4();
      return load(u4_, chain(Uniform, u4_, constants_, {b_.c_int(0), idx}));
    }
    case File::In: return load_input(o);
    case File::Out: return load_output(o);
    case File::Sv: return load_sv(o);
    case File::Addr: {
      if (o.index.value < 0 || o.index.value >= static_cast<int32_t>(kMaxAddr)) return fail("ADDR past the last"), zero4();
      Index x;
      x.indirect = true, x.file = File::Addr, x.index = static_cast<uint32_t>(o.index.value);
      (void)index_value(x);   // (makes the variable)
      return bitcast(u4_, load(i4_, addr_[o.index.value]));
    }
    default: return fail("a source of a file this compiler does not take"), zero4();
  }
}

// INTERP_CENTROID / INTERP_SAMPLE / INTERP_OFFSET of a fragment shader's float input: GLSL.std.450's InterpolateAtCentroid / AtSample / AtOffset of its variable, swizzled as the
// operand says (the sample an integer, the offset two floats in pixels).
bool Compiler::interpolate(const Instruction& ins) {
  if (p_.stage != Stage::Fragment || ins.src_count < 1 || ins.dst_count != 1) return fail("an interpolation outside the fragment stage");
  const Operand& o = ins.src[0];
  if (o.file != File::In || o.index.indirect || o.index.value < 0 || o.index.value >= static_cast<int32_t>(kMaxIn) || o.negate || o.absolute) return fail("an interpolation of what is not an input");
  const Io& io = in_[o.index.value];
  if (io.kind != IoKind::Generic || io.type != f4_ || !io.var || io.per_vertex) return fail("an interpolation of an input that is not a float varying");
  b_.capability(CapInterpolationFunction);
  uint32_t v = 0;
  if (ins.op == Op::InterpCentroid) {
    v = ext(f4_, GlslInterpolateAtCentroid, {io.var});
  } else if (ins.op == Op::InterpSample) {
    v = ext(f4_, GlslInterpolateAtSample, {io.var, scalar(ins, 1, Ty::I)});
  } else {
    const uint32_t offset = src(ins, 1, Ty::F);
    const uint32_t sel[4] = {0, 1, 0, 0};
    v = ext(f4_, GlslInterpolateAtOffset, {io.var, shuffle(f2_, offset, offset, sel, 2)});
  }
  if (!(o.swizzle[0] == 0 && o.swizzle[1] == 1 && o.swizzle[2] == 2 && o.swizzle[3] == 3)) {
    const uint32_t sel[4] = {o.swizzle[0], o.swizzle[1], o.swizzle[2], o.swizzle[3]};
    v = shuffle(f4_, v, v, sel, 4);
  }
  write(ins, v, Ty::F);
  return ok_;
}

uint32_t Compiler::swizzled(const Operand& o) {
  const uint32_t v = raw(o);
  if (o.swizzle[0] == 0 && o.swizzle[1] == 1 && o.swizzle[2] == 2 && o.swizzle[3] == 3) return v;
  const uint32_t sel[4] = {o.swizzle[0], o.swizzle[1], o.swizzle[2], o.swizzle[3]};
  return shuffle(u4_, v, v, sel, 4);
}

uint32_t Compiler::src(const Instruction& ins, uint32_t n, Ty t) {
  if (n >= ins.src_count) return fail("a missing source operand"), zero4();
  const Operand& o = ins.src[n];
  uint32_t v = swizzled(o);
  const uint32_t vt = vec_of(t);
  if (t != Ty::U) v = bitcast(vt, v);
  if (o.absolute) v = t == Ty::F ? ext(vt, GlslFAbs, {v}) : ext(vt, GlslSAbs, {v});
  if (o.negate) v = emit(t == Ty::F ? OpFNegate : OpSNegate, vt, {v});
  return v;
}

namespace {
uint32_t merge_selector(uint8_t mask, uint32_t sel[4]) {
  for (uint32_t c = 0; c < 4; ++c) sel[c] = (mask >> c & 1) ? 4 + c : c;
  return 4;
}
}  // namespace

void Compiler::store_output(const Operand& d, uint32_t bits) {
  if (d.index.indirect || d.index.value < 0 || d.index.value >= static_cast<int32_t>(kMaxOut)) {
    fail("an output index this compiler does not take");
    return;
  }
  const uint32_t reg = static_cast<uint32_t>(d.index.value);
  const Io& io = out_[reg];
  const uint8_t mask = d.mask;
  uint32_t sel[4];
  merge_selector(mask, sel);
  // (a tessellation control shader writes the control point it is: the producers write OUT[cp][r] with a literal cp inside a test of the invocation id)
  const uint32_t v = io.per_vertex ? load(i_, invocation_var_) : 0;
  auto merged_store = [&](uint32_t ptr, uint32_t type, ValueType vt) -> uint32_t {
    uint32_t full = bits;
    if (mask != 0xF) full = shuffle(u4_, to_bits(load(type, ptr), type, vt, 4), bits, sel, 4);
    store(ptr, vt == ValueType::Uint ? full : bitcast(type, full));
    return full;
  };
  uint32_t full = bits;
  switch (io.kind) {
    case IoKind::Generic:
    case IoKind::Patch:
      full = merged_store(io.per_vertex ? chain(Output, io.type, io.var, {v}) : io.var, io.type, io.value);
      break;
    case IoKind::Color:
      full = merged_store(io.var, io.type, io.value);
      // (FS_COLOR0_WRITES_ALL_CBUFS: the other targets' outputs exist only then, and take colour 0 too)
      if (io.index == 0)
        for (uint32_t t = 1; t < 8; ++t)
          if (color_vars_[t] && !out_has_color(t)) store(color_vars_[t], color_types_[t] == u4_ ? full : bitcast(color_types_[t], full));
      break;
    case IoKind::Position: {
      const uint32_t ptr = io.per_vertex ? chain(Output, f4_, io.var, {v, b_.c_int(0)}) : chain(Output, f4_, io.var, {b_.c_int(0)});
      full = merged_store(ptr, f4_, ValueType::Float);
      break;
    }
    case IoKind::PointSize: {
      if (!(mask & 1)) break;
      const uint32_t ptr = io.per_vertex ? chain(Output, f_, io.var, {v, b_.c_int(1)}) : chain(Output, f_, io.var, {b_.c_int(1)});
      store(ptr, bitcast(f_, extract(u_, bits, 0)));
      break;
    }
    case IoKind::ClipDist:
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask >> c & 1)) continue;
        const uint32_t el = io.index * 4 + c;
        if (el >= clip_count_) continue;
        const uint32_t value = (o_.clip_mask >> el & 1) ? bitcast(f_, extract(u_, bits, c)) : b_.c_float(1.0f);
        const uint32_t idx = b_.c_int(static_cast<int32_t>(el));
        const uint32_t ptr = io.per_vertex ? chain(Output, f_, io.var, {v, b_.c_int(2), idx}) : chain(Output, f_, io.var, {b_.c_int(2), idx});
        store(ptr, value);
      }
      break;
    case IoKind::FragDepth:
      if (mask & 4) store(io.var, bitcast(f_, extract(u_, bits, 2)));
      break;
    case IoKind::Layer:
    case IoKind::ViewportIndex:
      if (mask & 1) store(io.var, bitcast(i_, extract(u_, bits, 0)));
      break;
    case IoKind::SampleMask:
      if (mask & 1) store(chain(Output, i_, io.var, {b_.c_int(0)}), bitcast(i_, extract(u_, bits, 0)));
      break;
    case IoKind::TessOuter:
    case IoKind::TessInner: {
      const uint32_t n = io.kind == IoKind::TessOuter ? 4 : 2;
      for (uint32_t c = 0; c < n; ++c)
        if (mask >> c & 1) store(chain(Output, f_, io.var, {b_.c_int(static_cast<int32_t>(c))}), bitcast(f_, extract(u_, bits, c)));
      break;
    }
    default: fail("an undeclared output written"); return;
  }
  // stream output copies of this register
  for (const XfbCopy& x : xfb_) {
    if (x.reg != reg) continue;
    if (mask != 0xF && io.kind != IoKind::Generic && io.kind != IoKind::Position && io.kind != IoKind::Patch && io.kind != IoKind::Color) full = bits;
    if (x.count == 1) {
      store(x.var, bitcast(f_, extract(u_, full, x.first)));
    } else {
      uint32_t s[4];
      for (uint32_t c = 0; c < x.count; ++c) s[c] = x.first + c;
      store(x.var, bitcast(x.type, shuffle(b_.t_vector(u_, x.count), full, full, s, x.count)));
    }
  }
}

void Compiler::write_bits(const Operand& d, uint32_t bits) {
  switch (d.file) {
    case File::Temp: {
      const uint32_t ptr = temp_pointer(d);
      if (d.mask == 0xF) {
        store(ptr, bits);
      } else {
        uint32_t sel[4];
        merge_selector(d.mask, sel);
        store(ptr, shuffle(u4_, load(u4_, ptr), bits, sel, 4));
      }
      return;
    }
    case File::Out: store_output(d, bits); return;
    case File::Addr: {
      if (d.index.value < 0 || d.index.value >= static_cast<int32_t>(kMaxAddr)) {
        fail("ADDR past the last");
        return;
      }
      Index x;
      x.indirect = true, x.file = File::Addr, x.index = static_cast<uint32_t>(d.index.value);
      (void)index_value(x);
      const uint32_t ptr = addr_[d.index.value];
      uint32_t sel[4];
      merge_selector(d.mask, sel);
      store(ptr, shuffle(i4_, load(i4_, ptr), bitcast(i4_, bits), sel, 4));
      return;
    }
    default: fail("a destination of a file this compiler does not take"); return;
  }
}

void Compiler::write(const Instruction& ins, uint32_t value, Ty t) {
  if (ins.dst_count == 0) {
    fail("an instruction without its destination");
    return;
  }
  if (ins.saturate) {
    if (t != Ty::F) {
      fail("_SAT on an integer instruction");
      return;
    }
    value = ext(f4_, GlslFClamp, {value, fconst4(0.0f), fconst4(1.0f)});
  }
  write_bits(ins.dst[0], t == Ty::U ? value : bitcast(u4_, value));
}

// ---- arithmetic -------------------------------------------------------------------------------------------------------------------------------------
// A float converted to an integer the way Direct3D and TGSI define it: NaN is 0, out-of-range values clamp (SPIR-V leaves them undefined).
uint32_t Compiler::f2i(uint32_t v, bool is_unsigned) {
  const uint32_t nan = emit(OpIsNan, b4_, {v});
  if (is_unsigned) {
    const uint32_t hi = emit(OpFOrdGreaterThanEqual, b4_, {v, fconst4(4294967296.0f)});
    const uint32_t lo = emit(OpFOrdLessThanEqual, b4_, {v, fconst4(0.0f)});
    uint32_t r = emit(OpConvertFToU, u4_, {ext(f4_, GlslFClamp, {v, fconst4(0.0f), fconst4(4294967040.0f)})});
    r = select(u4_, hi, uconst4(0xFFFFFFFFu), r);
    r = select(u4_, lo, uconst4(0), r);
    return select(u4_, nan, uconst4(0), r);
  }
  const uint32_t hi = emit(OpFOrdGreaterThanEqual, b4_, {v, fconst4(2147483648.0f)});
  uint32_t r = bitcast(u4_, emit(OpConvertFToS, i4_, {ext(f4_, GlslFClamp, {v, fconst4(-2147483648.0f), fconst4(2147483520.0f)})}));
  r = select(u4_, hi, uconst4(0x7FFFFFFFu), r);
  return select(u4_, nan, uconst4(0), r);
}

bool Compiler::alu(const Instruction& ins) {
  const Op op = ins.op;
  auto bin = [&](uint16_t spv, Ty t) {
    const uint32_t a = src(ins, 0, t), c = src(ins, 1, t);
    write(ins, emit(spv, vec_of(t), {a, c}), t);
  };
  auto glsl1 = [&](uint32_t inst, Ty t) { write(ins, ext(vec_of(t), inst, {src(ins, 0, t)}), t); };
  auto glsl2 = [&](uint32_t inst, Ty t) { write(ins, ext(vec_of(t), inst, {src(ins, 0, t), src(ins, 1, t)}), t); };
  // a comparison whose result is ~0 / 0 (mask) or 1.0 / 0.0 (float)
  auto compare = [&](uint16_t spv, Ty t, bool mask) {
    const uint32_t cond = emit(spv, b4_, {src(ins, 0, t), src(ins, 1, t)});
    if (mask) write(ins, select(u4_, cond, uconst4(~0u), uconst4(0)), Ty::U);
    else write(ins, select(f4_, cond, fconst4(1.0f), fconst4(0.0f)), Ty::F);
  };
  // a scalar function of src0.x (and src1.x), the result in every channel
  auto scalar1 = [&](uint32_t inst) { write_scalar(ins, ext(f_, inst, {scalar(ins, 0, Ty::F)}), Ty::F); };
  auto dot = [&](uint32_t n) {
    const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F);
    const uint32_t sel[4] = {0, 1, 2, 3};
    const uint32_t t = b_.t_vector(f_, n);
    write_scalar(ins, emit(OpDot, f_, {shuffle(t, a, a, sel, n), shuffle(t, c, c, sel, n)}), Ty::F);
  };
  switch (op) {
    case Op::Mov: {
      // (bits as they are; a negated or absolute source is a float's, as TGSI's MOV is a float instruction)
      const Operand& o = ins.src[0];
      if (ins.saturate || o.negate || o.absolute) write(ins, src(ins, 0, Ty::F), Ty::F);
      else write_bits(ins.dst[0], src(ins, 0, Ty::U));
      return ok_;
    }
    case Op::Add: bin(OpFAdd, Ty::F); return ok_;
    case Op::Sub: bin(OpFSub, Ty::F); return ok_;
    case Op::Mul: bin(OpFMul, Ty::F); return ok_;
    case Op::Div: bin(OpFDiv, Ty::F); return ok_;
    case Op::Mad: {
      const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F), e = src(ins, 2, Ty::F);
      write(ins, emit(OpFAdd, f4_, {emit(OpFMul, f4_, {a, c}), e}), Ty::F);
      return ok_;
    }
    case Op::Fma: write(ins, ext(f4_, GlslFma, {src(ins, 0, Ty::F), src(ins, 1, Ty::F), src(ins, 2, Ty::F)}), Ty::F); return ok_;
    case Op::Lrp: {   // a * b + (1 - a) * c
      const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F), e = src(ins, 2, Ty::F);
      const uint32_t one_minus = emit(OpFSub, f4_, {fconst4(1.0f), a});
      write(ins, emit(OpFAdd, f4_, {emit(OpFMul, f4_, {a, c}), emit(OpFMul, f4_, {one_minus, e})}), Ty::F);
      return ok_;
    }
    case Op::Min: glsl2(GlslNMin, Ty::F); return ok_;
    case Op::Max: glsl2(GlslNMax, Ty::F); return ok_;
    case Op::Abs: glsl1(GlslFAbs, Ty::F); return ok_;
    case Op::Ssg: glsl1(GlslFSign, Ty::F); return ok_;
    case Op::Frc: glsl1(GlslFract, Ty::F); return ok_;
    case Op::Flr: glsl1(GlslFloor, Ty::F); return ok_;
    case Op::Ceil: glsl1(GlslCeil, Ty::F); return ok_;
    case Op::Trunc: glsl1(GlslTrunc, Ty::F); return ok_;
    case Op::Round: glsl1(GlslRoundEven, Ty::F); return ok_;
    case Op::Dp2: dot(2); return ok_;
    case Op::Dp3: dot(3); return ok_;
    case Op::Dp4: dot(4); return ok_;
    case Op::Dph: {   // dot(a.xyz, b.xyz) + b.w
      const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F);
      const uint32_t sel[4] = {0, 1, 2, 3};
      const uint32_t d3 = emit(OpDot, f_, {shuffle(f3_, a, a, sel, 3), shuffle(f3_, c, c, sel, 3)});
      write_scalar(ins, emit(OpFAdd, f_, {d3, extract(f_, c, 3)}), Ty::F);
      return ok_;
    }
    case Op::Dst: {   // (1, a.y * b.y, a.z, b.w)
      const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F);
      const uint32_t y = emit(OpFMul, f_, {extract(f_, a, 1), extract(f_, c, 1)});
      write(ins, emit(OpCompositeConstruct, f4_, {b_.c_float(1.0f), y, extract(f_, a, 2), extract(f_, c, 3)}), Ty::F);
      return ok_;
    }
    case Op::Lit: {   // (1, max(x, 0), x > 0 ? max(y, 0) ^ clamp(w, -128, 128) : 0, 1)
      const uint32_t a = src(ins, 0, Ty::F);
      const uint32_t x = extract(f_, a, 0), y = extract(f_, a, 1), w = extract(f_, a, 3);
      const uint32_t mx = ext(f_, GlslNMax, {x, b_.c_float(0.0f)});
      const uint32_t my = ext(f_, GlslNMax, {y, b_.c_float(0.0f)});
      const uint32_t cw = ext(f_, GlslFClamp, {w, b_.c_float(-128.0f), b_.c_float(128.0f)});
      const uint32_t pw = ext(f_, GlslPow, {my, cw});
      const uint32_t z = select(f_, emit(OpFOrdGreaterThan, bool_, {x, b_.c_float(0.0f)}), pw, b_.c_float(0.0f));
      write(ins, emit(OpCompositeConstruct, f4_, {b_.c_float(1.0f), mx, z, b_.c_float(1.0f)}), Ty::F);
      return ok_;
    }
    case Op::Rcp: write_scalar(ins, emit(OpFDiv, f_, {b_.c_float(1.0f), scalar(ins, 0, Ty::F)}), Ty::F); return ok_;
    case Op::Rsq: write_scalar(ins, ext(f_, GlslInverseSqrt, {ext(f_, GlslFAbs, {scalar(ins, 0, Ty::F)})}), Ty::F); return ok_;
    case Op::Sqrt: scalar1(GlslSqrt); return ok_;
    case Op::Ex2: scalar1(GlslExp2); return ok_;
    case Op::Lg2: scalar1(GlslLog2); return ok_;
    case Op::Sin: scalar1(GlslSin); return ok_;
    case Op::Cos: scalar1(GlslCos); return ok_;
    case Op::Pow: write_scalar(ins, ext(f_, GlslPow, {scalar(ins, 0, Ty::F), scalar(ins, 1, Ty::F)}), Ty::F); return ok_;
    case Op::Exp: {   // (2^floor(x), x - floor(x), 2^x, 1)
      const uint32_t x = scalar(ins, 0, Ty::F);
      const uint32_t fl = ext(f_, GlslFloor, {x});
      write(ins, emit(OpCompositeConstruct, f4_, {ext(f_, GlslExp2, {fl}), emit(OpFSub, f_, {x, fl}), ext(f_, GlslExp2, {x}), b_.c_float(1.0f)}), Ty::F);
      return ok_;
    }
    case Op::Log: {   // (floor(log2|x|), |x| / 2^floor(log2|x|), log2|x|, 1)
      const uint32_t ax = ext(f_, GlslFAbs, {scalar(ins, 0, Ty::F)});
      const uint32_t l = ext(f_, GlslLog2, {ax});
      const uint32_t fl = ext(f_, GlslFloor, {l});
      write(ins, emit(OpCompositeConstruct, f4_, {fl, emit(OpFDiv, f_, {ax, ext(f_, GlslExp2, {fl})}), l, b_.c_float(1.0f)}), Ty::F);
      return ok_;
    }
    case Op::Xpd: {   // (cross(a.xyz, b.xyz), 1)
      const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F);
      const uint32_t sel[4] = {0, 1, 2, 3};
      const uint32_t x = ext(f3_, GlslCross, {shuffle(f3_, a, a, sel, 3), shuffle(f3_, c, c, sel, 3)});
      write(ins, emit(OpCompositeConstruct, f4_, {extract(f_, x, 0), extract(f_, x, 1), extract(f_, x, 2), b_.c_float(1.0f)}), Ty::F);
      return ok_;
    }
    case Op::Cmp: {   // a < 0 ? b : c
      const uint32_t a = src(ins, 0, Ty::F), c = src(ins, 1, Ty::F), e = src(ins, 2, Ty::F);
      write(ins, select(f4_, emit(OpFOrdLessThan, b4_, {a, fconst4(0.0f)}), c, e), Ty::F);
      return ok_;
    }
    case Op::Ucmp: {   // a != 0 ? b : c (bits)
      const uint32_t a = src(ins, 0, Ty::U), c = src(ins, 1, Ty::U), e = src(ins, 2, Ty::U);
      write(ins, select(u4_, emit(OpINotEqual, b4_, {a, uconst4(0)}), c, e), Ty::U);
      return ok_;
    }
    case Op::Slt: compare(OpFOrdLessThan, Ty::F, false); return ok_;
    case Op::Sge: compare(OpFOrdGreaterThanEqual, Ty::F, false); return ok_;
    case Op::Seq: compare(OpFOrdEqual, Ty::F, false); return ok_;
    case Op::Sne: compare(OpFUnordNotEqual, Ty::F, false); return ok_;
    case Op::Sgt: compare(OpFOrdGreaterThan, Ty::F, false); return ok_;
    case Op::Sle: compare(OpFOrdLessThanEqual, Ty::F, false); return ok_;
    case Op::Fslt: compare(OpFOrdLessThan, Ty::F, true); return ok_;
    case Op::Fsge: compare(OpFOrdGreaterThanEqual, Ty::F, true); return ok_;
    case Op::Fseq: compare(OpFOrdEqual, Ty::F, true); return ok_;
    case Op::Fsne: compare(OpFUnordNotEqual, Ty::F, true); return ok_;
    case Op::Useq: compare(OpIEqual, Ty::U, true); return ok_;
    case Op::Usne: compare(OpINotEqual, Ty::U, true); return ok_;
    case Op::Uslt: compare(OpULessThan, Ty::U, true); return ok_;
    case Op::Usge: compare(OpUGreaterThanEqual, Ty::U, true); return ok_;
    case Op::Islt: compare(OpSLessThan, Ty::I, true); return ok_;
    case Op::Isge: compare(OpSGreaterThanEqual, Ty::I, true); return ok_;
    case Op::Ddx:
    case Op::Ddy:
    case Op::DdxFine:
    case Op::DdyFine: {
      if (p_.stage != Stage::Fragment) return fail("a derivative outside the fragment stage");
      if (op == Op::DdxFine || op == Op::DdyFine) b_.capability(CapDerivativeControl);
      const uint16_t spv = op == Op::Ddx ? OpDPdx : op == Op::Ddy ? OpDPdy : op == Op::DdxFine ? OpDPdxFine : OpDPdyFine;
      uint32_t r = emit(spv, f4_, {src(ins, 0, Ty::F)});
      // (TGSI's DDY runs down the target's rows as the producer's image is kept: down for the Direct3D layer's images, up for the GL translator's)
      if ((op == Op::Ddy || op == Op::DdyFine) && o_.ddy_up) r = emit(OpFNegate, f4_, {r});
      write(ins, r, Ty::F);
      return ok_;
    }
    // integers
    case Op::Uadd: bin(OpIAdd, Ty::U); return ok_;
    case Op::Ineg: write(ins, emit(OpSNegate, i4_, {src(ins, 0, Ty::I)}), Ty::I); return ok_;
    case Op::Iabs: glsl1(GlslSAbs, Ty::I); return ok_;
    case Op::Isgn: glsl1(GlslSSign, Ty::I); return ok_;
    case Op::Umul: bin(OpIMul, Ty::U); return ok_;
    case Op::UmulHi:
    case Op::ImulHi: {
      const bool s = op == Op::ImulHi;
      const Ty t = s ? Ty::I : Ty::U;
      const uint32_t vt = vec_of(t);
      const uint32_t pair = b_.id();
      const uint32_t st = b_.id();
      b_.op(Builder::kGlobals, OpTypeStruct, {st, vt, vt});
      const uint32_t a = src(ins, 0, t), c = src(ins, 1, t);
      b_.op(Builder::kCode, s ? OpSMulExtended : OpUMulExtended, {st, pair, a, c});
      write(ins, extract(vt, pair, 1), t);
      return ok_;
    }
    case Op::Umad: {
      const uint32_t a = src(ins, 0, Ty::U), c = src(ins, 1, Ty::U), e = src(ins, 2, Ty::U);
      write(ins, emit(OpIAdd, u4_, {emit(OpIMul, u4_, {a, c}), e}), Ty::U);
      return ok_;
    }
    case Op::Imin: glsl2(GlslSMin, Ty::I); return ok_;
    case Op::Imax: glsl2(GlslSMax, Ty::I); return ok_;
    case Op::Umin: glsl2(GlslUMin, Ty::U); return ok_;
    case Op::Umax: glsl2(GlslUMax, Ty::U); return ok_;
    case Op::Udiv:
    case Op::Umod: {   // (by zero: all ones, as Direct3D and TGSI say)
      const uint32_t a = src(ins, 0, Ty::U), c = src(ins, 1, Ty::U);
      const uint32_t zero = emit(OpIEqual, b4_, {c, uconst4(0)});
      const uint32_t safe = select(u4_, zero, uconst4(1), c);
      const uint32_t q = emit(op == Op::Udiv ? OpUDiv : OpUMod, u4_, {a, safe});
      write(ins, select(u4_, zero, uconst4(~0u), q), Ty::U);
      return ok_;
    }
    case Op::Idiv:
    case Op::Mod: {
      const uint32_t a = src(ins, 0, Ty::I), c = src(ins, 1, Ty::I);
      const uint32_t zero = emit(OpIEqual, b4_, {c, b_.c_null(i4_)});
      const uint32_t one = b_.c_composite(i4_, {b_.c_int(1), b_.c_int(1), b_.c_int(1), b_.c_int(1)});
      const uint32_t safe = select(i4_, zero, one, c);
      const uint32_t q = emit(op == Op::Idiv ? OpSDiv : OpSRem, i4_, {a, safe});
      write(ins, select(u4_, zero, uconst4(~0u), bitcast(u4_, q)), Ty::U);
      return ok_;
    }
    case Op::And: bin(OpBitwiseAnd, Ty::U); return ok_;
    case Op::Or: bin(OpBitwiseOr, Ty::U); return ok_;
    case Op::Xor: bin(OpBitwiseXor, Ty::U); return ok_;
    case Op::Not: write(ins, emit(OpNot, u4_, {src(ins, 0, Ty::U)}), Ty::U); return ok_;
    case Op::Shl:
    case Op::Ishr:
    case Op::Ushr: {   // (by the count's low five bits)
      const uint32_t count = emit(OpBitwiseAnd, u4_, {src(ins, 1, Ty::U), uconst4(31)});
      if (op == Op::Ishr) write(ins, emit(OpShiftRightArithmetic, i4_, {src(ins, 0, Ty::I), count}), Ty::I);
      else write(ins, emit(op == Op::Shl ? OpShiftLeftLogical : OpShiftRightLogical, u4_, {src(ins, 0, Ty::U), count}), Ty::U);
      return ok_;
    }
    case Op::F2i: write(ins, f2i(src(ins, 0, Ty::F), false), Ty::U); return ok_;
    case Op::F2u: write(ins, f2i(src(ins, 0, Ty::F), true), Ty::U); return ok_;
    case Op::I2f: write(ins, emit(OpConvertSToF, f4_, {src(ins, 0, Ty::I)}), Ty::F); return ok_;
    case Op::U2f: write(ins, emit(OpConvertUToF, f4_, {src(ins, 0, Ty::U)}), Ty::F); return ok_;
    // the address register
    case Op::Arl:
    case Op::Arr:
    case Op::Uarl: {
      if (ins.dst[0].file != File::Addr) return fail("ARL writes an address register");
      uint32_t v;
      if (op == Op::Uarl) v = src(ins, 0, Ty::I);
      else v = emit(OpConvertFToS, i4_, {ext(f4_, op == Op::Arl ? GlslFloor : GlslRoundEven, {src(ins, 0, Ty::F)})});
      write_bits(ins.dst[0], bitcast(u4_, v));
      return ok_;
    }
    default: return fail("an arithmetic opcode this compiler does not take");
  }
}

// Doubles: a register holds two, low word first (x y, z w). An operation works on the pairs its destination's mask names; a 32-bit result of pair k goes to
// the k-th channel the mask names (comparisons and conversions to 32 bits), a 32-bit source of pair k is source channel k (conversions to doubles).
bool Compiler::doubles(const Instruction& ins) {
  const uint32_t d = b_.t_double();
  const uint32_t d2 = b_.t_vector(u_, 2);
  const Op op = ins.op;
  const uint8_t mask = ins.dst[0].mask;
  auto pair = [&](uint32_t n, uint32_t k) {   // source n's double k
    const Operand& o = ins.src[n];
    const uint32_t v = swizzled(o);
    const uint32_t sel[2] = {2 * k, 2 * k + 1};
    uint32_t x = bitcast(d, shuffle(d2, v, v, sel, 2));
    if (o.absolute) x = ext(d, GlslFAbs, {x});
    if (o.negate) x = emit(OpFNegate, d, {x});
    return x;
  };
  const bool to_double = op == Op::F2d || op == Op::I2d || op == Op::U2d;
  const bool to_32 = op == Op::Dseq || op == Op::Dsge || op == Op::Dslt || op == Op::Dsne || op == Op::D2f || op == Op::D2i || op == Op::D2u;
  if (ins.src_count < op_info(op).srcs) return fail("a double instruction without all its sources");
  uint32_t out_bits = zero4();
  uint8_t out_mask = 0;
  uint32_t parts[4] = {b_.c_uint(0), b_.c_uint(0), b_.c_uint(0), b_.c_uint(0)};
  if (to_32) {
    uint32_t k = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      if (!(mask >> c & 1)) continue;
      uint32_t r = 0;
      const uint32_t a = pair(0, k);
      switch (op) {
        case Op::Dseq: r = select(u_, emit(OpFOrdEqual, bool_, {a, pair(1, k)}), b_.c_uint(~0u), b_.c_uint(0)); break;
        case Op::Dsne: r = select(u_, emit(OpFUnordNotEqual, bool_, {a, pair(1, k)}), b_.c_uint(~0u), b_.c_uint(0)); break;
        case Op::Dslt: r = select(u_, emit(OpFOrdLessThan, bool_, {a, pair(1, k)}), b_.c_uint(~0u), b_.c_uint(0)); break;
        case Op::Dsge: r = select(u_, emit(OpFOrdGreaterThanEqual, bool_, {a, pair(1, k)}), b_.c_uint(~0u), b_.c_uint(0)); break;
        case Op::D2f: r = bitcast(u_, emit(OpFConvert, f_, {a})); break;
        case Op::D2i: r = bitcast(u_, emit(OpConvertFToS, i_, {a})); break;
        default: r = emit(OpConvertFToU, u_, {a}); break;
      }
      parts[c] = r;
      out_mask |= static_cast<uint8_t>(1u << c);
      ++k;
      if (k == 2) break;
    }
    out_bits = emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]});
  } else {
    for (uint32_t k = 0; k < 2; ++k) {
      if (!(mask >> (2 * k) & 3)) continue;
      uint32_t r = 0;
      if (to_double) {
        const uint32_t v = swizzled(ins.src[0]);
        const uint32_t s = extract(u_, v, k);
        r = op == Op::F2d ? emit(OpFConvert, d, {bitcast(f_, s)}) : op == Op::I2d ? emit(OpConvertSToF, d, {bitcast(i_, s)}) : emit(OpConvertUToF, d, {s});
      } else {
        const uint32_t a = pair(0, k);
        switch (op) {
          case Op::Dadd: r = emit(OpFAdd, d, {a, pair(1, k)}); break;
          case Op::Dmul: r = emit(OpFMul, d, {a, pair(1, k)}); break;
          case Op::Ddiv: r = emit(OpFDiv, d, {a, pair(1, k)}); break;
          case Op::Dfma: r = ext(d, GlslFma, {a, pair(1, k), pair(2, k)}); break;
          case Op::Dmad: r = emit(OpFAdd, d, {emit(OpFMul, d, {a, pair(1, k)}), pair(2, k)}); break;
          case Op::Drcp: r = emit(OpFDiv, d, {b_.c_double(1.0), a}); break;
          case Op::Dsqrt: r = ext(d, GlslSqrt, {a}); break;
          case Op::Drsq: r = ext(d, GlslInverseSqrt, {a}); break;
          case Op::Dmin: r = ext(d, GlslNMin, {a, pair(1, k)}); break;
          case Op::Dmax: r = ext(d, GlslNMax, {a, pair(1, k)}); break;
          case Op::Dmov: r = a; break;
          case Op::Dneg: r = emit(OpFNegate, d, {a}); break;
          case Op::Dabs: r = ext(d, GlslFAbs, {a}); break;
          default: return fail("a double opcode this compiler does not take");
        }
      }
      const uint32_t w = bitcast(d2, r);
      parts[2 * k] = extract(u_, w, 0);
      parts[2 * k + 1] = extract(u_, w, 1);
      out_mask |= static_cast<uint8_t>(3u << (2 * k));
    }
    out_bits = emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]});
  }
  Operand dst = ins.dst[0];
  dst.mask = out_mask & mask ? out_mask : 0;
  if (!dst.mask) return ok_;
  write_bits(dst, out_bits);
  return ok_;
}

// ---- textures ---------------------------------------------------------------------------------------------------------------------------------------
uint32_t Compiler::texture_image(uint32_t unit) {
  if (!sview_var_[unit]) return fail("a texture unit without its view"), 0;
  return load(sview_type_[unit], sview_var_[unit]);
}

uint32_t Compiler::sampled_image(uint32_t unit, Target target) {
  if (!sview_var_[unit]) {
    // (a SAMP without an SVIEW, as the built-in shaders write: the view is the instruction's target)
    sview_target_[unit] = target;
    sview_type_[unit] = texture_image_type(unit, target);
    if (!sview_type_[unit]) return fail("a texture target this compiler does not take"), 0;
    sview_var_[unit] = b_.global(UniformConstant, sview_type_[unit]);
    b_.decorate(sview_var_[unit], DecDescriptorSet, {0});
    b_.decorate(sview_var_[unit], DecBinding, {unit < kMaxUnits ? kBindingTexture + unit : binding_texel(o_.stage_slot, unit - kMaxUnits)});
    r_.sview_mask |= 1u << unit;
  }
  if (!samp_var_[unit]) return fail("a texture lookup without its sampler"), 0;
  const uint32_t image = load(sview_type_[unit], sview_var_[unit]);
  const uint32_t sampler = load(b_.t_sampler(), samp_var_[unit]);
  return emit(OpSampledImage, b_.t_sampled_image(sview_type_[unit]), {image, sampler});
}

bool Compiler::texture(const Instruction& ins) {
  const Op op = ins.op;
  // the unit: the SAMP operand
  int32_t samp = -1;
  for (uint32_t i = 0; i < ins.src_count; ++i)
    if (ins.src[i].file == File::Samp) samp = static_cast<int32_t>(i);
  if (samp < 0) return fail("a texture instruction without SAMP");
  const int32_t u = ins.src[samp].index.value;
  if (u < 0 || u >= static_cast<int32_t>(kMaxSviews) || ins.src[samp].index.indirect) return fail("a sampler unit out of range");
  const uint32_t unit = static_cast<uint32_t>(u);
  const Target target = ins.target != Target::None ? ins.target : sview_target_[unit];
  TargetShape s;
  if (!shape_of(target, &s)) return fail("a texture instruction without a target");
  if (s.shadow) return fail("shadow targets (the producers compare in the shader)");
  if (sview_var_[unit] == 0 && op != Op::Txf && op != Op::Txq && op != Op::Txqs) {
    if (!sampled_image(unit, target)) return false;   // (declares the view)
  } else if (sview_var_[unit] == 0) {
    sview_target_[unit] = target;
    sview_type_[unit] = texture_image_type(unit, target);
    sview_var_[unit] = b_.global(UniformConstant, sview_type_[unit]);
    b_.decorate(sview_var_[unit], DecDescriptorSet, {0});
    b_.decorate(sview_var_[unit], DecBinding, {unit < kMaxUnits ? kBindingTexture + unit : binding_texel(o_.stage_slot, unit - kMaxUnits)});
    r_.sview_mask |= 1u << unit;
  }
  const ValueType vt = o_.sview_type[unit];
  const uint32_t texel = vt == ValueType::Float ? f4_ : vt == ValueType::Sint ? i4_ : u4_;
  const uint32_t n = s.coords + (s.arrayed ? 1 : 0);      // coordinate components (with the layer)
  const uint32_t ftype = b_.t_vector(f_, n), itype = b_.t_vector(i_, n);
  const uint32_t sel[4] = {0, 1, 2, 3};
  auto coords_f = [&](uint32_t v) { return n == 4 ? v : shuffle(ftype, v, v, sel, n); };
  auto coords_i = [&](uint32_t v) { return n == 4 ? v : shuffle(itype, v, v, sel, n); };
  const bool fragment = p_.stage == Stage::Fragment;
  // a constant offset (an immediate operand after the target), as many components as the texel address has
  uint32_t offset = 0;
  bool const_offset = false;
  if (ins.offsets) {
    const Operand& off = ins.src[ins.src_count - 1];
    const uint32_t on = s.dim == Dim3D ? 3 : s.dim == Dim1D ? 1 : 2;
    if (s.dim == DimCube) return fail("texel offsets on a cube");
    if (off.file == File::Imm && !off.index.indirect && off.index.value >= 0 && static_cast<uint64_t>(off.index.value) < p_.immediates.count()) {
      const Immediate& imm = p_.immediates[static_cast<uint64_t>(off.index.value)];
      uint32_t c[3];
      for (uint32_t k = 0; k < on; ++k) c[k] = b_.c_int(static_cast<int32_t>(imm.bits[off.swizzle[k]]));
      offset = on == 1 ? c[0] : on == 2 ? b_.c_composite(b_.t_vector(i_, 2), {c[0], c[1]}) : b_.c_composite(b_.t_vector(i_, 3), {c[0], c[1], c[2]});
      const_offset = true;
    } else {
      b_.capability(CapImageGatherExtended);
      const uint32_t v = swizzled(off);
      const uint32_t ot = b_.t_vector(i_, on);
      offset = on == 1 ? bitcast(i_, extract(u_, v, 0)) : bitcast(ot, shuffle(b_.t_vector(u_, on), v, v, sel, on));
    }
  }
  uint32_t result = 0;
  auto with_offset = [&](uint32_t mask, uint32_t* words, uint32_t& count) {
    if (!offset) return mask;
    words[count++] = offset;
    return mask | (const_offset ? ImConstOffset : ImOffset);
  };
  switch (op) {
    case Op::Tex:
    case Op::Txp:
    case Op::Txb:
    case Op::Txb2:
    case Op::Txl:
    case Op::Txl2:
    case Op::Txd: {
      if (s.ms || s.dim == DimBuffer) return fail("a sampled lookup of a multisampled or buffer view");
      const uint32_t si = sampled_image(unit, target);
      uint32_t c = src(ins, 0, Ty::F);
      if (op == Op::Txp) {   // (projective: the coordinates over w)
        const uint32_t w = extract(f_, c, 3);
        c = emit(OpFDiv, f4_, {c, splat(f4_, w)});
      }
      const uint32_t coords = coords_f(c);
      uint32_t words[12] = {texel, 0, si, coords};
      uint32_t count = 4;
      const bool implicit = fragment && (op == Op::Tex || op == Op::Txp || op == Op::Txb || op == Op::Txb2);
      uint32_t mask = 0;
      uint32_t operands[6];
      uint32_t on = 0;
      if (op == Op::Txb || op == Op::Txb2) {
        if (!fragment) return fail("a biased lookup outside the fragment stage");
        mask |= ImBias;
        operands[on++] = op == Op::Txb2 ? scalar(ins, 1, Ty::F) : extract(f_, src(ins, 0, Ty::F), 3);
      } else if (!implicit && op != Op::Txd) {
        mask |= ImLod;
        operands[on++] = op == Op::Txl ? extract(f_, src(ins, 0, Ty::F), 3) : op == Op::Txl2 ? scalar(ins, 1, Ty::F) : b_.c_float(0.0f);
      }
      if (op == Op::Txd) {
        const uint32_t gn = s.coords;
        const uint32_t gt = b_.t_vector(f_, gn);
        const uint32_t dx = src(ins, 1, Ty::F), dy = src(ins, 2, Ty::F);
        mask |= ImGrad;
        operands[on++] = gn == 4 ? dx : gn == 1 ? extract(f_, dx, 0) : shuffle(gt, dx, dx, sel, gn);
        operands[on++] = gn == 4 ? dy : gn == 1 ? extract(f_, dy, 0) : shuffle(gt, dy, dy, sel, gn);
      }
      mask = with_offset(mask, operands, on);
      if (mask) {
        words[count++] = mask;
        for (uint32_t i = 0; i < on; ++i) words[count++] = operands[i];
      }
      result = b_.id();
      words[1] = result;
      b_.op_list(Builder::kCode, implicit ? OpImageSampleImplicitLod : OpImageSampleExplicitLod, words, count);
      break;
    }
    case Op::Txf: {
      const uint32_t image = texture_image(unit);
      const uint32_t c = src(ins, 0, Ty::I);
      const uint32_t coords = s.dim == DimBuffer ? extract(i_, c, 0) : n == 1 ? extract(i_, c, 0) : coords_i(c);
      uint32_t words[10] = {texel, 0, image, coords};
      uint32_t count = 4;
      uint32_t mask = 0;
      uint32_t operands[4];
      uint32_t on = 0;
      if (s.ms) {
        mask |= ImSample;
        operands[on++] = extract(i_, c, 3);
      } else if (s.dim != DimBuffer) {
        mask |= ImLod;
        operands[on++] = extract(i_, c, 3);
      }
      mask = with_offset(mask, operands, on);
      if (mask & ImOffset) return fail("a texel fetch with a register offset");
      if (mask) {
        words[count++] = mask;
        for (uint32_t i = 0; i < on; ++i) words[count++] = operands[i];
      }
      result = b_.id();
      words[1] = result;
      b_.op_list(Builder::kCode, OpImageFetch, words, count);
      break;
    }
    case Op::Txq: {
      b_.capability(CapImageQuery);
      const uint32_t image = texture_image(unit);
      uint32_t size_n = s.coords + (s.arrayed ? 1 : 0);
      if (s.dim == DimCube) size_n = s.arrayed ? 3 : 2;
      const uint32_t st = b_.t_vector(i_, size_n);
      uint32_t size;
      uint32_t levels = b_.c_int(1);
      if (s.ms || s.dim == DimBuffer) {
        size = emit(OpImageQuerySize, st, {image});
      } else {
        size = emit(OpImageQuerySizeLod, st, {image, scalar(ins, 0, Ty::I)});
        levels = emit(OpImageQueryLevels, i_, {image});
      }
      uint32_t parts[4] = {b_.c_int(0), b_.c_int(0), b_.c_int(0), levels};
      for (uint32_t k = 0; k < size_n && k < 3; ++k) parts[k] = size_n == 1 ? size : extract(i_, size, k);
      write(ins, emit(OpCompositeConstruct, i4_, {parts[0], parts[1], parts[2], parts[3]}), Ty::I);
      return ok_;
    }
    case Op::Tg4: {
      if (s.ms || s.dim == DimBuffer || s.dim == Dim3D || s.dim == Dim1D) return fail("a gather from a view that has no gather");
      const uint32_t si = sampled_image(unit, target);
      const uint32_t coords = coords_f(src(ins, 0, Ty::F));
      // the component: src1.x, an immediate
      const Operand& co = ins.src[1];
      if (co.file != File::Imm || co.index.indirect || co.index.value < 0 || static_cast<uint64_t>(co.index.value) >= p_.immediates.count())
        return fail("a gather component that is not an immediate");
      uint32_t comp = p_.immediates[static_cast<uint64_t>(co.index.value)].bits[co.swizzle[0]];
      if (comp > 3) {   // (a float immediate: 0.0 .. 3.0)
        float f;
        __builtin_memcpy(&f, &comp, 4);
        comp = f >= 0 && f <= 3 ? static_cast<uint32_t>(f) : 0;
      }
      uint32_t words[10] = {texel, 0, si, coords, b_.c_int(static_cast<int32_t>(comp))};
      uint32_t count = 5;
      uint32_t operands[2];
      uint32_t on = 0;
      const uint32_t mask = with_offset(0, operands, on);
      if (mask) {
        words[count++] = mask;
        for (uint32_t i = 0; i < on; ++i) words[count++] = operands[i];
      }
      result = b_.id();
      words[1] = result;
      b_.op_list(Builder::kCode, OpImageGather, words, count);
      break;
    }
    case Op::Txqs: {   // (the samples per texel of a multisampled view: OpImageQuerySamples)
      if (!s.ms) return fail("TXQS of a view that is not multisampled");
      b_.capability(CapImageQuery);
      const uint32_t samples = emit(OpImageQuerySamples, i_, {texture_image(unit)});
      write(ins, splat(i4_, samples), Ty::I);
      return ok_;
    }
    case Op::Lodq: {
      if (!fragment) return fail("LODQ outside the fragment stage");
      b_.capability(CapImageQuery);
      const uint32_t si = sampled_image(unit, target);
      uint32_t c = src(ins, 0, Ty::F);
      const uint32_t qn = s.dim == DimCube ? 3 : s.coords;
      const uint32_t coords = qn == 4 ? c : qn == 1 ? extract(f_, c, 0) : shuffle(b_.t_vector(f_, qn), c, c, sel, qn);
      const uint32_t lod = emit(OpImageQueryLod, f2_, {si, coords});
      write(ins, emit(OpCompositeConstruct, f4_, {extract(f_, lod, 0), extract(f_, lod, 1), b_.c_float(0.0f), b_.c_float(0.0f)}), Ty::F);
      return ok_;
    }
    default: return fail("a texture opcode this compiler does not take");
  }
  write_bits(ins.dst[0], vt == ValueType::Uint ? result : bitcast(u4_, result));
  return ok_;
}

// ---- storage ----------------------------------------------------------------------------------------------------------------------------------------
uint32_t Compiler::buffer_word_pointer(const Operand& res, uint32_t word_index) {
  if (res.file == File::Memory) return chain(Workgroup, u_, shared_var_, {word_index});
  const int32_t k = res.index.value;
  if (k < 0 || k >= static_cast<int32_t>(kMaxStorage) || !storage_var_[k]) return fail("an undeclared BUFFER"), 0;
  return chain(StorageBuffer, u_, storage_var_[k], {b_.c_int(0), word_index});
}

uint32_t Compiler::image_texel_type(uint32_t k) { return image_type_[k] == f_ ? f4_ : image_type_[k] == i_ ? i4_ : u4_; }

uint32_t Compiler::image_coordinate(Target t, uint32_t bits) {
  TargetShape s;
  if (!shape_of(t, &s)) return fail("an image target this compiler does not take"), 0;
  uint32_t n = s.coords + (s.arrayed ? 1 : 0);
  if (s.dim == DimCube) n = 3;
  const uint32_t ints = bitcast(i4_, bits);
  if (n == 1) return extract(i_, ints, 0);
  const uint32_t sel[4] = {0, 1, 2, 3};
  return shuffle(b_.t_vector(i_, n), ints, ints, sel, n);
}

bool Compiler::memory(const Instruction& ins) {
  const Op op = ins.op;
  const Operand& res = op == Op::Store ? ins.dst[0] : ins.src[0];
  const bool image = res.file == File::Image;
  if (!image && res.file != File::Buffer && res.file != File::Memory) return fail("a memory instruction on a file that is not memory");
  if (res.file == File::Memory && !shared_var_) return fail("MEMORY without its declaration");
  const uint32_t scope = res.file == File::Memory ? ScopeWorkgroup : ScopeDevice;
  if (image) {
    const int32_t k = res.index.value;
    if (k < 0 || k >= static_cast<int32_t>(kMaxImages) || !image_var_[k]) return fail("an undeclared IMAGE");
    const Target t = ins.target != Target::None ? ins.target : image_target_[k];
    const uint32_t texel = image_texel_type(static_cast<uint32_t>(k));
    const uint32_t declared_type = image_decl_type_[k];
    switch (op) {
      case Op::Load: {
        const uint32_t coords = image_coordinate(t, src(ins, 1, Ty::U));
        const uint32_t v = emit(OpImageRead, texel, {load(declared_type, image_var_[k]), coords});
        write_bits(ins.dst[0], texel == u4_ ? v : bitcast(u4_, v));
        return ok_;
      }
      case Op::Store: {
        const uint32_t coords = image_coordinate(t, src(ins, 0, Ty::U));
        const uint32_t v = src(ins, 1, Ty::U);
        emit_void(OpImageWrite, {load(declared_type, image_var_[k]), coords, texel == u4_ ? v : bitcast(texel, v)});
        return ok_;
      }
      case Op::Resq: {
        b_.capability(CapImageQuery);
        TargetShape s;
        shape_of(t, &s);
        uint32_t n = s.coords + (s.arrayed ? 1 : 0);
        if (s.dim == DimCube) n = s.arrayed ? 3 : 2;
        const uint32_t size = emit(OpImageQuerySize, b_.t_vector(i_, n), {load(declared_type, image_var_[k])});
        uint32_t parts[4] = {b_.c_int(0), b_.c_int(0), b_.c_int(0), b_.c_int(1)};
        for (uint32_t c = 0; c < n && c < 3; ++c) parts[c] = n == 1 ? size : extract(i_, size, c);
        write(ins, emit(OpCompositeConstruct, i4_, {parts[0], parts[1], parts[2], parts[3]}), Ty::I);
        return ok_;
      }
      default: {
        if (image_type_[k] == f_) return fail("an atomic on a float image");
        const uint32_t elem = image_type_[k];
        const uint32_t coords = image_coordinate(t, src(ins, 1, Ty::U));
        const uint32_t ptr = emit(OpImageTexelPointer, b_.t_pointer(spirv::StorageClass(11), elem), {image_var_[k], coords, b_.c_uint(0)});
        uint32_t value = scalar(ins, 2, Ty::U);
        if (elem != u_) value = bitcast(elem, value);
        uint32_t r;
        if (op == Op::AtomCas) {
          uint32_t v2 = scalar(ins, 3, Ty::U);
          if (elem != u_) v2 = bitcast(elem, v2);
          r = emit(OpAtomicCompareExchange, elem, {ptr, b_.c_uint(ScopeDevice), b_.c_uint(0), b_.c_uint(0), v2, value});
        } else {
          const uint16_t a = op == Op::AtomUadd ? OpAtomicIAdd : op == Op::AtomAnd ? OpAtomicAnd : op == Op::AtomOr ? OpAtomicOr : op == Op::AtomXor ? OpAtomicXor
                           : op == Op::AtomImax ? OpAtomicSMax : op == Op::AtomImin ? OpAtomicSMin : op == Op::AtomUmax ? OpAtomicUMax : op == Op::AtomUmin ? OpAtomicUMin
                           : OpAtomicExchange;
          r = emit(a, elem, {ptr, b_.c_uint(ScopeDevice), b_.c_uint(0), value});
        }
        write_scalar(ins, elem == u_ ? r : bitcast(u_, r), Ty::U);
        return ok_;
      }
    }
  }
  // buffers and shared memory: arrays of 32-bit words at byte addresses
  switch (op) {
    case Op::Load: {
      const uint32_t word = emit(OpShiftRightLogical, u_, {scalar(ins, 1, Ty::U), b_.c_uint(2)});
      uint32_t parts[4] = {b_.c_uint(0), b_.c_uint(0), b_.c_uint(0), b_.c_uint(0)};
      for (uint32_t c = 0; c < 4; ++c)
        if (ins.dst[0].mask >> c & 1) {
          const uint32_t at = c ? emit(OpIAdd, u_, {word, b_.c_uint(c)}) : word;
          parts[c] = load(u_, buffer_word_pointer(res, at));
        }
      write_bits(ins.dst[0], emit(OpCompositeConstruct, u4_, {parts[0], parts[1], parts[2], parts[3]}));
      return ok_;
    }
    case Op::Store: {
      const uint32_t word = emit(OpShiftRightLogical, u_, {scalar(ins, 0, Ty::U), b_.c_uint(2)});
      const uint32_t v = src(ins, 1, Ty::U);
      for (uint32_t c = 0; c < 4; ++c)
        if (res.mask >> c & 1) {
          const uint32_t at = c ? emit(OpIAdd, u_, {word, b_.c_uint(c)}) : word;
          store(buffer_word_pointer(res, at), extract(u_, v, c));
        }
      return ok_;
    }
    case Op::Resq: {
      if (res.file == File::Memory) {
        write_scalar(ins, b_.c_uint(shared_words_ * 4), Ty::U);
        return ok_;
      }
      const int32_t k = res.index.value;
      if (k < 0 || k >= static_cast<int32_t>(kMaxStorage) || !storage_var_[k]) return fail("an undeclared BUFFER");
      const uint32_t words = emit(OpArrayLength, u_, {storage_var_[k], 0});
      write_scalar(ins, emit(OpShiftLeftLogical, u_, {words, b_.c_uint(2)}), Ty::U);
      return ok_;
    }
    default: {
      const uint32_t word = emit(OpShiftRightLogical, u_, {scalar(ins, 1, Ty::U), b_.c_uint(2)});
      const uint32_t ptr = buffer_word_pointer(res, word);
      const uint32_t value = scalar(ins, 2, Ty::U);
      uint32_t r;
      if (op == Op::AtomCas) {
        r = emit(OpAtomicCompareExchange, u_, {ptr, b_.c_uint(scope), b_.c_uint(0), b_.c_uint(0), scalar(ins, 3, Ty::U), value});
      } else {
        const uint16_t a = op == Op::AtomUadd ? OpAtomicIAdd : op == Op::AtomAnd ? OpAtomicAnd : op == Op::AtomOr ? OpAtomicOr : op == Op::AtomXor ? OpAtomicXor
                         : op == Op::AtomImax ? OpAtomicSMax : op == Op::AtomImin ? OpAtomicSMin : op == Op::AtomUmax ? OpAtomicUMax : op == Op::AtomUmin ? OpAtomicUMin
                         : OpAtomicExchange;
        r = emit(a, u_, {ptr, b_.c_uint(scope), b_.c_uint(0), value});
      }
      write_scalar(ins, r, Ty::U);
      return ok_;
    }
  }
}

// ---- control flow and the stage's own instructions ----------------------------------------------------------------------------------------------------
uint32_t Compiler::condition(const Instruction& ins, bool integer) {
  if (integer) return emit(OpINotEqual, bool_, {scalar(ins, 0, Ty::U), b_.c_uint(0)});
  return emit(OpFUnordNotEqual, bool_, {scalar(ins, 0, Ty::F), b_.c_float(0.0f)});   // (taken for NaN too, as TGSI says: x != 0.0)
}

uint32_t Compiler::stream_of(const Instruction& ins) {
  if (ins.src_count == 0) return 0;
  const Operand& o = ins.src[0];
  if (o.file != File::Imm || o.index.indirect || o.index.value < 0 || static_cast<uint64_t>(o.index.value) >= p_.immediates.count()) {
    fail("a stream that is not an immediate");
    return 0;
  }
  const Immediate& imm = p_.immediates[static_cast<uint64_t>(o.index.value)];
  uint32_t s = imm.bits[o.swizzle[0]];
  if (imm.type == ImmType::Flt32 && s != 0) {   // (a float immediate: 0.0 .. 3.0)
    float f;
    __builtin_memcpy(&f, &s, 4);
    s = f >= 0 && f < 4 ? static_cast<uint32_t>(f) : 4;
  }
  if (s > 3) {
    fail("a stream past the fourth");
    return 0;
  }
  if (o_.swap_stream) {
    if (s == 0) s = o_.swap_stream;
    else if (s == o_.swap_stream) s = 0;
  }
  return s;
}

void Compiler::finish_outputs() {
  const bool last = p_.stage == Stage::Vertex || p_.stage == Stage::TessEval;
  if (last && o_.write_point_size && out_block_ && !out_has_kind(IoKind::PointSize)) store(chain(Output, f_, out_block_, {b_.c_int(1)}), b_.c_float(1.0f));
}

void Compiler::emit_vertex(uint32_t stream) {
  if (o_.write_point_size && out_block_ && !out_has_kind(IoKind::PointSize)) store(chain(Output, f_, out_block_, {b_.c_int(1)}), b_.c_float(1.0f));
  if (multi_stream_) emit_void(OpEmitStreamVertex, {b_.c_uint(stream)});
  else emit_void(OpEmitVertex, {});
}

bool Compiler::control(const Instruction& ins) {
  switch (ins.op) {
    case Op::If:
    case Op::Uif: {
      const uint32_t cond = condition(ins, ins.op == Op::Uif);
      Construct c;
      c.merge = label();
      const uint32_t then_label = label();
      c.else_label = label();
      emit_void(OpSelectionMerge, {c.merge, 0});
      emit_void(OpBranchConditional, {cond, then_label, c.else_label});
      terminated_ = true;
      place(then_label);
      if (!constructs_.push_back(c)) return fail("out of memory");
      return ok_;
    }
    case Op::Else: {
      if (constructs_.count() == 0 || constructs_[constructs_.count() - 1].loop || constructs_[constructs_.count() - 1].has_else) return fail("ELSE without its IF");
      Construct& c = constructs_[constructs_.count() - 1];
      branch(c.merge);
      place(c.else_label);
      c.has_else = true;
      return ok_;
    }
    case Op::EndIf: {
      if (constructs_.count() == 0 || constructs_[constructs_.count() - 1].loop) return fail("ENDIF without its IF");
      const Construct c = constructs_[constructs_.count() - 1];
      (void)constructs_.remove_at(constructs_.count() - 1);
      branch(c.merge);
      if (!c.has_else) {
        place(c.else_label);
        branch(c.merge);
      }
      place(c.merge);
      return ok_;
    }
    case Op::BgnLoop: {
      Construct c;
      c.loop = true;
      c.header = label();
      c.merge = label();
      c.cont = label();
      const uint32_t body = label();
      branch(c.header);
      place(c.header);
      emit_void(OpLoopMerge, {c.merge, c.cont, 0});
      emit_void(OpBranch, {body});
      terminated_ = true;
      place(body);
      if (!constructs_.push_back(c)) return fail("out of memory");
      return ok_;
    }
    case Op::EndLoop: {
      if (constructs_.count() == 0 || !constructs_[constructs_.count() - 1].loop) return fail("ENDLOOP without its BGNLOOP");
      const Construct c = constructs_[constructs_.count() - 1];
      (void)constructs_.remove_at(constructs_.count() - 1);
      branch(c.cont);
      place(c.cont);
      branch(c.header);
      place(c.merge);
      return ok_;
    }
    case Op::Brk:
    case Op::Cont: {
      for (uint64_t i = constructs_.count(); i > 0; --i) {
        const Construct& c = constructs_[i - 1];
        if (!c.loop) continue;
        resume();
        branch(ins.op == Op::Brk ? c.merge : c.cont);
        return ok_;
      }
      return fail("BRK or CONT outside a loop");
    }
    case Op::Ret:
    case Op::End: {
      if (ins.op == Op::End && constructs_.count()) return fail("END inside an IF or a loop");
      resume();
      finish_outputs();
      emit_void(OpReturn, {});
      terminated_ = true;
      return ok_;
    }
    case Op::Kill: {
      if (p_.stage != Stage::Fragment) return fail("KILL outside the fragment stage");
      resume();
      emit_void(OpDemoteToHelperInvocation, {});
      return ok_;
    }
    case Op::KillIf: {
      if (p_.stage != Stage::Fragment) return fail("KILL_IF outside the fragment stage");
      resume();
      const uint32_t any = emit(OpAny, bool_, {emit(OpFOrdLessThan, b4_, {src(ins, 0, Ty::F), fconst4(0.0f)})});
      const uint32_t yes = label(), merge = label();
      emit_void(OpSelectionMerge, {merge, 0});
      emit_void(OpBranchConditional, {any, yes, merge});
      terminated_ = true;
      place(yes);
      emit_void(OpDemoteToHelperInvocation, {});
      branch(merge);
      place(merge);
      return ok_;
    }
    case Op::Emit:
    case Op::EndPrim: {
      if (p_.stage != Stage::Geometry) return fail("EMIT outside the geometry stage");
      resume();
      const uint32_t stream = stream_of(ins);
      if (ins.op == Op::Emit) emit_vertex(stream);
      else if (multi_stream_) emit_void(OpEndStreamPrimitive, {b_.c_uint(stream)});
      else emit_void(OpEndPrimitive, {});
      return ok_;
    }
    case Op::Membar:
      resume();
      emit_void(OpMemoryBarrier, {b_.c_uint(ScopeDevice), b_.c_uint(SemAcquireRelease | SemUniformMemory | SemWorkgroupMemory | SemImageMemory)});
      return ok_;
    case Op::Barrier:
      resume();
      if (p_.stage == Stage::TessControl)
        emit_void(OpControlBarrier, {b_.c_uint(ScopeWorkgroup), b_.c_uint(ScopeInvocation), b_.c_uint(0)});
      else if (p_.stage == Stage::Compute)
        emit_void(OpControlBarrier, {b_.c_uint(ScopeWorkgroup), b_.c_uint(ScopeWorkgroup), b_.c_uint(SemAcquireRelease | SemWorkgroupMemory | SemUniformMemory | SemImageMemory)});
      else
        return fail("BARRIER outside the compute and tessellation control stages");
      return ok_;
    case Op::Nop: return ok_;
    default: return fail("a control opcode this compiler does not take");
  }
}

bool Compiler::instruction(const Instruction& ins) {
  line_ = ins.line;
  switch (ins.op) {
    case Op::If: case Op::Uif: case Op::Else: case Op::EndIf: case Op::BgnLoop: case Op::EndLoop: case Op::Brk: case Op::Cont: case Op::Ret: case Op::End:
    case Op::Kill: case Op::KillIf: case Op::Emit: case Op::EndPrim: case Op::Membar: case Op::Barrier: case Op::Nop:
      if (ins.op == Op::If || ins.op == Op::Uif || ins.op == Op::BgnLoop) resume();
      return control(ins);
    default: break;
  }
  resume();
  if (is_texture_op(ins.op)) return texture(ins);
  if (ins.op == Op::InterpCentroid || ins.op == Op::InterpSample || ins.op == Op::InterpOffset) return interpolate(ins);
  if (is_memory_op(ins.op)) return memory(ins);
  if (is_double_op(ins.op)) return doubles(ins);
  return alu(ins);
}

// ---- the module -------------------------------------------------------------------------------------------------------------------------------------
bool Compiler::run() {
  // types and the extended instruction set
  f_ = b_.t_float(), u_ = b_.t_uint(), i_ = b_.t_int(), bool_ = b_.t_bool();
  f2_ = b_.t_vector(f_, 2), f3_ = b_.t_vector(f_, 3), f4_ = b_.t_vector(f_, 4);
  u2_ = b_.t_vector(u_, 2), u3_ = b_.t_vector(u_, 3), u4_ = b_.t_vector(u_, 4);
  i2_ = b_.t_vector(i_, 2), i3_ = b_.t_vector(i_, 3), i4_ = b_.t_vector(i_, 4), b4_ = b_.t_vector(bool_, 4);
  b_.capability(CapShader);
  glsl_ = b_.id();
  b_.op_string(Builder::kImports, OpExtInstImport, {glsl_}, "GLSL.std.450", nullptr, 0);
  b_.op(Builder::kMemoryModel, OpMemoryModel, {0, 1});   // (Logical, GLSL450)
  clip_count_ = o_.clip_distances;
  if (clip_count_) b_.capability(CapClipDistance);
  switch (p_.stage) {
    case Stage::Geometry: b_.capability(CapGeometry); break;
    case Stage::TessControl:
    case Stage::TessEval: b_.capability(CapTessellation); break;
    default: break;
  }
  // which streams a geometry shader emits to
  for (const Instruction& ins : p_.instructions)
    if ((ins.op == Op::Emit || ins.op == Op::EndPrim) && ins.src_count && ins.src[0].file == File::Imm && ins.src[0].index.value >= 0 &&
        static_cast<uint64_t>(ins.src[0].index.value) < p_.immediates.count()) {
      const Immediate& imm = p_.immediates[static_cast<uint64_t>(ins.src[0].index.value)];
      if (imm.bits[ins.src[0].swizzle[0]] != 0) multi_stream_ = true;
    }
  for (uint32_t i = 0; i < o_.stream_output_count; ++i)
    if (o_.stream_output[i].stream != 0) multi_stream_ = true;
  if (multi_stream_ && p_.stage == Stage::Geometry) b_.capability(CapGeometryStreams);
  if (o_.swap_stream && p_.stage == Stage::Geometry) multi_stream_ = true, b_.capability(CapGeometryStreams);

  if (!declare()) return false;

  // stream output: a copy of each captured part in a location of its own past the stage's outputs (the next stage does not read them)
  if (o_.stream_output_count) {
    b_.capability(CapTransformFeedback);
    uint32_t used = 0;
    for (const Io& io : out_)
      if (io.kind == IoKind::Generic) used = io.index + 1 > used ? io.index + 1 : used;
    uint32_t location = used, component = 0;
    for (uint32_t i = 0; i < o_.stream_output_count; ++i) {
      const StreamOutputEntry& e = o_.stream_output[i];
      if (e.reg >= kMaxOut || out_[e.reg].kind == IoKind::None) return fail("stream output of a register the shader does not write");
      if (e.count == 0 || e.count > 4 || e.first + e.count > 4 || e.buffer > 3) return fail("a stream output entry out of range");
      if (component + e.count > 4) ++location, component = 0;
      if (location >= 32) return fail("stream output needs more locations than a stage has");
      XfbCopy x;
      x.reg = e.reg, x.first = e.first, x.count = e.count;
      x.type = b_.t_vector(f_, e.count);
      x.var = b_.global(Output, x.type);
      b_.decorate(x.var, DecLocation, {location});
      if (component) b_.decorate(x.var, DecComponent, {component});
      b_.decorate(x.var, DecXfbBuffer, {e.buffer});
      b_.decorate(x.var, DecXfbStride, {o_.stream_output_stride[e.buffer] * 4});
      b_.decorate(x.var, DecOffset, {e.offset * 4});
      uint32_t stream = e.stream;
      if (o_.swap_stream) stream = stream == 0 ? o_.swap_stream : stream == o_.swap_stream ? 0 : stream;
      if (p_.stage == Stage::Geometry && multi_stream_) b_.decorate(x.var, DecStream, {stream});
      if (!xfb_.push_back(x)) return fail("out of memory");
      component += e.count;
    }
  }

  // the function
  const uint32_t main = b_.id();
  b_.op(Builder::kFunctionVariables, OpFunction, {b_.t_void(), main, 0, b_.t_function_void()});
  b_.op(Builder::kFunctionVariables, OpLabel, {b_.id()});
  for (const Instruction& ins : p_.instructions) {
    if (!instruction(ins)) return false;
    if (!ok_) return false;
  }
  if (!terminated_) return fail("the program does not end with END");
  b_.op(Builder::kCode, OpFunctionEnd, {});
  if (constructs_.count()) return fail("an IF or a loop without its end");

  // the kill instruction's extension
  bool demotes = false;
  for (const Instruction& ins : p_.instructions)
    if (ins.op == Op::Kill || ins.op == Op::KillIf) demotes = true;
  if (demotes) {
    b_.capability(CapDemoteToHelperInvocation);
    b_.extension("SPV_EXT_demote_to_helper_invocation");
  }

  // the entry point and its modes
  uint32_t model = ModelVertex;
  switch (p_.stage) {
    case Stage::Vertex: model = ModelVertex; break;
    case Stage::Fragment: model = ModelFragment; break;
    case Stage::Geometry: model = ModelGeometry; break;
    case Stage::TessControl: model = ModelTessControl; break;
    case Stage::TessEval: model = ModelTessEval; break;
    case Stage::Compute: model = ModelGLCompute; break;
  }
  b_.op_string(Builder::kEntryPoints, OpEntryPoint, {model, main}, "main", b_.interface().view().data(), static_cast<uint32_t>(b_.interface().count()));
  switch (p_.stage) {
    case Stage::Fragment:
      b_.op(Builder::kModes, OpExecutionMode, {main, ModeOriginUpperLeft});
      if (writes_depth_) b_.op(Builder::kModes, OpExecutionMode, {main, ModeDepthReplacing});
      if (p_.has(Property::FsEarlyDepthStencil) && p_.get(Property::FsEarlyDepthStencil)) b_.op(Builder::kModes, OpExecutionMode, {main, ModeEarlyFragmentTests});
      break;
    case Stage::Geometry: {
      const uint32_t in = p_.get(Property::GsInputPrimitive);
      const uint32_t in_mode = in == 0 ? ModeInputPoints : in == 1 || in == 3 ? ModeInputLines : in == 10 || in == 11 ? ModeInputLinesAdjacency
                               : in == 12 || in == 13 ? ModeInputTrianglesAdjacency : ModeTriangles;
      const uint32_t out = p_.get(Property::GsOutputPrimitive);
      const uint32_t out_mode = out == 0 ? ModeOutputPoints : out == 3 || out == 1 ? ModeOutputLineStrip : ModeOutputTriangleStrip;
      const uint32_t invocations = p_.has(Property::GsInvocations) && p_.get(Property::GsInvocations) ? p_.get(Property::GsInvocations) : 1;
      const uint32_t max_vertices = p_.get(Property::GsMaxOutputVertices);
      b_.op(Builder::kModes, OpExecutionMode, {main, in_mode});
      b_.op(Builder::kModes, OpExecutionMode, {main, ModeInvocations, invocations});
      b_.op(Builder::kModes, OpExecutionMode, {main, out_mode});
      b_.op(Builder::kModes, OpExecutionMode, {main, ModeOutputVertices, max_vertices ? max_vertices : 1});
      break;
    }
    case Stage::TessControl: b_.op(Builder::kModes, OpExecutionMode, {main, ModeOutputVertices, out_vertices_}); break;
    case Stage::TessEval: {
      const uint32_t prim = p_.get(Property::TesPrimMode);
      b_.op(Builder::kModes, OpExecutionMode, {main, prim == 1 ? ModeIsolines : prim == 7 ? ModeQuads : ModeTriangles});
      const uint32_t spacing = o_.tess_override ? o_.tess_spacing : p_.has(Property::TesSpacing) ? p_.get(Property::TesSpacing) : 2;
      const bool cw = o_.tess_override ? o_.tess_cw : p_.get(Property::TesVertexOrderCw) != 0;
      const bool points = o_.tess_override ? o_.tess_points : p_.get(Property::TesPointMode) != 0;
      b_.op(Builder::kModes, OpExecutionMode, {main, spacing == 0 ? ModeSpacingFractionalOdd : spacing == 1 ? ModeSpacingFractionalEven : ModeSpacingEqual});
      b_.op(Builder::kModes, OpExecutionMode, {main, cw ? ModeVertexOrderCw : ModeVertexOrderCcw});
      if (points) b_.op(Builder::kModes, OpExecutionMode, {main, ModePointMode});
      break;
    }
    case Stage::Compute: {
      auto dim = [&](Property p) { return p_.has(p) && p_.get(p) ? p_.get(p) : 1u; };
      b_.op(Builder::kModes, OpExecutionMode, {main, ModeLocalSize, dim(Property::CsFixedBlockWidth), dim(Property::CsFixedBlockHeight), dim(Property::CsFixedBlockDepth)});
      break;
    }
    default: break;
  }
  if (o_.stream_output_count) b_.op(Builder::kModes, OpExecutionMode, {main, ModeXfb});
  if (!b_.finish(&r_.words)) return fail("out of memory");
  return ok_;
}

}  // namespace

bool compile_spirv(const Program& program, const CompileOptions& options, CompileResult* result) {
  Compiler c(result->allocator, program, options, *result);
  return c.run();
}

}  // namespace graphics::tgsi
