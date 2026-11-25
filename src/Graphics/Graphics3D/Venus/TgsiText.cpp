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

#include "Graphics/Graphics3D/Venus/TgsiText.hpp"

namespace graphics::tgsi {
namespace {

constexpr OpInfo kOps[] = {
    {"MOV", 1, 1},     {"ADD", 1, 2},      {"SUB", 1, 2},      {"MUL", 1, 2},     {"MAD", 1, 3},     {"FMA", 1, 3},     {"DIV", 1, 2},      {"LRP", 1, 3},
    {"MIN", 1, 2},     {"MAX", 1, 2},      {"ABS", 1, 1},      {"SSG", 1, 1},     {"FRC", 1, 1},     {"FLR", 1, 1},     {"CEIL", 1, 1},     {"TRUNC", 1, 1},
    {"ROUND", 1, 1},   {"DP2", 1, 2},      {"DP3", 1, 2},      {"DP4", 1, 2},     {"DPH", 1, 2},     {"DST", 1, 2},     {"LIT", 1, 1},      {"RCP", 1, 1},
    {"RSQ", 1, 1},     {"SQRT", 1, 1},     {"EX2", 1, 1},      {"LG2", 1, 1},     {"EXP", 1, 1},     {"LOG", 1, 1},     {"POW", 1, 2},      {"SIN", 1, 1},
    {"COS", 1, 1},     {"XPD", 1, 2},      {"CMP", 1, 3},      {"SLT", 1, 2},     {"SGE", 1, 2},     {"SEQ", 1, 2},     {"SNE", 1, 2},      {"SGT", 1, 2},
    {"SLE", 1, 2},     {"FSLT", 1, 2},     {"FSGE", 1, 2},     {"FSEQ", 1, 2},    {"FSNE", 1, 2},    {"DDX", 1, 1},     {"DDY", 1, 1},      {"DDX_FINE", 1, 1},
    {"DDY_FINE", 1, 1}, {"KILL", 0, 0},    {"KILL_IF", 0, 1},
    {"UADD", 1, 2},    {"INEG", 1, 1},     {"IABS", 1, 1},     {"ISSG", 1, 1},    {"UMUL", 1, 2},    {"UMUL_HI", 1, 2}, {"IMUL_HI", 1, 2},  {"UMAD", 1, 3},
    {"IMIN", 1, 2},    {"IMAX", 1, 2},     {"UMIN", 1, 2},     {"UMAX", 1, 2},    {"UDIV", 1, 2},    {"IDIV", 1, 2},    {"UMOD", 1, 2},     {"MOD", 1, 2},
    {"AND", 1, 2},     {"OR", 1, 2},       {"XOR", 1, 2},      {"NOT", 1, 1},     {"SHL", 1, 2},     {"ISHR", 1, 2},    {"USHR", 1, 2},     {"F2I", 1, 1},
    {"F2U", 1, 1},     {"I2F", 1, 1},      {"U2F", 1, 1},      {"USEQ", 1, 2},    {"USNE", 1, 2},    {"ISLT", 1, 2},    {"ISGE", 1, 2},     {"USLT", 1, 2},
    {"USGE", 1, 2},    {"UCMP", 1, 3},
    {"DADD", 1, 2},    {"DMUL", 1, 2},     {"DDIV", 1, 2},     {"DFMA", 1, 3},    {"DMAD", 1, 3},    {"DRCP", 1, 1},    {"DSQRT", 1, 1},    {"DRSQ", 1, 1},
    {"DMIN", 1, 2},    {"DMAX", 1, 2},     {"DMOV", 1, 1},     {"DNEG", 1, 1},    {"DABS", 1, 1},    {"DSEQ", 1, 2},    {"DSGE", 1, 2},     {"DSLT", 1, 2},
    {"DSNE", 1, 2},    {"D2F", 1, 1},      {"D2I", 1, 1},      {"D2U", 1, 1},     {"F2D", 1, 1},     {"I2D", 1, 1},     {"U2D", 1, 1},
    {"ARL", 1, 1},     {"ARR", 1, 1},      {"UARL", 1, 1},
    {"TEX", 1, 2},     {"TXP", 1, 2},      {"TXB", 1, 2},      {"TXD", 1, 4},     {"TXL", 1, 2},     {"TXL2", 1, 3},    {"TXB2", 1, 3},     {"TEX2", 1, 3},
    {"TXF", 1, 2},     {"TXQ", 1, 2},      {"TG4", 1, 3},      {"LODQ", 1, 2},    {"TXQS", 1, 1},
    {"INTERP_CENTROID", 1, 1}, {"INTERP_SAMPLE", 1, 2}, {"INTERP_OFFSET", 1, 2},
    {"LOAD", 1, 2},    {"STORE", 1, 2},    {"RESQ", 1, 1},     {"ATOMUADD", 1, 3}, {"ATOMAND", 1, 3}, {"ATOMOR", 1, 3},  {"ATOMXOR", 1, 3},  {"ATOMIMAX", 1, 3},
    {"ATOMIMIN", 1, 3}, {"ATOMUMAX", 1, 3}, {"ATOMUMIN", 1, 3}, {"ATOMXCHG", 1, 3}, {"ATOMCAS", 1, 4},
    {"IF", 0, 1},      {"UIF", 0, 1},      {"ELSE", 0, 0},     {"ENDIF", 0, 0},   {"BGNLOOP", 0, 0}, {"ENDLOOP", 0, 0}, {"BRK", 0, 0},      {"CONT", 0, 0},
    {"RET", 0, 0},     {"END", 0, 0},      {"EMIT", 0, 1},     {"ENDPRIM", 0, 1}, {"MEMBAR", 0, 1},  {"BARRIER", 0, 0}, {"NOP", 0, 0},
};
static_assert(sizeof(kOps) / sizeof(kOps[0]) == static_cast<uint32_t>(Op::kCount), "every opcode has its name");

struct NamedTarget {
  const char* name;
  Target target;
};
constexpr NamedTarget kTargets[] = {
    {"BUFFER", Target::Buffer},        {"1D", Target::T1D},           {"2D", Target::T2D},           {"3D", Target::T3D},
    {"CUBE", Target::Cube},            {"RECT", Target::Rect},        {"1D_ARRAY", Target::T1DArray}, {"2D_ARRAY", Target::T2DArray},
    {"CUBE_ARRAY", Target::CubeArray}, {"CUBEARRAY", Target::CubeArray}, {"2D_MSAA", Target::T2DMsaa}, {"2D_ARRAY_MSAA", Target::T2DArrayMsaa},
    {"SHADOW1D", Target::Shadow1D},    {"SHADOW2D", Target::Shadow2D}, {"SHADOWRECT", Target::ShadowRect}, {"SHADOW1D_ARRAY", Target::Shadow1DArray},
    {"SHADOW2D_ARRAY", Target::Shadow2DArray}, {"SHADOWCUBE", Target::ShadowCube}, {"SHADOWCUBE_ARRAY", Target::ShadowCubeArray},
    {"SHADOWCUBEARRAY", Target::ShadowCubeArray},
};

struct NamedSemantic {
  const char* name;
  Semantic semantic;
};
constexpr NamedSemantic kSemantics[] = {
    {"POSITION", Semantic::Position},   {"COLOR", Semantic::Color},           {"BCOLOR", Semantic::BColor},       {"FOG", Semantic::Fog},
    {"PSIZE", Semantic::Psize},         {"GENERIC", Semantic::Generic},       {"NORMAL", Semantic::Normal},       {"FACE", Semantic::Face},
    {"EDGEFLAG", Semantic::EdgeFlag},   {"PRIM_ID", Semantic::PrimId},        {"INSTANCEID", Semantic::InstanceId}, {"VERTEXID", Semantic::VertexId},
    {"STENCIL", Semantic::StencilRef},  {"CULLDIST", Semantic::CullDist},     {"CLIPDIST", Semantic::ClipDist},   {"CLIPVERTEX", Semantic::ClipVertex},
    {"GRID_SIZE", Semantic::GridSize},  {"BLOCK_ID", Semantic::BlockId},      {"BLOCK_SIZE", Semantic::BlockSize}, {"THREAD_ID", Semantic::ThreadId},
    {"TEXCOORD", Semantic::TexCoord},   {"PCOORD", Semantic::Pcoord},         {"VIEWPORT_INDEX", Semantic::ViewportIndex}, {"LAYER", Semantic::Layer},
    {"SAMPLEID", Semantic::SampleId},   {"SAMPLEPOS", Semantic::SamplePos},   {"SAMPLEMASK", Semantic::SampleMask}, {"INVOCATIONID", Semantic::InvocationId},
    {"VERTEXID_NOBASE", Semantic::VertexIdNoBase}, {"BASEVERTEX", Semantic::BaseVertex}, {"PRIMID", Semantic::PrimitiveId},
    {"TESSCOORD", Semantic::TessCoord}, {"VERTICESIN", Semantic::VerticesIn}, {"PATCH", Semantic::Patch},     {"TESSOUTER", Semantic::TessOuter},
    {"TESSINNER", Semantic::TessInner}, {"VERTICESOUT", Semantic::VertexOut}, {"VIEWPORT_MASK", Semantic::ViewportMask},
    {"HELPER_INVOCATION", Semantic::HelperInvocation}, {"BASEINSTANCE", Semantic::BaseInstance}, {"DRAWID", Semantic::DrawId},
    {"WORK_DIM", Semantic::WorkDim},    {"SUBGROUP_SIZE", Semantic::SubgroupSize}, {"SUBGROUP_INVOCATION", Semantic::SubgroupInvocation},
};

struct NamedFormat {
  const char* name;
  ImageFormat format;
};
constexpr NamedFormat kFormats[] = {
    {"PIPE_FORMAT_R8G8B8A8_UNORM", ImageFormat::R8G8B8A8Unorm},         {"PIPE_FORMAT_R8G8B8A8_SNORM", ImageFormat::R8G8B8A8Snorm},
    {"PIPE_FORMAT_R8G8B8A8_UINT", ImageFormat::R8G8B8A8Uint},           {"PIPE_FORMAT_R8G8B8A8_SINT", ImageFormat::R8G8B8A8Sint},
    {"PIPE_FORMAT_B8G8R8A8_UNORM", ImageFormat::B8G8R8A8Unorm},         {"PIPE_FORMAT_R16G16B16A16_FLOAT", ImageFormat::R16G16B16A16Float},
    {"PIPE_FORMAT_R16G16B16A16_UNORM", ImageFormat::R16G16B16A16Unorm}, {"PIPE_FORMAT_R16G16B16A16_SNORM", ImageFormat::R16G16B16A16Snorm},
    {"PIPE_FORMAT_R16G16B16A16_UINT", ImageFormat::R16G16B16A16Uint},   {"PIPE_FORMAT_R16G16B16A16_SINT", ImageFormat::R16G16B16A16Sint},
    {"PIPE_FORMAT_R32G32B32A32_FLOAT", ImageFormat::R32G32B32A32Float}, {"PIPE_FORMAT_R32G32B32A32_UINT", ImageFormat::R32G32B32A32Uint},
    {"PIPE_FORMAT_R32G32B32A32_SINT", ImageFormat::R32G32B32A32Sint},   {"PIPE_FORMAT_R32G32_FLOAT", ImageFormat::R32G32Float},
    {"PIPE_FORMAT_R32G32_UINT", ImageFormat::R32G32Uint},               {"PIPE_FORMAT_R32G32_SINT", ImageFormat::R32G32Sint},
    {"PIPE_FORMAT_R16G16_FLOAT", ImageFormat::R16G16Float},             {"PIPE_FORMAT_R16G16_UNORM", ImageFormat::R16G16Unorm},
    {"PIPE_FORMAT_R16G16_SNORM", ImageFormat::R16G16Snorm},             {"PIPE_FORMAT_R16G16_UINT", ImageFormat::R16G16Uint},
    {"PIPE_FORMAT_R16G16_SINT", ImageFormat::R16G16Sint},               {"PIPE_FORMAT_R8G8_UNORM", ImageFormat::R8G8Unorm},
    {"PIPE_FORMAT_R8G8_SNORM", ImageFormat::R8G8Snorm},                 {"PIPE_FORMAT_R8G8_UINT", ImageFormat::R8G8Uint},
    {"PIPE_FORMAT_R8G8_SINT", ImageFormat::R8G8Sint},                   {"PIPE_FORMAT_R32_FLOAT", ImageFormat::R32Float},
    {"PIPE_FORMAT_R32_UINT", ImageFormat::R32Uint},                     {"PIPE_FORMAT_R32_SINT", ImageFormat::R32Sint},
    {"PIPE_FORMAT_R16_FLOAT", ImageFormat::R16Float},                   {"PIPE_FORMAT_R16_UNORM", ImageFormat::R16Unorm},
    {"PIPE_FORMAT_R16_SNORM", ImageFormat::R16Snorm},                   {"PIPE_FORMAT_R16_UINT", ImageFormat::R16Uint},
    {"PIPE_FORMAT_R16_SINT", ImageFormat::R16Sint},                     {"PIPE_FORMAT_R8_UNORM", ImageFormat::R8Unorm},
    {"PIPE_FORMAT_R8_SNORM", ImageFormat::R8Snorm},                     {"PIPE_FORMAT_R8_UINT", ImageFormat::R8Uint},
    {"PIPE_FORMAT_R8_SINT", ImageFormat::R8Sint},                       {"PIPE_FORMAT_R10G10B10A2_UNORM", ImageFormat::R10G10B10A2Unorm},
    {"PIPE_FORMAT_R10G10B10A2_UINT", ImageFormat::R10G10B10A2Uint},     {"PIPE_FORMAT_R11G11B10_FLOAT", ImageFormat::R11G11B10Float},
};

struct NamedProperty {
  const char* name;
  Property property;
};
constexpr NamedProperty kProperties[] = {
    {"GS_INPUT_PRIMITIVE", Property::GsInputPrimitive}, {"GS_OUTPUT_PRIMITIVE", Property::GsOutputPrimitive},
    {"GS_MAX_OUTPUT_VERTICES", Property::GsMaxOutputVertices}, {"GS_INVOCATIONS", Property::GsInvocations},
    {"FS_COLOR0_WRITES_ALL_CBUFS", Property::FsColor0WritesAllCbufs}, {"FS_COORD_ORIGIN", Property::FsCoordOrigin},
    {"FS_COORD_PIXEL_CENTER", Property::FsCoordPixelCenter}, {"FS_DEPTH_LAYOUT", Property::FsDepthLayout},
    {"FS_EARLY_DEPTH_STENCIL", Property::FsEarlyDepthStencil}, {"VS_PROHIBIT_UCPS", Property::VsProhibitUcps},
    {"VS_WINDOW_SPACE_POSITION", Property::VsWindowSpacePosition}, {"TCS_VERTICES_OUT", Property::TcsVerticesOut},
    {"TES_PRIM_MODE", Property::TesPrimMode}, {"TES_SPACING", Property::TesSpacing}, {"TES_VERTEX_ORDER_CW", Property::TesVertexOrderCw},
    {"TES_POINT_MODE", Property::TesPointMode}, {"CS_FIXED_BLOCK_WIDTH", Property::CsFixedBlockWidth},
    {"CS_FIXED_BLOCK_HEIGHT", Property::CsFixedBlockHeight}, {"CS_FIXED_BLOCK_DEPTH", Property::CsFixedBlockDepth},
    {"NUM_CLIPDIST_ENABLED", Property::NumClipdistEnabled}, {"NUM_CULLDIST_ENABLED", Property::NumCulldistEnabled}, {"NEXT_SHADER", Property::NextShader},
};

// (Gallium's primitive numbers, which the primitive PROPERTY values are)
struct NamedPrimitive {
  const char* name;
  uint32_t value;
};
constexpr NamedPrimitive kPrimitives[] = {
    {"POINTS", 0}, {"LINES", 1}, {"LINE_LOOP", 2}, {"LINE_STRIP", 3}, {"TRIANGLES", 4}, {"TRIANGLE_STRIP", 5}, {"TRIANGLE_FAN", 6}, {"QUADS", 7},
    {"LINES_ADJACENCY", 10}, {"LINE_STRIP_ADJACENCY", 11}, {"TRIANGLES_ADJACENCY", 12}, {"TRIANGLE_STRIP_ADJACENCY", 13},
};

struct NamedFile {
  const char* name;
  File file;
};
constexpr NamedFile kFiles[] = {
    {"TEMP", File::Temp}, {"IN", File::In},     {"OUT", File::Out},   {"CONST", File::Const}, {"IMM", File::Imm}, {"SV", File::Sv},
    {"SAMP", File::Samp}, {"SVIEW", File::Sview}, {"ADDR", File::Addr}, {"IMAGE", File::Image}, {"BUFFER", File::Buffer}, {"MEMORY", File::Memory},
};

// A cursor over one line.
class Cursor {
 public:
  explicit Cursor(StringView line) : s_(line) {}
  bool done() {
    skip_space();
    return at_ >= s_.size();
  }
  void skip_space() {
    while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\r')) ++at_;
  }
  bool peek(char c) {
    skip_space();
    return at_ < s_.size() && s_[at_] == c;
  }
  bool take(char c) {
    if (!peek(c)) return false;
    ++at_;
    return true;
  }
  // A word of letters, digits and '_' (may start with a digit: 2D).
  StringView word() {
    skip_space();
    const uint64_t start = at_;
    while (at_ < s_.size() && (is_alnum(s_[at_]) || s_[at_] == '_')) ++at_;
    return s_.substr(start, at_ - start);
  }
  // A word that is exactly `w` (and not the start of a longer one).
  bool take_word(StringView w) {
    skip_space();
    const uint64_t save = at_;
    if (word() == w) return true;
    at_ = save;
    return false;
  }
  bool number(uint32_t* out) {
    skip_space();
    uint64_t v = 0;
    const uint64_t start = at_;
    while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') {
      v = v * 10 + static_cast<uint64_t>(s_[at_] - '0');
      if (v > 0xFFFFFFFFull) return false;
      ++at_;
    }
    if (at_ == start) return false;
    *out = static_cast<uint32_t>(v);
    return true;
  }
  bool signed_number(int64_t* out) {
    const bool minus = take('-');
    uint32_t v = 0;
    if (!number(&v)) return false;
    *out = minus ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
    return true;
  }
  // A decimal float as TGSI writes it: [-]digits[.digits][e[+-]digits], or inf/nan spellings; the nearest float's bits.
  bool float_bits(uint32_t* out);
  uint64_t position() const { return at_; }
  void seek(uint64_t p) { at_ = p; }
  StringView rest() const { return s_.substr(at_, s_.size() - at_); }

 private:
  static bool is_alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
  StringView s_;
  uint64_t at_ = 0;
};

// (exact powers of ten as doubles: 10^0 .. 10^22)
double power_of_ten(uint32_t n) {
  double p = 1;
  for (uint32_t i = 0; i < n; ++i) p *= 10;
  return p;
}

uint32_t bits_of(float f) {
  uint32_t b;
  __builtin_memcpy(&b, &f, 4);
  return b;
}

bool Cursor::float_bits(uint32_t* out) {
  skip_space();
  bool minus = false;
  if (take('-')) minus = true;
  else take('+');
  skip_space();
  if (take_word("inf") || take_word("INF")) {
    *out = (minus ? 0x80000000u : 0) | 0x7F800000u;
    return true;
  }
  if (take_word("nan") || take_word("NAN")) {
    *out = 0x7FC00000u;
    return true;
  }
  uint64_t mantissa = 0;
  int32_t exponent = 0;
  uint32_t digits = 0;
  bool any = false;
  auto digit = [&](char c) {
    any = true;
    if (digits < 19) {
      mantissa = mantissa * 10 + static_cast<uint64_t>(c - '0');
      if (mantissa) ++digits;
      return 0;
    }
    return 1;   // (a digit past what 64 bits hold: only its place counts)
  };
  while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') exponent += digit(s_[at_++]);
  if (at_ < s_.size() && s_[at_] == '.') {
    ++at_;
    while (at_ < s_.size() && s_[at_] >= '0' && s_[at_] <= '9') {
      if (digit(s_[at_]) == 0) --exponent;
      ++at_;
    }
  }
  if (!any) return false;
  if (at_ < s_.size() && (s_[at_] == 'e' || s_[at_] == 'E')) {
    ++at_;
    int64_t e = 0;
    if (!signed_number(&e) && !(take('+') && signed_number(&e))) return false;
    if (e > 400 || e < -400) return false;
    exponent += static_cast<int32_t>(e);
  }
  // mantissa < 2^63; the producers write at most 12 fraction digits, so the value is mantissa / 10^k with an exact divisor (rounded once to double, then to float).
  double v = static_cast<double>(mantissa);
  if (mantissa != 0) {
    while (exponent > 22) v *= 1e22, exponent -= 22;
    while (exponent < -22) v /= 1e22, exponent += 22;
    if (exponent > 0) v *= power_of_ten(static_cast<uint32_t>(exponent));
    else if (exponent < 0) v /= power_of_ten(static_cast<uint32_t>(-exponent));
  }
  const float f = static_cast<float>(minus ? -v : v);
  *out = bits_of(f);
  return true;
}

template <class T, uint64_t N>
bool lookup(const T (&table)[N], StringView name, T* out) {
  for (const T& e : table)
    if (StringView(e.name) == name) {
      *out = e;
      return true;
    }
  return false;
}

class Reader {
 public:
  Reader(Program& program, ParseError& error) : p_(program), error_(error) {}
  bool run(StringView text);

 private:
  bool fail(const char* message) {
    error_.line = line_;
    error_.message.assign(StringView(message));
    return false;
  }
  bool header(StringView w);
  bool property(Cursor& c);
  bool declaration(Cursor& c);
  bool immediate(Cursor& c);
  bool instruction(Cursor& c);
  bool operand(Cursor& c, Operand* o, bool destination);
  bool index(Cursor& c, Index* out);
  bool swizzle(Cursor& c, Operand* o, bool destination);

  Program& p_;
  ParseError& error_;
  uint32_t line_ = 0;
  bool have_header_ = false;
  bool ended_ = false;
};

bool Reader::header(StringView w) {
  struct H {
    const char* name;
    Stage stage;
  };
  static constexpr H kHeaders[] = {{"VERT", Stage::Vertex}, {"FRAG", Stage::Fragment}, {"GEOM", Stage::Geometry},
                                   {"TESS_CTRL", Stage::TessControl}, {"TESS_EVAL", Stage::TessEval}, {"COMP", Stage::Compute}};
  for (const H& h : kHeaders)
    if (StringView(h.name) == w) {
      p_.stage = h.stage;
      have_header_ = true;
      return true;
    }
  return false;
}

bool Reader::property(Cursor& c) {
  NamedProperty np{};
  if (!lookup(kProperties, c.word(), &np)) return fail("unknown PROPERTY");
  const uint32_t at = static_cast<uint32_t>(np.property);
  uint32_t value = 0;
  if (!c.number(&value)) {
    NamedPrimitive prim{};
    StringView w = c.word();
    if (lookup(kPrimitives, w, &prim)) value = prim.value;
    else if (w == StringView("UPPER_LEFT") || w == StringView("HALF_INTEGER")) value = 0;
    else if (w == StringView("LOWER_LEFT") || w == StringView("INTEGER")) value = 1;
    else return fail("PROPERTY value not understood");
  }
  if (!c.done()) return fail("text after the PROPERTY value");
  p_.property[at] = value;
  p_.has_property[at] = true;
  return true;
}

bool Reader::index(Cursor& c, Index* out) {
  // n | FILE[n].c | FILE[n].c+n | FILE[n].c-n | n+FILE[n].c
  *out = Index{};
  int64_t n = 0;
  const uint64_t save = c.position();
  if (c.signed_number(&n)) {
    out->value = static_cast<int32_t>(n);
    if (!c.take('+')) return true;
  } else {
    c.seek(save);
  }
  NamedFile nf{};
  if (!lookup(kFiles, c.word(), &nf)) return fail("bad register index");
  uint32_t reg = 0;
  if (!c.take('[') || !c.number(&reg) || !c.take(']') || !c.take('.')) return fail("bad indirect register");
  const StringView comp = c.word();
  if (comp.size() != 1) return fail("an indirect index takes one component");
  const char ch = comp[0];
  const int k = ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : ch == 'w' ? 3 : -1;
  if (k < 0) return fail("bad indirect component");
  out->indirect = true;
  out->file = nf.file;
  out->index = reg;
  out->component = static_cast<uint8_t>(k);
  if (c.peek('+') || c.peek('-')) {
    const bool minus = c.take('-');
    if (!minus) c.take('+');
    uint32_t add = 0;
    if (!c.number(&add)) return fail("bad index offset");
    out->value += minus ? -static_cast<int32_t>(add) : static_cast<int32_t>(add);
  }
  return true;
}

bool Reader::swizzle(Cursor& c, Operand* o, bool destination) {
  if (!c.take('.')) return true;   // (none: .xyzw)
  const StringView w = c.word();
  if (w.empty() || w.size() > 4) return fail("bad swizzle or mask");
  uint8_t comps[4] = {};
  for (uint64_t i = 0; i < w.size(); ++i) {
    const char ch = w[i];
    const int k = ch == 'x' || ch == 'r' ? 0 : ch == 'y' || ch == 'g' ? 1 : ch == 'z' || ch == 'b' ? 2 : ch == 'w' || ch == 'a' ? 3 : -1;
    if (k < 0) return fail("bad swizzle letter");
    comps[i] = static_cast<uint8_t>(k);
  }
  if (destination) {
    uint8_t mask = 0;
    for (uint64_t i = 0; i < w.size(); ++i) mask |= static_cast<uint8_t>(1u << comps[i]);
    o->mask = mask;
  } else {
    for (uint64_t i = 0; i < 4; ++i) o->swizzle[i] = comps[i < w.size() ? i : w.size() - 1];
  }
  return true;
}

bool Reader::operand(Cursor& c, Operand* o, bool destination) {
  *o = Operand{};
  if (!destination) {
    if (c.take('-')) o->negate = true;
    if (c.take('|')) o->absolute = true;
  }
  NamedFile nf{};
  if (!lookup(kFiles, c.word(), &nf)) return fail("unknown register file");
  o->file = nf.file;
  if (!c.take('[')) return fail("a register needs an index");
  Index first;
  if (!index(c, &first) || !c.take(']')) return fail("bad register index");
  if (c.peek('[')) {
    c.take('[');
    Index second;
    if (!index(c, &second) || !c.take(']')) return fail("bad second register index");
    o->two_dimensional = true;
    o->dimension = first;
    o->index = second;
  } else {
    o->index = first;
  }
  if (c.take('(')) {
    uint32_t array = 0;
    if (!c.number(&array) || !c.take(')')) return fail("bad array number");
    o->array = array;
  }
  if (!swizzle(c, o, destination)) return false;
  if (o->absolute && !c.take('|')) return fail("an absolute value needs its closing |");
  return true;
}

bool Reader::declaration(Cursor& c) {
  Declaration d;
  d.line = line_;
  NamedFile nf{};
  if (!lookup(kFiles, c.word(), &nf)) return fail("DCL of an unknown file");
  d.file = nf.file;
  if (!c.take('[')) return fail("DCL needs a range");
  uint32_t a = 0, b = 0;
  if (c.take(']')) {   // FILE[][a..b]
    d.two_dimensional = true;
    if (!c.take('[')) return fail("DCL FILE[] needs a range");
  } else {
    if (!c.number(&a)) return fail("bad DCL range");
    b = a;
    if (c.take('.')) {
      if (!c.take('.') || !c.number(&b)) return fail("bad DCL range");
    }
    if (!c.take(']')) return fail("bad DCL range");
    if (c.peek('[')) {   // FILE[d][a..b]
      d.dimension = a;
      if (!c.take('[')) return fail("bad DCL range");
    } else {
      d.first = a, d.last = b;
      goto after_range;
    }
  }
  if (!c.number(&a)) return fail("bad DCL range");
  b = a;
  if (c.take('.')) {
    if (!c.take('.') || !c.number(&b)) return fail("bad DCL range");
  }
  if (!c.take(']')) return fail("bad DCL range");
  d.first = a, d.last = b;
after_range:
  if (d.last < d.first) return fail("DCL range ends before it starts");
  while (c.take(',')) {
    const StringView w = c.word();
    NamedSemantic sem{};
    NamedTarget tgt{};
    NamedFormat fmt{};
    if (d.file != File::Sview && d.file != File::Image && lookup(kSemantics, w, &sem)) {
      d.semantic = sem.semantic;
      if (c.take('[')) {
        if (!c.number(&d.semantic_index) || !c.take(']')) return fail("bad semantic index");
      }
    } else if ((d.file == File::Sview || d.file == File::Image) && d.target == Target::None && lookup(kTargets, w, &tgt)) {
      d.target = tgt.target;
    } else if (d.file == File::Sview && w == StringView("FLOAT")) {
      d.return_type = ReturnType::Float;
      while (c.take(',')) c.word();   // (one type per component, all the same in what the producers write)
    } else if (d.file == File::Sview && (w == StringView("UINT") || w == StringView("SINT") || w == StringView("UNORM") || w == StringView("SNORM"))) {
      d.return_type = w == StringView("UINT") ? ReturnType::Uint : w == StringView("SINT") ? ReturnType::Sint : w == StringView("UNORM") ? ReturnType::Unorm : ReturnType::Snorm;
      while (c.take(',')) c.word();
    } else if (d.file == File::Image && lookup(kFormats, w, &fmt)) {
      d.format = fmt.format;
    } else if (w == StringView("WR")) {
      d.writable = true;
    } else if (w == StringView("SHARED")) {
      d.shared = true;
    } else if (w == StringView("ATOMIC")) {
      d.atomic = true;
    } else if (w == StringView("ARRAY")) {
      if (!c.take('(') || !c.number(&d.array) || !c.take(')')) return fail("bad ARRAY");
    } else if (w == StringView("CONSTANT")) {
      d.interpolation = Interpolation::Constant;
    } else if (w == StringView("LINEAR")) {
      d.interpolation = Interpolation::Linear;
    } else if (w == StringView("PERSPECTIVE")) {
      d.interpolation = Interpolation::Perspective;
    } else if (w == StringView("COLOR")) {
      d.interpolation = Interpolation::Color;
    } else if (w == StringView("CENTROID")) {
      d.location = InterpolationLocation::Centroid;
    } else if (w == StringView("SAMPLE")) {
      d.location = InterpolationLocation::Sample;
    } else {
      return fail("unknown DCL attribute");
    }
  }
  if (!c.done()) return fail("text after the declaration");
  if (!p_.declarations.push_back(d)) return fail("out of memory");
  return true;
}

bool Reader::immediate(Cursor& c) {
  uint32_t n = 0;
  if (!c.take('[') || !c.number(&n) || !c.take(']')) return fail("bad IMM");
  if (n != p_.immediates.count()) return fail("immediates out of order");
  Immediate imm;
  const StringView type = c.word();
  if (type == StringView("FLT32")) imm.type = ImmType::Flt32;
  else if (type == StringView("UINT32")) imm.type = ImmType::Uint32;
  else if (type == StringView("INT32")) imm.type = ImmType::Int32;
  else if (type == StringView("FLT64")) imm.type = ImmType::Flt64;
  else return fail("unknown immediate type");
  if (!c.take('{')) return fail("an immediate needs {");
  const uint32_t count = imm.type == ImmType::Flt64 ? 2 : 4;
  for (uint32_t i = 0; i < 4; ++i) {
    if (i && !c.take(',')) {
      if (c.peek('}') && imm.type != ImmType::Flt64) break;   // (fewer values: the rest are zero)
      if (c.peek('}')) break;
      return fail("bad immediate list");
    }
    if (i >= count) return fail("too many immediate values");
    if (imm.type == ImmType::Flt32) {
      if (!c.float_bits(&imm.bits[i])) return fail("bad float immediate");
    } else if (imm.type == ImmType::Flt64) {
      return fail("FLT64 immediates are not written by any producer of this system");
    } else {
      int64_t v = 0;
      if (!c.signed_number(&v) || v < -2147483648ll || v > 4294967295ll) return fail("bad integer immediate");
      imm.bits[i] = static_cast<uint32_t>(v);
    }
  }
  if (!c.take('}') || !c.done()) return fail("bad immediate end");
  if (!p_.immediates.push_back(imm)) return fail("out of memory");
  return true;
}

bool Reader::instruction(Cursor& c) {
  Instruction ins;
  ins.line = line_;
  // [n:] already skipped by the caller
  StringView name = c.word();
  constexpr StringView kSat("_SAT");
  if (name.size() > kSat.size() && name.substr(name.size() - kSat.size(), kSat.size()) == kSat) {
    ins.saturate = true;
    name = name.substr(0, name.size() - kSat.size());
  }
  uint32_t op = 0;
  for (; op < static_cast<uint32_t>(Op::kCount); ++op)
    if (StringView(kOps[op].name) == name) break;
  if (op == static_cast<uint32_t>(Op::kCount)) return fail("unknown opcode");
  ins.op = static_cast<Op>(op);
  const OpInfo& info = kOps[op];
  uint32_t at = 0;   // operands read
  bool after_target = false;
  while (!c.done()) {
    if (at && !c.take(',')) return fail("operands are separated by commas");
    // A target or format word, or an operand.
    const uint64_t save = c.position();
    const StringView w = c.word();
    NamedTarget tgt{};
    NamedFormat fmt{};
    const bool is_register = c.peek('[');   // (BUFFER is a target word and a register file)
    if (!w.empty() && !is_register && lookup(kTargets, w, &tgt) && ins.target == Target::None) {
      ins.target = tgt.target;
      after_target = true;
      ++at;
      continue;
    }
    if (!w.empty() && lookup(kFormats, w, &fmt)) {
      ins.format = fmt.format;
      ++at;
      continue;
    }
    if (w == StringView("COHERENT") || w == StringView("RESTRICT") || w == StringView("VOLATILE")) {   // (memory qualifiers: the device's memory model keeps order)
      ++at;
      continue;
    }
    c.seek(save);
    const bool is_dst = ins.dst_count < info.dsts && ins.src_count == 0 && !after_target;
    if (is_dst) {
      if (!operand(c, &ins.dst[ins.dst_count], true)) return false;
      ++ins.dst_count;
    } else {
      if (ins.src_count >= 6) return fail("too many operands");
      if (!operand(c, &ins.src[ins.src_count], false)) return false;
      ++ins.src_count;
      if (after_target) ++ins.offsets;
    }
    ++at;
  }
  if (ins.dst_count != info.dsts) return fail("missing destination");
  if (ins.op == Op::End) ended_ = true;
  if (!p_.instructions.push_back(ins)) return fail("out of memory");
  return true;
}

bool Reader::run(StringView text) {
  uint64_t start = 0;
  while (start <= text.size()) {
    uint64_t end = start;
    while (end < text.size() && text[end] != '\n' && text[end] != '\0') ++end;
    ++line_;
    StringView line = text.substr(start, end - start);
    const bool nul = end < text.size() && text[end] == '\0';
    start = end + 1;
    Cursor c(line);
    if (c.done()) {
      if (nul) break;
      if (end >= text.size()) break;
      continue;
    }
    if (ended_) return fail("text after END");
    // a label "n:"
    {
      const uint64_t save = c.position();
      uint32_t label = 0;
      if (!(c.number(&label) && c.take(':'))) c.seek(save);
    }
    const uint64_t save = c.position();
    const StringView w = c.word();
    if (!have_header_) {
      if (!header(w) || !c.done()) return fail("TGSI starts with VERT, FRAG, GEOM, TESS_CTRL, TESS_EVAL or COMP");
    } else if (w == StringView("PROPERTY")) {
      if (!property(c)) return false;
    } else if (w == StringView("DCL")) {
      if (!declaration(c)) return false;
    } else if (w == StringView("IMM")) {
      if (!immediate(c)) return false;
    } else {
      c.seek(save);
      if (!instruction(c)) return false;
    }
    if (nul || end >= text.size()) break;
  }
  if (!have_header_) return fail("no TGSI header");
  if (!ended_) return fail("no END");
  return true;
}

}  // namespace

const OpInfo& op_info(Op op) { return kOps[static_cast<uint32_t>(op) < static_cast<uint32_t>(Op::kCount) ? static_cast<uint32_t>(op) : 0]; }

ImageFormat image_format_by_name(StringView name) {
  NamedFormat f{};
  return lookup(kFormats, name, &f) ? f.format : ImageFormat::None;
}

const char* image_format_name(ImageFormat format) {
  for (const NamedFormat& f : kFormats)
    if (f.format == format) return f.name;
  return "PIPE_FORMAT_R8G8B8A8_UNORM";
}

namespace {
// A mark at text[at]: its kind (0 IMGFMT, 1 IMGOFS, 2 IMGLEN, 3 TBOFS, 4 TBLEN), its number, and its length; 0 when there is none.
uint64_t mark_at(StringView text, uint64_t at, uint32_t* kind, uint32_t* index) {
  static constexpr const char* kNames[] = {"IMGFMT", "IMGOFS", "IMGLEN", "TBOFS", "TBLEN"};
  if (at > 0) {
    const char before = text[at - 1];
    if ((before >= 'A' && before <= 'Z') || (before >= 'a' && before <= 'z') || before == '_') return 0;
  }
  for (uint32_t k = 0; k < 5; ++k) {
    const StringView name(kNames[k]);
    if (at + name.size() > text.size() || text.substr(at, name.size()) != name) continue;
    uint64_t end = at + name.size();
    uint32_t n = 0, digits = 0;
    while (end < text.size() && digits < 4 && text[end] >= '0' && text[end] <= '9') n = n * 10 + static_cast<uint32_t>(text[end++] - '0'), ++digits;
    if (!digits) continue;
    *kind = k, *index = n;
    return end - at;
  }
  return 0;
}
}  // namespace

bool has_marks(StringView text) {
  uint32_t kind = 0, index = 0;
  for (uint64_t i = 0; i < text.size(); ++i)
    if (text[i] == 'I' || text[i] == 'T')
      if (mark_at(text, i, &kind, &index)) return true;
  return false;
}

bool substitute_marks(StringView text, const MarkValues& v, ByteBuffer* out) {
  out->clear();
  uint64_t i = 0;
  while (i < text.size()) {
    uint32_t kind = 0, index = 0;
    const uint64_t taken = (text[i] == 'I' || text[i] == 'T') ? mark_at(text, i, &kind, &index) : 0;
    if (!taken) {
      if (!out->put8(static_cast<uint8_t>(text[i]))) return false;
      ++i;
      continue;
    }
    if (kind == 0) {
      const char* name = image_format_name(index < kMarkImages ? v.image_format[index] : ImageFormat::None);
      if (!out->append(ConstByteView(reinterpret_cast<const uint8_t*>(name), StringView(name).size()))) return false;   // (a string's bytes)
    } else {
      const uint32_t value = kind == 1 ? (index < kMarkImages ? v.image_first[index] : 0) : kind == 2 ? (index < kMarkImages ? v.image_count[index] : 0)
                             : kind == 3 ? (index < kMarkTexelViews ? v.texel_first[index] : 0) : (index < kMarkTexelViews ? v.texel_count[index] : 0);
      char digits[12];
      uint32_t n = 0, x = value;
      do digits[n++] = static_cast<char>('0' + x % 10), x /= 10;
      while (x);
      while (n)
        if (!out->put8(static_cast<uint8_t>(digits[--n]))) return false;
    }
    i += taken;
  }
  return true;
}

bool parse(StringView text, Program* out, ParseError* error) {
  ParseError ignored;
  Reader r(*out, error ? *error : ignored);
  return r.run(text);
}

}  // namespace graphics::tgsi
