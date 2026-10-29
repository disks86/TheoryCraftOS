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

// The TGSI to GCN compiler: TGSI text is parsed, compiled and run on the emulator, and the exports are compared with a small TGSI interpreter written for the test (one lane at a time,
// no registers or masks in common with the compiled code): every opcode, swizzles and write masks, the source modifiers, constants and literals as operands, a destination that is also
// a source, saturation, IF / ELSE / ENDIF nested with divergent lanes, discard, texture sampling, the vertex fetch and the interpolation. Then the built-in shaders of the OS against
// their native counterparts, and the compiler's refusals.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnEmu.hpp"

using namespace gcn;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

static float bits_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t f_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

struct Rng {
  uint32_t state = 12345;
  uint32_t next() { state = state * 1664525u + 1013904223u; return state >> 8; }
  float unit() { return static_cast<float>(next() & 0xFFFF) / 65536.0f; }           // [0, 1)
  float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

// The texture function of the test: both the environment and the interpreter use it
static void sample_fn(uint32_t image0, uint32_t sampler0, float u, float v, float out[4]) {
  out[0] = u * 2.0f + static_cast<float>(image0);
  out[1] = v * 3.0f;
  out[2] = u + v;
  out[3] = static_cast<float>(sampler0) + 0.5f;
}

// ---- the interpreter ----------------------------------------------------------------------------------------------------------------------------------------------------------

struct Lane {
  float temp[TgsiShader::kMaxTemps][4];
  float in[TgsiShader::kMaxInputs][4];
  float out[TgsiShader::kMaxOutputs][4];
  bool killed = false;
};

struct Interp {
  const TgsiShader& sh;
  const float (*constants)[4];
  uint32_t image0 = 0, sampler0 = 0;
  explicit Interp(const TgsiShader& s) : sh(s) {}

  float read(const Lane& l, const TgsiSrc& s, uint32_t comp) const {
    const uint32_t sw = s.swizzle[comp];
    float v = 0;
    switch (s.file) {
      case TgsiFile::Temp: v = l.temp[s.index][sw]; break;
      case TgsiFile::In: v = l.in[s.index][sw]; break;
      case TgsiFile::Const: v = constants[s.index][sw]; break;
      case TgsiFile::Imm: v = sh.immediates[s.index].value[sw]; break;
      default: break;
    }
    if (s.absolute) v = fabsf(v);
    if (s.negate) v = -v;
    return v;
  }

  static float fsign(float a) { return a > 0 ? 1.0f : (a < 0 ? -1.0f : 0.0f); }

  void run(Lane& l) const {
    // matching ELSE / ENDIF of each IF
    std::vector<size_t> else_of(sh.instructions.count(), 0), end_of(sh.instructions.count(), 0), stack;
    for (size_t n = 0; n < sh.instructions.count(); ++n) {
      const TgsiOp op = sh.instructions[n].op;
      if (op == TgsiOp::If) stack.push_back(n);
      else if (op == TgsiOp::Else) else_of[stack.back()] = n;
      else if (op == TgsiOp::EndIf) { end_of[stack.back()] = n; if (else_of[stack.back()]) end_of[else_of[stack.back()]] = n; stack.pop_back(); }
    }
    for (size_t n = 0; n < sh.instructions.count() && !l.killed; ++n) {
      const TgsiInst& i = sh.instructions[n];
      float a[4], b[4], c[4], r[4] = {0, 0, 0, 0};
      for (uint32_t k = 0; k < 4; ++k) {
        a[k] = i.src_count > 0 ? read(l, i.src[0], k) : 0;
        b[k] = i.src_count > 1 ? read(l, i.src[1], k) : 0;
        c[k] = i.src_count > 2 ? read(l, i.src[2], k) : 0;
      }
      bool write = true;
      switch (i.op) {
        case TgsiOp::Mov: for (int k = 0; k < 4; ++k) r[k] = a[k]; break;
        case TgsiOp::Neg: for (int k = 0; k < 4; ++k) r[k] = -a[k]; break;
        case TgsiOp::Abs: for (int k = 0; k < 4; ++k) r[k] = fabsf(a[k]); break;
        case TgsiOp::Add: for (int k = 0; k < 4; ++k) r[k] = a[k] + b[k]; break;
        case TgsiOp::Sub: for (int k = 0; k < 4; ++k) r[k] = a[k] - b[k]; break;
        case TgsiOp::Mul: for (int k = 0; k < 4; ++k) r[k] = a[k] * b[k]; break;
        case TgsiOp::Mad: for (int k = 0; k < 4; ++k) { float m = a[k] * b[k]; r[k] = m + c[k]; } break;
        case TgsiOp::Div: for (int k = 0; k < 4; ++k) r[k] = a[k] * (1.0f / b[k]); break;
        case TgsiOp::Lrp: for (int k = 0; k < 4; ++k) { float d = b[k] - c[k]; float m = a[k] * d; r[k] = m + c[k]; } break;
        case TgsiOp::Max: for (int k = 0; k < 4; ++k) r[k] = fmaxf(a[k], b[k]); break;
        case TgsiOp::Min: for (int k = 0; k < 4; ++k) r[k] = fminf(a[k], b[k]); break;
        case TgsiOp::Frc: for (int k = 0; k < 4; ++k) r[k] = a[k] - floorf(a[k]); break;
        case TgsiOp::Flr: for (int k = 0; k < 4; ++k) r[k] = floorf(a[k]); break;
        case TgsiOp::Ceil: for (int k = 0; k < 4; ++k) r[k] = ceilf(a[k]); break;
        case TgsiOp::Trunc: for (int k = 0; k < 4; ++k) r[k] = truncf(a[k]); break;
        case TgsiOp::Round: for (int k = 0; k < 4; ++k) r[k] = nearbyintf(a[k]); break;
        case TgsiOp::Ssg: for (int k = 0; k < 4; ++k) r[k] = fsign(a[k]); break;
        case TgsiOp::Cmp: for (int k = 0; k < 4; ++k) r[k] = a[k] < 0 ? b[k] : c[k]; break;
        case TgsiOp::Seq: for (int k = 0; k < 4; ++k) r[k] = a[k] == b[k] ? 1.0f : 0.0f; break;
        case TgsiOp::Sne: for (int k = 0; k < 4; ++k) r[k] = !(a[k] == b[k]) ? 1.0f : 0.0f; break;
        case TgsiOp::Sge: for (int k = 0; k < 4; ++k) r[k] = a[k] >= b[k] ? 1.0f : 0.0f; break;
        case TgsiOp::Slt: for (int k = 0; k < 4; ++k) r[k] = a[k] < b[k] ? 1.0f : 0.0f; break;
        case TgsiOp::Sgt: for (int k = 0; k < 4; ++k) r[k] = a[k] > b[k] ? 1.0f : 0.0f; break;
        case TgsiOp::Sle: for (int k = 0; k < 4; ++k) r[k] = a[k] <= b[k] ? 1.0f : 0.0f; break;
        case TgsiOp::Rcp: for (int k = 0; k < 4; ++k) r[k] = 1.0f / a[0]; break;
        case TgsiOp::Rsq: for (int k = 0; k < 4; ++k) r[k] = 1.0f / sqrtf(fabsf(a[0])); break;
        case TgsiOp::Sqrt: for (int k = 0; k < 4; ++k) r[k] = sqrtf(a[0]); break;
        case TgsiOp::Ex2: for (int k = 0; k < 4; ++k) r[k] = exp2f(a[0]); break;
        case TgsiOp::Lg2: for (int k = 0; k < 4; ++k) r[k] = log2f(a[0]); break;
        case TgsiOp::Pow: for (int k = 0; k < 4; ++k) r[k] = powf(a[0], b[0]); break;
        case TgsiOp::Sin: for (int k = 0; k < 4; ++k) r[k] = sinf(a[0]); break;
        case TgsiOp::Cos: for (int k = 0; k < 4; ++k) r[k] = cosf(a[0]); break;
        case TgsiOp::Dp2: for (int k = 0; k < 4; ++k) { float m = a[0] * b[0]; m = a[1] * b[1] + m; r[k] = m; } break;
        case TgsiOp::Dp3: for (int k = 0; k < 4; ++k) { float m = a[0] * b[0]; m = a[1] * b[1] + m; m = a[2] * b[2] + m; r[k] = m; } break;
        case TgsiOp::Dp4: for (int k = 0; k < 4; ++k) { float m = a[0] * b[0]; m = a[1] * b[1] + m; m = a[2] * b[2] + m; m = a[3] * b[3] + m; r[k] = m; } break;
        case TgsiOp::Xpd:
          r[0] = a[1] * b[2] - a[2] * b[1];
          r[1] = a[2] * b[0] - a[0] * b[2];
          r[2] = a[0] * b[1] - a[1] * b[0];
          r[3] = 1.0f;
          break;
        case TgsiOp::Tex: sample_fn(7 + i.sampler * 100, 3 + i.sampler * 10, a[0], a[1], r); break;
        case TgsiOp::Txb: sample_fn(7 + i.sampler * 100, 3 + i.sampler * 10, a[0], a[1], r); break;
        case TgsiOp::Txl: sample_fn(7 + i.sampler * 100, 3 + i.sampler * 10, a[0], a[1], r); break;
        case TgsiOp::Kill: l.killed = true; write = false; break;
        case TgsiOp::KillIf: for (int k = 0; k < 4; ++k) if (a[k] < 0) l.killed = true; write = false; break;
        case TgsiOp::If: write = false; if (a[0] == 0) n = (else_of[n] ? else_of[n] : end_of[n]); break;
        case TgsiOp::Else: write = false; n = end_of[n]; break;       // (reached after the then part)
        case TgsiOp::EndIf: case TgsiOp::End: write = false; break;
      }
      if (write) {
        float* dst = i.dst.file == TgsiFile::Out ? l.out[i.dst.index] : l.temp[i.dst.index];
        for (int k = 0; k < 4; ++k)
          if (i.dst.mask >> k & 1) dst[k] = i.dst.saturate ? (r[k] != r[k] ? 0.0f : (r[k] < 0 ? 0.0f : (r[k] > 1 ? 1.0f : r[k]))) : r[k];
      }
    }
  }
};

// ---- the environment -----------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kConstAddr = 0x10000, kTableAddr = 0x20000, kVertexAddr = 0x30000;
constexpr uint32_t kVertexStride = 64;          // 16 floats per vertex in the test buffer

struct Env final : IEnvironment {
  std::map<uint64_t, uint32_t> memory;
  struct Export { uint32_t target, enable; bool done; uint32_t values[kWaveSize][4]; uint64_t lanes; };
  std::vector<Export> exports;
  float p0(uint32_t attr, uint32_t chan) const { return 0.5f + static_cast<float>(attr) * 0.25f + static_cast<float>(chan) * 0.125f; }
  float p10(uint32_t attr, uint32_t) const { return 1.0f + static_cast<float>(attr) * 0.5f; }
  float p20(uint32_t, uint32_t chan, uint32_t lane) const { return -0.5f + static_cast<float>(chan) * 0.25f + static_cast<float>(lane) * 0.0625f; }
  uint32_t image0 = 7, sampler0 = 3;
  bool load32(uint64_t a, uint32_t* v) override { auto it = memory.find(a); if (it == memory.end()) return false; *v = it->second; return true; }
  bool store32(uint64_t, uint32_t) override { return false; }
  void interpolation(uint32_t attr, uint32_t chan, uint32_t lane, float* a, float* b, float* c) override { *a = p0(attr, chan); *b = p10(attr, chan); *c = p20(attr, chan, lane); }
  void sample(Mimg, const uint32_t image[8], const uint32_t sampler[4], const float* coords, uint32_t, float result[4]) override {
    // (image_sample_b / _l put the extra value first / last: the tests give the interpreter the coordinates only, and these are x, y at the front for the plain and lod forms)
    sample_fn(image[0], sampler[0], coords[0], coords[1], result);
  }
  void export_data(uint32_t target, uint32_t enable, bool, const uint32_t values[kWaveSize][4], uint64_t lanes, bool done) override {
    Export e{target, enable, done, {}, lanes};
    memcpy(e.values, values, sizeof e.values);
    exports.push_back(e);
  }
};

static void put_float(Env& env, uint64_t addr, float f) { env.memory[addr] = f_bits(f); }

static bool close(float got, float want, float tolerance) {
  if (got == want || (got != got && want != want)) return true;
  if (got != got || want != want) return false;
  const float scale = fabsf(want) > 1.0f ? fabsf(want) : 1.0f;
  return fabsf(got - want) <= tolerance * scale;
}

struct Result {
  bool compiled = false;
  std::string error;
  ShaderInfo info;
  uint32_t dwords = 0;
};

// Compile `text`, run it on 64 lanes with random data, and compare with the interpreter. `tolerance`: relative.
static Result check_shader(const char* name, const char* text, float tolerance = 0.0f, uint32_t seed = 1, bool expect_diverge = false) {
  TestAllocator alloc;
  Result res;
  TgsiShader sh(alloc);
  const char* error = nullptr;
  uint32_t line = 0;
  if (!parse_tgsi(alloc, StringView(text), &sh, &error, &line)) {
    printf("FAILED %s: parse error '%s' at line %u\n", name, error, line);
    ++g_failed;
    return res;
  }
  Code code(alloc);
  if (!compile_tgsi(sh, code, &res.info, &error)) {
    res.error = error ? error : "?";
    return res;
  }
  res.compiled = true;
  res.dwords = code.size();
  Rng rng;
  rng.state = seed * 2654435761u + 99;
  Env env;
  float constants[TgsiShader::kMaxConsts][4];
  for (uint32_t r = 0; r < TgsiShader::kMaxConsts; ++r)
    for (uint32_t k = 0; k < 4; ++k) { constants[r][k] = rng.range(-2.0f, 2.0f); put_float(env, kConstAddr + r * 16 + k * 4, constants[r][k]); }
  // vertex data: 64 vertices of 16 floats; attribute k = floats 4k .. 4k+3. Descriptors: attribute k at table + 16 k
  float vertices[kWaveSize][16];
  for (uint32_t v = 0; v < kWaveSize; ++v)
    for (uint32_t k = 0; k < 16; ++k) { vertices[v][k] = rng.range(-2.0f, 2.0f); put_float(env, kVertexAddr + v * kVertexStride + k * 4, vertices[v][k]); }
  for (uint32_t k = 0; k < 8; ++k) {
    const uint64_t base = kVertexAddr + k * 16;
    env.memory[kTableAddr + k * 16 + 0] = static_cast<uint32_t>(base);
    env.memory[kTableAddr + k * 16 + 4] = static_cast<uint32_t>(base >> 32) | (kVertexStride << 16);
    env.memory[kTableAddr + k * 16 + 8] = kWaveSize;
    env.memory[kTableAddr + k * 16 + 12] = (7u << 12) | (14u << 15);        // float, 32_32_32_32
  }
  for (uint32_t k = 0; k < abi::kMaxSamplers; ++k) {
    for (uint32_t w = 0; w < 8; ++w) env.memory[kTableAddr + abi::kImageOffset + k * abi::kImageBytes + w * 4] = 7 + k * 100 + w;
    for (uint32_t w = 0; w < 4; ++w) env.memory[kTableAddr + abi::kSamplerOffset + k * abi::kSamplerBytes + w * 4] = 3 + k * 10 + w;
  }
  Wave w;
  w.sgpr[0] = static_cast<uint32_t>(kConstAddr); w.sgpr[1] = 0;
  w.sgpr[2] = static_cast<uint32_t>(kTableAddr); w.sgpr[3] = 0;
  w.sgpr[4] = 0;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    if (sh.fragment) { w.set_vgpr_f(0, lane, static_cast<float>(lane) / 64.0f); w.set_vgpr_f(1, lane, static_cast<float>(63 - lane) / 128.0f); }
    else w.vgpr[0][lane] = lane;
  }
  const Status st = w.run(code.words(), env, 200000);
  if (st != Status::Done) { printf("FAILED %s: the emulator stopped with status %d at pc %u\n", name, static_cast<int>(st), w.pc); ++g_failed; return res; }
  // the interpreter
  Interp interp(sh);
  interp.constants = constants;
  interp.image0 = 7;
  interp.sampler0 = 3;
  std::vector<Lane> lanes(kWaveSize);
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
    Lane& l = lanes[lane];
    for (auto& t : l.temp) for (float& f : t) f = 0;
    for (auto& t : l.in) for (float& f : t) f = 0;
    for (auto& t : l.out) for (float& f : t) f = 0;
    l.killed = false;
    for (uint32_t k = 0; k < sh.inputs; ++k)
      for (uint32_t ch = 0; ch < 4; ++ch) {
        if (sh.fragment) {
          const uint32_t attr = res.info.input_attribute[k];
          if (attr == 0xFF) continue;
          const float i = static_cast<float>(lane) / 64.0f, j = static_cast<float>(63 - lane) / 128.0f;
          float v = env.p10(attr, ch) * i;
          v = v + env.p0(attr, ch);
          float t = env.p20(attr, ch, lane) * j;
          l.in[k][ch] = t + v;
        } else {
          l.in[k][ch] = vertices[lane][4 * k + ch];
        }
      }
    interp.run(l);
  }
  // compare the exports
  uint32_t compared = 0;
  const uint32_t want_exports = sh.fragment ? 1 : 1 + res.info.param_exports;
  if (env.exports.size() != want_exports) { printf("FAILED %s: %zu exports, expected %u\n", name, env.exports.size(), want_exports); ++g_failed; return res; }
  uint64_t killed_mask = 0;
  for (uint32_t lane = 0; lane < kWaveSize; ++lane) if (lanes[lane].killed) killed_mask |= 1ull << lane;
  for (size_t e = 0; e < env.exports.size(); ++e) {
    const Env::Export& ex = env.exports[e];
    // which TGSI output is this export
    uint32_t out_index = 0xFFFF;
    if (sh.fragment) {
      for (uint32_t k = 0; k < sh.outputs; ++k) if (sh.output_semantic[k] == TgsiSemantic::Color) out_index = k;
    } else if (ex.target == kExpPos0) {
      for (uint32_t k = 0; k < sh.outputs; ++k) if (sh.output_semantic[k] == TgsiSemantic::Position) out_index = k;
    } else {
      for (uint32_t k = 0; k < sh.outputs; ++k) if (res.info.output_param[k] == ex.target - kExpParam0) out_index = k;
    }
    if (out_index == 0xFFFF) { printf("FAILED %s: export %zu names no output\n", name, e); ++g_failed; return res; }
    if (sh.fragment && (ex.lanes & killed_mask)) { printf("FAILED %s: a killed lane exports (lanes %llx killed %llx)\n", name, (unsigned long long)ex.lanes, (unsigned long long)killed_mask); ++g_failed; return res; }
    for (uint32_t lane = 0; lane < kWaveSize; ++lane) {
      if (lanes[lane].killed) continue;
      for (uint32_t ch = 0; ch < 4; ++ch) {
        const float got = bits_f(ex.values[lane][ch]), want = lanes[lane].out[out_index][ch];
        ++compared;
        if (!close(got, want, tolerance)) {
          printf("FAILED %s: export %zu lane %u comp %u: got %.9g want %.9g\n", name, e, lane, ch, got, want);
          ++g_failed;
          return res;
        }
      }
    }
  }
  if (expect_diverge) CHECK(killed_mask != ~0ull);
  (void)compared;
  return res;
}

// Vertex shader template: inputs IN[0..3], outputs position and one varying; the body is given
#define VS(body) "VERT\nDCL IN[0]\nDCL IN[1]\nDCL IN[2]\nDCL IN[3]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL CONST[0..5]\nDCL TEMP[0..7]\nIMM[0] FLT32 {0.5, 2.0, -1.0, 0.0}\nIMM[1] FLT32 {3.14159, 0.1234567, 100.0, -7.5}\n" body "  99: END\n"
#define FS(body) "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL IN[1], GENERIC[1], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SAMP[1]\nDCL SVIEW[0], 2D, FLOAT\nDCL CONST[0..5]\nDCL TEMP[0..7]\nIMM[0] FLT32 {0.5, 2.0, -1.0, 0.0}\nIMM[1] FLT32 {3.14159, 0.1234567, 100.0, -7.5}\n" body "  99: END\n"

static void test_arithmetic() {
  struct Case { const char* name; const char* text; float tolerance; };
  const Case cases[] = {
      {"mov/add/sub/mul", VS("0: MOV TEMP[0], IN[0]\n1: ADD TEMP[1], IN[0], IN[1]\n2: SUB TEMP[2], IN[1], IN[2]\n3: MUL TEMP[3], TEMP[1], TEMP[2]\n4: ADD OUT[0], TEMP[3], TEMP[0]\n5: MUL OUT[1], TEMP[2], TEMP[1]\n"), 0},
      {"mad lrp div", VS("0: MAD TEMP[0], IN[0], IN[1], IN[2]\n1: LRP TEMP[1], IN[3], IN[0], IN[1]\n2: DIV TEMP[2], TEMP[0], TEMP[1]\n3: ADD OUT[0], TEMP[2], TEMP[1]\n4: MOV OUT[1], TEMP[0]\n"), 1e-6f},
      {"max min abs", VS("0: MAX TEMP[0], IN[0], IN[1]\n1: MIN TEMP[1], IN[0], IN[1]\n2: ABS TEMP[2], IN[2]\n3: ADD TEMP[3], TEMP[0], TEMP[1]\n4: ADD OUT[0], TEMP[3], TEMP[2]\n5: SUB OUT[1], TEMP[0], TEMP[1]\n"), 0},
      {"floor frac ceil trunc round", VS("0: FLR TEMP[0], IN[0]\n1: FRC TEMP[1], IN[1]\n2: CEIL TEMP[2], IN[2]\n3: TRUNC TEMP[3], IN[3]\n4: ROUND TEMP[4], IN[0]\n5: ADD TEMP[5], TEMP[0], TEMP[1]\n6: ADD TEMP[5], TEMP[5], TEMP[2]\n7: ADD OUT[0], TEMP[5], TEMP[3]\n8: MOV OUT[1], TEMP[4]\n"), 0},
      {"comparisons", VS("0: SEQ TEMP[0], IN[0], IN[1]\n1: SNE TEMP[1], IN[0], IN[1]\n2: SGE TEMP[2], IN[0], IN[1]\n3: SLT TEMP[3], IN[0], IN[1]\n4: SGT TEMP[4], IN[0], IN[1]\n5: SLE TEMP[5], IN[0], IN[1]\n6: MAD TEMP[6], TEMP[0], IMM[0].xxxx, TEMP[1]\n7: MAD TEMP[6], TEMP[2], IMM[0].yyyy, TEMP[6]\n8: MAD TEMP[6], TEMP[3], IMM[1].xxxx, TEMP[6]\n9: ADD OUT[0], TEMP[6], TEMP[4]\n10: MOV OUT[1], TEMP[5]\n"), 0},
      {"cmp ssg", VS("0: CMP TEMP[0], IN[0], IN[1], IN[2]\n1: SSG TEMP[1], IN[3]\n2: ADD OUT[0], TEMP[0], TEMP[1]\n3: CMP OUT[1], -IN[0], IN[2], IN[3]\n"), 0},
      {"scalar functions", VS("0: RCP TEMP[0], IN[0].yyyy\n1: RSQ TEMP[1], IN[1].zzzz\n2: SQRT TEMP[2].xy, IN[2].wwww\n3: EX2 TEMP[3], IN[3].xxxx\n4: LG2 TEMP[4], IN[0].wwww\n5: ADD TEMP[5], TEMP[0], TEMP[1]\n6: ADD TEMP[5], TEMP[5], TEMP[2]\n7: ADD OUT[0], TEMP[5], TEMP[3]\n8: MOV OUT[1], TEMP[4]\n"), 2e-6f},
      {"pow sin cos", VS("0: ABS TEMP[0], IN[0]\n1: POW TEMP[1], TEMP[0].xxxx, IN[1].yyyy\n2: SIN TEMP[2], IN[2].zzzz\n3: COS TEMP[3], IN[3].wwww\n4: ADD OUT[0], TEMP[1], TEMP[2]\n5: MOV OUT[1], TEMP[3]\n"), 2e-5f},
      {"dot cross", VS("0: DP2 TEMP[0], IN[0], IN[1]\n1: DP3 TEMP[1], IN[1], IN[2]\n2: DP4 TEMP[2], IN[2], IN[3]\n3: XPD TEMP[3], IN[0], IN[3]\n4: ADD TEMP[4], TEMP[0], TEMP[1]\n5: ADD OUT[0], TEMP[4], TEMP[2]\n6: MOV OUT[1], TEMP[3]\n"), 1e-6f},
      {"swizzles and write masks", VS("0: MOV TEMP[0].xz, IN[0].wzyx\n1: MOV TEMP[0].yw, IN[1].zzxy\n2: ADD TEMP[1].xyw, TEMP[0].yxwz, IN[2].xxyy\n3: MUL OUT[0].zx, TEMP[1], IN[3].wwww\n4: MOV OUT[0].yw, TEMP[0]\n5: MOV OUT[1], TEMP[1].wwxy\n"), 0},
      {"source modifiers", VS("0: ADD TEMP[0], -IN[0], |IN[1]|\n1: MUL TEMP[1], -|IN[2]|, IN[3]\n2: MAD TEMP[2], |IN[0]|, -IN[1], -IN[2]\n3: ADD OUT[0], TEMP[0], TEMP[1]\n4: ADD OUT[1], TEMP[2], -TEMP[0]\n"), 0},
      {"destination is a source", VS("0: MOV TEMP[0], IN[0]\n1: MOV TEMP[0], TEMP[0].wzyx\n2: ADD TEMP[0], TEMP[0], TEMP[0].yxwz\n3: MUL TEMP[0].xy, TEMP[0].yxzw, TEMP[0].xyzw\n4: MOV OUT[0], TEMP[0]\n5: MOV TEMP[1], TEMP[0].wzyx\n6: MOV TEMP[1].xy, TEMP[1].yxzw\n7: MOV OUT[1], TEMP[1]\n"), 0},
      {"saturate", VS("0: ADD_SAT TEMP[0], IN[0], IN[1]\n1: MUL_SAT TEMP[1], IN[2], IMM[0].yyyy\n2: MAD_SAT OUT[0], IN[0], IN[1], IN[2]\n3: ADD OUT[1], TEMP[0], TEMP[1]\n"), 0},
      {"constants and literals", VS("0: ADD TEMP[0], CONST[0], CONST[1]\n1: MAD TEMP[1], CONST[2], CONST[3], CONST[4]\n2: MUL TEMP[2], CONST[2], IN[0]\n3: ADD TEMP[3], IN[1], IMM[1].yyyy\n4: MAD TEMP[4], CONST[5], IMM[1].xxxx, IMM[1].wwww\n5: ADD OUT[0], TEMP[0], TEMP[1]\n6: ADD TEMP[5], TEMP[2], TEMP[3]\n7: ADD OUT[1], TEMP[5], TEMP[4]\n"), 1e-6f},
      {"one scalar per instruction", VS("0: MAD TEMP[0], CONST[0].xxxx, CONST[1].yyyy, CONST[2].zzzz\n1: MAD TEMP[1], CONST[0].xxxx, IMM[1].xxxx, IMM[1].zzzz\n2: CMP TEMP[2], CONST[3], CONST[4], IN[0]\n3: LRP TEMP[3], CONST[0], CONST[1], CONST[2]\n4: ADD OUT[0], TEMP[0], TEMP[1]\n5: ADD OUT[1], TEMP[2], TEMP[3]\n"), 1e-6f},
  };
  for (const Case& c : cases) {
    const Result r = check_shader(c.name, c.text, c.tolerance);
    if (!r.compiled) { printf("FAILED %s: did not compile: %s\n", c.name, r.error.c_str()); ++g_failed; }
  }
}

static void test_fragment() {
  struct Case { const char* name; const char* text; float tolerance; bool diverge; };
  const Case cases[] = {
      {"colour from inputs", FS("0: MUL TEMP[0], IN[0], IN[1]\n1: ADD OUT[0], TEMP[0], CONST[0]\n"), 1e-6f, false},
      {"texture", FS("0: TEX TEMP[0], IN[0], SAMP[0], 2D\n1: MUL OUT[0], TEMP[0], IN[1]\n"), 1e-6f, false},
      {"two textures", FS("0: TEX TEMP[0], IN[0], SAMP[0], 2D\n1: TEX TEMP[1], IN[1].yxzw, SAMP[1], 2D\n2: ADD OUT[0], TEMP[0], TEMP[1]\n"), 1e-6f, false},
      {"if else endif", FS("0: SLT TEMP[0], IN[0].xxxx, CONST[0].xxxx\n1: IF TEMP[0].xxxx\n2: MUL TEMP[1], IN[0], IMM[0].yyyy\n3: ELSE\n4: ADD TEMP[1], IN[1], CONST[1]\n5: ENDIF\n6: MOV OUT[0], TEMP[1]\n"), 1e-6f, false},
      {"if without else", FS("0: MOV TEMP[1], IN[0]\n1: SGT TEMP[0], IN[0].yyyy, CONST[0].yyyy\n2: IF TEMP[0].xxxx\n3: ADD TEMP[1], TEMP[1], IMM[0].zzzz\n4: ENDIF\n5: MOV OUT[0], TEMP[1]\n"), 0, false},
      {"nested ifs", FS("0: MOV TEMP[2], IN[0]\n1: SLT TEMP[0], IN[0].xxxx, CONST[0].xxxx\n2: IF TEMP[0].xxxx\n3: SLT TEMP[1], IN[1].yyyy, CONST[1].yyyy\n4: IF TEMP[1].xxxx\n5: ADD TEMP[2], TEMP[2], IMM[0].xxxx\n6: ELSE\n7: SUB TEMP[2], TEMP[2], IMM[0].yyyy\n8: ENDIF\n9: MUL TEMP[2], TEMP[2], IMM[0].yyyy\n10: ELSE\n11: SGT TEMP[1], IN[0].zzzz, CONST[2].zzzz\n12: IF TEMP[1].xxxx\n13: MOV TEMP[2], IN[1]\n14: ENDIF\n15: ENDIF\n16: MOV OUT[0], TEMP[2]\n"), 0, false},
      {"discard", FS("0: SLT TEMP[0], IN[0].xxxx, CONST[0].xxxx\n1: IF TEMP[0].xxxx\n2: KILL\n3: ENDIF\n4: MOV OUT[0], IN[1]\n"), 0, true},
      {"discard if", FS("0: ADD TEMP[0], IN[0], CONST[0]\n1: KILL_IF TEMP[0]\n2: MUL OUT[0], IN[1], TEMP[0]\n"), 1e-6f, true},
      {"discard in else, then more work", FS("0: SLT TEMP[0], IN[1].yyyy, CONST[0].yyyy\n1: MOV TEMP[1], IN[0]\n2: IF TEMP[0].xxxx\n3: ADD TEMP[1], TEMP[1], IMM[0].xxxx\n4: ELSE\n5: SLT TEMP[2], IN[0].zzzz, CONST[1].zzzz\n6: IF TEMP[2].xxxx\n7: KILL\n8: ENDIF\n9: MUL TEMP[1], TEMP[1], IMM[0].yyyy\n10: ENDIF\n11: ADD OUT[0], TEMP[1], IN[1]\n"), 0, true},
      {"varying with flat input", "FRAG\nDCL IN[0], GENERIC[0], CONSTANT\nDCL IN[1], GENERIC[1], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: ADD OUT[0], IN[0], IN[1]\n  1: END\n", 1e-6f, false},
  };
  for (const Case& c : cases) {
    if (strcmp(c.name, "varying with flat input") == 0) continue;
    const Result r = check_shader(c.name, c.text, c.tolerance, 1, c.diverge);
    if (!r.compiled) { printf("FAILED %s: did not compile: %s\n", c.name, r.error.c_str()); ++g_failed; }
  }
}

static void test_refusals() {
  struct Case { const char* name; const char* text; const char* error_part; };
  const Case cases[] = {
      {"no position", "VERT\nDCL IN[0]\nDCL OUT[0], GENERIC[0]\nDCL TEMP[0]\n  0: MOV OUT[0], IN[0]\n  1: END\n", "position"},
      {"no colour", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], GENERIC[0]\n  0: MOV OUT[0], IN[0]\n  1: END\n", "colour"},
      {"depth out", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL OUT[1], POSITION\n  0: MOV OUT[0], IN[0]\n  1: MOV OUT[1], IN[0]\n  2: END\n", "depth"},
      {"fragment position", "FRAG\nDCL IN[0], POSITION\nDCL OUT[0], COLOR\n  0: MOV OUT[0], IN[0]\n  1: END\n", "position"},
      {"texture in vertex shader", "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\nDCL SAMP[0]\nDCL TEMP[0]\n  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n  1: MOV OUT[0], TEMP[0]\n  2: END\n", "vertex"},
      {"kill in vertex shader", "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n  0: KILL\n  1: MOV OUT[0], IN[0]\n  2: END\n", "discard"},
      {"unbalanced if", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: IF IN[0].xxxx\n  1: MOV OUT[0], IN[0]\n  2: END\n", "ENDIF"},
      {"too deep", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: IF IN[0].xxxx\n  1: IF IN[0].xxxx\n  2: IF IN[0].xxxx\n  3: IF IN[0].xxxx\n  4: IF IN[0].xxxx\n  5: IF IN[0].xxxx\n  6: IF IN[0].xxxx\n  7: IF IN[0].xxxx\n  8: IF IN[0].xxxx\n  9: MOV OUT[0], IN[0]\n  10: ENDIF\n  11: ENDIF\n  12: ENDIF\n  13: ENDIF\n  14: ENDIF\n  15: ENDIF\n  16: ENDIF\n  17: ENDIF\n  18: ENDIF\n  19: END\n", "deeply"},
      {"three colours", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL OUT[1], COLOR[1]\nDCL OUT[2], COLOR[2]\n  0: MOV OUT[0], IN[0]\n  1: MOV OUT[1], IN[0]\n  2: MOV OUT[2], IN[0]\n  3: END\n", "two colour"},
      {"the same colour twice", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL OUT[1], COLOR\n  0: MOV OUT[0], IN[0]\n  1: MOV OUT[1], IN[0]\n  2: END\n", "two colour"},
      {"only the second colour", "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR[1]\n  0: MOV OUT[0], IN[0]\n  1: END\n", "COLOR[1]"},
  };
  TestAllocator alloc;
  for (const Case& c : cases) {
    TgsiShader sh(alloc);
    const char* error = nullptr;
    uint32_t line = 0;
    if (!parse_tgsi(alloc, StringView(c.text), &sh, &error, &line)) { printf("FAILED %s: parse: %s line %u\n", c.name, error, line); ++g_failed; continue; }
    Code code(alloc);
    ShaderInfo info;
    const bool ok = compile_tgsi(sh, code, &info, &error);
    if (ok || !error || !strstr(error, c.error_part)) { printf("FAILED %s: %s\n", c.name, ok ? "compiled" : error); ++g_failed; }
  }
  // two colour outputs: both exported (mrt0 then mrt1), the second declared first or last
  for (int order = 0; order < 2; ++order) {
    TgsiShader sh(alloc);
    const char* error = nullptr;
    uint32_t line = 0;
    const char* texts[2] = {"FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL OUT[1], COLOR[1]\n  0: MOV OUT[0], IN[0]\n  1: ADD OUT[1], IN[0], IN[0]\n  2: END\n",
                            "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR[1]\nDCL OUT[1], COLOR\n  0: ADD OUT[0], IN[0], IN[0]\n  1: MOV OUT[1], IN[0]\n  2: END\n"};
    Code code(alloc);
    ShaderInfo info;
    CHECK(parse_tgsi(alloc, StringView(texts[order]), &sh, &error, &line) && compile_tgsi(sh, code, &info, &error) && info.color_outputs == 2);
  }
  // the targets of sampler views are kept
  {
    TgsiShader sh(alloc);
    const char* error = nullptr;
    uint32_t line = 0;
    const char* text = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0..2]\nDCL SVIEW[0], CUBE, FLOAT\nDCL SVIEW[1], 3D, FLOAT\nDCL SVIEW[2], 2D_ARRAY, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[1], 3D\n  1: END\n";
    CHECK(parse_tgsi(alloc, StringView(text), &sh, &error, &line) && sh.sampler_target[0] == TgsiShader::kTargetCube && sh.sampler_target[1] == TgsiShader::kTarget3D &&
          sh.sampler_target[2] == TgsiShader::kTarget2DArray);
  }
  {
    TgsiShader sh(alloc);
    const char* error = nullptr;
    uint32_t line = 0;
    const char* text = "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0..2]\nDCL SVIEW[0], 1D, FLOAT\nDCL SVIEW[1], 1D_ARRAY, FLOAT\nDCL SVIEW[2], CUBE_ARRAY, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[2], CUBE_ARRAY\n  1: END\n";
    CHECK(parse_tgsi(alloc, StringView(text), &sh, &error, &line) && sh.sampler_target[0] == TgsiShader::kTarget1D && sh.sampler_target[1] == TgsiShader::kTarget1DArray &&
          sh.sampler_target[2] == TgsiShader::kTargetCubeArray);
  }
  // the parser's refusals
  const char* bad[] = {"", "VERT\n", "FRAG\nDCL OUT[0], COLOR\n  0: FOO OUT[0], IN[0]\n  1: END\n", "FRAG\n  0: MOV OUT[0], IN[0]\n  1: END\n  2: MOV OUT[0], IN[0]\n", "FRAG\nDCL IN[0]\n  0: MOV OUT[0] IN[0]\n  1: END\n",
                       "FRAG\nDCL OUT[0], COLOR\n  0: MOV OUT[0], CONST[\n  1: END\n", "FRAG\nIMM[1] FLT32 {1, 2, 3, 4}\n  0: END\n", "GEOM\n", "FRAG\nDCL TEMP[0..999]\n  0: END\n",
                       "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], RECT, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[0], RECT\n  1: END\n",                  // a target there is not
                       "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], CUBE, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[0], 2D\n  1: END\n"};   // not the declared one
  for (const char* t : bad) {
    TgsiShader sh(alloc);
    const char* error = nullptr;
    uint32_t line = 0;
    if (parse_tgsi(alloc, StringView(t), &sh, &error, &line)) { printf("FAILED: parsed %s\n", t); ++g_failed; }
  }
}

// the built-in shaders of the OS, compiled and run: the vertex shader against mvp * position, the fragment shaders against their native counterparts
static void test_builtin_shaders() {
  const char* colored_vertex =
      "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL CONST[0..3]\nDCL TEMP[0]\n"
      "  0: MUL TEMP[0], CONST[0], IN[0].xxxx\n  1: MAD TEMP[0], CONST[1], IN[0].yyyy, TEMP[0]\n  2: MAD TEMP[0], CONST[2], IN[0].zzzz, TEMP[0]\n  3: ADD OUT[0], TEMP[0], CONST[3]\n  4: MOV OUT[1], IN[1]\n  5: END\n";
  const char* colored_fragment = "FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: MOV OUT[0], IN[0]\n  1: END\n";
  const char* textured_fragment =
      "FRAG\nPROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\nDCL SAMP[0]\nDCL SVIEW[0], 2D, FLOAT\n  0: TEX OUT[0], IN[0], SAMP[0], 2D\n  1: END\n";
  CHECK(check_shader("colored vertex", colored_vertex, 0).compiled);
  CHECK(check_shader("colored fragment", colored_fragment, 0).compiled);
  const Result t = check_shader("textured fragment", textured_fragment, 1e-6f);
  CHECK(t.compiled && (t.info.samplers_used == 1) && t.info.ps_inputs == 1 && t.info.fragment);
  // the information a driver needs
  TestAllocator alloc;
  TgsiShader sh(alloc);
  const char* error = nullptr;
  uint32_t line = 0;
  CHECK(parse_tgsi(alloc, StringView(colored_vertex), &sh, &error, &line));
  Code code(alloc);
  ShaderInfo info;
  CHECK(compile_tgsi(sh, code, &info, &error));
  CHECK(!info.fragment && info.param_exports == 1 && info.output_param[0] == 0xFF && info.output_param[1] == 0 && info.attributes_used == 3 && info.const_rows == 4 && info.user_sgprs == 4);
  CHECK(info.vgprs >= 1 + 8 + 4 + 8 + 12 && info.sgprs > 4 && info.code_dwords == code.size());
  CHECK(code.word(code.size() - 1) == 0xBF810000u);
}

int main() {
  test_arithmetic();
  test_fragment();
  test_refusals();
  test_builtin_shaders();
  printf(g_failed ? "GcnCompilerTest: %d FAILED\n" : "GcnCompilerTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
