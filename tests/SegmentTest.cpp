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

// Host test of the segment registers and far control transfers: mov to and from a segment register, lss / lfs / lgs, push / pop fs / gs, call far / jmp far through memory, retf, iretq and int n.
// Selectors are validated the way a flat user address space allows (null or RPL 3), CS 0x33 is the only code segment, and a bad one is #GP with nothing changed.
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

constexpr uint64_t kCode = 0x10000, kData = 0x20000, kStack = 0x21000;
struct Machine {
  A alloc;
  process::AddressSpace space{alloc};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtWrite | process::kProtExecute);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64; start.entry = kCode; start.stack_pointer = kStack;
    cpu.reset(start);
  }
  StopReason run(uint64_t n = 16) { GuestMemory m(space); return cpu.run(m, n); }
  bool faulted(int vector) { return run() == StopReason::Fault && cpu.fault().vector == vector; }
  void poke(uint64_t addr, const void* p, size_t n) { space.write_privileged(addr, ConstByteView(static_cast<const uint8_t*>(p), n)); }
  uint64_t peek64(uint64_t addr) { uint64_t v = 0; GuestMemory m(space); m.read(addr, ByteView(reinterpret_cast<uint8_t*>(&v), 8)); return v; }
};

static Bytes with_end(Bytes b) { b.push_back(0x0F); b.push_back(0x0B); return b; }
static X86_64Cpu::State& st(Machine& m) { return m.cpu.state(); }

int main() {
  {      // mov to and from segment registers
    Machine m(with_end({0xB8, 0x2B, 0, 0, 0, 0x8E, 0xC0,       // mov eax, 0x2B; mov es, eax
                        0x31, 0xC0, 0x8E, 0xD8,                 // xor eax, eax (null); mov ds, eax
                        0x8C, 0xC1, 0x8C, 0xDA,                 // mov ecx, es; mov edx, ds
                        0x66, 0x8C, 0xCB}));                    // mov bx, cs
    CHECK(m.faulted(6) && m.cpu.fault().pc == kCode + 18);
    CHECK((st(m).r[1] & 0xFFFF) == 0x2B && (st(m).r[2] & 0xFFFF) == 0 && (st(m).r[3] & 0xFFFF) == 0x33);
  }
  {
    Machine m(with_end({0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0,        // mov rdi, kData
                        0x48, 0xC7, 0x07, 0xFF, 0xFF, 0xFF, 0xFF,   // mov qword [rdi], -1
                        0x8C, 0x1F}));                              // mov [rdi], ds
    CHECK(m.faulted(6));
    CHECK(m.peek64(kData) == 0xFFFFFFFFFFFF002Bull);
  }
  {      // invalid selectors: #GP, nothing changes; mov to cs and a register above gs are #UD
    Machine bad_rpl(with_end({0xB8, 0x28, 0, 0, 0, 0x8E, 0xC0}));      // RPL 0
    CHECK(bad_rpl.faulted(13));
    Machine bad_index(with_end({0xB8, 0xFB, 0xFF, 0, 0, 0x8E, 0xC0}));
    CHECK(bad_index.faulted(13));
    Machine null_ss(with_end({0x31, 0xC0, 0x8E, 0xD0}));      // mov ss, eax (null)
    CHECK(null_ss.faulted(13));
    Machine to_cs(with_end({0xB8, 0x33, 0, 0, 0, 0x8E, 0xC8}));
    CHECK(to_cs.faulted(6));
    Machine sreg6(with_end({0xB8, 0x2B, 0, 0, 0, 0x8E, 0xF0}));
    CHECK(sreg6.faulted(6));
  }
  {      // push / pop fs and gs keep the selector
    Machine m(with_end({0x0F, 0xA0, 0x58, 0x0F, 0xA9}));      // push fs; pop rax; pop gs (pops the next stack slot)
    uint64_t one = 0x2B;
    m.poke(kStack, &one, 8);
    CHECK(m.faulted(6));
    CHECK(st(m).r[0] == 0x2B);
    Machine bad(with_end({0x6A, 0x10, 0x0F, 0xA1}));      // push 0x10; pop fs: RPL 0
    CHECK(bad.faulted(13));
    CHECK(st(bad).r[4] == kStack - 8);      // (the pop did not happen)
  }
  {      // lss / lfs / lgs
    Machine m(with_end({0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0,        // mov rdi, kData
                        0x0F, 0xB4, 0x0F,                           // lfs ecx, [rdi]
                        0x48, 0x0F, 0xB5, 0x57, 0x10,               // lgs rdx, [rdi + 16]
                        0x0F, 0xB2, 0x5F, 0x20,                     // lss ebx, [rdi + 32]
                        0x8C, 0xE0}));                              // mov eax, fs
    const uint32_t a = 0x11223344; const uint16_t sa = 0x53;
    m.poke(kData, &a, 4); m.poke(kData + 4, &sa, 2);
    const uint64_t b = 0x1122334455667788ull; const uint16_t sb = 0x2B;
    m.poke(kData + 16, &b, 8); m.poke(kData + 24, &sb, 2);
    const uint32_t c = 0x1000; const uint16_t sc = 0x2B;
    m.poke(kData + 32, &c, 4); m.poke(kData + 36, &sc, 2);
    CHECK(m.faulted(6));
    CHECK(st(m).r[1] == 0x11223344 && st(m).r[2] == b && st(m).r[3] == 0x1000 && (st(m).r[0] & 0xFFFF) == 0x53);
    Machine reg_form(with_end({0x0F, 0xB4, 0xC1}));
    CHECK(reg_form.faulted(6));
    Machine bad(with_end({0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0, 0x0F, 0xB4, 0x0F}));
    const uint16_t sbad = 0x10;
    bad.poke(kData + 4, &sbad, 2);
    CHECK(bad.faulted(13));
    CHECK(st(bad).r[1] == 0);
  }
  {      // call far / jmp far through memory
    // the target (kCode + 0x100) holds: mov eax, 7; retf
    Bytes code = with_end({0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0,       // mov rdi, kData
                           0x48, 0xFF, 0x1F,                          // call far [rdi] (REX.W: m16:64)
                           0xBB, 9, 0, 0, 0});                         // mov ebx, 9
    code.resize(0x100, 0x90);
    for (uint8_t b : {0xB8, 0x07, 0x00, 0x00, 0x00, 0x48, 0xCB}) code.push_back(b);      // mov eax, 7; retfq
    Machine m(code);
    const uint64_t target = kCode + 0x100; const uint16_t cs = 0x33;
    m.poke(kData, &target, 8); m.poke(kData + 8, &cs, 2);
    m.cpu.state().r[3] = 0;
    // ud2 at the end of the first block is at offset 10 + 3 + 5
    CHECK(m.faulted(6));
    CHECK(st(m).r[0] == 7 && st(m).r[3] == 9 && st(m).r[4] == kStack);
    // jmp far: lands on the target and does not push
    Bytes jcode = with_end({0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0, 0xFF, 0x2F});      // jmp far [rdi] (m16:32)
    jcode.resize(0x100, 0x90);
    for (uint8_t b : {0xB8, 0x05, 0, 0, 0, 0x0F, 0x0B}) jcode.push_back(b);
    Machine j(jcode);
    const uint32_t target32 = uint32_t(kCode + 0x100);
    j.poke(kData, &target32, 4); j.poke(kData + 4, &cs, 2);
    CHECK(j.faulted(6) && j.cpu.fault().pc == kCode + 0x105);
    CHECK(st(j).r[0] == 5 && st(j).r[4] == kStack);
    // a compatibility-mode code selector, a register operand
    Machine compat(with_end({0x48, 0xBF, 0, 0, 2, 0, 0, 0, 0, 0, 0xFF, 0x2F}));
    const uint16_t cs32 = 0x23;
    compat.poke(kData, &target32, 4); compat.poke(kData + 4, &cs32, 2);
    CHECK(compat.faulted(13));
    Machine reg(with_end({0xFF, 0xE8}));
    CHECK(reg.faulted(6));
  }
  {      // retf with an immediate drops arguments
    Machine m(with_end({0x6A, 0x33, 0x68, 0x0B, 0x00, 0x01, 0x00, 0x48, 0xCA, 0x08, 0x00}));      // push cs; push rip (the ud2 at +11); retfq 8
    CHECK(m.faulted(6) && m.cpu.fault().pc == kCode + 11);
    CHECK(st(m).r[4] == kStack - 16 + 16 + 8);
    Machine bad(with_end({0x6A, 0x00, 0x6A, 0x00, 0x48, 0xCB}));      // rip 0, cs 0
    CHECK(bad.faulted(13));
    CHECK(st(bad).r[4] == kStack - 16);
  }
  {      // iretq: rip, cs, rflags, rsp, ss
    Machine m(with_end({0x6A, 0x2B, 0x48, 0xC7, 0xC0, 0x00, 0x00, 0x02, 0x00, 0x50,      // push ss; mov rax, 0x20000; push rax (rsp)
                        0x68, 0x47, 0x02, 0x00, 0x00,                                      // push rflags 0x247 (IF, ZF, PF, CF and the always-one bit... )
                        0x6A, 0x33, 0x68, 0x18, 0x00, 0x01, 0x00,                          // push cs; push rip = the ud2 at kCode + 24
                        0x48, 0xCF}));
    CHECK(m.faulted(6) && m.cpu.fault().pc == kCode + 24);
    CHECK(st(m).r[4] == 0x20000);
    CHECK((st(m).rflags & 0x45) == 0x45);      // CF, PF and ZF came from the frame
    CHECK((st(m).rflags & 0x200) == 0x200);    // IF is not changed by an iret at ring 3 (it was set and the frame's value is the same; see below)
    Machine noif(with_end({0x6A, 0x2B, 0x48, 0xC7, 0xC0, 0x00, 0x00, 0x02, 0x00, 0x50, 0x68, 0x07, 0x00, 0x00, 0x00, 0x6A, 0x33, 0x68, 0x18, 0x00, 0x01, 0x00, 0x48, 0xCF}));
    CHECK(noif.faulted(6));
    CHECK((st(noif).rflags & 0x200) == 0x200);      // (a frame with IF clear does not clear it)
    Machine bad(with_end({0x6A, 0x2B, 0x6A, 0x00, 0x6A, 0x02, 0x6A, 0x10, 0x6A, 0x00, 0x48, 0xCF}));      // cs = 0x10
    CHECK(bad.faulted(13));
  }
  {      // int n and int1
    Machine breakpoint(with_end({0xCD, 0x03}));
    CHECK(breakpoint.faulted(3));
    Machine other(with_end({0xCD, 0x80}));
    CHECK(other.faulted(13));
    Machine debug(with_end({0xF1}));
    CHECK(debug.faulted(1));
    Machine into(with_end({0xCE}));      // invalid in 64-bit mode
    CHECK(into.faulted(6));
    Machine les(with_end({0x06}));       // push es: invalid in 64-bit mode
    CHECK(les.faulted(6));
  }
  {      // every prefix of a few encodings, and bit flips: no sanitizer reports
    Bytes all;
    for (const Bytes& part : {Bytes{0x8E, 0xD8}, Bytes{0x0F, 0xB4, 0x0F}, Bytes{0x48, 0xFF, 0x1F}, Bytes{0x48, 0xCB}, Bytes{0x48, 0xCF}, Bytes{0xCD, 0x21}, Bytes{0xFF, 0x2F}, Bytes{0xCA, 0x08, 0x00}})
      all.insert(all.end(), part.begin(), part.end());
    for (size_t cut = 0; cut < all.size(); ++cut) {
      Machine m(Bytes(all.begin(), all.begin() + cut));
      m.run(8);
    }
    for (size_t i = 0; i < all.size(); ++i)
      for (int bit = 0; bit < 8; ++bit) {
        Bytes b = all;
        b[i] ^= uint8_t(1 << bit);
        Machine m(b);
        st(m).r[7] = kData;
        m.run(32);
      }
  }
  if (g_failed) { printf("SegmentTest: %d failed\n", g_failed); return 1; }
  printf("SegmentTest: all passed\n");
  return 0;
}
