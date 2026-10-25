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

// The shader interpreter behind WebGL and WebGPU. A front end (Glsl.cpp for GLSL ES 1.00/3.00, Wgsl.cpp for WGSL) parses source text into a Module: a tree of
// nodes with the types, globals and functions of the shader. A Machine runs a function of a Module.
//
// Values are flat: every scalar (float, int, uint, bool, or the handle of a texture or sampler) is one 32-bit slot; a vector is rows slots, a matrix cols*rows slots
// column by column, an array or a structure its elements one after the other. The machine is dynamically typed on top of that (each value carries its Type, and
// the usual promotions - int to float for WGSL's abstract numbers, scalar broadcast - are done as operations run). A Machine owns an arena of slots for
// temporaries and local variables, so running a shader allocates nothing.
//
// Shared storage (uniforms, buffers, workgroup variables, textures' handles) and private storage (inputs, outputs, private variables, builtins) are separate
// regions so that the invocations of a compute workgroup can share one and have their own of the other. A Machine can pause at a workgroup barrier and be
// resumed, which is how a workgroup's invocations are run in lockstep.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace shader {

constexpr uint32_t kRuntimeArray = 0xFFFFFFFEu;   // the length of a runtime-sized array until a buffer gives it one

enum class Language : uint8_t { Glsl100, Glsl300, Wgsl };
enum class Stage : uint8_t { Vertex, Fragment, Compute };

enum class Base : uint8_t { Void, Bool, Int, Uint, Float, Sampler, Texture, SamplerState, Struct };
enum : uint8_t { kDim2D = 0, kDimCube = 1, kDim3D = 2, kDim2DArray = 3, kDimCubeArray = 4, kDim1D = 5, kDimMultisampled = 6, kDimDepth2D = 7 };

struct Type {
  Base base = Base::Void;
  uint8_t cols = 1, rows = 1;   // scalar 1x1, vecN: rows = N, matCxR: cols = C, rows = R
  uint8_t dim = kDim2D;         // Sampler / Texture
  bool shadow = false;          // Sampler / Texture
  Base texel = Base::Float;     // Sampler / Texture: what a sample is made of
  uint16_t sid = 0;             // Struct: index in Module::structs
  uint32_t array = 0;           // 0: not an array
  bool is_scalar() const { return array == 0 && cols == 1 && rows == 1 && base != Base::Struct; }
  bool is_vector() const { return array == 0 && cols == 1 && rows > 1; }
  bool is_matrix() const { return array == 0 && cols > 1; }
  bool is_numeric() const { return array == 0 && (base == Base::Int || base == Base::Uint || base == Base::Float || base == Base::Bool); }
  Type element() const { Type t = *this; t.array = 0; return t; }
  bool operator==(const Type& o) const {
    return base == o.base && cols == o.cols && rows == o.rows && array == o.array && (base != Base::Struct || sid == o.sid) &&
           ((base != Base::Sampler && base != Base::Texture) || (dim == o.dim && shadow == o.shadow));
  }
  bool operator!=(const Type& o) const { return !(*this == o); }
};

inline Type make_type(Base b, uint8_t rows = 1, uint8_t cols = 1) {
  Type t;
  t.base = b;
  t.rows = rows;
  t.cols = cols;
  return t;
}

struct Field {
  Field(allocators::IAllocator& a) : name(a) {}
  String name;
  Type type;
  uint32_t offset = 0;       // slots
  int32_t location = -1;     // WGSL @location
  int32_t builtin = -1;      // WGSL @builtin (a BuiltinVar)
  bool flat = false;         // WGSL @interpolate(flat)
};

struct StructDef {
  explicit StructDef(allocators::IAllocator& a) : name(a), fields(a) {}
  String name;
  List<Field> fields;
  uint32_t slots = 0;
};

// Built-in variables (gl_Position ... and WGSL's @builtin values).
enum BuiltinVar : int32_t {
  kBvNone = -1, kBvPosition = 0, kBvPointSize, kBvFragCoord, kBvFrontFacing, kBvPointCoord, kBvFragColor, kBvFragData, kBvFragDepth, kBvVertexIndex, kBvInstanceIndex,
  kBvLocalInvocationId, kBvLocalInvocationIndex, kBvGlobalInvocationId, kBvWorkgroupId, kBvNumWorkgroups, kBvSampleIndex, kBvSampleMask, kBvBaseVertex, kBvBaseInstance,
  kBvClipDistance
};

enum class GlobalKind : uint8_t { Uniform, In, Out, Private, Const, Builtin, Buffer, Workgroup, Handle };

struct Global {
  explicit Global(allocators::IAllocator& a) : name(a) {}
  String name;
  Type type;
  GlobalKind kind = GlobalKind::Private;
  bool shared = false;      // in the shared region
  bool flat = false;        // flat-interpolated varying
  bool is_read_only = false;
  bool storage = false;     // WGSL var<storage>
  bool referenced = false;  // named somewhere in a function body
  uint32_t slot = 0;        // offset (in slots) in its region
  int32_t location = -1;    // layout(location) / @location
  int32_t builtin = kBvNone;
  int32_t group = 0, binding = -1;   // WGSL @group/@binding, GLSL layout(binding)
  int32_t init = -1;        // node of the initializer
  int32_t block = -1;       // uniform block index (GLSL) or -1
  uint32_t block_offset = 0;   // byte offset inside the block (std140)
  uint32_t uniform_index = 0;
};

struct UniformBlock {
  explicit UniformBlock(allocators::IAllocator& a) : name(a), instance(a) {}
  String name, instance;
  int32_t binding = -1;
  uint32_t bytes = 0;   // std140 size
};

struct Param {
  Type type;
  uint8_t dir = 0;   // 0 in, 1 out, 2 inout
  int32_t location = -1;
  int32_t builtin = kBvNone;
  bool flat = false;
};

struct Function {
  explicit Function(allocators::IAllocator& a) : name(a), params(a) {}
  String name;
  List<Param> params;
  Type ret;
  int32_t body = -1;
  uint32_t locals = 0;       // variable slots, parameters first
  bool is_entry = false;
  Stage stage = Stage::Vertex;
  uint32_t wg_x = 1, wg_y = 1, wg_z = 1;
  bool ret_location_set = false;
  int32_t ret_location = -1;
  int32_t ret_builtin = kBvNone;
  bool uses_derivatives = false;
};

enum class NodeKind : uint8_t {
  // expressions
  Const, Local, Global, Unary, Binary, Logical, Assign, Ternary, Call, Builtin, Construct, Index, Field, Swizzle, PreInc, PostInc, Comma, Cast, Bitcast, Length, TexCall, Zero,
  // statements
  Block, Decl, ExprStmt, If, For, While, DoWhile, Loop, Switch, Case, Return, Break, Continue, Discard, BreakIf, Empty
};

enum class Op : uint8_t { None, Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr, Eq, Ne, Lt, Le, Gt, Ge, LogAnd, LogOr, LogXor, Neg, Not, BitNot, Plus };

// A texture call's operation (Node::op of a TexCall).
enum class TexOp : uint8_t { Sample, Bias, Lod, Grad, Compare, CompareLevel, Fetch, Size, Levels, Gather, GatherCompare, Proj, ProjBias, ProjLod, Samples, Store };

struct Node {
  NodeKind kind = NodeKind::Empty;
  Op op = Op::None;
  uint8_t flags = 0;       // TexCall: 1 = WGSL argument layout
  int32_t a = -1, b = -1, c = -1, d = -1;
  int32_t first = 0, count = 0;   // children in Module::kids
  int32_t ival = 0;        // Local/Global: index; Call: function; Builtin: BuiltinFn; Field: field index; Swizzle: components packed 4 x 2 bits + count in bits 8..; Decl: slot
  Type type;               // Const: its type; Construct/Cast/Zero: target; Decl: variable type
  uint32_t line = 0;
};

enum class BuiltinFn : uint16_t {
  None,
  Radians, Degrees, Sin, Cos, Tan, Asin, Acos, Atan, Atan2, Sinh, Cosh, Tanh, Asinh, Acosh, Atanh,
  Pow, Exp, Log, Exp2, Log2, Sqrt, InverseSqrt, Abs, Sign, Floor, Ceil, Fract, Trunc, Round, RoundEven, Saturate, Mod, Min, Max, Clamp, Mix, Step, Smoothstep, Fma,
  IsNan, IsInf, Length, Distance, Dot, Cross, Normalize, FaceForward, Reflect, Refract, Ldexp, Frexp, Modf,
  MatrixCompMult, OuterProduct, Transpose, Determinant, Inverse,
  LessThan, LessThanEqual, GreaterThan, GreaterThanEqual, Equal, NotEqual, Any, All, Not, Select,
  FloatBitsToInt, FloatBitsToUint, IntBitsToFloat, UintBitsToFloat,
  PackSnorm2x16, PackUnorm2x16, PackHalf2x16, UnpackSnorm2x16, UnpackUnorm2x16, UnpackHalf2x16, PackUnorm4x8, PackSnorm4x8, UnpackUnorm4x8, UnpackSnorm4x8,
  BitCount, FindLsb, FindMsb, BitfieldExtract, BitfieldInsert, BitfieldReverse, UaddCarry, UsubBorrow, FirstLeadingBit, FirstTrailingBit,
  Dfdx, Dfdy, Fwidth,
  Barrier, AtomicLoad, AtomicStore, AtomicAdd, AtomicSub, AtomicMax, AtomicMin, AtomicAnd, AtomicOr, AtomicXor, AtomicExchange, AtomicCompareExchange,
  ArrayLength, Dot4U8Packed, CountLeadingZeros, CountTrailingZeros, ReverseBits, ExtractBits, InsertBits, Step1, TextureBarrier, Transform2
};

struct Module {
  explicit Module(allocators::IAllocator& a)
      : allocator(a), nodes(a), kids(a), consts(a), structs(a), globals(a), functions(a), blocks(a), error(a), shared_slots(0), private_slots(0) {}
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  allocators::IAllocator& allocator;
  Language language = Language::Glsl100;
  Stage stage = Stage::Vertex;
  List<Node> nodes;
  List<int32_t> kids;
  List<uint32_t> consts;
  List<UniquePointer<StructDef>> structs;
  List<UniquePointer<Global>> globals;
  List<UniquePointer<Function>> functions;
  List<UniquePointer<UniformBlock>> blocks;
  String error;                 // why parsing failed (empty: fine)
  uint32_t shared_slots, private_slots;
  int32_t main_function = -1;   // GLSL: main()
  bool uses_derivatives = false;   // dFdx/dFdy/fwidth is in the shader
  bool uses_implicit_lod = false;  // a texture lookup that takes its level from the derivatives of the coordinate
  bool writes_frag_depth = false;
  bool uses_discard = false;
  bool uses_barrier = false;
  uint32_t wg_x = 1, wg_y = 1, wg_z = 1;   // WGSL compute entry points: copied from the entry point asked for
  uint32_t glsl_version = 100;

  uint32_t slots_of(const Type& t) const;
  const StructDef* struct_of(const Type& t) const { return t.base == Base::Struct && t.sid < structs.count() ? structs[t.sid].get() : nullptr; }
  int32_t find_global(StringView name) const;
  int32_t find_function(StringView name) const;
  int32_t find_builtin_global(int32_t builtin) const;
};

// --- Front ends. Return false (with module.error set) when the source is wrong.
// `extensions` (GLSL): the extension names the context offers, as a bit set the front end understands is not needed: #extension is accepted for any name.
bool parse_glsl(Module& module, StringView source, Stage stage);
bool parse_wgsl(Module& module, StringView source);
// Gives the runtime-sized array of a storage buffer global (or the last field of its struct) a length and updates the layout.
void set_runtime_length(Module& module, uint32_t global, uint32_t count);
// Gives a WGSL `override` constant its value (before the shader runs). False if there is no such constant.
bool set_override(Module& module, StringView name, double value);
// Recomputes where shared globals sit after a runtime-sized array got its length (WGSL storage buffers).
void relayout_shared(Module& module);

// --- Textures, as seen by a running shader.
enum class SampleMode : uint8_t { Implicit, Bias, Lod, Grad, Fetch, Size, Levels, Gather, Samples, Store };
struct SampleRequest {
  uint32_t texture = 0, sampler = 0;   // the handle values in the shader's slots
  uint8_t dim = kDim2D;
  bool shadow = false;
  bool compare = false;
  Base texel = Base::Float;
  SampleMode mode = SampleMode::Implicit;
  float coord[4] = {0, 0, 0, 0};     // s, t, r (or layer in coord[2] for 2D arrays; cubes use 3 components, cube arrays carry the layer in layer)
  float layer = 0;
  float reference = 0;               // depth comparison
  float lod = 0, bias = 0;
  float ddx[3] = {0, 0, 0}, ddy[3] = {0, 0, 0};
  bool has_derivatives = false;      // ddx/ddy are real (otherwise level 0 is used for Implicit/Bias)
  int32_t offset[3] = {0, 0, 0};
  int32_t icoord[4] = {0, 0, 0, 0};  // Fetch
  int32_t level = 0;                 // Fetch, Size
  int32_t component = 0;             // Gather
  uint32_t value[4] = {0, 0, 0, 0};  // Store: the texel (raw lanes)
};
class ITextureHost {
 public:
  // Writes the result as four 32-bit values (floats for Float texels; ints for Int/Uint ones, and for sizes: width, height, depth, 0).
  virtual void sample(const SampleRequest& request, uint32_t out[4]) = 0;

 protected:
  ~ITextureHost() = default;
};

// Which derivative-site values a neighbouring pixel recorded (see Machine::set_derivative_source).
struct DerivativeLog {
  explicit DerivativeLog(allocators::IAllocator& a) : values(a) {}
  List<float> values;
  void clear() { values.clear(); }
};

class Machine {
 public:
  enum class Status : uint8_t { Done, Yield, Discarded, Failed };
  static constexpr uint32_t kDefaultArena = 16384;

  Machine(const Module& module, allocators::IAllocator& allocator, uint32_t arena_slots = kDefaultArena);
  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;
  ~Machine();

  bool valid() const { return valid_; }
  const Module& module() const { return module_; }
  uint32_t* private_region() { return priv_; }
  uint32_t* shared_region() const { return shared_; }
  void set_shared(uint32_t* region) { shared_ = region; }
  void set_texture_host(ITextureHost* host) { host_ = host; }
  // Zeroes the private region and runs the initializers of the private globals (call before the first run, and to reset between invocations when wanted).
  bool reset_privates();
  // Runs the initializers of the shared globals of a region of module.shared_slots slots; once per region.
  bool init_shared(uint32_t* shared);

  // Runs function `index` with arguments (one pointer per parameter, read as the parameter's type). When Done, result() is the return value.
  Status start(int32_t index, ArrayView<const uint32_t* const> args);
  Status resume();
  const uint32_t* result() const { return result_; }
  Type result_type() const { return result_type_; }
  const char* failure() const { return failure_; }

  // Derivatives: record the values the derivative sites see (for the neighbouring pixel), or compute against recorded ones.
  void set_derivative_record(DerivativeLog* log) { record_ = log; replay_x_ = nullptr; replay_y_ = nullptr; }
  void set_derivative_replay(const DerivativeLog* x, const DerivativeLog* y) { record_ = nullptr; replay_x_ = x; replay_y_ = y; }
  void set_step_limit(uint64_t steps) { limit_ = steps; }

 private:
  friend struct Interp;
  struct Var { uint32_t* p; Type t; };
  struct Frame { int32_t node; int32_t state; int32_t index; uint32_t mark; uint32_t aux; };

  const Module& module_;
  allocators::IAllocator& allocator_;
  uint32_t* arena_ = nullptr;
  uint32_t arena_size_ = 0, top_ = 0;
  uint32_t* priv_ = nullptr;
  uint32_t* shared_ = nullptr;
  bool valid_ = false;
  ITextureHost* host_ = nullptr;
  static constexpr uint32_t kMaxVars = 4096, kMaxFrames = 2048;
  Var* vars_ = nullptr;
  Frame* frames_ = nullptr;
  uint32_t var_top_ = 0, frame_top_ = 0;
  uint32_t var_base_ = 0;
  uint32_t depth_ = 0;
  const uint32_t* result_ = nullptr;
  Type result_type_;
  const char* failure_ = nullptr;
  uint64_t steps_ = 0, limit_ = 4000000;
  DerivativeLog* record_ = nullptr;
  const DerivativeLog* replay_x_ = nullptr;
  const DerivativeLog* replay_y_ = nullptr;
  uint32_t site_ = 0;
  bool discarded_ = false;
};

// Parse a GLSL/WGSL source into a freshly made module. (Convenience over the front ends.)
inline const char* module_error(const Module& m) { return m.error.c_str(); }

// --- Memory layouts of host-shareable data (uniform and storage buffers). Std140 is OpenGL's uniform block layout; Natural is WGSL's (and std430's).
enum class Layout : uint8_t { Std140, Natural, WgslUniform };
void layout_of(const Module& m, const Type& t, Layout kind, uint32_t* align, uint32_t* size);
// Reads/writes a value of type t at byte `offset` of `bytes` from/to the module's flat slots. Out-of-range reads give zero; out-of-range writes are dropped.
void load_layout(const Module& m, const Type& t, Layout kind, ConstByteView bytes, uint64_t offset, uint32_t* slots);
void store_layout(const Module& m, const Type& t, Layout kind, ByteView bytes, uint64_t offset, const uint32_t* slots);
// The byte offset of each field of a struct under a layout.
uint32_t field_offset(const Module& m, const StructDef& sd, uint32_t field, Layout kind);

// Math helpers shared with the rasterizer (no libm in the image).
float fsin(float x);
float fcos(float x);
float fsqrt(float x);
float fpow(float x, float y);
float fexp(float x);
float flog(float x);
float fatan2(float y, float x);
float ffloor(float x);
float fround_even(float x);
uint16_t float_to_half(float f);
float half_to_float(uint16_t h);

}  // namespace shader
