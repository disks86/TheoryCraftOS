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

// The GLSL ES 1.00 and 3.00 front end: the preprocessor (GlslPreprocessor.cpp), a lexer and a recursive-descent parser that builds a shader::Module.
#include "WebGl/GlslPreprocessor.hpp"
#include "WebGl/ShaderInternal.hpp"

namespace shader {

namespace {

// --- Characters ------------------------------------------------------------------------------------------------------------------------------------------

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

// --- Lexer -----------------------------------------------------------------------------------------------------------------------------------------------

enum class Tok : uint8_t { End, Ident, Int, Uint, Float, Punct, UnitBreak };   // UnitBreak: where one linked shader ends and the next begins (parse_glsl_units)

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
    // desktop GLSL
    {"sampler1D", Base::Sampler, 1, 1, kDim1D, false, Base::Float}, {"sampler1DShadow", Base::Sampler, 1, 1, kDim1D, true, Base::Float},
    {"sampler1DArray", Base::Sampler, 1, 1, kDim1DArray, false, Base::Float}, {"sampler1DArrayShadow", Base::Sampler, 1, 1, kDim1DArray, true, Base::Float},
    {"sampler2DRect", Base::Sampler, 1, 1, kDimRect, false, Base::Float}, {"sampler2DRectShadow", Base::Sampler, 1, 1, kDimRect, true, Base::Float},
    {"isampler1D", Base::Sampler, 1, 1, kDim1D, false, Base::Int}, {"usampler1D", Base::Sampler, 1, 1, kDim1D, false, Base::Uint},
    {"isampler1DArray", Base::Sampler, 1, 1, kDim1DArray, false, Base::Int}, {"usampler1DArray", Base::Sampler, 1, 1, kDim1DArray, false, Base::Uint},
    {"isampler2DRect", Base::Sampler, 1, 1, kDimRect, false, Base::Int}, {"usampler2DRect", Base::Sampler, 1, 1, kDimRect, false, Base::Uint},
    {"samplerCubeArray", Base::Sampler, 1, 1, kDimCubeArray, false, Base::Float}, {"samplerCubeArrayShadow", Base::Sampler, 1, 1, kDimCubeArray, true, Base::Float},
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
    {"EmitVertex", BuiltinFn::EmitVertex}, {"EndPrimitive", BuiltinFn::EndPrimitive}, {"EmitStreamVertex", BuiltinFn::EmitStreamVertex},
    {"EndStreamPrimitive", BuiltinFn::EndStreamPrimitive}, {"barrier", BuiltinFn::Barrier},
    {"dFdx", BuiltinFn::Dfdx}, {"dFdy", BuiltinFn::Dfdy}, {"fwidth", BuiltinFn::Fwidth}, {"dFdxCoarse", BuiltinFn::Dfdx}, {"dFdyCoarse", BuiltinFn::Dfdy},
    {"dFdxFine", BuiltinFn::Dfdx}, {"dFdyFine", BuiltinFn::Dfdy}, {"fwidthCoarse", BuiltinFn::Fwidth}, {"fwidthFine", BuiltinFn::Fwidth},
};

struct TexName {
  const char* name;
  TexOp op;
  uint8_t form;   // 0: (s,P) +opt bias  1: (s,P,lod)  2: (s,P,dx,dy)  3: (s,P,off)+opt bias  4: (s,P,lod,off)  5: (s,P,dx,dy,off)  6: fetch(s,P,lod)  7: fetchOffset  8: size  9: gather  10: levels
  bool legacy_shadow = false;   // shadow1D/shadow2D...: a vec4 result
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
    // desktop GLSL 1.10 - 1.20 (and the compatibility profile)
    {"texture1D", TexOp::Sample, 0}, {"texture1DProj", TexOp::Proj, 0}, {"texture1DLod", TexOp::Lod, 1}, {"texture1DProjLod", TexOp::ProjLod, 1},
    {"texture3DProjLod", TexOp::ProjLod, 1}, {"texture2DRect", TexOp::Sample, 0}, {"texture2DRectProj", TexOp::Proj, 0},
    {"shadow1D", TexOp::Sample, 0, true}, {"shadow2D", TexOp::Sample, 0, true}, {"shadow1DProj", TexOp::Proj, 0, true}, {"shadow2DProj", TexOp::Proj, 0, true},
    {"shadow1DLod", TexOp::Lod, 1, true}, {"shadow2DLod", TexOp::Lod, 1, true}, {"shadow1DProjLod", TexOp::ProjLod, 1, true}, {"shadow2DProjLod", TexOp::ProjLod, 1, true},
    {"shadow2DRect", TexOp::Sample, 0, true}, {"shadow2DRectProj", TexOp::Proj, 0, true},
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
  Parser(Module& m, List<Token>& toks, Stage stage, bool merge_units = false, bool compile_only = false)
      : m_(m), a_(m.allocator), toks_(toks), stage_(stage), syms_(m.allocator), scopes_(m.allocator), merge_units_(merge_units), compile_only_(compile_only) {}

  bool run() {
    prescan_layouts();
    declare_builtin_vars();
    push_scope();
    while (cur().kind != Tok::End && !failed_) top_level();
    end_unit();
    if (!failed_ && merge_units_ && (frag_coord_conflict_ || (any_redeclared_ && any_used_bare_)))
      return err("gl_FragCoord is not redeclared with the same layout in every fragment shader that uses it");
    if (!failed_) finish_unsized();
    pop_scope();
    if (failed_) return false;
    m_.main_function = m_.find_function("main");
    if (compile_only_ && (m_.main_function < 0 || m_.functions[static_cast<uint64_t>(m_.main_function)]->body < 0)) {
      m_.main_function = -1;
      return true;
    }
    if (m_.main_function < 0 || m_.functions[static_cast<uint64_t>(m_.main_function)]->body < 0) return err("no main function");
    if (!compile_only_) {
      // (linking: every function main can reach must be defined in one of the shaders)
      List<bool> reached(a_);
      List<int32_t> work(a_);
      for (uint64_t i = 0; i < m_.functions.count(); ++i) if (!reached.push_back(false)) return err("out of memory");
      reached[static_cast<uint64_t>(m_.main_function)] = true;
      if (!work.push_back(m_.main_function)) return err("out of memory");
      while (!work.empty()) {
        const int32_t fn = work[work.count() - 1];
        work.remove_at(work.count() - 1);
        for (const BodyNodes& b : body_nodes_) {
          if (b.function != fn) continue;
          for (uint64_t k = b.begin; k < b.end && k < m_.nodes.count(); ++k) {
            Node& n = m_.nodes[k];
            if (n.kind != NodeKind::Call || n.ival < 0 || static_cast<uint64_t>(n.ival) >= m_.functions.count()) continue;
            // A prototype only: a definition in another shader that the arguments convert to (int to float, GLSL 1.20 4.1.10) is the one called.
            if (m_.functions[static_cast<uint64_t>(n.ival)]->body < 0 && m_.desktop && m_.desktop_version >= 120) {
              const Function& proto = *m_.functions[static_cast<uint64_t>(n.ival)];
              for (uint64_t c = 0; c < m_.functions.count(); ++c) {
                const Function& def = *m_.functions[c];
                if (def.body < 0 || !def.name.view().equals(proto.name.view()) || def.params.count() != proto.params.count() || !(def.ret == proto.ret)) continue;
                bool converts = true;
                for (uint64_t a = 0; a < def.params.count() && converts; ++a) {
                  const Type& from = proto.params[a].type;
                  const Type& to = def.params[a].type;
                  converts = from == to || (to.base == Base::Float && (from.base == Base::Int || from.base == Base::Uint) && from.rows == to.rows && from.cols == to.cols && from.array == to.array);
                }
                if (converts) { n.ival = static_cast<int32_t>(c); break; }
              }
            }
            const uint64_t callee = static_cast<uint64_t>(n.ival);
            if (m_.functions[callee]->body < 0) {
              String msg(a_);
              msg.assign("function ");
              msg.append(m_.functions[callee]->name.view());
              msg.append(" is called but not defined");
              m_.error.assign(msg.view());
              return false;
            }
            if (!reached[callee]) {
              reached[callee] = true;
              if (!work.push_back(n.ival)) return err("out of memory");
            }
          }
        }
      }
    }
    m_.functions[static_cast<uint64_t>(m_.main_function)]->is_entry = true;
    m_.functions[static_cast<uint64_t>(m_.main_function)]->stage = stage_;
    return true;
  }

 private:
  Module& m_;
  allocators::IAllocator& a_;
  List<Token>& toks_;
  uint8_t default_stream_ = 0, default_xfb_buffer_ = 0;      // `layout(stream = n) out;`, `layout(xfb_buffer = n) out;`
  Stage stage_;
  List<UniquePointer<Sym>> syms_;
  List<uint64_t> scopes_;
  uint64_t pos_ = 0;
  bool failed_ = false;
  int32_t cur_fn_ = -1;
  uint32_t nlocals_ = 0;
  uint32_t anon_ = 0;
  // Unsized global arrays: the highest constant index each is used with (sized at the end, finish_unsized).
  struct Unsized { int32_t global; uint32_t length; };
  List<Unsized> unsized_{a_};
  int32_t interfaces_ = 0;      // in/out interface blocks seen (they are not Module::blocks)
  // Arrayed interface instances (gl_in[], `in Blk {...} v[];`): their number and length.
  struct Arrayed { int32_t iface; uint32_t length; };
  List<Arrayed> arrayed_{a_};
  // The length of a per-vertex array: the input primitive's vertices (geometry), the patch's output vertices (control outputs) or gl_MaxPatchVertices.
  uint32_t per_vertex_length(bool output) const {
    if (stage_ == Stage::Geometry) return m_.input_vertices ? m_.input_vertices : 6;
    if (stage_ == Stage::TessControl && output) return m_.tcs_vertices > 0 ? static_cast<uint32_t>(m_.tcs_vertices) : 32;
    return 32;
  }
  bool is_per_vertex(GlobalKind kind, bool patch) const {
    if (patch) return false;
    if (stage_ == Stage::Geometry) return kind == GlobalKind::In;
    if (stage_ == Stage::TessControl) return kind == GlobalKind::In || kind == GlobalKind::Out;
    if (stage_ == Stage::TessEval) return kind == GlobalKind::In;
    return false;
  }
  uint32_t arrayed_length(int32_t iface) const {
    for (const Arrayed& x : arrayed_) if (x.iface == iface) return x.length;
    return 0;
  }
  bool merge_units_ = false;    // several shaders of the stage in one token stream (parse_glsl_units)
  uint32_t pending_inner_ = 0;  // the inner size of the per-vertex array of arrays being declared
  bool compile_only_ = false;   // GlslOptions::compile_only
  // The nodes each function body added (they are added in order, bodies do not nest): which functions a function calls.
  struct BodyNodes { int32_t function; uint64_t begin, end; };
  // gl_FragCoord in linked fragment shaders (GLSL 1.50, 4.3.8.1): if one redeclares it, every one that uses it must, all with the same layout qualifiers.
  // Which linked shaders declare each function (bit per unit; from the 64th on, they share a bit): a function is visible only where it is declared, so one shader's
  // abs() does not hide the built-in in another.
  uint32_t unit_ = 0;
  List<uint64_t> fn_units_{a_};
  uint64_t unit_bit() const { return uint64_t{1} << (unit_ < 63 ? unit_ : 63); }
  bool visible_here(uint64_t fn) const { return !merge_units_ || (fn < fn_units_.count() && (fn_units_[fn] & unit_bit()) != 0); }
  bool unit_uses_frag_coord_ = false, unit_redeclares_frag_coord_ = false;
  uint8_t unit_frag_coord_ = 0;
  bool any_redeclared_ = false, any_used_bare_ = false;
  int16_t frag_coord_layout_ = -1;      // the qualifiers of the redeclarations so far, -1 none yet
  bool frag_coord_conflict_ = false;
  void end_unit() {
    if (unit_redeclares_frag_coord_) {
      any_redeclared_ = true;
      if (frag_coord_layout_ >= 0 && frag_coord_layout_ != unit_frag_coord_) frag_coord_conflict_ = true;
      frag_coord_layout_ = unit_frag_coord_;
    } else if (unit_uses_frag_coord_) {
      any_used_bare_ = true;
    }
    unit_uses_frag_coord_ = unit_redeclares_frag_coord_ = false;
    unit_frag_coord_ = 0;
  }
  List<BodyNodes> body_nodes_{a_};

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
      case NodeKind::Builtin: {      // min, max, abs of constants (desktop GLSL allows them in constant expressions: array sizes made of gl_Max* limits)
        const BuiltinFn fn = static_cast<BuiltinFn>(n.ival);
        int64_t x, y;
        if (n.count < 1 || !fold_int(m_.kids[static_cast<uint64_t>(n.first)], &x)) return false;
        if (fn == BuiltinFn::Abs && n.count == 1) { *v = x < 0 ? -x : x; return true; }
        if (n.count != 2 || !fold_int(m_.kids[static_cast<uint64_t>(n.first + 1)], &y)) return false;
        if (fn == BuiltinFn::Min) { *v = x < y ? x : y; return true; }
        if (fn == BuiltinFn::Max) { *v = x > y ? x : y; return true; }
        return false;
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
  bool is_qualifier_word(StringView w) const {
    static const char* const q[] = {"const", "uniform", "attribute", "varying", "in", "out", "inout", "flat", "smooth", "centroid", "noperspective", "layout"};
    for (const char* x : q) if (w.equals(x)) return true;
    return false;
  }
  bool at_type_at(uint64_t ahead) const {
    const Token& t = peek(ahead);
    if (t.kind != Tok::Ident) return false;
    Type unused;
    return find_type_name(t.text, &unused);
  }

  bool at_qualifier() const {
    if (cur().kind != Tok::Ident) return false;
    static const char* const q[] = {"const", "uniform", "attribute", "varying", "in", "out", "inout", "highp", "mediump", "lowp", "flat", "smooth", "centroid", "invariant", "noperspective", "sample", "precise", "patch", "layout", "precision", "shared", "readonly", "writeonly", "coherent", "volatile", "restrict"};
    for (const char* w : q)
      if (cur().text.equals(w)) return true;
    return false;
  }

  // `[n]`, `[]` (and with `one`, only the first of several).
  bool parse_array_suffix(Type* t, bool one = false) {
    while (accept("[")) {
      if (is("]")) {
        ++pos_;
        t->array = kUnsizedArray;   // unsized: the initializer, a redeclaration or the indices used say
        if (one) break;
        continue;
      }
      const int32_t e = expression();
      int64_t v;
      if (e < 0 || !fold_int(e, &v) || v <= 0) return err("array size must be a positive constant");
      if (v > static_cast<int64_t>(kMaxArrayLength)) return err("array too large");
      if (!expect("]")) return false;
      t->array = static_cast<uint32_t>(v);
      if (one) break;
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
    if (stage_ == Stage::Geometry || stage_ == Stage::TessControl || stage_ == Stage::TessEval) {
      declare_primitive_stage_builtins();
    } else if (stage_ == Stage::Vertex) {
      add_builtin("gl_Position", f4, GlobalKind::Out, kBvPosition);
      add_builtin("gl_PointSize", f1, GlobalKind::Out, kBvPointSize);
      add_builtin("gl_VertexID", i1, GlobalKind::In, kBvVertexIndex);
      add_builtin("gl_InstanceID", i1, GlobalKind::In, kBvInstanceIndex);
      if (m_.desktop) {
        Type clip = f1;
        clip.array = 8;
        add_builtin("gl_ClipDistance", clip, GlobalKind::Out, kBvClipDistance);
        // (gl_ClipVertex: written, clipping by it is the fixed-function clip planes' job, not done for programs yet)
        if (m_.compatibility) { const int32_t g = new_global("gl_ClipVertex", f4, GlobalKind::Private, false); (void)g; }
      }
    } else {
      add_builtin("gl_FragCoord", f4, GlobalKind::In, kBvFragCoord);
      add_builtin("gl_FrontFacing", b1, GlobalKind::In, kBvFrontFacing);
      add_builtin("gl_PointCoord", f2, GlobalKind::In, kBvPointCoord);
      add_builtin("gl_FragColor", f4, GlobalKind::Out, kBvFragColor);
      add_builtin("gl_FragData", frag_data, GlobalKind::Out, kBvFragData);
      add_builtin("gl_FragDepth", f1, GlobalKind::Out, kBvFragDepth);
      add_builtin("gl_FragDepthEXT", f1, GlobalKind::Out, kBvFragDepth);
      if (m_.desktop) {
        const int32_t g = new_global("gl_PrimitiveID", i1, GlobalKind::Private, false);      // (from the geometry stage, else the draw counts primitives: FragmentPipeline)
        if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->is_read_only = true;
        Type clip = f1;
        clip.array = 8;
        add_builtin("gl_ClipDistance", clip, GlobalKind::In, kBvClipDistance);
      }
    }
    const struct { const char* name; int v; } consts[] = {{"gl_MaxVertexAttribs", 16}, {"gl_MaxDrawBuffers", 4}, {"gl_MaxVertexUniformVectors", 1024},
                                                          {"gl_MaxFragmentUniformVectors", 1024}, {"gl_MaxVaryingVectors", 30}, {"gl_MaxTextureImageUnits", 14}};
    // What desktop GLSL adds (the values the kernel's OpenGL reports through glGet).
    const struct { const char* name; int v; } desktop_consts[] = {{"gl_MaxLights", 8}, {"gl_MaxClipPlanes", 8}, {"gl_MaxClipDistances", 8}, {"gl_MaxTextureUnits", 8}, {"gl_MaxTextureCoords", 8},
                                                                  {"gl_MaxVertexUniformComponents", 16384}, {"gl_MaxFragmentUniformComponents", 16384}, {"gl_MaxVaryingFloats", 64},
                                                                  {"gl_MaxVaryingComponents", 64}, {"gl_MaxVertexTextureImageUnits", 14}, {"gl_MaxCombinedTextureImageUnits", 14},
                                                                  {"gl_MaxVertexOutputComponents", 64}, {"gl_MaxFragmentInputComponents", 64}};
    // geometry and tessellation limits (the values glGet reports)
    const struct { const char* name; int v; } stage_consts[] = {{"gl_MaxGeometryInputComponents", 64}, {"gl_MaxGeometryOutputComponents", 128}, {"gl_MaxGeometryTextureImageUnits", 16},
                                                                {"gl_MaxGeometryOutputVertices", 256}, {"gl_MaxGeometryTotalOutputComponents", 1024}, {"gl_MaxGeometryUniformComponents", 1024},
                                                                {"gl_MaxGeometryVaryingComponents", 64}, {"gl_MaxPatchVertices", 32}, {"gl_MaxTessGenLevel", 64},
                                                                {"gl_MaxTessControlInputComponents", 128}, {"gl_MaxTessControlOutputComponents", 128},
                                                                {"gl_MaxTessControlTextureImageUnits", 16}, {"gl_MaxTessControlUniformComponents", 1024},
                                                                {"gl_MaxTessControlTotalOutputComponents", 4096}, {"gl_MaxTessEvaluationInputComponents", 128},
                                                                {"gl_MaxTessEvaluationOutputComponents", 128}, {"gl_MaxTessEvaluationTextureImageUnits", 16},
                                                                {"gl_MaxTessEvaluationUniformComponents", 1024}, {"gl_MaxTessPatchComponents", 120}};
    if (m_.desktop && m_.desktop_version >= 150)
      for (const auto& c : stage_consts) {
        const int32_t n = int_node(c.v);
        const int32_t g = new_global(c.name, i1, GlobalKind::Const, false);
        if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->init = n;
      }
    if (m_.desktop)
      for (const auto& c : desktop_consts) {
        const int32_t n = int_node(c.v);
        const int32_t g = new_global(c.name, i1, GlobalKind::Const, false);
        if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->init = n;
      }
    for (const auto& c : consts) {
      const int32_t n = int_node(c.v);
      const int32_t g = new_global(c.name, i1, GlobalKind::Const, false);
      if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->init = n;
    }
  }
  // A value the host sets before each invocation (gl_InvocationID, gl_PrimitiveIDIn, gl_TessCoord ...): private, read-only.
  int32_t system_value(const char* name, const Type& t) {
    const int32_t g = new_global(name, t, GlobalKind::Private, false);
    if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->is_read_only = true;
    return g;
  }
  // The members of gl_in[] / gl_out[]: per-vertex arrays named gl_in.gl_Position ..., reached as gl_in[i].gl_Position (a symbol of kind 4).
  // Whether the shaders name an identifier anywhere.
  bool mentions(StringView name) const {
    for (const Token& t : toks_) if (t.kind == Tok::Ident && t.text.equals(name)) return true;
    return false;
  }
  void per_vertex_block(const char* instance, GlobalKind kind, uint32_t count, bool shared) {
    const int32_t iface = interfaces_++;
    // (the compatibility profile adds gl_ClipVertex and the colours, texture coordinates and fog coordinate; the desktop translation names those tcos_*, and only the
    // ones the shaders name are made, as each is an array of every vertex)
    const char* const members[] = {"gl_Position", "gl_PointSize", "gl_ClipDistance", "gl_ClipVertex", "tcos_FrontColor", "tcos_BackColor", "tcos_FrontSecondaryColor",
                                   "tcos_BackSecondaryColor", "tcos_TexCoord", "tcos_FogFragCoord"};
    const Type f1 = make_type(Base::Float), f4 = make_type(Base::Float, 4);
    const Type types[] = {f4, f1, f1, f4, f4, f4, f4, f4, f4, f1};
    const uint32_t inners[] = {0, 0, 8, 0, 0, 0, 0, 0, 8, 0};      // (gl_ClipDistance, gl_TexCoord: 8 per vertex)
    for (int i = 0; i < (m_.compatibility ? 10 : 3); ++i) {
      if (i >= 4 && !mentions(StringView(members[i]))) continue;
      String name(a_);
      name.assign(instance);
      name.push_back('.');
      name.append(members[i]);
      Type t = types[i];
      t.array = count * (inners[i] ? inners[i] : 1);
      const int32_t g = new_global(name.view(), t, kind, shared);
      if (g < 0) return;
      Global& gv = *m_.globals[static_cast<uint64_t>(g)];
      gv.iface = iface;
      gv.inner = inners[i];
      gv.per_vertex = true;
      gv.is_read_only = kind == GlobalKind::In;
    }
    add_sym(instance, make_type(Base::Void), 4, iface);
    if (!arrayed_.push_back(Arrayed{iface, count})) err("out of memory");
  }
  void declare_primitive_stage_builtins() {
    const Type f1 = make_type(Base::Float), f4 = make_type(Base::Float, 4), i1 = make_type(Base::Int), f3 = make_type(Base::Float, 3);
    Type clip = f1;
    clip.array = 8;
    if (stage_ == Stage::Geometry) {
      per_vertex_block("gl_in", GlobalKind::In, m_.input_vertices ? m_.input_vertices : 6, false);
      system_value("gl_PrimitiveIDIn", i1);
      system_value("gl_InvocationID", i1);
    } else {
      per_vertex_block("gl_in", GlobalKind::In, 32, false);
      system_value("gl_PatchVerticesIn", i1);
      system_value("gl_PrimitiveID", i1);
    }
    if (stage_ == Stage::TessControl) {
      system_value("gl_InvocationID", i1);
      // The outputs are shared by the patch's invocations (they read each other's after barrier()): in the shared region, which the host gives each patch.
      per_vertex_block("gl_out", GlobalKind::Out, m_.tcs_vertices > 0 ? static_cast<uint32_t>(m_.tcs_vertices) : 32, true);
      Type outer = f1, inner = f1;
      outer.array = 4;
      inner.array = 2;
      const struct { const char* name; Type type; } levels[] = {{"gl_TessLevelOuter", outer}, {"gl_TessLevelInner", inner}};
      for (const auto& lv : levels) {
        const int32_t g = new_global(lv.name, lv.type, GlobalKind::Out, true);
        if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->patch = true;
      }
      return;
    }
    if (stage_ == Stage::TessEval) {
      system_value("gl_TessCoord", f3);
      Type outer = f1, inner = f1;
      outer.array = 4;
      inner.array = 2;
      const struct { const char* name; Type type; } levels[] = {{"gl_TessLevelOuter", outer}, {"gl_TessLevelInner", inner}};
      for (const auto& lv : levels) {
        const int32_t g = new_global(lv.name, lv.type, GlobalKind::In, false);
        if (g >= 0) {
          m_.globals[static_cast<uint64_t>(g)]->patch = true;
          m_.globals[static_cast<uint64_t>(g)]->is_read_only = true;
        }
      }
    }
    // The vertex the stage puts out (the geometry stage, per EmitVertex(); the evaluation stage, per tessellated vertex).
    add_builtin("gl_Position", f4, GlobalKind::Out, kBvPosition);
    add_builtin("gl_PointSize", f1, GlobalKind::Out, kBvPointSize);
    add_builtin("gl_ClipDistance", clip, GlobalKind::Out, kBvClipDistance);
    // (compatibility: the eye-space vertex the clip planes are applied to)
    if (m_.compatibility && stage_ != Stage::TessControl) new_global("gl_ClipVertex", f4, GlobalKind::Private, false);
    if (stage_ == Stage::Geometry) {
      new_global("gl_PrimitiveID", i1, GlobalKind::Out, false);
      new_global("gl_Layer", i1, GlobalKind::Out, false);
      new_global("gl_ViewportIndex", i1, GlobalKind::Out, false);
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

  // Room for a global in its region; false (an error) if the module would need more than any shader may.
  bool place_global(Global& g) {
    const uint32_t n = m_.slots_of(g.type);
    uint32_t& top = g.shared ? m_.shared_slots : m_.private_slots;
    if (n >= kMaxValueSlots || top > kMaxValueSlots - n) return err("the shader's variables are too large");
    g.slot = top;
    top += n;
    return true;
  }

  // Whether two expressions are written the same (operators, constants, variables, calls): initializers of one global in linked shaders.
  bool same_expression(int32_t x, int32_t y, uint32_t depth) const {
    if (x == y) return true;
    if (x < 0 || y < 0 || depth > 64) return false;
    const Node& a = m_.nodes[static_cast<uint64_t>(x)];
    const Node& b = m_.nodes[static_cast<uint64_t>(y)];
    if (a.kind != b.kind || a.op != b.op || a.ival != b.ival || a.count != b.count || a.flags != b.flags || !(a.type == b.type)) return false;
    if (a.kind == NodeKind::Const) {
      const uint32_t n = m_.slots_of(a.type);
      if (a.a < 0 || b.a < 0 || n > m_.consts.count() || static_cast<uint64_t>(a.a) > m_.consts.count() - n || static_cast<uint64_t>(b.a) > m_.consts.count() - n) return false;
      for (uint32_t k = 0; k < n; ++k) if (m_.consts[static_cast<uint64_t>(a.a) + k] != m_.consts[static_cast<uint64_t>(b.a) + k]) return false;
      return true;
    }
    if (!same_expression(a.a, b.a, depth + 1) || !same_expression(a.b, b.b, depth + 1) || !same_expression(a.c, b.c, depth + 1) || !same_expression(a.d, b.d, depth + 1)) return false;
    for (int32_t k = 0; k < a.count; ++k) {
      const uint64_t ka = static_cast<uint64_t>(a.first) + static_cast<uint64_t>(k), kb = static_cast<uint64_t>(b.first) + static_cast<uint64_t>(k);
      if (ka >= m_.kids.count() || kb >= m_.kids.count() || !same_expression(m_.kids[ka], m_.kids[kb], depth + 1)) return false;
    }
    return true;
  }

  // Whether an expression uses only constants (literals, const globals, constructors, operators and built-in functions of them).
  bool constant_expression(int32_t x, uint32_t depth) const {
    if (x < 0) return true;
    if (depth > 64) return false;
    const Node& n = m_.nodes[static_cast<uint64_t>(x)];
    if (n.kind == NodeKind::Local || n.kind == NodeKind::Call || n.kind == NodeKind::TexCall) return false;
    if (n.kind == NodeKind::Global) return n.ival >= 0 && static_cast<uint64_t>(n.ival) < m_.globals.count() && m_.globals[static_cast<uint64_t>(n.ival)]->kind == GlobalKind::Const;
    if (!constant_expression(n.a, depth + 1) || !constant_expression(n.b, depth + 1) || !constant_expression(n.c, depth + 1) || !constant_expression(n.d, depth + 1)) return false;
    for (int32_t k = 0; k < n.count; ++k) {
      const uint64_t kk = static_cast<uint64_t>(n.first) + static_cast<uint64_t>(k);
      if (kk >= m_.kids.count() || !constant_expression(m_.kids[kk], depth + 1)) return false;
    }
    return true;
  }

  int32_t new_global(StringView name, const Type& t, GlobalKind kind, bool shared) {
    UniquePointer<Global> g = UniquePointer<Global>::make(a_, a_);
    if (!g) { err("out of memory"); return -1; }
    g->name.assign(name);
    g->type = t;
    g->kind = kind;
    g->shared = shared;
    // An unsized array gets its room once its size is known (finish_unsized), at the end.
    if (t.array != kUnsizedArray && !place_global(*g)) return -1;
    m_.globals.push_back(static_cast<UniquePointer<Global>&&>(g));
    const int32_t idx = static_cast<int32_t>(m_.globals.count() - 1);
    if (t.array == kUnsizedArray && !unsized_.push_back(Unsized{idx, 0})) { err("out of memory"); return -1; }
    add_sym(name, t, 1, idx);
    return idx;
  }

  struct UnsizedLocal { int32_t local; int32_t decl; uint32_t length; };
  List<UnsizedLocal> unsized_locals_{a_};
  UnsizedLocal* unsized_local_of(int32_t local) {
    for (UnsizedLocal& u : unsized_locals_) if (u.local == local) return &u;
    return nullptr;
  }
  void finish_unsized_locals() {
    for (const UnsizedLocal& u : unsized_locals_) {
      Node& d = m_.nodes[static_cast<uint64_t>(u.decl)];
      if (d.type.array == kUnsizedArray) d.type.array = u.length ? u.length : 1;
    }
    unsized_locals_.clear();
  }

  Unsized* unsized_of(int32_t global) {
    for (Unsized& u : unsized_) if (u.global == global) return &u;
    return nullptr;
  }
  bool finish_unsized() {
    for (const Unsized& u : unsized_) {
      Global& g = *m_.globals[static_cast<uint64_t>(u.global)];
      if (g.type.array != kUnsizedArray) continue;      // (redeclared with a size)
      g.type.array = u.length ? u.length : 1;
      if (!place_global(g)) return false;
      if (Sym* s = find_sym(g.name.view())) if (s->kind == 1 && s->index == u.global) s->type = g.type;
    }
    return true;
  }

  struct Quals {
    bool is_const = false, uniform = false, attribute = false, varying = false, in = false, out = false, inout = false, flat = false, layout_std140 = false;
    uint8_t frag_coord = 0;      // layout(origin_upper_left) 1, layout(pixel_center_integer) 2
    int32_t location = -1, binding = -1;
    bool patch = false;          // tessellation: per patch
    // layout declarations of the geometry and tessellation stages (`layout(triangles) in;` ...)
    uint32_t prim = 0xFFFFFFFFu;
    int32_t max_vertices = -1, invocations = -1, vertices = -1, stream = -1;
    int32_t xfb_buffer = -1, xfb_offset = -1, xfb_stride = -1;      // (GLSL 4.40, ARB_enhanced_layouts)
    uint32_t spacing = 0, order = 0;
    bool point_mode = false;
  };

  // The keys of the geometry and tessellation layouts (others are accepted and ignored, as before).
  bool layout_stage_key(StringView key, int32_t value, Quals* q) {
    struct K { const char* name; uint32_t prim; };
    static const K kPrims[] = {{"points", 0}, {"lines", 1}, {"lines_adjacency", 0xA}, {"triangles", 4}, {"triangles_adjacency", 0xC}, {"line_strip", 3}, {"triangle_strip", 5},
                               {"quads", 7}, {"isolines", 0x8E7A}};
    for (const K& k : kPrims) if (key.equals(k.name)) { q->prim = k.prim; return true; }
    if (key.equals("max_vertices")) q->max_vertices = value;
    else if (key.equals("invocations")) { if (value < 1 || value > 32) return err("bad number of geometry shader invocations"); q->invocations = value; }
    else if (key.equals("vertices")) { if (value < 1 || value > 32) return err("bad number of patch vertices"); q->vertices = value; }
    else if (key.equals("stream")) { if (value < 0 || value > 3) return err("bad vertex stream"); q->stream = value; }
    else if (key.equals("xfb_buffer")) { if (value < 0 || value > 3) return err("bad transform feedback buffer"); q->xfb_buffer = value; }
    else if (key.equals("xfb_offset")) { if (value % 4) return err("a transform feedback offset must be a multiple of 4"); q->xfb_offset = value; }
    else if (key.equals("xfb_stride")) { if (value % 4 || value > 4096) return err("bad transform feedback stride"); q->xfb_stride = value; }
    else if (key.equals("equal_spacing")) q->spacing = 0x0202;
    else if (key.equals("fractional_even_spacing")) q->spacing = 0x8E7C;
    else if (key.equals("fractional_odd_spacing")) q->spacing = 0x8E7B;
    else if (key.equals("cw")) q->order = 0x0900;
    else if (key.equals("ccw")) q->order = 0x0901;
    else if (key.equals("point_mode")) q->point_mode = true;
    return true;
  }

  // Vertices of a geometry shader's input primitive.
  static uint32_t primitive_vertices(uint32_t prim) {
    switch (prim) {
      case 0: return 1;
      case 1: return 2;
      case 0xA: return 4;
      case 4: return 3;
      case 0xC: return 6;
      default: return 0;
    }
  }

  bool set_stride(int32_t buffer, int32_t stride) {
    if (m_.xfb_stride[buffer & 3] >= 0 && m_.xfb_stride[buffer & 3] != stride) return err("transform feedback strides conflict");
    m_.xfb_stride[buffer & 3] = stride;
    return true;
  }
  // An output's stream and transform feedback capture, from its own qualifiers, its block's, or the defaults; `next`: the offset a block's next member takes (-1 none).
  bool set_capture(Global& gv, const Quals& q, const Quals* block, int32_t* next) {
    const int32_t stream = q.stream >= 0 ? q.stream : (block && block->stream >= 0 ? block->stream : default_stream_);
    if (stream && stage_ != Stage::Geometry) return err("only geometry shader outputs have streams");
    gv.stream = static_cast<uint8_t>(stream);
    if (stream) m_.gs_streams = true;
    const int32_t buffer = q.xfb_buffer >= 0 ? q.xfb_buffer : (block && block->xfb_buffer >= 0 ? block->xfb_buffer : default_xfb_buffer_);
    if (q.xfb_stride >= 0 && !set_stride(buffer, q.xfb_stride)) return false;
    int32_t offset = q.xfb_offset;
    if (offset < 0 && next && *next >= 0) offset = *next;
    if (offset < 0) return true;
    const uint32_t bytes = m_.slots_of(gv.type) * 4;
    if (bytes > 0x10000 || static_cast<uint32_t>(offset) > 0x10000) return err("transform feedback offset too large");
    gv.xfb_buffer = static_cast<int8_t>(buffer);
    gv.xfb_offset = offset;
    m_.has_xfb = true;
    if (next) *next = offset + static_cast<int32_t>(bytes);
    return true;
  }

  // `layout(...) in;` / `layout(...) out;` with nothing declared: the stage's layout.
  void apply_layout(const Quals& q) {
    const auto conflict = [&](const char* what) { err(what); };
    // `layout(stream = 1) out;`, `layout(xfb_buffer = 1, xfb_stride = 32) out;`: the defaults of the declarations after it, and a buffer's stride
    if (q.out) {
      if (q.stream >= 0) default_stream_ = static_cast<uint8_t>(q.stream);
      if (q.xfb_buffer >= 0) default_xfb_buffer_ = static_cast<uint8_t>(q.xfb_buffer);
      if (q.xfb_stride >= 0 && !set_stride(q.xfb_buffer >= 0 ? q.xfb_buffer : default_xfb_buffer_, q.xfb_stride)) return;
    }
    if (stage_ == Stage::Geometry && q.in) {
      if (q.prim != 0xFFFFFFFFu) {
        if (!primitive_vertices(q.prim)) return conflict("bad geometry shader input primitive");
        if (m_.gs_input != 0xFFFFFFFFu && m_.gs_input != q.prim) return conflict("geometry shader input primitives conflict");
        m_.gs_input = q.prim;
      }
      if (q.invocations > 0) {
        if (m_.gs_invocations != 1 && m_.gs_invocations != static_cast<uint32_t>(q.invocations)) return conflict("geometry shader invocation counts conflict");
        m_.gs_invocations = static_cast<uint32_t>(q.invocations);
      }
    } else if (stage_ == Stage::Geometry && q.out) {
      if (q.prim != 0xFFFFFFFFu) {
        if (q.prim != 0 && q.prim != 3 && q.prim != 5) return conflict("bad geometry shader output primitive");
        if (m_.gs_output != 0xFFFFFFFFu && m_.gs_output != q.prim) return conflict("geometry shader output primitives conflict");
        m_.gs_output = q.prim;
      }
      if (q.max_vertices >= 0) {
        if (q.max_vertices > 256) return conflict("max_vertices is more than gl_MaxGeometryOutputVertices");
        if (m_.gs_max_vertices >= 0 && m_.gs_max_vertices != q.max_vertices) return conflict("max_vertices values conflict");
        m_.gs_max_vertices = q.max_vertices;
      }
    } else if (stage_ == Stage::TessControl && q.out) {
      if (q.vertices > 0) {
        if (m_.tcs_vertices >= 0 && m_.tcs_vertices != q.vertices) return conflict("patch vertex counts conflict");
        m_.tcs_vertices = q.vertices;
      }
    } else if (stage_ == Stage::TessEval && q.in) {
      if (q.prim != 0xFFFFFFFFu) {
        if (q.prim != 4 && q.prim != 7 && q.prim != 0x8E7A) return conflict("bad tessellation primitive mode");
        if (m_.tes_mode && m_.tes_mode != q.prim) return conflict("tessellation primitive modes conflict");
        m_.tes_mode = q.prim;
      }
      if (q.spacing) {
        if (m_.tes_spacing && m_.tes_spacing != q.spacing) return conflict("tessellation spacings conflict");
        m_.tes_spacing = q.spacing;
      }
      if (q.order) {
        if (m_.tes_order && m_.tes_order != q.order) return conflict("tessellation vertex orders conflict");
        m_.tes_order = q.order;
      }
      if (q.point_mode) m_.tes_point_mode = true;
    }
  }

  // Before the built-ins are declared: the sizes of the per-vertex arrays (gl_in[], gl_out[]) from the layouts, wherever they are in the shaders.
  void prescan_layouts() {
    if (stage_ == Stage::TessControl || stage_ == Stage::TessEval) m_.input_vertices = 32;
    uint32_t depth = 0;
    for (uint64_t i = 0; i + 3 < toks_.count(); ++i) {
      const Token& t = toks_[i];
      if (t.kind == Tok::Punct && t.text.equals("{")) ++depth;
      else if (t.kind == Tok::Punct && t.text.equals("}") && depth) --depth;
      if (depth || t.kind != Tok::Ident || !t.text.equals("layout") || !toks_[i + 1].text.equals("(")) continue;
      uint64_t k = i + 2;
      uint32_t prim = 0xFFFFFFFFu;
      int32_t vertices = -1;
      for (; k < toks_.count() && !toks_[k].text.equals(")"); ++k) {
        if (toks_[k].kind != Tok::Ident) continue;
        Quals q;
        if (toks_[k].text.equals("vertices") && k + 2 < toks_.count() && toks_[k + 2].kind == Tok::Int) vertices = static_cast<int32_t>(toks_[k + 2].ival);
        else if (layout_stage_key(toks_[k].text, 0, &q) && q.prim != 0xFFFFFFFFu) prim = q.prim;
      }
      if (k + 2 >= toks_.count() || toks_[k + 2].kind != Tok::Punct || !toks_[k + 2].text.equals(";")) continue;
      const StringView dir = toks_[k + 1].text;
      if (stage_ == Stage::Geometry && dir.equals("in") && primitive_vertices(prim)) m_.input_vertices = primitive_vertices(prim);
      if (stage_ == Stage::TessControl && dir.equals("out") && vertices > 0 && vertices <= 32) m_.tcs_vertices = vertices;
    }
  }

  bool parse_layout(Quals* q) {
    if (!expect("(")) return false;
    while (!is(")") && !failed_) {
      if (cur().kind != Tok::Ident) return err("bad layout qualifier");
      const StringView key = cur().text;
      ++pos_;
      int32_t value = 0;
      if (accept("=")) {
        if (cur().kind == Tok::Int && peek().kind == Tok::Punct && (peek().text.equals(",") || peek().text.equals(")"))) {
          value = static_cast<int32_t>(cur().ival);
          ++pos_;
        } else {      // (a constant expression: `max_vertices = N * 3`)
          const int32_t e = assignment();
          int64_t v;
          if (e < 0 || !fold_int(e, &v) || v < 0 || v > 0x7FFFFFFF) return err("bad layout value");
          value = static_cast<int32_t>(v);
        }
      }
      if (key.equals("location")) q->location = value;
      else if (key.equals("binding")) q->binding = value;
      else if (key.equals("std140")) q->layout_std140 = true;
      else if (key.equals("origin_upper_left")) q->frag_coord |= 1;
      else if (key.equals("pixel_center_integer")) q->frag_coord |= 2;
      else if (!layout_stage_key(key, value, q)) return false;
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
      else if (w.equals("patch")) q->patch = true;
      else if (w.equals("layout")) { if (!parse_layout(q)) return false; }
    }
    return !failed_;
  }

  void top_level() {
    if (cur().kind == Tok::UnitBreak) {
      end_unit();
      ++unit_;
      ++pos_;
      return;
    }
    if (accept(";")) return;
    if (is_id("invariant") && peek().kind == Tok::Ident && !at_type_at(1) && !is_qualifier_word(peek().text)) {
      // `invariant gl_Position, v;`: every value is computed the same way here already
      ++pos_;
      while (cur().kind == Tok::Ident && !failed_) {
        if (!find_sym(cur().text)) { err("invariant names an undeclared variable"); return; }
        ++pos_;
        if (!accept(",")) break;
      }
      expect(";");
      return;
    }
    if (is_id("precision")) {
      while (!is(";") && cur().kind != Tok::End) ++pos_;
      expect(";");
      return;
    }
    if (is_id("struct")) {
      Type st;
      struct_decl(&st);
      if (!failed_) {
        // a declaration like `struct S {...} s;` or just `struct S {...};`
        global_declarators_after_struct(st);
      }
      return;
    }
    Quals q;
    if (!parse_quals(&q)) return;
    if (is(";") && (q.in || q.out || q.uniform)) {      // `layout(triangles) in;`
      ++pos_;
      apply_layout(q);
      return;
    }
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
      // (`in vec4 v[][2]`: per vertex, an array of two; the first size, then the second)
      pending_inner_ = 0;
      if (is("[")) {
        if (!parse_array_suffix(&vt, true)) return;
        if (is("[")) {
          const uint32_t outer = vt.array;
          Type in = t;
          if (!parse_array_suffix(&in) || in.array == kUnsizedArray) { err("the inner array needs a size"); return; }
          pending_inner_ = in.array;
          vt.array = outer;
        }
      }
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
    // `float a[];` then `float a[4];`: the second gives the first its size (it must cover the indices used so far).
    if (t.array && t.array != kUnsizedArray) {
      const int32_t old = m_.find_global(name);
      if (old >= 0) {
        Global& og = *m_.globals[static_cast<uint64_t>(old)];
        Unsized* u = unsized_of(old);
        if (u && og.type.array == kUnsizedArray && og.type.element() == t.element()) {
          if (u->length > t.array) { err("array redeclared smaller than an index used"); return; }
          og.type.array = t.array;
          if (!place_global(og)) return;
          if (Sym* s = find_sym(name)) if (s->kind == 1 && s->index == old) s->type = og.type;
          return;
        }
      }
    }
    GlobalKind kind = GlobalKind::Private;
    bool shared = false;
    if (q.is_const && !q.uniform) kind = GlobalKind::Const;
    else if (q.uniform) { kind = GlobalKind::Uniform; shared = true; }
    else if (q.attribute) kind = GlobalKind::In;
    // (`varying` in a geometry or evaluation shader of the compatibility profile: an output, as the stage's vertex goes on to the fragment stage)
    else if (q.varying) kind = stage_ == Stage::Vertex || stage_ == Stage::Geometry || stage_ == Stage::TessEval ? GlobalKind::Out : GlobalKind::In;
    else if (q.in) kind = GlobalKind::In;
    else if (q.out) kind = GlobalKind::Out;
    // Geometry and tessellation stages: per-vertex inputs (and control outputs) are arrays, one element per vertex; control outputs are shared by the patch's invocations.
    const bool per_vertex = !name.starts_with("gl_") && is_per_vertex(kind, q.patch);
    const uint32_t inner = pending_inner_;
    pending_inner_ = 0;
    if (inner && !per_vertex) { err("arrays of arrays are only supported as per-vertex varyings"); return; }
    if (per_vertex) {
      const uint32_t len = per_vertex_length(kind == GlobalKind::Out);
      if (!t.array) { err(stage_ == Stage::Geometry ? "geometry shader inputs must be arrays" : "tessellation shader per-vertex varyings must be arrays"); return; }
      if (t.array == kUnsizedArray) t.array = len;
      else if (stage_ == Stage::Geometry && m_.input_vertices && t.array != len) { err("the array size of a geometry shader input does not match the input primitive"); return; }
      else if (stage_ == Stage::TessControl && kind == GlobalKind::Out && m_.tcs_vertices > 0 && t.array != len) { err("the array size of a tessellation control output does not match the patch's vertices"); return; }
      if (inner) {
        if (t.array > kMaxArrayLength / inner) { err("array too large"); return; }
        t.array *= inner;
      }
    }
    if (stage_ == Stage::TessControl && kind == GlobalKind::Out) shared = true;
    // A redeclared built-in (`layout(origin_upper_left) in vec4 gl_FragCoord;`, `out vec4 gl_Position;`): the built-in it is.
    if (name.starts_with("gl_")) {
      const int32_t old = m_.find_global(name);
      // (arrays such as gl_ClipDistance[3] and gl_TexCoord[] are redeclared with a size, which the declaration below gives them)
      if (old >= 0 && m_.globals[static_cast<uint64_t>(old)]->builtin != kBvNone && !t.array) {
        if (q.out) set_capture(*m_.globals[static_cast<uint64_t>(old)], q, nullptr, nullptr);      // (`layout(xfb_offset = 0) out vec4 gl_Position;`)
        if (m_.globals[static_cast<uint64_t>(old)]->builtin == kBvFragCoord) {
          unit_redeclares_frag_coord_ = true;
          unit_frag_coord_ = q.frag_coord;
        }
        return;
      }
    }
    // Linked shaders that declare the same variable: one variable (GLSL 1.10, 4.1.9 ... "global variables ... must have the same type in every shader").
    if (merge_units_) {
      const int32_t old = m_.find_global(name);
      if (old >= 0 && m_.globals[static_cast<uint64_t>(old)]->builtin == kBvNone) {
        Global& og = *m_.globals[static_cast<uint64_t>(old)];
        // (`vec4 v[2];` in one shader and `vec4 v[];` in another: one array of 2)
        if (og.kind == kind && og.type.array && t.array == kUnsizedArray && og.type.element() == t.element()) {
          if (Sym* s = find_sym(name)) if (s->kind == 1 && s->index == old) return;
          add_sym(name, og.type, 1, old);
          return;
        }
        if (og.kind != kind || !(og.type == t)) { err("a global is declared with different types or qualifiers in two shaders"); return; }
        if (init >= 0) {
          // (two shaders may initialize it alike: GLSL 1.20 4.3 "... must have the same initializer")
          // (only a constant one: an initializer that reads uniforms or variables is per shader, so two of them are an error even if written alike)
          if (og.init >= 0 && (!same_expression(og.init, init, 0) || !constant_expression(init, 0))) { err("a global is initialized in two shaders"); return; }
          if (og.init < 0) og.init = init;
        }
        if (Sym* s = find_sym(name)) if (s->kind == 1 && s->index == old) return;
        add_sym(name, og.type, 1, old);
        return;
      }
    }
    const int32_t g = new_global(name, t, kind, shared);
    if (g < 0) return;
    Global& gv = *m_.globals[static_cast<uint64_t>(g)];
    gv.init = init;
    gv.location = q.location;
    gv.binding = q.binding;
    gv.flat = q.flat;
    gv.per_vertex = per_vertex;
    gv.inner = inner;
    gv.patch = q.patch;
    if (kind == GlobalKind::Uniform) gv.uniform_index = 0;
    if (kind == GlobalKind::Out) set_capture(gv, q, nullptr, nullptr);
  }

  void global_declarators_after_struct(const Type& t) {
    // `struct S {...};` was parsed by struct_decl up to the closing brace
    if (is(";")) { ++pos_; return; }
    if (cur().kind == Tok::Ident) {
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
        if (t.array == kUnsizedArray) return err("a structure member must have a size");
        f.type = t;
        f.offset = offset;
        const uint32_t n = m_.slots_of(t);
        if (n >= kMaxValueSlots || offset > kMaxValueSlots - n) return err("structure too large");
        offset += n;
        sd->fields.push_back(static_cast<Field&&>(f));
        if (!accept(",")) break;
      }
      if (!expect(";")) return false;
    }
    if (!expect("}")) return false;
    sd->slots = offset;
    // Linked shaders that each define the structure (at file scope, the same members): one type.
    if (merge_units_ && cur_fn_ < 0) {
      for (uint64_t i = 0; i < m_.structs.count(); ++i) {
        const StructDef& old = *m_.structs[i];
        if (!old.name.view().equals(sd->name.view())) continue;
        bool same = old.fields.count() == sd->fields.count();
        for (uint64_t k = 0; same && k < old.fields.count(); ++k) same = old.fields[k].name.view().equals(sd->fields[k].name.view()) && old.fields[k].type == sd->fields[k].type;
        if (!same) return err("a structure is defined differently in two shaders");
        if (out) {
          *out = make_type(Base::Struct);
          out->sid = static_cast<uint16_t>(i);
        }
        return true;
      }
    }
    m_.structs.push_back(static_cast<UniquePointer<StructDef>&&>(sd));
    if (out) {
      *out = make_type(Base::Struct);
      out->sid = static_cast<uint16_t>(m_.structs.count() - 1);
    }
    return true;
  }

  // A uniform block (Module::blocks), or an in/out interface block, whose members are plain varyings of the stage.
  void uniform_block(const Quals& q) {
    const bool is_uniform = q.uniform;
    int32_t bi = -1;
    if (is_uniform) {
      UniquePointer<UniformBlock> blk = UniquePointer<UniformBlock>::make(a_, a_);
      if (!blk) { err("out of memory"); return; }
      blk->name.assign(cur().text);
      blk->binding = q.binding;
      bi = static_cast<int32_t>(m_.blocks.count());
      m_.blocks.push_back(static_cast<UniquePointer<UniformBlock>&&>(blk));
    }
    const int32_t iface = is_uniform ? -1 : interfaces_++;
    const StringView block_name = cur().text;
    ++pos_;
    if (!expect("{")) return;
    // An interface block with an instance name: its members are only reached as inst.member, and are named Block.member (as GL names them), so they cannot collide
    // with the stage's other varyings and match the other stage's block by block and member name.
    bool named_interface = false;
    // An arrayed instance (`in Blk {...} v[];` of the geometry and tessellation stages): every member is an array with one element per vertex.
    uint32_t arrayed = 0;
    const bool builtin_block = !is_uniform && (block_name.equals("gl_PerVertex") || block_name.equals("gl_PerFragment"));
    if (!is_uniform) {
      uint64_t k = pos_;
      while (k < toks_.count() && toks_[k].kind != Tok::End && !(toks_[k].text.equals("}"))) ++k;
      named_interface = k + 1 < toks_.count() && toks_[k + 1].kind == Tok::Ident;
      if (named_interface && k + 2 < toks_.count() && toks_[k + 2].text.equals("[")) {
        arrayed = per_vertex_length(q.out);
        if (k + 4 < toks_.count() && toks_[k + 3].kind == Tok::Int && toks_[k + 4].text.equals("]")) {
          const uint32_t n = toks_[k + 3].ival;
          if (n == 0 || n > 32) { err("bad interface block array size"); return; }
          if (stage_ == Stage::Geometry && q.in && m_.input_vertices && n != m_.input_vertices) { err("the array size of a geometry shader input does not match the input primitive"); return; }
          arrayed = n;
        }
      }
    }
    String qualified(a_);
    uint32_t offset = 0;
    int32_t xfb_next = q.xfb_offset;      // (a block with an xfb_offset: its members one after another from there)
    List<int32_t> members(a_);
    struct Sized { StringView name; uint32_t length; };
    List<Sized> sized(a_);                // (a gl_PerVertex redeclaration's array members with their sizes: applied once the instance name is known)
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
        // out gl_PerVertex { vec4 gl_Position; ... }: the members are the built-ins (only which ones are used is said)
        if (builtin_block) {
          if (q.out && (mq.xfb_offset >= 0 || xfb_next >= 0)) {
            const int32_t bg = m_.find_global(name);
            if (bg >= 0 && !set_capture(*m_.globals[static_cast<uint64_t>(bg)], mq, &q, &xfb_next)) return;
          }
          if (t.array && t.array != kUnsizedArray) {
            if (t.array > 8) { err("a built-in array redeclared larger than it is"); return; }
            if (!sized.push_back(Sized{name, t.array})) { err("out of memory"); return; }
          }
          if (!accept(",")) break;
          continue;
        }
        uint32_t member_inner = 0;
        if (arrayed) {
          if (t.array == kUnsizedArray) { err("a member of an arrayed interface block must have a size"); return; }
          member_inner = t.array;
          t.array = member_inner ? arrayed * member_inner : arrayed;
        }
        StringView global_name = name;
        if (!is_uniform) {      // (every member of an interface block is Block.member, which is how the stages match them; an unnamed block's members keep their own names as symbols)
          qualified.assign(block_name);
          qualified.push_back('.');
          qualified.append(name);
          global_name = qualified.view();
        }
        const bool tcs_out = stage_ == Stage::TessControl && q.out;
        const int32_t g = new_global(global_name, t, is_uniform ? GlobalKind::Uniform : (q.in ? GlobalKind::In : GlobalKind::Out), is_uniform || tcs_out);
        if (g < 0) return;
        Global& gv = *m_.globals[static_cast<uint64_t>(g)];
        gv.block = bi;
        gv.iface = iface;
        if (!is_uniform && !named_interface) add_sym(name, gv.type, 1, g);
        gv.per_vertex = arrayed != 0;
        gv.inner = member_inner;
        gv.patch = q.patch;
        if (q.out && !set_capture(gv, mq, &q, &xfb_next)) return;
        if (!is_uniform) {      // (a member's own interpolation qualifier)
          gv.flat = gv.flat || mq.flat || q.flat;
        }
        members.push_back(g);
        (void)offset;
        if (!accept(",")) break;
      }
      if (!expect(";")) return;
    }
    if (!expect("}")) return;
    if (cur().kind == Tok::Ident) {   // instance name
      const StringView instance = cur().text;
      ++pos_;
      if (builtin_block) {      // (gl_in[], gl_out[]: the built-in instance stays)
        Type ignored = make_type(Base::Void);
        if (is("[") && !parse_array_suffix(&ignored)) return;
        if (!apply_declared_lengths(instance, sized.view())) return;
        sized.clear();
      } else if (is_uniform) {
        m_.blocks[static_cast<uint64_t>(bi)]->instance.assign(instance);
        add_sym(instance, make_type(Base::Void), 2, bi);
      } else if (arrayed) {
        Type ignored = make_type(Base::Void);
        if (is("[") && !parse_array_suffix(&ignored)) return;
        add_sym(instance, make_type(Base::Void), 4, iface);
        if (!arrayed_.push_back(Arrayed{iface, arrayed})) { err("out of memory"); return; }
      } else {
        add_sym(instance, make_type(Base::Void), 3, iface);
      }
    }
    if (builtin_block && !apply_declared_lengths(StringView(), sized.view())) return;
    expect(";");
  }

  // The sizes a gl_PerVertex redeclaration gives built-in arrays: on the built-in (`gl_ClipDistance`) or the instance's member (`gl_in.gl_ClipDistance`).
  template <class SizedView>
  bool apply_declared_lengths(StringView instance, SizedView sized) {
    String full(a_);
    for (const auto& sz : sized) {
      full.clear();
      if (!instance.empty()) {
        full.append(instance);
        full.push_back('.');
      }
      full.append(sz.name);
      const int32_t g = m_.find_global(full.view());
      if (g >= 0) m_.globals[static_cast<uint64_t>(g)]->declared_length = sz.length;
    }
    return true;
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
    while (fn_units_.count() < m_.functions.count()) if (!fn_units_.push_back(0)) { err("out of memory"); return; }
    fn_units_[static_cast<uint64_t>(index)] |= unit_bit();
    if (accept(";")) return;   // a prototype
    if (!is("{")) { err("expected a function body"); return; }
    Function& F = *m_.functions[static_cast<uint64_t>(index)];
    if (F.body >= 0) {
      // (the helpers the desktop translation adds to each shader, tcos_*, are the same in each: the first is kept)
      if (merge_units_ && F.name.view().starts_with("tcos_")) {
        uint32_t depth = 0;
        do {
          if (is("{")) ++depth;
          else if (is("}")) --depth;
          ++pos_;
        } while (depth && cur().kind != Tok::End);
        return;
      }
      err(merge_units_ ? "a function is defined in two shaders" : "a function is defined twice");
      return;
    }
    cur_fn_ = index;
    nlocals_ = static_cast<uint32_t>(F.params.count());
    push_scope();
    for (uint64_t i = 0; i < pnames.count(); ++i)
      if (!pnames[i]->empty()) add_sym(pnames[i]->view(), F.params[i].type, 0, static_cast<int32_t>(i));
    const uint64_t first_node = m_.nodes.count();
    const int32_t body = block_stmt(false);
    if (!body_nodes_.push_back(BodyNodes{index, first_node, m_.nodes.count()})) { err("out of memory"); return; }
    finish_unsized_locals();
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
    if (is_id("const") || is_id("highp") || is_id("mediump") || is_id("lowp") || is_id("precision") || is_id("struct")) return true;
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
    if (is_id("struct")) {      // a structure declared inside a function (and maybe variables of it)
      if (!struct_decl(&base)) return -1;
      if (accept(";")) { Node e; e.kind = NodeKind::Empty; return add(e); }
    } else if (!parse_type(&base)) {
      return -1;
    }
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
      Node d;
      d.kind = NodeKind::Decl;
      d.type = t;
      d.a = init;
      d.ival = static_cast<int32_t>(nlocals_);
      d.line = cur().line;
      add_sym(name, t, 0, static_cast<int32_t>(nlocals_));
      const int32_t decl = add(d);
      // An unsized local is sized by the constant indices it is used with, when the function ends (finish_unsized_locals).
      if (t.array == kUnsizedArray && !unsized_locals_.push_back(UnsizedLocal{static_cast<int32_t>(nlocals_), decl, 0})) return err("out of memory"), -1;
      ++nlocals_;
      decls.push_back(decl);
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

  // Index `index` of a per-vertex array: of an array of arrays (inner > 0), the start of vertex `index`'s array, completed by the next index (kPartialIndex).
  static constexpr uint8_t kPartialIndex = 0x80;
  int32_t vertex_index(int32_t base, int32_t index, uint32_t inner) {
    Node ix;
    ix.kind = NodeKind::Index;
    ix.line = cur().line;
    ix.a = base;
    ix.b = index;
    if (inner) {
      Node mul;
      mul.kind = NodeKind::Binary;
      mul.op = Op::Mul;
      mul.line = ix.line;
      mul.a = index;
      mul.b = int_node(static_cast<int32_t>(inner));
      ix.b = add(mul);
      ix.flags = kPartialIndex;
      // (read alone, the vertex's whole array: the interpreter takes `inner` from the type)
      const Node& bn = m_.nodes[static_cast<uint64_t>(base)];
      if (bn.kind == NodeKind::Global) {
        ix.type = m_.globals[static_cast<uint64_t>(bn.ival)]->type.element();
        ix.type.array = inner;
      }
    }
    return add(ix);
  }

  int32_t postfix_expr() {
    int32_t e = primary();
    while (e >= 0 && !failed_) {
      if (is("[")) {
        ++pos_;
        // the second index of a per-vertex array of arrays: added to the first
        if (m_.nodes[static_cast<uint64_t>(e)].kind == NodeKind::Index && (m_.nodes[static_cast<uint64_t>(e)].flags & kPartialIndex)) {
          const int32_t j = expression();
          if (j < 0 || !expect("]")) return -1;
          Node sum;
          sum.kind = NodeKind::Binary;
          sum.op = Op::Add;
          sum.line = cur().line;
          sum.a = m_.nodes[static_cast<uint64_t>(e)].b;
          sum.b = j;
          const int32_t b = add(sum);
          m_.nodes[static_cast<uint64_t>(e)].b = b;
          m_.nodes[static_cast<uint64_t>(e)].flags &= static_cast<uint8_t>(~kPartialIndex);
          continue;
        }
        const uint32_t base_inner = m_.nodes[static_cast<uint64_t>(e)].kind == NodeKind::Global ? m_.globals[static_cast<uint64_t>(m_.nodes[static_cast<uint64_t>(e)].ival)]->inner : 0;
        if (base_inner) {
          const int32_t i = expression();
          if (i < 0 || !expect("]")) return -1;
          e = vertex_index(e, i, base_inner);
          continue;
        }
        Node n;
        n.kind = NodeKind::Index;
        n.a = e;
        n.b = expression();
        if (n.b < 0 || !expect("]")) return -1;
        const Node& base = m_.nodes[static_cast<uint64_t>(e)];
        if (base.kind == NodeKind::Local) {
          if (UnsizedLocal* u = unsized_local_of(base.ival)) {
            int64_t v;
            if (!fold_int(n.b, &v) || v < 0) return err("an unsized array can only be indexed with a constant"), -1;
            if (v >= static_cast<int64_t>(kMaxArrayLength)) return err("array too large"), -1;
            if (static_cast<uint32_t>(v) + 1 > u->length) u->length = static_cast<uint32_t>(v) + 1;
          }
        }
        if (base.kind == NodeKind::Global && m_.globals[static_cast<uint64_t>(base.ival)]->type.array == kUnsizedArray) {
          // An unsized array grows to the highest constant index it is used with; a variable index needs a size (GLSL 1.10, 4.1.9).
          int64_t v;
          Unsized* u = unsized_of(base.ival);
          if (!u || !fold_int(n.b, &v) || v < 0) return err("an unsized array can only be indexed with a constant"), -1;
          if (v >= static_cast<int64_t>(kMaxArrayLength)) return err("array too large"), -1;
          if (static_cast<uint32_t>(v) + 1 > u->length) u->length = static_cast<uint32_t>(v) + 1;
        }
        e = add(n);
      } else if (is(".")) {
        ++pos_;
        if (cur().kind != Tok::Ident) return err("expected a field name"), -1;
        const StringView name = cur().text;
        ++pos_;
        if (name.equals("length") && is("(")) {
          ++pos_;
          if (!expect(")")) return -1;
          // a redeclared built-in array (alone, or one vertex's of gl_in[]): the size the redeclaration gave
          const Node& bn = m_.nodes[static_cast<uint64_t>(e)];
          const Node* gn = bn.kind == NodeKind::Global ? &bn : (bn.kind == NodeKind::Index && (bn.flags & kPartialIndex) ? &m_.nodes[static_cast<uint64_t>(bn.a)] : nullptr);
          if (gn && gn->kind == NodeKind::Global && m_.globals[static_cast<uint64_t>(gn->ival)]->declared_length) {
            e = int_node(static_cast<int32_t>(m_.globals[static_cast<uint64_t>(gn->ival)]->declared_length));
            continue;
          }
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
    if (s->kind == 4) {      // an arrayed interface instance: inst[i].member, or inst.length()
      const uint32_t len = arrayed_length(s->index);
      if (accept(".")) {
        if (!is_id("length")) return err("expected length()"), -1;
        ++pos_;
        if (!expect("(") || !expect(")")) return -1;
        return int_node(static_cast<int32_t>(len));
      }
      if (!expect("[")) return -1;
      const int32_t index = expression();
      if (index < 0 || !expect("]")) return -1;
      if (!accept(".") || cur().kind != Tok::Ident) return err("expected a member of the block"), -1;
      const StringView member = cur().text;
      ++pos_;
      for (uint64_t i = m_.globals.count(); i > 0; --i) {
        const Global& g = *m_.globals[i - 1];
        const StringView gn = g.name.view();
        if (g.iface != s->index || gn.size() <= member.size() || gn[gn.size() - member.size() - 1] != '.' || !gn.substr(gn.size() - member.size(), member.size()).equals(member)) continue;
        n.kind = NodeKind::Global;
        n.ival = static_cast<int32_t>(i - 1);
        m_.globals[i - 1]->referenced = true;
        const int32_t base = add(n);
        return vertex_index(base, index, g.inner);
      }
      return err("no such member in the block"), -1;
    }
    if (s->kind == 2 || s->kind == 3) {   // block instance (uniform 2, interface 3): inst.member
      if (!accept(".") || cur().kind != Tok::Ident) return err("expected a member of the block"), -1;
      const StringView member = cur().text;
      ++pos_;
      for (uint64_t i = m_.globals.count(); i > 0; --i) {
        const Global& g = *m_.globals[i - 1];
        const StringView gn = g.name.view();
        const bool named = s->kind == 2 ? gn.equals(member)
                                         : (gn.size() > member.size() && gn[gn.size() - member.size() - 1] == '.' && gn.substr(gn.size() - member.size(), member.size()).equals(member));
        if ((s->kind == 2 ? g.block : g.iface) == s->index && named) {
          n.kind = NodeKind::Global;
          n.ival = static_cast<int32_t>(i - 1);
          m_.globals[i - 1]->referenced = true;
          return add(n);
        }
      }
      return err("no such member in the block"), -1;
    }
    n.kind = s->kind == 0 ? NodeKind::Local : NodeKind::Global;
    n.ival = s->index;
    if (s->kind == 1) {
      m_.globals[static_cast<uint64_t>(s->index)]->referenced = true;
      if (m_.globals[static_cast<uint64_t>(s->index)]->builtin == kBvFragCoord) unit_uses_frag_coord_ = true;
    }
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
      if (!f.name.view().equals(name) || f.params.count() != args.count() || !visible_here(i)) continue;
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
    if (t.legacy_shadow) n.flags |= 2;
    n.first = add_kids(kid, 9);
    n.count = 9;
    n.line = cur().line;
    if (op == TexOp::Sample || op == TexOp::Bias || op == TexOp::Proj || op == TexOp::ProjBias) m_.uses_implicit_lod = true;
    return add(n);
  }
};

}  // namespace

namespace {

// One shader's preprocessed text with the directive lines (#version, #extension, #pragma, #line, which glcpp keeps) blanked, keeping the line count.
bool preprocess_unit(Module& module, StringView source, const GlslOptions& glsl, String* pp, PreprocessResult* pre) {
  PreprocessOptions options;
  options.default_version = glsl.desktop ? 110 : 100;
  options.default_es = !glsl.desktop;
  if (!preprocess_glsl(module.allocator, source, options, pre)) {
    module.error.assign(pre->log.empty() ? StringView("preprocessor error") : pre->log.view());
    return false;
  }
  const StringView text = pre->output.view();
  uint64_t i = 0;
  while (i < text.size()) {
    uint64_t end = i;
    while (end < text.size() && text[end] != '\n') ++end;
    uint64_t k = i;
    while (k < end && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r')) ++k;
    if (k >= end || text[k] != '#') pp->append(text.substr(i, end - i));
    if (end < text.size()) pp->append("\n");
    i = end + 1;
  }
  return true;
}

}  // namespace

bool parse_glsl_units(Module& module, ArrayView<const StringView> sources, Stage stage, const GlslOptions& glsl) {
  module.stage = stage;
  module.language = Language::Glsl100;
  if (sources.size() == 0) {
    module.error.assign("no shader");
    return false;
  }
  // The texts stay alive while the tokens (views into them) are parsed.
  List<UniquePointer<String>> texts(module.allocator);
  List<Token> toks(module.allocator);
  uint32_t version = 0;
  bool es = false, compatibility_profile = false;
  for (const StringView source : sources) {
    UniquePointer<String> pp = UniquePointer<String>::make(module.allocator, module.allocator);
    PreprocessResult pre(module.allocator);
    if (!pp || !texts.push_back(static_cast<UniquePointer<String>&&>(pp))) {
      module.error.assign("out of memory");
      return false;
    }
    String& text = *texts[texts.count() - 1];
    if (!preprocess_unit(module, source, glsl, &text, &pre)) return false;
    if (texts.count() > 1 && pre.es != es) {
      module.error.assign("linked shaders mix GLSL ES and desktop GLSL");
      return false;
    }
    es = pre.es;
    if (pre.version > version) version = pre.version;
    compatibility_profile = compatibility_profile || pre.profile == 2;
    List<Token> unit(module.allocator);
    if (!lex(text.view(), &unit)) {
      module.error.assign("out of memory");
      return false;
    }
    // (each unit's End token is dropped but the last: one stream, with a break between units)
    if (texts.count() > 1) {
      Token brk;
      brk.kind = Tok::UnitBreak;
      if (!toks.push_back(brk)) {
        module.error.assign("out of memory");
        return false;
      }
    }
    for (const Token& t : unit) {
      if (t.kind == Tok::End) continue;
      if (!toks.push_back(t)) {
        module.error.assign("out of memory");
        return false;
      }
    }
  }
  Token end;
  end.kind = Tok::End;
  if (!toks.push_back(end)) {
    module.error.assign("out of memory");
    return false;
  }
  if (glsl.desktop && !es) {
    // Desktop GLSL: 1.10 and 1.20 by the 1.00 rules, 1.30 and later by the 3.00 ones, with what desktop GLSL adds (Module::desktop).
    module.desktop = true;
    module.desktop_version = version;
    module.compatibility = version <= 140 || compatibility_profile;
    module.glsl_version = version >= 130 ? 300 : 100;
  } else {
    module.glsl_version = version;
  }
  if (module.glsl_version >= 300) module.language = Language::Glsl300;
  Parser parser(module, toks, stage, sources.size() > 1, glsl.compile_only);
  return parser.run();
}

bool parse_glsl(Module& module, StringView source, Stage stage, const GlslOptions& glsl) {
  const StringView one[1] = {source};
  return parse_glsl_units(module, ArrayView<const StringView>(one, 1), stage, glsl);
}

}  // namespace shader
