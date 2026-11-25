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

// Host-side tests of the subroutines and switches of the kernel's DXBC front end (src/Kernel/Dxgkrnl/D3d9Dxbc.cpp, Flattener):
//   - shaders Microsoft's compiler made with [call] switches (call / label / ret, returns from inside ifs and loops of the subroutines) and plain switches (case
//     labels together, breaks inside ifs of cases, a switch inside a switch, a return inside a case) give, on the interpreter and as TGSI on the test's TGSI machine
//     (what virgl runs), what the HLSL says, for every selector and constant set tried; the vertex and compute ones run too, the geometry, domain and hull ones compile
//     for both;
//   - hand-made programs: returns from loops of subroutines called from loops of subroutines (the flag), callc / retc / breakc, a continue from inside a switch, a case
//     that falls into the next and a default between cases, recursion and calls nested deeper than Direct3D allows refused, an interface's function body refused;
//   - every prefix of the programs and every bit turned compile or are refused without a read outside them (ASan, UBSan).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Kernel/Dxgkrnl/D3d9Shader.hpp"
#include "Utility/String.hpp"
#include "TgsiMachine.hpp"

using namespace dxgk;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    return p;
  }
  void deallocate(void* p) override { free(p); }
};

std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  uint8_t buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + n);
  fclose(f);
  return out;
}

// The constants of the fixtures: cb0[0] = int4 sel, cb0[1] = float4 a (the hand-made programs read the same).
struct Constants {
  int32_t sel[4] = {};
  float a[4] = {};
  uint8_t bytes[64] = {};
  const uint8_t* data() {
    memcpy(bytes, sel, 16);
    memcpy(bytes + 16, a, 16);
    return bytes;
  }
};
struct F4 {
  float v[4];
};

bool same4(const float* got, const F4& want) {
  for (int k = 0; k < 4; ++k)
    if (!(fabsf(got[k] - want.v[k]) <= 1e-5f)) return false;
  return true;
}

// Runs a pixel shader on the interpreter and as TGSI; both must give `want`.
struct Runner {
  HostAllocator& allocator;
  std::unique_ptr<tgsi_test::TgsiMachine> machine = std::make_unique<tgsi_test::TgsiMachine>();
  int reported = 0;

  bool pixel(const D3d9Program& program, const std::string& tgsi, Constants& c, const F4& want, const char* what) {
    graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
    graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
    const uint8_t* constants = c.data();
    const bool ran = program.run_fragment(varyings, constants, nullptr, colour);
    const float got[4] = {colour[0].x, colour[0].y, colour[0].z, colour[0].w};
    bool ok = ran && same4(got, want);
    tgsi_test::TgsiMachine& m = *machine;
    m = tgsi_test::TgsiMachine();
    for (int r = 0; r < 4; ++r) memcpy(m.cst[r], constants + 16 * r, 16);
    const bool tgsi_ran = m.run(tgsi);
    const bool tgsi_ok = tgsi_ran && same4(m.out[0], want);
    if ((!ok || !tgsi_ok) && reported++ < 6)
      printf("  %s (sel %d %d %d %d, a %g %g %g %g): interpreter %g %g %g %g%s, TGSI %g %g %g %g%s, want %g %g %g %g\n", what, c.sel[0], c.sel[1], c.sel[2], c.sel[3], c.a[0], c.a[1],
             c.a[2], c.a[3], got[0], got[1], got[2], got[3], ran ? "" : " (did not run)", m.out[0][0], m.out[0][1], m.out[0][2], m.out[0][3], tgsi_ran ? "" : " (did not run)", want.v[0],
             want.v[1], want.v[2], want.v[3]);
    return ok && tgsi_ok;
  }
};

struct Compiled {
  UniquePointer<D3d9Program> program;
  std::string tgsi;
  NtStatus status = status::kInvalidParameter;
  NtStatus tgsi_status = status::kInvalidParameter;
};
Compiled compile(HostAllocator& allocator, const std::vector<uint8_t>& dxbc) {
  Compiled c;
  c.status = D3d9Program::compile_dxbc(allocator, ConstByteView(dxbc.data(), dxbc.size()), &c.program);
  if (c.status != status::kSuccess) return c;
  String text(allocator);
  c.tgsi_status = c.program->emit_tgsi(&text, graphics::kMaxVaryings);
  c.tgsi.assign(text.data(), text.size());
  return c;
}

// --- hand-made programs ------------------------------------------------------------------------------------------------------------------------------------------------------

enum : uint32_t {
  ADD = 0, BREAK = 2, BREAKC = 3, CALL = 4, CALLC = 5, CASE = 6, CONTINUE = 7, CONTINUEC = 8, DEFAULT = 10, ELSE = 18, ENDIF = 21, ENDLOOP = 22, ENDSWITCH = 23, IADD = 30, IF = 31,
  IEQ = 32, IGE = 33, ILT = 34, LABEL = 44, LOOP = 48, MOV = 54, RET = 62, RETC = 63, SWITCH = 76, DCL_CONSTANT_BUFFER = 89, DCL_OUTPUT = 101, DCL_TEMPS = 104,
};
constexpr uint32_t NZ = 1u << 18;
using Words = std::vector<uint32_t>;

Words ins(uint32_t opcode, std::initializer_list<Words> operands) {
  Words w{opcode};
  for (const Words& o : operands) w.insert(w.end(), o.begin(), o.end());
  w[0] |= static_cast<uint32_t>(w.size()) << 24;
  return w;
}
Words rd(uint32_t t, uint32_t mask) { return {0x00100002u | mask << 4, t}; }           // rT.mask
Words rs(uint32_t t, uint32_t c) { return {0x0010000Au | c << 4, t}; }                  // rT.c
Words rv(uint32_t t) { return {0x00100002u | 1u << 2 | 0xE4u << 4, t}; }                // rT.xyzw
Words cb(uint32_t reg, uint32_t c) { return {0x0020800Au | c << 4, 0, reg}; }          // cb0[reg].c
Words lf(float v) { uint32_t b; memcpy(&b, &v, 4); return {0x00004001u, b}; }          // l(v)
Words li(uint32_t v) { return {0x00004001u, v}; }
Words l4(float x, float y, float z, float w) {
  Words o{0x00004002u};
  for (float v : {x, y, z, w}) { uint32_t b; memcpy(&b, &v, 4); o.push_back(b); }
  return o;
}
Words label(uint32_t n) { return {0x0010A000u, n}; }

// A pixel shader container: no inputs, o0 the colour, cb0 of 2 vectors, `temps` temporaries; `code` the instructions (the declarations come first).
std::vector<uint8_t> pixel_shader(const Words& code, uint32_t temps) {
  Words program{0x50, 0};
  for (const Words& d : {Words{DCL_CONSTANT_BUFFER | 4u << 24, 0x00208000u, 0, 2}, Words{DCL_OUTPUT | 3u << 24, 0x001020F2u, 0}, Words{DCL_TEMPS | 2u << 24, temps}})
    program.insert(program.end(), d.begin(), d.end());
  program.insert(program.end(), code.begin(), code.end());
  program[1] = static_cast<uint32_t>(program.size());
  std::vector<uint8_t> chunk_isgn = {0, 0, 0, 0, 8, 0, 0, 0};
  std::vector<uint8_t> chunk_osgn(8 + 24 + 12, 0);
  const uint32_t osgn[] = {1, 8, 32, 0, 64, 3, 0, 0x0F0F};
  memcpy(chunk_osgn.data(), osgn, sizeof osgn);
  memcpy(chunk_osgn.data() + 32, "SV_Target", 10);
  std::vector<uint8_t> chunk_shex(program.size() * 4);
  memcpy(chunk_shex.data(), program.data(), chunk_shex.size());
  const std::vector<uint8_t>* parts[] = {&chunk_isgn, &chunk_osgn, &chunk_shex};
  const char* tags[] = {"ISGN", "OSGN", "SHEX"};
  std::vector<uint8_t> out(32 + 4 * 3, 0);
  memcpy(out.data(), "DXBC", 4);
  const uint32_t one = 1, count = 3;
  memcpy(out.data() + 20, &one, 4);
  memcpy(out.data() + 28, &count, 4);
  for (int i = 0; i < 3; ++i) {
    const uint32_t at = static_cast<uint32_t>(out.size()), n = static_cast<uint32_t>(parts[i]->size());
    memcpy(out.data() + 32 + 4 * i, &at, 4);
    out.insert(out.end(), tags[i], tags[i] + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
    out.insert(out.end(), parts[i]->begin(), parts[i]->end());
  }
  const uint32_t total = static_cast<uint32_t>(out.size());
  memcpy(out.data() + 24, &total, 4);
  return out;
}
Words cat(std::initializer_list<Words> parts) {
  Words w;
  for (const Words& p : parts) w.insert(w.end(), p.begin(), p.end());
  return w;
}

// Returns from loops of subroutines called from a loop of a subroutine: n = sel.y times, A adds 1 to x and loops j = 0..3 adding 1 to y; at j == sel.x it calls B
// and returns. B loops adding 1 to w until it has done sel.z (retc from inside its loop) - 100 is never added. A adds 1 to z when its loop ran out.
Words returns_from_loops() {
  return cat({
      ins(MOV, {rd(0, 0xF), l4(0, 0, 0, 0)}), ins(MOV, {rd(1, 1), li(0)}),
      ins(LOOP, {}), ins(IGE, {rd(1, 2), rs(1, 0), cb(0, 1)}), ins(BREAKC | NZ, {rs(1, 1)}), ins(CALL, {label(0)}), ins(IADD, {rd(1, 1), rs(1, 0), li(1)}), ins(ENDLOOP, {}),
      ins(MOV, {{0x001020F2u, 0}, rv(0)}), ins(RET, {}),
      ins(LABEL, {label(0)}),
      ins(ADD, {rd(0, 1), rs(0, 0), lf(1)}), ins(MOV, {rd(2, 1), li(0)}),
      ins(LOOP, {}),
      ins(ADD, {rd(0, 2), rs(0, 1), lf(1)}), ins(IEQ, {rd(2, 2), rs(2, 0), cb(0, 0)}),
      ins(IF | NZ, {rs(2, 1)}), ins(CALL, {label(1)}), ins(RET, {}), ins(ENDIF, {}),
      ins(IADD, {rd(2, 1), rs(2, 0), li(1)}), ins(IGE, {rd(2, 2), rs(2, 0), li(4)}), ins(BREAKC | NZ, {rs(2, 1)}),
      ins(ENDLOOP, {}),
      ins(ADD, {rd(0, 4), rs(0, 2), lf(1)}), ins(RET, {}),
      ins(LABEL, {label(1)}),
      ins(MOV, {rd(3, 1), li(0)}),
      ins(LOOP, {}),
      ins(ADD, {rd(0, 8), rs(0, 3), lf(1)}), ins(IADD, {rd(3, 1), rs(3, 0), li(1)}), ins(IGE, {rd(3, 2), rs(3, 0), cb(0, 2)}), ins(RETC | NZ, {rs(3, 1)}),
      ins(ILT, {rd(3, 2), rs(3, 0), li(100)}), ins(BREAKC, {rs(3, 1)}),
      ins(ENDLOOP, {}),
      ins(ADD, {rd(0, 8), rs(0, 3), lf(100)}), ins(RET, {}),
  });
}
F4 returns_from_loops_reference(const Constants& c) {
  F4 r = {{0, 0, 0, 0}};
  for (int i = 0; i < c.sel[1]; ++i) {
    r.v[0] += 1;
    bool returned = false;
    for (int j = 0; j < 4; ++j) {
      r.v[1] += 1;
      if (j == c.sel[0]) {
        for (int k = 1; k <= 100; ++k) {
          r.v[3] += 1;
          if (k >= c.sel[2]) break;
        }
        returned = true;
        break;
      }
    }
    if (!returned) r.v[2] += 1;
  }
  return r;
}

// A switch inside a loop (i = 1 .. sel.y): case 1 adds 1 to x and continues the loop from inside the switch; case 2 adds 1 to y and falls into the default; the
// default adds 1 to z and continues when i == sel.x, else falls into case 5, which adds 1 to w (and breaks); case 6 calls (callc_nz on sel.z) a subroutine adding 4
// to w. After the switch 10 is added to x. retc_nz in the main program: with sel.w set the colour is (7, 7, 7, 7) written before.
Words switch_continue() {
  return cat({
      ins(MOV, {{0x001020F2u, 0}, l4(7, 7, 7, 7)}),
      ins(RETC | NZ, {cb(0, 3)}),
      ins(MOV, {rd(0, 0xF), l4(0, 0, 0, 0)}), ins(MOV, {rd(1, 1), li(0)}),
      ins(LOOP, {}), ins(IGE, {rd(1, 2), rs(1, 0), cb(0, 1)}), ins(BREAKC | NZ, {rs(1, 1)}), ins(IADD, {rd(1, 1), rs(1, 0), li(1)}),
      ins(SWITCH, {rs(1, 0)}),
      ins(CASE, {li(1)}), ins(ADD, {rd(0, 1), rs(0, 0), lf(1)}), ins(CONTINUE, {}),
      ins(CASE, {li(2)}), ins(ADD, {rd(0, 2), rs(0, 1), lf(1)}),
      ins(DEFAULT, {}), ins(ADD, {rd(0, 4), rs(0, 2), lf(1)}), ins(IEQ, {rd(1, 4), rs(1, 0), cb(0, 0)}), ins(CONTINUEC | NZ, {rs(1, 2)}),
      ins(CASE, {li(5)}), ins(ADD, {rd(0, 8), rs(0, 3), lf(1)}), ins(BREAK, {}),
      ins(CASE, {li(6)}), ins(CALLC | NZ, {cb(0, 2), label(0)}), ins(BREAK, {}),
      ins(ENDSWITCH, {}),
      ins(ADD, {rd(0, 1), rs(0, 0), lf(10)}),
      ins(ENDLOOP, {}),
      ins(MOV, {{0x001020F2u, 0}, rv(0)}), ins(RET, {}),
      ins(LABEL, {label(0)}), ins(ADD, {rd(0, 8), rs(0, 3), lf(4)}), ins(RET, {}),
  });
}
F4 switch_continue_reference(const Constants& c) {
  if (c.sel[3]) return {{7, 7, 7, 7}};
  F4 r = {{0, 0, 0, 0}};
  for (int i = 1; i <= c.sel[1]; ++i) {
    switch (i) {
      case 1: r.v[0] += 1; continue;
      case 2: r.v[1] += 1; [[fallthrough]];
      default: r.v[2] += 1; if (i == c.sel[0]) continue; [[fallthrough]];
      case 5: r.v[3] += 1; break;
      case 6: if (c.sel[2]) r.v[3] += 4; break;
    }
    r.v[0] += 10;
  }
  return r;
}

// A chain of `depth` subroutines, each calling the next (the last adds 1 to x).
std::vector<uint8_t> call_chain(uint32_t depth, bool recursive) {
  Words code = cat({ins(MOV, {rd(0, 0xF), l4(0, 0, 0, 0)}), ins(CALL, {label(0)}), ins(MOV, {{0x001020F2u, 0}, rv(0)}), ins(RET, {})});
  for (uint32_t d = 0; d < depth; ++d) {
    const Words body = d + 1 < depth ? ins(CALL, {label(d + 1)}) : recursive ? ins(CALL, {label(0)}) : ins(ADD, {rd(0, 1), rs(0, 0), lf(1)});
    code = cat({code, ins(LABEL, {label(d)}), body, ins(RET, {})});
  }
  return pixel_shader(code, 1);
}

// --- the checks ----------------------------------------------------------------------------------------------------------------------------------------------------

void run_pixel_cases(Runner& run, const Compiled& c, const char* what, const std::function<F4(const Constants&)>& reference, const std::vector<Constants>& sets) {
  CHECK(c.status == status::kSuccess && c.tgsi_status == status::kSuccess);
  if (c.status != status::kSuccess || c.tgsi_status != status::kSuccess) {
    printf("  %s: compile %x, TGSI %x\n", what, static_cast<unsigned>(c.status), static_cast<unsigned>(c.tgsi_status));
    return;
  }
  int bad = 0;
  for (Constants k : sets)
    if (!run.pixel(*c.program, c.tgsi, k, reference(k), what)) ++bad;
  CHECK(bad == 0);
  printf("  %s: %zu constant sets\n", what, sets.size());
}

std::vector<Constants> constant_sets(int sel0_from, int sel0_to, int n_to, int sel2_to, int sel3_to) {
  std::vector<Constants> sets;
  const float as[3][4] = {{0.25f, 0.75f, 0.125f, 0.25f}, {0.75f, 0.25f, 0.75f, 0.75f}, {0.5f, 2.0f, 0.625f, 0.375f}};
  for (int s0 = sel0_from; s0 <= sel0_to; ++s0)
    for (int n = 0; n <= n_to; ++n)
      for (int s2 = 0; s2 <= sel2_to; ++s2)
        for (int s3 = 0; s3 <= sel3_to; ++s3)
          for (const auto& a : as) {
            Constants c;
            c.sel[0] = s0, c.sel[1] = n, c.sel[2] = s2, c.sel[3] = s3;
            memcpy(c.a, a, 16);
            sets.push_back(c);
          }
  return sets;
}

// Every prefix and every bit turned: compiled or refused, never a read outside (the sanitizers say), and what compiles runs.
void damaged(HostAllocator& allocator, const std::vector<uint8_t>& dxbc, const char* what) {
  int taken = 0;
  graphics::Vec4 varyings[graphics::kMaxVaryings] = {};
  graphics::Vec4 colour[graphics::kMaxColorTargets] = {};
  auto attempt = [&](const std::vector<uint8_t>& bytes) {
    UniquePointer<D3d9Program> program;
    if (D3d9Program::compile_dxbc(allocator, ConstByteView(bytes.data(), bytes.size()), &program) != status::kSuccess) return;
    ++taken;
    if (program->stage() == graphics::ShaderStage::Fragment && !program->uses_textures()) {
      std::vector<uint8_t> constants(program->constant_bytes() + 64, 0);
      program->run_fragment(varyings, constants.data(), nullptr, colour);
    }
    String text(allocator);
    (void)program->emit_tgsi(&text, graphics::kMaxVaryings);
  };
  for (size_t n = 0; n < dxbc.size(); ++n) attempt(std::vector<uint8_t>(dxbc.begin(), dxbc.begin() + static_cast<long>(n)));
  for (size_t bit = 0; bit < dxbc.size() * 8; ++bit) {
    std::vector<uint8_t> turned = dxbc;
    turned[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    attempt(turned);
  }
  printf("  damaged %s: %d of %zu prefixes and turned bits compiled\n", what, taken, dxbc.size() * 9);
}

}  // namespace

int main() {
  HostAllocator allocator;
  Runner run{allocator};

  // Microsoft's compiler: [call] switch, a subroutine that returns from inside an if
  {
    const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/ps_call_ret.dxbc");
    CHECK(dxbc.size() > 100);
    const Compiled c = compile(allocator, dxbc);
    auto reference = [](const Constants& k) -> F4 {
      const float* a = k.a;
      if (k.sel[0] == 0) return {{a[0] * 2, a[1] * 2, a[2] * 2, a[3] * 2}};
      if (k.sel[0] == 1) return a[0] > 0.5f ? F4{{a[0] + 1, 3, a[2] + 1, a[3] + 1}} : F4{{a[0] + 1, a[1] + 1, 4, a[3] + 1}};
      return {{7, 7, 7, 7}};
    };
    run_pixel_cases(run, c, "ps_call_ret", reference, constant_sets(-1, 3, 0, 0, 0));
    damaged(allocator, dxbc, "ps_call_ret");
  }
  // [call] switch inside a loop: case labels together, a loop inside a subroutine
  {
    const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/ps_call_switch.dxbc");
    const Compiled c = compile(allocator, dxbc);
    auto reference = [](const Constants& k) -> F4 {
      F4 r = {{0, 0, 0, 0}};
      const float* a = k.a;
      for (int i = 0; i < k.sel[1]; ++i) {
        switch (k.sel[0] + i) {
          case 0: r.v[0] += 1; break;
          case 1: { float s = 0; for (int q = 0; q < 4; ++q) { s += a[0]; if (s > a[1]) break; } r.v[1] += s; break; }
          case 2: case 3: r.v[2] += a[2]; if (a[3] > 0.5f) { r.v[3] += 1; break; } r.v[3] += 2; break;
          default: for (float& v : r.v) v += 0.5f; break;
        }
      }
      return r;
    };
    run_pixel_cases(run, c, "ps_call_switch", reference, constant_sets(-1, 4, 4, 0, 0));
    damaged(allocator, dxbc, "ps_call_switch");
  }
  // switch / case: breaks inside ifs of a case (a loop around the cases), a switch inside a switch, a return inside a case, a default with only a break
  {
    const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/ps_switch.dxbc");
    const Compiled c = compile(allocator, dxbc);
    auto reference = [](const Constants& k) -> F4 {
      F4 r = {{0, 0, 0, 0}};
      const float* a = k.a;
      for (int i = 0; i < k.sel[1]; ++i) {
        switch (k.sel[0] + i) {
          case 0: r.v[0] += 1; break;
          case 1: if (a[0] > 0.5f) { r.v[1] += 3; break; } r.v[1] += 1; break;
          case 2: case 3:
            switch (k.sel[2]) { case 0: r.v[2] += 1; break; default: r.v[2] += 2; break; }
            if (a[1] > 0.5f) break;
            r.v[3] += 1; break;
          case 4: if (a[2] > 0.5f) return {{9, 9, 9, 9}}; r.v[3] += 10; break;
          default: for (float& v : r.v) v += 0.5f; break;
        }
        r.v[0] += 0.25f;
      }
      switch (k.sel[3]) { case 1: r.v[1] += 100; break; case 7: r.v[2] += 100; break; }
      return r;
    };
    std::vector<Constants> sets = constant_sets(-1, 5, 5, 1, 1);
    for (Constants& k : sets) k.sel[3] *= 7;
    for (Constants k : constant_sets(0, 0, 2, 0, 0)) { k.sel[3] = 1; sets.push_back(k); }
    run_pixel_cases(run, c, "ps_switch", reference, sets);
    damaged(allocator, dxbc, "ps_switch");
  }
  // hand-made: returns through flags out of loops of nested subroutines; continue / callc / retc and a falling case
  {
    const std::vector<uint8_t> dxbc = pixel_shader(returns_from_loops(), 4);
    const Compiled c = compile(allocator, dxbc);
    run_pixel_cases(run, c, "returns from loops", returns_from_loops_reference, constant_sets(-1, 4, 3, 3, 0));
    damaged(allocator, dxbc, "returns from loops");
  }
  {
    const std::vector<uint8_t> dxbc = pixel_shader(switch_continue(), 2);
    const Compiled c = compile(allocator, dxbc);
    run_pixel_cases(run, c, "switch with continue", switch_continue_reference, constant_sets(0, 7, 7, 1, 1));
    damaged(allocator, dxbc, "switch with continue");
  }
  // Direct3D's nesting limit of subroutines (32): reached, passed; recursion
  {
    UniquePointer<D3d9Program> program;
    const std::vector<uint8_t> deep = call_chain(32, false), deeper = call_chain(33, false), recursive = call_chain(3, true);
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(deep.data(), deep.size()), &program) == status::kSuccess);
    if (program) {
      Constants k;
      CHECK(run.pixel(*program, compile(allocator, deep).tgsi, k, F4{{1, 0, 0, 0}}, "32 calls deep"));
    }
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(deeper.data(), deeper.size()), &program) == status::kInvalidParameter);
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(recursive.data(), recursive.size()), &program) == status::kInvalidParameter);
    // a call of a label no subroutine has, a label twice, a case outside a switch
    const Words base = cat({ins(MOV, {rd(0, 0xF), l4(0, 0, 0, 0)}), ins(CALL, {label(5)}), ins(MOV, {{0x001020F2u, 0}, rv(0)}), ins(RET, {}), ins(LABEL, {label(0)}), ins(RET, {})});
    const std::vector<uint8_t> missing = pixel_shader(base, 1);
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(missing.data(), missing.size()), &program) == status::kInvalidParameter);
    const std::vector<uint8_t> twice = pixel_shader(cat({ins(CALL, {label(0)}), ins(RET, {}), ins(LABEL, {label(0)}), ins(RET, {}), ins(LABEL, {label(0)}), ins(RET, {})}), 1);
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(twice.data(), twice.size()), &program) == status::kInvalidParameter);
    const std::vector<uint8_t> stray = pixel_shader(cat({ins(CASE, {li(0)}), ins(RET, {})}), 1);
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(stray.data(), stray.size()), &program) != status::kSuccess);
    // an interface's function body (fb#, operand type 17): the shader must be specialised first
    const std::vector<uint8_t> body = pixel_shader(cat({ins(RET, {}), ins(LABEL, {{0x00111000u, 0}}), ins(RET, {})}), 1);
    CHECK(D3d9Program::compile_dxbc(allocator, ConstByteView(body.data(), body.size()), &program) == status::kNotSupported);
  }
  // the other stages: a vertex shader and a compute shader run; geometry, domain and hull shaders compile for both devices
  {
    const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/vs_call_switch.dxbc");
    const Compiled c = compile(allocator, dxbc);
    CHECK(c.status == status::kSuccess && c.tgsi_status == status::kSuccess);
    if (c.status == status::kSuccess) {
      int bad = 0;
      for (int sel = -1; sel <= 2; ++sel)
        for (float x : {0.5f, 1.0f}) {
          Constants k;
          k.sel[0] = sel;
          k.a[0] = 0.25f, k.a[1] = 0.5f, k.a[2] = 0.125f, k.a[3] = 1;
          graphics::Vec4 attributes[graphics::kMaxVertexAttributes] = {};
          attributes[0] = {x, 0.25f, 0.5f, 1};
          graphics::ShaderIo io = {};
          c.program->run_vertex(attributes, k.data(), &io);
          const F4 want = sel == 0 ? F4{{1, 0, 0, 1}} : sel == 1 ? (x > 0.75f ? F4{{0.25f, 1, 0.125f, 1}} : F4{{0.25f, 0.5f, 1, 1}}) : F4{{0, 0, 1, 1}};
          const float got[4] = {io.varying[2].x, io.varying[2].y, io.varying[2].z, io.varying[2].w};   // (TEXCOORD0)
          if (!same4(got, want)) ++bad;
        }
      CHECK(bad == 0);
    }
    damaged(allocator, dxbc, "vs_call_switch");
  }
  {
    const std::vector<uint8_t> dxbc = read_file("tests/data/dxbc/cs_call_switch.dxbc");
    const Compiled c = compile(allocator, dxbc);
    CHECK(c.status == status::kSuccess && c.tgsi_status == status::kSuccess);
    if (c.status == status::kSuccess) {
      int bad = 0;
      for (int y : {0, 1})
        for (int z : {3, 40}) {
          Constants k;
          k.sel[0] = 5, k.sel[1] = y, k.sel[2] = z;
          uint32_t result[8] = {};
          for (uint32_t id = 0; id < 8; ++id) {
            graphics::ComputeContext context;
            context.dispatch_id[0] = context.thread_id[0] = id;
            context.group_index = id;
            context.buffers[0] = ByteView(reinterpret_cast<uint8_t*>(result), sizeof result);
            c.program->run_compute(context, k.data());
          }
          for (uint32_t id = 0; id < 8; ++id) {
            uint32_t r = 0;
            switch (id % 4) {
              case 0: r = 10 + 5; break;
              case 1: r = y > 0 ? 21 : 22; break;
              case 2: for (uint32_t q = 0; q < 16; ++q) { r += q; if (r > static_cast<uint32_t>(z)) break; } break;
              default: r = id * 100; break;
            }
            if (id == 5) r += 1000;
            if (id == 6 || id == 7) r += 2000;
            if (result[id] != r) {
              if (bad++ < 4) printf("  cs_call_switch thread %u (y %d, z %d): %u, want %u\n", id, y, z, result[id], r);
            }
          }
        }
      CHECK(bad == 0);
    }
    damaged(allocator, dxbc, "cs_call_switch");
  }
  for (const char* name : {"gs_call_switch", "ds_call_switch", "hs_call_switch"}) {
    const std::string path = std::string("tests/data/dxbc/") + name + ".dxbc";
    const std::vector<uint8_t> dxbc = read_file(path.c_str());
    const Compiled c = compile(allocator, dxbc);
    printf("  %s: compile %x, TGSI %x\n", name, static_cast<unsigned>(c.status), static_cast<unsigned>(c.tgsi_status));
    CHECK(c.status == status::kSuccess && c.tgsi_status == status::kSuccess);
    damaged(allocator, dxbc, name);
  }
  // a program with none of these is compiled as before (nothing rewritten)
  {
    const std::vector<uint8_t> plain = read_file("tests/data/dxbc/ps_tex.dxbc");
    UniquePointer<D3d9Program> program;
    CHECK(!plain.empty() && D3d9Program::compile_dxbc(allocator, ConstByteView(plain.data(), plain.size()), &program) == status::kSuccess);
  }
  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
