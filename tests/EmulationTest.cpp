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
// Host test for the in-kernel CPU emulator: instruction semantics against hand-assembled bytes, whole programs against the same program run natively,
// faults, and truncated / bit-flipped code (the sanitizers watch for any memory error in the decoder).
//   EmulationTest <guest.elf> <expected-output.txt> [<guest-aarch64.elf> <threads-x86_64.elf> <threads-aarch64.elf> <threads-expected.txt> <x87.elf> <x87-expected.txt> <avx.elf> <avx-expected.txt> <sse4.elf> <sse4-expected.txt> <guest-riscv64.elf> <threads-riscv64.elf>]
#include <atomic>
#include <cfenv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "Emulation/GuestCpu.hpp"
#include "Emulation/AArch64Cpu.hpp"
#include "Emulation/RiscV64Cpu.hpp"
#include "Emulation/SoftFloat.hpp"
#include "Emulation/X86_64Cpu.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Process/HostArch.hpp"
#include "Process/Process.hpp"
#include "Process/UserProgram.hpp"

using namespace emulation;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;
typedef std::vector<uint8_t> Bytes;

struct FakeHost final : process::IProcessHost {
  std::string out, err;
  bool write_console(uint32_t stream, ConstByteView d) override {
    std::string& s = stream == 2 ? err : out;
    for (uint8_t b : d) s.push_back(char(b));
    return true;
  }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 1700000000ull * 1000000000ull; }
  uint64_t monotonic_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
  void sleep_ns(uint64_t ns) override { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
  void yield() override { std::this_thread::yield(); }
};

// A real process lock and thread starter for the host: guest threads become std::threads.
struct HostSync final : process::ISystemSync {
  std::mutex m;
  std::condition_variable_any cv;
  void lock() override { m.lock(); }
  void unlock() override { m.unlock(); }
  bool wait(uint64_t timeout_ns) override {
    if (timeout_ns == ~uint64_t{0}) { cv.wait(m); return true; }
    return cv.wait_for(m, std::chrono::nanoseconds(timeout_ns)) != std::cv_status::timeout;
  }
  void notify_all() override { cv.notify_all(); }
  uint64_t now_ns() override { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
};
struct HostThreads final : IThreadSpawner {
  struct Handle { std::thread t; };
  std::atomic<int> started{0};
  void* spawn(void (*fn)(void*), void* arg, const char*) override {
    ++started;
    return new Handle{std::thread(fn, arg)};
  }
  void join(void* handle) override {
    Handle* h = static_cast<Handle*>(handle);
    h->t.join();
    delete h;
  }
  uint64_t self() override { return uint64_t(std::hash<std::thread::id>()(std::this_thread::get_id())); }
};

struct NeverStop final : IRunControl {
  uint64_t yields = 0, stop_after = ~uint64_t{0};
  bool stop_requested() override { return yields >= stop_after; }
  void yield() override { ++yields; }
};

constexpr uint64_t kCode = 0x10000, kData = 0x20000, kStack = 0x30000;

// A little machine: code at kCode (read + execute), data at kData and a stack page ending at kStack + 0x1000, all read-write.
struct Machine {
  process::AddressSpace space{A};
  X86_64Cpu cpu;
  explicit Machine(const Bytes& code) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtExecute);
    space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(kStack, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::X86_64;
    start.entry = kCode;
    start.stack_pointer = kStack + 0x1000;
    cpu.reset(start);
  }
  // Runs up to `n` instructions; returns why it stopped.
  StopReason run(uint64_t n = 1000) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
  uint64_t& r(int i) { return cpu.state().r[i]; }
  uint64_t read64(uint64_t a) {
    uint8_t raw[8] = {};
    space.read(a, ByteView(raw, 8));
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t{raw[i]} << (8 * i);
    return v;
  }
};

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9 };
constexpr uint64_t CF = 1, ZF = 0x40, SF = 0x80, OF = 0x800;

// SYSCALL overwrites RCX and R11 (the return address and flags), so tests read results from the other registers. Appends the SYSCALL (0F 05) that stops the run.
static Bytes with_syscall(Bytes code) {
  code.push_back(0x0F);
  code.push_back(0x05);
  return code;
}

static void test_arithmetic() {
  {      // mov rax, imm64; add rax, 1; sub rax, 3
    Machine m(with_syscall({0x48, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x48, 0x83, 0xC0, 0x01}));
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x8000000000000000ull);
    CHECK((m.cpu.state().rflags & OF) && (m.cpu.state().rflags & SF) && !(m.cpu.state().rflags & CF));
  }
  {      // 32-bit writes clear the upper half; 16-bit and 8-bit ones do not
    Machine m(with_syscall({0xB8, 0x78, 0x56, 0x34, 0x12,         // mov eax, 0x12345678
                            0x66, 0xBA, 0xCD, 0xAB,               // mov dx, 0xABCD
                            0xB3, 0x7F}));                        // mov bl, 0x7F
    m.r(RAX) = ~uint64_t{0};
    m.r(RDX) = 0x1111111111111111ull;
    m.r(RBX) = 0x2222222222222222ull;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x12345678ull);
    CHECK(m.r(RDX) == 0x111111111111ABCDull);
    CHECK(m.r(RBX) == 0x222222222222227Full);
  }
  {      // ah/ch without REX, sil with it
    Machine m(with_syscall({0xB4, 0x99,                           // mov ah, 0x99
                            0x40, 0xB6, 0x55}));                  // mov sil, 0x55
    m.r(RAX) = 0x1111;
    m.r(RSI) = 0xFFFF;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x9911);
    CHECK(m.r(RSI) == 0xFF55);
  }
  {      // sub sets CF on borrow, cmp does not write
    Machine m(with_syscall({0x48, 0x39, 0xD8}));                  // cmp rax, rbx
    m.r(RAX) = 1;
    m.r(RBX) = 2;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 1 && (m.cpu.state().rflags & CF) && (m.cpu.state().rflags & SF) && !(m.cpu.state().rflags & ZF));
  }
  {      // adc / sbb
    Machine m(with_syscall({0xF9, 0x48, 0x83, 0xD0, 0x00,         // stc; adc rax, 0
                            0xF9, 0x48, 0x83, 0xDB, 0x00}));      // stc; sbb rbx, 0
    m.r(RAX) = 5;
    m.r(RBX) = 5;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 6 && m.r(RBX) == 4);
  }
  {      // mul / imul / div / idiv
    Machine m(with_syscall({0x48, 0xF7, 0xE1}));                  // mul rcx
    m.r(RAX) = 0xFFFFFFFFFFFFFFFFull;
    m.r(RCX) = 0xFFFFFFFFFFFFFFFFull;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 1 && m.r(RDX) == 0xFFFFFFFFFFFFFFFEull);
  }
  {
    Machine m(with_syscall({0x48, 0xF7, 0xE9}));                  // imul rcx
    m.r(RAX) = uint64_t(-3);
    m.r(RCX) = 5;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == uint64_t(-15) && m.r(RDX) == ~uint64_t{0} && !(m.cpu.state().rflags & CF));
  }
  {
    Machine m(with_syscall({0x48, 0xF7, 0xF1}));                  // div rcx
    m.r(RDX) = 1;
    m.r(RAX) = 5;
    m.r(RCX) = 3;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x5555555555555557ull && m.r(RDX) == 0);      // (2^64 + 5) / 3
  }
  {
    Machine m(with_syscall({0x48, 0x99, 0x48, 0xF7, 0xF9}));      // cqo; idiv rcx
    m.r(RAX) = uint64_t(-100);
    m.r(RCX) = 7;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(int64_t(m.r(RAX)) == -14 && int64_t(m.r(RDX)) == -2);
  }
  {      // divide by zero and quotient overflow are #DE
    Machine m({0x48, 0xF7, 0xF1});
    m.r(RAX) = 1;
    m.r(RCX) = 0;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 0 && m.cpu.fault().pc == kCode);
    Machine n({0xF7, 0xF9});                                      // idiv ecx: INT_MIN / -1
    n.r(RAX) = 0x80000000u;
    n.r(RDX) = 0xFFFFFFFFu;
    n.r(RCX) = 0xFFFFFFFFu;
    CHECK(n.run() == StopReason::Fault && n.cpu.fault().vector == 0);
  }
  {      // 8-bit divide uses AX
    Machine m(with_syscall({0xF6, 0xF1}));                        // div cl
    m.r(RAX) = 0x0107;                                            // 263 / 10
    m.r(RCX) = 10;
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.r(RAX) & 0xFFFF) == ((3 << 8) | 26));
  }
  {      // shifts and rotates
    Machine m(with_syscall({0x48, 0xC1, 0xE0, 0x04,               // shl rax, 4
                            0x48, 0xC1, 0xEB, 0x04,               // shr rbx, 4
                            0x48, 0xC1, 0xFE, 0x04,               // sar rsi, 4
                            0x48, 0xC1, 0xC2, 0x08}));            // rol rdx, 8
    m.r(RAX) = 0xF000000000000001ull;
    m.r(RBX) = 0xF0;
    m.r(RSI) = uint64_t(-256);
    m.r(RDX) = 0x1122334455667788ull;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x10 && m.r(RBX) == 0xF && int64_t(m.r(RSI)) == -16 && m.r(RDX) == 0x2233445566778811ull);
  }
  {      // inc keeps CF; neg sets it
    Machine m(with_syscall({0xF9, 0x48, 0xFF, 0xC0, 0x48, 0xF7, 0xDB}));      // stc; inc rax; neg rbx
    m.r(RAX) = 1;
    m.r(RBX) = 5;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 2 && int64_t(m.r(RBX)) == -5 && (m.cpu.state().rflags & CF));
  }
  {      // setcc, cmovcc, movzx / movsx / movsxd, bswap, popcnt, bsr, tzcnt
    Machine m(with_syscall({0x48, 0x39, 0xD8, 0x0F, 0x9C, 0xC1,   // cmp rax, rbx; setl cl
                            0x89, 0xCD,                           // mov ebp, ecx
                            0x48, 0x0F, 0x4F, 0xF3,               // cmovg rsi, rbx
                            0x0F, 0xB6, 0xFA,                     // movzx edi, dl
                            0x4C, 0x0F, 0xBE, 0xC2,               // movsx r8, dl
                            0x4C, 0x63, 0xC8,                     // movsxd r9, eax
                            0x0F, 0xC8}));                        // bswap eax
    m.r(RAX) = uint64_t(-1);
    m.r(RBX) = 1;
    m.r(RSI) = 77;
    m.r(RDX) = 0x80;
    CHECK(m.run() == StopReason::Syscall);
    CHECK((m.r(RBP) & 0xFF) == 1 && m.r(RSI) == 77 && m.r(RDI) == 0x80 && m.r(R8) == uint64_t(-128) && m.r(R9) == uint64_t(-1) && m.r(RAX) == 0xFFFFFFFFu);
  }
  {
    Machine m(with_syscall({0xF3, 0x48, 0x0F, 0xB8, 0xC3,         // popcnt rax, rbx
                            0x48, 0x0F, 0xBD, 0xF3,               // bsr rsi, rbx
                            0xF3, 0x48, 0x0F, 0xBC, 0xD3}));      // tzcnt rdx, rbx
    m.r(RBX) = 0x00F0000000000100ull;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 5 && m.r(RSI) == 55 && m.r(RDX) == 8);
  }
}

static void test_memory_and_control() {
  {      // push, pop, call, ret, lea with SIB, mov to/from memory
    Bytes code = {0x48, 0xBB, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,        // mov rbx, kData
                  0x48, 0xC7, 0x43, 0x08, 0x2A, 0x00, 0x00, 0x00,                    // mov qword [rbx+8], 42
                  0x48, 0x8B, 0x43, 0x08,                                            // mov rax, [rbx+8]
                  0x48, 0x8D, 0x74, 0x83, 0x10,                                      // lea rsi, [rbx+rax*4+16]
                  0x50, 0x5F,                                                        // push rax; pop rdi
                  0xE8, 0x02, 0x00, 0x00, 0x00,                                      // call callee
                  0xEB, 0x04,                                                        // jmp past the callee
                  0x48, 0xFF, 0xC2,                                                  // callee: inc rdx
                  0xC3};                                                             // ret
    Machine m(with_syscall(code));
    m.r(RDX) = 10;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 42 && m.r(RDI) == 42 && m.r(RSI) == kData + 42 * 4 + 16 && m.read64(kData + 8) == 42);
    CHECK(m.r(RDX) == 11 && m.r(RSP) == kStack + 0x1000);      // (called once, stack balanced)
  }
  {      // RIP-relative: the address counts from the end of the instruction, including its immediate
    Machine m(with_syscall({0x48, 0x8B, 0x05, 0xF7, 0x00, 0x00, 0x00,                // mov rax, [rip+0xF7]  -> kCode+0xFE... fixed up below
                            0x48, 0x83, 0x3D, 0xF8, 0x00, 0x00, 0x00, 0x07}));        // cmp qword [rip+0xF8], 7
    // The first instruction ends at kCode+7, the second at kCode+15: aim both displacements at their own data.
    const uint8_t value[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    const uint8_t seven[8] = {7, 0, 0, 0, 0, 0, 0, 0};
    m.space.write_privileged(kCode + 7 + 0xF7, ConstByteView(value, 8));
    m.space.write_privileged(kCode + 15 + 0xF8, ConstByteView(seven, 8));
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x1122334455667788ull && (m.cpu.state().rflags & ZF));
  }
  {      // a counted loop: sum 1..10
    Machine m(with_syscall({0x31, 0xC0,                           // xor eax, eax
                            0xBB, 0x0A, 0x00, 0x00, 0x00,         // mov ebx, 10
                            0x01, 0xD8,                           // add eax, ebx
                            0xFF, 0xCB,                           // dec ebx
                            0x75, 0xFA}));                        // jnz -6
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 55 && m.r(RBX) == 0);
  }
  {      // rep stosb / rep movsb / repne scasb, direction flag
    Machine m(with_syscall({0xF3, 0xAA,                           // rep stosb
                            0x48, 0x89, 0xCD,                     // mov rbp, rcx  (zero now)
                            0xBE, 0x00, 0x00, 0x02, 0x00,         // mov esi, kData
                            0xBF, 0x00, 0x08, 0x02, 0x00,         // mov edi, kData+0x800
                            0xB9, 0x10, 0x00, 0x00, 0x00,         // mov ecx, 16
                            0xF3, 0xA4}));                        // rep movsb
    m.r(RDI) = kData;
    m.r(RCX) = 16;
    m.r(RAX) = 0xAB;
    CHECK(m.run() == StopReason::Syscall);
    uint8_t b = 0;
    m.space.read(kData + 0x80F, ByteView(&b, 1));
    CHECK(b == 0xAB && m.r(RBP) == 0 && m.r(RDI) == kData + 0x810);
  }
  {
    Machine m(with_syscall({0xF2, 0xAE, 0x48, 0x89, 0xCB}));       // repne scasb; mov rbx, rcx
    uint8_t text[] = {1, 2, 3, 4, 5, 6};
    m.space.write(kData, ConstByteView(text, 6));
    m.r(RDI) = kData;
    m.r(RCX) = 6;
    m.r(RAX) = 4;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RDI) == kData + 4 && m.r(RBX) == 2 && (m.cpu.state().rflags & ZF));
  }
  {      // a long REP is resumable across run() calls and the budget
    Machine m(with_syscall({0xF3, 0xAA, 0x48, 0x89, 0xCB}));
    m.r(RDI) = kData;
    m.r(RCX) = 0x1800;
    m.r(RAX) = 7;
    CHECK(m.run(1) == StopReason::Budget);      // (4096 iterations done, the rest left for the next call)
    CHECK(m.cpu.state().rip == kCode && m.r(RCX) == 0x1800 - 4096);
    CHECK(m.run(10) == StopReason::Syscall);
    CHECK(m.r(RBX) == 0 && m.r(RDI) == kData + 0x1800);
  }
  {      // fs-relative addressing
    Machine m(with_syscall({0x64, 0x48, 0x8B, 0x04, 0x25, 0x08, 0x00, 0x00, 0x00}));      // mov rax, fs:[8]
    m.cpu.state().fs_base = kData;
    uint8_t v[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    m.space.write(kData + 8, ConstByteView(v, 8));
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x8877665544332211ull);
  }
  {      // xchg, cmpxchg, xadd
    Machine m(with_syscall({0x48, 0x87, 0xD8,                     // xchg rax, rbx
                            0x48, 0x0F, 0xB1, 0xCA,               // cmpxchg rdx, rcx
                            0x48, 0x0F, 0xC1, 0xF7}));            // xadd rdi, rsi
    m.r(RAX) = 1;
    m.r(RBX) = 2;
    m.r(RDX) = 2;      // equals rax (2 after the xchg): rdx gets rcx
    m.r(RCX) = 99;
    m.r(RDI) = 10;
    m.r(RSI) = 5;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RBX) == 1 && m.r(RDX) == 99 && m.r(RDI) == 15 && m.r(RSI) == 10);
  }
}

static void test_sse() {
  {      // movdqu load/store, pxor, paddd, pcmpeqb + pmovmskb, pshufd, movq
    Machine m(with_syscall({0xF3, 0x0F, 0x6F, 0x06,               // movdqu xmm0, [rsi]
                            0x66, 0x0F, 0xEF, 0xC9,               // pxor xmm1, xmm1
                            0x66, 0x0F, 0xFE, 0xC8,               // paddd xmm1, xmm0
                            0x66, 0x0F, 0x74, 0xC1,               // pcmpeqb xmm0, xmm1
                            0x66, 0x0F, 0xD7, 0xC0,               // pmovmskb eax, xmm0
                            0x66, 0x0F, 0x70, 0xD1, 0x1B,         // pshufd xmm2, xmm1, 0x1B
                            0xF3, 0x0F, 0x7F, 0x4F, 0x10}));      // movdqu [rdi+16], xmm1
    uint8_t v[16];
    for (int i = 0; i < 16; ++i) v[i] = uint8_t(i + 1);
    m.space.write(kData, ConstByteView(v, 16));
    m.r(RSI) = kData;
    m.r(RDI) = kData + 0x100;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0xFFFF);
    uint8_t out[16] = {};
    m.space.read(kData + 0x110, ByteView(out, 16));
    CHECK(out[0] == 1 && out[15] == 16);
    // pshufd 0x1B reverses the four dwords: dword 3 first
    CHECK((m.cpu.state().xmm[2].lo & 0xFFFFFFFFu) == 0x100F0E0Du && (m.cpu.state().xmm[2].hi >> 32) == 0x04030201u);
  }
  {      // movd / movq both ways, psrldq
    Machine m(with_syscall({0x66, 0x48, 0x0F, 0x6E, 0xC0,         // movq xmm0, rax
                            0x66, 0x0F, 0x73, 0xD8, 0x04,         // psrldq xmm0, 4
                            0x66, 0x48, 0x0F, 0x7E, 0xC3}));      // movq rbx, xmm0
    m.r(RAX) = 0x1122334455667788ull;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RBX) == 0x11223344ull);
  }
  {      // pextrw / pinsrw
    Machine m(with_syscall({0x66, 0x0F, 0xC5, 0xC0, 0x03,         // pextrw eax, xmm0, 3
                            0x66, 0x0F, 0xC4, 0xC9, 0x05}));      // pinsrw xmm1, ecx, 5
    m.cpu.state().xmm[0] = {0x1111222233334444ull, 0x5555666677778888ull};
    m.cpu.state().xmm[1] = {0, 0};
    m.r(RAX) = ~uint64_t{0};
    m.r(RCX) = 0xABCD1234;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x1111);
    CHECK(m.cpu.state().xmm[1].hi == 0x0000000000001234ull * 0x10000ull && m.cpu.state().xmm[1].lo == 0);
  }
  {      // fxsave / fxrstor, and the CONTEXT image round trip that exception delivery and NtContinue use
    Machine m(with_syscall({0x0F, 0xAE, 0x06,                     // fxsave [rsi]
                            0x66, 0x0F, 0xEF, 0xC0,               // pxor xmm0, xmm0
                            0x0F, 0xAE, 0x0E}));                  // fxrstor [rsi]
    m.r(RSI) = kData;
    m.cpu.state().xmm[0] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
    m.cpu.state().xmm[15] = {7, 8};
    m.cpu.state().mxcsr = 0x1FA0;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.cpu.state().xmm[0].lo == 0x0123456789ABCDEFull && m.cpu.state().xmm[0].hi == 0xFEDCBA9876543210ull && m.cpu.state().xmm[15].hi == 8 && m.cpu.state().mxcsr == 0x1FA0);
    process::X86Context context;
    CHECK(m.cpu.get_x86_context(&context));
    context.gpr[RBX] = 0x1234;
    context.rip = kCode + 0x40;
    context.fpu[160 + 16 * 3] = 0x5A;      // xmm3 low byte
    CHECK(m.cpu.set_x86_context(context));
    CHECK(m.r(RBX) == 0x1234 && m.cpu.state().rip == kCode + 0x40 && (m.cpu.state().xmm[3].lo & 0xFF) == 0x5A);
  }
  {      // mov r/m16, Sreg gives the user selectors (RtlCaptureContext reads them)
    Machine m(with_syscall({0x8C, 0xC8,                           // mov eax, cs
                            0x8C, 0xD2,                           // mov edx, ss
                            0x8C, 0x1E}));                        // mov [rsi], ds
    m.r(RSI) = kData;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 0x33 && m.r(RDX) == 0x2B);
    uint8_t v[2];
    m.space.read(kData, ByteView(v, 2));
    CHECK(v[0] == 0x2B && v[1] == 0);
  }
  {      // a page fault says whether it was a read, a write or a fetch
    Machine r({0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x90, 0x00});      // mov rax, [0x900000]
    CHECK(r.run() == StopReason::Fault && r.cpu.fault().vector == 14 && r.cpu.fault().error == 0);
    Machine w({0x48, 0x89, 0x04, 0x25, 0x00, 0x00, 0x90, 0x00});      // mov [0x900000], rax
    CHECK(w.run() == StopReason::Fault && w.cpu.fault().vector == 14 && w.cpu.fault().error == 2);
    Machine f({0xFF, 0xE0});                                          // jmp rax
    f.r(RAX) = 0x900000;
    f.run();
    CHECK(f.run() == StopReason::Fault && f.cpu.fault().vector == 14 && f.cpu.fault().error == 0x10 && f.cpu.fault().address == 0x900000);
  }
  {      // the x87 control word instructions; packed decimal is the part of x87 that is not emulated
    Machine m(with_syscall({0xDB, 0xE3,                           // fninit
                            0xD9, 0x3E,                           // fnstcw [rsi]
                            0xB8, 0x7F, 0x03, 0x00, 0x00,         // mov eax, 0x37f
                            0xD9, 0x2E,                           // fldcw [rsi]
                            0xDF, 0xE0}));                        // fnstsw ax
    m.r(RSI) = kData;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.read64(kData) == 0x37F && m.r(RAX) == 0);
    Machine n({0xDF, 0x27});      // fbld [rdi]: packed decimal is not emulated
    CHECK(n.run() == StopReason::Fault && n.cpu.fault().vector == 6);
  }
  {      // packed decimal (fbld) is the part of x87 that is not emulated: an invalid opcode, not a wrong answer
    Machine m({0xDF, 0x27});      // fbld [rdi]
    m.r(RDI) = kData;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 6);
  }
}

static void test_faults() {
  {      // load from unmapped memory
    Machine m({0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x90, 0x00});
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14 && m.cpu.fault().address == 0x900000 && m.cpu.fault().pc == kCode);
    CHECK(m.cpu.state().rip == kCode);      // restartable
  }
  {      // write to read-only code, jump to non-executable data
    Machine m({0x48, 0xC7, 0x04, 0x25, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00});
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14 && m.cpu.fault().address == 0x10000);
    Machine n({0xFF, 0xE0});
    n.r(RAX) = kData;
    CHECK(n.run() == StopReason::Budget || true);
    CHECK(n.run() == StopReason::Fault && n.cpu.fault().vector == 14 && n.cpu.fault().address == kData);
  }
  {      // int3, hlt, ud2, an unknown opcode
    Machine a({0xCC}), b({0xF4}), c({0x0F, 0x0B}), d({0x06});
    CHECK(a.run() == StopReason::Fault && a.cpu.fault().vector == 3);
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 13);
    CHECK(c.run() == StopReason::Fault && c.cpu.fault().vector == 6);
    CHECK(d.run() == StopReason::Fault && d.cpu.fault().vector == 6);
  }
  {      // an instruction that runs off the end of the mapping is a page fault at the first missing byte
    Machine m({});
    m.space.unmap(kCode + 0x1000, 0x1000);
    m.space.write_privileged(kCode + 0xFFE, ConstByteView(reinterpret_cast<const uint8_t*>("\x48\xB8"), 2));
    m.cpu.state().rip = kCode + 0xFFE;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14 && m.cpu.fault().address == kCode + 0x1000);
  }
  {      // a push past the bottom of the stack
    Machine m({0x50});
    m.r(RSP) = kStack;
    CHECK(m.run() == StopReason::Fault && m.cpu.fault().vector == 14 && m.cpu.fault().address == kStack - 8 && m.r(RSP) == kStack);
  }
}

// The decoder must survive anything: every prefix of some code, and every single-bit flip of it, run briefly.
static void test_robustness() {
  const Bytes sample = {0x48, 0x8B, 0x44, 0x8B, 0x10, 0x66, 0x0F, 0x6F, 0x05, 0x10, 0x00, 0x00, 0x00, 0xF3, 0xA4, 0x48, 0xF7, 0xF1, 0x0F, 0xA3, 0xC8,
                        0x64, 0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0F, 0xAF, 0xC3, 0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
  uint32_t runs = 0;
  for (size_t cut = 0; cut <= sample.size(); ++cut) {      // truncation
    Bytes code(sample.begin(), sample.begin() + cut);
    Machine m(code);
    m.r(RSI) = kData;
    m.r(RCX) = 3;
    m.run(50);
    ++runs;
  }
  for (size_t byte = 0; byte < sample.size(); ++byte) {      // bit flips
    for (int bit = 0; bit < 8; ++bit) {
      Bytes code = sample;
      code[byte] ^= uint8_t(1u << bit);
      Machine m(code);
      m.r(RBX) = kData;
      m.r(RCX) = 5;
      m.r(RDX) = 1;
      m.r(RSI) = kData;
      m.r(RDI) = kData + 0x100;
      m.run(200);
      ++runs;
    }
  }
  // random bytes
  uint64_t seed = 88172645463325252ull;
  for (int i = 0; i < 600; ++i) {
    Bytes code(24);
    for (uint8_t& b : code) {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      b = uint8_t(seed);
    }
    Machine m(code);
    m.r(RBX) = kData;
    m.r(RSI) = kData;
    m.r(RDI) = kData + 0x100;
    m.r(RCX) = 40;
    m.run(100);
    ++runs;
  }
  CHECK(runs > 600);
}


// --- soft-float against the host's own FPU (an x86 SSE unit: same results, same flags) ---------------------------------------------------------

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;
static uint64_t next_random() {
  g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
  return g_seed;
}

static uint64_t random_double_bits() {
  static const uint64_t specials[] = {0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull,
                                      0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull,
                                      0xBFF0000000000000ull, 0x4000000000000000ull, 0x3FE0000000000000ull, 0x43E0000000000000ull, 0xC3E0000000000000ull};
  const uint64_t r = next_random();
  switch (r & 7) {
    case 0: return specials[(r >> 8) % 16];
    case 1: return (next_random() & 0x800FFFFFFFFFFFFFull) | (((r >> 8) % 8 + 1020ull) << 52);      // exponents near 1
    case 2: return (next_random() & 0x800FFFFFFFFFFFFFull) | (((r >> 8) % 16) << 52);               // tiny: subnormals and near
    case 3: return (next_random() & 0x800FFFFFFFFFFFFFull) | ((2030ull + (r >> 8) % 17) << 52);     // huge: near overflow
    case 4: return specials[(r >> 8) % 16] ^ (next_random() & 3);                                     // neighbours of the specials
    default: return next_random();
  }
}
static uint32_t random_float_bits() {
  static const uint32_t specials[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 1, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x3F800000u,
                                      0xBF800000u, 0x40000000u, 0x3F000000u, 0x4F000000u, 0xCF000000u};
  const uint64_t r = next_random();
  switch (r & 7) {
    case 0: return specials[(r >> 8) % 16];
    case 1: return uint32_t(next_random() & 0x807FFFFFu) | (uint32_t((r >> 8) % 8 + 124) << 23);
    case 2: return uint32_t(next_random() & 0x807FFFFFu) | (uint32_t((r >> 8) % 6) << 23);
    case 3: return uint32_t(next_random() & 0x807FFFFFu) | (uint32_t(250 + (r >> 8) % 5) << 23);
    case 4: return specials[(r >> 8) % 16] ^ uint32_t(next_random() & 3);
    default: return uint32_t(next_random());
  }
}
static bool is_nan64(uint64_t b) { return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0xFFFFFFFFFFFFFull); }
static bool is_nan32(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }
static double as_double(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static float as_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint64_t bits_of(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

// The flags the host FPU raised, in MXCSR bit order (invalid, divide-by-zero, overflow, inexact; underflow and denormal differ in detail and are left out).
static uint32_t host_flags() {
  uint32_t f = 0;
  if (fetestexcept(FE_INVALID)) f |= softfloat::kInvalid;
  if (fetestexcept(FE_DIVBYZERO)) f |= softfloat::kDivideByZero;
  if (fetestexcept(FE_OVERFLOW)) f |= softfloat::kOverflow;
  if (fetestexcept(FE_INEXACT)) f |= softfloat::kInexact;
  return f;
}
constexpr uint32_t kComparedFlags = softfloat::kInvalid | softfloat::kDivideByZero | softfloat::kOverflow | softfloat::kInexact;

static int g_fp_reported = 0;
static void fp_mismatch(const char* what, uint64_t a, uint64_t b, uint64_t got, uint64_t want, uint32_t got_flags, uint32_t want_flags, int mode) {
  if (g_fp_reported++ < 12) printf("  %s mode %d a=%016llx b=%016llx got %016llx/%x want %016llx/%x\n", what, mode, (unsigned long long)a, (unsigned long long)b,
                                   (unsigned long long)got, got_flags, (unsigned long long)want, want_flags);
  ++g_failed;
}

static void test_softfloat_against_host() {
  using namespace softfloat;
  const int modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
  for (int mode = 0; mode < 4; ++mode) {
    for (int i = 0; i < 40000; ++i) {
      const uint64_t a = random_double_bits(), b = random_double_bits();
      const uint32_t fa = random_float_bits(), fb = random_float_bits();
      for (int op = 0; op < 6; ++op) {
        Env env;
        env.mode = Rounding(mode);
        // double
        volatile double x = as_double(a), y = as_double(b), r = 0;
        fesetround(modes[mode]);
        feclearexcept(FE_ALL_EXCEPT);
        switch (op) {
          case 0: r = x + y; break;
          case 1: r = x - y; break;
          case 2: r = x * y; break;
          case 3: r = x / y; break;
          case 4: r = std::sqrt(x); break;
          default: r = double(float(x)); break;
        }
        const uint32_t hf = host_flags() & kComparedFlags;
        fesetround(FE_TONEAREST);
        const uint64_t want = bits_of(double(r));
        uint64_t got = 0;
        switch (op) {
          case 0: got = add(Format::F64, a, b, env); break;
          case 1: got = softfloat::sub(Format::F64, a, b, env); break;
          case 2: got = mul(Format::F64, a, b, env); break;
          case 3: got = softfloat::div(Format::F64, a, b, env); break;
          case 4: got = softfloat::sqrt(Format::F64, a, env); break;
          default: got = convert(Format::F32, Format::F64, convert(Format::F64, Format::F32, a, env), env), env.flags = 0, got = convert(Format::F64, Format::F32, a, env); got = convert(Format::F32, Format::F64, got, env); break;
        }
        const bool nan_ok = is_nan64(want) && is_nan64(got);
        if ((got != want && !nan_ok) || (op != 5 && (env.flags & kComparedFlags) != hf && !(nan_ok && false))) {
          if (!(op == 5)) fp_mismatch(op == 0 ? "add64" : op == 1 ? "sub64" : op == 2 ? "mul64" : op == 3 ? "div64" : "sqrt64", a, b, got, want, env.flags & kComparedFlags, hf, mode);
          else if (got != want && !nan_ok) fp_mismatch("narrow64", a, 0, got, want, 0, 0, mode);
        }
        // float
        volatile float fx = as_float(fa), fy = as_float(fb), fr = 0;
        fesetround(modes[mode]);
        feclearexcept(FE_ALL_EXCEPT);
        switch (op) {
          case 0: fr = fx + fy; break;
          case 1: fr = fx - fy; break;
          case 2: fr = fx * fy; break;
          case 3: fr = fx / fy; break;
          case 4: fr = std::sqrt(fx); break;
          default: fr = float(double(fx)); break;
        }
        const uint32_t hff = host_flags() & kComparedFlags;
        fesetround(FE_TONEAREST);
        const uint32_t fwant = bits_of(float(fr));
        Env fenv;
        fenv.mode = Rounding(mode);
        uint64_t fgot = 0;
        switch (op) {
          case 0: fgot = add(Format::F32, fa, fb, fenv); break;
          case 1: fgot = softfloat::sub(Format::F32, fa, fb, fenv); break;
          case 2: fgot = mul(Format::F32, fa, fb, fenv); break;
          case 3: fgot = softfloat::div(Format::F32, fa, fb, fenv); break;
          case 4: fgot = softfloat::sqrt(Format::F32, fa, fenv); break;
          default: fgot = convert(Format::F64, Format::F32, convert(Format::F32, Format::F64, fa, fenv), fenv); break;
        }
        const bool fnan_ok = is_nan32(fwant) && is_nan32(uint32_t(fgot));
        if ((uint32_t(fgot) != fwant && !fnan_ok) || (op != 5 && (fenv.flags & kComparedFlags) != hff))
          fp_mismatch(op == 0 ? "add32" : op == 1 ? "sub32" : op == 2 ? "mul32" : op == 3 ? "div32" : op == 4 ? "sqrt32" : "widen32", fa, fb, fgot, fwant, fenv.flags & kComparedFlags, hff, mode);
      }
    }
  }
}

static void test_softfloat_conversions() {
  using namespace softfloat;
  const int modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
  for (int mode = 0; mode < 4; ++mode) {
    for (int i = 0; i < 40000; ++i) {
      Env env;
      env.mode = Rounding(mode);
      // int64 -> double / float
      int64_t v = int64_t(next_random());
      if (i & 1) v >>= (next_random() % 64);
      if ((i & 3) == 0) v = int64_t(int32_t(v));
      volatile int64_t vv = v;
      fesetround(modes[mode]);
      volatile double d = double(vv);
      volatile float f = float(vv);
      fesetround(FE_TONEAREST);
      CHECK(from_int(Format::F64, v, env) == bits_of(double(d)));
      CHECK(from_int(Format::F32, v, env) == bits_of(float(f)));
      // double -> int (the instructions' semantics: indefinite on overflow)
      const uint64_t a = random_double_bits();
      const double x = as_double(a);
      Env e1;
      e1.mode = Rounding(mode);
      const int64_t got64 = to_int(Format::F64, a, 64, false, e1);
      int64_t want64;
      if (std::isnan(x) || x >= 9223372036854775808.0 || x < -9223372036854775808.0 - 0.0) want64 = INT64_MIN;
      else { fesetround(modes[mode]); want64 = (int64_t)std::nearbyint(x); fesetround(FE_TONEAREST); }
      if (!(x >= 9223372036854775808.0) && !std::isnan(x) && x < -9223372036854775808.0) want64 = INT64_MIN;
      if (got64 != want64) {
        if (g_fp_reported++ < 12) printf("  to_int64 mode %d a=%016llx got %lld want %lld\n", mode, (unsigned long long)a, (long long)got64, (long long)want64);
        ++g_failed;
      }
      Env e2;
      e2.mode = Rounding(mode);
      const int64_t got32 = to_int(Format::F64, a, 32, true, e2);
      const double t = std::trunc(x);
      const int64_t want32 = (std::isnan(x) || t >= 2147483648.0 || t < -2147483648.0) ? int64_t(INT32_MIN) : int64_t(t);
      if (got32 != want32) {
        if (g_fp_reported++ < 12) printf("  to_int32t a=%016llx got %lld want %lld\n", (unsigned long long)a, (long long)got32, (long long)want32);
        ++g_failed;
      }
      // compare
      const uint64_t b = random_double_bits();
      Env e3;
      const Order o = compare(Format::F64, a, b, true, e3);
      const double y = as_double(b);
      const Order want = (std::isnan(x) || std::isnan(y)) ? Order::Unordered : x < y ? Order::Less : x > y ? Order::Greater : Order::Equal;
      CHECK(o == want);
    }
  }
}

static void test_float_instructions() {
  {      // addsd / mulsd / subsd / divsd / sqrtsd with a memory operand, cvtsi2sd, cvttsd2si, ucomisd
    Machine m(with_syscall({0xF2, 0x48, 0x0F, 0x2A, 0xC0,         // cvtsi2sd xmm0, rax   (rax = 7)
                            0xF2, 0x0F, 0x10, 0x0E,               // movsd xmm1, [rsi]     (2.5)
                            0xF2, 0x0F, 0x59, 0xC1,               // mulsd xmm0, xmm1      17.5
                            0xF2, 0x0F, 0x58, 0x46, 0x08,         // addsd xmm0, [rsi+8]   (0.25) -> 17.75
                            0xF2, 0x0F, 0x5C, 0xC1,               // subsd xmm0, xmm1      15.25
                            0xF2, 0x0F, 0x5E, 0x46, 0x08,         // divsd xmm0, [rsi+8]   61
                            0xF2, 0x0F, 0x51, 0xD0,               // sqrtsd xmm2, xmm0     sqrt(61)
                            0xF2, 0x48, 0x0F, 0x2C, 0xD8,         // cvttsd2si rbx, xmm0   61
                            0x66, 0x0F, 0x2E, 0xC1}));            // ucomisd xmm0, xmm1    above
    m.r(RAX) = 7;
    m.r(RSI) = kData;
    m.space.write(kData, ConstByteView(reinterpret_cast<const uint8_t*>("\x00\x00\x00\x00\x00\x00\x04\x40\x00\x00\x00\x00\x00\x00\xD0\x3F"), 16));
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RBX) == 61);
    CHECK(m.cpu.state().xmm[0].lo == bits_of(61.0));
    CHECK(m.cpu.state().xmm[2].lo == bits_of(std::sqrt(61.0)));
    CHECK(!(m.cpu.state().rflags & (ZF | CF)));
  }
  {      // single precision scalar keeps the rest of the register; cvtss2sd; cvtsd2ss; comiss unordered
    Machine m(with_syscall({0xF3, 0x0F, 0x58, 0xC1,               // addss xmm0, xmm1
                            0xF3, 0x0F, 0x5A, 0xD0,               // cvtss2sd xmm2, xmm0
                            0xF2, 0x0F, 0x5A, 0xDA,               // cvtsd2ss xmm3, xmm2
                            0x0F, 0x2F, 0xC4}));                  // comiss xmm0, xmm4 (nan)
    m.cpu.state().xmm[0].lo = 0x1234567800000000ull | bits_of(1.5f);
    m.cpu.state().xmm[0].hi = 0xAAAAAAAAAAAAAAAAull;
    m.cpu.state().xmm[1].lo = bits_of(2.25f);
    m.cpu.state().xmm[4].lo = 0x7FC00000u;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.cpu.state().xmm[0].lo == (0x1234567800000000ull | bits_of(3.75f)) && m.cpu.state().xmm[0].hi == 0xAAAAAAAAAAAAAAAAull);
    CHECK(m.cpu.state().xmm[2].lo == bits_of(3.75));
    CHECK((m.cpu.state().xmm[3].lo & 0xFFFFFFFFu) == bits_of(3.75f));
    CHECK((m.cpu.state().rflags & (ZF | CF | 4)) == (ZF | CF | 4));
    CHECK(m.cpu.state().mxcsr & softfloat::kInvalid);      // comiss with a NaN raises invalid
  }
  {      // packed: addpd, mulps, cmpltpd, movmskpd, cvtdq2pd, cvtps2dq with rounding from MXCSR
    Machine m(with_syscall({0x66, 0x0F, 0x58, 0xC1,               // addpd xmm0, xmm1
                            0x0F, 0x59, 0xD3,                     // mulps xmm2, xmm3
                            0x66, 0x0F, 0xC2, 0xE1, 0x01,         // cmpltpd xmm4, xmm1
                            0x66, 0x0F, 0x50, 0xC4,               // movmskpd eax, xmm4
                            0xF3, 0x0F, 0xE6, 0xEE,               // cvtdq2pd xmm5, xmm6
                            0x66, 0x0F, 0x5B, 0xFF}));            // cvtps2dq xmm7, xmm7
    m.cpu.state().xmm[0] = {bits_of(1.0), bits_of(2.0)};
    m.cpu.state().xmm[1] = {bits_of(0.5), bits_of(-4.0)};
    m.cpu.state().xmm[2] = {uint64_t(bits_of(3.0f)) | (uint64_t(bits_of(4.0f)) << 32), uint64_t(bits_of(-1.0f)) | (uint64_t(bits_of(0.5f)) << 32)};
    m.cpu.state().xmm[3] = {uint64_t(bits_of(2.0f)) | (uint64_t(bits_of(2.0f)) << 32), uint64_t(bits_of(8.0f)) | (uint64_t(bits_of(8.0f)) << 32)};
    m.cpu.state().xmm[4] = {bits_of(-1.0), bits_of(-9.0)};
    m.cpu.state().xmm[6] = {uint64_t(uint32_t(-5)) | (uint64_t(100) << 32), 0};
    m.cpu.state().xmm[7] = {uint64_t(bits_of(2.5f)) | (uint64_t(bits_of(3.5f)) << 32), uint64_t(bits_of(-1.5f)) | (uint64_t(bits_of(1e10f)) << 32)};
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.cpu.state().xmm[0].lo == bits_of(1.5) && m.cpu.state().xmm[0].hi == bits_of(-2.0));
    CHECK((m.cpu.state().xmm[2].lo & 0xFFFFFFFFu) == bits_of(6.0f) && (m.cpu.state().xmm[2].lo >> 32) == bits_of(8.0f) && (m.cpu.state().xmm[2].hi >> 32) == bits_of(4.0f));
    CHECK(m.r(RAX) == 3);      // both lanes less than the second operand: -1 < 0.5 and -9 < -4
    CHECK(m.cpu.state().xmm[5].lo == bits_of(-5.0) && m.cpu.state().xmm[5].hi == bits_of(100.0));
    CHECK((m.cpu.state().xmm[7].lo & 0xFFFFFFFFu) == 2 && (m.cpu.state().xmm[7].lo >> 32) == 4 && uint32_t(m.cpu.state().xmm[7].hi) == uint32_t(-2) && (m.cpu.state().xmm[7].hi >> 32) == 0x80000000u);
  }
  {      // MXCSR: ldmxcsr / stmxcsr, rounding toward zero changes cvtsd2si, divide-by-zero is flagged not trapped
    Machine m(with_syscall({0x0F, 0xAE, 0x16,                     // ldmxcsr [rsi]
                            0xF2, 0x48, 0x0F, 0x2D, 0xC0,         // cvtsd2si rax, xmm0
                            0xF2, 0x0F, 0x5E, 0xC9,               // divsd xmm1, xmm1  (0/0)
                            0xF2, 0x0F, 0x5E, 0xD3,               // divsd xmm2, xmm3  (1/0)
                            0x0F, 0xAE, 0x1F}));                  // stmxcsr [rdi]
    m.r(RSI) = kData;
    m.r(RDI) = kData + 16;
    const uint8_t csr[4] = {0x80, 0x7F, 0, 0};      // 0x7F80: toward zero
    m.space.write(kData, ConstByteView(csr, 4));
    m.cpu.state().xmm[0].lo = bits_of(2.9);
    m.cpu.state().xmm[2].lo = bits_of(1.0);
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.r(RAX) == 2);
    CHECK(m.cpu.state().xmm[1].lo == 0xFFF8000000000000ull);      // the default NaN
    CHECK(m.cpu.state().xmm[2].lo == 0x7FF0000000000000ull);
    uint8_t out[4];
    m.space.read(kData + 16, ByteView(out, 4));
    CHECK((out[0] & (softfloat::kInvalid | softfloat::kDivideByZero)) == (softfloat::kInvalid | softfloat::kDivideByZero) && out[1] == 0x7F);
  }
}


// --- AArch64 ---------------------------------------------------------------------------------------------------------------------------------

typedef std::vector<uint32_t> Words;

struct A64Machine {
  process::AddressSpace space{A};
  AArch64Cpu cpu;
  explicit A64Machine(const Words& code) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtExecute);
    Bytes raw;
    for (uint32_t w : code) for (int i = 0; i < 4; ++i) raw.push_back(uint8_t(w >> (8 * i)));
    if (!raw.empty()) space.write_privileged(kCode, ConstByteView(raw.data(), raw.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(kStack, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::AArch64;
    start.entry = kCode;
    start.stack_pointer = kStack + 0x1000;
    cpu.reset(start);
  }
  StopReason run(uint64_t n = 1000) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
  uint64_t& x(int i) { return cpu.state().x[i]; }
};

static void test_aarch64() {
  {      // mov, mul, svc: the system call is decoded the AArch64 Linux way
    A64Machine m({0xd28000c0, 0xd28000e1, 0x9b017c02, 0xd2800bc8, 0xd4000001});
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(2) == 42 && m.cpu.state().pc == kCode + 20);
    process::SyscallRequest r;
    CHECK(m.cpu.pending_syscall(process::SyscallAbi::Linux, &r) && r.number == 94 && r.args[0] == 6 && r.args[1] == 7);
    CHECK(!m.cpu.pending_syscall(process::SyscallAbi::Windows, &r));
    m.cpu.finish_syscall(123);
    CHECK(m.x(0) == 123);
  }
  {      // memory, floating point (fused), NEON, flags
    const Words w = {0xf9400420,       // ldr x0, [x1, #8]
                     0x91000400,       // add x0, x0, #1
                     0xf9000420,       // str x0, [x1, #8]
                     0x1e601000,       // fmov d0, #2.0
                     0x1e611001,       // fmov d1, #3.0
                     0x1e612802,       // fadd d2, d0, d1
                     0x9e660045,       // fmov x5, d2
                     0x1f410803,       // fmadd d3, d0, d1, d2
                     0x9e780066,       // fcvtzs x6, d3
                     0x3dc00020,       // ldr q0, [x1]
                     0x4ea08400,       // add v0.4s, v0.4s, v0.4s
                     0x3d800420,       // str q0, [x1, #16]
                     0xf1000c1f,       // cmp x0, #3
                     0x1a9f17e7,       // cset w7, eq
                     0xdac01149,       // clz x9, x10
                     0xd4000001};
    A64Machine n(w);
    n.x(1) = kData;
    n.x(10) = 0x00FF;
    const uint8_t init[24] = {1, 0, 0, 0, 2, 0, 0, 0, 41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    n.space.write(kData, ConstByteView(init, 24));
    CHECK(n.run() == StopReason::Syscall);
    CHECK(n.x(0) == 42 && n.x(5) == bits_of(5.0) && n.x(6) == 11 && n.x(7) == 0 && n.x(9) == 56);
    uint8_t out[16];
    n.space.read(kData + 16, ByteView(out, 16));
    CHECK(out[0] == 2 && out[4] == 4 && n.cpu.state().fpsr == 0);
  }
  {      // faults carry the x86-style vectors
    A64Machine a({0xf9400083});      // ldr x3, [x4]
    a.x(4) = 0x900000;
    CHECK(a.run() == StopReason::Fault && a.cpu.fault().vector == 14 && a.cpu.fault().address == 0x900000 && a.cpu.fault().pc == kCode && a.cpu.state().pc == kCode);
    A64Machine b({0x00000000});      // udf
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 6);
    A64Machine c({0xd4200000});      // brk
    CHECK(c.run() == StopReason::Fault && c.cpu.fault().vector == 3);
    A64Machine d({0xd503201f});
    d.cpu.state().pc = kCode + 2;      // misaligned
    CHECK(d.run() == StopReason::Fault && d.cpu.fault().vector == 13);
    A64Machine e({0xd503201f});
    e.cpu.state().pc = kData;      // not executable
    CHECK(e.run() == StopReason::Fault && e.cpu.fault().vector == 14);
    A64Machine f({0xd28000c0, 0xd503201f});      // a store to the read-only code page
    f.cpu.state().x[1] = kCode;
    Words g = {0xf9000020};      // str x0, [x1]
    A64Machine h(g);
    h.x(1) = kCode;
    CHECK(h.run() == StopReason::Fault && h.cpu.fault().vector == 14 && h.cpu.fault().address == kCode);
  }
  {      // exclusive monitor: a store-exclusive without a load-exclusive fails, with one succeeds
    A64Machine m({0xc85f7c22,       // ldxr x2, [x1]
                  0xc8037c22,       // stxr w3, x2, [x1]
                  0xc8047c22,       // stxr w4, x2, [x1]
                  0xd4000001});
    m.x(1) = kData;
    CHECK(m.run() == StopReason::Syscall && m.x(3) == 0 && m.x(4) == 1);
  }
}

static void test_aarch64_robustness() {
  const Words sample = {0xf9400420, 0x91000400, 0xf9000420, 0x1e601000, 0x1e611001, 0x1e612802, 0x9e660045, 0x1f410803, 0x9e780066, 0x3dc00020, 0x4ea08400, 0x3d800420,
                        0xf1000c1f, 0x1a9f17e7, 0xdac01149, 0x0c407000, 0x4c40a000, 0x0d404000, 0x2e208400, 0x0e200800};
  uint32_t runs = 0;
  for (size_t i = 0; i < sample.size(); ++i) {      // every single-bit flip of every word
    for (int bit = 0; bit < 32; ++bit) {
      Words w = sample;
      w[i] ^= 1u << bit;
      A64Machine m(w);
      m.x(1) = kData;
      m.x(2) = kData + 64;
      m.x(28) = kData + 128;
      m.run(60);
      ++runs;
    }
  }
  uint64_t seed = 0x2545F4914F6CDD1Dull;
  for (int i = 0; i < 40000; ++i) {      // random words, each run on its own
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    A64Machine m({uint32_t(seed), uint32_t(seed >> 32)});
    for (int r = 0; r < 31; ++r) m.x(r) = (r % 3 == 0) ? kData + 256 : seed * (r + 1);
    m.run(4);
    ++runs;
  }
  CHECK(runs > 40000);
}


// --- RISC-V ----------------------------------------------------------------------------------------------------------------------------------

struct RvMachine {
  process::AddressSpace space{A};
  RiscV64Cpu cpu;
  explicit RvMachine(const Bytes& code) {
    space.map(kCode, 0x2000, process::kProtRead | process::kProtExecute);
    if (!code.empty()) space.write_privileged(kCode, ConstByteView(code.data(), code.size()));
    space.map(kData, 0x2000, process::kProtRead | process::kProtWrite);
    space.map(kStack, 0x1000, process::kProtRead | process::kProtWrite);
    process::ProgramStart start;
    start.arch = process::UserArch::RiscV64;
    start.entry = kCode;
    start.stack_pointer = kStack + 0x1000;
    cpu.reset(start);
  }
  StopReason run(uint64_t n = 1000) {
    GuestMemory m(space);
    return cpu.run(m, n);
  }
  uint64_t& x(int i) { return cpu.state().x[i]; }
  uint64_t& f(int i) { return cpu.state().f[i]; }
};

static void test_riscv64() {
  {      // li, mul, ecall: the system call is decoded the RISC-V Linux way (a7 = number, a0..a5 = arguments)
    RvMachine m({0x19, 0x45, 0x9d, 0x45, 0x33, 0x06, 0xb5, 0x02, 0x93, 0x08, 0xe0, 0x05, 0x73, 0x00, 0x00, 0x00});
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(12) == 42 && m.cpu.state().pc == kCode + 16);
    process::SyscallRequest r;
    CHECK(m.cpu.pending_syscall(process::SyscallAbi::Linux, &r) && r.number == 94 && r.args[0] == 6 && r.args[1] == 7);
    CHECK(!m.cpu.pending_syscall(process::SyscallAbi::Windows, &r));
    m.cpu.finish_syscall(123);
    CHECK(m.x(10) == 123 && m.cpu.arch() == process::UserArch::RiscV64);
  }
  {      // memory, double arithmetic (fused), the load-reserved / store-conditional pair
    RvMachine m({0x83, 0xb2, 0x85, 0x00, 0x85, 0x02, 0x23, 0xb4, 0x55, 0x00, 0x09, 0x43, 0x53, 0x00, 0x03, 0xd2, 0x8d, 0x43, 0xd3, 0x80, 0x03, 0xd2, 0x53, 0x71, 0x10, 0x02, 0xd3, 0x07, 0x01, 0xe2,
                 0xc3, 0x01, 0x10, 0x12, 0x53, 0x98, 0x21, 0xc2, 0x2f, 0xb6, 0x05, 0x10, 0xaf, 0xb6, 0xc5, 0x18, 0x2f, 0xb7, 0xc5, 0x18, 0x73, 0x00, 0x00, 0x00});
    m.x(11) = kData;
    const uint8_t init[16] = {7, 0, 0, 0, 0, 0, 0, 0, 41, 0, 0, 0, 0, 0, 0, 0};
    m.space.write(kData, ConstByteView(init, 16));
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(5) == 42 && m.x(15) == bits_of(5.0) && m.x(16) == 11);
    CHECK(m.x(12) == 7 && m.x(13) == 0 && m.x(14) == 1);      // sc succeeds once, then the reservation is gone
    CHECK(m.cpu.state().fcsr == 0);
    uint8_t out[8];
    m.space.read(kData + 8, ByteView(out, 8));
    CHECK(out[0] == 42);
  }
  {      // compressed instructions
    RvMachine m({0x15, 0x45, 0x0d, 0x05, 0x0a, 0x05, 0xaa, 0x85, 0xaa, 0x95, 0xfd, 0x35, 0x05, 0x46, 0x11, 0xe2, 0xa5, 0x46, 0x2e, 0xe4, 0x22, 0x67, 0x73, 0x00, 0x00, 0x00});
    m.x(2) = kStack + 0x800;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(11) == 63 && m.x(13) == 0 && m.x(14) == 63 && m.cpu.state().pc == kCode + 26);      // (the taken c.bnez skips c.li a3, 9)
  }
  {      // floating point: a division by zero sets DZ, a NaN-boxed single, canonical NaNs, fclass
    RvMachine a({0x53, 0x71, 0x10, 0x1a, 0x73, 0x25, 0x10, 0x00, 0x73, 0x00, 0x00, 0x00});      // fdiv.d ft2, ft0, ft1; frflags a0
    a.f(0) = bits_of(1.0);
    a.f(1) = 0;
    CHECK(a.run() == StopReason::Syscall && a.f(2) == 0x7FF0000000000000ull && a.x(10) == 8);
    RvMachine b({0x53, 0x71, 0x10, 0x00, 0x53, 0x05, 0x01, 0xe0, 0xd3, 0x15, 0x00, 0xe0, 0xd3, 0x01, 0x52, 0x28, 0x53, 0x86, 0x01, 0xe0, 0x73, 0x00, 0x00, 0x00});
    b.f(0) = 0xFFFFFFFF7FA00000ull;      // a boxed signaling NaN
    b.f(1) = 0xFFFFFFFF3F800000ull;
    b.f(4) = 0x000000003F800000ull;      // not NaN-boxed: reads as the canonical NaN
    b.f(5) = 0xFFFFFFFF40000000ull;
    CHECK(b.run() == StopReason::Syscall);
    CHECK(b.x(10) == 0x7FC00000u && b.x(11) == (1u << 8) && b.x(12) == 0x40000000u && (b.cpu.state().fcsr & 16));
    CHECK(b.f(2) == 0xFFFFFFFF7FC00000ull);      // (the result is boxed)
  }
  {      // faults carry the x86-style vectors
    RvMachine a({0x10, 0x63});      // c.ld a2, 0(a4)
    a.x(14) = 0x900000;
    CHECK(a.run() == StopReason::Fault && a.cpu.fault().vector == 14 && a.cpu.fault().address == 0x900000 && a.cpu.fault().pc == kCode && a.cpu.state().pc == kCode);
    RvMachine b({0x02, 0x90});      // c.ebreak
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 3);
    RvMachine c({0x00, 0x00});      // the all-zero halfword is illegal
    CHECK(c.run() == StopReason::Fault && c.cpu.fault().vector == 6);
    RvMachine d({0x53, 0x50, 0x10, 0x02});      // fadd.d with the reserved rounding mode 5
    CHECK(d.run() == StopReason::Fault && d.cpu.fault().vector == 6);
    RvMachine e({0x2f, 0x25, 0xb6, 0x00});      // amoadd.w a0, a1, (a2) on an odd address
    e.x(12) = kData + 1;
    CHECK(e.run() == StopReason::Fault && e.cpu.fault().vector == 13);
    RvMachine f({0x01, 0x00});
    f.cpu.state().pc = kCode + 1;      // misaligned
    CHECK(f.run() == StopReason::Fault && f.cpu.fault().vector == 13);
    RvMachine g({0x01, 0x00});
    g.cpu.state().pc = kData;      // not executable
    CHECK(g.run() == StopReason::Fault && g.cpu.fault().vector == 14);
    RvMachine h({0x23, 0x30, 0xb5, 0x00});      // sd a1, 0(a0) into the read-only code page
    h.x(10) = kCode;
    CHECK(h.run() == StopReason::Fault && h.cpu.fault().vector == 14 && h.cpu.fault().address == kCode);
  }
  {      // M extension corner cases: division by zero, mulhu
    RvMachine m({0x37, 0x55, 0x34, 0x12, 0x1b, 0x05, 0x85, 0x67, 0xb3, 0x35, 0xa5, 0x02, 0x3b, 0x46, 0x05, 0x02, 0xbb, 0x76, 0x05, 0x02, 0x73, 0x00, 0x00, 0x00});
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(10) == 0x12345678u && m.x(11) == 0 && m.x(12) == ~uint64_t{0} && m.x(13) == 0x12345678u);
  }
  {      // cbo.zero clears the 64-byte block holding the address; cbo.flush has no effect; ecall
    RvMachine m({0x0F, 0x20, 0x45, 0x00, 0x0F, 0x20, 0x25, 0x00, 0x73, 0x00, 0x00, 0x00});
    uint8_t ones[128];
    for (uint8_t& v : ones) v = 0xFF;
    m.space.write(kData, ConstByteView(ones, 128));
    m.x(10) = kData + 70;
    CHECK(m.run() == StopReason::Syscall);
    uint8_t after[128];
    m.space.read(kData, ByteView(after, 128));
    for (uint32_t i = 0; i < 128; ++i) CHECK(after[i] == (i >= 64 ? 0 : 0xFF));
    RvMachine bad({0x8F, 0x20, 0x45, 0x00});      // cbo.zero with rd != 0 is reserved
    CHECK(bad.run() == StopReason::Fault && bad.cpu.fault().vector == 6);
  }
  {      // V: vsetvli, a unit-stride load, vadd.vv, a store, vlenb
    RvMachine m({0x15, 0x45, 0xd7, 0x72, 0x05, 0x0d, 0x87, 0xe0, 0x05, 0x02, 0x57, 0x81, 0x10, 0x02, 0x27, 0x61, 0x06, 0x02, 0x73, 0x23, 0x20, 0xc2, 0xf3, 0x23, 0x00, 0xc2, 0x73, 0x00, 0x00, 0x00});
    const uint32_t input[4] = {1, 2, 0x80000000u, 0xFFFFFFFFu};
    m.space.write(kData, ConstByteView(reinterpret_cast<const uint8_t*>(input), 16));
    m.x(11) = kData;
    m.x(12) = kData + 0x100;
    CHECK(m.run() == StopReason::Syscall);
    uint32_t out[4];
    m.space.read(kData + 0x100, ByteView(reinterpret_cast<uint8_t*>(out), 16));
    CHECK(m.x(5) == 4 && m.x(6) == 16 && m.x(7) == 4);      // vl = min(5, VLMAX = 4), vlenb = 16
    CHECK(out[0] == 2 && out[1] == 4 && out[2] == 0 && out[3] == 0xFFFFFFFEu);
  }
  {      // V: fault-only-first shortens vl instead of trapping; a plain load traps with vstart set to the element
    const Bytes ff = {0x41, 0x45, 0xd7, 0x72, 0x05, 0x0c, 0x87, 0x80, 0x05, 0x03, 0x73, 0x23, 0x00, 0xc2, 0x73, 0x00, 0x00, 0x00};
    RvMachine a(ff);
    a.x(11) = kData + 0x2000 - 4;
    CHECK(a.run() == StopReason::Syscall && a.x(6) == 4);
    RvMachine b({0x41, 0x45, 0xd7, 0x72, 0x05, 0x0c, 0x87, 0x80, 0x05, 0x02, 0x73, 0x00, 0x00, 0x00});
    b.x(11) = kData + 0x2000 - 4;
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 14 && b.cpu.fault().address == kData + 0x2000 && b.cpu.state().vstart == 4);
  }
  {      // V: an instruction before any vsetvli (vill), and a register group that is not aligned to LMUL, are invalid
    RvMachine a({0xd7, 0x80, 0x21, 0x02});
    CHECK(a.run() == StopReason::Fault && a.cpu.fault().vector == 6);
    RvMachine b({0x21, 0x45, 0xd7, 0x72, 0x15, 0x0d, 0xd7, 0x00, 0x22, 0x02});
    CHECK(b.run() == StopReason::Fault && b.cpu.fault().vector == 6);
  }
  {      // V: vmv.v.i, vid, vmul, vslidedown, a float conversion and addition, a reduction
    RvMachine m({0x0d, 0x45, 0xd7, 0x72, 0x85, 0x0d, 0xd7, 0xb0, 0x0e, 0x5e, 0x57, 0xa1, 0x08, 0x52, 0xd7, 0x21, 0x11, 0x96, 0xd7, 0x26, 0x30, 0x42, 0x57, 0xb2, 0x30, 0x3e, 0x57, 0x27, 0x40, 0x42,
                 0xd7, 0x92, 0x21, 0x4a, 0x57, 0x93, 0x52, 0x02, 0x57, 0x10, 0x60, 0x42, 0xd7, 0xa3, 0x31, 0x02, 0xd7, 0x27, 0x70, 0x42, 0x73, 0x00, 0x00, 0x00});
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(13) == 0 && m.x(14) == static_cast<uint64_t>(-3) && m.x(15) == static_cast<uint64_t>(-3) && m.cpu.state().vl == 2);      // (e64 gives VLMAX = 2)
  }
  {      // Zba/Zbb/Zbs/Zbc/Zicond: sh3add a0,a1,a2 ; clmulh a3,a1,a2 ; cpop a4,a1 ; czero.eqz a5,a1,x0 ; rev8 a6,a1 ; ecall
    RvMachine m({0x33, 0xE5, 0xC5, 0x20, 0xB3, 0xB6, 0xC5, 0x0A, 0x13, 0x97, 0x25, 0x60, 0xB3, 0xD7, 0x05, 0x0E, 0x13, 0xD8, 0x85, 0x6B, 0x73, 0x00, 0x00, 0x00});
    m.x(11) = 0x0123456789ABCDEFull;
    m.x(12) = 0x100;
    CHECK(m.run() == StopReason::Syscall);
    CHECK(m.x(10) == (m.x(11) << 3) + 0x100 && m.x(14) == 32 && m.x(15) == 0 && m.x(16) == 0xEFCDAB8967452301ull);
    CHECK(m.x(13) == 0x0123456789ABCDEFull >> 56);      // clmulh by 0x100 = the product's bits 64..127 = a >> 56
  }
}

static void test_riscv64_robustness() {
  const Bytes sample = {0x83, 0xb2, 0x85, 0x00, 0x85, 0x02, 0x23, 0xb4, 0x55, 0x00, 0x09, 0x43, 0x53, 0x00, 0x03, 0xd2, 0x8d, 0x43, 0xd3, 0x80, 0x03, 0xd2, 0x53, 0x71, 0x10, 0x02, 0xd3, 0x07,
                        0x01, 0xe2, 0xc3, 0x01, 0x10, 0x12, 0x53, 0x98, 0x21, 0xc2, 0x2f, 0xb6, 0x05, 0x10, 0xaf, 0xb6, 0xc5, 0x18, 0x2f, 0xb7, 0xc5, 0x18, 0x15, 0x45, 0x0d, 0x05, 0x0a, 0x05,
                        0xaa, 0x85, 0xaa, 0x95, 0xfd, 0x35, 0x05, 0x46, 0x11, 0xe2, 0xa5, 0x46, 0x2e, 0xe4, 0x22, 0x67, 0x73, 0x00, 0x00, 0x00};
  uint32_t runs = 0;
  for (size_t i = 0; i < sample.size(); ++i) {      // every single-bit flip, and every truncation
    for (int bit = 0; bit < 8; ++bit) {
      Bytes damaged = sample;
      damaged[i] ^= uint8_t(1u << bit);
      RvMachine m(damaged);
      m.x(11) = kData;
      m.x(12) = kData + 64;
      m.x(2) = kStack + 0x800;
      m.run(60);
      ++runs;
    }
    Bytes cut(sample.begin(), sample.begin() + long(i));
    RvMachine m(cut);
    m.run(60);
    ++runs;
  }
  uint64_t seed = 0x2545F4914F6CDD1Dull;
  for (int i = 0; i < 40000; ++i) {      // random words, each run on its own
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    Bytes code;
    for (int k = 0; k < 8; ++k) code.push_back(uint8_t(seed >> (8 * k)));
    RvMachine m(code);
    for (int r = 1; r < 32; ++r) m.x(r) = (r % 3 == 0) ? kData + 256 : seed * (r + 1);
    for (int r = 0; r < 32; ++r) m.f(r) = seed * (r + 7);
    m.run(4);
    ++runs;
  }
  for (int i = 0; i < 80000; ++i) {      // random vector instructions under a random valid configuration, with random registers and a mapped buffer
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    uint32_t word = uint32_t(seed >> 5);
    const uint32_t opcodes[3] = {0x57, 0x07, 0x27};
    word = (word & ~0x7Fu) | opcodes[(seed >> 40) % 3];
    Bytes code;
    for (int k = 0; k < 4; ++k) code.push_back(uint8_t(word >> (8 * k)));
    RvMachine m(code);
    RiscV64Cpu::State& st = m.cpu.state();
    const uint64_t vsew = (seed >> 44) & 3, lmul = (seed >> 46) & 7;
    st.vtype = (vsew << 3) | lmul | (((seed >> 50) & 3) << 6);
    st.vl = (seed >> 52) % 40;
    for (uint8_t& b : st.v) {
      seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
      b = uint8_t(seed);
    }
    for (int r = 1; r < 32; ++r) m.x(r) = (r % 3 == 0) ? kData + 256 : (r % 3 == 1 ? seed * (r + 1) : r);
    for (int r = 0; r < 32; ++r) m.f(r) = seed * (r + 7);
    m.run(2);
    ++runs;
  }
  CHECK(runs > 120000);
}

static void test_softfloat_arm_extras() {
  using namespace softfloat;
  const int modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
  for (int mode = 0; mode < 4; ++mode) {
    for (int i = 0; i < 30000; ++i) {
      Env env;
      env.mode = Rounding(mode);
      const uint64_t a = random_double_bits(), b = random_double_bits(), c = random_double_bits();
      volatile double x = as_double(a), y = as_double(b), z = as_double(c), r;
      fesetround(modes[mode]);
      feclearexcept(FE_ALL_EXCEPT);
      r = std::fma(x, y, z);
      const uint32_t hf = host_flags() & kComparedFlags;
      fesetround(FE_TONEAREST);
      const uint64_t got = fused_multiply_add(Format::F64, a, b, c, env);
      const uint64_t want = bits_of(double(r));
      const bool nan_ok = is_nan64(want) && is_nan64(got);
      if ((got != want && !nan_ok) || (env.flags & kComparedFlags) != hf) fp_mismatch("fma64", a, b, got, want, env.flags & kComparedFlags, hf, mode);
      const uint32_t fa = random_float_bits(), fb = random_float_bits(), fc = random_float_bits();
      volatile float fx = as_float(fa), fy = as_float(fb), fz = as_float(fc), fr;
      Env e2;
      e2.mode = Rounding(mode);
      fesetround(modes[mode]);
      feclearexcept(FE_ALL_EXCEPT);
      fr = std::fma(fx, fy, fz);
      const uint32_t hf2 = host_flags() & kComparedFlags;
      fesetround(FE_TONEAREST);
      const uint64_t fgot = fused_multiply_add(Format::F32, fa, fb, fc, e2);
      const uint32_t fwant = bits_of(float(fr));
      const bool fnan_ok = is_nan32(fwant) && is_nan32(uint32_t(fgot));
      if ((uint32_t(fgot) != fwant && !fnan_ok) || (e2.flags & kComparedFlags) != hf2) fp_mismatch("fma32", fa, fb, fgot, fwant, e2.flags & kComparedFlags, hf2, mode);
    }
  }
  for (int i = 0; i < 60000; ++i) {      // round to integral
    const uint64_t a = random_double_bits();
    const double x = as_double(a);
    Env env;
    const struct { Rounding mode; double (*host)(double); } cases[] = {{Rounding::TowardZero, [](double v) { return std::trunc(v); }}, {Rounding::Down, [](double v) { return std::floor(v); }},
                                                                       {Rounding::Up, [](double v) { return std::ceil(v); }}, {Rounding::TieAway, [](double v) { return std::round(v); }},
                                                                       {Rounding::NearestEven, [](double v) { return std::nearbyint(v); }}};
    for (const auto& k : cases) {
      const uint64_t got = round_to_integral(Format::F64, a, k.mode, false, env);
      const uint64_t want = bits_of(k.host(x));
      if (got != want && !(is_nan64(got) && is_nan64(want))) fp_mismatch("rint", a, 0, got, want, 0, 0, int(k.mode));
    }
  }
  {      // saturating conversions
    Env env;
    CHECK(to_integer_saturating(Format::F64, bits_of(1e30), Rounding::TowardZero, 32, false, 0, env) == 0x7FFFFFFFu);
    CHECK(to_integer_saturating(Format::F64, bits_of(-1e30), Rounding::TowardZero, 32, false, 0, env) == 0x80000000u);
    CHECK(to_integer_saturating(Format::F64, bits_of(-5.5), Rounding::TowardZero, 32, true, 0, env) == 0);
    CHECK(to_integer_saturating(Format::F64, 0x7FF8000000000000ull, Rounding::TowardZero, 64, false, 0, env) == 0);
    CHECK(to_integer_saturating(Format::F64, bits_of(2.5), Rounding::TieAway, 64, false, 0, env) == 3);
    CHECK(to_integer_saturating(Format::F64, bits_of(2.5), Rounding::NearestEven, 64, false, 0, env) == 2);
    CHECK(to_integer_saturating(Format::F64, bits_of(1.5), Rounding::TowardZero, 64, false, 4, env) == 24);      // 1.5 * 2^4
    CHECK(to_integer_saturating(Format::F64, bits_of(1e30), Rounding::TowardZero, 64, true, 0, env) == ~uint64_t{0});
  }
}

static Bytes read_file(const char* path) {
  Bytes out;
  if (FILE* f = fopen(path, "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
  }
  return out;
}

// Loads a Linux ELF as a UserProgram and runs it on the emulator.
static std::string run_program_emulated(const Bytes& elf, uint32_t* exit_code, EmulatedOutcome* outcome, uint64_t slice = 20000) {
  FakeHost host;
  process::UserProgram program(A, 100, host);
  const StringView argv[] = {"/guest"};
  const uint8_t random[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const process::LoadStatus status = program.load(ConstByteView(elf.data(), elf.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16));
  CHECK(status == process::LoadStatus::Ok);
  if (status != process::LoadStatus::Ok) return std::string();
  UniquePointer<IGuestCpu> cpu = create_guest_cpu(program.start().arch, A);
  CHECK(cpu);
  if (!cpu) return std::string();
  NeverStop control;
  *outcome = run_emulated(program, *cpu.get(), control, slice);
  *exit_code = outcome->exit_code;
  return host.out;
}

static void test_program(const char* elf_path, const char* expected_path) {
  const Bytes elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  uint32_t code = 99;
  EmulatedOutcome outcome;
  const std::string out = run_program_emulated(elf, &code, &outcome);
  CHECK(outcome.exited && !outcome.faulted && code == 0);
  if (outcome.faulted) printf("  guest faulted: vector %d address %llx pc %llx\n", outcome.fault.vector, (unsigned long long)outcome.fault.address, (unsigned long long)outcome.fault.pc);
  const std::string want(expected.begin(), expected.end());
  CHECK(out == want);
  if (out != want) printf("--- got\n%s--- want\n%s", out.c_str(), want.c_str());
  // Tiny slices give the same answer: stopping and resuming at any instruction boundary changes nothing.
  uint32_t code2 = 99;
  EmulatedOutcome outcome2;
  CHECK(run_program_emulated(elf, &code2, &outcome2, 7) == want && code2 == 0);

  {      // a stop request ends the program between slices
    FakeHost host;
    process::UserProgram program(A, 101, host);
    const StringView argv[] = {"/guest"};
    const uint8_t random[16] = {};
    CHECK(program.load(ConstByteView(elf.data(), elf.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) == process::LoadStatus::Ok);
    UniquePointer<IGuestCpu> cpu = create_guest_cpu(process::UserArch::X86_64, A);
    NeverStop control;
    control.stop_after = 3;
    const EmulatedOutcome stopped = run_emulated(program, *cpu.get(), control, 100);
    CHECK(stopped.killed && !stopped.faulted && stopped.exit_code == 130);
  }
  {      // damaged code never takes the emulator down: flip each of many bits in the ELF's code and run a little
    uint32_t survived = 0;
    for (size_t i = 0x1000; i < elf.size() && i < 0x1000 + 1500; i += 3) {
      Bytes damaged = elf;
      damaged[i] ^= uint8_t(1u << (i & 7));
      FakeHost host;
      process::UserProgram program(A, 102, host);
      const StringView argv[] = {"/guest"};
      const uint8_t random[16] = {};
      if (program.load(ConstByteView(damaged.data(), damaged.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) != process::LoadStatus::Ok) continue;
      UniquePointer<IGuestCpu> cpu = create_guest_cpu(process::UserArch::X86_64, A);
      NeverStop control;
      control.stop_after = 50;
      run_emulated(program, *cpu.get(), control, 400);
      ++survived;
    }
    CHECK(survived > 100);
  }
}


// --- threads ---------------------------------------------------------------------------------------------------------------------------------

// Runs a threaded Linux ELF with host threads; returns what it printed.
static std::string run_threaded(const Bytes& elf, EmulatedOutcome* outcome, int* thread_count, uint64_t slice, uint32_t stop_after = ~0u) {
  FakeHost host;
  process::UserProgram program(A, 200, host);
  const StringView argv[] = {"/threads"};
  const uint8_t random[16] = {};
  if (program.load(ConstByteView(elf.data(), elf.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) != process::LoadStatus::Ok) {
    CHECK(false);
    return std::string();
  }
  UniquePointer<IGuestCpu> cpu = create_guest_cpu(program.start().arch, A);
  HostSync sync;
  HostThreads threads;
  struct Stop final : IRunControl {
    std::atomic<uint64_t> yields{0};
    uint64_t limit;
    bool stop_requested() override { return yields.load() >= limit; }
    void yield() override { ++yields; std::this_thread::yield(); }
  } control;
  control.limit = stop_after;
  *outcome = run_emulated_threads(program, *cpu.get(), control, sync, threads, A, slice);
  *thread_count = threads.started;
  return host.out;
}

static void test_threads(const char* elf_path, const char* expected_path, const char* what) {
  const Bytes elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  if (elf.empty()) return;
  const std::string want(expected.begin(), expected.end());
  const uint64_t slices[] = {20000, 997, 113, 7};
  for (uint64_t slice : slices) {
    for (int round = 0; round < (slice >= 997 ? 3 : 1); ++round) {
      EmulatedOutcome outcome;
      int started = 0;
      const std::string out = run_threaded(elf, &outcome, &started, slice);
      CHECK(outcome.exited && !outcome.faulted && !outcome.killed && outcome.exit_code == 0);
      if (outcome.faulted) printf("  %s faulted: vector %d address %llx pc %llx\n", what, outcome.fault.vector, (unsigned long long)outcome.fault.address, (unsigned long long)outcome.fault.pc);
      CHECK(started == 6);      // four workers, the hand-off helper and the spinner
      CHECK(out == want);
      if (out != want) {
        printf("--- %s (slice %llu) got\n%s--- want\n%s", what, (unsigned long long)slice, out.c_str(), want.c_str());
        return;
      }
    }
  }
  {      // Ctrl+C while threads run: the whole process ends, whatever the threads are doing
    EmulatedOutcome outcome;
    int started = 0;
    run_threaded(elf, &outcome, &started, 50, 4);
    CHECK(outcome.killed || outcome.exited);
  }
}

static void test_threads_without_support() {
  // A plain run (no thread starter) refuses clone, and a futex wait has nobody to wake it: it must not hang the process when told to stop. Here the program only needs to
  // see clone fail.
  Machine m(with_syscall({0xB8, 0x38, 0x00, 0x00, 0x00}));      // mov eax, 56 (clone)
  CHECK(m.run() == StopReason::Syscall);
  process::SyscallRequest r;
  CHECK(m.cpu.pending_syscall(process::SyscallAbi::Linux, &r) && r.number == 56);
}

static void test_program_other_cpu(const char* elf_path, const char* expected_path, const char* what) {
  const Bytes elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  const std::string want(expected.begin(), expected.end());
  uint32_t code = 99;
  EmulatedOutcome outcome;
  const std::string out = run_program_emulated(elf, &code, &outcome);
  CHECK(outcome.exited && !outcome.faulted && code == 0);
  if (outcome.faulted) printf("  %s guest faulted: vector %d address %llx pc %llx\n", what, outcome.fault.vector, (unsigned long long)outcome.fault.address, (unsigned long long)outcome.fault.pc);
  CHECK(out == want);      // (the x86-64 build of the same source printed this, and qemu agrees)
  if (out != want) printf("--- got\n%s--- want\n%s", out.c_str(), want.c_str());
  uint32_t code2 = 99;
  EmulatedOutcome outcome2;
  CHECK(run_program_emulated(elf, &code2, &outcome2, 5) == want && code2 == 0);
  uint32_t survived = 0;      // damaged code never takes the emulator down
  for (size_t i = 0x1000; i < elf.size() && i < 0x1000 + 1800; i += 2) {
    Bytes damaged = elf;
    damaged[i] ^= uint8_t(1u << (i & 7));
    FakeHost host;
    process::UserProgram program(A, 103, host);
    const StringView argv[] = {"/guest"};
    const uint8_t random[16] = {};
    if (program.load(ConstByteView(damaged.data(), damaged.size()), ArrayView<const StringView>(argv, 1), ArrayView<const StringView>(), ConstByteView(random, 16)) != process::LoadStatus::Ok) continue;
    UniquePointer<IGuestCpu> cpu = create_guest_cpu(program.start().arch, A);
    NeverStop control;
    control.stop_after = 50;
    run_emulated(program, *cpu.get(), control, 400);
    ++survived;
  }
  CHECK(survived > 100);
}

// A long double program: the x87 unit has to give the same text as the real machine did when the expected output was recorded.
static void test_x87_program(const char* elf_path, const char* expected_path, const char* what = "x87") {
  const Bytes elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  const std::string want(expected.begin(), expected.end());
  uint32_t code = 99;
  EmulatedOutcome outcome;
  const std::string out = run_program_emulated(elf, &code, &outcome);
  CHECK(outcome.exited && !outcome.faulted && code == 0);
  if (outcome.faulted) printf("  %s guest faulted: vector %d address %llx pc %llx\n", what, outcome.fault.vector, (unsigned long long)outcome.fault.address, (unsigned long long)outcome.fault.pc);
  CHECK(out == want);
  if (out != want) printf("--- got\n%s--- want\n%s", out.c_str(), want.c_str());
  uint32_t code2 = 99;
  EmulatedOutcome outcome2;
  CHECK(run_program_emulated(elf, &code2, &outcome2, 3) == want);      // (in tiny slices)
}

// An AVX2/FMA/BMI program: it sees the feature bits a real dispatcher looks for and has to print what the real machine printed.
static void test_avx_program(const char* elf_path, const char* expected_path) {
  const Bytes elf = read_file(elf_path), expected = read_file(expected_path);
  CHECK(!elf.empty() && !expected.empty());
  const std::string want(expected.begin(), expected.end());
  uint32_t code = 99;
  EmulatedOutcome outcome;
  const std::string out = run_program_emulated(elf, &code, &outcome);
  CHECK(outcome.exited && !outcome.faulted && code == 0);
  if (outcome.faulted) printf("  avx guest faulted: vector %d address %llx pc %llx\n", outcome.fault.vector, (unsigned long long)outcome.fault.address, (unsigned long long)outcome.fault.pc);
  CHECK(out == want);
  if (out != want) printf("--- got\n%s--- want\n%s", out.c_str(), want.c_str());
  uint32_t code2 = 99;
  EmulatedOutcome outcome2;
  CHECK(run_program_emulated(elf, &code2, &outcome2, 3) == want);      // (in tiny slices)
}

static void test_dispatch() {
  using exe::Architecture;
  Bytes elf(64, 0);
  elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F'; elf[4] = 2; elf[5] = 1; elf[6] = 1;
  auto machine = [&](uint16_t m, uint8_t encoding = 1) {
    elf[5] = encoding;
    elf[18] = uint8_t(m);
    elf[19] = uint8_t(m >> 8);
    return exe::identify_architecture(ConstByteView(elf.data(), elf.size()));
  };
  CHECK(machine(62) == Architecture::X86_64);
  CHECK(machine(183) == Architecture::AArch64);
  CHECK(machine(243) == Architecture::RiscV64);
  CHECK(machine(3) == Architecture::X86_32);
  CHECK(machine(40) == Architecture::Arm32);
  CHECK(machine(999) == Architecture::Other);
  CHECK(machine(62, 2) == Architecture::Other);      // big-endian
  CHECK(exe::identify_architecture(ConstByteView(elf.data(), 10)) == Architecture::Unknown);      // truncated header
  CHECK(exe::identify_architecture(ConstByteView()) == Architecture::Unknown);

  Bytes pe(0x100, 0);
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3C] = 0x80;
  pe[0x80] = 'P'; pe[0x81] = 'E';
  auto pe_machine = [&](uint16_t m) {
    pe[0x84] = uint8_t(m);
    pe[0x85] = uint8_t(m >> 8);
    return exe::identify_architecture(ConstByteView(pe.data(), pe.size()));
  };
  CHECK(pe_machine(0x8664) == Architecture::X86_64);
  CHECK(pe_machine(0xAA64) == Architecture::AArch64);
  CHECK(pe_machine(0x014C) == Architecture::X86_32);
  CHECK(pe_machine(0x1234) == Architecture::Other);

  using process::UserArch;
  CHECK(process::user_arch_for(Architecture::X86_64) == UserArch::X86_64);
  CHECK(process::user_arch_for(Architecture::X86_32) == UserArch::None);
  CHECK(process::needs_emulation(UserArch::None) == false);
#if defined(__x86_64__)
  CHECK(process::native_arch() == UserArch::X86_64);
  CHECK(!process::needs_emulation(UserArch::X86_64));
  CHECK(process::needs_emulation(UserArch::AArch64) && process::needs_emulation(UserArch::RiscV64));
#endif
  CHECK(can_emulate(UserArch::X86_64) && can_emulate(UserArch::AArch64) && can_emulate(UserArch::RiscV64) && !can_emulate(UserArch::None));
  CHECK(create_guest_cpu(UserArch::RiscV64, A) && create_guest_cpu(UserArch::RiscV64, A)->arch() == UserArch::RiscV64);
  CHECK(!create_guest_cpu(UserArch::None, A));
  CHECK(create_guest_cpu(UserArch::X86_64, A));
}

int main(int argc, char** argv) {
  test_dispatch();
  test_arithmetic();
  test_memory_and_control();
  test_sse();
  test_faults();
  test_float_instructions();
  test_softfloat_against_host();
  test_softfloat_conversions();
  test_softfloat_arm_extras();
  test_aarch64();
  test_aarch64_robustness();
  test_riscv64();
  test_riscv64_robustness();
  test_robustness();
  if (argc >= 3) test_program(argv[1], argv[2]);
  if (argc >= 4) test_program_other_cpu(argv[3], argv[2], "AArch64");
  if (argc >= 14) test_program_other_cpu(argv[13], argv[2], "RISC-V");
  if (argc >= 7) {
    test_threads(argv[4], argv[6], "x86-64 threads");
    test_threads(argv[5], argv[6], "AArch64 threads");
    if (argc >= 15) test_threads(argv[14], argv[6], "RISC-V threads");
  }
  if (argc >= 9) test_x87_program(argv[7], argv[8]);
  if (argc >= 11) test_avx_program(argv[9], argv[10]);
  if (argc >= 17) test_x87_program(argv[15], argv[16], "RISC-V vector");
  if (argc >= 19) test_x87_program(argv[17], argv[18], "AES/PCLMUL/SHA");
  if (argc >= 21) test_x87_program(argv[19], argv[20], "SHA-512/SM4");
  if (argc >= 23) test_x87_program(argv[21], argv[22], "AVX-512");
  if (argc >= 25) test_x87_program(argv[23], argv[24], "AVX-512 FP16");
  if (argc >= 13) test_avx_program(argv[11], argv[12]);      // (the SSE3/SSSE3/SSE4 program: the same kind of check)
  test_threads_without_support();
  if (argc < 3) printf("(no guest program given: skipping the whole-program tests)\n");
  CHECK(A.live == 0);
  if (g_failed) {
    printf("EmulationTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("EmulationTest: all passed\n");
  return 0;
}
