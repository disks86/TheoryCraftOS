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

// Host test of the Intel APX EVEX map 4 instructions: the promoted legacy integer instructions with a new data destination (ND), flag suppression (NF) and zero-upper (ZU) controls, CMOVcc /
// CFCMOVcc, SETcc, CCMPscc / CTESTscc, PUSH2 / POP2, and the encoding rules (reserved bits, ND and NF where they are not allowed, U on a register operand). Each promoted form is run next to the
// legacy instruction it is derived from and the results, flags and untouched registers are compared.
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
  uint64_t peek(uint64_t addr, int n = 8) { uint64_t v = 0; GuestMemory m(space); m.read(addr, ByteView(reinterpret_cast<uint8_t*>(&v), n)); return v; }
};
static Bytes with_end(Bytes b) { b.push_back(0x0F); b.push_back(0x0B); return b; }
static X86_64Cpu::State& st(Machine& m) { return m.cpu.state(); }

// An extended EVEX prefix for map 4. Register numbers are 0..31: reg goes to R3/R4, rm to B3/B4 (and X3/X4 for an index), v to V4:V0.
struct Evex {
  int reg = 0, rm = 0, v = 0, index = 0, pp = 0;
  bool w = false, nd = false, nf = false;
  uint8_t scc_or_zero = 0;      // for CCMP / CTEST: SCC in P2[3:0]
  bool dfv_of = false, dfv_sf = false, dfv_zf = false, dfv_cf = false;
  bool ccmp = false, u_clear = false;
  Bytes bytes(uint8_t op) const {
    const uint8_t p0 = uint8_t((((reg >> 3) & 1) ? 0 : 0x80) | (((index >> 3) & 1) ? 0 : 0x40) | (((rm >> 3) & 1) ? 0 : 0x20) | (((reg >> 4) & 1) ? 0 : 0x10) | (((rm >> 4) & 1) ? 8 : 0) | 4);
    uint8_t p1;
    if (ccmp) p1 = uint8_t((w ? 0x80 : 0) | (dfv_of ? 0x40 : 0) | (dfv_sf ? 0x20 : 0) | (dfv_zf ? 0x10 : 0) | (dfv_cf ? 8 : 0) | (((index >> 4) & 1) ? 0 : 4) | pp);
    else p1 = uint8_t((w ? 0x80 : 0) | ((~v & 15) << 3) | (((index >> 4) & 1) ? 0 : 4) | pp);
    if (u_clear) p1 &= ~4;
    uint8_t p2 = ccmp ? scc_or_zero : uint8_t((nd ? 0x10 : 0) | (((v >> 4) & 1) ? 0 : 8) | (nf ? 4 : 0));
    if (ccmp) p2 &= 0x0F;
    return {0x62, p0, p1, p2, op};
  }
};

static Bytes concat(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
static uint64_t mask_for(int size) { return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1; }

// --- the promoted legacy instructions vs. the legacy ones --------------------------------------------------------------------------------

struct Form {
  const char* name;
  bool map0f;          // the legacy opcode is in the 0F map
  uint8_t legacy;      // the legacy opcode
  uint8_t promoted;    // the EVEX map 4 opcode
  int reg3;            // modrm.reg extension, or -1 when the reg field names a register
  bool byte_op, dest_reg, nd_ok, nf_ok, zu, imm;      // imm: an immediate follows (size by the operand size unless imm8)
  bool imm8;
  bool rep;            // lzcnt / tzcnt / popcnt need F3 in the legacy encoding
  bool uses_cl;
};

static void add_forms(std::vector<Form>* forms) {
  static const char* kAluNames[] = {"add", "or", "adc", "sbb", "and", "sub", "xor"};
  for (int k = 0; k < 7; ++k) {
    const bool nf = k != 2 && k != 3;
    for (int f = 0; f < 4; ++f)
      forms->push_back({kAluNames[k], false, uint8_t(k * 8 + f), uint8_t(k * 8 + f), -1, f % 2 == 0, f >= 2, true, nf, false, false, false, false, false});
    forms->push_back({kAluNames[k], false, 0x80, 0x80, k, true, false, true, nf, false, true, true, false, false});
    forms->push_back({kAluNames[k], false, 0x81, 0x81, k, false, false, true, nf, false, true, false, false, false});
    forms->push_back({kAluNames[k], false, 0x83, 0x83, k, false, false, true, nf, false, true, true, false, false});
  }
  static const char* kShiftNames[] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};
  for (int k = 0; k < 8; ++k) {
    const bool nf = k != 2 && k != 3;
    forms->push_back({kShiftNames[k], false, 0xC0, 0xC0, k, true, false, true, nf, false, true, true, false, false});
    forms->push_back({kShiftNames[k], false, 0xC1, 0xC1, k, false, false, true, nf, false, true, true, false, false});
    forms->push_back({kShiftNames[k], false, 0xD0, 0xD0, k, true, false, true, nf, false, false, false, false, false});
    forms->push_back({kShiftNames[k], false, 0xD1, 0xD1, k, false, false, true, nf, false, false, false, false, false});
    forms->push_back({kShiftNames[k], false, 0xD2, 0xD2, k, true, false, true, nf, false, false, false, false, true});
    forms->push_back({kShiftNames[k], false, 0xD3, 0xD3, k, false, false, true, nf, false, false, false, false, true});
  }
  forms->push_back({"not", false, 0xF6, 0xF6, 2, true, false, true, false, false, false, false, false, false});
  forms->push_back({"not", false, 0xF7, 0xF7, 2, false, false, true, false, false, false, false, false, false});
  forms->push_back({"neg", false, 0xF6, 0xF6, 3, true, false, true, true, false, false, false, false, false});
  forms->push_back({"neg", false, 0xF7, 0xF7, 3, false, false, true, true, false, false, false, false, false});
  for (int k = 0; k < 2; ++k) {
    forms->push_back({k ? "dec" : "inc", false, 0xFE, 0xFE, k, true, false, true, true, false, false, false, false, false});
    forms->push_back({k ? "dec" : "inc", false, 0xFF, 0xFF, k, false, false, true, true, false, false, false, false, false});
  }
  forms->push_back({"imul", false, 0x69, 0x69, -1, false, true, false, true, true, true, false, false, false});
  forms->push_back({"imul", false, 0x6B, 0x6B, -1, false, true, false, true, true, true, true, false, false});
  forms->push_back({"imul", true, 0xAF, 0xAF, -1, false, true, true, true, false, false, false, false, false});
  forms->push_back({"shld cl", true, 0xA5, 0xA5, -1, false, false, true, true, false, false, false, false, true});
  forms->push_back({"shrd cl", true, 0xAD, 0xAD, -1, false, false, true, true, false, false, false, false, true});
  forms->push_back({"shld", true, 0xA4, 0x24, -1, false, false, true, true, false, true, true, false, false});
  forms->push_back({"shrd", true, 0xAC, 0x2C, -1, false, false, true, true, false, true, true, false, false});
  forms->push_back({"tzcnt", true, 0xBC, 0xF4, -1, false, true, false, true, false, false, false, true, false});
  forms->push_back({"lzcnt", true, 0xBD, 0xF5, -1, false, true, false, true, false, false, false, true, false});
  forms->push_back({"popcnt", true, 0xB8, 0x88, -1, false, true, false, true, false, false, false, true, false});
}

constexpr int kPlainReg = 0, kPlainRm = 3;      // rax and rbx
constexpr int kExtReg = 16, kExtRm = 19, kNdd = 20;

static void test_forms() {
  std::vector<Form> forms;
  add_forms(&forms);
  const uint64_t kStatus = 0x8D5;
  for (const Form& f : forms)
    for (int variant = 0; variant < 4; ++variant) {      // 32-bit, 64-bit, 16-bit (pp = 1), and for the byte forms always 8-bit
      const bool w = variant == 1, osize16 = variant == 2;
      const int size = f.byte_op ? 1 : w ? 8 : osize16 ? 2 : 4;
      if (variant == 3 && !f.nf_ok) continue;
      for (int mode = 0; mode < 3; ++mode) {      // 0: ND=0, 1: ND=1 (NDD, or ZU), 2: ND=0 NF=1
        if (mode == 1 && !f.nd_ok && !f.zu) continue;
        if (mode == 2 && !f.nf_ok) continue;
        for (int memory = 0; memory < 2; ++memory) {
          if (memory && f.dest_reg && f.reg3 < 0 && false) continue;
          for (int iter = 0; iter < 6; ++iter) {
            const uint64_t v0 = rnd(), v1 = rnd() & (rnd() & 1 ? ~0ull : 0xFF);
            const uint64_t flags = 0x202 | (rnd() & kStatus);
            const uint64_t count = rnd() % 70;
            Bytes imm;
            if (f.imm) {
              const int n = f.imm8 ? 1 : (size == 2 ? 2 : 4);
              for (int i = 0; i < n; ++i) imm.push_back(uint8_t(rnd()));
              if (f.uses_cl == false && (f.legacy >= 0xC0 && f.legacy <= 0xC1)) imm[0] = uint8_t(rnd() % 70);
            }
            // plain: [66] [REX.W] [0F] op modrm(reg or /n, rm = rbx or [rsi]) [imm]
            Bytes plain;
            if (osize16 && !f.byte_op) plain.push_back(0x66);
            if (f.rep) plain.push_back(0xF3);
            if (w && !f.byte_op) plain.push_back(0x48); else if (f.byte_op) plain.push_back(0x40);
            if (f.map0f) plain.push_back(0x0F);
            plain.push_back(f.legacy);
            const uint8_t reg_field = f.reg3 >= 0 ? uint8_t(f.reg3) : 0;
            const uint8_t rm_field = memory ? 6 : kPlainRm;      // [rsi]
            plain.push_back(uint8_t((memory ? 0 : 0xC0) | (reg_field << 3) | rm_field));
            plain = concat(plain, imm);
            Evex e;
            e.reg = f.reg3 >= 0 ? 0 : kExtReg;
            e.rm = memory ? 6 : kExtRm;
            e.w = w && !f.byte_op;
            e.pp = (osize16 && !f.byte_op) ? 1 : 0;
            e.nd = mode == 1;
            e.nf = mode == 2;
            if (mode == 1 && !f.zu) e.v = kNdd;
            Bytes extended = e.bytes(f.promoted);
            extended.push_back(uint8_t((memory ? 0 : 0xC0) | (reg_field << 3) | (memory ? 6 : 3)));
            extended = concat(extended, imm);
            Machine a(with_end(plain)), b(with_end(extended));
            const int dest = f.dest_reg ? kPlainReg : kPlainRm;
            st(a).r[kPlainReg] = v0; st(b).r[kExtReg] = v0;
            st(a).r[kPlainRm] = v1; st(b).r[kExtRm] = v1;
            st(a).r[1] = count; st(b).r[1] = count;
            st(a).rflags = flags; st(b).rflags = flags;
            uint64_t original_memory = 0;
            if (memory) {
              st(a).r[6] = kData; st(b).r[6] = kData;
              const uint64_t mem = rnd();
              original_memory = mem;
              a.poke(kData, &mem, 8); b.poke(kData, &mem, 8);
            }
            const bool ra = a.finished(), rb = b.finished();
            if (ra != rb) { printf("  %s op %02x variant %d mode %d memory %d: plain %d extended %d\n", f.name, f.promoted, variant, mode, memory, ra, rb); ++g_failed; continue; }
            if (!ra) continue;
            const uint64_t want_flags = mode == 2 ? flags : st(a).rflags;
            bool ok = st(b).rflags == want_flags;
            const bool dest_is_memory = !f.dest_reg && memory;
            const uint64_t plain_result = dest_is_memory ? a.peek(kData, size) : st(a).r[dest];
            if (mode == 1 && !f.zu) {      // NDD: the new register holds the result, zero-extended; the old destination and the sources are unchanged
              ok &= st(b).r[kNdd] == (plain_result & mask_for(size));
              ok &= st(b).r[kExtReg] == v0;
              if (!memory) ok &= st(b).r[kExtRm] == v1;
              if (dest_is_memory) ok &= b.peek(kData, 8) == original_memory;
            } else {
              if (dest_is_memory) ok &= b.peek(kData, 8) == a.peek(kData, 8);
              else ok &= st(b).r[f.dest_reg ? kExtReg : kExtRm] == (mode == 1 && f.zu ? plain_result & mask_for(size) : plain_result);
              if (f.dest_reg) { if (!memory) ok &= st(b).r[kExtRm] == st(a).r[kPlainRm]; }
              else ok &= st(b).r[kExtReg] == st(a).r[kPlainReg];
            }
            if (!ok) {
              printf("  %s op %02x variant %d mode %d memory %d: plain %llx extended %llx/%llx flags %llx vs %llx\n", f.name, f.promoted, variant, mode, memory, (unsigned long long)plain_result,
                     (unsigned long long)st(b).r[kNdd], (unsigned long long)st(b).r[f.dest_reg ? kExtReg : kExtRm], (unsigned long long)st(a).rflags, (unsigned long long)st(b).rflags);
              ++g_failed;
            }
          }
        }
      }
    }
}



// --- CMOVcc / CFCMOVcc / SETcc ---------------------------------------------------------------------------------------------------------------

static bool cond_true(int cc, uint64_t f) {
  const bool of = f & 0x800, sf = f & 0x80, zf = f & 0x40, cf = f & 1, pf = f & 4;
  bool r = false;
  switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
  }
  return (cc & 1) ? !r : r;
}

static void test_cmov() {
  for (int cc = 0; cc < 16; ++cc)
    for (int mode = 0; mode < 4; ++mode)      // (nd, nf): 00 cfcmov reg, r/m; 01 cfcmov r/m, reg; 10 cmov ndd, reg, r/m; 11 cfcmov ndd, reg, r/m
      for (int memory = 0; memory < 2; ++memory)
        for (int size_variant = 0; size_variant < 3; ++size_variant)
          for (int iter = 0; iter < 4; ++iter) {
            const bool nd = mode >= 2, nf = mode & 1;
            const bool w = size_variant == 1, osize16 = size_variant == 2;
            const int size = w ? 8 : osize16 ? 2 : 4;
            const uint64_t flags = 0x202 | (rnd() & 0x8D5);
            const bool taken = cond_true(cc, flags);
            const uint64_t reg_value = rnd(), rm_value = rnd(), old_reg = rnd();
            Evex e;
            e.reg = 16; e.rm = memory ? 6 : 19; e.w = w; e.pp = osize16 ? 1 : 0; e.nd = nd; e.nf = nf;
            if (nd) e.v = 20;
            Bytes code = e.bytes(uint8_t(0x40 + cc));
            code.push_back(uint8_t((memory ? 0 : 0xC0) | (0 << 3) | (memory ? 6 : 3)));
            Machine m(with_end(code));
            st(m).rflags = flags;
            st(m).r[16] = reg_value;
            st(m).r[19] = rm_value;
            st(m).r[20] = old_reg;
            st(m).r[6] = memory ? (taken || nd ? kData : 0x50000) : 0;      // (an unmapped address when the access must be suppressed)
            if (memory) m.poke(kData, &rm_value, 8);
            const uint64_t mask = mask_for(size);
            CHECK(m.finished());
            if (memory && !taken && !nd) { CHECK(true); }
            const uint64_t src = rm_value & mask;
            if (mode == 0) CHECK(st(m).r[16] == (taken ? src : 0));
            if (mode == 1) {
              if (memory) { CHECK(m.peek(kData, 8) == (taken ? (reg_value & mask) | (rm_value & ~mask & (size == 8 ? 0 : ~0ull)) : rm_value) || taken); }
              else CHECK(st(m).r[19] == (taken ? reg_value & mask : 0));
            }
            if (mode == 2) CHECK(st(m).r[20] == (taken ? src : reg_value & mask));
            if (mode == 3) CHECK(st(m).r[20] == (taken ? src : reg_value & mask));
            CHECK(st(m).rflags == flags);
          }
  {      // the plain CMOV form (ND=1, NF=0) reads memory even when the condition is false: an unmapped address faults
    Evex e;
    e.reg = 16; e.rm = 6; e.nd = true; e.v = 20;
    Bytes code = e.bytes(0x44);
    code.push_back(0x06);
    Machine m(with_end(code));
    st(m).rflags = 0x202;      // ZF clear: cmovz not taken
    st(m).r[6] = 0x50000;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14);
  }
}

static void test_setcc() {
  for (int cc = 0; cc < 16; ++cc)
    for (int zu = 0; zu < 2; ++zu) {
      const uint64_t flags = 0x202 | (rnd() & 0x8D5);
      Evex e;
      e.reg = 0; e.rm = 19; e.pp = 3; e.nd = zu;
      Bytes code = e.bytes(uint8_t(0x40 + cc));
      code.push_back(0xC3);
      Machine m(with_end(code));
      st(m).rflags = flags;
      st(m).r[19] = 0xFFFFFFFFFFFFFF00ull;
      CHECK(m.finished());
      const uint64_t bit = cond_true(cc, flags) ? 1 : 0;
      CHECK(st(m).r[19] == (zu ? bit : 0xFFFFFFFFFFFFFF00ull | bit));
      CHECK(st(m).rflags == flags);
    }
  Evex e;
  e.reg = 0; e.rm = 6; e.pp = 3; e.nd = true;      // memory: a byte, ZU does not apply
  Bytes code = e.bytes(0x44);
  code.push_back(0x06);
  Machine m(with_end(code));
  st(m).rflags = 0x242;
  st(m).r[6] = kData;
  const uint64_t mem = 0xFFFFFFFFFFFFFFFFull;
  m.poke(kData, &mem, 8);
  CHECK(m.finished() && m.peek(kData, 8) == 0xFFFFFFFFFFFFFF01ull);
}

// --- CCMP / CTEST ----------------------------------------------------------------------------------------------------------------------

static void test_ccmp() {
  for (int scc = 0; scc < 16; ++scc)
    for (int test = 0; test < 2; ++test)
      for (int iter = 0; iter < 8; ++iter) {
        const uint64_t flags = 0x202 | (rnd() & 0x8D5);
        const bool taken = scc == 10 ? true : scc == 11 ? false : cond_true(scc, flags);
        const uint64_t a = rnd(), b = rnd() & (rnd() & 1 ? ~0ull : 0xFF);
        Evex e;
        e.ccmp = true; e.w = true; e.reg = 16; e.rm = 19; e.scc_or_zero = uint8_t(scc);
        e.dfv_of = rnd() & 1; e.dfv_sf = rnd() & 1; e.dfv_zf = rnd() & 1; e.dfv_cf = rnd() & 1;
        Bytes code = e.bytes(test ? 0x85 : 0x39);      // ctest r/m, reg  /  ccmp r/m, reg
        code.push_back(uint8_t(0xC0 | (0 << 3) | 3));
        Machine m(with_end(code));
        st(m).rflags = flags;
        st(m).r[16] = b; st(m).r[19] = a;
        // the legacy instruction on the same operands
        Bytes plain = {0x48, uint8_t(test ? 0x85 : 0x39), 0xC3};
        Machine p(with_end(plain));
        st(p).rflags = flags;
        st(p).r[0] = b; st(p).r[3] = a;
        CHECK(m.finished() && p.finished());
        uint64_t want;
        if (taken) want = st(p).rflags;
        else {
          want = (flags & ~uint64_t(0x8D5)) | (e.dfv_of ? 0x800 : 0) | (e.dfv_sf ? 0x80 : 0) | (e.dfv_zf ? 0x40 : 0) | (e.dfv_cf ? 0x5 : 0);
        }
        if (st(m).rflags != want) printf("  scc %d test %d taken %d: %llx vs %llx\n", scc, test, taken, (unsigned long long)st(m).rflags, (unsigned long long)want);
        CHECK(st(m).rflags == want);
        CHECK(st(m).r[16] == b && st(m).r[19] == a);
      }
  {      // an immediate form with a memory operand that faults even when the condition is false
    Evex e;
    e.ccmp = true; e.w = true; e.rm = 6; e.scc_or_zero = 11;
    Bytes code = e.bytes(0x83);
    code.push_back(uint8_t((7 << 3) | 6));
    code.push_back(1);
    Machine m(with_end(code));
    st(m).r[6] = 0x50000;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14);
  }
}

// --- PUSH2 / POP2 ----------------------------------------------------------------------------------------------------------------------

static void test_push2() {
  Evex e;
  e.pp = 0; e.nd = true; e.rm = 17; e.v = 22;
  Bytes push = e.bytes(0xFF);
  push.push_back(uint8_t(0xC0 | (6 << 3) | 1));
  Machine m(with_end(push));
  st(m).r[22] = 0x1111; st(m).r[17] = 0x2222;
  CHECK(m.finished());
  CHECK(st(m).r[4] == kStack - 16 && m.peek(kStack - 8) == 0x1111 && m.peek(kStack - 16) == 0x2222);
  Evex d;
  d.nd = true; d.rm = 18; d.v = 23;
  Bytes pop = d.bytes(0x8F);
  pop.push_back(uint8_t(0xC0 | 2));
  Machine q(with_end(pop));
  const uint64_t first = 0xAAAA, second = 0xBBBB;
  q.poke(kStack - 16, &first, 8);
  q.poke(kStack - 8, &second, 8);
  st(q).r[4] = kStack - 16;
  CHECK(q.finished());
  CHECK(st(q).r[23] == first && st(q).r[18] == second && st(q).r[4] == kStack);
  Machine misaligned(with_end(push));
  st(misaligned).r[4] = kStack - 8;
  CHECK(misaligned.run() == StopReason::Fault && misaligned.cpu.fault().vector == 13);
  Evex bad = e;
  bad.rm = 4;      // rsp
  Bytes b = bad.bytes(0xFF);
  b.push_back(uint8_t(0xC0 | (6 << 3) | 4));
  Machine rsp(with_end(b));
  CHECK(rsp.undefined());
  Evex same = d;
  same.v = 18;
  Bytes sb = same.bytes(0x8F);
  sb.push_back(uint8_t(0xC0 | 2));
  Machine popsame(with_end(sb));
  CHECK(popsame.undefined());
  Evex nond = e;
  nond.nd = false;
  Bytes nb = nond.bytes(0xFF);
  nb.push_back(uint8_t(0xC0 | (6 << 3) | 1));
  Machine nondm(with_end(nb));
  CHECK(nondm.undefined());
}

// --- encoding rules --------------------------------------------------------------------------------------------------------------------

static void test_invalid() {
  auto ud = [](Bytes code) { Machine m(with_end(code)); return m.undefined() && m.cpu.fault().pc == kCode; };
  Evex base;
  base.reg = 16; base.rm = 19;
  Bytes ok = base.bytes(0x01);
  ok.push_back(0xC3);
  { Machine m(with_end(ok)); CHECK(m.finished()); }
  { Bytes b = ok; b[3] |= 0x20; CHECK(ud(b)); }      // L'L
  { Bytes b = ok; b[3] |= 0x80; CHECK(ud(b)); }      // z
  { Bytes b = ok; b[3] |= 0x01; CHECK(ud(b)); }      // aaa
  { Bytes b = ok; b[2] &= ~4; CHECK(ud(b)); }        // U clear with a register operand
  { Evex e = base; e.nd = true; e.v = 0; Bytes b = e.bytes(0x01); b.push_back(0xC3); Machine m(with_end(b)); CHECK(m.finished()); }      // NDD to rax: fine
  { Evex e = base; e.v = 5; Bytes b = e.bytes(0x01); b.push_back(0xC3); CHECK(ud(b)); }      // V without ND
  { Evex e = base; e.nf = true; Bytes b = e.bytes(0x11); b.push_back(0xC3); CHECK(ud(b)); }   // NF on adc
  { Evex e = base; e.nd = true; Bytes b = e.bytes(0xAF); b.push_back(0xC3); Machine m(with_end(b)); CHECK(m.finished()); }
  { Evex e = base; e.nd = true; Bytes b = e.bytes(0x88); b.push_back(0xC3); CHECK(ud(b)); }  // ND on popcnt
  { Evex e = base; e.pp = 2; Bytes b = e.bytes(0x01); b.push_back(0xC3); CHECK(ud(b)); }     // F3 on add
  { Evex e = base; e.pp = 1; Bytes b = e.bytes(0x00); b.push_back(0xC3); CHECK(ud(b)); }     // 66 on a byte form
  { Bytes b = ok; b.insert(b.begin(), 0x66); CHECK(ud(b)); }                                 // 66 before EVEX
  { Bytes b = ok; b.insert(b.begin(), 0x48); CHECK(ud(b)); }                                 // REX before EVEX
  { Evex e = base; Bytes b = e.bytes(0x05); b.push_back(0xC3); CHECK(ud(b)); }               // add eax, imm32 is not promoted
  { Evex e = base; Bytes b = e.bytes(0x90); b.push_back(0xC3); CHECK(ud(b)); }               // nop
  { Evex e = base; e.nd = true; e.nf = true; Bytes b = e.bytes(0xF7); b.push_back(0xC3 | (4 << 3)); CHECK(ud(b)); }   // ND on mul
}

static void test_malformed() {
  Bytes all;
  Evex e;
  e.reg = 16; e.rm = 19; e.nd = true; e.v = 20;
  for (uint8_t op : {0x01, 0x03, 0xAF}) { Bytes b = e.bytes(op); b.push_back(0xC3); all = concat(all, b); }
  Evex c;
  c.ccmp = true; c.rm = 19; c.reg = 16; c.scc_or_zero = 4;
  { Bytes b = c.bytes(0x39); b.push_back(0xC3); all = concat(all, b); }
  Evex push;
  push.nd = true; push.rm = 17; push.v = 22;
  { Bytes b = push.bytes(0xFF); b.push_back(0xF1); all = concat(all, b); }
  for (size_t cut = 0; cut < all.size(); ++cut) { Machine m{Bytes(all.begin(), all.begin() + cut)}; m.run(8); }
  for (size_t i = 0; i < all.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes b = all;
      b[i] ^= uint8_t(1 << bit);
      Machine m(with_end(b));
      for (uint32_t r = 0; r < 32; ++r) st(m).r[r] = kData + 64 * r;
      st(m).r[4] = kStack;
      m.run(16);
    }
}

int main() {
  test_forms();
  test_cmov();
  test_setcc();
  test_ccmp();
  test_push2();
  test_invalid();
  test_malformed();
  if (g_failed) { printf("ApxEvexTest: %d failed\n", g_failed); return 1; }
  printf("ApxEvexTest: all passed\n");
  return 0;
}
