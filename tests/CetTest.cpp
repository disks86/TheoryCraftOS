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

// Host test of the CET shadow stack (call / ret checks, rdssp, incssp, wrss, wruss) and of enqcmd / enqcmds, in their legacy and APX map 4 encodings, with the instructions' failure modes.
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

constexpr uint64_t kCode = 0x10000, kData = 0x20000, kStack = 0x22000, kShadow = 0x23000;
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  uint64_t end = 0;
  explicit Machine(Bytes code) {
    code.push_back(0x0F); code.push_back(0x0B);
    space.map(kCode, 0x2000, process::kProtRead | process::kProtWrite | process::kProtExecute);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x4000, process::kProtRead | process::kProtWrite);
    end = kCode + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = kCode; start.stack_pointer = kStack;
    cpu.reset(start);
    cpu.state().r[6] = kData;      // rsi
    cpu.state().r[7] = kData + 0x100;      // rdi
  }
  StopReason run(uint64_t n = 64) { GuestMemory m(space); return cpu.run(m, n); }
  bool finished() { return run() == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  bool faulted(uint8_t vector) { return run() == StopReason::Fault && cpu.fault().vector == vector && cpu.fault().pc != end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  uint64_t peek(uint64_t addr, int n = 8) { uint64_t v = 0; GuestMemory m(space); m.read(addr, ByteView(reinterpret_cast<uint8_t*>(&v), n)); return v; }
};
static X86_64Cpu::State& st(Machine& m) { return m.cpu.state(); }

// jmp over a function; the function; the call to it: the function body is `body` then ret
static Bytes call_program(const Bytes& body) {
  Bytes code = {0xEB, uint8_t(body.size() + 1)};
  code.insert(code.end(), body.begin(), body.end());
  code.push_back(0xC3);
  const int32_t rel = -int32_t(body.size() + 1 + 5);
  code.insert(code.end(), {0xE8, uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)});
  return code;
}

static const Bytes kRdsspq = {0xF3, 0x48, 0x0F, 0x1E, 0xC8}, kRdsspd = {0xF3, 0x0F, 0x1E, 0xC8}, kIncsspq = {0xF3, 0x48, 0x0F, 0xAE, 0xE8}, kIncsspd = {0xF3, 0x0F, 0xAE, 0xE8};
static const Bytes kWrssq = {0x48, 0x0F, 0x38, 0xF6, 0x06}, kWrssd = {0x0F, 0x38, 0xF6, 0x06}, kWruss = {0x66, 0x0F, 0x38, 0xF5, 0x06};
static const Bytes kEnqcmd = {0xF2, 0x0F, 0x38, 0xF8, 0x3E}, kEnqcmds = {0xF3, 0x0F, 0x38, 0xF8, 0x3E};      // [rsi] -> the address in rdi
static const Bytes kWrssqApx = {0x62, 0xF4, 0xFC, 0x08, 0x66, 0x06}, kWrussApx = {0x62, 0xF4, 0x7D, 0x08, 0x65, 0x06}, kEnqcmdApx = {0x62, 0xF4, 0x7F, 0x08, 0xF8, 0x3E}, kEnqcmdsApx = {0x62, 0xF4, 0x7E, 0x08, 0xF8, 0x3E};

static void test_disabled() {
  Machine call(call_program({}));
  CHECK(call.finished());
  Machine r(kRdsspq);
  st(r).r[0] = 0x1234;
  CHECK(r.finished() && st(r).r[0] == 0x1234);      // a nop
  Machine i(kIncsspq);
  CHECK(i.finished());
  for (const Bytes& b : {kWrssq, kWrssd, kWruss, kWrssqApx, kWrussApx}) { Machine m(b); CHECK(m.faulted(6)); CHECK(m.cpu.fault().pc == kCode); }
}

static void test_shadow() {
  {      // a call and its return agree; the shadow stack pointer comes back
    Machine m(call_program({}));
    m.cpu.enable_shadow_stack(kShadow, false);
    CHECK(m.finished() && m.cpu.shadow_stack_pointer() == kShadow);
  }
  {      // inside the function the shadow stack holds the return address; rdsspq reads the pointer
    Bytes body = kRdsspq;
    Machine m(call_program(body));
    m.cpu.enable_shadow_stack(kShadow, false);
    CHECK(m.finished());
    CHECK(st(m).r[0] == kShadow - 8);
    CHECK(m.peek(kShadow - 8) == kCode + 2 + body.size() + 1 + 5);
  }
  {      // a smashed return address is a control protection fault (#CP, 21) and the data stack is left as it was
    Machine m(call_program({0x48, 0x83, 0x04, 0x24, 0x01}));      // add qword [rsp], 1
    m.cpu.enable_shadow_stack(kShadow, false);
    CHECK(m.faulted(21));
    CHECK(st(m).r[4] == kStack - 8);
    Machine n(call_program({0x48, 0x83, 0x04, 0x24, 0x01}));      // (without shadow stacks the program just goes wrong later)
    CHECK(n.run() == StopReason::Fault && n.cpu.fault().vector != 21);
  }
  {      // nested calls, call through a register, ret imm16
    Bytes code = {0xEB, 0x0E,
                  0xC3,                                   // 2: inner: ret
                  0xE8, 0xFA, 0xFF, 0xFF, 0xFF,           // 3: middle: call inner (to 2)
                  0xC2, 0x00, 0x00,                       // 8: ret 0
                  0x90, 0x90, 0x90, 0x90, 0x90,           // 11..15
                  0xE8, 0xEE, 0xFF, 0xFF, 0xFF,           // 16: call middle (to 3)
                  0x48, 0x8D, 0x05, 0xE7, 0xFF, 0xFF, 0xFF,   // 21: lea rax, [rip - 25]  -> 3 (middle)
                  0xFF, 0xD0};                            // call rax
    Machine m(code);
    m.cpu.enable_shadow_stack(kShadow, false);
    CHECK(m.finished());
    CHECK(m.cpu.shadow_stack_pointer() == kShadow);
  }
  {      // incsspq skips entries: 2 of 8 bytes; incsspd: 4 bytes each
    Bytes q = kIncsspq;
    Machine m(q);
    m.cpu.enable_shadow_stack(kShadow - 64, false);
    st(m).r[0] = 0x102;      // (only the low byte counts)
    CHECK(m.finished() && m.cpu.shadow_stack_pointer() == kShadow - 64 + 16);
    Machine d(kIncsspd);
    d.cpu.enable_shadow_stack(kShadow - 64, false);
    st(d).r[0] = 3;
    CHECK(d.finished() && d.cpu.shadow_stack_pointer() == kShadow - 64 + 12);
    Machine bad(kIncsspq);
    bad.cpu.enable_shadow_stack(0xDEAD0000, false);
    st(bad).r[0] = 1;
    CHECK(bad.faulted(14) && bad.cpu.shadow_stack_pointer() == 0xDEAD0000);
    Machine rd(kRdsspd);
    rd.cpu.enable_shadow_stack(kShadow, false);
    st(rd).r[0] = ~0ull;
    CHECK(rd.finished() && st(rd).r[0] == kShadow);      // (a 32-bit write clears the upper half)
  }
  {      // a shadow stack that cannot be written faults the call and leaves the data stack alone
    Machine m(call_program({}));
    m.cpu.enable_shadow_stack(0xDEAD0008, false);
    CHECK(m.faulted(14));
    CHECK(st(m).r[4] == kStack);
  }
}

static void test_wrss() {
  for (int apx = 0; apx < 2; ++apx) {
    {      // wrss stores the register at an aligned address when it is allowed
      Machine m(apx ? kWrssqApx : kWrssq);
      m.cpu.enable_shadow_stack(kShadow, true);
      st(m).r[0] = 0x1122334455667788ull;
      CHECK(m.finished());
      CHECK(m.peek(kData) == 0x1122334455667788ull);
    }
    {      // not allowed: #UD
      Machine m(apx ? kWrssqApx : kWrssq);
      m.cpu.enable_shadow_stack(kShadow, false);
      CHECK(m.faulted(6) && m.cpu.fault().pc == kCode);
    }
    {      // misaligned: #GP(0)
      Machine m(apx ? kWrssqApx : kWrssq);
      m.cpu.enable_shadow_stack(kShadow, true);
      st(m).r[6] = kData + 4;
      CHECK(m.faulted(13));
    }
    {      // wruss is supervisor-only: #GP at CPL 3
      Machine m(apx ? kWrussApx : kWruss);
      m.cpu.enable_shadow_stack(kShadow, true);
      CHECK(m.faulted(13));
    }
  }
  Machine d(kWrssd);
  d.cpu.enable_shadow_stack(kShadow, true);
  st(d).r[0] = 0xAABBCCDD11223344ull;
  CHECK(d.finished() && d.peek(kData, 8) == 0x11223344ull);
  Machine reg({0x48, 0x0F, 0x38, 0xF6, 0xC6});      // a register operand
  reg.cpu.enable_shadow_stack(kShadow, true);
  CHECK(reg.faulted(6));
}

static void test_enqcmd() {
  uint8_t command[64];
  for (int i = 0; i < 64; ++i) command[i] = uint8_t(0x40 + i);
  for (int apx = 0; apx < 2; ++apx) {
    {      // the command goes to the destination with the PASID in its first dword; ZF is clear
      Machine m(apx ? kEnqcmdApx : kEnqcmd);
      m.poke(kData, command, 64);
      m.cpu.set_pasid(0x12345);
      st(m).rflags |= 0x8D5;
      CHECK(m.finished());
      CHECK(m.peek(kData + 0x100, 4) == 0x12345);
      uint8_t out[64];
      GuestMemory gm(m.space);
      CHECK(gm.read(kData + 0x100, ByteView(out, 64)) && memcmp(out + 4, command + 4, 60) == 0);
      CHECK((st(m).rflags & 0x8D5) == 0);
    }
    {      // without a PASID it is #GP(0); enqcmds is #GP at CPL 3; a misaligned destination is #GP
      Machine m(apx ? kEnqcmdApx : kEnqcmd);
      CHECK(m.faulted(13));
      Machine s(apx ? kEnqcmdsApx : kEnqcmds);
      s.cpu.set_pasid(1);
      CHECK(s.faulted(13));
      Machine u(apx ? kEnqcmdApx : kEnqcmd);
      u.cpu.set_pasid(1);
      st(u).r[7] = kData + 0x108;
      CHECK(u.faulted(13));
      Machine p(apx ? kEnqcmdApx : kEnqcmd);
      p.cpu.set_pasid(1);
      st(p).r[6] = 0xDEAD0000;
      CHECK(p.faulted(14));
    }
  }
  Machine reg({0xF2, 0x0F, 0x38, 0xF8, 0xFE});      // a register source
  reg.cpu.set_pasid(1);
  CHECK(reg.faulted(6));
  Machine id({0xB8, 7, 0, 0, 0, 0x31, 0xC9, 0x0F, 0xA2});
  CHECK(id.finished() && ((st(id).r[1] >> 29) & 1));      // cpuid.7.0:ECX bit 29
}

static const Bytes kRstorssp = {0xF3, 0x0F, 0x01, 0x2E}, kSaveprevssp = {0xF3, 0x0F, 0x01, 0xEA}, kSetssbsy = {0xF3, 0x0F, 0x01, 0xE8}, kClrssbsy = {0xF3, 0x0F, 0xAE, 0x36};

// jmp over a far-returning function; the function body; the far call through [rsi] (m16:64 holding the function and the code selector)
static Bytes far_program(const Bytes& body) {
  Bytes code = {0xEB, uint8_t(body.size() + 2)};
  code.insert(code.end(), body.begin(), body.end());
  code.insert(code.end(), {0x48, 0xCB});      // retfq
  code.insert(code.end(), {0x48, 0xFF, 0x1E});      // call far [rsi]
  return code;
}

static void test_far_and_tokens() {
  for (int smash = 0; smash < 2; ++smash) {
    const Bytes body = smash ? Bytes{0x48, 0x83, 0x04, 0x24, 0x01} : Bytes{};      // add qword [rsp], 1
    Machine m(far_program(body));
    m.cpu.enable_shadow_stack(kShadow, false);
    const uint64_t target = kCode + 2;
    m.poke(kData, &target, 8);
    const uint16_t selector = 0x33;
    m.poke(kData + 8, &selector, 2);
    if (smash) {
      CHECK(m.faulted(21));
      CHECK(m.cpu.shadow_stack_pointer() == kShadow - 16);
    } else {
      CHECK(m.finished() && m.cpu.shadow_stack_pointer() == kShadow);
    }
  }
  {      // the far call pushes the selector and then the return address
    Machine m(far_program({}));
    m.cpu.enable_shadow_stack(kShadow, false);
    const uint64_t target = kCode + 2;
    m.poke(kData, &target, 8);
    const uint16_t selector = 0x33;
    m.poke(kData + 8, &selector, 2);
    Bytes stop = far_program({0x0F, 0x0B});      // (the body is an ud2: stop inside the function)
    Machine inside(stop);
    inside.cpu.enable_shadow_stack(kShadow, false);
    inside.poke(kData, &target, 8);
    inside.poke(kData + 8, &selector, 2);
    CHECK(inside.faulted(6));
    CHECK(inside.peek(kShadow - 8) == 0x33 && inside.peek(kShadow - 16) == kCode + stop.size());
  }
  {      // rstorssp switches stacks and leaves a previous-SSP token; saveprevssp turns it into a restore token for the stack that was left; rstorssp switches back
    const uint64_t other = kShadow + 0x200;
    Machine a(kRstorssp);
    a.cpu.enable_shadow_stack(kShadow, false);
    const uint64_t token = (other + 8) | 1;
    a.poke(other, &token, 8);
    st(a).r[6] = other;
    st(a).rflags |= 0x8D5;
    CHECK(a.finished());
    CHECK(a.cpu.shadow_stack_pointer() == other);
    CHECK(a.peek(other) == (kShadow | 3));
    CHECK((st(a).rflags & 0x8D5) == 0);
    Bytes both = kRstorssp;
    both.insert(both.end(), kSaveprevssp.begin(), kSaveprevssp.end());
    Machine b(both);
    b.cpu.enable_shadow_stack(kShadow, false);
    b.poke(other, &token, 8);
    st(b).r[6] = other;
    CHECK(b.finished());
    CHECK(b.cpu.shadow_stack_pointer() == other + 8);
    CHECK(b.peek(kShadow - 8) == (kShadow | 1));      // the restore token for the old stack
    // and back again
    Bytes back = both;
    back.insert(back.end(), kRstorssp.begin(), kRstorssp.end());
    Machine c(back);
    c.cpu.enable_shadow_stack(kShadow, false);
    c.poke(other, &token, 8);
    st(c).r[6] = other;
    // (the third instruction takes rsi from the register: point it at the new token)
    c.run(2);
    st(c).r[6] = kShadow - 8;
    CHECK(c.finished());
    CHECK(c.cpu.shadow_stack_pointer() == kShadow - 8);
  }
  {      // a token that is not a restore token for this address is #CP(RSTORSSP) and is written back unchanged; misaligned is #GP; disabled is #UD
    Machine m(kRstorssp);
    m.cpu.enable_shadow_stack(kShadow, false);
    const uint64_t bad = 0x1234567;
    m.poke(kData, &bad, 8);
    CHECK(m.faulted(21) && m.peek(kData) == bad && m.cpu.shadow_stack_pointer() == kShadow);
    Machine g(kRstorssp);
    g.cpu.enable_shadow_stack(kShadow, false);
    st(g).r[6] = kData + 4;
    CHECK(g.faulted(13));
    Machine d(kRstorssp);
    CHECK(d.faulted(6));
    Machine e(kSaveprevssp);
    CHECK(e.faulted(6));
    Machine f(kSaveprevssp);
    f.cpu.enable_shadow_stack(kShadow, false);
    CHECK(f.faulted(13));      // (an empty stack: no token with bit 1)
  }
  {      // supervisor-only instructions: #UD, as without supervisor shadow stacks
    Machine a(kSetssbsy);
    a.cpu.enable_shadow_stack(kShadow, false);
    CHECK(a.faulted(6) && a.cpu.fault().pc == kCode);
    Machine b(kClrssbsy);
    b.cpu.enable_shadow_stack(kShadow, false);
    CHECK(b.faulted(6) && b.cpu.fault().pc == kCode);
  }
}

static void test_user_msr() {
  auto imm = [](uint32_t v) { return Bytes{uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; };
  auto cat = [](Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; };
  {      // the register forms: urdmsr rax <- MSR[rcx], uwrmsr MSR[rcx] <- rax
    Machine r({0xF2, 0x48, 0x0F, 0x38, 0xF8, 0xC8});
    r.cpu.set_user_msr(0x1234, 0xAABBCCDD11223344ull, false);
    st(r).r[1] = 0x1234;
    CHECK(r.finished() && st(r).r[0] == 0xAABBCCDD11223344ull);
    Machine w({0xF3, 0x48, 0x0F, 0x38, 0xF8, 0xC8});
    w.cpu.set_user_msr(0x1234, 0, true);
    st(w).r[1] = 0x1234;
    st(w).r[0] = 0x5566;
    CHECK(w.finished());
    uint64_t value = 0;
    CHECK(w.cpu.user_msr(0x1234, &value) && value == 0x5566);
    Machine ro({0xF3, 0x48, 0x0F, 0x38, 0xF8, 0xC8});
    ro.cpu.set_user_msr(0x1234, 7, false);
    st(ro).r[1] = 0x1234;
    CHECK(ro.faulted(13));
    Machine none({0xF2, 0x48, 0x0F, 0x38, 0xF8, 0xC8});
    st(none).r[1] = 0x1234;
    CHECK(none.faulted(13));
    Machine big({0xF2, 0x48, 0x0F, 0x38, 0xF8, 0xC8});
    big.cpu.set_user_msr(0x34, 1, false);
    st(big).r[1] = 0x100000034ull;      // (bits above 31 are not ignored)
    CHECK(big.faulted(13));
    Machine w0({0xF2, 0x0F, 0x38, 0xF8, 0xC8});      // without REX.W
    w0.cpu.set_user_msr(0, 0, false);
    CHECK(w0.faulted(6));
  }
  {      // the immediate forms in VEX map 7, and the EVEX ones with r20
    Machine r(cat({0xC4, 0xE7, 0x7B, 0xF8, 0xC0}, imm(0x4000)));
    r.cpu.set_user_msr(0x4000, 0x99, false);
    CHECK(r.finished() && st(r).r[0] == 0x99);
    Machine w(cat({0xC4, 0xE7, 0x7A, 0xF8, 0xC0}, imm(0x4000)));
    w.cpu.set_user_msr(0x4000, 0, true);
    st(w).r[0] = 0x77;
    uint64_t value = 0;
    CHECK(w.finished() && w.cpu.user_msr(0x4000, &value) && value == 0x77);
    Machine er(cat({0x62, 0xFF, 0x7F, 0x08, 0xF8, 0xC4}, imm(0x4000)));
    er.cpu.set_user_msr(0x4000, 0x1234, false);
    CHECK(er.finished() && st(er).r[20] == 0x1234);
    Machine ew(cat({0x62, 0xFF, 0x7E, 0x08, 0xF8, 0xC4}, imm(0x4000)));
    ew.cpu.set_user_msr(0x4000, 0, true);
    st(ew).r[20] = 0x4321;
    CHECK(ew.finished() && ew.cpu.user_msr(0x4000, &value) && value == 0x4321);
    Machine bad(cat({0xC4, 0xE7, 0x7B, 0xF8, 0xC8}, imm(1)));      // /1 is reserved
    bad.cpu.set_user_msr(1, 0, false);
    CHECK(bad.faulted(6));
    Machine un(cat({0xC4, 0xE7, 0x7B, 0xF8, 0xC0}, imm(0x4001)));
    CHECK(un.faulted(13));
    Machine apx(cat({0x62, 0xF4, 0x7F, 0x08, 0xF8, 0xC0}, Bytes{}));      // (the register form with REX.W clear in map 4 needs W: #UD)
    CHECK(apx.faulted(6));
    Machine m4({0x62, 0xF4, 0xFF, 0x08, 0xF8, 0xC8});      // the map 4 form (F2, W1): urdmsr rax, rcx
    m4.cpu.set_user_msr(0x55, 0xABCD, false);
    st(m4).r[1] = 0x55;
    CHECK(m4.finished() && st(m4).r[0] == 0xABCD);
  }
  {      // the table fills up; the cpuid bit is set
    Machine m({0x90});
    for (uint32_t i = 0; i < 16; ++i) CHECK(m.cpu.set_user_msr(i, i, false));
    CHECK(!m.cpu.set_user_msr(99, 0, false));
    CHECK(m.cpu.set_user_msr(3, 33, true));      // (an existing entry is updated)
    Machine id({0xB8, 7, 0, 0, 0, 0xB9, 1, 0, 0, 0, 0x0F, 0xA2});
    CHECK(id.finished() && ((st(id).r[2] >> 15) & 1));
  }
}

// Every prefix and every single-bit flip of the encodings: whatever happens, it must happen safely.
static void test_malformed() {
  Bytes all;
  for (const Bytes& b : {kWrssq, kWruss, kEnqcmd, kEnqcmds, kWrssqApx, kEnqcmdApx, kRdsspq, kIncsspq, kRstorssp, kSaveprevssp, kSetssbsy, kClrssbsy}) all.insert(all.end(), b.begin(), b.end());
  for (size_t cut = 0; cut < all.size(); ++cut) {
    Machine m{Bytes(all.begin(), all.begin() + cut)};
    m.cpu.enable_shadow_stack(kShadow, true);
    m.cpu.set_pasid(3);
    m.run(16);
  }
  for (size_t i = 0; i < all.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes b = all;
      b[i] ^= uint8_t(1 << bit);
      Machine m(b);
      m.cpu.enable_shadow_stack(kShadow, true);
      m.cpu.set_pasid(3);
      for (uint32_t r = 0; r < 32; ++r) st(m).r[r] = kData + 64 * (r % 8);
      st(m).r[4] = kStack;
      m.run(32);
    }
}

int main() {
  test_disabled();
  test_shadow();
  test_wrss();
  test_enqcmd();
  test_far_and_tokens();
  test_user_msr();
  test_malformed();
  if (g_failed) { printf("CetTest: %d failed\n", g_failed); return 1; }
  printf("CetTest: all passed\n");
  return 0;
}
