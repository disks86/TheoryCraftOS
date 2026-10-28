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

// Differential test of the AtomBIOS interpreter: random but well formed images are run by this OS's AtomBios and by the Linux driver's interpreter (atom.c, compiled in user space,
// linux_side.c) against identical simulated cards, and everything the interpreter does must agree: the result, every register, memory controller and PLL access and delay in order,
// the parameter space and the scratch memory afterwards. Run it with scripts/atom_differential/run.sh. The programs avoid what is undefined in C in the driver (shifts by 32 or more,
// reads beyond the scratch memory or the image), where AtomBios deliberately differs, and have no backward jumps (the driver's loop rule runs on the wall clock).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "../../tests/AtomBiosImage.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"

using namespace hardware;
using namespace atomimage;

extern "C" {
typedef struct {
  uint32_t (*reg_read)(void*, uint32_t);
  void (*reg_write)(void*, uint32_t, uint32_t);
  uint32_t (*mc_read)(void*, uint32_t);
  void (*mc_write)(void*, uint32_t, uint32_t);
  uint32_t (*pll_read)(void*, uint32_t);
  void (*pll_write)(void*, uint32_t, uint32_t);
  void (*delay_us)(void*, unsigned);
  void (*delay_ms)(void*, unsigned);
  void* user;
} diff_host;
int linux_run(unsigned char* image, int table, uint32_t* ps, int ps_size, uint32_t* scratch_out, diff_host* host, int* parsed, uint32_t* version);
}

struct Event {
  char kind;
  uint32_t a, b;
  bool operator==(const Event& o) const { return kind == o.kind && a == o.a && b == o.b; }
};

// A card whose reads depend on the register and on how often it was read, so a polling loop or a compare sees different values.
struct SimHost final : IAtomHost {
  std::vector<Event> log;
  uint32_t reads[3] = {0, 0, 0};
  uint32_t seed;
  explicit SimHost(uint32_t s) : seed(s) {}
  uint32_t value(int space, uint32_t reg) {
    uint32_t x = seed * 2654435761u + static_cast<uint32_t>(space) * 40503u + reg * 2246822519u + (++reads[space]) * 3266489917u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (x & 3) == 0 ? (x & 0xFF) : x;   // (now and then a small number, so compares and masks see both)
  }
  uint32_t read_register(uint32_t r) override { const uint32_t v = value(0, r); log.push_back({'r', r, v}); return v; }
  void write_register(uint32_t r, uint32_t v) override { log.push_back({'w', r, v}); }
  uint32_t read_mc(uint32_t r) override { const uint32_t v = value(1, r); log.push_back({'m', r, v}); return v; }
  void write_mc(uint32_t r, uint32_t v) override { log.push_back({'M', r, v}); }
  uint32_t read_pll(uint32_t r) override { const uint32_t v = value(2, r); log.push_back({'p', r, v}); return v; }
  void write_pll(uint32_t r, uint32_t v) override { log.push_back({'P', r, v}); }
  void delay_us(uint32_t n) override { log.push_back({'d', n, 0}); }
  void delay_ms(uint32_t n) override { log.push_back({'D', n, 0}); }
  uint64_t now_us() override { return 0; }
};
static SimHost* cast(void* p) { return static_cast<SimHost*>(p); }
static uint32_t cb_rr(void* h, uint32_t r) { return cast(h)->read_register(r); }
static void cb_rw(void* h, uint32_t r, uint32_t v) { cast(h)->write_register(r, v); }
static uint32_t cb_mr(void* h, uint32_t r) { return cast(h)->read_mc(r); }
static void cb_mw(void* h, uint32_t r, uint32_t v) { cast(h)->write_mc(r, v); }
static uint32_t cb_pr(void* h, uint32_t r) { return cast(h)->read_pll(r); }
static void cb_pw(void* h, uint32_t r, uint32_t v) { cast(h)->write_pll(r, v); }
static void cb_du(void* h, unsigned n) { cast(h)->delay_us(n); }
static void cb_dm(void* h, unsigned n) { cast(h)->delay_ms(n); }

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<uint32_t>(s >> 11); }
  uint32_t below(uint32_t n) { return next() % n; }
  bool chance(uint32_t percent) { return below(100) < percent; }
};

// Random operations. Every operand is well formed (kinds, sizes), the indexes are mostly in range and sometimes not (both interpreters carry on the same way), and nothing asks for what C
// leaves undefined.
struct Gen {
  Rng& rng;
  uint32_t ws_count, ps_count;
  int level;   // 0: may call tables 1 and 2; 1: may call table 2; 2: calls nothing
  std::vector<size_t> labels;
  std::vector<std::pair<size_t, int>> fixups;   // a 16-bit target to patch, and which later label it takes (relative to the label list size at the time: resolved at the end)
  explicit Gen(Rng& r, uint32_t ws, uint32_t ps, int lv) : rng(r), ws_count(ws), ps_count(ps), level(lv) {}

  int dst_kind() { static const int k[] = {REG, PS, WS, FB, PLL, MC}; return k[rng.below(6)]; }
  int src_kind() { static const int k[] = {REG, PS, WS, FB, ID, IMM, IMM, IMM, PLL, MC}; return k[rng.below(10)]; }
  uint32_t index_of(int k, bool is_dst) {
    switch (k) {
      case REG: return rng.chance(10) ? 0 : rng.below(0x40);
      case PS: return rng.chance(8) ? ps_count + rng.below(4) : rng.below(ps_count);
      case WS: {
        if (rng.chance(25)) {
          // the special registers: not the ones whose writing makes later accesses undefined (the shift, the data block and the framebuffer window are set by their own operations)
          static const uint32_t readable[] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48};
          static const uint32_t writable[] = {0x40, 0x41, 0x47, 0x48, 0x44, 0x45};
          return is_dst ? writable[rng.below(6)] : readable[rng.below(9)];
        }
        return rng.chance(8) ? ws_count + rng.below(3) : rng.below(ws_count ? ws_count : 1);
      }
      case FB: return rng.below(200);
      case ID: return rng.below(0x80);
      case PLL: return rng.below(16);
      default: return rng.below(16);   // MC
    }
  }
  // A source operand, with an alignment; an immediate for the shifts must be small.
  void src(Code& c, int k, int align, bool small) {
    if (k == IMM) {
      uint32_t v = rng.chance(30) ? rng.below(40) : rng.next();
      if (small) v %= 32;
      c.operand(IMM, v, align);
    } else {
      c.operand(k, index_of(k, false), align);
    }
  }
  void binary(Code& c, int base, bool compare_like = false) {
    const int dk = dst_kind(), sk = src_kind();
    const int align = rng.below(8), dalign = rng.below(4);
    c.u8(base + kind(dk));
    c.u8(sk | (align << 3) | (dalign << 6));
    c.operand(dk, index_of(dk, !compare_like), align);
    src(c, sk, align, false);
  }
  void shift_by_byte(Code& c, int base) {
    const int dk = dst_kind();
    const int align = rng.below(8), dalign = rng.below(4);
    c.u8(base + kind(dk));
    c.u8(IMM | (align << 3) | (dalign << 6));
    c.operand(dk, index_of(dk, true), align);
    c.u8(rng.below(32));   // the shift amount: a direct byte
  }
  void shift_by_src(Code& c, int base) {
    const int dk = dst_kind();
    const int align = rng.below(8), dalign = rng.below(4);
    c.u8(base + kind(dk));
    c.u8(IMM | (align << 3) | (dalign << 6));
    c.operand(dk, index_of(dk, true), align);
    src(c, IMM, align, true);
  }
  void mask(Code& c) {
    const int dk = dst_kind(), sk = src_kind();
    const int align = rng.below(8), dalign = rng.below(4);
    c.u8(MASK + kind(dk));
    c.u8(sk | (align << 3) | (dalign << 6));
    c.operand(dk, index_of(dk, true), align);
    c.operand(IMM, rng.next(), align);   // the mask: a direct value of the destination's alignment
    src(c, sk, align, false);
  }
  void clear(Code& c) {
    const int dk = dst_kind();
    const int align = rng.below(8), dalign = rng.below(4);
    c.u8(CLEAR + kind(dk));
    c.u8(IMM | (align << 3) | (dalign << 6));
    c.operand(dk, index_of(dk, true), align);
  }
  void wide(Code& c, int opcode) {   // Mul32 / Div32: parameter space or workspace destination
    const int dk = (opcode == MUL32_PS || opcode == DIV32_PS) ? PS : WS;
    const int sk = src_kind();
    const int align = rng.below(8), dalign = rng.below(4);
    c.u8(opcode);
    c.u8(sk | (align << 3) | (dalign << 6));
    c.operand(dk, index_of(dk, false), align);
    src(c, sk, align, false);
  }
  void jump(Code& c, int op) {
    c.u8(op);
    fixups.push_back({c.pos(), 0});
    c.u16(0);
  }
  void one_op(Code& c) {
    labels.push_back(c.pos());
    uint32_t pick = rng.below(100);
    if (const char* only = getenv("ATOM_ONLY")) {   // for chasing a difference: only the operations whose pick range contains this number
      std::vector<uint32_t> wanted;
      for (const char* q = only; *q;) {
        char* end = nullptr;
        wanted.push_back(static_cast<uint32_t>(strtoul(q, &end, 0)));
        q = *end == ',' ? end + 1 : end;
      }
      pick = wanted[rng.below(static_cast<uint32_t>(wanted.size()))];
    }
    if (pick < 22) binary(c, MOVE);
    else if (pick < 28) binary(c, ADD);
    else if (pick < 33) binary(c, SUB);
    else if (pick < 38) binary(c, AND);
    else if (pick < 42) binary(c, OR);
    else if (pick < 46) binary(c, XOR);
    else if (pick < 49) binary(c, MUL);
    else if (pick < 52) binary(c, DIV);
    else if (pick < 54) wide(c, MUL32_PS + rng.below(2));
    else if (pick < 56) wide(c, DIV32_PS + rng.below(2));
    else if (pick < 60) mask(c);
    else if (pick < 63) clear(c);
    else if (pick < 66) shift_by_byte(c, SHL_OP);
    else if (pick < 69) shift_by_byte(c, SHR_OP);
    else if (pick < 71) shift_by_src(c, SHL);
    else if (pick < 73) shift_by_src(c, SHR);
    else if (pick < 79) { binary(c, rng.chance(50) ? COMPARE : TEST, true); if (rng.chance(80)) jump(c, JUMP + rng.below(7)); }
    else if (pick < 81) { c.u8(DELAY_US).u8(rng.below(200)); }
    else if (pick < 82) { c.u8(DELAY_MS).u8(rng.below(20)); }
    else if (pick < 85) { const int port = rng.below(6); if (port == 0) c.u8(SETPORT_ATI).u16(0); else if (port < 4) { static const uint32_t ports[] = {3, 4, 5, 0x83, 0x84, 0x85}; c.u8(SETPORT_ATI).u16(ports[rng.below(6)]); } else if (port == 4) c.u8(SETPORT_PCI).u8(0); else c.u8(SETPORT_SYSIO).u8(0); }
    else if (pick < 86) { c.u8(SETREGBLOCK).u16(rng.chance(50) ? 0 : rng.below(0x400)); }
    else if (pick < 87) { c.u8(SETFBBASE).u8(IMM).u32(4 * rng.below(1200)); }
    else if (pick < 89) { static const uint32_t idx[] = {0, 40, 255}; c.u8(SETDATABLOCK).u8(idx[rng.below(3)]); }
    else if (pick < 92 && level < 2) { c.u8(CALLTABLE).u8(level == 0 ? 1 + rng.below(2) : 2); }
    else if (pick < 93) { c.u8(CALLTABLE).u8(9); }
    else if (pick < 95) { c.u8(rng.chance(50) ? POSTCARD : DEBUG_OP).u8(rng.next()); }
    else if (pick < 96) { c.u8(NOP); }
    else if (pick < 97) { c.u8(PROCESSDS).u16(2).u8(rng.next()).u8(rng.next()); }
    else if (pick < 99) {   // a switch on a source, two cases (one likely to match), then falls through
      const int sk = src_kind();
      const int align = rng.below(8);
      c.u8(SWITCH).u8(sk | (align << 3));
      src(c, sk, align, false);
      for (int i = 0; i < 2; ++i) {
        c.u8(0x63);
        c.operand(IMM, rng.chance(50) ? rng.below(16) : rng.next(), align);
        fixups.push_back({c.pos(), 0});
        c.u16(0);
      }
      c.u16(0x5A5A);
    } else {
      c.u8(BEEP);
    }
  }
  void table(Code& c, int ops) {
    for (int i = 0; i < ops; ++i) one_op(c);
    labels.push_back(c.pos());
    c.eot();
    // patch the jump and case targets to later operation boundaries (relative to the table's start: the 6 byte header plus the offset into the code)
    for (auto& f : fixups) {
      size_t later = 0;
      for (size_t i = 0; i < labels.size(); ++i) if (labels[i] > f.first) { later = i; break; }
      const size_t span = labels.size() - later;
      const size_t pick = later + rng.below(static_cast<uint32_t>(span));
      c.patch16(f.first, 6 + static_cast<uint32_t>(labels[pick]));
    }
  }
};

static void gen_iio(Rng& rng, Image& img) {
  for (uint32_t port : {3u, 4u, 5u, 0x83u, 0x84u, 0x85u}) {   // (a port 0x8n is also reached by a write to port n: the driver's write index keeps the 0x80)
    Iio p;
    p.port = port;
    const int n = 1 + rng.below(6);
    for (int i = 0; i < n; ++i) {
      switch (rng.below(8)) {
        case 0: p.program.push_back(0); break;                                                   // NOP
        case 1: p.program.push_back(2); p.program.push_back(rng.below(256)); p.program.push_back(rng.below(4)); break;   // READ
        case 2: p.program.push_back(3); p.program.push_back(rng.below(256)); p.program.push_back(rng.below(4)); break;   // WRITE
        case 3: p.program.push_back(4); p.program.push_back(1 + rng.below(32)); p.program.push_back(rng.below(32)); break;   // CLEAR
        case 4: p.program.push_back(5); p.program.push_back(1 + rng.below(32)); p.program.push_back(rng.below(32)); break;   // SET
        case 5: p.program.push_back(6); p.program.push_back(1 + rng.below(32)); p.program.push_back(rng.below(32)); p.program.push_back(rng.below(32)); break;   // MOVE_INDEX
        case 6: p.program.push_back(8); p.program.push_back(1 + rng.below(32)); p.program.push_back(rng.below(32)); p.program.push_back(rng.below(32)); break;   // MOVE_DATA
        default: p.program.push_back(7); p.program.push_back(1 + rng.below(32)); p.program.push_back(rng.below(32)); p.program.push_back(rng.below(32)); break;   // MOVE_ATTR
      }
    }
    img.iio.push_back(p);
  }
}

int main(int argc, char** argv) {
  const uint32_t first = argc > 1 ? static_cast<uint32_t>(strtoul(argv[1], nullptr, 0)) : 1;
  const uint32_t count = argc > 2 ? static_cast<uint32_t>(strtoul(argv[2], nullptr, 0)) : 2000;
  uint32_t compared = 0, skipped = 0, differing = 0, failed_runs = 0, total_events = 0;
  for (uint32_t seed = first; seed < first + count; ++seed) {
    Rng rng(seed);
    Image img;
    img.sclk = 30000;
    img.mclk = 40000;
    img.extra_data.resize(64);
    for (uint8_t& b : img.extra_data) b = static_cast<uint8_t>(rng.next());
    gen_iio(rng, img);
    // The parameter spaces: table 0 gets 16 dwords and declares 2 (a callee's parameters start 2 dwords into the caller's), table 1 so gets 14 and declares 2, table 2 gets 12. (The
    // driver passes a negative size to a callee when the caller's declared size is bigger than what it was given, and then reads beyond the array.)
    const uint32_t ps0 = 8, ps1 = 8, ps2 = 4;
    for (int t = 2; t >= 0; --t) {
      Code c;
      Gen g(rng, 6, 16 - 2 * t, t);
      const char* fixed = getenv("ATOM_NOPS");
      g.table(c, fixed ? static_cast<int>(strtoul(fixed, nullptr, 0)) : 4 + rng.below(t == 0 ? 60 : 30));
      img.tables.push_back({static_cast<uint32_t>(t), 6, t == 0 ? ps0 : t == 1 ? ps1 : ps2, c});
    }
    std::vector<uint8_t> bytes = img.build();
    uint32_t init[16];
    for (uint32_t& v : init) v = rng.chance(30) ? rng.below(64) : rng.next();

    // this OS's interpreter first: a run that hits its limits is skipped
    SimHost mine(seed);
    TestAllocator alloc;
    AtomBios bios(mine, ConstByteView(bytes.data(), bytes.size()), alloc);
    if (!bios.valid()) { ++skipped; continue; }
    bios.set_max_operations(200000);
    uint32_t ps_mine[16];
    memcpy(ps_mine, init, sizeof init);
    const AtomBios::Status status = bios.execute_table(0, ArrayView<uint32_t>(ps_mine, 16));
    if (status == AtomBios::Status::TooLong || status == AtomBios::Status::TooDeep || status == AtomBios::Status::BadRead) { ++skipped; continue; }

    if (const char* dump = getenv("ATOM_DUMP")) {
      if (static_cast<uint32_t>(strtoul(dump, nullptr, 0)) == seed) {
        const uint32_t at = img.table_at(0);
        printf("seed %u: table 0 at %x:", seed, at);
        for (size_t i = 0; i < img.tables[2].code.b.size() && i < 80; ++i) printf(" %02x", img.tables[2].code.b[i]);
        printf("\n  table 1:");
        for (size_t i = 0; i < img.tables[1].code.b.size() && i < 40; ++i) printf(" %02x", img.tables[1].code.b[i]);
        printf("\n  table 2:");
        for (size_t i = 0; i < img.tables[0].code.b.size() && i < 40; ++i) printf(" %02x", img.tables[0].code.b[i]);
        printf("\n  ps in:");
        for (uint32_t v : init) printf(" %x", v);
        printf("\n  mine:");
        for (size_t i = 0; i < mine.log.size() && i < 14; ++i) printf(" %c%x=%x", mine.log[i].kind, mine.log[i].a, mine.log[i].b);
        printf("\n");
      }
    }
    SimHost theirs(seed);
    diff_host host = {cb_rr, cb_rw, cb_mr, cb_mw, cb_pr, cb_pw, cb_du, cb_dm, &theirs};
    uint32_t ps_linux[16];
    memcpy(ps_linux, init, sizeof init);
    std::vector<uint32_t> scratch(5120, 0);
    int parsed = 0;
    uint32_t version = 0;
    const int r = linux_run(bytes.data(), 0, ps_linux, 16, scratch.data(), &host, &parsed, &version);
    ++compared;
    total_events += static_cast<uint32_t>(mine.log.size());
    bool same = parsed == 1 && version == bios.firmware_revision() && ((r == 0) == (status == AtomBios::Status::Ok)) && memcmp(ps_mine, ps_linux, sizeof ps_mine) == 0;
    same = same && mine.log.size() == theirs.log.size();
    for (size_t i = 0; same && i < mine.log.size(); ++i) same = mine.log[i] == theirs.log[i];
    for (uint32_t w = 0; same && w < 5120; ++w) same = bios.scratch_word(w) == scratch[w];
    if (r != 0) ++failed_runs;
    if (!same) {
      ++differing;
      if (differing <= 5) {
        printf("seed %u differs: result linux %d mine %s, %zu vs %zu events\n", seed, r, AtomBios::status_name(status), theirs.log.size(), mine.log.size());
        for (size_t i = 0; i < mine.log.size() && i < theirs.log.size(); ++i)
          if (!(mine.log[i] == theirs.log[i])) { printf("  first difference at event %zu: linux %c %x %x, mine %c %x %x\n", i, theirs.log[i].kind, theirs.log[i].a, theirs.log[i].b, mine.log[i].kind, mine.log[i].a, mine.log[i].b); break; }
        for (int i = 0; i < 16; ++i) if (ps_mine[i] != ps_linux[i]) { printf("  PS[%d]: linux %x mine %x\n", i, ps_linux[i], ps_mine[i]); break; }
      }
    }
  }
  printf("%u programs compared (%u skipped for the limits), %u differing; %u events in all, %u ended in an error in the driver too\n", compared, skipped, differing, total_events, failed_runs);
  return differing ? 1 : 0;
}
