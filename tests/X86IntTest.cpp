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

// Host test of the x86-64 integer instructions that need no vector state: rcl / rcr, cmpxchg8b / cmpxchg16b, adcx / adox, sahf / lahf and movnti. The host runs the same bytes from the
// same random registers, flags and memory (a generated stub loads the state, executes the instruction and stores everything back), and registers, the defined flags and memory must match.
// Then the instructions with no host oracle (rdrand, rdseed, rdpid, rdtscp, xtest, xbegin, movdiri, movdir64b, rdfsbase...) are checked against what the architecture says.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "Emulation/X86_64Cpu.hpp"
#include "Process/AddressSpace.hpp"
#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#define HOST_X86 1
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

static uint64_t seed = 0xD1B54A32D192ED03ull;
static uint64_t rnd() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

constexpr uint64_t kBase = 0x20800;      // rdi
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(0x10000, 0x1000, process::kProtRead | process::kProtExecute);
    space.write_privileged(0x10000, ConstByteView(code.data(), code.size()));
    space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(0x2F000, 0x2000, process::kProtRead | process::kProtWrite);
    end = 0x10000 + code.size() - 2;
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = 0x10000; start.stack_pointer = 0x30000;
    cpu.reset(start);
    cpu.state().r[7] = kBase;
  }
  uint64_t end = 0;
  StopReason run(uint64_t n = 4) { GuestMemory m(space); return cpu.run(m, n); }
  // True when the program ran to its final ud2.
  bool finished(uint64_t n = 8) { return run(n) == StopReason::Fault && cpu.fault().vector == 6 && cpu.fault().pc == end; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  void peek(uint64_t addr, void* p, size_t n) { GuestMemory gm(space); gm.read(addr, ByteView(static_cast<uint8_t*>(p), n)); }
};


// (a ud2 ends the program: unlike syscall it leaves rcx and r11 alone)
static Bytes with_syscall(Bytes b) { b.push_back(0x0F); b.push_back(0x0B); return b; }

struct Snap {
  uint64_t reg[16];
  uint64_t flags;
  uint64_t mm[8];        // MM0..MM7
  uint64_t xmm[4];       // XMM0 and XMM1 (low, high)
  uint32_t mxcsr;
  uint32_t pad;
  alignas(64) uint8_t mem[64];
};
constexpr uint64_t kFlagBits = 0x8D5;      // CF PF AF ZF SF OF
constexpr uint64_t kEmuMem = 0x20800;

#ifdef HOST_X86
static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
// mov reg, [base + disp32] (load) or mov [base + disp32], reg (store); base is rdi (7) for loads and rax (0) for stores
static void mov_mem(Bytes& b, bool load, int reg, int base, uint32_t disp) {
  b.push_back(uint8_t(0x48 | ((reg >> 3) << 2)));
  b.push_back(load ? 0x8B : 0x89);
  b.push_back(uint8_t(0x80 | ((reg & 7) << 3) | base));
  put32(b, disp);
}
static const int kCompared[] = {0, 1, 2, 3, 5, 6, 9, 10, 11};      // rax rcx rdx rbx rbp rsi r9 r10 r11

static Bytes build_stub(const Bytes& insn) {
  Bytes b = {0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x57};      // push rbx rbp r12 r13 r14 r15 rdi
  for (int r : kCompared) mov_mem(b, true, r, 7, uint32_t(offsetof(Snap, reg) + 8 * r));
  b.insert(b.end(), {0x0F, 0xAE, 0x97}); put32(b, uint32_t(offsetof(Snap, mxcsr)));                      // ldmxcsr [rdi + mxcsr]
  b.insert(b.end(), {0x0F, 0x10, 0x87}); put32(b, uint32_t(offsetof(Snap, xmm)));                        // movups xmm0, [rdi + xmm]
  b.insert(b.end(), {0x0F, 0x10, 0x8F}); put32(b, uint32_t(offsetof(Snap, xmm) + 16));                   // movups xmm1, [rdi + xmm + 16]
  for (int i = 0; i < 8; ++i) { b.insert(b.end(), {0x0F, 0x6F, uint8_t(0x80 | (i << 3) | 7)}); put32(b, uint32_t(offsetof(Snap, mm) + 8 * i)); }      // movq mmi, [rdi + mm + 8i]
  b.push_back(0x4C); b.push_back(0x8D); b.push_back(0x87); put32(b, uint32_t(offsetof(Snap, mem)));      // lea r8, [rdi + mem]
  b.push_back(0xFF); b.push_back(0xB7); put32(b, uint32_t(offsetof(Snap, flags)));                       // push qword [rdi + flags]
  b.push_back(0x9D);                                                                                     // popfq
  b.insert(b.end(), insn.begin(), insn.end());
  b.push_back(0x9C); b.push_back(0x50);                                                                  // pushfq; push rax
  b.insert(b.end(), {0x48, 0x87, 0x44, 0x24, 0x10});                                                     // xchg rax, [rsp + 16]  (rax = Snap*)
  for (int r : kCompared) if (r != 0) mov_mem(b, false, r, 0, uint32_t(offsetof(Snap, reg) + 8 * r));
  b.insert(b.end(), {0x0F, 0xAE, 0x98}); put32(b, uint32_t(offsetof(Snap, mxcsr)));                      // stmxcsr [rax + mxcsr]
  b.insert(b.end(), {0x0F, 0x11, 0x80}); put32(b, uint32_t(offsetof(Snap, xmm)));                        // movups [rax + xmm], xmm0
  b.insert(b.end(), {0x0F, 0x11, 0x88}); put32(b, uint32_t(offsetof(Snap, xmm) + 16));
  for (int i = 0; i < 8; ++i) { b.insert(b.end(), {0x0F, 0x7F, uint8_t(0x80 | (i << 3))}); put32(b, uint32_t(offsetof(Snap, mm) + 8 * i)); }      // movq [rax + mm + 8i], mmi
  b.insert(b.end(), {0x0F, 0x77});                                                                       // emms
  b.insert(b.end(), {0x48, 0x8B, 0x4C, 0x24, 0x10});                                                     // mov rcx, [rsp + 16]   (the old rax)
  b.insert(b.end(), {0x48, 0x89, 0x88}); put32(b, uint32_t(offsetof(Snap, reg)));                        // mov [rax + reg[0]], rcx
  b.insert(b.end(), {0x48, 0x8B, 0x4C, 0x24, 0x08});                                                     // mov rcx, [rsp + 8]    (the flags)
  b.insert(b.end(), {0x48, 0x89, 0x88}); put32(b, uint32_t(offsetof(Snap, flags)));
  b.insert(b.end(), {0x48, 0x83, 0xC4, 0x18});                                                           // add rsp, 24  (drops the rax copy, the flags and the saved rdi)
  b.insert(b.end(), {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3});               // pop r15 r14 r13 r12 rbp rbx; ret  (the rdi slot went with the add)
  return b;
}

static void run_host(const Bytes& insn, Snap* snap) {
  const Bytes stub = build_stub(insn);
  void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memcpy(page, stub.data(), stub.size());
  reinterpret_cast<void (*)(Snap*)>(page)(snap);
  munmap(page, 4096);
}
#endif

static void run_emulator(const Bytes& insn, Snap* snap) {
  Machine m(with_syscall(insn));
  X86_64Cpu::State& st = m.cpu.state();
  for (int r : {0, 1, 2, 3, 5, 6, 9, 10, 11}) st.r[r] = snap->reg[r];
  st.r[8] = kEmuMem;
  st.rflags = (snap->flags & kFlagBits) | 0x202;
  m.poke(kEmuMem, snap->mem, 64);
  for (int i = 0; i < 8; ++i) { st.st[i].mant = snap->mm[i]; st.st[i].se = 0xFFFF; }
  st.fpu_valid = 0xFF;
  st.fpu_status = 0;
  st.xmm[0] = {snap->xmm[0], snap->xmm[1]};
  st.xmm[1] = {snap->xmm[2], snap->xmm[3]};
  st.mxcsr = snap->mxcsr;
  if (!m.finished()) { printf("emulator fault vector %d\n", m.cpu.fault().vector); snap->flags = ~uint64_t{0}; return; }
  for (int r : {0, 1, 2, 3, 5, 6, 9, 10, 11}) snap->reg[r] = st.r[r];
  snap->flags = st.rflags & kFlagBits;
  m.peek(kEmuMem, snap->mem, 64);
  for (int i = 0; i < 8; ++i) snap->mm[i] = st.st[i].mant;
  snap->xmm[0] = st.xmm[0].lo; snap->xmm[1] = st.xmm[0].hi; snap->xmm[2] = st.xmm[1].lo; snap->xmm[3] = st.xmm[1].hi;
  snap->mxcsr = st.mxcsr;
  // (an MMX instruction leaves TOP at zero and every x87 register valid, which the host's emms below hides: so check it here)
  if (st.fpu_valid != 0xFF && st.fpu_valid != 0) snap->flags = ~uint64_t{0};
}

static uint32_t rnd_float_bits() {
  switch (rnd() % 6) {
    case 0: { const float f = float(int(rnd() % 2001) - 1000) / 8; uint32_t u; memcpy(&u, &f, 4); return u; }
    case 1: { const float f = float(int64_t(rnd()) >> (rnd() % 40)); uint32_t u; memcpy(&u, &f, 4); return u; }
    case 2: return 0x7F800000u | uint32_t(rnd() & 0x80800000u);      // infinities
    case 3: return 0x7FC00000u;                                       // NaN
    default: return uint32_t(rnd());
  }
}

static uint64_t rnd_value() {
  switch (rnd() % 8) {
    case 0: return 0;
    case 1: return ~uint64_t{0};
    case 2: return rnd() % 70;
    case 3: return uint64_t{1} << (rnd() % 64);
    case 4: return ~(uint64_t{1} << (rnd() % 64));
    default: return rnd();
  }
}

struct Case {
  std::string name;
  Bytes code;
  std::function<uint64_t(const Snap&)> flag_mask = [](const Snap&) { return kFlagBits; };
  std::function<void(Snap&)> prepare;
};
static std::vector<Case> g_cases;

#ifdef HOST_X86
static void run_cases(int iterations) {
  int total = 0;
  for (const Case& c : g_cases) {
    int bad = 0;
    for (int i = 0; i < iterations; ++i) {
      Snap init;
      memset(&init, 0, sizeof(init));
      for (int r : {0, 1, 2, 3, 5, 6, 9, 10, 11}) init.reg[r] = rnd_value();
      init.flags = (rnd() & kFlagBits) | 0x202;
      for (int b = 0; b < 64; ++b) init.mem[b] = uint8_t(rnd());
      for (auto& v : init.mm) v = rnd_value();
      for (auto& v : init.xmm) v = rnd_value();
      if (rnd() % 3 == 0) for (auto& v : init.xmm) v = (uint64_t{rnd_float_bits()} << 32) | rnd_float_bits();
      init.mxcsr = 0x1F80 | uint32_t((rnd() & 3) << 13);
      if (c.prepare) c.prepare(init);
      Snap host = init, emu = init;
      run_host(c.code, &host);
      emu.flags &= kFlagBits;
      run_emulator(c.code, &emu);
      host.flags &= kFlagBits;
      const uint64_t mask = c.flag_mask(init);
      bool ok = (host.flags & mask) == (emu.flags & mask) && memcmp(host.mem, emu.mem, 64) == 0 && memcmp(host.mm, emu.mm, 64) == 0 && memcmp(host.xmm, emu.xmm, 32) == 0 && ((host.mxcsr ^ emu.mxcsr) & 0x3D) == 0;
      for (int r : {0, 1, 2, 3, 5, 6, 9, 10, 11}) ok = ok && host.reg[r] == emu.reg[r];
      ++total;
      if (!ok) {
        if (++bad <= 2) {
          printf("  mm host %llx %llx %llx %llx emu %llx %llx %llx %llx; xmm0 host %llx:%llx emu %llx:%llx; mxcsr host %x emu %x\n", (unsigned long long)host.mm[0], (unsigned long long)host.mm[1], (unsigned long long)host.mm[2], (unsigned long long)host.mm[3],
                 (unsigned long long)emu.mm[0], (unsigned long long)emu.mm[1], (unsigned long long)emu.mm[2], (unsigned long long)emu.mm[3], (unsigned long long)host.xmm[1], (unsigned long long)host.xmm[0], (unsigned long long)emu.xmm[1], (unsigned long long)emu.xmm[0], host.mxcsr, emu.mxcsr);
          printf("MISMATCH %s\n  in: rax %llx rcx %llx rdx %llx rbx %llx flags %llx\n  host: rax %llx rcx %llx rdx %llx rbx %llx flags %llx mem0 %llx\n  emu:  rax %llx rcx %llx rdx %llx rbx %llx flags %llx mem0 %llx\n",
                 c.name.c_str(), (unsigned long long)init.reg[0], (unsigned long long)init.reg[1], (unsigned long long)init.reg[2], (unsigned long long)init.reg[3], (unsigned long long)init.flags,
                 (unsigned long long)host.reg[0], (unsigned long long)host.reg[1], (unsigned long long)host.reg[2], (unsigned long long)host.reg[3], (unsigned long long)host.flags, *(unsigned long long*)host.mem,
                 (unsigned long long)emu.reg[0], (unsigned long long)emu.reg[1], (unsigned long long)emu.reg[2], (unsigned long long)emu.reg[3], (unsigned long long)emu.flags, *(unsigned long long*)emu.mem);
        }
        ++g_failed;
      }
    }
  }
  printf("compared %d runs of %zu instructions with the host\n", total, g_cases.size());
}
#endif

static void build_cases() {
  // rcl / rcr: every size, by one, by an immediate, by CL. Operand: bl / bx / ebx / rbx.
  for (int rotate_right = 0; rotate_right < 2; ++rotate_right) {
    for (int size : {1, 2, 4, 8}) {
      const uint8_t reg_field = uint8_t(0xC0 | ((2 + rotate_right) << 3) | 3);      // /2 rcl, /3 rcr; rm = rbx
      Bytes prefix;
      if (size == 2) prefix.push_back(0x66);
      if (size == 8) prefix.push_back(0x48);
      const char* mnemonic = rotate_right ? "rcr" : "rcl";
      for (int form = 0; form < 3; ++form) {      // 0: by 1 (D0/D1), 1: imm8 (C0/C1), 2: by CL (D2/D3)
        Case c;
        c.name = std::string(mnemonic) + " size " + std::to_string(size) + (form == 0 ? " by 1" : form == 1 ? " by imm" : " by cl");
        c.code = prefix;
        const uint8_t base = form == 0 ? 0xD0 : form == 1 ? 0xC0 : 0xD2;
        c.code.push_back(uint8_t(base | (size == 1 ? 0 : 1)));
        c.code.push_back(reg_field);
        uint8_t imm = 0;
        if (form == 1) { imm = uint8_t(rnd() % 70); c.code.push_back(imm); }
        c.flag_mask = [form, imm, size](const Snap& s) {
          const uint64_t count = (form == 0 ? 1 : form == 1 ? imm : (s.reg[1] & 0xFF)) & (size == 8 ? 63 : 31);
          return count == 1 || count == 0 ? kFlagBits : (kFlagBits & ~uint64_t{0x800});      // (OF is undefined for a count above one)
        };
        g_cases.push_back(c);
        if (form == 1) {      // more immediates
          for (uint8_t extra : {uint8_t(2), uint8_t(7), uint8_t(8), uint8_t(9), uint8_t(16), uint8_t(17), uint8_t(31), uint8_t(32), uint8_t(33), uint8_t(63), uint8_t(64), uint8_t(0)}) {
            Case e = c;
            e.code.back() = extra;
            e.name += " " + std::to_string(extra);
            e.flag_mask = [extra, size](const Snap&) {
              const uint64_t count = extra & (size == 8 ? 63 : 31);
              return count == 1 || count == 0 ? kFlagBits : (kFlagBits & ~uint64_t{0x800});
            };
            g_cases.push_back(e);
          }
        }
      }
    }
  }
  {
    Case c;
    c.name = "cmpxchg8b [r8]";
    c.code = {0x41, 0x0F, 0xC7, 0x08};
    c.prepare = [](Snap& s) { if (rnd() & 1) { memcpy(s.mem, &s.reg[0], 4); memcpy(s.mem + 4, &s.reg[2], 4); } };
    g_cases.push_back(c);
    Case d;
    d.name = "lock cmpxchg16b [r8]";
    d.code = {0xF0, 0x49, 0x0F, 0xC7, 0x08};
    d.prepare = [](Snap& s) { if (rnd() & 1) { memcpy(s.mem, &s.reg[0], 8); memcpy(s.mem + 8, &s.reg[2], 8); } };
    g_cases.push_back(d);
  }
  g_cases.push_back({"adcx ecx, ebx", {0x66, 0x0F, 0x38, 0xF6, 0xCB}});
  g_cases.push_back({"adcx rcx, rbx", {0x66, 0x48, 0x0F, 0x38, 0xF6, 0xCB}});
  g_cases.push_back({"adox ecx, ebx", {0xF3, 0x0F, 0x38, 0xF6, 0xCB}});
  g_cases.push_back({"adox rcx, rbx", {0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xCB}});
  g_cases.push_back({"adcx ecx, [r8]", {0x66, 0x41, 0x0F, 0x38, 0xF6, 0x08}});
  g_cases.push_back({"adox rcx, [r8+8]", {0xF3, 0x49, 0x0F, 0x38, 0xF6, 0x48, 0x08}});
  g_cases.push_back({"sahf", {0x9E}});

  // loop, loope, loopne (with and without the address-size prefix): a taken branch skips the mov eax, 1 that follows
  for (uint8_t op : {uint8_t(0xE0), uint8_t(0xE1), uint8_t(0xE2)}) {
    for (bool prefix : {false, true}) {
      Case c;
      c.name = std::string(op == 0xE2 ? "loop" : op == 0xE1 ? "loope" : "loopne") + (prefix ? " (ecx)" : " (rcx)");
      if (prefix) c.code.push_back(0x67);
      c.code.insert(c.code.end(), {op, 0x05, 0xB8, 0x01, 0x00, 0x00, 0x00});
      c.prepare = [](Snap& snap) { snap.reg[1] = (rnd() & 1) ? rnd() % 3 : (rnd() & 1) ? 0x100000000ull + rnd() % 3 : rnd(); };
      g_cases.push_back(c);
    }
  }

  g_cases.push_back({"lahf", {0x9F}});
  g_cases.push_back({"movnti [r8], eax", {0x41, 0x0F, 0xC3, 0x00}});
  g_cases.push_back({"movnti [r8+8], rax", {0x49, 0x0F, 0xC3, 0x40, 0x08}});

  // --- MMX ---
  // The generic two-operand integer opcodes: mm2 <- mm2 op mm3 (register) and mm2 <- mm2 op [r8] (memory).
  {
    static const uint8_t generic[] = {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x74, 0x75, 0x76, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
                                      0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE};
    for (uint8_t op : generic) {
      char name[64];
      snprintf(name, sizeof(name), "mmx %02X mm2, mm3", op);
      g_cases.push_back({name, {0x0F, op, 0xD3}});
      snprintf(name, sizeof(name), "mmx %02X mm2, [r8]", op);
      g_cases.push_back({name, {0x41, 0x0F, op, 0x10}});
    }
  }
  for (uint8_t op : {uint8_t(0x00), uint8_t(0x01), uint8_t(0x02), uint8_t(0x03), uint8_t(0x04), uint8_t(0x05), uint8_t(0x06), uint8_t(0x07), uint8_t(0x08), uint8_t(0x09), uint8_t(0x0A), uint8_t(0x0B), uint8_t(0x1C), uint8_t(0x1D), uint8_t(0x1E)}) {
    char name[64];
    snprintf(name, sizeof(name), "ssse3 mm 0F38 %02X", op);
    g_cases.push_back({name, {0x0F, 0x38, op, 0xD3}});
    snprintf(name, sizeof(name), "ssse3 mm 0F38 %02X mem", op);
    g_cases.push_back({name, {0x41, 0x0F, 0x38, op, 0x10}});
  }
  for (uint8_t imm : {uint8_t(0), uint8_t(1), uint8_t(3), uint8_t(7), uint8_t(8), uint8_t(9), uint8_t(15), uint8_t(16), uint8_t(17), uint8_t(255)}) {
    char name[64];
    snprintf(name, sizeof(name), "palignr mm2, mm3, %u", imm);
    g_cases.push_back({name, {0x0F, 0x3A, 0x0F, 0xD3, imm}});
  }
  for (uint8_t imm : {uint8_t(0x1B), uint8_t(0x00), uint8_t(0xE4), uint8_t(0x55), uint8_t(0xFF), uint8_t(0xA7)}) {
    char name[64];
    snprintf(name, sizeof(name), "pshufw mm2, mm3, %02X", imm);
    g_cases.push_back({name, {0x0F, 0x70, 0xD3, imm}});
    snprintf(name, sizeof(name), "pshufw mm2, [r8], %02X", imm);
    g_cases.push_back({name, {0x41, 0x0F, 0x70, 0x10, imm}});
  }
  for (uint8_t count : {uint8_t(0), uint8_t(1), uint8_t(3), uint8_t(8), uint8_t(15), uint8_t(16), uint8_t(31), uint8_t(32), uint8_t(63), uint8_t(64), uint8_t(200)}) {
    for (int group = 0; group < 3; ++group) {      // 71 words, 72 dwords, 73 qword
      for (uint8_t kind : {uint8_t(2), uint8_t(4), uint8_t(6)}) {
        if (group == 2 && kind == 4) continue;
        char name[64];
        snprintf(name, sizeof(name), "mmx shift %02X /%u mm3, %u", 0x71 + group, kind, count);
        g_cases.push_back({name, {0x0F, uint8_t(0x71 + group), uint8_t(0xC0 | (kind << 3) | 3), count}});
      }
    }
  }
  for (uint8_t lanes : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(6)}) {
    char name[64];
    snprintf(name, sizeof(name), "pextrw eax, mm3, %u", lanes);
    g_cases.push_back({name, {0x0F, 0xC5, 0xC3, lanes}});
    snprintf(name, sizeof(name), "pinsrw mm2, eax, %u", lanes);
    g_cases.push_back({name, {0x0F, 0xC4, 0xD0, lanes}});
    snprintf(name, sizeof(name), "pinsrw mm2, [r8], %u", lanes);
    g_cases.push_back({name, {0x41, 0x0F, 0xC4, 0x10, lanes}});
  }
  g_cases.push_back({"pmovmskb eax, mm3", {0x0F, 0xD7, 0xC3}});
  g_cases.push_back({"movd mm2, eax", {0x0F, 0x6E, 0xD0}});
  g_cases.push_back({"movq mm2, rax", {0x48, 0x0F, 0x6E, 0xD0}});
  g_cases.push_back({"movd mm2, [r8]", {0x41, 0x0F, 0x6E, 0x10}});
  g_cases.push_back({"movd eax, mm3", {0x0F, 0x7E, 0xD8}});
  g_cases.push_back({"movq rax, mm3", {0x48, 0x0F, 0x7E, 0xD8}});
  g_cases.push_back({"movd [r8], mm3", {0x41, 0x0F, 0x7E, 0x18}});
  g_cases.push_back({"movq mm2, mm3", {0x0F, 0x6F, 0xD3}});
  g_cases.push_back({"movq mm2, [r8]", {0x41, 0x0F, 0x6F, 0x10}});
  g_cases.push_back({"movq [r8], mm3", {0x41, 0x0F, 0x7F, 0x18}});
  g_cases.push_back({"movq mm5, mm3 (7F)", {0x0F, 0x7F, 0xDD}});
  g_cases.push_back({"movntq [r8], mm3", {0x41, 0x0F, 0xE7, 0x18}});
  g_cases.push_back({"movq2dq xmm1, mm3", {0xF3, 0x0F, 0xD6, 0xCB}});
  g_cases.push_back({"movdq2q mm2, xmm1", {0xF2, 0x0F, 0xD6, 0xD1}});
  g_cases.push_back({"emms", {0x0F, 0x77}});
  g_cases.push_back({"cvtpi2ps xmm1, mm3", {0x0F, 0x2A, 0xCB}});
  g_cases.push_back({"cvtpi2ps xmm1, [r8]", {0x41, 0x0F, 0x2A, 0x08}});
  g_cases.push_back({"cvtpi2pd xmm1, mm3", {0x66, 0x0F, 0x2A, 0xCB}});
  g_cases.push_back({"cvtpi2pd xmm1, [r8]", {0x66, 0x41, 0x0F, 0x2A, 0x08}});
  g_cases.push_back({"cvtps2pi mm2, xmm1", {0x0F, 0x2D, 0xD1}});
  g_cases.push_back({"cvttps2pi mm2, xmm1", {0x0F, 0x2C, 0xD1}});
  g_cases.push_back({"cvtps2pi mm2, [r8]", {0x41, 0x0F, 0x2D, 0x10}});
  g_cases.push_back({"cvtpd2pi mm2, xmm1", {0x66, 0x0F, 0x2D, 0xD1}});
  g_cases.push_back({"cvttpd2pi mm2, xmm1", {0x66, 0x0F, 0x2C, 0xD1}});
  g_cases.push_back({"cvttpd2pi mm2, [r8]", {0x66, 0x41, 0x0F, 0x2C, 0x10}});

#ifdef HOST_X86
  if (__builtin_cpu_supports("sse4a")) {
    // SSE4a: extrq / insertq with a valid field (length + index <= 64; a length of 0 means 64 and then the index is 0), and the scalar non-temporal stores
    for (int n = 0; n < 40; ++n) {
      uint8_t length = uint8_t(rnd() % 64), index = uint8_t(rnd() % 64);
      if (length == 0) index = 0;
      else if (index + length > 64) index = uint8_t(64 - length);
      char name[80];
      snprintf(name, sizeof(name), "extrq xmm1, %u, %u", length, index);
      g_cases.push_back({name, {0x66, 0x0F, 0x78, 0xC1, length, index}, [](const Snap&) { return kFlagBits; }, nullptr});
      snprintf(name, sizeof(name), "insertq xmm0, xmm1, %u, %u", length, index);
      g_cases.push_back({name, {0xF2, 0x0F, 0x78, 0xC1, length, index}, [](const Snap&) { return kFlagBits; }, nullptr});
      Case e;
      snprintf(name, sizeof(name), "extrq xmm0, xmm1 (%u, %u)", length, index);
      e.name = name;
      e.code = {0x66, 0x0F, 0x79, 0xC1};
      e.prepare = [length, index](Snap& snap) { snap.xmm[2] = (snap.xmm[2] & ~uint64_t{0x3F3F}) | length | (uint64_t{index} << 8); };
      g_cases.push_back(e);
      Case f;
      snprintf(name, sizeof(name), "insertq xmm0, xmm1 (%u, %u)", length, index);
      f.name = name;
      f.code = {0xF2, 0x0F, 0x79, 0xC1};
      f.prepare = [length, index](Snap& snap) { snap.xmm[3] = (snap.xmm[3] & ~uint64_t{0x3F3F}) | length | (uint64_t{index} << 8); };
      g_cases.push_back(f);
    }
    g_cases.push_back({"movntss [r8], xmm0", {0xF3, 0x41, 0x0F, 0x2B, 0x00}, [](const Snap&) { return kFlagBits; }, nullptr});
    g_cases.push_back({"movntsd [r8], xmm0", {0xF2, 0x41, 0x0F, 0x2B, 0x00}, [](const Snap&) { return kFlagBits; }, nullptr});
  }
#endif
}

// --- instructions with no host oracle ------------------------------------------------------------------------------------------------------

static void test_architectural() {
  {      // rdrand / rdseed: CF = 1, the other flags clear, the value is written (and differs between calls)
    Machine m(with_syscall({0x48, 0x0F, 0xC7, 0xF0, 0x48, 0x0F, 0xC7, 0xF1, 0x0F, 0xC7, 0xFA, 0x66, 0x0F, 0xC7, 0xF3}));      // rdrand rax; rdrand rcx; rdseed edx; rdrand bx
    m.cpu.state().rflags = 0x8D7 | 0x200;
    m.cpu.state().r[2] = ~uint64_t{0};
    CHECK(m.finished());
    CHECK(m.cpu.state().r[0] != m.cpu.state().r[1] && m.cpu.state().r[0] != 0);
    CHECK(m.cpu.state().r[2] <= 0xFFFFFFFFu);      // (the 32-bit form zero-extends)
    CHECK((m.cpu.state().rflags & kFlagBits) == 1);
  }
  {      // rdpid, rdtscp
    Machine m(with_syscall({0xF3, 0x48, 0x0F, 0xC7, 0xF8, 0x0F, 0x01, 0xF9}));
    m.cpu.state().r[0] = 0x55;
    m.cpu.state().r[1] = 0x66;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[1] == 0);      // IA32_TSC_AUX
  }
  {      // transactional memory: xbegin falls through to the fallback address with EAX = 0, xtest says "not in a transaction", xabort is a no-op, xend outside one is #GP
    Machine m(with_syscall({0xC7, 0xF8, 0x03, 0x00, 0x00, 0x00, 0xC6, 0xF8, 0x07, 0x90, 0x90}));      // xbegin +3 -> the syscall (skips xabort 7 and nop... lands on the second nop)
    m.cpu.state().r[0] = 0x1234;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[0] == 0);
    Machine t(with_syscall({0x0F, 0x01, 0xD6}));
    t.cpu.state().rflags = 0x8D7 | 0x200;
    CHECK(t.finished());
    CHECK((t.cpu.state().rflags & kFlagBits) == 0x40);      // ZF only
    Machine x(with_syscall({0xC6, 0xF8, 0x07}));
    CHECK(x.finished());
    Machine e(with_syscall({0x0F, 0x01, 0xD5}));
    CHECK(e.run(8) == StopReason::Fault && e.cpu.fault().vector == 13);
  }
  {      // rdfsbase / wrgsbase round trip, and a non-canonical wrfsbase is #GP
    Machine m(with_syscall({0x48, 0xB8, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,        // mov rax, 0x1000
                            0xF3, 0x48, 0x0F, 0xAE, 0xD8,                                       // wrgsbase rax
                            0xF3, 0x48, 0x0F, 0xAE, 0xC1,                                       // rdfsbase rcx
                            0xF3, 0x48, 0x0F, 0xAE, 0xCA}));                                    // rdgsbase rdx
    m.cpu.state().fs_base = 0x7000;
    CHECK(m.finished());
    CHECK(m.cpu.state().gs_base == 0x1000 && m.cpu.state().r[1] == 0x7000 && m.cpu.state().r[2] == 0x1000);
    Machine bad(with_syscall({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0x01, 0x00, 0xF3, 0x48, 0x0F, 0xAE, 0xD0}));      // mov rax, 1<<56; wrfsbase rax
    CHECK(bad.run(8) == StopReason::Fault && bad.cpu.fault().vector == 13);
  }
  {      // movdiri stores; movdir64b copies 64 bytes between aligned blocks
    Machine m(with_syscall({0x0F, 0x38, 0xF9, 0x07, 0x66, 0x0F, 0x38, 0xF8, 0x4F, 0x40}));      // movdiri [rdi], eax ; movdir64b rcx, [rdi+64]
    uint8_t src[64];
    for (int i = 0; i < 64; ++i) src[i] = uint8_t(i + 1);
    m.poke(kBase + 64, src, 64);
    m.cpu.state().r[0] = 0xCAFEF00D;
    m.cpu.state().r[1] = kBase + 192;
    CHECK(m.finished());
    uint32_t stored = 0;
    m.peek(kBase, &stored, 4);
    CHECK(stored == 0xCAFEF00D);
    uint8_t copy[64];
    m.peek(kBase + 192, copy, 64);
    CHECK(memcmp(copy, src, 64) == 0);
  }
  {      // cpuid advertises what was added
    Machine m(with_syscall({0x31, 0xC9, 0xB8, 0x07, 0, 0, 0, 0x0F, 0xA2}));
    CHECK(m.finished());
    CHECK(((m.cpu.state().r[3] >> 19) & 1) && ((m.cpu.state().r[3] >> 18) & 1) && (m.cpu.state().r[3] & 1));      // ADX, RDSEED, FSGSBASE
    CHECK((m.cpu.state().r[2] >> 11) & 1);      // RTM_ALWAYS_ABORT
  }
}


// --- enter, xlat, push / pop fs and gs, rcpps / rsqrtps ------------------------------------------------------------------------------------

static void test_odds_and_ends() {
  {      // enter 0x20, level: the saved frame pointers and the new stack and frame pointers
    for (int level : {0, 1, 3}) {
      Machine m(with_syscall({0xC8, 0x20, 0x00, uint8_t(level)}));
      const uint64_t top = kBase + 0x400, old_rbp = kBase + 0x300;
      m.cpu.state().r[4] = top;
      m.cpu.state().r[5] = old_rbp;
      const uint64_t outer[3] = {0xAAA1, 0xAAA2, 0xAAA3};      // the enclosing frame pointers, below old_rbp
      m.poke(old_rbp - 16, outer, 16);
      CHECK(m.finished());
      const uint64_t frame = top - 8;
      uint64_t stored[4] = {};
      m.peek(frame, &stored[0], 8);
      CHECK(stored[0] == old_rbp);
      CHECK(m.cpu.state().r[5] == frame);
      const uint64_t pushed = level == 0 ? 1 : level == 1 ? 2 : 4;      // the old rbp, the (level - 1) copied pointers, the frame pointer
      CHECK(m.cpu.state().r[4] == top - 8 * pushed - 0x20);
      if (level == 1) { uint64_t top_value; m.peek(frame - 8, &top_value, 8); CHECK(top_value == frame); }
      if (level == 3) {
        uint64_t a, b, c;
        m.peek(frame - 8, &a, 8); m.peek(frame - 16, &b, 8); m.peek(frame - 24, &c, 8);
        CHECK(a == 0xAAA2 && b == 0xAAA1 && c == frame);      // the two enclosing pointers (rbp-8, rbp-16), then the frame pointer
      }
    }
    Machine leave_check(with_syscall({0xC8, 0x10, 0x00, 0x00, 0xC9}));      // enter 0x10, 0; leave
    leave_check.cpu.state().r[4] = kBase + 0x400;
    leave_check.cpu.state().r[5] = 0x1234;
    CHECK(leave_check.finished());
    CHECK(leave_check.cpu.state().r[4] == kBase + 0x400 && leave_check.cpu.state().r[5] == 0x1234);
  }
  {      // xlat
    Machine m(with_syscall({0xD7}));
    uint8_t table[16] = {};
    table[5] = 0xAB;
    m.poke(kBase + 0x100, table, 16);
    m.cpu.state().r[3] = kBase + 0x100;
    m.cpu.state().r[0] = 0xFFFFFF05;
    CHECK(m.finished());
    CHECK((m.cpu.state().r[0] & 0xFF) == 0xAB && (m.cpu.state().r[0] >> 8) == 0xFFFFFF);
  }
  {      // push fs, pop gs: a zero selector goes onto the stack and comes off again
    Machine m(with_syscall({0x0F, 0xA0, 0x0F, 0xA9}));
    m.cpu.state().r[4] = kBase + 0x400;
    m.cpu.state().fs_base = 0x5000;
    CHECK(m.finished());
    CHECK(m.cpu.state().r[4] == kBase + 0x400 && m.cpu.state().fs_base == 0x5000);
  }
  {      // rcpps / rsqrtps / rcpss / rsqrtss: within the architecture's 1.5 * 2^-12, the special values exactly
    static const float inputs[4] = {3.0f, 0.25f, 12345.678f, 0.00049f};
    for (int kind = 0; kind < 4; ++kind) {      // 0 rcpps, 1 rsqrtps, 2 rcpss, 3 rsqrtss
      Bytes code;
      if (kind >= 2) code.push_back(0xF3);
      code.insert(code.end(), {0x0F, uint8_t(kind & 1 ? 0x52 : 0x53), 0xC8});
      Machine m(with_syscall(code));
      uint64_t lo, hi;
      memcpy(&lo, inputs, 8);
      memcpy(&hi, inputs + 2, 8);
      m.cpu.state().xmm[0] = {lo, hi};
      m.cpu.state().xmm[1] = {0x1111111111111111ull, 0x2222222222222222ull};
      CHECK(m.finished());
      for (int i = 0; i < (kind >= 2 ? 1 : 4); ++i) {
        uint32_t bits = uint32_t((i < 2 ? m.cpu.state().xmm[1].lo : m.cpu.state().xmm[1].hi) >> (32 * (i & 1)));
        float got;
        memcpy(&got, &bits, 4);
        const double want = (kind & 1) ? 1.0 / __builtin_sqrt(inputs[i]) : 1.0 / inputs[i];
        CHECK(__builtin_fabs(got - want) <= 1.5 * __builtin_ldexp(1.0, -12) * want);
      }
      if (kind >= 2) CHECK(m.cpu.state().xmm[1].lo >> 32 == 0x11111111u && m.cpu.state().xmm[1].hi == 0x2222222222222222ull);
    }
    struct Special { int kind; uint32_t in; uint32_t out; };
    static const Special specials[] = {{0, 0x00000000u, 0x7F800000u}, {0, 0x80000000u, 0xFF800000u}, {0, 0x7F800000u, 0}, {0, 0xFF800000u, 0x80000000u}, {0, 0x00000001u, 0x7F800000u},
                                       {0, 0x7FC00001u, 0x7FC00001u}, {0, 0x7F800001u, 0x7FC00001u}, {1, 0x00000000u, 0x7F800000u}, {1, 0x80000000u, 0xFF800000u}, {1, 0xBF800000u, 0xFFC00000u},
                                       {1, 0x7F800000u, 0}, {1, 0x00400000u, 0x7F800000u}};
    for (const Special& sp : specials) {
      Machine m(with_syscall({0x0F, uint8_t(sp.kind == 0 ? 0x53 : 0x52), 0xC8}));
      m.cpu.state().xmm[0] = {sp.in, 0};
      const uint32_t before = m.cpu.state().mxcsr;
      CHECK(m.finished());
      if (uint32_t(m.cpu.state().xmm[1].lo) != sp.out) { printf("rcp/rsqrt special %d in %08x: got %08x want %08x\n", sp.kind, sp.in, uint32_t(m.cpu.state().xmm[1].lo), sp.out); ++g_failed; }
      CHECK(m.cpu.state().mxcsr == before);      // (no exception flags)
    }
  }
}

// --- xsave / xsaveopt / xsavec / xrstor ----------------------------------------------------------------------------------------------------

static void test_xsave() {
  // xsavec writes the compacted format (XCOMP_BV bit 63, components back to back); xrstor reads both formats back to the same state.
  for (int variant = 0; variant < 3; ++variant) {      // 0 xsave, 1 xsaveopt, 2 xsavec
    Bytes save = variant == 0 ? Bytes{0x0F, 0xAE, 0x27} : variant == 1 ? Bytes{0x0F, 0xAE, 0x37} : Bytes{0x0F, 0xC7, 0x27};      // [rdi]
    Bytes code = save;
    code.insert(code.end(), {0xC5, 0xFC, 0x77});      // vzeroall
    code.insert(code.end(), {0x0F, 0xAE, 0x2F});      // xrstor [rdi]
    Machine m(with_syscall(code));
    X86_64Cpu::State& st = m.cpu.state();
    st.r[0] = 0xE7;
    st.r[2] = 0;
    for (int r = 0; r < 16; ++r) {
      st.xmm[r] = {rnd(), rnd()};
      st.ymm_high[r].lo = rnd(); st.ymm_high[r].hi = rnd();
      st.ymm_high[r].upper[0].lo = rnd(); st.ymm_high[r].upper[0].hi = rnd();
      st.ymm_high[r].upper[1].lo = rnd(); st.ymm_high[r].upper[1].hi = rnd();
      st.evex_lo[r] = {rnd(), rnd()};
      st.evex_hi[r].lo = rnd(); st.evex_hi[r].hi = rnd();
      st.evex_hi[r].upper[0].lo = rnd(); st.evex_hi[r].upper[0].hi = rnd();
      st.evex_hi[r].upper[1].lo = rnd(); st.evex_hi[r].upper[1].hi = rnd();
    }
    for (int k = 0; k < 8; ++k) st.k[k] = rnd();
    const X86_64Cpu::State before = st;
    CHECK(m.finished());
    for (int r = 0; r < 16; ++r) {
      CHECK(st.xmm[r].lo == before.xmm[r].lo && st.xmm[r].hi == before.xmm[r].hi);
      CHECK(st.ymm_high[r].lo == before.ymm_high[r].lo && st.ymm_high[r].hi == before.ymm_high[r].hi);
      CHECK(st.ymm_high[r].upper[0].lo == before.ymm_high[r].upper[0].lo && st.ymm_high[r].upper[1].hi == before.ymm_high[r].upper[1].hi);
      CHECK(st.evex_lo[r].lo == before.evex_lo[r].lo && st.evex_hi[r].upper[1].hi == before.evex_hi[r].upper[1].hi);
    }
    for (int k = 0; k < 8; ++k) CHECK(st.k[k] == before.k[k]);
    uint64_t xcomp = 0, xstate = 0;
    m.peek(kBase + 512, &xstate, 8);
    m.peek(kBase + 520, &xcomp, 8);
    CHECK(xstate == 0xE7);
    CHECK(variant == 2 ? xcomp == (0xE7 | (uint64_t{1} << 63)) : xcomp == 0);
    if (variant == 2) {      // the AVX component follows the header at 576, the opmask registers right after it (832), not at the standard offsets
      uint64_t ymm0_high = 0, k0 = 0;
      m.peek(kBase + 576, &ymm0_high, 8);
      m.peek(kBase + 832, &k0, 8);
      CHECK(ymm0_high == before.ymm_high[0].lo);
      CHECK(k0 == before.k[0]);
    }
  }
}

int main() {
  seed ^= 0x1234;
  if (getenv("X86_SEED")) seed = strtoull(getenv("X86_SEED"), nullptr, 0) | 1;
#ifdef HOST_X86
  build_cases();
  run_cases(getenv("X86_ITER") ? atoi(getenv("X86_ITER")) : 400);
#endif
  test_architectural();
  test_odds_and_ends();
  test_xsave();
  if (g_failed) { printf("X86IntTest: %d failed\n", g_failed); return 1; }
  printf("X86IntTest: all passed\n");
  return 0;
}
