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
// Host tests for src/Jit: the code buffer and its fixups, the three assemblers (checked against bytes produced by llvm-mc), the IR validator and
// interpreter, the x86-64 back end run natively and compared with the interpreter on random programs, the RCU-published code cache (with readers
// running compiled code while a writer replaces, invalidates and flushes it), the hotness tracker, and the dispatcher end to end.
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>
#include "Jit/Backend/X86_64Backend.hpp"
#include "Jit/Emitter/AArch64Assembler.hpp"
#include "Jit/Emitter/RiscV64Assembler.hpp"
#include "Jit/Emitter/X86_64Assembler.hpp"
#include "Jit/Ir/RegisterAllocator.hpp"
#include "Jit/JitCompiler.hpp"
#include "tests/JitHost.hpp"
#include "tests/JitRandom.hpp"

using namespace jit;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

// Hands out a core number per thread, so readers spread over the RCU shards.
namespace {
std::atomic<uint32_t> g_next_core{1};
thread_local uint32_t t_core = ~uint32_t{0};
}
uint32_t kernel::current_core() {
  if (t_core == ~uint32_t{0}) t_core = g_next_core++;
  return t_core;
}

namespace {

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int64_t> live{0};
  std::atomic<int64_t> fail_after{-1};
  bool should_fail() {
    int64_t f = fail_after.load();
    if (f == 0) return true;
    if (f > 0) fail_after.fetch_sub(1);
    return false;
  }
  void* allocate(uint64_t size) override {
    if (size == 0 || should_fail()) return nullptr;
    ++live;
    return aligned_alloc(64, (size + 63) / 64 * 64);
  }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (size == 0 || should_fail()) return nullptr;
    ++live;
    const uint64_t align = alignment < 64 ? 64 : alignment;
    return aligned_alloc(align, (size + align - 1) / align * align);
  }
  void deallocate(void* p) override {
    if (!p) return;
    --live;
    free(p);
  }
};

std::vector<uint8_t> bytes_of_buffer(const CodeBuffer& b) { return std::vector<uint8_t>(b.view().begin(), b.view().end()); }
using Bytes = std::vector<uint8_t>;

// ---- assemblers ----

void test_x86_encodings() {
  TestAllocator a;
  struct Case { void (*emit)(X86_64Assembler&); Bytes want; };
  using R = X86Reg;
  const Case cases[] = {
      {[](X86_64Assembler& s) { s.mov_rr(R::Rax, R::Rcx); }, {0x48, 0x89, 0xc8}},
      {[](X86_64Assembler& s) { s.mov_rr(R::R8, R::R9); }, {0x4d, 0x89, 0xc8}},
      {[](X86_64Assembler& s) { s.mov_ri(R::Rax, 5); }, {0xb8, 5, 0, 0, 0}},
      {[](X86_64Assembler& s) { s.mov_ri(R::R10, 5); }, {0x41, 0xba, 5, 0, 0, 0}},
      {[](X86_64Assembler& s) { s.mov_ri(R::Rax, static_cast<uint64_t>(-5)); }, {0x48, 0xc7, 0xc0, 0xfb, 0xff, 0xff, 0xff}},
      {[](X86_64Assembler& s) { s.mov_ri(R::Rax, 0x123456789aull); }, {0x48, 0xb8, 0x9a, 0x78, 0x56, 0x34, 0x12, 0, 0, 0}},
      {[](X86_64Assembler& s) { s.mov_load(R::Rax, R::Rbx, 8); }, {0x48, 0x8b, 0x43, 0x08}},
      {[](X86_64Assembler& s) { s.mov_load(R::R9, R::Rbp, -16); }, {0x4c, 0x8b, 0x4d, 0xf0}},
      {[](X86_64Assembler& s) { s.mov_load(R::Rax, R::Rsp, 0); }, {0x48, 0x8b, 0x04, 0x24}},
      {[](X86_64Assembler& s) { s.mov_load(R::Rax, R::Rbp, 0); }, {0x48, 0x8b, 0x45, 0x00}},
      {[](X86_64Assembler& s) { s.mov_load(R::Rax, R::R12, 0); }, {0x49, 0x8b, 0x04, 0x24}},
      {[](X86_64Assembler& s) { s.mov_store(R::R13, 300, R::Rax); }, {0x49, 0x89, 0x85, 0x2c, 0x01, 0, 0}},
      {[](X86_64Assembler& s) { s.lea(R::Rsi, R::Rsp, 16); }, {0x48, 0x8d, 0x74, 0x24, 0x10}},
      {[](X86_64Assembler& s) { s.add_rr(R::Rax, R::Rcx); }, {0x48, 0x01, 0xc8}},
      {[](X86_64Assembler& s) { s.sub_rr(R::Rax, R::R10); }, {0x4c, 0x29, 0xd0}},
      {[](X86_64Assembler& s) { s.and_rr(R::R11, R::Rcx); }, {0x49, 0x21, 0xcb}},
      {[](X86_64Assembler& s) { s.or_rr(R::Rax, R::Rcx); }, {0x48, 0x09, 0xc8}},
      {[](X86_64Assembler& s) { s.xor_rr(R::Rax, R::Rax); }, {0x48, 0x31, 0xc0}},
      {[](X86_64Assembler& s) { s.cmp_rr(R::Rax, R::Rcx); }, {0x48, 0x39, 0xc8}},
      {[](X86_64Assembler& s) { s.test_rr(R::Rax, R::Rax); }, {0x48, 0x85, 0xc0}},
      {[](X86_64Assembler& s) { s.add_ri(R::Rax, 100000); }, {0x48, 0x81, 0xc0, 0xa0, 0x86, 0x01, 0x00}},
      {[](X86_64Assembler& s) { s.sub_ri(R::Rsp, 8); }, {0x48, 0x81, 0xec, 0x08, 0, 0, 0}},
      {[](X86_64Assembler& s) { s.imul_rr(R::Rax, R::Rcx); }, {0x48, 0x0f, 0xaf, 0xc1}},
      {[](X86_64Assembler& s) { s.neg(R::Rax); }, {0x48, 0xf7, 0xd8}},
      {[](X86_64Assembler& s) { s.not_(R::R8); }, {0x49, 0xf7, 0xd0}},
      {[](X86_64Assembler& s) { s.shl_cl(R::Rax); }, {0x48, 0xd3, 0xe0}},
      {[](X86_64Assembler& s) { s.shr_cl(R::R9); }, {0x49, 0xd3, 0xe9}},
      {[](X86_64Assembler& s) { s.sar_cl(R::Rax); }, {0x48, 0xd3, 0xf8}},
      {[](X86_64Assembler& s) { s.shl_ri(R::Rax, 3); }, {0x48, 0xc1, 0xe0, 0x03}},
      {[](X86_64Assembler& s) { s.sar_ri(R::R11, 63); }, {0x49, 0xc1, 0xfb, 0x3f}},
      {[](X86_64Assembler& s) { s.push(R::Rbp); }, {0x55}},
      {[](X86_64Assembler& s) { s.push(R::R12); }, {0x41, 0x54}},
      {[](X86_64Assembler& s) { s.pop(R::Rbx); }, {0x5b}},
      {[](X86_64Assembler& s) { s.ret(); }, {0xc3}},
      {[](X86_64Assembler& s) { s.call_r(R::Rax); }, {0xff, 0xd0}},
      {[](X86_64Assembler& s) { s.jmp_r(R::R11); }, {0x41, 0xff, 0xe3}},
  };
  int n = 0;
  for (const Case& c : cases) {
    CodeBuffer buf(a);
    X86_64Assembler as(buf);
    c.emit(as);
    if (bytes_of_buffer(buf) != c.want) {
      printf("x86 case %d differs:", n);
      for (uint8_t b : bytes_of_buffer(buf)) printf(" %02x", b);
      printf("\n");
      ++g_failed;
    }
    ++n;
  }
  // setcc: sete al (+ movzx eax, al) and setl sil
  CodeBuffer buf(a);
  X86_64Assembler as(buf);
  as.setcc(X86Cond::E, R::Rax);
  CHECK(bytes_of_buffer(buf) == (Bytes{0x40, 0x0f, 0x94, 0xc0, 0x40, 0x0f, 0xb6, 0xc0}));
}

void test_a64_encodings() {
  TestAllocator a;
  struct Case { void (*emit)(AArch64Assembler&); Bytes want; };
  const Case cases[] = {
      {[](AArch64Assembler& s) { s.mov_imm(1, 0x1234); }, {0x81, 0x46, 0x82, 0xd2}},
      {[](AArch64Assembler& s) { s.mov_imm(1, 0x1234ull << 48); }, {0x81, 0x46, 0xe2, 0xd2}},
      {[](AArch64Assembler& s) { s.mov_imm(0, ~0ull); }, {0x00, 0x00, 0x80, 0x92}},
      {[](AArch64Assembler& s) { s.add_rr(3, 4, 5); }, {0x83, 0x00, 0x05, 0x8b}},
      {[](AArch64Assembler& s) { s.sub_rr(3, 4, 5); }, {0x83, 0x00, 0x05, 0xcb}},
      {[](AArch64Assembler& s) { s.add_ri(3, 4, 17); }, {0x83, 0x44, 0x00, 0x91}},
      {[](AArch64Assembler& s) { s.sub_ri(A64Sp, A64Sp, 32); }, {0xff, 0x83, 0x00, 0xd1}},
      {[](AArch64Assembler& s) { s.and_rr(3, 4, 5); }, {0x83, 0x00, 0x05, 0x8a}},
      {[](AArch64Assembler& s) { s.orr_rr(3, 4, 5); }, {0x83, 0x00, 0x05, 0xaa}},
      {[](AArch64Assembler& s) { s.eor_rr(3, 4, 5); }, {0x83, 0x00, 0x05, 0xca}},
      {[](AArch64Assembler& s) { s.mvn(3, 5); }, {0xe3, 0x03, 0x25, 0xaa}},
      {[](AArch64Assembler& s) { s.mul(3, 4, 5); }, {0x83, 0x7c, 0x05, 0x9b}},
      {[](AArch64Assembler& s) { s.lsl_rr(3, 4, 5); }, {0x83, 0x20, 0xc5, 0x9a}},
      {[](AArch64Assembler& s) { s.lsr_rr(3, 4, 5); }, {0x83, 0x24, 0xc5, 0x9a}},
      {[](AArch64Assembler& s) { s.asr_rr(3, 4, 5); }, {0x83, 0x28, 0xc5, 0x9a}},
      {[](AArch64Assembler& s) { s.cmp_rr(4, 5); }, {0x9f, 0x00, 0x05, 0xeb}},
      {[](AArch64Assembler& s) { s.cset(7, A64Cond::Lt); }, {0xe7, 0xa7, 0x9f, 0x9a}},
      {[](AArch64Assembler& s) { s.ldr(2, 3, 16); }, {0x62, 0x08, 0x40, 0xf9}},
      {[](AArch64Assembler& s) { s.str(2, A64Sp, 8); }, {0xe2, 0x07, 0x00, 0xf9}},
      {[](AArch64Assembler& s) { s.stp_push(A64Fp, A64Lr, 16); }, {0xfd, 0x7b, 0xbf, 0xa9}},
      {[](AArch64Assembler& s) { s.ldp_pop(A64Fp, A64Lr, 16); }, {0xfd, 0x7b, 0xc1, 0xa8}},
      {[](AArch64Assembler& s) { s.br(16); }, {0x00, 0x02, 0x1f, 0xd6}},
      {[](AArch64Assembler& s) { s.blr(8); }, {0x00, 0x01, 0x3f, 0xd6}},
      {[](AArch64Assembler& s) { s.ret(); }, {0xc0, 0x03, 0x5f, 0xd6}},
      {[](AArch64Assembler& s) { s.brk(0); }, {0x00, 0x00, 0x20, 0xd4}},
  };
  int n = 0;
  for (const Case& c : cases) {
    CodeBuffer buf(a);
    AArch64Assembler as(buf);
    c.emit(as);
    if (bytes_of_buffer(buf) != c.want) {
      printf("a64 case %d differs:", n);
      for (uint8_t b : bytes_of_buffer(buf)) printf(" %02x", b);
      printf("\n");
      ++g_failed;
    }
    ++n;
  }
  CodeBuffer buf(a);
  AArch64Assembler as(buf);
  CHECK(!as.ldr(0, 1, 4) && !as.ldr(0, 1, 32768) && buf.size() == 0);   // misaligned / out-of-range offsets emit nothing
}

// Runs MOVZ/MOVN/MOVK on the emitted words.
uint64_t eval_a64_mov(const CodeBuffer& b) {
  uint64_t x = 0;
  for (uint64_t off = 0; off < b.size(); off += 4) {
    uint32_t w = 0;
    b.read32(off, &w);
    const uint32_t hw = w >> 21 & 3, imm = w >> 5 & 0xFFFF;
    const uint64_t shifted = static_cast<uint64_t>(imm) << (16 * hw);
    switch (w & 0xFF800000u) {
      case 0xD2800000u: x = shifted; break;
      case 0x92800000u: x = ~shifted; break;
      case 0xF2800000u: x = (x & ~(0xFFFFull << (16 * hw))) | shifted; break;
      default: printf("unexpected word %08x\n", w); ++g_failed;
    }
  }
  return x;
}

// Runs LUI/ADDI/ADDIW/SLLI on the emitted words.
uint64_t eval_rv_li(const CodeBuffer& b) {
  uint64_t x = 0;
  for (uint64_t off = 0; off < b.size(); off += 4) {
    uint32_t w = 0;
    b.read32(off, &w);
    const uint64_t imm12 = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(w) >> 20));
    switch (w & 0x7F) {
      case 0x37: x = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(w & 0xFFFFF000u))); break;
      case 0x13: x = (w >> 12 & 7) == 1 ? x << (w >> 20 & 63) : ((w >> 15 & 31) == 0 ? 0 : x) + imm12; break;
      case 0x1B: x = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(x) + static_cast<uint32_t>(imm12)))); break;
      default: printf("unexpected word %08x\n", w); ++g_failed;
    }
  }
  return x;
}

void test_constants() {
  TestAllocator a;
  const uint64_t edge[] = {0, 1, 0x7FF, 0x800, 0xFFF, 0x1000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000ull, ~0ull, 0x8000000000000000ull,
                           0x7FFFFFFFFFFFFFFFull, 0xFFFF0000FFFF0000ull, 0x123456789abcdef0ull, 0xFFFFFFFFFFFFF800ull, 0xFFFFFFFF80000000ull, 0x7FFFF800ull};
  uint64_t seed = 88172645463325252ull;
  auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
  std::vector<uint64_t> values(edge, edge + sizeof(edge) / sizeof(edge[0]));
  for (int i = 0; i < 3000; ++i) {
    uint64_t v = next();
    values.push_back(v >> (next() % 64));   // varied magnitudes
    values.push_back(~(v >> (next() % 64)));
  }
  for (uint64_t v : values) {
    CodeBuffer m(a), l(a);
    AArch64Assembler(m).mov_imm(5, v);
    RiscV64Assembler(l).li(RvA0, static_cast<int64_t>(v));
    if (eval_a64_mov(m) != v) { printf("a64 mov_imm %llx\n", (unsigned long long)v); ++g_failed; break; }
    if (eval_rv_li(l) != v) { printf("rv li %llx\n", (unsigned long long)v); ++g_failed; break; }
    CHECK(m.size() <= 16);
  }
}

void test_rv_encodings() {
  TestAllocator a;
  struct Case { void (*emit)(RiscV64Assembler&); Bytes want; };
  const Case cases[] = {
      {[](RiscV64Assembler& s) { s.add(RvA0, RvA1, RvA2); }, {0x33, 0x85, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.sub(RvA0, RvA1, RvA2); }, {0x33, 0x85, 0xc5, 0x40}},
      {[](RiscV64Assembler& s) { s.sll(RvA0, RvA1, RvA2); }, {0x33, 0x95, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.slt(RvA0, RvA1, RvA2); }, {0x33, 0xa5, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.sltu(RvA0, RvA1, RvA2); }, {0x33, 0xb5, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.xor_(RvA0, RvA1, RvA2); }, {0x33, 0xc5, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.srl(RvA0, RvA1, RvA2); }, {0x33, 0xd5, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.sra(RvA0, RvA1, RvA2); }, {0x33, 0xd5, 0xc5, 0x40}},
      {[](RiscV64Assembler& s) { s.or_(RvA0, RvA1, RvA2); }, {0x33, 0xe5, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.and_(RvA0, RvA1, RvA2); }, {0x33, 0xf5, 0xc5, 0x00}},
      {[](RiscV64Assembler& s) { s.mul(RvA0, RvA1, RvA2); }, {0x33, 0x85, 0xc5, 0x02}},
      {[](RiscV64Assembler& s) { s.not_(RvA0, RvA1); }, {0x13, 0xc5, 0xf5, 0xff}},
      {[](RiscV64Assembler& s) { s.seqz(RvA0, RvA1); }, {0x13, 0xb5, 0x15, 0x00}},
      {[](RiscV64Assembler& s) { s.addi(RvA0, RvA1, -5); }, {0x13, 0x85, 0xb5, 0xff}},
      {[](RiscV64Assembler& s) { s.addiw(RvA0, RvA1, 7); }, {0x1b, 0x85, 0x75, 0x00}},
      {[](RiscV64Assembler& s) { s.slli(RvA0, RvA1, 33); }, {0x13, 0x95, 0x15, 0x02}},
      {[](RiscV64Assembler& s) { s.srli(RvA0, RvA1, 33); }, {0x13, 0xd5, 0x15, 0x02}},
      {[](RiscV64Assembler& s) { s.srai(RvA0, RvA1, 33); }, {0x13, 0xd5, 0x15, 0x42}},
      {[](RiscV64Assembler& s) { s.lui(RvA0, 0x12345); }, {0x37, 0x55, 0x34, 0x12}},
      {[](RiscV64Assembler& s) { s.ld(RvA0, RvSp, 16); }, {0x03, 0x35, 0x01, 0x01}},
      {[](RiscV64Assembler& s) { s.sd(RvSp, -8, RvA1); }, {0x23, 0x3c, 0xb1, 0xfe}},
      {[](RiscV64Assembler& s) { s.jalr(RvRa, RvA0, 0); }, {0xe7, 0x00, 0x05, 0x00}},
      {[](RiscV64Assembler& s) { s.ret(); }, {0x67, 0x80, 0x00, 0x00}},
      {[](RiscV64Assembler& s) { s.ebreak(); }, {0x73, 0x00, 0x10, 0x00}},
  };
  int n = 0;
  for (const Case& c : cases) {
    CodeBuffer buf(a);
    RiscV64Assembler as(buf);
    c.emit(as);
    if (bytes_of_buffer(buf) != c.want) {
      printf("rv case %d differs:", n);
      for (uint8_t b : bytes_of_buffer(buf)) printf(" %02x", b);
      printf("\n");
      ++g_failed;
    }
    ++n;
  }
  CodeBuffer buf(a);
  RiscV64Assembler as(buf);
  CHECK(!as.addi(RvA0, RvA0, 2048) && !as.ld(RvA0, RvA0, -2049) && !as.sd(RvA0, 4000, RvA0) && buf.size() == 0);
}

// ---- code buffer, labels and fixups ----

void test_fixups() {
  TestAllocator a;
  {   // x86: forward and backward jumps, and a jump that does not fit
    CodeBuffer buf(a);
    X86_64Assembler as(buf);
    const Label top = buf.new_label(), end = buf.new_label();
    buf.bind(top);
    as.jmp(end);        // forward: E9 +N
    as.jcc(X86Cond::Ne, top);   // backward
    buf.bind(end);
    as.ret();
    CHECK(buf.finish());
    // jmp is bytes 0..4, jcc 5..10, end = 11: rel32 of jmp = 11 - 5 = 6; jcc = 0 - 11 = -11
    CHECK(bytes_of_buffer(buf) == (Bytes{0xE9, 6, 0, 0, 0, 0x0F, 0x85, 0xF5, 0xFF, 0xFF, 0xFF, 0xC3}));
    Label unbound = buf.new_label();
    as.jmp(unbound);
    CHECK(!buf.finish());   // a jump to a label that was never bound
  }
  {   // AArch64: b forward, b.cond backward, cbz
    CodeBuffer buf(a);
    AArch64Assembler as(buf);
    const Label top = buf.new_label(), end = buf.new_label();
    buf.bind(top);
    as.b(end);
    as.b_cond(A64Cond::Ne, top);
    as.cbz(3, end);
    buf.bind(end);
    as.ret();
    CHECK(buf.finish());
    uint32_t w = 0;
    buf.read32(0, &w); CHECK(w == 0x14000000u + 3);          // b +12
    buf.read32(4, &w); CHECK(w == (0x54000000u | 1u | (0x7FFFFu & static_cast<uint32_t>(-1)) << 5));   // b.ne -4
    buf.read32(8, &w); CHECK(w == (0xB4000000u | 3u | 1u << 5));                                      // cbz x3, +4
  }
  {   // RISC-V: jal forward, beq backward (the assembled words below come from llvm-mc)
    CodeBuffer buf(a);
    RiscV64Assembler as(buf);
    const Label top = buf.new_label(), end = buf.new_label();
    buf.bind(top);
    as.j(end);
    as.branch(RvBranch::Ne, RvA0, RvA1, top);
    buf.bind(end);
    as.ret();
    CHECK(buf.finish());
    uint32_t w = 0;
    buf.read32(0, &w); CHECK(w == 0x0080006Fu);   // jal x0, +8
    buf.read32(4, &w); CHECK(w == 0xFEB51EE3u);   // bne a0, a1, -4
  }
  {   // out of range
    CodeBuffer buf(a);
    AArch64Assembler as(buf);
    const Label far = buf.new_label();
    as.b_cond(A64Cond::Eq, far);
    for (int i = 0; i < 300000; ++i) as.ret();   // > 2^18 instructions away
    buf.bind(far);
    CHECK(!buf.finish());
  }
  {   // allocation failure sticks
    TestAllocator small;
    small.fail_after = 0;
    CodeBuffer buf(small);
    CHECK(!buf.emit8(1) && buf.failed() && !buf.finish());
  }
}

// ---- IR ----

uint64_t g_helper_calls = 0;
uint64_t helper_mix(void* state, uint64_t a, uint64_t b) {
  ++g_helper_calls;
  uint64_t word0;
  __builtin_memcpy(&word0, state, 8);
  return (a * 31) ^ (b + word0);
}

void test_ir_validate() {
  TestAllocator a;
  {
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(16);
    const uint32_t x = b.load_state(8);
    b.exit_value(x);
    CHECK(validate(a, block));
  }
  auto bad = [&](auto build) {
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(16);
    build(b, block);
    return validate(a, block);
  };
  CHECK(!bad([](IrBuilder&, IrBlock&) {}));                                                       // empty
  CHECK(!bad([](IrBuilder& b, IrBlock&) { b.constant(1); }));                                     // does not end in an exit
  CHECK(!bad([](IrBuilder& b, IrBlock&) { b.load_state(16); b.exit(0); }));                       // past the state
  CHECK(!bad([](IrBuilder& b, IrBlock&) { b.load_state(4); b.exit(0); }));                        // misaligned
  CHECK(!bad([](IrBuilder& b, IrBlock&) { b.exit_value(7); }));                                   // unknown register
  CHECK(!bad([](IrBuilder& b, IrBlock&) { b.jump(0); }));                                         // unknown label
  CHECK(!bad([](IrBuilder& b, IrBlock&) { const uint32_t l = b.new_label(); b.jump(l); b.exit(0); }));   // never placed
  CHECK(!bad([](IrBuilder& b, IrBlock&) { const uint32_t l = b.new_label(); b.place_label(l); b.place_label(l); b.exit(0); }));   // placed twice
  CHECK(!bad([](IrBuilder& b, IrBlock& blk) { b.exit(0); blk.insts[0].op = IrOp::Helper; blk.insts[0].imm = 0; }));   // helper without an address
  CHECK(bad([](IrBuilder& b, IrBlock&) { const uint32_t l = b.new_label(); b.place_label(l); b.jump(l); }));          // an endless loop is valid IR
  // allocation failure inside validate
  TestAllocator none;
  IrBlock ok(a);
  IrBuilder b(ok);
  const uint32_t l = b.new_label();
  b.place_label(l);
  b.exit(1);
  none.fail_after = 0;
  CHECK(!validate(none, ok));
}

void test_ir_interpret() {
  TestAllocator a;
  alignas(8) uint8_t state[32] = {};
  ByteView sv(state, 32);
  {   // sum 1..10 with a loop: state[0] = i, state[8] = sum
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(32);
    const uint32_t i = b.constant(10), sum = b.constant(0), zero = b.constant(0), one = b.constant(1);
    const uint32_t top = b.new_label(), done = b.new_label();
    b.place_label(top);
    b.branch_if(IrCond::Eq, i, zero, done);
    b.binary(IrOp::Add, sum, i);   // result unused: the destination is a fresh register, so do the update with a move
    const uint32_t next = b.binary(IrOp::Add, sum, i);
    block.insts.push_back(IrInst{IrOp::Move, IrCond::Eq, sum, next, kNoVReg, 0});
    const uint32_t dec = b.binary(IrOp::Sub, i, one);
    block.insts.push_back(IrInst{IrOp::Move, IrCond::Eq, i, dec, kNoVReg, 0});
    b.jump(top);
    b.place_label(done);
    b.store_state(8, sum);
    b.exit_value(sum);
    CHECK(validate(a, block));
    uint64_t out = 0;
    CHECK(interpret(a, block, sv, 1000, &out) && out == 55);
    uint64_t stored;
    __builtin_memcpy(&stored, state + 8, 8);
    CHECK(stored == 55);
    CHECK(!interpret(a, block, sv, 20, &out));   // step limit
  }
  {   // arithmetic corner cases
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(32);
    const uint32_t m1 = b.constant(-1), sixty5 = b.constant(65), x = b.constant(0x8000000000000000ll);
    const uint32_t sar = b.binary(IrOp::Sar, x, sixty5);    // count 65 & 63 = 1
    const uint32_t shr = b.binary(IrOp::Shr, x, sixty5);
    const uint32_t lt = b.set_cond(IrCond::Lt, x, m1);      // INT64_MIN < -1
    const uint32_t ltu = b.set_cond(IrCond::Ltu, x, m1);
    uint32_t acc = b.binary(IrOp::Xor, sar, shr);
    acc = b.binary(IrOp::Add, acc, b.binary(IrOp::Shl, lt, b.constant(1)));
    acc = b.binary(IrOp::Add, acc, ltu);
    b.exit_value(acc);
    uint64_t out = 0;
    CHECK(interpret(a, block, sv, 100, &out));
    CHECK(out == ((0xC000000000000000ull ^ 0x4000000000000000ull) + 2 + 1));
  }
}

// ---- x86-64 back end, run natively ----

bool g_can_run = native_host_arch() == HostArch::X86_64;

JitEntry compile_ir(HostCodeMemory& mem, allocators::IAllocator& a, const IrBlock& block, CodeRegion* region, X86CallingConvention cc = native_x86_calling_convention(),
                    uint32_t max_registers = 32) {
  X86_64Backend backend(a, cc, max_registers);
  CodeBuffer code(a);
  if (!backend.compile(block, code)) return nullptr;
  if (!mem.allocate(code.size(), region)) return nullptr;
  region->bytes.copy_from(code.view());
  if (!mem.make_executable(*region)) return nullptr;
  return reinterpret_cast<JitEntry>(region->bytes.data());
}

void test_backend_fixed() {
  if (!g_can_run) return;
  TestAllocator a;
  HostCodeMemory mem;
  alignas(8) uint8_t state[64] = {};
  const uint64_t seed = 7;
  __builtin_memcpy(state, &seed, 8);
  IrBlock block(a);
  IrBuilder b(block);
  b.set_state_size(64);
  const uint32_t x = b.load_state(0);
  const uint32_t y = b.constant(0x1234567890ll);
  const uint32_t s = b.binary(IrOp::Add, x, y);
  const uint32_t h = b.helper(helper_mix, s, x);
  b.store_state(8, h);
  b.store_state(16, b.set_cond(IrCond::Gtu, h, x));
  b.exit_value(b.unary(IrOp::Neg, h));
  CHECK(validate(a, block));
  CodeRegion region;
  JitEntry fn = compile_ir(mem, a, block, &region);
  CHECK(fn != nullptr);
  if (!fn) return;
  alignas(8) uint8_t ref[64];
  __builtin_memcpy(ref, state, 64);
  uint64_t want = 0;
  CHECK(interpret(a, block, ByteView(ref, 64), 100, &want));
  const uint64_t got = fn(state);
  CHECK(got == want);
  CHECK(__builtin_memcmp(state, ref, 64) == 0);
  mem.release(region);
  CHECK(mem.live == 0);
}

void run_random(X86CallingConvention cc, int programs, uint32_t max_registers) {
  if (!g_can_run) return;
  TestAllocator a;
  HostCodeMemory mem;
  uint64_t seed = 0x2545F4914F6CDD1Dull;
  for (int p = 0; p < programs; ++p) {
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(64);
    build_random(b, seed, 4 + p % 60, helper_mix);
    if (b.failed() || !validate(a, block)) { printf("random program %d invalid\n", p); ++g_failed; return; }
    CodeRegion region;
    JitEntry fn = compile_ir(mem, a, block, &region, cc, max_registers);
    if (!fn) { printf("random program %d did not compile\n", p); ++g_failed; return; }
    for (int trial = 0; trial < 4; ++trial) {
      alignas(8) uint8_t native[64], ref[64];
      for (int i = 0; i < 64; ++i) native[i] = ref[i] = static_cast<uint8_t>((seed >> (i % 8 * 8)) + i * 37 + trial);
      uint64_t want = 0;
      if (!interpret(a, block, ByteView(ref, 64), 100000, &want)) { printf("random program %d did not finish\n", p); ++g_failed; mem.release(region); return; }
      const uint64_t got = fn(native);
      if (got != want || __builtin_memcmp(native, ref, 64) != 0) {
        printf("random program %d trial %d (%u registers): native %llx, interpreter %llx\n", p, trial, max_registers, (unsigned long long)got, (unsigned long long)want);
        ++g_failed;
        mem.release(region);
        return;
      }
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    }
    mem.release(region);
  }
  CHECK(mem.live == 0);
}

void test_backend_loops() {
  if (!g_can_run) return;
  TestAllocator a;
  HostCodeMemory mem;
  IrBlock block(a);
  IrBuilder b(block);
  b.set_state_size(64);
  build_loops(b, helper_mix);
  CHECK(!b.failed() && validate(a, block));
  for (uint32_t limit : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 32u}) {
    CodeRegion region;
    JitEntry fn = compile_ir(mem, a, block, &region, native_x86_calling_convention(), limit);
    CHECK(fn != nullptr);
    if (!fn) continue;
    for (uint64_t trips : {0ull, 1ull, 5ull, 31ull, 33ull}) {
      alignas(8) uint8_t native[64] = {}, ref[64] = {};
      const uint64_t init = 0x1111 * (trips + 1);
      __builtin_memcpy(native, &trips, 8);
      __builtin_memcpy(native + 8, &init, 8);
      __builtin_memcpy(ref, native, 64);
      uint64_t want = 0;
      CHECK(interpret(a, block, ByteView(ref, 64), 1000000, &want));
      const uint64_t got = fn(native);
      if (got != want || __builtin_memcmp(native, ref, 64) != 0) printf("loops, %u registers, %llu trips: native %llx, interpreter %llx\n", limit, (unsigned long long)trips, (unsigned long long)got, (unsigned long long)want);
      CHECK(got == want && __builtin_memcmp(native, ref, 64) == 0);
    }
    mem.release(region);
  }
}

void test_backend_random() {
  // every size of register pool, down to none at all (everything spilled) and up to the whole pool: callee-saved only, then with caller-saved ones
  for (uint32_t limit : {0u, 1u, 2u, 4u, 5u, 32u}) run_random(native_x86_calling_convention(), 600, limit);
}

void test_backend_win64_codegen() {
  // The Win64 flavour cannot be run on a System V host, but it must still generate: the helper call loads rcx/rdx/r8 and reserves shadow space.
  TestAllocator a;
  IrBlock block(a);
  IrBuilder b(block);
  b.set_state_size(16);
  b.exit_value(b.helper(helper_mix, b.load_state(0), b.load_state(8)));
  X86_64Backend backend(a, X86CallingConvention::Win64);
  CodeBuffer code(a);
  CHECK(backend.compile(block, code));
  // prologue: push rbp; mov rbp,rsp; push rbx; mov rbx,rcx
  CHECK(bytes_of_buffer(code).size() > 10);
  const Bytes got = bytes_of_buffer(code);
  CHECK(got[0] == 0x55 && got[1] == 0x48 && got[2] == 0x89 && got[3] == 0xe5 && got[4] == 0x53 && got[5] == 0x48 && got[6] == 0x89 && got[7] == 0xcb);
  HeapCodeMemory heap(a);
  CodeRegion region;
  CHECK(heap.allocate(code.size(), &region) && !heap.make_executable(region) && !heap.can_execute());
  heap.release(region);
}

// ---- register allocator ----

void test_register_allocator() {
  TestAllocator a;
  const RegisterPool pool = {4, 2};
  uint64_t seed = 0x1234567;
  for (int p = 0; p < 300; ++p) {
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(64);
    build_random(b, seed, 4 + p % 80, helper_mix);
    for (uint32_t limit : {0u, 1u, 3u, 6u}) {
      RegisterAllocation alloc(a);
      CHECK(alloc.allocate(block, pool, limit));
      uint32_t spills = 0;
      for (uint32_t v = 0; v < block.vreg_count; ++v) {
        const VRegLocation lv = alloc.location(v);
        if (lv.kind == VRegLocation::Kind::Spill) ++spills;
        if (lv.kind == VRegLocation::Kind::Register) {
          CHECK(lv.index < limit && lv.index < 6);
          if (alloc.crosses_call(v)) CHECK(lv.index < pool.callee_saved);   // a value that lives across a call is never in a caller-saved register
          CHECK(alloc.used_registers() >> lv.index & 1);
          for (uint32_t w = v + 1; w < block.vreg_count; ++w) {
            const VRegLocation lw = alloc.location(w);
            if (lw.kind != VRegLocation::Kind::Register || lw.index != lv.index) continue;
            CHECK(alloc.interval_end(v) <= alloc.interval_start(w) || alloc.interval_end(w) <= alloc.interval_start(v));   // sharing a register means not overlapping
          }
        }
        if (lv.kind == VRegLocation::Kind::Spill) {
          CHECK(lv.index < alloc.spill_slots());
          for (uint32_t w = v + 1; w < block.vreg_count; ++w) {   // a slot is shared only by intervals that do not overlap
            const VRegLocation lw = alloc.location(w);
            if (lw.kind != VRegLocation::Kind::Spill || lw.index != lv.index) continue;
            CHECK(alloc.interval_end(v) <= alloc.interval_start(w) || alloc.interval_end(w) <= alloc.interval_start(v));
          }
        }
      }
      CHECK(alloc.spill_slots() <= spills && (spills == 0) == (alloc.spill_slots() == 0));
      if (limit == 0 && block.vreg_count > 40) CHECK(alloc.spill_slots() < spills);   // slots are reused
      if (limit == 0) CHECK(alloc.used_registers() == 0);
    }
  }
  {   // directed: a value read before it is written is zeroed at entry; one that is never used gets no location; a loop keeps its value alive
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(16);
    const uint32_t unused = b.new_vreg();
    const uint32_t early = b.new_vreg();                       // read below before any write
    const uint32_t acc = b.constant(0);
    const uint32_t top = b.new_label();
    b.place_label(top);                                         // loop head
    const uint32_t one = b.constant(1);
    const uint32_t sum = b.binary(IrOp::Add, acc, one);
    block.insts.push_back(IrInst{IrOp::Move, IrCond::Eq, acc, sum, kNoVReg, 0});
    b.branch_if(IrCond::Ltu, acc, b.constant(10), top);         // back edge
    b.exit_value(b.binary(IrOp::Add, acc, early));
    RegisterAllocation alloc(a);
    CHECK(alloc.allocate(block, pool));
    CHECK(alloc.location(unused).kind == VRegLocation::Kind::Unused);
    CHECK(alloc.zero_at_entry(early) && alloc.interval_start(early) == 0);
    CHECK(!alloc.zero_at_entry(acc));
    CHECK(alloc.interval_end(acc) >= alloc.interval_start(one) + 3);   // acc lives to the end of the loop at least
    uint64_t out = 0;
    alignas(8) uint8_t st[16] = {};
    CHECK(interpret(a, block, ByteView(st, 16), 1000, &out) && out == 10);
  }
  {   // not enough registers: the interval that ends last is the one spilled
    IrBlock block(a);
    IrBuilder b(block);
    b.set_state_size(16);
    uint32_t v[8];
    for (int i = 0; i < 8; ++i) v[i] = b.constant(i + 1);
    uint32_t sum = v[0];
    for (int i = 1; i < 8; ++i) sum = b.binary(IrOp::Add, sum, v[i]);
    b.exit_value(sum);
    RegisterAllocation alloc(a);
    CHECK(alloc.allocate(block, {2, 2}, 4));
    CHECK(alloc.spill_slots() > 0 && alloc.used_registers() == 0xF);
    RegisterAllocation too_big(a);
    CHECK(!too_big.allocate(block, {20, 20}));
  }
}

// ---- cache, hotness, dispatcher ----

// pc -> adds pc to state[0], returns pc + 16 (a "guest block" covering 16 bytes); 0xBAD cannot be translated.
struct CountingFrontend final : IJitFrontend {
  std::atomic<int> translated{0};
  bool translate(uint64_t pc, IrBuilder& b, uint64_t* size) override {
    if (pc == 0xBAD) return false;
    ++translated;
    b.set_state_size(16);
    const uint32_t v = b.load_state(0);
    const uint32_t sum = b.binary(IrOp::Add, v, b.constant(static_cast<int64_t>(pc)));
    b.store_state(0, sum);
    b.exit(static_cast<int64_t>(pc + 16));
    *size = 16;
    return true;
  }
};

void test_hotness() {
  TestAllocator a;
  HotnessTracker t(a, 100, 5);
  CHECK(t.ok());
  int crossings = 0;
  for (int i = 0; i < 20; ++i) crossings += t.record(0x1000);
  CHECK(crossings == 1 && t.count(0x1000) == 20);
  t.reset(0x1000);
  CHECK(t.count(0x1000) == 0);
  for (int i = 0; i < 5; ++i) crossings += t.record(0x1000);
  CHECK(crossings == 2);
  t.reset_all();
  CHECK(t.count(0x1000) == 0);
  // threaded: exactly one thread sees the crossing
  HotnessTracker shared(a, 64, 1000);
  std::atomic<int> winners{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { for (int k = 0; k < 500; ++k) winners += shared.record(0x2000); });
  for (auto& th : threads) th.join();
  CHECK(winners == 1 && shared.count(0x2000) == 2000);   // (only the visit that reaches the threshold counts)
}

void test_compile_and_cache() {
  if (!g_can_run) return;
  TestAllocator a;
  HostCodeMemory mem;
  X86_64Backend backend(a);
  CodeCache cache(a, 64);
  JitCompiler compiler(a, mem, backend, cache);
  CountingFrontend fe;
  CHECK(cache.ok());
  alignas(8) uint8_t state[16] = {};
  {
    kernel::RcuReader r;
    CHECK(cache.lookup(0x1000) == nullptr);
  }
  CHECK(compiler.compile(fe, 0x1000) == CompileResult::Compiled);
  CHECK(compiler.compile(fe, 0xBAD) == CompileResult::Untranslatable);
  CHECK(cache.block_count() == 1);
  {
    kernel::RcuReader r;
    const CompiledBlock* blk = cache.lookup(0x1000);
    CHECK(blk && blk->guest_start == 0x1000 && blk->guest_size == 16 && blk->tier == 0 && blk->code_size > 0);
    CHECK(blk && run_block(*blk, ByteView(state, 16)) == 0x1010);
    uint64_t s0;
    __builtin_memcpy(&s0, state, 8);
    CHECK(s0 == 0x1000);
    CHECK(cache.lookup(0x2000) == nullptr);
  }
  // Replace with a higher tier: the old block is retired, the live count of code regions stays bounded.
  CHECK(compiler.compile(fe, 0x1000, 1) == CompileResult::Compiled);
  {
    kernel::RcuReader r;
    const CompiledBlock* blk = cache.lookup(0x1000);
    CHECK(blk && blk->tier == 1);
  }
  kernel::rcu_barrier();
  CHECK(mem.live == 1 && cache.block_count() == 1);
  // Invalidation by range: [0x1008, 0x1009) overlaps the block 0x1000..0x1010, 0x1010.. does not
  CHECK(cache.invalidate(0x1010, 8) == 0 && cache.invalidate(0x0FF8, 8) == 0);
  CHECK(cache.invalidate(0x1008, 1) == 1 && cache.block_count() == 0);
  {
    kernel::RcuReader r;
    CHECK(cache.lookup(0x1000) == nullptr);
  }
  kernel::rcu_barrier();
  CHECK(mem.live == 0);
  // Code memory that cannot execute publishes nothing and leaks nothing.
  HeapCodeMemory heap(a);
  JitCompiler dry(a, heap, backend, cache);
  const int64_t before = a.live;
  CHECK(dry.compile(fe, 0x3000) == CompileResult::NotExecutable && cache.block_count() == 0 && a.live == before);
  // ... but generate() shows the code it would have built
  CodeBuffer code(a);
  uint64_t size = 0;
  CHECK(dry.generate(fe, 0x3000, code, &size) == CompileResult::Compiled && size == 16 && code.size() > 0);
  // Allocation failures anywhere in compile() are reported, not crashes
  for (int64_t n = 0; n < 60; ++n) {
    a.fail_after = n;
    const CompileResult r = compiler.compile(fe, 0x4000 + 16 * static_cast<uint64_t>(n));
    a.fail_after = -1;
    if (r == CompileResult::Compiled) continue;
    CHECK(r == CompileResult::OutOfMemory || r == CompileResult::Untranslatable || r == CompileResult::BackendFailed);   // (the register allocator running out of memory is a back end failure)
  }
  cache.flush();
  kernel::rcu_barrier();
  CHECK(mem.live == 0);
}

void test_dispatcher() {
  if (!g_can_run) return;
  TestAllocator a;
  HostCodeMemory mem;
  X86_64Backend backend(a);
  CodeCache cache(a, 64);
  HotnessTracker hot(a, 64, 3);
  JitCompiler compiler(a, mem, backend, cache);
  CountingFrontend fe;
  JitDispatcher disp(compiler, cache, hot, fe);
  alignas(8) uint8_t state[16] = {};
  uint64_t out = 0;
  CHECK(!disp.try_run(0x1000, ByteView(state, 16), &out));   // visit 1, 2: interpret
  CHECK(!disp.try_run(0x1000, ByteView(state, 16), &out));
  CHECK(disp.try_run(0x1000, ByteView(state, 16), &out) && out == 0x1010);   // visit 3: hot, compiled and run
  CHECK(disp.try_run(0x1000, ByteView(state, 16), &out) && out == 0x1010);   // cached
  uint64_t s0;
  __builtin_memcpy(&s0, state, 8);
  CHECK(s0 == 0x2000);
  CHECK(fe.translated == 1);
  // an untranslatable hot address keeps being interpreted and is not retried on every visit
  for (int i = 0; i < 10; ++i) CHECK(!disp.try_run(0xBAD, ByteView(state, 16), &out));
  CHECK(compiler.failed() == 1);
  // the guest overwrote its code
  disp.invalidate(0x1000, 4);
  CHECK(!disp.try_run(0x1000, ByteView(state, 16), &out));
  CHECK(hot.count(0x1000) == 1);
  cache.flush();
  kernel::rcu_barrier();
  CHECK(mem.live == 0);
}

// Readers run compiled code while a writer keeps replacing, invalidating and flushing; the address sanitizer catches a block freed under a reader
// (a munmap'd code page would crash the reader outright), ThreadSanitizer any race in the publication.
void test_cache_stress() {
  if (!g_can_run) return;
  TestAllocator a;
  HostCodeMemory mem;
  {
    X86_64Backend backend(a);
    CodeCache cache(a, 32);
    JitCompiler compiler(a, mem, backend, cache);
    CountingFrontend fe;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ran{0}, bad{0};
    constexpr uint64_t kPcs = 24;
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&, t] {
        alignas(8) uint8_t state[16] = {};
        uint64_t i = static_cast<uint64_t>(t);
        while (!stop) {
          const uint64_t pc = 0x1000 + (i++ % kPcs) * 16;
          kernel::RcuReader r;
          const CompiledBlock* blk = cache.lookup(pc);
          if (!blk) continue;
          const uint64_t out = run_block(*blk, ByteView(state, 16));
          if (out != pc + 16 || blk->guest_start != pc) ++bad;
          ++ran;
        }
      });
    }
    std::thread writer([&] {
      uint64_t seed = 12345;
      for (int i = 0; i < 4000; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const uint64_t pc = 0x1000 + (seed >> 33) % kPcs * 16;
        switch (seed >> 60) {
          case 0: cache.invalidate(pc, 16); break;
          case 1: if ((seed >> 40 & 7) == 0) cache.flush(); break;
          default: compiler.compile(fe, pc, static_cast<uint32_t>(i & 3)); break;
        }
      }
      stop = true;
    });
    writer.join();
    for (auto& th : readers) th.join();
    CHECK(bad == 0);
    CHECK(ran > 0);
  }   // the cache's destructor flushes and waits for the grace period
  CHECK(mem.live == 0);
  CHECK(a.live == 0);
}

}  // namespace

int main() {
  test_x86_encodings();
  test_a64_encodings();
  test_rv_encodings();
  test_constants();
  test_fixups();
  test_ir_validate();
  test_ir_interpret();
  test_backend_fixed();
  test_register_allocator();
  test_backend_loops();
  test_backend_random();
  test_backend_win64_codegen();
  test_hotness();
  test_compile_and_cache();
  test_dispatcher();
  test_cache_stress();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed.load());
    return 1;
  }
  printf("JitTest: all passed\n");
  return 0;
}
