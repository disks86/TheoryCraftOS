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

// The AtomBIOS interpreter against hand-assembled images: every operation with expected results worked out by hand from the driver's semantics (operand kinds, alignments, the
// compare/jump conditions, tables calling tables, the indirect I/O programs, the scratch memory), the parser's refusals, the limits (a loop, an endless chain, a deep chain), and
// truncated and bit-flipped images (which must end in an error, never in a read outside the image: the tests run under the address and undefined behaviour sanitizers).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// The card as the interpreter sees it: registers that remember what was written (and read back), a log of the accesses, a clock that moves on.
struct Host final : IAtomHost {
  struct Access { char kind; uint32_t reg, value; };   // 'r' read, 'w' write; 'm'/'M' mc read/write; 'p'/'P' pll read/write
  std::vector<Access> log;
  std::map<uint32_t, uint32_t> regs, mc, pll;
  uint64_t us = 0, step = 1;
  uint64_t delayed_us = 0, delayed_ms = 0;
  uint32_t read_register(uint32_t reg) override { const uint32_t v = regs.count(reg) ? regs[reg] : 0; log.push_back({'r', reg, v}); return v; }
  void write_register(uint32_t reg, uint32_t value) override { regs[reg] = value; log.push_back({'w', reg, value}); }
  uint32_t read_mc(uint32_t reg) override { const uint32_t v = mc.count(reg) ? mc[reg] : 0; log.push_back({'m', reg, v}); return v; }
  void write_mc(uint32_t reg, uint32_t value) override { mc[reg] = value; log.push_back({'M', reg, value}); }
  uint32_t read_pll(uint32_t reg) override { const uint32_t v = pll.count(reg) ? pll[reg] : 0; log.push_back({'p', reg, v}); return v; }
  void write_pll(uint32_t reg, uint32_t value) override { pll[reg] = value; log.push_back({'P', reg, value}); }
  void delay_us(uint32_t n) override { delayed_us += n; }
  void delay_ms(uint32_t n) override { delayed_ms += n; }
  uint64_t now_us() override { us += step; return us; }
  std::vector<uint32_t> writes_to(uint32_t reg) const { std::vector<uint32_t> v; for (const Access& a : log) if (a.kind == 'w' && a.reg == reg) v.push_back(a.value); return v; }
};

#include "AtomBiosImage.hpp"
using namespace atomimage;

struct Run {
  TestAllocator alloc;
  Host host;
  std::vector<uint8_t> image;
  std::unique_ptr<AtomBios> bios;
  explicit Run(const Image& img) : image(img.build()) { bios.reset(new AtomBios(host, ConstByteView(image.data(), image.size()), alloc)); }
};

static Image one_table(const Code& code, uint32_t ws = 8, uint32_t ps = 0) {
  Image img;
  img.tables.push_back({0, ws, ps, code});
  return img;
}

static AtomBios::Status run_one(Run& r, ArrayView<uint32_t> params = ArrayView<uint32_t>()) { return r.bios->execute_table(0, params); }

static void test_parse() {
  Image img = one_table(Code().eot());
  {
    Run r(img);
    CHECK(r.bios->valid() && r.bios->cmd_table() == 0x200 && r.bios->data_table() == 0x300 && r.bios->firmware_revision() == 0x01020304 && r.bios->scratch_bytes() == 20 * 1024);
    uint8_t f = 0, c = 0;
    CHECK(r.bios->cmd_table_header(0, &f, &c) && !r.bios->cmd_table_header(1, &f, &c) && !r.bios->cmd_table_header(300, &f, &c));
    uint16_t size = 0, start = 0;
    CHECK(r.bios->data_table_header(4, &size, &f, &c, &start) && start == 0x400 && size == 0x20 && !r.bios->data_table_header(5, &size, &f, &c, &start) && !r.bios->data_table_header(999, &size, &f, &c, &start));
  }
  // The magic numbers and pointers, one at a time.
  const std::vector<uint8_t> good = img.build();
  struct Break { size_t at; uint8_t value; };
  const Break breaks[] = {{0, 0}, {1, 0}, {0x30, 'X'}, {0x39, 0}, {0x48, 0x01}, {0x49, 0x80}, {0x100 + 4, 'X'}, {0x100 + 7, 0}};
  for (const Break& b : breaks) {
    std::vector<uint8_t> bad = good;
    bad[b.at] = b.value;
    TestAllocator alloc;
    Host host;
    AtomBios bios(host, ConstByteView(bad.data(), bad.size()), alloc);
    CHECK(!bios.valid());
    CHECK(bios.execute_table(0, ArrayView<uint32_t>()) == AtomBios::Status::BadImage && bios.asic_init() == AtomBios::Status::BadImage);
  }
  // Too small, too big, empty.
  {
    TestAllocator alloc;
    Host host;
    AtomBios tiny(host, ConstByteView(good.data(), 0x40), alloc);
    CHECK(!tiny.valid());
    AtomBios empty(host, ConstByteView(), alloc);
    CHECK(!empty.valid());
  }
  // An indirect I/O program that ends nowhere, or has an operation that does not exist.
  {
    Image bad = img;
    bad.iio.push_back({3, {0x7F}});
    Run r(bad);
    CHECK(!r.bios->valid());
    Image runaway = img;
    runaway.iio.push_back({3, {2, 0x00, 0x10}});
    std::vector<uint8_t> bytes = runaway.build();
    for (uint32_t i = Image::kIioAt + 4; i < Image::kImageBytes; ++i) bytes[i] = 0x00 + (i % 2 ? 0 : 0);   // no END anywhere after the program
    memset(&bytes[Image::kIioAt + 4], 0, Image::kImageBytes - Image::kIioAt - 4);
    bytes[Image::kIioAt + 4] = 1;   // START, then NOPs to the end of the image
    bytes[Image::kIioAt + 5] = 3;
    TestAllocator alloc;
    Host host;
    AtomBios bios(host, ConstByteView(bytes.data(), bytes.size()), alloc);
    CHECK(!bios.valid());
  }
  // An indirect I/O program the 16-bit offsets of the table cannot point to (the image is bigger than 64 KiB and the program is past its end): not usable.
  {
    std::vector<uint8_t> big = good;
    big.resize(0x20000, 0);
    big[Image::kData + 4 + 2 * 23] = 0xFC;     // the indirect I/O table at 0xFFFC: its first program at 0x10000
    big[Image::kData + 4 + 2 * 23 + 1] = 0xFF;
    big[0x10000] = 1;                          // START, port 3, END
    big[0x10001] = 3;
    big[0x10002] = 9;
    TestAllocator alloc;
    Host host;
    AtomBios bios(host, ConstByteView(big.data(), big.size()), alloc);
    CHECK(!bios.valid());
  }
  // The scratch memory: what VRAM_UsageByFirmware asks for, 20 KB if nothing, 20 KB for an SR-IOV reservation, and not more than a few megabytes.
  {
    Image a = img;
    a.vram_usage_kb = 48;
    Run ra(a);
    CHECK(ra.bios->valid() && ra.bios->scratch_bytes() == 48 * 1024);
    Image b = img;
    b.vram_usage_kb = 48;
    b.vram_usage_flags = 0x80000000u;   // ATOM_VRAM_BLOCK_SRIOV_MSG_SHARE_RESERVATION (2) << 30
    Run rb(b);
    CHECK(rb.bios->valid() && rb.bios->scratch_bytes() == 20 * 1024);
    Image c = img;
    c.vram_usage_kb = 40000;
    Run rc(c);
    CHECK(!rc.bios->valid());
  }
}

static void test_move_and_registers() {
  // Move: register <- immediate; register -> register; the reads and writes go to the host.
  Code c;
  c.move(REG, 0x1234, IMM, 0xA5A5A5A5);
  c.move(REG, 0x2000, REG, 0x1234);
  c.move(REG, 0, IMM, 0x11);   // (register 0 is written shifted left by two)
  c.eot();
  Run r(one_table(c));
  CHECK(run_one(r) == AtomBios::Status::Ok);
  CHECK(r.host.writes_to(0x1234) == std::vector<uint32_t>{0xA5A5A5A5} && r.host.writes_to(0x2000) == std::vector<uint32_t>{0xA5A5A5A5} && r.host.writes_to(0) == std::vector<uint32_t>{0x44});
  CHECK(r.bios->warnings() == 0);
  // The register block (SetRegBlock) is added to the register numbers of the operations that follow; it starts from zero for every run.
  Code d;
  d.u8(SETREGBLOCK).u16(0x100);
  d.move(REG, 0x20, IMM, 7);
  d.eot();
  Run r2(one_table(d));
  CHECK(run_one(r2) == AtomBios::Status::Ok && r2.host.writes_to(0x120) == std::vector<uint32_t>{7});
  CHECK(run_one(r2) == AtomBios::Status::Ok && r2.host.writes_to(0x120).size() == 2);
}

static void test_alignments() {
  // WS[5] <- the bytes of an immediate in each of the destination positions; the other bytes of the register are kept (the destination is read first when the source is not a dword).
  // Source alignment BYTE0 (4) with the destination field 0..3 gives byte 0, 8, 16, 24 of the destination (kDstToSrc[4..7] = 4, 5, 6, 7).
  for (int d = 0; d < 4; ++d) {
    Code c;
    c.move(WS, 5, IMM, 0xAB, BYTE0, d);
    c.move(PS, 0, WS, 5);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[2] = {0, 0};
    // preload the workspace through a table that sets it: a dword move first
    Code pre;
    pre.move(WS, 5, IMM, 0x11223344);
    pre.move(WS, 5, IMM, 0xAB, BYTE0, d);
    pre.move(PS, 0, WS, 5);
    pre.eot();
    Run rp(one_table(pre));
    CHECK(run_one(rp, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok);
    const uint32_t shifts[4] = {0, 8, 16, 24};
    const uint32_t expected = (0x11223344u & ~(0xFFu << shifts[d])) | (0xABu << shifts[d]);
    CHECK(ps[0] == expected);
  }
  // Words: the source alignment WORD0 (1): the destination field 0, 1, 2 gives bits 15:0, 23:8, 31:16 (kDstToSrc[1..3] = {1, 2, 3, 0}); field 3 is the whole dword.
  const uint32_t word_expected[4] = {0x1122BEEF, 0x11BEEF44, 0xBEEF3344, 0x0000BEEF};
  for (int d = 0; d < 4; ++d) {
    Code pre;
    pre.move(WS, 5, IMM, 0x11223344);
    pre.move(WS, 5, IMM, 0xBEEF, WORD0, d);
    pre.move(PS, 0, WS, 5);
    pre.eot();
    Run rp(one_table(pre));
    uint32_t ps[2] = {0, 0};
    CHECK(run_one(rp, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok);
    if (d < 3) CHECK(ps[0] == word_expected[d]);
    else CHECK(ps[0] == 0x0000BEEF || ps[0] == 0xBEEF);
  }
  // A source that is a byte of a register: Move PS[0] <- byte 2 of PS[1] (the source alignment BYTE16 (6) selects the byte, the destination is the whole dword when its field is 0
  // and the alignment is dword... here the destination alignment follows the source's: byte 0 of PS[0] for the field 0).
  Code c;
  c.move(PS, 0, PS, 1, BYTE16, 0);
  c.eot();
  Run r(one_table(c));
  uint32_t ps[2] = {0xFFFFFF00, 0x00AA0000};
  CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 0xFFFFFFAA);
}

static uint32_t ws_after(const Code& code, uint32_t ps0 = 0, uint32_t ps1 = 0) {
  Run r(one_table(code));
  uint32_t ps[4] = {ps0, ps1, 0, 0};
  r.bios->execute_table(0, ArrayView<uint32_t>(ps, 4));
  return ps[2];
}

static void test_arithmetic() {
  // PS[2] <- operation(PS[0], PS[1]) by the operation: dst op= src, whole dwords.
  auto binary = [&](int opcode, uint32_t a, uint32_t b) {
    Code c;
    c.op2(opcode, PS, 0, PS, 1);
    c.move(PS, 2, PS, 0);
    c.eot();
    return ws_after(c, a, b);
  };
  CHECK(binary(ADD, 0xFFFFFFF0, 0x20) == 0x10);          // (wraps)
  CHECK(binary(SUB, 5, 7) == 0xFFFFFFFE);
  CHECK(binary(AND, 0xF0F0F0F0, 0x0FF00FF0) == 0x00F000F0);
  CHECK(binary(OR, 0xF0F0F0F0, 0x0FF00FF0) == 0xFFF0FFF0);
  CHECK(binary(XOR, 0xF0F0F0F0, 0x0FF00FF0) == 0xFF00FF00);
  // Mul: the product goes to the quotient register (WS 0x40), Div: quotient and remainder (0x40, 0x41); neither changes the destination.
  {
    Code c;
    c.op2(MUL, PS, 0, PS, 1);
    c.move(PS, 2, WS, 0x40);
    c.move(PS, 3, WS, 0x41);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[4] = {100000, 50000, 0, 0xCC};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 4)) == AtomBios::Status::Ok && ps[2] == 100000u * 50000u && ps[0] == 100000);   // (low 32 bits: 5000000000 mod 2^32)
    CHECK(ps[2] == 705032704);
  }
  {
    Code c;
    c.op2(DIV, PS, 0, PS, 1);
    c.move(PS, 2, WS, 0x40);
    c.move(PS, 3, WS, 0x41);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[4] = {1000, 7, 0, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 4)) == AtomBios::Status::Ok && ps[2] == 142 && ps[3] == 6);
    uint32_t zero[4] = {1000, 0, 5, 5};
    CHECK(run_one(r, ArrayView<uint32_t>(zero, 4)) == AtomBios::Status::Ok && zero[2] == 0 && zero[3] == 0);   // (a division by zero gives zero)
  }
  // Mul32 / Div32 (parameter space or workspace): 64-bit results in the quotient and remainder registers.
  {
    Code c;
    c.op2(MUL32_PS, PS, 0, PS, 1);   // opcode 123 is PS-destination multiply (the opcode itself names the destination kind)
    c.b[0] = MUL32_PS;
    c.move(PS, 2, WS, 0x40);
    c.move(PS, 3, WS, 0x41);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[4] = {0xFFFFFFFF, 0x10, 0, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 4)) == AtomBios::Status::Ok && ps[2] == 0xFFFFFFF0 && ps[3] == 0xF);   // 0xFFFFFFFF * 16 = 0xF_FFFFFFF0
  }
  {
    Code c;
    c.u8(DIV32_PS).u8(PS).u8(0).u8(1);   // dividend PS[0] with the high half in the remainder register, divisor PS[1]
    c.move(PS, 2, WS, 0x40);
    c.move(PS, 3, WS, 0x41);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[4] = {0x00000000, 0x10, 0, 0};
    // (WS 0x41 is zero at the start of a run: the dividend is PS[0] alone)
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 4)) == AtomBios::Status::Ok && ps[2] == 0 && ps[3] == 0);
    uint32_t ps2[4] = {0x1000, 0x10, 0, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps2, 4)) == AtomBios::Status::Ok && ps2[2] == 0x100 && ps2[3] == 0);
  }
  // Mask: dst = (dst & mask) | src, the mask a direct immediate of the source's alignment.
  {
    Code c;
    c.u8(MASK + kind(PS)).u8(IMM).u8(0).u32(0xFFFF00FF).u32(0x00004200);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[2] = {0x12345678, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 0x12344278);
  }
  // Clear (the destination dword, or the byte the attribute names), the shifts by an immediate byte, and Shl/Shr by a source.
  {
    Code c;
    c.u8(CLEAR + kind(PS)).u8(0x00).u8(0);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[2] = {0xDEADBEEF, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 0);
    Code s;
    s.u8(SHL_OP + kind(PS)).u8(0x00).u8(0).u8(4);          // PS[0] <<= 4
    s.u8(SHR_OP + kind(PS)).u8(0x00).u8(1).u8(8);          // PS[1] >>= 8
    s.eot();
    Run rs(one_table(s));
    uint32_t sp[2] = {0x0000ABCD, 0xAABBCCDD};
    CHECK(run_one(rs, ArrayView<uint32_t>(sp, 2)) == AtomBios::Status::Ok && sp[0] == 0x000ABCD0 && sp[1] == 0x00AABBCC);
    Code big;
    big.u8(SHL_OP + kind(PS)).u8(0x00).u8(0).u8(40);       // (a shift by 40: zero, not undefined)
    big.eot();
    Run rb(one_table(big));
    uint32_t bp[1] = {0xFFFFFFFF};
    CHECK(run_one(rb, ArrayView<uint32_t>(bp, 1)) == AtomBios::Status::Ok && bp[0] == 0);
    Code v;
    v.op2(SHL, PS, 0, PS, 1);                              // Shl: PS[0] <<= PS[1] (from the register as it was read)
    v.eot();
    Run rv(one_table(v));
    uint32_t vp[2] = {3, 5};
    CHECK(run_one(rv, ArrayView<uint32_t>(vp, 2)) == AtomBios::Status::Ok && vp[0] == 96);
    Code w;
    w.op2(SHR, PS, 0, PS, 1);
    w.eot();
    Run rw(one_table(w));
    uint32_t wp[2] = {0x100, 4};
    CHECK(run_one(rw, ArrayView<uint32_t>(wp, 2)) == AtomBios::Status::Ok && wp[0] == 0x10);
  }
}

static void test_compare_and_jump() {
  // Compare PS[0] with PS[1], then each conditional jump over a "set PS[2]" move; PS[2] says whether the jump was taken (0 taken, 1 not).
  struct Case { int jump; uint32_t a, b; bool taken; };
  const Case cases[] = {
      {JUMP, 1, 2, true},
      {JUMP_EQUAL, 5, 5, true},      {JUMP_EQUAL, 5, 6, false},
      {JUMP_NOTEQUAL, 5, 6, true},   {JUMP_NOTEQUAL, 5, 5, false},
      {JUMP_ABOVE, 6, 5, true},      {JUMP_ABOVE, 5, 5, false},      {JUMP_ABOVE, 4, 5, false},
      {JUMP_ABOVEEQ, 6, 5, true},    {JUMP_ABOVEEQ, 5, 5, true},     {JUMP_ABOVEEQ, 4, 5, false},
      {JUMP_BELOW, 4, 5, true},      {JUMP_BELOW, 5, 5, false},      {JUMP_BELOW, 6, 5, false},
      {JUMP_BELOWEQ, 4, 5, true},    {JUMP_BELOWEQ, 5, 5, true},     {JUMP_BELOWEQ, 6, 5, false},
      {JUMP_ABOVE, 0xFFFFFFFF, 1, true},   // (unsigned)
  };
  for (const Case& k : cases) {
    Code c;
    c.op2(COMPARE, PS, 0, PS, 1);
    c.u8(k.jump);
    const size_t target = c.pos();
    c.u16(0);
    c.move(PS, 2, IMM, 1);
    const size_t after = c.pos();
    c.eot();
    // the jump target is relative to the start of the table: its address is 6 (the header) + the offset into the code
    c.patch16(target, 6 + static_cast<uint32_t>(after));
    Run r(one_table(c));
    uint32_t ps[3] = {k.a, k.b, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 3)) == AtomBios::Status::Ok && (ps[2] == 0) == k.taken);
  }
  // Test sets "equal" when the AND of the two is zero.
  for (int i = 0; i < 2; ++i) {
    Code c;
    c.op2(TEST, PS, 0, PS, 1);
    c.u8(JUMP_EQUAL);
    const size_t target = c.pos();
    c.u16(0);
    c.move(PS, 2, IMM, 1);
    c.patch16(target, 6 + static_cast<uint32_t>(c.pos()));
    c.eot();
    Run r(one_table(c));
    uint32_t ps[3] = {0xF0, i == 0 ? 0x0Fu : 0x10u, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 3)) == AtomBios::Status::Ok && (ps[2] == 0) == (i == 0));
  }
  // A loop that counts: PS[0] from 0 to 5.
  {
    Code c;
    const size_t top = c.pos();
    c.op2(ADD, PS, 0, IMM, 1);
    c.op2(COMPARE, PS, 0, IMM, 5);
    c.u8(JUMP_NOTEQUAL).u16(6 + static_cast<uint32_t>(top));
    c.eot();
    Run r(one_table(c));
    uint32_t ps[1] = {0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 1)) == AtomBios::Status::Ok && ps[0] == 5 && r.bios->operations() == 15);
  }
}

static void test_switch_and_tables() {
  // Switch on PS[0]: cases 1, 2, 7 set PS[1] to 10, 20, 70; no match falls through to 99. (A case: 0x63, the value as the source's immediate, the target.)
  for (uint32_t value : {1u, 2u, 7u, 3u}) {
    Code c;
    c.u8(SWITCH).u8(PS | (DWORD << 3)).u8(0);
    const size_t cases = c.pos();
    for (int i = 0; i < 3; ++i) c.u8(0x63).u32(i == 0 ? 1 : i == 1 ? 2 : 7).u16(0);
    c.u16(0x5A5A);
    c.move(PS, 1, IMM, 99);
    c.eot();
    const uint32_t values[3] = {10, 20, 70};
    std::vector<size_t> targets;
    for (int i = 0; i < 3; ++i) {
      targets.push_back(c.pos());
      c.move(PS, 1, IMM, values[i]);
      c.eot();
    }
    for (int i = 0; i < 3; ++i) c.patch16(cases + i * 7 + 5, 6 + static_cast<uint32_t>(targets[i]));
    Run r(one_table(c));
    uint32_t ps[2] = {value, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok);
    CHECK(ps[1] == (value == 1 ? 10u : value == 2 ? 20u : value == 7 ? 70u : 99u));
  }
  // A call: table 1 doubles PS[0] (its parameters are the caller's from the callee's parameter-space size on: the caller's PS shifted by its own PS size / 4); table 0 calls it.
  {
    Image img;
    Code callee;
    callee.op2(ADD, PS, 0, PS, 0);
    callee.eot();
    img.tables.push_back({1, 0, 4, callee});    // PS size 4 bytes: one dword of parameters
    Code caller;
    caller.u8(CALLTABLE).u8(1);
    caller.eot();
    img.tables.push_back({0, 0, 0, caller});
    Run r(img);
    uint32_t ps[2] = {21, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 42);
    // A call of a table that does not exist does nothing.
    Code none;
    none.u8(CALLTABLE).u8(9);
    none.move(PS, 1, IMM, 5);
    none.eot();
    Image img2;
    img2.tables.push_back({0, 0, 0, none});
    Run r2(img2);
    uint32_t ps2[2] = {0, 0};
    CHECK(run_one(r2, ArrayView<uint32_t>(ps2, 2)) == AtomBios::Status::Ok && ps2[1] == 5);
    // The callee's own parameter space size decides what it sees: callee PS size 8 -> ps_shift 2 for ITS callees; here a callee with a callee.
    Image img3;
    Code leaf;
    leaf.move(PS, 0, IMM, 77);
    leaf.eot();
    img3.tables.push_back({2, 0, 0, leaf});
    Code mid;
    mid.u8(CALLTABLE).u8(2);
    mid.eot();
    img3.tables.push_back({1, 0, 8, mid});      // 8 bytes of parameters: its callee's PS[0] is the caller's PS[2]
    Code top;
    top.u8(CALLTABLE).u8(1);
    top.eot();
    img3.tables.push_back({0, 0, 0, top});
    Run r3(img3);
    uint32_t ps3[4] = {1, 2, 3, 4};
    CHECK(run_one(r3, ArrayView<uint32_t>(ps3, 4)) == AtomBios::Status::Ok && ps3[0] == 1 && ps3[1] == 2 && ps3[2] == 77 && ps3[3] == 4);
    // (table 1 declares 8 bytes of parameters, so what it passes on starts at its parameter 2; table 2's PS[0] is that one)
  }
  // The workspace is per table: table 1 sets its WS[0], table 0's is untouched; and it starts at zero every call.
  {
    Image img;
    Code callee;
    callee.move(PS, 0, WS, 0);      // PS[0] <- WS[0] (zero at the start)
    callee.move(WS, 0, IMM, 99);
    callee.eot();
    img.tables.push_back({1, 2, 4, callee});
    Code caller;
    caller.u8(CALLTABLE).u8(1);
    caller.u8(CALLTABLE).u8(1);
    caller.move(PS, 1, WS, 0);
    caller.eot();
    img.tables.push_back({0, 2, 0, caller});
    Run r(img);
    uint32_t ps[2] = {5, 5};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 0 && ps[1] == 0);
  }
}

static void test_ports_and_iio() {
  // Port 0 is the memory-mapped registers. The PCI and SYSIO ports are not implemented: reads give zero and writes are dropped, with a warning.
  {
    Code c;
    c.u8(SETPORT_PCI).u8(0);
    c.move(REG, 0x10, IMM, 5);
    c.move(PS, 0, REG, 0x10);
    c.u8(SETPORT_SYSIO).u8(0);
    c.move(REG, 0x11, IMM, 6);
    c.u8(SETPORT_ATI).u16(0);
    c.move(REG, 0x12, IMM, 7);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[1] = {0x55};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 1)) == AtomBios::Status::Ok && ps[0] == 0 && r.host.writes_to(0x10).empty() && r.host.writes_to(0x11).empty() && r.host.writes_to(0x12).size() == 1);
    CHECK(r.bios->warnings() == 3);
  }
  // An indirect I/O port: the program for port 3 reads a register, so a register read through it gives that register's value.
  //   program: READ reg 0x0100; CLEAR 4 bits at 0; MOVE_INDEX (4 bits from index bit 0 to bit 0)...
  // Read path: temp = reg 0x100; SET 8 bits at 24; END. The register operand is the index (unused here).
  {
    Image img;
    img.iio.push_back({3, {2, 0x00, 0x01,          // READ register 0x0100
                           5, 8, 24}});            // SET 8 bits at bit 24
    Code c;
    c.u8(SETPORT_ATI).u16(3);
    c.move(PS, 0, REG, 0x2222);
    c.eot();
    img.tables.push_back({0, 0, 0, c});
    Run r(img);
    r.host.regs[0x100] = 0x00123456;
    uint32_t ps[1] = {0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 1)) == AtomBios::Status::Ok && ps[0] == 0xFF123456);
  }
  // The programs for writing. The driver reads through the program of port (io_mode & 0x7F) and writes through the one of (io_mode & 0xFF): for a port number with the top bit set,
  // 0x83, a read uses the program of port 3 and a write the program of port 0x83. That program: CLEAR 32 bits at 0 (the temporary is zero); MOVE_INDEX 8 bits from index bit 4 to
  // bit 0; MOVE_DATA 4 bits from data bit 0 to bit 8; MOVE_ATTR 4 bits from attribute bit 0 to bit 16; SET 4 bits at bit 28; WRITE register 0x0300.
  {
    Image img;
    img.iio.push_back({0x83, {4, 32, 0, 6, 8, 4, 0, 8, 4, 0, 8, 7, 4, 0, 16, 5, 4, 28, 3, 0x00, 0x03}});
    img.iio.push_back({3, {2, 0x00, 0x01}});      // (port 3 reads register 0x0100)
    Code c;
    c.u8(SETPORT_ATI).u16(0x83);
    c.move(WS, 0x47, IMM, 0xA);                     // the I/O attributes
    c.move(REG, 0xAB5C, IMM, 0x9);                  // write 9 to register number 0xAB5C: the index is the register number, the data the value
    c.move(PS, 0, REG, 0x2222);                     // a read: through port 3's program: register 0x100
    c.eot();
    img.tables.push_back({0, 0, 0, c});
    Run r(img);
    r.host.regs[0x100] = 0xCAFE;
    uint32_t ps[1] = {0};
    // temporary: 0 | (0xAB5C >> 4) & 0xFF = 0xB5 | 9 << 8 | 0xA << 16 | 0xF << 28 = 0xF00A09B5
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 1)) == AtomBios::Status::Ok && r.host.writes_to(0x300) == std::vector<uint32_t>{0xF00A09B5} && ps[0] == 0xCAFE && r.bios->warnings() == 0);
  }
  // A write to a port that has only the read program (3): dropped, with a warning, because the driver looks for 0x83.
  {
    Image img;
    img.iio.push_back({3, {4, 32, 0, 3, 0x00, 0x03}});
    Code c;
    c.u8(SETPORT_ATI).u16(3);
    c.move(REG, 0x10, IMM, 1);
    c.eot();
    img.tables.push_back({0, 0, 0, c});
    Run r(img);
    CHECK(run_one(r) == AtomBios::Status::Ok && r.host.writes_to(0x300).empty() && r.bios->warnings() == 1);
  }
  // A port with no program: a read gives zero, with a warning.
  {
    Code c;
    c.u8(SETPORT_ATI).u16(9);
    c.move(PS, 0, REG, 0x10);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[1] = {0x77};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 1)) == AtomBios::Status::Ok && ps[0] == 0 && r.bios->warnings() == 1);
  }
  // Register 0 through the data port etc. is untouched. The attributes and the register block are visible in the workspace registers 0x47 and 0x48.
  {
    Code c;
    c.u8(SETREGBLOCK).u16(0x1234);
    c.move(PS, 0, WS, 0x48);
    c.move(WS, 0x47, IMM, 0xBEEF);
    c.move(PS, 1, WS, 0x47);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[2] = {0, 0};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 0x1234 && ps[1] == 0xBEEF);
  }
}

static void test_data_scratch_pll_mc() {
  // SetDataBlock (index 40: the extra data table) and ID operands: the dword at the block plus the operand.
  {
    Image img;
    img.extra_data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    Code c;
    c.u8(SETDATABLOCK).u8(40);
    c.move(PS, 0, ID, 4);
    c.move(PS, 1, WS, 0x42);       // WS 0x42 is the data block pointer
    c.u8(SETDATABLOCK).u8(0);
    c.move(PS, 2, WS, 0x42);
    c.u8(SETDATABLOCK).u8(255);    // 255: the current table
    c.move(PS, 3, WS, 0x42);
    c.eot();
    img.tables.push_back({0, 0, 0, c});
    Run r(img);
    uint32_t ps[4] = {};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 4)) == AtomBios::Status::Ok && ps[0] == 0x88776655 && ps[1] == Image::kExtra && ps[2] == 0 && ps[3] == img.table_at(0));
  }
  // The scratch memory ("framebuffer"): FB[i] at the base from SetFbBase / WS 0x46; bounds are exact.
  {
    Code c;
    c.move(FB, 0, IMM, 0xA1);
    c.move(FB, 3, IMM, 0xA4);
    c.u8(SETFBBASE).u8(IMM).u32(16);
    c.move(FB, 0, IMM, 0xB0);
    c.move(PS, 0, FB, 3);
    c.move(PS, 1, WS, 0x46);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[2] = {};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && r.bios->scratch_word(0) == 0xA1 && r.bios->scratch_word(3) == 0xA4 && r.bios->scratch_word(4) == 0xB0 && ps[0] == 0 && ps[1] == 16);
    CHECK(r.bios->warnings() == 0);
    // at the edge: the last dword of the scratch memory is reachable, the one after it is not (the driver's check lets that one through)
    Code e;
    e.u8(SETFBBASE).u8(IMM).u32(20 * 1024 - 4);
    e.move(FB, 0, IMM, 0xE0);      // the last dword
    e.move(FB, 1, IMM, 0xE1);      // one beyond: refused
    e.move(PS, 0, FB, 1);
    e.eot();
    Run r2(one_table(e));
    uint32_t ps2[1] = {0x99};
    CHECK(run_one(r2, ArrayView<uint32_t>(ps2, 1)) == AtomBios::Status::Ok && r2.bios->scratch_word(5119) == 0xE0 && ps2[0] == 0 && r2.bios->warnings() == 2);
    // a base far beyond
    Code f;
    f.u8(SETFBBASE).u8(IMM).u32(0xFFFFFFFF);
    f.move(FB, 255, IMM, 1);
    f.move(PS, 0, FB, 255);
    f.eot();
    Run r3(one_table(f));
    uint32_t ps3[1] = {0x99};
    CHECK(run_one(r3, ArrayView<uint32_t>(ps3, 1)) == AtomBios::Status::Ok && ps3[0] == 0);
  }
  // PLL and MC registers go to the host's.
  {
    Code c;
    c.move(PLL, 9, IMM, 0x1111);
    c.move(MC, 4, IMM, 0x2222);
    c.move(PS, 0, PLL, 9);
    c.move(PS, 1, MC, 4);
    c.eot();
    Run r(one_table(c));
    uint32_t ps[2] = {};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && r.host.pll[9] == 0x1111 && r.host.mc[4] == 0x2222 && ps[0] == 0x1111 && ps[1] == 0x2222);
  }
  // Parameter and workspace indexes out of range: a warning, the read gives 0xCDCDCDCD, the write is dropped.
  {
    Code c;
    c.move(PS, 9, IMM, 1);
    c.move(PS, 0, PS, 9);
    c.move(WS, 200, IMM, 1);
    c.move(PS, 1, WS, 200);
    c.eot();
    Run r(one_table(c, 4));
    uint32_t ps[2] = {};
    CHECK(run_one(r, ArrayView<uint32_t>(ps, 2)) == AtomBios::Status::Ok && ps[0] == 0xCDCDCDCD && ps[1] == 0xCDCDCDCD && r.bios->warnings() == 4);
  }
}

static void test_delays_and_misc() {
  Code c;
  c.u8(DELAY_US).u8(30);
  c.u8(DELAY_MS).u8(7);
  c.u8(POSTCARD).u8(0xAA);
  c.u8(BEEP);
  c.u8(NOP);
  c.u8(DEBUG_OP).u8(1);
  c.u8(REPEAT);
  c.u8(100);       // (SaveReg: does nothing, takes no operand)
  c.u8(PROCESSDS).u16(3).u8(0xFF).u8(0xFF).u8(0xFF);   // skips three bytes of data
  c.move(PS, 0, IMM, 9);
  c.eot();
  c.move(PS, 0, IMM, 1);   // (after the end)
  Run r(one_table(c));
  uint32_t ps[1] = {};
  CHECK(run_one(r, ArrayView<uint32_t>(ps, 1)) == AtomBios::Status::Ok && r.host.delayed_us == 30 && r.host.delayed_ms == 7 && ps[0] == 9);
  // Opcode 0 and the ones past the table end a table too.
  Code z;
  z.move(PS, 0, IMM, 5);
  z.u8(0);
  z.move(PS, 0, IMM, 6);
  Run rz(one_table(z));
  uint32_t zp[1] = {};
  CHECK(run_one(rz, ArrayView<uint32_t>(zp, 1)) == AtomBios::Status::Ok && zp[0] == 5);
  Code y;
  y.move(PS, 0, IMM, 5);
  y.u8(127);
  y.move(PS, 0, IMM, 6);
  Run ry(one_table(y));
  CHECK(run_one(ry, ArrayView<uint32_t>(zp, 1)) == AtomBios::Status::Ok && zp[0] == 5);
}

static void test_limits() {
  // A loop that jumps back to itself: the 20-second rule, by the host's clock.
  {
    Code c;
    c.u8(JUMP).u16(6);
    Run r(one_table(c));
    r.host.step = 1000000;    // a second a look at the clock
    CHECK(run_one(r) == AtomBios::Status::Stuck);
    CHECK(r.host.us > 20ull * 1000 * 1000 && r.host.us < 25ull * 1000 * 1000);
  }
  // The same loop with a clock that does not move: the operation limit ends it.
  {
    Code c;
    c.u8(JUMP).u16(6);
    Run r(one_table(c));
    r.host.step = 0;
    r.bios->set_max_operations(1000);
    CHECK(run_one(r) == AtomBios::Status::TooLong && r.bios->operations() > 1000 && r.bios->operations() < 1100);
  }
  // A table that calls itself: too deep.
  {
    Image img;
    Code c;
    c.u8(CALLTABLE).u8(0);
    c.eot();
    img.tables.push_back({0, 0, 0, c});
    Run r(img);
    CHECK(run_one(r) == AtomBios::Status::TooDeep);
  }
  // Two tables that call each other, a failure deep in the chain: the cause comes out.
  {
    Image img;
    Code a;
    a.u8(CALLTABLE).u8(1);
    a.eot();
    Code b;
    b.u8(CALLTABLE).u8(0);
    b.eot();
    img.tables.push_back({0, 0, 0, a});
    img.tables.push_back({1, 0, 0, b});
    Run r(img);
    CHECK(run_one(r) == AtomBios::Status::TooDeep);
  }
  // A table past the end of the image, a table that runs off the end, a table number that does not exist.
  {
    Image img = one_table(Code().eot());
    std::vector<uint8_t> bytes = img.build();
    bytes[Image::kCmd + 4] = 0xFE;   // table 0 at 0x...FE: near the end of the image
    bytes[Image::kCmd + 5] = 0x3F;
    TestAllocator alloc;
    Host host;
    AtomBios bios(host, ConstByteView(bytes.data(), bytes.size()), alloc);
    CHECK(bios.valid() && bios.execute_table(0, ArrayView<uint32_t>()) == AtomBios::Status::BadRead);
    Run r(one_table(Code().eot()));
    CHECK(r.bios->execute_table(1, ArrayView<uint32_t>()) == AtomBios::Status::NoTable && r.bios->execute_table(255, ArrayView<uint32_t>()) == AtomBios::Status::NoTable);
    Code runs_off;
    for (int i = 0; i < 4; ++i) runs_off.move(PS, 0, IMM, 1);   // no end
    Image img2 = one_table(runs_off);
    std::vector<uint8_t> b2 = img2.build();
    // the table is followed by zeros: opcode 0 ends it, so this is an ordinary end; put it at the very end of the image instead
    const uint32_t at = Image::kImageBytes - 12;
    b2[Image::kCmd + 4] = at & 0xFF;
    b2[Image::kCmd + 5] = at >> 8;
    for (uint32_t i = 0; i < 12; ++i) b2[at + i] = 0;
    b2[at + 6] = MOVE + kind(PS);
    b2[at + 7] = IMM;
    b2[at + 8] = 0;
    b2[at + 9] = b2[at + 10] = b2[at + 11] = 0xFF;   // the immediate dword: its last byte is beyond the end of the image
    Host host2;
    TestAllocator alloc2;
    AtomBios bios2(host2, ConstByteView(b2.data(), b2.size()), alloc2);
    uint32_t ps[1] = {};
    CHECK(bios2.valid() && bios2.execute_table(0, ArrayView<uint32_t>(ps, 1)) != AtomBios::Status::Ok);
  }
  // A switch with a bad case, and one that never ends.
  {
    Code c;
    c.u8(SWITCH).u8(IMM).u32(1);
    c.u8(0x12);
    c.eot();
    Run r(one_table(c));
    CHECK(run_one(r) == AtomBios::Status::Ok && r.bios->warnings() == 1);
  }
}

static void test_asic_init() {
  // The POST: table 0 with the firmware info's default engine and memory clocks as its first two parameters.
  Code c;
  c.move(REG, 0x700, PS, 0);
  c.move(REG, 0x701, PS, 1);
  c.move(REG, 0x702, PS, 2);
  c.eot();
  Image img = one_table(c);
  Run r(img);
  CHECK(r.bios->asic_init() == AtomBios::Status::Ok);
  CHECK(r.host.writes_to(0x700) == std::vector<uint32_t>{30000} && r.host.writes_to(0x701) == std::vector<uint32_t>{40000} && r.host.writes_to(0x702) == std::vector<uint32_t>{0});
  // No default clocks: not run. No table 0: not run.
  Image no_sclk = img;
  no_sclk.sclk = 0;
  Run r2(no_sclk);
  CHECK(r2.bios->asic_init() == AtomBios::Status::NoClocks && r2.host.log.empty());
  Image no_mclk = img;
  no_mclk.mclk = 0;
  Run r3(no_mclk);
  CHECK(r3.bios->asic_init() == AtomBios::Status::NoClocks);
  Image no_table;
  no_table.tables.push_back({5, 0, 0, Code().eot()});
  Run r4(no_table);
  CHECK(r4.bios->asic_init() == AtomBios::Status::NoTable && r4.host.log.empty());
  // A failing table: its status comes out.
  Code loop;
  loop.u8(JUMP).u16(6);
  Run r5(one_table(loop));
  r5.host.step = 1000000;
  CHECK(r5.bios->asic_init() == AtomBios::Status::Stuck);
  // The image has no firmware info table.
  Image no_fwi = img;
  no_fwi.with_firmware_info = false;
  Run r6(no_fwi);
  CHECK(!r6.bios->valid() || r6.bios->asic_init() != AtomBios::Status::Ok);
}

// Truncated and bit-flipped images: every outcome is an error status or a clean run; the sanitizers watch for a read outside the image.
static void test_damaged_images() {
  Image img;
  Code init;
  init.move(REG, 0x700, PS, 0);
  init.op2(ADD, PS, 3, IMM, 5);
  init.op2(COMPARE, PS, 3, IMM, 100);
  init.u8(JUMP_ABOVE).u16(6 + 0);
  init.u8(CALLTABLE).u8(1);
  init.u8(SETPORT_ATI).u16(3);
  init.move(PS, 2, REG, 0x20);
  init.eot();
  Code sub;
  sub.move(WS, 0, IMM, 3);
  sub.u8(SETDATABLOCK).u8(40);
  sub.move(PS, 0, ID, 0);
  sub.move(FB, 1, WS, 0);
  sub.eot();
  img.tables.push_back({0, 4, 16, init});
  img.tables.push_back({1, 4, 4, sub});
  img.extra_data = {1, 2, 3, 4, 5, 6, 7, 8};
  img.iio.push_back({3, {2, 0x00, 0x01, 5, 8, 24}});
  const std::vector<uint8_t> good = img.build();
  {
    Run r(img);
    const AtomBios::Status st = r.bios->asic_init();
    if (st != AtomBios::Status::Ok) printf("  asic_init: %s\n", AtomBios::status_name(st));
    CHECK(st == AtomBios::Status::Ok);
  }
  uint32_t outcomes[16] = {};
  auto try_image = [&](const std::vector<uint8_t>& bytes) {
    TestAllocator alloc;
    Host host;
    host.step = 100000;
    AtomBios bios(host, ConstByteView(bytes.data(), bytes.size()), alloc);
    bios.set_max_operations(2000);
    if (!bios.valid()) { ++outcomes[0]; return; }
    const AtomBios::Status s = bios.asic_init();
    ++outcomes[static_cast<uint32_t>(s) + 1];
  };
  // every length
  for (size_t n = 0; n <= good.size(); n += 7) try_image(std::vector<uint8_t>(good.begin(), good.begin() + n));
  // a bit flip in every byte of the part of the image that is used (the first 0x800 bytes: headers, data tables, the programs) and the tables
  for (size_t at = 0; at < 0x700; ++at) {
    for (int bit : {0, 3, 7}) {
      std::vector<uint8_t> bad = good;
      bad[at] ^= static_cast<uint8_t>(1u << bit);
      try_image(bad);
    }
  }
  // the whole image filled with one byte value
  for (int v : {0x00, 0x01, 0x5A, 0x63, 0x7F, 0x80, 0xFF}) {
    std::vector<uint8_t> bad(good.size(), static_cast<uint8_t>(v));
    for (size_t i = 0; i < 0x100; ++i) bad[i] = good[i];
    try_image(bad);
  }
  // Something of each kind happened (and nothing escaped the image: the sanitizers would have said so).
  CHECK(outcomes[0] > 0 && outcomes[static_cast<uint32_t>(AtomBios::Status::Ok) + 1] > 0);
}

int main() {
  test_parse();
  test_move_and_registers();
  test_alignments();
  test_arithmetic();
  test_compare_and_jump();
  test_switch_and_tables();
  test_ports_and_iio();
  test_data_scratch_pll_mc();
  test_delays_and_misc();
  test_limits();
  test_asic_init();
  test_damaged_images();
  printf(g_failed ? "AtomBiosTest: %d FAILED\n" : "AtomBiosTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
