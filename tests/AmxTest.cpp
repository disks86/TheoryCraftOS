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

// Host test of Intel AMX. No CPU the tests run on has it, so the instructions are checked against models written from the architecture definition: the integer and floating-point tile matrix
// multiplies on random shapes and data (including the tail of each row and the rows past the configured count, which must be zero), the configuration rules (#GP / #UD), tile loads and stores
// with strides and partial rows, restartable faults, XSAVE / XRSTOR / XSAVEC of the tile state, cpuid and xgetbv. Truncated and bit-flipped encodings must not misbehave under the sanitizers.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"

using namespace emulation;
typedef std::vector<uint8_t> Bytes;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct A final : allocators::IAllocator {
  void* allocate(uint64_t s) override { return malloc(s); }
  void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); }
  void deallocate(void* p) override { free(p); }
};

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

constexpr uint64_t kCfg = 0x20000, kC = 0x20400, kA = 0x20800, kB = 0x20C00, kOut = 0x21000, kXsave = 0x24000;

struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  uint64_t end = 0;
  explicit Machine(const Bytes& code, uint64_t code_at = 0x10000) {
    space.map(0x10000, code_at == 0x10000 ? 0x2000 : 0x1000, process::kProtRead | process::kProtWrite | process::kProtExecute);
    space.write_privileged(code_at, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x4000, process::kProtRead | process::kProtWrite);
    space.map(kXsave, 0x4000, process::kProtRead | process::kProtWrite);
    space.map(0x2F000, 0x2000, process::kProtRead | process::kProtWrite);
    end = code_at + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = code_at; start.stack_pointer = 0x30000;
    cpu.reset(start);
    cpu.state().r[7] = kCfg;
    cpu.state().r[2] = kXsave;
    cpu.state().r[3] = kXsave;
  }
  StopReason run(uint64_t n = 100000) { GuestMemory m(space); return cpu.run(m, n); }
  bool finished() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  bool faulted(int vector) { return run() == StopReason::Fault && cpu.fault().vector == vector && cpu.fault().pc != end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  void peek(uint64_t addr, void* p, size_t n) { space.read_privileged(addr, ByteView(static_cast<uint8_t*>(p), n)); }
};

// --- encodings -----------------------------------------------------------------------------------------------------------------------------

struct Code {
  Bytes b;
  Code& raw(std::initializer_list<uint8_t> v) { b.insert(b.end(), v); return *this; }
  Code& vex(uint8_t op, int pp, int vvvv, bool l = false, bool w = false) { return raw({0xC4, 0xE2, uint8_t((w ? 0x80 : 0) | ((~vvvv & 15) << 3) | (l ? 4 : 0) | pp), op}); }
  Code& ldtilecfg() { return vex(0x49, 0, 0).raw({0x07}); }                    // [rdi]
  Code& sttilecfg() { return vex(0x49, 1, 0).raw({0x02}); }                    // [rdx]
  Code& sttilecfg_rdi() { return vex(0x49, 1, 0).raw({0x07}); }      // [rdi]
  Code& tilerelease() { return vex(0x49, 0, 0).raw({0xC0}); }
  Code& tilezero(int t) { return vex(0x49, 3, 0).raw({uint8_t(0xC0 | (t << 3))}); }
  Code& tileloadd(int t) { return vex(0x4B, 3, 0).raw({uint8_t(0x04 | (t << 3)), 0x37}); }      // [rdi + rsi]
  Code& tileloaddt1(int t) { return vex(0x4B, 1, 0).raw({uint8_t(0x04 | (t << 3)), 0x37}); }
  Code& tilestored(int t) { return vex(0x4B, 2, 0).raw({uint8_t(0x04 | (t << 3)), 0x37}); }
  Code& dot(uint8_t op, int pp, int dst, int s1, int s2) { return vex(op, pp, s2).raw({uint8_t(0xC0 | (dst << 3) | s1)}); }
  Code& mov_rdi(uint32_t v) { return raw({0x48, 0xC7, 0xC7, uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }
  Code& mov_esi(uint32_t v) { return raw({0xBE, uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}); }
  Code& mov_eax_edx(uint32_t a, uint32_t d) { return raw({0xB8, uint8_t(a), uint8_t(a >> 8), uint8_t(a >> 16), uint8_t(a >> 24), 0xBA, uint8_t(d), uint8_t(d >> 8), uint8_t(d >> 16), uint8_t(d >> 24)}); }
  Code& xsave() { return raw({0x0F, 0xAE, 0x23}); }      // [rbx]
  Code& xrstor() { return raw({0x0F, 0xAE, 0x2B}); }
  Code& xsavec() { return raw({0x0F, 0xC7, 0x23}); }
  Code& end() { return raw({0x0F, 0x0B}); }
};

struct Config {
  uint8_t palette = 1;
  uint16_t colsb[16] = {};
  uint8_t rows[16] = {};
  void bytes(uint8_t out[64]) const {
    memset(out, 0, 64);
    out[0] = palette;
    for (int i = 0; i < 16; ++i) { out[16 + 2 * i] = uint8_t(colsb[i]); out[17 + 2 * i] = uint8_t(colsb[i] >> 8); out[48 + i] = rows[i]; }
  }
};

static void load_config(Machine& m, const Config& c) { uint8_t raw[64]; c.bytes(raw); m.poke(kCfg, raw, 64); }

static Config full_config() {
  Config c;
  for (int t = 0; t < 8; ++t) { c.rows[t] = 16; c.colsb[t] = 64; }
  return c;
}

// --- configuration -------------------------------------------------------------------------------------------------------------------------

static void test_config() {
  {
    Config c = full_config();
    c.rows[3] = 5; c.colsb[3] = 40;
    Machine m(Code().ldtilecfg().sttilecfg().end().b);
    load_config(m, c);
    m.cpu.state().r[2] = kXsave;
    CHECK(m.finished());
    uint8_t want[64], got[64];
    c.bytes(want);
    m.peek(kXsave, got, 64);
    CHECK(memcmp(want, got, 64) == 0);
  }
  struct Bad { const char* what; Config c; };
  std::vector<Bad> bad;
  { Config c = full_config(); c.palette = 2; bad.push_back({"palette 2", c}); }
  { Config c = full_config(); c.colsb[1] = 65; bad.push_back({"65 bytes", c}); }
  { Config c = full_config(); c.rows[1] = 17; bad.push_back({"17 rows", c}); }
  { Config c = full_config(); c.rows[1] = 0; bad.push_back({"rows without bytes", c}); }
  { Config c = full_config(); c.colsb[1] = 0; bad.push_back({"bytes without rows", c}); }
  { Config c = full_config(); c.rows[9] = 1; c.colsb[9] = 4; bad.push_back({"tile 9", c}); }
  for (const Bad& b : bad) {
    Machine m(Code().ldtilecfg().end().b);
    load_config(m, b.c);
    CHECK(m.faulted(13));
  }
  {
    Config c = full_config();
    Machine m(Code().ldtilecfg().end().b);
    uint8_t raw[64];
    c.bytes(raw);
    raw[5] = 1;      // reserved
    m.poke(kCfg, raw, 64);
    CHECK(m.faulted(13));
  }
  {      // tilerelease clears the configuration: sttilecfg then stores zeros
    Machine m(Code().ldtilecfg().tilerelease().sttilecfg().end().b);
    load_config(m, full_config());
    CHECK(m.finished());
    uint8_t got[64], zero[64] = {};
    m.peek(kXsave, got, 64);
    CHECK(memcmp(got, zero, 64) == 0);
  }
  {      // so does a configuration with palette 0
    Machine m(Code().ldtilecfg().sttilecfg().end().b);
    Config c = full_config();
    c.palette = 0;
    load_config(m, c);
    CHECK(m.finished());
    uint8_t got[64], zero[64] = {};
    m.peek(kXsave, got, 64);
    CHECK(memcmp(got, zero, 64) == 0);
  }
  {      // instructions without a configuration, with bad registers, or with bad encodings are #UD
    Machine m(Code().tilezero(0).end().b);
    CHECK(m.faulted(6));
    Machine m2(Code().mov_esi(64).tileloadd(0).end().b);
    CHECK(m2.faulted(6));
    Machine m3(Code().ldtilecfg().vex(0x49, 3, 0).raw({0xC0 | (7 << 3)}).end().b);      // tilezero tmm7: fine
    load_config(m3, full_config());
    CHECK(m3.finished());
    Machine m4(Code().ldtilecfg().raw({0xC4, 0x62, 0x7B, 0x49, 0xC0}).end().b);      // VEX.R clear: tmm8
    load_config(m4, full_config());
    CHECK(m4.faulted(6));
    Machine m5(Code().ldtilecfg().vex(0x49, 3, 0, true).raw({0xC0}).end().b);      // VEX.L
    load_config(m5, full_config());
    CHECK(m5.faulted(6));
    Machine m6(Code().ldtilecfg().vex(0x49, 3, 0, false, true).raw({0xC0}).end().b);      // VEX.W
    load_config(m6, full_config());
    CHECK(m6.faulted(6));
    Machine m7(Code().ldtilecfg().vex(0x49, 3, 1).raw({0xC0}).end().b);      // vvvv in use
    load_config(m7, full_config());
    CHECK(m7.faulted(6));
    Machine m8(Code().ldtilecfg().vex(0x4B, 3, 0).raw({0x07}).end().b);      // tileloadd without a SIB index
    load_config(m8, full_config());
    CHECK(m8.faulted(6));
    Machine m9(Code().ldtilecfg().vex(0x49, 0, 0).raw({0xC1}).end().b);      // 49 NP with a register modrm other than C0
    load_config(m9, full_config());
    CHECK(m9.faulted(6));
    Machine m10(Code().ldtilecfg().tilerelease().tilezero(0).end().b);      // zero after release
    load_config(m10, full_config());
    CHECK(m10.faulted(6));
  }
}

// --- loads and stores ----------------------------------------------------------------------------------------------------------------------

static void test_load_store() {
  for (int iter = 0; iter < 200; ++iter) {
    Config c = full_config();
    const int t = rnd() % 8, u = rnd() % 8;
    const uint32_t rows = 1 + rnd() % 16, colsb = 1 + rnd() % 64, stride = 64 + 4 * (rnd() % 16);
    c.rows[t] = uint8_t(rows); c.colsb[t] = uint16_t(colsb);
    c.rows[u] = uint8_t(rows); c.colsb[u] = uint16_t(colsb);
    Code code;
    code.ldtilecfg().mov_rdi(kC).mov_esi(stride);
    if (rnd() & 1) code.tileloadd(t); else code.tileloaddt1(t);
    // out through memory at the same stride, back into another tile, out again
    code.mov_rdi(kOut).tilestored(t).mov_rdi(kOut).tileloadd(u).mov_rdi(0x22000).tilestored(u).end();
    Machine m(code.b);
    load_config(m, c);
    uint8_t mem[16 * 124 + 64];
    for (uint8_t& b : mem) b = uint8_t(rnd());
    m.poke(kC, mem, 16 * stride + 64);
    CHECK(m.finished());
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t want[64] = {}, got[64] = {};
      m.peek(kC + uint64_t(r) * stride, want, colsb);
      m.peek(0x22000 + uint64_t(r) * stride, got, colsb);
      CHECK(memcmp(want, got, colsb) == 0);
    }
  }
  {      // a fault in the middle: start_row is kept, and the restarted instruction continues from that row
    Code code;
    code.ldtilecfg().mov_rdi(0x31000 - 5 * 64).mov_esi(64).tileloadd(1).mov_eax_edx(0x40000, 0).xsave().end();
    Machine m(code.b);
    load_config(m, full_config());
    uint8_t first[1024];
    for (uint8_t& b : first) b = uint8_t(rnd());
    m.poke(0x31000 - 5 * 64, first, 5 * 64);
    CHECK(m.faulted(14));
    CHECK(m.cpu.fault().address == 0x31000);
    m.space.map(0x31000, 0x1000, process::kProtRead | process::kProtWrite);
    m.poke(0x31000 - 5 * 64, "\xAA", 1);      // (the first rows are not read again)
    uint8_t rest[11 * 64];
    for (uint8_t& b : rest) b = uint8_t(rnd());
    m.poke(0x31000, rest, sizeof(rest));
    CHECK(m.finished());
    uint8_t got[1024];
    m.peek(kXsave + 2816 + 1024, got, 1024);
    CHECK(memcmp(got, first, 5 * 64) == 0);
    CHECK(memcmp(got + 5 * 64, rest, sizeof(rest)) == 0);
  }
  {      // a store to an unmapped address
    Config c = full_config();
    Code code;
    code.ldtilecfg().mov_rdi(0x50000).mov_esi(64).tilestored(1).end();
    Machine m(code.b);
    load_config(m, c);
    CHECK(m.faulted(14));
    CHECK(m.cpu.fault().address == 0x50000);
  }
}

// --- matrix multiplies ---------------------------------------------------------------------------------------------------------------------

static float half_to_float(uint16_t h) {
  const int sign = h >> 15, exp = (h >> 10) & 31, man = h & 1023;
  double v;
  if (exp == 0) v = std::ldexp(man, -24);
  else v = std::ldexp(1024 + man, exp - 25);
  return float(sign ? -v : v);
}
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float from_bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float flush(float f) { return (bits(f) & 0x7F800000u) == 0 ? from_bits(bits(f) & 0x80000000u) : f; }

// Runs `op` on random tiles of a random shape and returns the destination tile's 1024 bytes (through XSAVE) and the model's.
struct Shape { uint32_t m, k, n; };

static void run_dot(uint8_t op, int pp, int kind) {
  for (int iter = 0; iter < 120; ++iter) {
    Shape s = {1 + uint32_t(rnd() % 16), 1 + uint32_t(rnd() % 16), 1 + uint32_t(rnd() % 16)};
    if (iter == 0) s = {16, 16, 16};
    if (iter == 1) s = {1, 1, 1};
    Config c = full_config();
    int tile[3] = {int(rnd() % 8), 0, 0};
    do { tile[1] = rnd() % 8; } while (tile[1] == tile[0]);
    do { tile[2] = rnd() % 8; } while (tile[2] == tile[0] || tile[2] == tile[1]);
    c.rows[tile[0]] = uint8_t(s.m); c.colsb[tile[0]] = uint16_t(4 * s.n);
    c.rows[tile[1]] = uint8_t(s.m); c.colsb[tile[1]] = uint16_t(4 * s.k);
    c.rows[tile[2]] = uint8_t(s.k); c.colsb[tile[2]] = uint16_t(4 * s.n);
    uint8_t dst[1024], a[1024], b[1024];
    for (int i = 0; i < 1024; ++i) { dst[i] = uint8_t(rnd()); a[i] = uint8_t(rnd()); b[i] = uint8_t(rnd()); }
    if (kind >= 4) {      // floating point: sane exponents, with zeros and denormals
      auto fill = [&](uint8_t* t, bool is_bf16, bool is_dst) {
        for (int i = 0; i < 512; ++i) {
          uint16_t h;
          if (is_dst) {
            const float f = float(int(rnd() % 2001) - 1000) / 16;
            const uint32_t u = bits(rnd() % 9 == 0 ? 0.0f : f);
            t[4 * (i / 2) + 0] = uint8_t(u); t[4 * (i / 2) + 1] = uint8_t(u >> 8); t[4 * (i / 2) + 2] = uint8_t(u >> 16); t[4 * (i / 2) + 3] = uint8_t(u >> 24);
            ++i;
            continue;
          }
          const uint32_t sign = rnd() & 1;
          switch (rnd() % 10) {
            case 0: h = uint16_t(sign << 15); break;      // zero
            case 1: h = uint16_t((sign << 15) | (rnd() & (is_bf16 ? 0x7F : 0x3FF))); break;      // denormal
            default: h = is_bf16 ? uint16_t((sign << 15) | ((0x70 + rnd() % 24) << 7) | (rnd() & 0x7F)) : uint16_t((sign << 15) | ((8 + rnd() % 14) << 10) | (rnd() & 0x3FF)); break;
          }
          t[2 * i] = uint8_t(h); t[2 * i + 1] = uint8_t(h >> 8);
        }
      };
      fill(dst, kind == 4, true); fill(a, kind == 4, false); fill(b, kind == 4, false);
    }
    Code code;
    code.ldtilecfg().mov_esi(64);
    code.mov_rdi(kC).tileloadd(tile[0]).mov_rdi(kA).tileloadd(tile[1]).mov_rdi(kB).tileloadd(tile[2]);
    code.dot(op, pp, tile[0], tile[1], tile[2]);
    code.mov_eax_edx(0x40000, 0).xsave().end();
    Machine m(code.b);
    load_config(m, c);
    m.poke(kC, dst, 1024); m.poke(kA, a, 1024); m.poke(kB, b, 1024);
    CHECK(m.finished());
    uint8_t got[1024];
    m.peek(kXsave + 2816 + 1024 * tile[0], got, 1024);
    // the model: rows of the sources as loaded (colsb bytes per row, the rest zero)
    uint8_t want[1024] = {};
    for (uint32_t r = 0; r < s.m; ++r) {
      for (uint32_t n = 0; n < s.n; ++n) {
        uint32_t acc;
        memcpy(&acc, dst + 64 * r + 4 * n, 4);
        float f = from_bits(acc);
        for (uint32_t k = 0; k < s.k; ++k) {
          const uint8_t* ar = a + 64 * r + 4 * k;
          const uint8_t* br = b + 64 * k + 4 * n;
          if (kind < 4) {
            const bool a_signed = kind <= 1, b_signed = kind == 0 || kind == 2;
            for (int j = 0; j < 4; ++j) {
              const int64_t x = a_signed ? int8_t(ar[j]) : ar[j], y = b_signed ? int8_t(br[j]) : br[j];
              acc = uint32_t(acc + uint32_t(int32_t(x * y)));
            }
            continue;
          }
          auto elem = [&](const uint8_t* p, int i) -> float {
            const uint16_t h = uint16_t(p[2 * i] | (p[2 * i + 1] << 8));
            return kind == 4 ? flush(from_bits(uint32_t(h) << 16)) : half_to_float(h);
          };
          const float a0 = elem(ar, 0), a1 = elem(ar, 1), b0 = elem(br, 0), b1 = elem(br, 1);
          auto step = [&](float x, float y) { f = flush(std::fmaf(x, y, flush(f))); };
          if (kind == 4 || kind == 5) { step(a0, b0); step(a1, b1); }
          else if (kind == 6) { step(a0, b1); step(a1, b0); }
          else { step(a0, b0); step(-a1, b1); }
        }
        if (kind >= 4) acc = bits(f);
        memcpy(want + 64 * r + 4 * n, &acc, 4);
      }
    }
    if (memcmp(want, got, 1024) != 0) {
      printf("  op %02x pp %d kind %d shape %u/%u/%u\n", op, pp, kind, s.m, s.k, s.n);
      for (int i = 0; i < 1024; ++i) if (want[i] != got[i]) { printf("  first difference at byte %d: want %02x got %02x\n", i, want[i], got[i]); break; }
    }
    CHECK(memcmp(want, got, 1024) == 0);
  }
}

static void test_dot() {
  run_dot(0x5E, 3, 0);      // tdpbssd
  run_dot(0x5E, 2, 1);      // tdpbsud
  run_dot(0x5E, 1, 2);      // tdpbusd
  run_dot(0x5E, 0, 3);      // tdpbuud
  run_dot(0x5C, 2, 4);      // tdpbf16ps
  run_dot(0x5C, 3, 5);      // tdpfp16ps
  run_dot(0x6C, 1, 6);      // tcmmimfp16ps
  run_dot(0x6C, 0, 7);      // tcmmrlfp16ps
  {      // an overflow gives infinity
    Config c = full_config();
    c.rows[0] = 1; c.colsb[0] = 4; c.rows[1] = 1; c.colsb[1] = 4; c.rows[2] = 1; c.colsb[2] = 4;
    Code code;
    code.ldtilecfg().mov_esi(64).mov_rdi(kC).tileloadd(0).mov_rdi(kA).tileloadd(1).mov_rdi(kB).tileloadd(2).dot(0x5C, 2, 0, 1, 2).mov_eax_edx(0x40000, 0).xsave().end();
    Machine m(code.b);
    load_config(m, c);
    const uint8_t hot[4] = {0x7F, 0x7F, 0, 0}, zero[4] = {};
    m.poke(kC, zero, 4); m.poke(kA, hot, 4); m.poke(kB, hot, 4);
    CHECK(m.finished());
    uint32_t got;
    m.peek(kXsave + 2816, &got, 4);
    CHECK(got == 0x7F800000u);
  }
  // shapes that do not fit are #UD, as are repeated tiles and tiles that are not configured
  struct Case { const char* what; Config c; int dst, s1, s2; };
  std::vector<Case> cases;
  { Config c = full_config(); c.rows[0] = 4; c.colsb[0] = 16; c.rows[1] = 4; c.colsb[1] = 8; c.rows[2] = 3; c.colsb[2] = 16; cases.push_back({"K mismatch", c, 0, 1, 2}); }
  { Config c = full_config(); c.rows[0] = 4; c.colsb[0] = 16; c.rows[1] = 5; c.colsb[1] = 8; c.rows[2] = 2; c.colsb[2] = 16; cases.push_back({"M mismatch", c, 0, 1, 2}); }
  { Config c = full_config(); c.rows[0] = 4; c.colsb[0] = 16; c.rows[1] = 4; c.colsb[1] = 8; c.rows[2] = 2; c.colsb[2] = 12; cases.push_back({"N mismatch", c, 0, 1, 2}); }
  { Config c = full_config(); cases.push_back({"same tile", c, 0, 0, 1}); }
  { Config c = full_config(); cases.push_back({"same tile 2", c, 1, 2, 1}); }
  { Config c = full_config(); c.rows[2] = 0; c.colsb[2] = 0; cases.push_back({"not configured", c, 0, 1, 2}); }
  for (const Case& cs : cases) {
    Machine m(Code().ldtilecfg().dot(0x5E, 3, cs.dst, cs.s1, cs.s2).end().b);
    load_config(m, cs.c);
    if (!m.faulted(6)) printf("  case %s\n", cs.what);
    CHECK(m.cpu.fault().vector == 6);
  }
}


// --- XSAVE, cpuid, malformed encodings -----------------------------------------------------------------------------------------------------

static uint64_t q(Machine& m, uint64_t at) { uint64_t v = 0; m.peek(at, &v, 8); return v; }

static void test_xsave() {
  Config c = full_config();
  c.rows[2] = 7; c.colsb[2] = 28;
  uint8_t data[1024];
  for (uint8_t& b : data) b = uint8_t(rnd());
  {      // standard format: XSTATE_BV bits 17 and 18, the configuration at 2752, the tiles at 2816
    Code code;
    code.ldtilecfg().mov_rdi(kC).mov_esi(64).tileloadd(2).mov_eax_edx(0x60000, 0).xsave().end();
    Machine m(code.b);
    load_config(m, c);
    m.poke(kC, data, 1024);
    CHECK(m.finished());
    CHECK((q(m, kXsave + 512) & 0x60000) == 0x60000);
    uint8_t cfg[64], want[64];
    c.bytes(want);
    m.peek(kXsave + 2752, cfg, 64);
    CHECK(memcmp(cfg, want, 64) == 0);
    uint8_t tile[1024] = {};
    m.peek(kXsave + 2816 + 2 * 1024, tile, 1024);
    for (int r = 0; r < 7; ++r) CHECK(memcmp(tile + 64 * r, data + 64 * r, 28) == 0);
    for (int r = 0; r < 16; ++r) for (int i = (r < 7 ? 28 : 0); i < 64; ++i) CHECK(tile[64 * r + i] == 0);
  }
  {      // restore into a machine that has no tiles; xrstor with the bits clear puts them back to the initial state
    Code save;
    save.ldtilecfg().mov_rdi(kC).mov_esi(64).tileloadd(2).mov_eax_edx(0x60000, 0).xsave().end();
    Machine m(save.b);
    load_config(m, c);
    m.poke(kC, data, 1024);
    CHECK(m.finished());
    uint8_t image[2816 + 8192];
    m.peek(kXsave, image, sizeof(image));
    Code restore;
    restore.mov_eax_edx(0x60000, 0).xrstor().mov_rdi(kOut).sttilecfg_rdi().mov_esi(64).mov_rdi(kOut + 0x400).tilestored(2).end();
    Machine r(restore.b);
    r.poke(kXsave, image, sizeof(image));
    CHECK(r.finished());
    uint8_t cfg[64], want[64];
    c.bytes(want);
    r.peek(kOut, cfg, 64);
    CHECK(memcmp(cfg, want, 64) == 0);
    for (int row = 0; row < 7; ++row) {
      uint8_t got[28];
      r.peek(kOut + 0x400 + 64 * row, got, 28);
      CHECK(memcmp(got, data + 64 * row, 28) == 0);
    }
    // XSTATE_BV without the tile bits: the state becomes initial (tilecfg all zero, tiles unusable)
    uint64_t zero_bv = q(r, kXsave + 512) & ~uint64_t{0x60000};
    memcpy(image + 512, &zero_bv, 8);
    Machine r2(Code().ldtilecfg().mov_eax_edx(0x60000, 0).xrstor().tilezero(0).end().b);
    load_config(r2, c);
    r2.poke(kXsave, image, sizeof(image));
    CHECK(r2.faulted(6));
    // an invalid configuration in the image is #GP
    image[2752] = 2;
    uint64_t bv = 0x60000;
    memcpy(image + 512, &bv, 8);
    Machine r3(Code().mov_eax_edx(0x60000, 0).xrstor().end().b);
    r3.poke(kXsave, image, sizeof(image));
    CHECK(r3.faulted(13));
    // tile data without a configuration is #GP
    image[2752] = 1;
    bv = 0x40000;
    memcpy(image + 512, &bv, 8);
    Machine r4(Code().mov_eax_edx(0x60000, 0).xrstor().end().b);
    r4.poke(kXsave, image, sizeof(image));
    CHECK(r4.faulted(13));
  }
  {      // nothing configured: the components are in the initial state and XSTATE_BV says so
    Machine m(Code().mov_eax_edx(0x60007, 0).xsave().end().b);
    CHECK(m.finished());
    CHECK((q(m, kXsave + 512) & 0x60000) == 0);
  }
  {      // xsavec packs the components: x87, SSE, AVX (576 + 256 = 832), then the tiles on a 64-byte boundary
    Code code;
    code.ldtilecfg().mov_rdi(kC).mov_esi(64).tileloadd(2).mov_eax_edx(0x60007, 0).xsavec().end();
    Machine m(code.b);
    load_config(m, c);
    m.poke(kC, data, 1024);
    CHECK(m.finished());
    CHECK(q(m, kXsave + 520) == (0x60007ull | (1ull << 63)));
    uint8_t cfg[64], want[64];
    c.bytes(want);
    m.peek(kXsave + 832, cfg, 64);
    CHECK(memcmp(cfg, want, 64) == 0);
    uint8_t tile[28];
    m.peek(kXsave + 896 + 2 * 1024 + 64, tile, 28);
    CHECK(memcmp(tile, data + 64, 28) == 0);
  }
}

static void test_cpuid() {
  struct R { uint32_t a, b, c, d; };
  auto cpuid = [](uint32_t leaf, uint32_t sub) {
    Machine m({0xB8, uint8_t(leaf), uint8_t(leaf >> 8), 0, 0, 0xB9, uint8_t(sub), 0, 0, 0, 0x0F, 0xA2, 0x0F, 0x0B});
    CHECK(m.finished());
    const auto& st = m.cpu.state();
    return R{uint32_t(st.r[0]), uint32_t(st.r[3]), uint32_t(st.r[1]), uint32_t(st.r[2])};
  };
  const R l7 = cpuid(7, 0), l71 = cpuid(7, 1);
  CHECK(((l7.d >> 22) & 1) && ((l7.d >> 24) & 1) && ((l7.d >> 25) & 1));      // AMX-BF16, AMX-TILE, AMX-INT8
  CHECK((l71.a >> 21) & 1);      // AMX-FP16
  CHECK((l71.d >> 8) & 1);       // AMX-COMPLEX
  const R d0 = cpuid(0xD, 0), d17 = cpuid(0xD, 17), d18 = cpuid(0xD, 18);
  CHECK((d0.a & 0x60000) == 0x60000 && d0.b == 11136);
  CHECK(d17.a == 64 && d17.b == 2752 && d18.a == 8192 && d18.b == 2816);
  const R p0 = cpuid(0x1D, 0), p1 = cpuid(0x1D, 1), t = cpuid(0x1E, 0);
  CHECK(p0.a == 1);
  CHECK((p1.a & 0xFFFF) == 8192 && (p1.a >> 16) == 1024 && (p1.b & 0xFFFF) == 64 && (p1.b >> 16) == 8 && p1.c == 16);
  CHECK((t.b & 0xFF) == 16 && ((t.b >> 8) & 0xFFFF) == 64);
  Machine m({0x31, 0xC9, 0x0F, 0x01, 0xD0, 0x0F, 0x0B});      // xgetbv
  CHECK(m.finished());
  CHECK((m.cpu.state().r[0] & 0x60000) == 0x60000);
}

// Every prefix of an encoding (the program ends where the bytes do, at a page boundary) and every single-bit flip of it: whatever happens, it must happen safely.
static void test_malformed() {
  Code ok;
  ok.ldtilecfg().mov_rdi(kC).mov_esi(64).tileloadd(1).tileloadd(2).tilezero(3).dot(0x5E, 3, 0, 1, 2).tilestored(0).dot(0x5C, 2, 3, 1, 2).dot(0x6C, 0, 4, 1, 2).sttilecfg().tilerelease().end();
  Config c = full_config();
  c.rows[0] = 4; c.colsb[0] = 16; c.rows[1] = 4; c.colsb[1] = 8; c.rows[2] = 2; c.colsb[2] = 16;
  for (size_t cut = 0; cut < ok.b.size(); ++cut) {
    Bytes b(ok.b.begin(), ok.b.begin() + cut);
    Machine m(b, 0x11000 - cut);      // (the code ends at the end of the mapped page)
    load_config(m, c);
    m.run();
  }
  for (size_t i = 0; i < ok.b.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      Bytes b = ok.b;
      b[i] ^= uint8_t(1 << bit);
      Machine m(b);
      load_config(m, c);
      m.run(2000);
    }
  }
}

int main() {
  test_config();
  test_load_store();
  test_dot();
  test_xsave();
  test_cpuid();
  test_malformed();
  if (g_failed) { printf("AmxTest: %d failed\n", g_failed); return 1; }
  printf("AmxTest: all passed\n");
  return 0;
}
