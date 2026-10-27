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

// Host test of the x87 emulation (SoftFloatExt, the register stack and the instruction decoder). Wherever the host has a real x87 (x86-64 Linux) it is the oracle: random
// operands, control words and instructions are run on both and the results, status words, condition codes, tags and stored memory must be identical. The transcendental
// functions are compared with a tolerance of a few units in the last place (neither side rounds them exactly). The rest are hand-written checks that need no x87 host.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"
#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#define HOST_X87 1
#endif

using namespace emulation;
using namespace emulation::softfloat;
using namespace emulation::softfloat::ext;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct A final : allocators::IAllocator {
  void* allocate(uint64_t s) override { return malloc(s); }
  void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); }
  void deallocate(void* p) override { free(p); }
};
typedef std::vector<uint8_t> Bytes;

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static Ext rnd_ext(int mode) {
  Ext e;
  const uint64_t r = rnd();
  switch (r & 15) {
    case 0: e.mant = 0; e.se = (rnd() & 1) << 15; return e;                       // zero
    case 1: e.mant = 0x8000000000000000ull; e.se = ((rnd() & 1) << 15) | 0x7FFF; return e;      // inf
    case 2: e.mant = 0xC000000000000000ull | (rnd() >> 3); e.se = ((rnd() & 1) << 15) | 0x7FFF; if (e.mant == 0x8000000000000000ull) e.mant |= 1; return e;      // quiet NaN
    case 3: e.mant = 0x8000000000000000ull | (rnd() >> 2) | 1; e.se = ((rnd() & 1) << 15) | 0x7FFF; e.mant &= ~0x4000000000000000ull; if (!(e.mant & 0x3FFFFFFFFFFFFFFFull)) e.mant |= 1; return e;      // signaling
    case 4: e.mant = rnd() >> (rnd() % 64); e.se = (rnd() & 1) << 15; return e;      // denormal-ish (exponent field 0)
    case 5: e.mant = 0x8000000000000000ull | rnd(); e.se = ((rnd() & 1) << 15) | (0x7FFE - (rnd() & 3)); return e;      // huge
    case 6: e.mant = 0x8000000000000000ull | rnd(); e.se = ((rnd() & 1) << 15) | (1 + (rnd() & 3)); return e;      // tiny
    default: e.mant = 0x8000000000000000ull | rnd(); e.se = ((rnd() & 1) << 15) | (0x3FFF - 40 + (rnd() % 80)); return e;
  }
}
static bool bad_operand(const Ext& e) { return classify(e) == Class::Unsupported; }

#ifdef HOST_X87
static long double to_ld(const Ext& e) { long double l = 0; memcpy(&l, &e.mant, 8); memcpy(((char*)&l) + 8, &e.se, 2); return l; }
static Ext from_ld(long double l) { Ext e; memcpy(&e.mant, &l, 8); memcpy(&e.se, ((char*)&l) + 8, 2); return e; }

#define BIN(NAME, INSN) static long double NAME(long double a, long double b, uint16_t cw, uint16_t* sw) { long double r; uint16_t s; \
  asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[b]\n\tfldt %[a]\n\t" INSN "\n\tfnstsw %[s]\n\tfstpt %[r]\n\tfstp %%st(0)\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a), [b] "m"(b) : "memory", "st", "st(1)"); *sw = s; return r; }
BIN(h_add, "faddp %%st, %%st(1)")
BIN(h_sub, "fsubp %%st, %%st(1)")
BIN(h_mul, "fmulp %%st, %%st(1)")
BIN(h_div, "fdivp %%st, %%st(1)")
// fprem family: st0 = a, st1 = b; result st0 stays
#define REM(NAME, INSN) static long double NAME(long double a, long double b, uint16_t cw, uint16_t* sw) { long double r; uint16_t s; \
  asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[b]\n\tfldt %[a]\n\t" INSN "\n\tfnstsw %[s]\n\tfstpt %[r]\n\tfstp %%st(0)\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a), [b] "m"(b) : "memory", "st", "st(1)"); *sw = s; return r; }
REM(h_prem, "fprem")
REM(h_prem1, "fprem1")
REM(h_scale, "fscale")
static long double h_sqrt(long double a, uint16_t cw, uint16_t* sw) { long double r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfsqrt\n\tfnstsw %[s]\n\tfstpt %[r]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static long double h_rint(long double a, uint16_t cw, uint16_t* sw) { long double r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfrndint\n\tfnstsw %[s]\n\tfstpt %[r]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static double h_to_double(long double a, uint16_t cw, uint16_t* sw) { double r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfstpl %[r]\n\tfnstsw %[s]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static float h_to_single(long double a, uint16_t cw, uint16_t* sw) { float r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfstps %[r]\n\tfnstsw %[s]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static int64_t h_to_i64(long double a, uint16_t cw, uint16_t* sw) { int64_t r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfistpll %[r]\n\tfnstsw %[s]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static int32_t h_to_i32(long double a, uint16_t cw, uint16_t* sw) { int32_t r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfistpl %[r]\n\tfnstsw %[s]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static int16_t h_to_i16(long double a, uint16_t cw, uint16_t* sw) { int16_t r; uint16_t s; asm volatile("fldcw %[cw]\n\tfnclex\n\tfldt %[a]\n\tfistps %[r]\n\tfnstsw %[s]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [cw] "m"(cw), [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static long double h_from_double(double a, uint16_t* sw) { long double r; uint16_t s; asm volatile("fnclex\n\tfldl %[a]\n\tfnstsw %[s]\n\tfstpt %[r]\n\tfnclex" : [r] "=m"(r), [s] "=m"(s) : [a] "m"(a) : "memory", "st"); *sw = s; return r; }
static int h_cmp(long double a, long double b, bool ordered, uint16_t* sw) { uint16_t s;
  if (ordered) asm volatile("fnclex\n\tfldt %[b]\n\tfldt %[a]\n\tfcompp\n\tfnstsw %[s]\n\tfnclex" : [s] "=m"(s) : [a] "m"(a), [b] "m"(b) : "memory", "st", "st(1)");
  else asm volatile("fnclex\n\tfldt %[b]\n\tfldt %[a]\n\tfucompp\n\tfnstsw %[s]\n\tfnclex" : [s] "=m"(s) : [a] "m"(a), [b] "m"(b) : "memory", "st", "st(1)");
  *sw = s; const int c0 = (s >> 8) & 1, c2 = (s >> 10) & 1, c3 = (s >> 14) & 1;
  if (c0 && c2 && c3) return 2; if (c0) return -1; if (c3) return 0; return 1; }


static int failures = 0, reported = 0;
static void report(const char* what, Ext a, Ext b, uint16_t cw, Ext got, uint32_t gf, Ext want, uint32_t wf) {
  ++failures;
  if (reported++ < 25) printf("%s cw=%04x a=%04x:%016llx b=%04x:%016llx got %04x:%016llx f=%02x want %04x:%016llx f=%02x\n", what, cw, a.se, (unsigned long long)a.mant, b.se, (unsigned long long)b.mant, got.se, (unsigned long long)got.mant, gf, want.se, (unsigned long long)want.mant, wf);
}
static void report2(const char* what, Ext a, uint16_t cw, uint64_t got, uint32_t gf, uint64_t want, uint32_t wf) {
  ++failures;
  if (reported++ < 25) printf("%s cw=%04x a=%04x:%016llx got %016llx f=%02x want %016llx f=%02x\n", what, cw, a.se, (unsigned long long)a.mant, (unsigned long long)got, gf, (unsigned long long)want, wf);
}
static bool same(Ext a, Ext b) {
  if (classify(a) == Class::QuietNan && classify(b) == Class::QuietNan) return a.mant == b.mant && a.se == b.se;
  if (a.mant == 0 && b.mant == 0 && (a.se & 0x7FFF) == 0 && (b.se & 0x7FFF) == 0) return a.se == b.se;
  return a.mant == b.mant && a.se == b.se;
}


static void test_core_against_host(int iterations) {
  for (int i = 0; i < iterations; ++i) {
    const uint16_t pc = (uint16_t[]){0x000, 0x200, 0x300, 0x300}[rnd() & 3];
    const uint16_t rc = (rnd() & 3) << 10;
    const uint16_t cw = 0x37F;
    uint16_t control = (cw & ~0xF00) | pc | rc;
    Ext a = rnd_ext(0), b = rnd_ext(0);
    if (bad_operand(a) || bad_operand(b)) continue;
    if ((rnd() & 3) == 0) { b = a; b.se = (b.se & 0x8000) | ((b.se & 0x7FFF) + (rnd() % 5) - 2) ; if ((rnd() & 1)) b.se ^= 0x8000; if (bad_operand(b)) continue; }
    Env env; env.mode = (Rounding)((control >> 10) & 3);
    const uint32_t precision = precision_bits((control >> 8) & 3);
    uint16_t sw;
    auto check = [&](const char* name, Ext got, Ext want, uint32_t hostflags, Env& e, uint16_t) {
      const bool c1 = (hostflags >> 9) & 1;
      if (!same(got, want) || (e.flags & 0x3F) != (hostflags & 0x3F) || (strncmp(name, "prem", 4) != 0 && ((e.flags & 0x20) && e.rounded_up) != c1 && !(e.flags & 1))) report(name, a, b, control, got, e.flags & 0x3F | (e.rounded_up ? 0x200 : 0), want, hostflags & 0x3F | (hostflags & 0x200));
    };
    { Env e = env; Ext g = add(a, b, precision, e); Ext w = from_ld(h_add(to_ld(a), to_ld(b), control, &sw)); check("add", g, w, sw, e, sw); }
    { Env e = env; Ext g = sub(a, b, precision, e); Ext w = from_ld(h_sub(to_ld(a), to_ld(b), control, &sw)); check("sub", g, w, sw, e, sw); }
    { Env e = env; Ext g = mul(a, b, precision, e); Ext w = from_ld(h_mul(to_ld(a), to_ld(b), control, &sw)); check("mul", g, w, sw, e, sw); }
    { Env e = env; Ext g = div(a, b, precision, e); Ext w = from_ld(h_div(to_ld(a), to_ld(b), control, &sw)); check("div", g, w, sw, e, sw); }
    { Env e = env; Ext g = sqrt(a, precision, e); Ext w = from_ld(h_sqrt(to_ld(a), control, &sw)); check("sqrt", g, w, sw, e, sw); }
    { Env e = env; Ext g = round_to_integer(a, env.mode, e); Ext w = from_ld(h_rint(to_ld(a), control, &sw)); check("rint", g, w, sw, e, sw); }
    { Env e = env; Ext g = scale(a, b, e); Ext w = from_ld(h_scale(to_ld(a), to_ld(b), control, &sw)); check("scale", g, w, sw, e, sw); }
    for (int k = 0; k < 2; ++k) {
      Env e = env; bool complete; uint32_t qb;
      Ext g = remainder(a, b, k == 1, &complete, &qb, e);
      Ext w = from_ld(k ? h_prem1(to_ld(a), to_ld(b), control, &sw) : h_prem(to_ld(a), to_ld(b), control, &sw));
      const bool hw_incomplete = (sw >> 10) & 1;
      if (hw_incomplete != !complete) { report(k ? "prem1-c2" : "prem-c2", a, b, control, g, complete, w, hw_incomplete); continue; }
      if (complete) {
        const uint32_t hwq = (((sw >> 8) & 1) << 2) | (((sw >> 14) & 1) << 1) | ((sw >> 9) & 1);
        if (hwq != qb && classify(g) != Class::QuietNan && classify(g) != Class::SignalingNan) report(k ? "prem1-q" : "prem-q", a, b, control, g, qb, w, hwq);
        check(k ? "prem1" : "prem", g, w, sw, e, sw);
      }
    }
    { Env e = env; Ext g = from_integer(0); (void)g; }
    { Env e = env; uint64_t g = to_double(a, e); double w = h_to_double(to_ld(a), control, &sw); uint64_t wb; memcpy(&wb, &w, 8);
      bool nan_ok = (classify(a) == Class::QuietNan || classify(a) == Class::SignalingNan);
      if ((g != wb && !nan_ok) || (e.flags & 0x3F) != (sw & 0x3F)) report2("todouble", a, control, g, e.flags & 0x3F, wb, sw & 0x3F); }
    { Env e = env; uint32_t g = to_single(a, e); float w = h_to_single(to_ld(a), control, &sw); uint32_t wb; memcpy(&wb, &w, 4);
      bool nan_ok = (classify(a) == Class::QuietNan || classify(a) == Class::SignalingNan);
      if ((g != wb && !nan_ok) || (e.flags & 0x3F) != (sw & 0x3F)) report2("tosingle", a, control, g, e.flags & 0x3F, wb, sw & 0x3F); }
    { Env e = env; int64_t g = 0; bool ok = to_integer(a, 64, env.mode, e, &g); int64_t w = h_to_i64(to_ld(a), control, &sw); if (!ok) g = INT64_MIN;
      if (g != w || (e.flags & 0x3F) != (sw & 0x3F)) report("toi64", a, b, control, from_ld(g), e.flags & 0x3F, from_ld(w), sw & 0x3F); }
    { Env e = env; int64_t g = 0; bool ok = to_integer(a, 32, env.mode, e, &g); int32_t w = h_to_i32(to_ld(a), control, &sw); if (!ok) g = INT32_MIN;
      if ((int32_t)g != w || (e.flags & 0x3F) != (sw & 0x3F)) report("toi32", a, b, control, from_ld(g), e.flags & 0x3F, from_ld(w), sw & 0x3F); }
    { Env e = env; int64_t g = 0; bool ok = to_integer(a, 16, env.mode, e, &g); int16_t w = h_to_i16(to_ld(a), control, &sw); if (!ok) g = INT16_MIN;
      if ((int16_t)g != w || (e.flags & 0x3F) != (sw & 0x3F)) report("toi16", a, b, control, from_ld(g), e.flags & 0x3F, from_ld(w), sw & 0x3F); }
    { Env e = env; int g = compare(a, b, true, e); int w = h_cmp(to_ld(a), to_ld(b), true, &sw); if (g != w || (e.flags & 0x3F) != (sw & 0x3F)) report("fcom", a, b, control, from_ld(g), e.flags & 0x3F, from_ld(w), sw & 0x3F); }
    { Env e = env; int g = compare(a, b, false, e); int w = h_cmp(to_ld(a), to_ld(b), false, &sw); if (g != w || (e.flags & 0x3F) != (sw & 0x3F)) report("fucom", a, b, control, from_ld(g), e.flags & 0x3F, from_ld(w), sw & 0x3F); }
    { uint64_t d = rnd(); if ((rnd() & 3) == 0) d = (d & 0x800FFFFFFFFFFFFFull); double dd; memcpy(&dd, &d, 8); Env e; Ext g = from_double(d, e); Ext w = from_ld(h_from_double(dd, &sw)); if (!same(g, w) || (e.flags & 0x3F) != (sw & 0x3F)) report("fromdouble", g, g, 0, g, e.flags, w, sw & 0x3F); }
  }
  CHECK(failures == 0);
}

struct Case { Bytes code; const char* name; };
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
// modrm for [rdi + disp32] with the given reg field
static void mem_op(Bytes& b, std::initializer_list<uint8_t> opcode, int reg, uint32_t disp) { for (uint8_t o : opcode) b.push_back(o); b.push_back(uint8_t(0x80 | (reg << 3) | 7)); put32(b, disp); }

constexpr uint32_t kCtl = 0x100, kVal = 0x000, kOut = 0x300, kSave = 0x200;
static void emit_setup(Bytes& b, int depth) {
  b.push_back(0xDB); b.push_back(0xE3);                               // fninit
  mem_op(b, {0xD9}, 5, kCtl);                                         // fldcw [rdi+0x100]
  for (int i = depth - 1; i >= 0; --i) mem_op(b, {0xDB}, 5, kVal + 16 * i);      // fldt [rdi + 16*i]
}


static void test_instructions_against_host(int iterations) {
  A alloc;
  int failures = 0, reported = 0, ran = 0;
  void* exec = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (exec == MAP_FAILED) { printf("(no executable memory: instruction comparison skipped)\n"); return; }
  uint32_t names_len = 0;
  // the instruction table: name, bytes before the memory operand (or full encoding for register forms), reg field, kind
  struct Op { const char* name; Bytes enc; int reg; bool mem; };
  std::vector<Op> ops;
  auto reg_op = [&](const char* n, std::initializer_list<uint8_t> e) { ops.push_back({n, Bytes(e), 0, false}); };
  for (int i = 0; i < 8; ++i) {
    for (int k : {0, 1, 2, 3, 4, 5, 6, 7}) { ops.push_back({"D8 reg", {0xD8, uint8_t(0xC0 + 8 * k + i)}, 0, false}); }
    for (int k : {0, 1, 4, 5, 6, 7}) { ops.push_back({"DC reg", {0xDC, uint8_t(0xC0 + 8 * k + i)}, 0, false}); ops.push_back({"DE reg", {0xDE, uint8_t(0xC0 + 8 * k + i)}, 0, false}); }
    reg_op("fld st(i)", {0xD9, uint8_t(0xC0 + i)}); reg_op("fxch", {0xD9, uint8_t(0xC8 + i)});
    reg_op("fst st(i)", {0xDD, uint8_t(0xD0 + i)}); reg_op("fstp st(i)", {0xDD, uint8_t(0xD8 + i)});
    reg_op("fucom", {0xDD, uint8_t(0xE0 + i)}); reg_op("fucomp", {0xDD, uint8_t(0xE8 + i)});
    reg_op("fucomi", {0xDB, uint8_t(0xE8 + i)}); reg_op("fcomi", {0xDB, uint8_t(0xF0 + i)});
    reg_op("fucomip", {0xDF, uint8_t(0xE8 + i)}); reg_op("fcomip", {0xDF, uint8_t(0xF0 + i)});
    reg_op("ffree", {0xDD, uint8_t(0xC0 + i)});
  }
  reg_op("fucompp", {0xDA, 0xE9}); reg_op("fcompp", {0xDE, 0xD9});
  for (uint8_t b : {0xE0, 0xE1, 0xE4, 0xE5, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xFA, 0xFC, 0xFD}) ops.push_back({"D9 xx", {0xD9, b}, 0, false});
  // memory forms: D8 (m32), DC (m64), DA (i32), DE (i16): reg 0..7
  for (int k = 0; k < 8; ++k) for (uint8_t o : {0xD8, 0xDC, 0xDA, 0xDE}) ops.push_back({"arith mem", {o}, k, true});
  ops.push_back({"fld m32", {0xD9}, 0, true}); ops.push_back({"fst m32", {0xD9}, 2, true}); ops.push_back({"fstp m32", {0xD9}, 3, true});
  ops.push_back({"fild m32", {0xDB}, 0, true}); ops.push_back({"fisttp m32", {0xDB}, 1, true}); ops.push_back({"fist m32", {0xDB}, 2, true}); ops.push_back({"fistp m32", {0xDB}, 3, true});
  ops.push_back({"fld m80", {0xDB}, 5, true}); ops.push_back({"fstp m80", {0xDB}, 7, true});
  ops.push_back({"fld m64", {0xDD}, 0, true}); ops.push_back({"fisttp m64", {0xDD}, 1, true}); ops.push_back({"fst m64", {0xDD}, 2, true}); ops.push_back({"fstp m64", {0xDD}, 3, true});
  ops.push_back({"fild m16", {0xDF}, 0, true}); ops.push_back({"fisttp m16", {0xDF}, 1, true}); ops.push_back({"fist m16", {0xDF}, 2, true}); ops.push_back({"fistp m16", {0xDF}, 3, true});
  ops.push_back({"fild m64", {0xDF}, 5, true}); ops.push_back({"fistp m64", {0xDF}, 7, true});

  for (int it = 0; it < iterations; ++it) {
    const Op& op = ops[rnd() % ops.size()];
    const int depth = 1 + rnd() % 5;
    const uint16_t pc = (uint16_t[]){0x000, 0x200, 0x300, 0x300}[rnd() & 3];
    const uint16_t control = (0x37F & ~0xF00) | pc | ((rnd() & 3) << 10);
    uint8_t data[0x400];
    memset(data, 0, sizeof(data));
    for (int k = 0; k < 6; ++k) { Ext e = rnd_ext(0); if (bad_operand(e)) e = Ext{0x8000000000000000ull, 0x3FFF}; memcpy(data + kVal + 16 * k, &e.mant, 8); memcpy(data + kVal + 16 * k + 8, &e.se, 2); }
    memcpy(data + kCtl, &control, 2);
    // a memory operand: a random 8 bytes (some integer-like, some float-like) at 0x340 + stored outputs at kOut
    uint64_t operand = rnd();
    if (rnd() & 1) operand = (rnd() & 1) ? uint64_t(int64_t(int32_t(rnd()))) : uint64_t(int16_t(rnd()));
    if (op.mem && (op.reg == 0 && op.enc[0] == 0xDB) && false) {}
    memcpy(data + 0x340, &operand, 8);
    Ext m80 = rnd_ext(0); if (bad_operand(m80)) m80 = Ext{0x8000000000000000ull, 0x3FFF};
    if (op.mem && op.enc[0] == 0xDB && op.reg == 5) { memcpy(data + 0x340, &m80.mant, 8); memcpy(data + 0x348, &m80.se, 2); }
    Bytes code;
    emit_setup(code, depth);
    if (op.mem) {
      const bool store_op = (op.enc[0] == 0xD9 && (op.reg == 2 || op.reg == 3)) || (op.enc[0] == 0xDB && (op.reg == 1 || op.reg == 2 || op.reg == 3 || op.reg == 7)) ||
                            (op.enc[0] == 0xDD && (op.reg == 1 || op.reg == 2 || op.reg == 3)) || (op.enc[0] == 0xDF && (op.reg == 1 || op.reg == 2 || op.reg == 3 || op.reg == 7));
      mem_op(code, {op.enc[0]}, op.reg, store_op ? kOut : 0x340);
    } else {
      for (uint8_t b : op.enc) code.push_back(b);
    }
    // dump the state: fnsave [rdi + 0x200] (DD /6), then ret
    mem_op(code, {0xDD}, 6, kSave);
    code.push_back(0xC3);

    // --- host
    uint8_t host_data[0x400];
    memcpy(host_data, data, sizeof(data));
    memcpy(exec, code.data(), code.size());
    reinterpret_cast<void (*)(void*)>(exec)(host_data);

    // --- emulator
    process::AddressSpace space{alloc};
    space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x1000, process::kProtRead | process::kProtWrite);
    space.write_privileged(0x20000, ConstByteView(data, sizeof(data)));
    space.map(0x30000, 0x1000, process::kProtRead | process::kProtWrite);
    X86_64Cpu cpu;
    process::ProgramStart start; start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x31000;
    cpu.reset(start);
    cpu.state().r[7] = 0x20000;
    GuestMemory gm(space);
    // run until the final ret (it pops 0 from the empty stack: a fault at address 0 -- stop when the pc leaves the code)
    bool faulted = false;
    for (int steps = 0; steps < 200; ++steps) {
      if (cpu.state().rip >= 0x10000 + code.size()) break;
      if (code[cpu.state().rip - 0x10000] == 0xC3 && cpu.state().rip == 0x10000 + code.size() - 1) break;
      StopReason r = cpu.run(gm, 1);
      if (r == StopReason::Fault) { faulted = true; break; }
    }
    uint8_t emu_data[0x400];
    space.read_privileged(0x20000, ByteView(emu_data, sizeof(emu_data)));
    ++ran;
    bool ok = !faulted;
    // compare the fnsave image: control, status, tags and the non-empty registers; and the stored output area
    auto u16 = [](const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); };
    const uint8_t* h = host_data + kSave; const uint8_t* e = emu_data + kSave;
    if (u16(h + 0) != u16(e + 0) || (u16(h + 4) & ~0x0080) != (u16(e + 4) & ~0x0080) || u16(h + 8) != u16(e + 8)) ok = false;      // (the ES summary bit is not compared)
    const uint16_t tags = u16(h + 8);
    for (int k = 0; k < 8; ++k) {
      // fnsave stores ST(0)..ST(7); the tag word is by physical register, convert
      const int top = (u16(h + 4) >> 11) & 7;
      const int phys = (top + k) & 7;
      if (((tags >> (2 * phys)) & 3) != 3 && memcmp(h + 28 + 10 * k, e + 28 + 10 * k, 10) != 0) ok = false;
    }
    if (memcmp(host_data + kOut, emu_data + kOut, 16) != 0) ok = false;
    if (!ok) {
      ++failures;
      if (reported++ < 15) {
        printf("%s [", op.name); for (uint8_t b : op.enc) printf("%02x ", b); printf("] reg %d depth %d control %04x faulted %d (rip %llx vec %d)\n   host: cw %04x sw %04x tw %04x st0 ", op.reg, depth, control, faulted, (unsigned long long)cpu.state().rip, cpu.fault().vector, u16(h), u16(h + 4), u16(h + 8));
        for (int b = 9; b >= 0; --b) printf("%02x", h[28 + b]); printf("\n   emu : cw %04x sw %04x tw %04x st0 ", u16(e), u16(e + 4), u16(e + 8)); for (int b = 9; b >= 0; --b) printf("%02x", e[28 + b]);
        printf("\n   vals:"); for (int k = 0; k < depth; ++k) { printf(" [%02x%02x:", data[kVal + 16 * k + 9], data[kVal + 16 * k + 8]); for (int b = 7; b >= 0; --b) printf("%02x", data[kVal + 16 * k + b]); printf("]"); } printf(" operand %016llx", (unsigned long long)operand);
        printf("\n   out host "); for (int b = 0; b < 10; ++b) printf("%02x", host_data[kOut + b]); printf(" emu "); for (int b = 0; b < 10; ++b) printf("%02x", emu_data[kOut + b]); printf("\n");
      }
    }
  }
  (void)names_len;
  (void)ran;
  CHECK(failures == 0);
}

// distance in units of the last place (same sign and exponent neighbourhood)
static double ulps(const Ext& a, const Ext& b) {
  if (a.se == b.se) return (double)(a.mant > b.mant ? a.mant - b.mant : b.mant - a.mant);
  long double x = to_ld(a), y = to_ld(b);
  if (x == y) return 0;
  long double d = fabsl(x - y), m = fabsl(y);
  if (m == 0) return 1e30;
  return (double)(d / m * 9223372036854775808.0L);
}
static long double rnd_range(long double lo, long double hi) { return lo + (hi - lo) * ((long double)(rnd() >> 11) / 9007199254740992.0L); }
static int bad = 0; static double worst[8] = {};
static void check(const char* name, int idx, long double arg1, long double arg2, Ext got, Ext want, double tol) {
  double u = ulps(got, want);
  if (u > worst[idx]) worst[idx] = u;
  if (u > tol && bad++ < 20) printf("%s(%.21Lg, %.21Lg): got %.21Lg want %.21Lg (%.1f ulps)\n", name, arg1, arg2, to_ld(got), to_ld(want), u);
}

static void test_transcendentals(int n) {
  for (int i = 0; i < n; ++i) {
    long double x = rnd_range(-1, 1), y = rnd_range(-30, 30);
    long double r;
    // f2xm1
    asm("f2xm1" : "=t"(r) : "0"(x)); { Env e; check("f2xm1", 0, x, 0, ext::f2xm1(from_ld(x), e), from_ld(r), 10); }
    long double s = ((rnd() & 3) == 0) ? rnd_range(0.5L, 2.0L) : expl(rnd_range(-40, 40));
    asm("fyl2x" : "=t"(r) : "0"(s), "u"(y) : "st(1)"); { Env e; check("fyl2x", 1, s, y, ext::fyl2x(from_ld(s), from_ld(y), e), from_ld(r), 10); }
    long double p = rnd_range(-0.29L, 0.41L);
    asm("fyl2xp1" : "=t"(r) : "0"(p), "u"(y) : "st(1)"); { Env e; check("fyl2xp1", 2, p, y, ext::fyl2xp1(from_ld(p), from_ld(y), e), from_ld(r), 10); }
    long double a = rnd_range(-100, 100), b = rnd_range(-100, 100);
    if (rnd() & 1) b = rnd_range(-1, 1);
    asm("fpatan" : "=t"(r) : "0"(b), "u"(a) : "st(1)"); { Env e; check("fpatan", 3, a, b, ext::fpatan(from_ld(a), from_ld(b), e), from_ld(r), 10); }
    long double t = (rnd() % 4 == 0) ? rnd_range(-1e6L, 1e6L) : rnd_range(-20, 20);
    if (rnd() % 8 == 0) t = rnd_range(-1e-3L, 1e-3L);
    long double sn, cs;
    asm("fsin" : "=t"(sn) : "0"(t)); asm("fcos" : "=t"(cs) : "0"(t));
    { Ext S, C; Env e; ext::sin_cos(from_ld(t), &S, &C, e); check("fsin", 4, t, 0, S, from_ld(sn), 10); check("fcos", 5, t, 0, C, from_ld(cs), 10); }
    long double tn, one;
    asm("fptan" : "=t"(one), "=u"(tn) : "0"(t)); { Ext T; Env e; ext::tangent(from_ld(t), &T, e); check("fptan", 6, t, 0, T, from_ld(tn), 16); }
  }
  CHECK(bad == 0);
}
#endif

// --- hand-written checks (no x87 needed on the host) -------------------------------------------------------------------------------------------

constexpr uint64_t kCodeBase = 0x10000, kDataBase = 0x20000, kStackBase = 0x30000;
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(kCodeBase, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(kCodeBase, ConstByteView(code.data(), code.size()));
    space.map(kDataBase, 0x1000, process::kProtRead | process::kProtWrite);
    space.map(kStackBase, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64;
    start.entry = kCodeBase;
    start.stack_pointer = kStackBase + 0x1000;
    cpu.reset(start);
    cpu.state().r[7] = kDataBase;      // rdi
  }
  // Runs the code; it ends with a SYSCALL (0F 05).
  StopReason run(uint64_t n = 100000) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
  void put(uint64_t off, const void* p, size_t n) { space.write_privileged(kDataBase + off, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  uint64_t get64(uint64_t off) { uint8_t r[8]; space.read_privileged(kDataBase + off, ByteView(r, 8)); uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t{r[i]} << (8 * i); return v; }
  Ext get_ext(uint64_t off) { uint8_t r[10]; space.read_privileged(kDataBase + off, ByteView(r, 10)); Ext e; e.mant = 0; for (int i = 0; i < 8; ++i) e.mant |= uint64_t{r[i]} << (8 * i); e.se = uint16_t(r[8] | (r[9] << 8)); return e; }
};
static Bytes code_of(std::initializer_list<uint8_t> bytes) { Bytes b(bytes); b.push_back(0x0F); b.push_back(0x05); return b; }
static Ext constant_pi_ext() { Ext e; e.mant = 0xC90FDAA22168C235ull; e.se = 0x4000; return e; }
static uint64_t double_bits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

static void test_basic_arithmetic() {
  {      // 3 / 8 and a square root, exact
    Machine m(code_of({0xDF, 0x07,                    // fild word [rdi]        (3)
                       0xDB, 0x47, 0x08,              // fild dword [rdi+8]     (8)
                       0xDE, 0xF9,                    // fdivp st1, st0          3/8
                       0xDD, 0x5F, 0x10,              // fstp qword [rdi+16]
                       0xDD, 0x47, 0x20,              // fld qword [rdi+32]      (2.25)
                       0xD9, 0xFA,                    // fsqrt
                       0xDD, 0x5F, 0x18}));           // fstp qword [rdi+24]
    const int16_t three = 3; const int32_t eight = 8; const double v = 2.25;
    m.put(0, &three, 2); m.put(8, &eight, 4); m.put(32, &v, 8);
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.get64(16) == double_bits(0.375) && m.get64(24) == double_bits(1.5));
    CHECK((m.cpu.state().fpu_status & 0x3F) == 0 && m.cpu.state().fpu_valid == 0);      // exact, and the stack is empty again
  }
  {      // 1/3 stored as a double and as an 80-bit value; the rounding shows in the status word
    Machine m(code_of({0xD9, 0xE8,                    // fld1
                       0xDF, 0x07,                    // fild word [rdi]  (3)
                       0xDE, 0xF9,                    // fdivp  -> 1/3
                       0xD9, 0xC0,                    // fld st0
                       0xDD, 0x5F, 0x10,              // fstp qword [rdi+16]
                       0xDB, 0x7F, 0x20,              // fstp tbyte [rdi+32]
                       0xDF, 0xE0}));                 // fnstsw ax
    const int16_t three = 3;
    m.put(0, &three, 2);
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.get64(16) == 0x3FD5555555555555ull);
    const Ext third = m.get_ext(32);
    CHECK(third.se == 0x3FFD && third.mant == 0xAAAAAAAAAAAAAAABull);      // 1/3 rounded to nearest at 64 bits
    CHECK((m.cpu.state().r[0] & 0x20) != 0);      // inexact
  }
  {      // precision control: 24 bits
    Machine m(code_of({0xD9, 0x6F, 0x40,              // fldcw [rdi+64]   (single precision)
                       0xD9, 0xE8,                    // fld1
                       0xDF, 0x07,                    // fild word [rdi]  (3)
                       0xDE, 0xF9,                    // fdivp
                       0xDB, 0x7F, 0x20}));           // fstp tbyte [rdi+32]
    const int16_t three = 3; const uint16_t cw = 0x007F;
    m.put(0, &three, 2); m.put(64, &cw, 2);
    CHECK(m.run() == StopReason::Syscall);
    const Ext third = m.get_ext(32);
    CHECK(third.se == 0x3FFD && third.mant == 0xAAAAAB0000000000ull);      // 24 significant bits, rounded to nearest
  }
  {      // integers: round to nearest even by default, and fisttp truncates; the integer indefinite on overflow
    Machine m(code_of({0xDD, 0x07,                    // fld qword [rdi]  (2.5)
                       0xDF, 0x7F, 0x10,              // fistp qword [rdi+16]
                       0xDD, 0x07,                    // fld qword [rdi]
                       0xDF, 0x4F, 0x18,              // fisttp word [rdi+24]
                       0xDD, 0x47, 0x08,              // fld qword [rdi+8]  (1e30)
                       0xDF, 0x7F, 0x20}));           // fistp qword [rdi+32]
    const double a = 2.5, b = 1e30;
    m.put(0, &a, 8); m.put(8, &b, 8);
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.get64(16) == 2 && (m.get64(24) & 0xFFFF) == 2 && m.get64(32) == 0x8000000000000000ull);
    CHECK((m.cpu.state().fpu_status & 1) != 0);      // invalid operation
  }
}

static void test_stack_and_state() {
  {      // push nine: the ninth overflows (invalid operation, stack fault, C1 set) and pushes the real indefinite
    Bytes code;
    for (int i = 0; i < 9; ++i) { code.push_back(0xD9); code.push_back(0xE8); }      // fld1 x9
    code.push_back(0xD9); code.push_back(0xE5);                                         // fxam
    code.push_back(0x0F); code.push_back(0x05);
    Machine m(code);
    CHECK(m.run() == StopReason::Syscall);
    const uint16_t sw = m.cpu.state().fpu_status;
    CHECK((sw & 0x41) == 0x41 && (sw & 0x200) != 0);      // IE, SF and C1
    CHECK(m.cpu.state().st[(((sw >> 11) & 7))].se == 0xFFFF);      // the real indefinite on top
  }
  {      // reading an empty register: the result is the real indefinite
    Machine m(code_of({0xD9, 0xC1,                    // fld st(1)  of an empty stack
                       0xDB, 0x7F, 0x20}));           // fstp tbyte [rdi+32]
    CHECK(m.run() == StopReason::Syscall);
    const Ext e = m.get_ext(32);
    CHECK(e.se == 0xFFFF && e.mant == 0xC000000000000000ull && (m.cpu.state().fpu_status & 0x41) == 0x41);
  }
  {      // fxsave / fxrstor and fnsave / frstor keep the registers, their order and which are in use
    Machine m(code_of({0xD9, 0xE8,                    // fld1
                       0xD9, 0xEB,                    // fldpi
                       0x0F, 0xAE, 0x07,              // fxsave [rdi]
                       0xDB, 0xE3,                    // fninit
                       0x0F, 0xAE, 0x0F,              // fxrstor [rdi]
                       0xDD, 0x5F, 0x40,              // fstp qword [rdi+64]   (pi)
                       0xDD, 0x5F, 0x48,              // fstp qword [rdi+72]   (1)
                       0xD9, 0xEE,                    // fldz
                       0xDD, 0xB7, 0x00, 0x02, 0x00, 0x00,      // fnsave [rdi+0x200]
                       0xDD, 0xA7, 0x00, 0x02, 0x00, 0x00,      // frstor [rdi+0x200]
                       0xDD, 0x5F, 0x50}));           // fstp qword [rdi+80]   (0)
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.get64(64) == double_bits(3.141592653589793) && m.get64(72) == double_bits(1.0) && m.get64(80) == 0);
    CHECK(m.cpu.state().fpu_valid == 0);
  }
  {      // fprem until it is done: 2^100 mod 7 = 2
    Machine m(code_of({0xDF, 0x07,                    // fild word [rdi]    (7)            st0 = 7
                       0xDF, 0x47, 0x08,              // fild word [rdi+8]  (100)          st0 = 100, st1 = 7
                       0xD9, 0xE8,                    // fld1                              st0 = 1, st1 = 100, st2 = 7
                       0xD9, 0xFD,                    // fscale                            st0 = 2^100
                       0xDD, 0xD9,                    // fstp st(1)                        st0 = 2^100, st1 = 7
                       0xD9, 0xC9,                    // fxch                              st0 = 7, st1 = 2^100
                       0xD9, 0xC9,                    // fxch                              st0 = 2^100, st1 = 7
                       0xD9, 0xF8,                    // L: fprem
                       0xDF, 0xE0,                    // fnstsw ax
                       0xF6, 0xC4, 0x04,              // test ah, 4
                       0x75, 0xF7,                    // jnz L
                       0xDF, 0x7F, 0x10}));           // fistp qword [rdi+16]
    const int16_t seven = 7, hundred = 100;
    m.put(0, &seven, 2); m.put(8, &hundred, 2);
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.get64(16) == 2);
  }
  {      // the comparison instructions set the condition codes, and FCOMI sets EFLAGS
    Machine m(code_of({0xD9, 0xE8,                    // fld1
                       0xD9, 0xEE,                    // fldz               st0 = 0, st1 = 1
                       0xDB, 0xF1,                    // fcomi st0, st1      0 < 1: CF
                       0xDD, 0xD8,                    // fstp st(0)
                       0xDD, 0xD8}));                 // fstp st(0)
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.cpu.state().rflags & 1) != 0 && !(m.cpu.state().rflags & 0x40));
  }
  {      // fldcw / fnstcw / fnstenv masks / fninit defaults
    Machine m(code_of({0xD9, 0x6F, 0x40,              // fldcw [rdi+64]
                       0xD9, 0x7F, 0x48,              // fnstcw [rdi+72]
                       0xDB, 0xE3,                    // fninit
                       0xD9, 0x7F, 0x50}));           // fnstcw [rdi+80]
    const uint16_t cw = 0x0C00;
    m.put(64, &cw, 2);
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.get64(72) & 0xFFFF) == 0x0C00 && (m.get64(80) & 0xFFFF) == 0x037F);
  }
}

static void test_transcendental_values() {
  // known values, accurate to well under a part in 10^15
  Env env;
  Ext s, c;
  const Ext pi_over_6 = div(constant_pi_ext(), from_integer(6), 64, env);
  CHECK(sin_cos(pi_over_6, &s, &c, env));
  Env e2;
  const uint64_t sbits = to_double(s, e2), cbits = to_double(c, e2);
  double sd, cd;
  memcpy(&sd, &sbits, 8); memcpy(&cd, &cbits, 8);
  CHECK(std::fabs(sd - 0.5) < 1e-15 && std::fabs(cd - 0.8660254037844386) < 1e-15);
  // 2^0.5 - 1 = 0.41421356...
  Ext half = from_integer(1); half.se = uint16_t(half.se - 1);
  Env e3;
  const Ext r = f2xm1(half, e3);
  const uint64_t rbits = to_double(r, e3);
  double rd;
  memcpy(&rd, &rbits, 8);
  CHECK(std::fabs(rd - 0.41421356237309503) < 1e-15);
  // log2(8) = 3
  Env e4;
  const Ext l = fyl2x(from_integer(8), one(), e4);
  const Ext three = from_integer(3);
  const uint64_t lbits = to_double(l, e4);
  double ld_;
  memcpy(&ld_, &lbits, 8);
  CHECK(std::fabs(ld_ - 3.0) < 1e-15);
  (void)three;
  // atan2(1, 1) = pi/4
  Env e5;
  const Ext a = fpatan(one(), one(), e5);
  const uint64_t abits = to_double(a, e5);
  double ad;
  memcpy(&ad, &abits, 8);
  CHECK(std::fabs(ad - 0.7853981633974483) < 1e-15);
}

int main(int argc, char** argv) {
  const int scale = argc > 1 ? atoi(argv[1]) : 1;
  test_basic_arithmetic();
  test_stack_and_state();
  test_transcendental_values();
#ifdef HOST_X87
  test_core_against_host(40000 * scale);
  test_instructions_against_host(20000 * scale);
  test_transcendentals(4000 * scale);
#else
  printf("(no x87 on this host: the comparison against real hardware is skipped)\n");
#endif
  if (g_failed) {
    printf("X87Test: %d FAILED\n", g_failed);
    return 1;
  }
  printf("X87Test: all passed\n");
  return 0;
}
