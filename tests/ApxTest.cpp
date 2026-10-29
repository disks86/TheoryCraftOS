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

// Host test of the Intel APX register extension: the REX2 prefix, the sixteen extra general registers R16..R31 in modrm, SIB and opcode-embedded register fields, jmpabs, the XSAVE component
// that holds them, xgetbv and cpuid. Integer instructions are checked by running each once with R16 / R17 in REX2 form and once on RAX / RCX in plain form: the results and flags must match.
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

constexpr uint64_t kCode = 0x10000, kData = 0x20000, kStack = 0x22000;
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  uint64_t end = 0;
  explicit Machine(const Bytes& code) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtWrite | process::kProtExecute);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x4000, process::kProtRead | process::kProtWrite);
    end = kCode + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = kCode; start.stack_pointer = kStack;
    cpu.reset(start);
  }
  StopReason run(uint64_t n = 16) { GuestMemory m(space); return cpu.run(m, n); }
  bool finished() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  bool undefined() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc != end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  uint64_t peek64(uint64_t addr) { uint64_t v = 0; GuestMemory m(space); m.read(addr, ByteView(reinterpret_cast<uint8_t*>(&v), 8)); return v; }
};
static Bytes with_end(Bytes b) { b.push_back(0x0F); b.push_back(0x0B); return b; }
static X86_64Cpu::State& st(Machine& m) { return m.cpu.state(); }

static void test_registers() {
  {      // mov r16d, imm32; mov r17, imm64
    Machine m(with_end({0xD5, 0x10, 0xB8, 0x78, 0x56, 0x34, 0x12, 0xD5, 0x18, 0xB9, 1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(m.finished());
    CHECK(st(m).r[16] == 0x12345678 && st(m).r[17] == 0x0807060504030201ull && st(m).r[0] == 0 && st(m).r[1] == 0);
  }
  {      // memory forms: r20 as base, r21 as index
    Machine m(with_end({0xD5, 0x58, 0x89, 0xC1, 0xD5, 0x38, 0x8B, 0x44, 0xAC, 0x08}));
    st(m).r[16] = 0x1122334455667788ull;
    st(m).r[20] = kData;
    st(m).r[21] = 4;
    const uint64_t v = 0xDEADBEEFCAFEF00Dull;
    m.poke(kData + 4 * 4 + 8, &v, 8);
    CHECK(m.finished());
    CHECK(st(m).r[17] == 0x1122334455667788ull && st(m).r[0] == v);
  }
  {      // the index field 100 with X4 is r20, not "no index"; without it nothing is added
    Machine with(with_end({0xD5, 0x20, 0x8B, 0x04, 0x23}));      // mov eax, [rbx + r20]  (SIB: scale 0, index 100 + X4, base 011)
    st(with).r[3] = kData;
    st(with).r[20] = 8;
    const uint32_t v = 0x600D;
    with.poke(kData + 8, &v, 4);
    CHECK(with.finished() && st(with).r[0] == 0x600D);
    Machine without(with_end({0x8B, 0x04, 0x23}));
    st(without).r[3] = kData + 8;
    without.poke(kData + 8, &v, 4);
    CHECK(without.finished() && st(without).r[0] == 0x600D);
  }
  {      // push r16; pop r17
    Machine m(with_end({0xD5, 0x10, 0x50, 0xD5, 0x10, 0x59}));
    st(m).r[16] = 0xABCDEF0123456789ull;
    CHECK(m.finished());
    CHECK(st(m).r[17] == 0xABCDEF0123456789ull && st(m).r[4] == kStack);
  }
  {      // sete r16b
    Machine m(with_end({0x31, 0xC0, 0xD5, 0x90, 0x94, 0xC0}));
    st(m).r[16] = 0xFFFFFFFFFFFFFF00ull;
    CHECK(m.finished());
    CHECK(st(m).r[16] == 0xFFFFFFFFFFFFFF01ull);
  }
  {      // bswap r18 (0F C8+r with M0 and B4)
    Machine m(with_end({0xD5, 0x90, 0xCA}));
    st(m).r[18] = 0x1122334455667788ull;
    CHECK(m.finished() && (st(m).r[18] & 0xFFFFFFFFull) == 0x88776655);
  }
  {      // jmpabs
    Bytes code = with_end({0xD5, 0x00, 0xA1, 0x00, 0x01, 0x01, 0x00, 0, 0, 0, 0});
    code.resize(0x100, 0x90);
    for (uint8_t b : {0xB8, 0x07, 0, 0, 0, 0x0F, 0x0B}) code.push_back(b);
    Machine m(code);
    CHECK(m.finished());
    CHECK(st(m).r[0] == 7);
    Machine bad(with_end({0xD5, 0x08, 0xA1, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(bad.undefined() && bad.cpu.fault().pc == kCode);
    Machine plain(with_end({0xA1, 0, 0, 0, 0, 0, 0, 0, 0}));      // mov eax, moffs is fine without REX2... (it reads address 0: a page fault)
    CHECK(plain.run() == StopReason::Fault && plain.cpu.fault().vector == 14);
  }
}

// The same integer instruction on R16 (reg) / R17 (rm) with REX2 and on RAX / RCX without it.
static void test_equivalence() {
  struct Form { Bytes opcode; bool map0f; bool has_imm; };
  std::vector<Form> forms;
  for (uint8_t op : {0x01, 0x03, 0x09, 0x0B, 0x11, 0x13, 0x19, 0x1B, 0x21, 0x23, 0x29, 0x2B, 0x31, 0x33, 0x39, 0x3B, 0x85, 0x87, 0x89, 0x8B}) forms.push_back({{op}, false, false});
  for (uint8_t op : {0xAF, 0xBC, 0xBD, 0xB8, 0xA3, 0xAB, 0xB3, 0xBB, 0xC1, 0xB6, 0xB7, 0xBE, 0xBF, 0x44, 0x4F, 0x92}) forms.push_back({{op}, true, false});
  for (uint8_t op : {0xA4, 0xAC}) forms.push_back({{op}, true, true});      // shld / shrd by an immediate
  for (uint8_t op : {0xD1, 0xF7, 0xFF, 0xF6}) forms.push_back({{op}, false, false});
  for (const Form& f : forms)
    for (int variant = 0; variant < (f.opcode[0] == 0xD1 ? 8 : f.opcode[0] == 0xF7 || f.opcode[0] == 0xF6 ? 4 : f.opcode[0] == 0xFF ? 2 : 1); ++variant)
      for (int iter = 0; iter < 40; ++iter) {
        const bool wide = rnd() & 1;
        // plain: [REX.W] op modrm(reg = 0 (rax), rm = 1 (rcx)); rex2: D5 (M0 | R4 | B4 | W) op modrm
        const uint8_t reg_field = (f.opcode[0] >= 0xD0 || f.opcode[0] == 0xF6) ? uint8_t(variant) : 0;      // groups use /n
        const uint8_t modrm_plain = uint8_t(0xC0 | (reg_field << 3) | 1);
        Bytes plain, extended;
        if (wide) plain.push_back(0x48);
        if (f.map0f) plain.push_back(0x0F);
        plain.insert(plain.end(), f.opcode.begin(), f.opcode.end());
        plain.push_back(modrm_plain);
        const bool group = f.opcode[0] >= 0xD0 || f.opcode[0] == 0xF6;
        extended.push_back(0xD5);
        extended.push_back(uint8_t((f.map0f ? 0x80 : 0) | (group ? 0 : 0x40) | 0x10 | (wide ? 8 : 0)));
        extended.insert(extended.end(), f.opcode.begin(), f.opcode.end());
        extended.push_back(modrm_plain);
        if (f.has_imm) { const uint8_t imm = uint8_t(rnd() % 70); plain.push_back(imm); extended.push_back(imm); }
        if (f.opcode[0] == 0xF6 && variant < 2) { plain.push_back(0x5A); extended.push_back(0x5A); }
        if (f.opcode[0] == 0xF7 && variant < 2) { for (int i = 0; i < 4; ++i) { plain.push_back(uint8_t(0x11 * (i + 1))); extended.push_back(uint8_t(0x11 * (i + 1))); } }
        if (f.opcode[0] == 0xC1) { plain.push_back(3); extended.push_back(3); }
        Machine a(with_end(plain)), b(with_end(extended));
        const uint64_t v0 = rnd(), v1 = rnd() & (rnd() & 1 ? ~0ull : 0xFF);
        const uint64_t fl = rnd() & 0x8D5;
        st(a).r[0] = v0; st(a).r[1] = v1; st(a).rflags = 0x202 | fl;
        st(b).r[16] = v0; st(b).r[17] = v1; st(b).rflags = 0x202 | fl;
        st(a).r[4] = kStack; st(b).r[4] = kStack;
        const bool ra = a.finished(), rb = b.finished();
        if (ra != rb) printf("  op %02x map0f %d variant %d: plain %d extended %d\n", f.opcode[0], f.map0f, variant, ra, rb);
        CHECK(ra == rb);
        if (!ra) continue;
        if (st(a).r[0] != st(b).r[16] || st(a).r[1] != st(b).r[17] || st(a).rflags != st(b).rflags || st(b).r[0] != 0 || st(b).r[1] != 0)
          printf("  op %02x map0f %d variant %d wide %d: %llx/%llx vs %llx/%llx\n", f.opcode[0], f.map0f, variant, wide, (unsigned long long)st(a).r[0], (unsigned long long)st(a).r[1], (unsigned long long)st(b).r[16], (unsigned long long)st(b).r[17]);
        CHECK(st(a).r[0] == st(b).r[16] && st(a).r[1] == st(b).r[17] && st(a).rflags == st(b).rflags);
        CHECK(st(b).r[0] == 0 && st(b).r[1] == 0);
      }
}

static void test_invalid() {
  Machine addps_plain(with_end({0xD5, 0x80, 0x58, 0xC1}));      // addps xmm0, xmm1 through REX2 with no register bits: fine
  CHECK(addps_plain.finished());
  Machine addps_high(with_end({0xD5, 0x90, 0x58, 0xC1}));       // B4 on an SSE instruction
  CHECK(addps_high.undefined() && addps_high.cpu.fault().pc == kCode);
  Machine x87(with_end({0xD5, 0x10, 0xD9, 0xC0}));
  CHECK(x87.undefined() && x87.cpu.fault().pc == kCode);
  Machine after_rex(with_end({0x48, 0xD5, 0x10, 0x90}));
  CHECK(after_rex.undefined() && after_rex.cpu.fault().pc == kCode);
  Machine then_rex(with_end({0xD5, 0x10, 0x48, 0x90}));
  CHECK(then_rex.undefined() && then_rex.cpu.fault().pc == kCode);
  Machine then_vex(with_end({0xD5, 0x10, 0xC5, 0xF8, 0x77}));
  CHECK(then_vex.undefined() && then_vex.cpu.fault().pc == kCode);
  Machine legacy_after(with_end({0xD5, 0x10, 0x66, 0x90}));
  CHECK(legacy_after.undefined() && legacy_after.cpu.fault().pc == kCode);
  Machine truncated(Bytes{0xD5});
  CHECK(truncated.run() == StopReason::Fault);
}

static void test_state() {
  {      // xgetbv, cpuid, xsave / xrstor of R16..R31
    Machine m(with_end({0x31, 0xC9, 0x0F, 0x01, 0xD0}));
    CHECK(m.finished() && (st(m).r[0] & 0x80000));
    Machine c({0xB8, 7, 0, 0, 0, 0xB9, 1, 0, 0, 0, 0x0F, 0xA2, 0x0F, 0x0B});
    CHECK(c.finished() && ((st(c).r[2] >> 21) & 1));
    Machine d({0xB8, 0xD, 0, 0, 0, 0xB9, 19, 0, 0, 0, 0x0F, 0xA2, 0x0F, 0x0B});
    CHECK(d.finished() && st(d).r[0] == 128 && st(d).r[3] == 11008);
  }
  {
    // xsave rfbm = 0x80000; wipe r16..r31; xrstor
    Bytes code = {0xB8, 0, 0, 8, 0, 0xBA, 0, 0, 0, 0,        // eax = 0x80000, edx = 0
                  0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0,         // rdi = kData
                  0x0F, 0xAE, 0x27,                            // xsave [rdi]
                  0x0F, 0xAE, 0x2F};                           // xrstor [rdi]
    Machine m(with_end(code));
    for (uint32_t i = 16; i < 32; ++i) st(m).r[i] = 0x0101010101010101ull * i;
    // run the xsave, then clobber, then the xrstor: split with a budget
    CHECK(m.run(4) == StopReason::Budget);
    for (uint32_t i = 16; i < 32; ++i) {
      CHECK(m.peek64(kData + 11008 + 8 * (i - 16)) == 0x0101010101010101ull * i);
      st(m).r[i] = 0;
    }
    uint64_t bv = m.peek64(kData + 512);
    CHECK(bv & 0x80000);
    CHECK(m.finished());
    for (uint32_t i = 16; i < 32; ++i) CHECK(st(m).r[i] == 0x0101010101010101ull * i);
  }
  {      // all-zero state is initial: XSTATE_BV bit 19 clear
    Bytes code = {0xB8, 0, 0, 8, 0, 0xBA, 0, 0, 0, 0, 0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0, 0x0F, 0xAE, 0x27};
    Machine m(with_end(code));
    CHECK(m.finished());
    CHECK((m.peek64(kData + 512) & 0x80000) == 0);
  }
}

static void test_malformed() {
  Bytes all;
  for (const Bytes& part : {Bytes{0xD5, 0x58, 0x89, 0xC1}, Bytes{0xD5, 0x38, 0x8B, 0x44, 0xAC, 0x08}, Bytes{0xD5, 0x90, 0x94, 0xC0}, Bytes{0xD5, 0x00, 0xA1, 1, 2, 3, 4, 5, 6, 7, 8}, Bytes{0xD5, 0x10, 0x50}})
    all.insert(all.end(), part.begin(), part.end());
  for (size_t cut = 0; cut < all.size(); ++cut) { Machine m(Bytes(all.begin(), all.begin() + cut)); m.run(8); }
  for (size_t i = 0; i < all.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes b = all;
      b[i] ^= uint8_t(1 << bit);
      Machine m(b);
      for (uint32_t r = 0; r < 32; ++r) st(m).r[r] = kData + 64 * r;
      m.run(16);
    }
}

int main() {
  test_registers();
  test_equivalence();
  test_invalid();
  test_state();
  test_malformed();
  if (g_failed) { printf("ApxTest: %d failed\n", g_failed); return 1; }
  printf("ApxTest: all passed\n");
  return 0;
}
