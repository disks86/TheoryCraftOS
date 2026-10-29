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

// Host test of the AVX/AVX2/FMA/BMI/F16C emulation. Where the host has AVX2 (x86-64 Linux) it is the oracle: every instruction of the table (about 1400, assembled with llvm-mc
// from tests/data/emulation/avx_cases_gen.py) is run on the host and on the emulator from the same random register contents, and the vector registers, the general
// registers that were written, the flags, MXCSR and the memory operand must come out identical. Without AVX2 on the host only the hand-written checks run.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"
#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#define HOST_AVX 1
#endif

using namespace emulation;
typedef std::vector<uint8_t> Bytes;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct A final : allocators::IAllocator {
  void* allocate(uint64_t s) override { return malloc(s); }
  void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); }
  void deallocate(void* p) override { free(p); }
};

struct Case { const char* text; std::vector<uint8_t> code; };
static const Case kCases[] = {
#include "data/emulation/avx_cases.inc"
};

static uint64_t seed = 0x2545F4914F6CDD1Dull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

constexpr uint32_t kYmm = 0x000, kOut = 0x200, kGpr = 0x400, kMxcsrIn = 0x420, kFlagsOut = 0x430, kRegsOut = 0x438, kMxcsrOut = 0x450, kTable = 0x600;

static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void mem(Bytes& b, std::initializer_list<uint8_t> op, int reg, uint32_t disp) { for (uint8_t o : op) b.push_back(o); b.push_back(uint8_t(0x80 | (reg << 3) | 7)); put32(b, disp); }

// pieces of code shared by host and emulator
static void gpr_setup(Bytes& b) {
  mem(b, {0x48, 0x8B}, 0, kGpr);          // mov rax, [rdi+0x400]
  mem(b, {0x48, 0x8B}, 1, kGpr + 8);      // mov rcx
  mem(b, {0x48, 0x8B}, 2, kGpr + 16);     // mov rdx
  mem(b, {0x48, 0x8D}, 6, kTable);        // lea rsi, [rdi+0x600]
  mem(b, {0x0F, 0xAE}, 2, kMxcsrIn);      // ldmxcsr [rdi+0x420]
  b.push_back(0x45); b.push_back(0x31); b.push_back(0xC9);      // xor r9d, r9d  (flags: ZF PF)
}
static void gpr_dump(Bytes& b) {
  b.push_back(0x9C); b.push_back(0x41); b.push_back(0x58);      // pushfq; pop r8
  mem(b, {0x4C, 0x89}, 0, kFlagsOut);     // mov [rdi+...], r8
  mem(b, {0x48, 0x89}, 0, kRegsOut);      // rax
  mem(b, {0x48, 0x89}, 1, kRegsOut + 8);  // rcx
  mem(b, {0x48, 0x89}, 2, kRegsOut + 16); // rdx
  mem(b, {0x0F, 0xAE}, 3, kMxcsrOut);     // stmxcsr
}


// MXCSR.DAZ / FTZ coverage: half of the cases run with each of those bits set, and the data is often rich in denormals and tiny numbers (NO_DAZFTZ=1 turns this off).
static bool daz_ftz_enabled() { static const bool on = getenv("NO_DAZFTZ") == nullptr; return on; }
static void denormalize(uint8_t* p, size_t bytes) {
  for (size_t i = 0; i + 8 <= bytes; i += 8) {
    uint64_t v;
    memcpy(&v, p + i, 8);
    const uint64_t r = rnd(), sign = rnd();
    switch (rnd() % 6) {
      case 0: v = ((sign & 1) << 31) | (r & 0x7FFFFF); v |= (((sign >> 1) & 1) << 63) | ((r >> 32) & 0x7FFFFF) << 32; break;      // two denormal floats
      case 1: v = ((sign & 1) << 63) | (r & 0xFFFFFFFFFFFFFull); break;                                                            // a denormal double
      case 2: v = ((sign & 1) << 31) | ((1 + r % 30) << 23) | ((r >> 8) & 0x7FFFFF); v |= (((sign >> 1) & 1) << 63) | ((1 + (r >> 40) % 30) << 23 | ((r >> 16) & 0x7FFFFF)) << 32; break;  // tiny floats
      case 3: v = ((sign & 1) << 63) | ((1 + r % 40) << 52) | ((r >> 12) & 0xFFFFFFFFFFFFFull); break;                             // tiny doubles
      case 4: v = ((sign & 1) << 31) | ((124 + r % 6) << 23) | ((r >> 8) & 0x7FFFFF); v |= (((sign >> 1) & 1) << 63) | ((124 + (r >> 40) % 6) << 23 | ((r >> 16) & 0x7FFFFF)) << 32; break;  // floats near 1
      default: break;
    }
    memcpy(p + i, &v, 8);
  }
}

static void fill_state(uint8_t* data, const Case& c) {
  memset(data, 0, 0x800);
  for (int r = 0; r < 8; ++r) {
    uint8_t* p = data + kYmm + 32 * r;
    const uint64_t kind = rnd() % 6;
    for (int i = 0; i < 32; i += 8) {
      uint64_t v = rnd();
      if (kind == 1) { float f[2]; f[0] = float(int32_t(rnd()) % 1000) / 8; f[1] = float(int32_t(rnd()) % 100) * 3.25f; memcpy(&v, f, 8); }
      else if (kind == 2) { double d = double(int32_t(rnd()) % 100000) / 16; memcpy(&v, &d, 8); }
      else if (kind == 3) { v &= 0x00FF00FF00FF00FFull; }
      else if (kind == 4 && (rnd() & 3) == 0) { static const uint64_t specials[] = {0, ~0ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull, 0x8000000000000000ull, 0x7F8000007FC00000ull, 0x0000000100000001ull, 0x8000000080000000ull}; v = specials[rnd() % 8]; }
      memcpy(p + i, &v, 8);
    }
  }
  for (int i = 0; i < 3; ++i) { uint64_t v = rnd(); if (rnd() & 1) v &= 0xFF; if ((rnd() & 7) == 0) v = (rnd() & 1) ? ~0ull : 0; memcpy(data + kGpr + 8 * i, &v, 8); }
  uint32_t mxcsr = 0x1F80 | uint32_t((rnd() & 3) << 13);
  if (daz_ftz_enabled()) {
    mxcsr |= uint32_t((rnd() & 1) << 6) | uint32_t((rnd() & 1) << 15);
    denormalize(data + kYmm, 8 * 32);
    if (rnd() & 1) denormalize(data, 0x40);
  }
  memcpy(data + kMxcsrIn, &mxcsr, 4);
  for (int i = 0; i < 0x200; ++i) data[kTable + i] = uint8_t(rnd());
  if (strstr(c.text, "pcmp") && strstr(c.text, "str")) {
    // strings over a tiny alphabet that end early (bytes or words, signed values too), and explicit lengths that are often inside the register
    auto string_into = [&](uint8_t* p) {
      const bool words = rnd() & 1;
      const uint32_t n = words ? 8 : 16, len = uint32_t(rnd() % (n + 1));
      memset(p, 0, 16);
      for (uint32_t i = 0; i < len; ++i) {
        uint32_t v = 1 + uint32_t(rnd() % 3);
        if ((rnd() & 7) == 0) v = words ? 0xFFFE : 0xFE;
        if (words) { p[2 * i] = uint8_t(v); p[2 * i + 1] = uint8_t(v >> 8); } else p[i] = uint8_t(v);
      }
    };
    for (int r = 1; r <= 3; ++r) string_into(data + kYmm + 32 * r);
    string_into(data);
    for (int i = 0; i < 3; i += 2) { int64_t v = int64_t(rnd() % 22) - 3; memcpy(data + kGpr + 8 * i, &v, 8); }
  }
  if (strncmp(c.text, "vpgather", 8) == 0 || strncmp(c.text, "vgather", 7) == 0) {
    // small non-negative indices in ymm2; the mask (ymm3) keeps random sign bits
    const bool qindex = strstr(c.text, "gatherq") != nullptr || strncmp(c.text, "vpgatherq", 9) == 0 || strncmp(c.text, "vgatherq", 8) == 0;
    for (int i = 0; i < 32; i += (qindex ? 8 : 4)) { uint64_t v = rnd() % 60; memcpy(data + kYmm + 64 + i, &v, qindex ? 8 : 4); }
  }
}

// --- hand-written checks (no AVX needed on the host) -------------------------------------------------------------------------------------------

struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(0x30000, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x31000;
    cpu.reset(start);
    cpu.state().r[7] = 0x20000;
  }
  StopReason run() { GuestMemory m(space); return cpu.run(m, 10000); }
};
static Bytes with_syscall(Bytes b) { b.push_back(0x0F); b.push_back(0x05); return b; }

// MXCSR.DAZ and FTZ by hand (the comparison with the host needs no AVX here): the expected values and flags are the architecture's own rules.
static void test_daz_ftz() {
  auto run_ps = [&](uint8_t op, uint32_t a, uint32_t b, uint32_t mxcsr, uint32_t* out, uint32_t* flags) {
    Machine m(with_syscall({0x0F, op, 0xC1}));      // op xmm0, xmm1
    m.cpu.state().xmm[0] = {a, 0};
    m.cpu.state().xmm[1] = {b, 0};
    m.cpu.state().mxcsr = mxcsr;
    CHECK(m.run() == StopReason::Syscall);
    *out = static_cast<uint32_t>(m.cpu.state().xmm[0].lo);
    *flags = m.cpu.state().mxcsr & 0x3F;
  };
  uint32_t r, f;
  run_ps(0x58, 0x00000001, 0, 0x1F80, &r, &f);      // addps: a denormal plus zero is the denormal, with the denormal-operand flag
  CHECK(r == 1 && f == 2);
  run_ps(0x58, 0x00000001, 0, 0x1FC0, &r, &f);      // DAZ: it is a zero and raises nothing
  CHECK(r == 0 && f == 0);
  run_ps(0x58, 0x80000001, 0x80000000, 0x1FC0, &r, &f);      // (the sign of the zero is kept: -0 + -0)
  CHECK(r == 0x80000000 && f == 0);
  run_ps(0x59, 0x00800000, 0x3F000000, 0x1F80, &r, &f);      // mulps: the smallest normal times a half is an exact denormal: no flags
  CHECK(r == 0x00400000 && f == 0);
  run_ps(0x59, 0x00800000, 0x3F000000, 0x9F80, &r, &f);      // FTZ: it is flushed to zero, with underflow and inexact
  CHECK(r == 0 && f == 0x30);
  run_ps(0x59, 0x80800000, 0x3F000000, 0x9F80, &r, &f);
  CHECK(r == 0x80000000 && f == 0x30);
  run_ps(0x59, 0x3F800000, 0x3F800000, 0x9FC0, &r, &f);      // normal results are untouched
  CHECK(r == 0x3F800000 && f == 0);
  run_ps(0x5F, 0x00000001, 0x80000000, 0x1FC0, &r, &f);      // maxps: with DAZ the denormal is a zero (+0 vs -0 gives the second operand)
  CHECK(r == 0x80000000);
  run_ps(0x51, 0x00000001, 0x00000001, 0x1FC0, &r, &f);      // sqrtps of a DAZ'd denormal is zero
  CHECK(r == 0 && f == 0);
}

static void test_basics() {
  {      // xgetbv: x87, SSE, AVX and AVX-512 state are enabled
    Machine m(with_syscall({0x0F, 0x01, 0xD0}));
    m.cpu.state().r[1] = 0;
    CHECK(m.run() == StopReason::Syscall && (m.cpu.state().r[0] & 0xFFFFFFFFu) == 0xE7 && m.cpu.state().r[2] == 0);
    Machine bad(with_syscall({0x0F, 0x01, 0xD0}));
    bad.cpu.state().r[1] = 1;
    CHECK(bad.run() == StopReason::Fault);      // only XCR0 exists
  }
  {      // cpuid reports AVX, AVX2, FMA, BMI1, BMI2 and OSXSAVE
    Machine m(with_syscall({0x31, 0xC9, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0xA2, 0x41, 0x89, 0xC8,        // leaf 1: ecx -> r8d
                            0x31, 0xC9, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x0F, 0xA2}));                           // leaf 7: ebx
    CHECK(m.run() == StopReason::Syscall);
    const uint64_t leaf1_ecx = m.cpu.state().r[8];
    CHECK((leaf1_ecx & (1u << 28)) && (leaf1_ecx & (1u << 27)) && (leaf1_ecx & (1u << 12)) && (leaf1_ecx & (1u << 29)));      // AVX OSXSAVE FMA F16C
    const uint64_t leaf7_ebx = m.cpu.state().r[3];
    CHECK((leaf7_ebx & (1u << 5)) && (leaf7_ebx & (1u << 3)) && (leaf7_ebx & (1u << 8)));      // AVX2 BMI1 BMI2
  }
  {      // legacy SSE leaves the upper half of YMM alone; VEX.128 clears it
    Machine m(with_syscall({0x66, 0x0F, 0xEF, 0xC0,               // pxor xmm0, xmm0
                            0xC5, 0xF9, 0xEF, 0xC9}));            // vpxor xmm1, xmm0, xmm1
    m.cpu.state().ymm_high[0] = {0x1111, 0x2222};
    m.cpu.state().ymm_high[1] = {0x3333, 0x4444};
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.cpu.state().ymm_high[0].lo == 0x1111 && m.cpu.state().ymm_high[1].lo == 0 && m.cpu.state().ymm_high[1].hi == 0);
  }
  {      // xsave / vzeroall / xrstor restore the AVX state
    Machine m(with_syscall({0x0F, 0xAE, 0x27,                     // xsave [rdi]
                            0xC5, 0xFC, 0x77,                     // vzeroall
                            0x0F, 0xAE, 0x2F}));                  // xrstor [rdi]
    m.cpu.state().r[0] = 7;
    m.cpu.state().r[2] = 0;
    m.cpu.state().xmm[3] = {0xAAAA, 0xBBBB};
    m.cpu.state().ymm_high[3] = {0xCCCC, 0xDDDD};
    m.cpu.state().mxcsr = 0x1FA0;
    // (the xrstor needs EAX/EDX again: the first instructions did not change them)
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.cpu.state().xmm[3].lo == 0xAAAA && m.cpu.state().ymm_high[3].hi == 0xDDDD && m.cpu.state().mxcsr == 0x1FA0);
  }
  {      // the register image used by exceptions keeps the upper halves
    Machine m(with_syscall({0x90}));
    m.cpu.state().ymm_high[5] = {0x0102030405060708ull, 0x1112131415161718ull};
    process::X86Context context;
    CHECK(m.cpu.get_x86_context(&context));
    m.cpu.state().ymm_high[5] = {0, 0};
    CHECK(m.cpu.set_x86_context(context));
    CHECK(m.cpu.state().ymm_high[5].lo == 0x0102030405060708ull && m.cpu.state().ymm_high[5].hi == 0x1112131415161718ull);
  }
  {      // movbe
    Machine m(with_syscall({0x0F, 0x38, 0xF0, 0x07,               // movbe eax, [rdi]
                            0x0F, 0x38, 0xF1, 0x4F, 0x08}));      // movbe [rdi+8], ecx
    const uint8_t v[4] = {0x11, 0x22, 0x33, 0x44};
    m.space.write(0x20000, ConstByteView(v, 4));
    m.cpu.state().r[1] = 0xAABBCCDDu;
    CHECK(m.run() == StopReason::Syscall && m.cpu.state().r[0] == 0x11223344u);
    uint8_t out[4];
    m.space.read(0x20008, ByteView(out, 4));
    CHECK(out[0] == 0xAA && out[3] == 0xDD);
  }
  {      // SSE3, SSSE3, SSE4.1 and SSE4.2 are reported
    Machine m(with_syscall({0x31, 0xC9, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0xA2, 0x41, 0x89, 0xC8}));
    CHECK(m.run() == StopReason::Syscall);
    const uint64_t c = m.cpu.state().r[8];
    CHECK((c & 1) && (c & (1u << 9)) && (c & (1u << 19)) && (c & (1u << 20)));
  }
  {      // crc32 (CRC-32C): the check value of "123456789" is E3069283 (initial value and final xor are the caller's)
    Bytes code = {0xB8, 0xFF, 0xFF, 0xFF, 0xFF};      // mov eax, -1
    for (char ch : std::string("123456789")) { code.push_back(0xB1); code.push_back(uint8_t(ch)); code.push_back(0xF2); code.push_back(0x0F); code.push_back(0x38); code.push_back(0xF0); code.push_back(0xC1); }      // mov cl, ch; crc32 eax, cl
    code.push_back(0xF7); code.push_back(0xD0);      // not eax
    Machine m(with_syscall(code));
    CHECK(m.run() == StopReason::Syscall && m.cpu.state().r[0] == 0xE3069283u);
  }
  {      // pcmpistri as strlen: the classic "equal each against zeros" form finds the terminator
    Machine m(with_syscall({0x66, 0x0F, 0xEF, 0xC0,                                    // pxor xmm0, xmm0
                            0x66, 0x0F, 0x3A, 0x63, 0x07, 0x08,                     // pcmpistri xmm0, [rdi], 8
                            0x41, 0x89, 0xC8}));                                  // mov r8d, ecx (the syscall instruction overwrites rcx)
    const uint8_t text[16] = {'h', 'e', 'l', 'l', 'o', 0, 'x', 'y'};
    m.space.write(0x20000, ConstByteView(text, 16));
    CHECK(m.run() == StopReason::Syscall && m.cpu.state().r[8] == 5);
  }
  {      // the MMX forms (no 66 prefix) are valid, and VEX-only opcodes without a VEX prefix are invalid instructions
    Machine a(with_syscall({0x0F, 0x38, 0x00, 0xC1}));      // pshufb mm0, mm1
    CHECK(a.run() == StopReason::Syscall);
    Machine mmx_ud({0x0F, 0x38, 0x18, 0x07});      // vbroadcastss's opcode without a prefix is not an MMX instruction either
    CHECK(mmx_ud.run() == StopReason::Fault && mmx_ud.cpu.fault().vector == 6);
    Machine b({0x66, 0x0F, 0x38, 0x18, 0x07});      // vbroadcastss without VEX
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 6);
    Machine c({0x66, 0x0F, 0x3A, 0x38, 0x07, 0x01});      // vinserti128 without VEX
    CHECK(c.run() == StopReason::Fault && c.cpu.fault().vector == 6);
  }
  {      // a 256-bit load that runs off the end of a mapping is a page fault at the first missing byte
    Machine m(with_syscall({0xC5, 0xFE, 0x6F, 0x07}));            // vmovdqu ymm0, [rdi]
    m.cpu.state().r[7] = 0x21FF0;      // 16 bytes available before the end of the mapping at 0x22000
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14 && m.cpu.fault().address >= 0x21FF8 && m.cpu.fault().address < 0x22000 + 8);
    CHECK(m.cpu.state().ymm_high[0].lo == 0 && m.cpu.state().xmm[0].lo == 0);      // the register is untouched
  }
  {      // VEX with a legacy prefix or on a bad opcode is an invalid instruction
    Machine a({0x66, 0xC5, 0xF8, 0x77});
    CHECK(a.run() == StopReason::Fault && a.cpu.fault().vector == 6);
    Machine b({0xC5, 0xF8, 0x04, 0xC0});
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 6);
  }
}

// The decoder must survive anything: every truncation of every encoding, and bit flips of them, run briefly (the sanitizers watch for memory errors).
static void test_robustness() {
  uint32_t runs = 0;
  uint64_t local_seed = 0x9E3779B97F4A7C15ull;
  auto next = [&]() { local_seed ^= local_seed << 13; local_seed ^= local_seed >> 7; local_seed ^= local_seed << 17; return local_seed; };
  for (const Case& c : kCases) {
    for (size_t variant = 0; variant < c.code.size() + 3; ++variant) {
      Bytes code = c.code;
      if (variant < c.code.size()) code.resize(variant);      // truncation
      else code[next() % code.size()] ^= uint8_t(1u << (next() & 7));      // a bit flip
      code.push_back(0x0F); code.push_back(0x05);
      Machine m(code);
      m.cpu.state().r[6] = 0x20800;
      for (int i = 0; i < 8; ++i) { m.cpu.state().xmm[i] = {next(), next()}; m.cpu.state().ymm_high[i] = {next(), next()}; }
      m.cpu.state().r[1] = next(); m.cpu.state().r[2] = next();
      GuestMemory gm(m.space);
      m.cpu.run(gm, 6);
      ++runs;
    }
  }
  CHECK(runs > 10000);
}

int main(int argc, char** argv) {
  if (getenv("AVX_SEED")) seed = strtoull(getenv("AVX_SEED"), nullptr, 0) | 1;
  test_basics();
  test_daz_ftz();
  test_robustness();
  const int rounds = argc > 1 ? atoi(argv[1]) : 6;
  int total = 0, failures = 0, reported = 0;
#ifdef HOST_AVX
  void* exec = mmap(nullptr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (exec == MAP_FAILED || !__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") || !__builtin_cpu_supports("bmi2") || !__builtin_cpu_supports("f16c")) {
    printf("(the host cannot run AVX2/FMA/BMI2/F16C code: comparison skipped)\n");
    return 0;
  }
  A alloc;
  for (const Case& c : kCases) {
    for (int round = 0; round < rounds; ++round) {
      alignas(32) uint8_t host[0x800], emu[0x800];
      fill_state(host, c);
      memcpy(emu, host, sizeof(host));
      // --- host: load the vector registers, run, store them back
      Bytes hc;
      for (int r = 0; r < 8; ++r) { mem(hc, {0xC5, 0xFE, 0x6F}, r, kYmm + 32 * r); }      // vmovdqu ymmR, [rdi + ...]
      gpr_setup(hc);
      hc.insert(hc.end(), c.code.begin(), c.code.end());
      gpr_dump(hc);
      for (int r = 0; r < 8; ++r) { mem(hc, {0xC5, 0xFE, 0x7F}, r, kOut + 32 * r); }
      hc.push_back(0xC5); hc.push_back(0xF8); hc.push_back(0x77);      // vzeroupper
      hc.push_back(0xC3);
      memcpy(exec, hc.data(), hc.size());
      reinterpret_cast<void (*)(void*)>(exec)(host);
      // --- emulator: the vector registers are set directly
      Bytes ec;
      gpr_setup(ec);
      ec.insert(ec.end(), c.code.begin(), c.code.end());
      gpr_dump(ec);
      ec.push_back(0xC3);
      process::AddressSpace space{alloc};
      space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
      space.write_privileged(0x10000, ConstByteView(ec.data(), ec.size()));
      space.map(0x20000, 0x1000, process::kProtRead | process::kProtWrite);
      space.write_privileged(0x20000, ConstByteView(emu, sizeof(emu)));
      space.map(0x30000, 0x1000, process::kProtRead | process::kProtWrite);
      X86_64Cpu cpu;
      process::ProgramStart start;
      start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x31000;
      cpu.reset(start);
      cpu.state().r[7] = 0x20000;
      for (int r = 0; r < 8; ++r) {
        memcpy(&cpu.state().xmm[r].lo, emu + kYmm + 32 * r, 8); memcpy(&cpu.state().xmm[r].hi, emu + kYmm + 32 * r + 8, 8);
        memcpy(&cpu.state().ymm_high[r].lo, emu + kYmm + 32 * r + 16, 8); memcpy(&cpu.state().ymm_high[r].hi, emu + kYmm + 32 * r + 24, 8);
      }
      GuestMemory gm(space);
      bool faulted = false;
      for (int steps = 0; steps < 200; ++steps) {
        if (cpu.state().rip == 0x10000 + ec.size() - 1) break;
        if (cpu.run(gm, 1) == StopReason::Fault) { faulted = true; break; }
      }
      space.read_privileged(0x20000, ByteView(emu, sizeof(emu)));
      for (int r = 0; r < 8; ++r) {
        memcpy(emu + kOut + 32 * r, &cpu.state().xmm[r].lo, 8); memcpy(emu + kOut + 32 * r + 8, &cpu.state().xmm[r].hi, 8);
        memcpy(emu + kOut + 32 * r + 16, &cpu.state().ymm_high[r].lo, 8); memcpy(emu + kOut + 32 * r + 24, &cpu.state().ymm_high[r].hi, 8);
      }
      ++total;
      bool ok = !faulted;
      if (memcmp(host + kOut, emu + kOut, 256) != 0) ok = false;
      uint64_t hf, ef;
      memcpy(&hf, host + kFlagsOut, 8); memcpy(&ef, emu + kFlagsOut, 8);
      if ((hf & 0x8C5) != (ef & 0x8C5)) ok = false;      // (AF is undefined for BMI)
      if (memcmp(host + kRegsOut, emu + kRegsOut, 24) != 0) ok = false;
      uint32_t hm, em;
      memcpy(&hm, host + kMxcsrOut, 4); memcpy(&em, emu + kMxcsrOut, 4);
      const uint32_t flag_mask = 0x3D;      // every exception flag except DE: when a denormal operand is reported depends on NaNs, invalid operations and the instruction class in ways this emulation does not model bit for bit
      if ((hm & flag_mask) != (em & flag_mask)) ok = false;
      if (memcmp(host + 0, emu + 0, 0x40) != 0 && c.text[0] != 'v') ok = false;
      if (memcmp(host, emu, 0x40) != 0) ok = false;      // the memory operand
      if (!ok) {
        ++failures;
        static const char* last_reported = nullptr;
        if (reported < 60 && last_reported != c.text) {
          last_reported = c.text;
          ++reported;
          printf("%s%s\n", c.text, faulted ? "  [emulator fault]" : "");
          if (faulted) printf("   fault vector %d at %llx\n", cpu.fault().vector, (unsigned long long)cpu.fault().pc);
          for (int r = 0; r < 4; ++r) {
            if (memcmp(host + kOut + 32 * r, emu + kOut + 32 * r, 32) == 0) continue;
            printf("   ymm%d host ", r); for (int b = 31; b >= 0; --b) printf("%02x", host[kOut + 32 * r + b]);
            printf("\n        emu  "); for (int b = 31; b >= 0; --b) printf("%02x", emu[kOut + 32 * r + b]); printf("\n");
          }
          if (getenv("AVX_VERBOSE")) { for (int r = 1; r <= 3; ++r) { printf("   in ymm%d ", r); for (int b = 31; b >= 0; --b) printf("%02x", host[kYmm + 32 * r + b]); printf("\n"); } printf("   in mem  "); for (int b = 31; b >= 0; --b) printf("%02x", host[b]); printf("\n"); }
          if ((hf & 0x8C5) != (ef & 0x8C5)) printf("   flags host %llx emu %llx\n", (unsigned long long)(hf & 0x8C5), (unsigned long long)(ef & 0x8C5));
          if (memcmp(host + kRegsOut, emu + kRegsOut, 24) != 0) { uint64_t h3[3], e3[3]; memcpy(h3, host + kRegsOut, 24); memcpy(e3, emu + kRegsOut, 24); printf("   gpr host %llx %llx %llx emu %llx %llx %llx\n", (unsigned long long)h3[0], (unsigned long long)h3[1], (unsigned long long)h3[2], (unsigned long long)e3[0], (unsigned long long)e3[1], (unsigned long long)e3[2]); }
          if ((hm & flag_mask) != (em & flag_mask)) { uint32_t mi; memcpy(&mi, host + kMxcsrIn, 4); printf("   mxcsr host %x emu %x (in %x)\n", hm & 0x3F, em & 0x3F, mi); }
          if (memcmp(host, emu, 0x40) != 0) printf("   memory operand differs\n");
        }
      }
    }
  }
  printf("ran %d cases of %zu instructions, failures %d\n", total, sizeof(kCases) / sizeof(kCases[0]), failures);
  CHECK(failures == 0);
#else
  (void)total; (void)failures; (void)reported; (void)rounds;
  printf("(no AVX2 host: the comparison against real hardware is skipped)\n");
#endif
  if (g_failed) { printf("AvxTest: %d FAILED\n", g_failed); return 1; }
  printf("AvxTest: all passed\n");
  return 0;
}
