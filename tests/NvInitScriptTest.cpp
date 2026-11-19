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

// The NVIDIA VBIOS init-script interpreter (Nvkm/NvInitScript) and NvDevinit::post against a Kepler-like image with a BIT 'I' table and
// hand-written scripts, a register model, a VGA port model (CRTC / sequencer index-data pairs at the PRMCIO/PRMVIO mirrors) and fake
// I2C / AUX / PLL / GPIO hooks. The hostile cases: the sample script cut at every length, a few hundred images with random bit flips in
// the scripts and tables, an endless JUMP, self-recursive SUB and SUB_DIRECT, an unknown opcode, delays past the budget. And the GM20x+
// path of NvDevinit::post: the BIT 'p' falcon ucode table, the DEVINIT upload on a PMU falcon model (IMEM/DMEM ports, auto-increment,
// tags, start, the completion bit polled), its timeout, dry run, and hostile tables (cut at every length, random bit flips).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDevinit.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInitScript.hpp"

using namespace hardware;
using namespace hardware::nv;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "NvBiosImages.hpp"

// --- Fakes -------------------------------------------------------------------------------------------------------------------------------

struct FakeRegs final : INvRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  uint64_t delayed = 0;
  uint32_t delays = 0;
  uint32_t read32(uint32_t offset) override {
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint32_t offset, uint32_t value) override {
    regs[offset] = value;
    writes.push_back({offset, value});
  }
  void delay_us(uint32_t us) override { delayed += us; ++delays; }
  uint32_t get(uint32_t offset) { return read32(offset); }
};

// The VGA ports: plain ports and index/data pairs (the index port selects, the data port reads/writes the selected register).
struct FakeVga final : INvVgaIo {
  std::map<uint32_t, uint8_t> ports;                 // plain ports and index ports
  std::map<uint32_t, std::map<uint8_t, uint8_t>> indexed;   // by index port offset
  uint32_t writes = 0;
  static bool index_port(uint32_t off) {
    const uint32_t p = off & 0xfff;
    return p == 0x3c4 || p == 0x3ce || p == 0x3d4;
  }
  uint8_t read8(uint32_t off) override {
    if (index_port(off - 1)) return indexed[off - 1][ports[off - 1]];
    return ports[off];
  }
  void write8(uint32_t off, uint8_t v) override {
    ++writes;
    if (index_port(off - 1)) indexed[off - 1][ports[off - 1]] = v;
    else ports[off] = v;
  }
  uint8_t cr(uint8_t index) { return indexed[0x6013d4][index]; }
};

struct FakeI2c final : II2cBus {
  std::map<uint16_t, uint8_t> regs;   // address << 8 | register
  uint32_t transfers = 0;
  bool fail = false;
  std::vector<uint8_t> last_write;
  bool transfer(ArrayView<Message> messages) override {
    ++transfers;
    if (fail) return false;
    uint8_t reg = 0;
    for (const Message& m : messages) {
      if (m.read) {
        for (size_t i = 0; i < m.data.size(); ++i) m.data[i] = regs[static_cast<uint16_t>(m.address << 8 | (reg + i))];
      } else {
        last_write.assign(m.data.data(), m.data.data() + m.data.size());
        if (m.data.size() >= 1) reg = m.data[0];
        if (m.data.size() == 2) regs[static_cast<uint16_t>(m.address << 8 | reg)] = m.data[1];
      }
    }
    return true;
  }
};

struct FakeAux final : IDpcdAux {
  std::map<uint32_t, uint8_t> dpcd;
  bool dpcd_read(uint32_t address, ByteView out) override {
    for (size_t i = 0; i < out.size(); ++i) out[i] = dpcd[address + i];
    return true;
  }
  bool dpcd_write(uint32_t address, ConstByteView data) override {
    for (size_t i = 0; i < data.size(); ++i) dpcd[address + i] = data[i];
    return true;
  }
};

struct FakeHooks final : INvInitHooks {
  std::vector<std::pair<uint32_t, uint32_t>> plls;
  std::vector<uint8_t> gpio;
  uint32_t meminits = 0;
  FakeI2c bus;
  FakeAux aux;
  bool set_pll(uint32_t type, uint32_t khz) override { plls.push_back({type, khz}); return true; }
  II2cBus* i2c_bus(uint8_t index) override { return index == 2 || index == kInitI2cPrimary ? &bus : nullptr; }
  IDpcdAux* aux_channel(uint8_t i2c_index) override { return i2c_index == 0 ? &aux : nullptr; }
  bool gpio_reset(uint8_t function) override { gpio.push_back(function); return true; }
  bool meminit() override { ++meminits; return true; }
};

struct Trace final : INvInitTrace {
  std::vector<NvInitWrite> all;
  void on_write(const NvInitWrite& w) override { all.push_back(w); }
  uint32_t count(NvInitAccess k) const {
    uint32_t n = 0;
    for (const auto& w : all) n += w.kind == k;
    return n;
  }
};

// --- The image ---------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kInit = 0x2000, kScripts = 0x2020, kCond = 0x2100, kIoCond = 0x2200, kIoFlag = 0x2280, kMacro = 0x2300,
                   kXlat = 0x2400, kRamM = 0x2500, kCode = 0x3000;

// A Kepler-like image (NvBiosImages' kepler_image) whose BIT 'I' entry points at an init table with every pointer, plus a BIT 'M' entry.
static Bytes base_image() {
  Bytes v = kepler_image(false);
  const size_t e = 0x100 + 12;   // the 'I' entry of bit_table
  put16(v, e + 2, 0x12);
  put16(v, e + 4, kInit);
  put8(v, 0x100 + 10, 4);        // a fourth entry: 'M' version 2 (count at +0, xlat pointer at +1)
  put8(v, e + 18, 'M'); put8(v, e + 19, 2); put16(v, e + 20, 3); put16(v, e + 22, kRamM);
  put8(v, kRamM, 4); put16(v, kRamM + 1, 0);
  put16(v, kInit + 0x00, kScripts);
  put16(v, kInit + 0x02, 0);
  put16(v, kInit + 0x04, kMacro);
  put16(v, kInit + 0x06, kCond);
  put16(v, kInit + 0x08, kIoCond);
  put16(v, kInit + 0x0a, kIoFlag);
  put16(v, kInit + 0x0c, 0);
  put16(v, kInit + 0x0e, 0);
  put16(v, kInit + 0x10, kXlat);
  put16(v, kScripts, 0);
  // condition 0: (R[0x1000] & 0xff) == 0x42; condition 1: (R[0x1004] & 1) == 1
  put32(v, kCond + 0, 0x1000); put32(v, kCond + 4, 0xff); put32(v, kCond + 8, 0x42);
  put32(v, kCond + 12, 0x1004); put32(v, kCond + 16, 1); put32(v, kCond + 20, 1);
  // io condition 0: (CR[0x55] & 0x0f) == 0x05
  put16(v, kIoCond, 0x3d4); put8(v, kIoCond + 2, 0x55); put8(v, kIoCond + 3, 0x0f); put8(v, kIoCond + 4, 0x05);
  // macro 0: R[0x2000] = 0x77
  put32(v, kMacro, 0x2000); put32(v, kMacro + 4, 0x77);
  // xlat table 0 -> bytes at 0x2410
  put16(v, kXlat, 0x2410);
  for (int i = 0; i < 16; ++i) put8(v, 0x2410 + i, 0xa0 + i);
  if (v.size() < 0x8000) v.resize(0x8000, 0);
  return v;
}

// A script emitter.
struct Script {
  Bytes* v;
  size_t at;
  Script& b(uint32_t x) { put8(*v, at, x); at += 1; return *this; }
  Script& w(uint32_t x) { put16(*v, at, x); at += 2; return *this; }
  Script& d(uint32_t x) { put32(*v, at, x); at += 4; return *this; }
  Script& zm_reg(uint32_t reg, uint32_t val) { return b(0x7a).d(reg).d(val); }
  Script& nv_reg(uint32_t reg, uint32_t mask, uint32_t val) { return b(0x6e).d(reg).d(mask).d(val); }
  Script& done() { return b(0x71); }
};

static Script script_at(Bytes& v, size_t at) { return Script{&v, at}; }

static const Chip kKepler = chip_of(0x0e4000a2);

struct Run {
  FakeRegs regs;
  FakeVga vga;
  FakeHooks hooks;
  Trace trace;
  NvInitOptions options;
  NvInitResult result;
  bool ok = false;
  bool run(const Bytes& v, uint32_t at, const Chip& chip = kKepler, bool post = false) {
    NvBios bios(view(v));
    NvInitScript s(bios, chip, regs, &vga, &hooks, &trace, options);
    ok = post ? s.post() : s.run(at);
    result = s.result();
    return ok;
  }
};

// --- Tests -------------------------------------------------------------------------------------------------------------------------------

static void test_tables() {
  Bytes v = base_image();
  NvBios bios(view(v));
  const NvInitTables t = find_init_tables(bios);
  CHECK(t.table == kInit);
  CHECK(t.length == 0x12);
  CHECK(t.script_table == kScripts && t.macro_table == kMacro && t.condition_table == kCond && t.io_condition_table == kIoCond);
  CHECK(t.io_flag_condition_table == kIoFlag && t.xlat_table == kXlat && t.function_table == 0 && t.unknown_script == 0);
  FakeRegs regs;
  NvInitScript s(bios, kKepler, regs, nullptr, nullptr, nullptr, NvInitOptions{});
  CHECK(s.bios_major() == 0x80);
  CHECK(s.script_offset(0) == 0);
}

static void test_register_opcodes() {
  Bytes v = base_image();
  script_at(v, kCode)
      .zm_reg(0x1234, 0xdeadbeef)
      .zm_reg(0x000200, 0x10)                               // PMC_ENABLE: bit 0 stays on
      .nv_reg(0x1100, 0xffff0000, 0x00005678)               // (old & mask) | data
      .b(0x58).d(0x1200).b(3).d(1).d(2).d(3)                // ZM_REG_SEQUENCE
      .b(0x91).d(0x1300).b(3).d(7).d(8).d(9)                // ZM_REG_GROUP: one register, three writes
      .b(0x77).d(0x1400).w(0xbeef)                          // ZM_REG16
      .b(0x47).d(0x1500).d(0x0f)                            // ANDN_REG
      .b(0x48).d(0x1504).d(0xf0)                            // OR_REG
      .b(0x90).d(0x1234).d(0x1600)                          // COPY_ZM_REG
      .b(0x97).d(0x1700).d(0xffff0000).d(0x10)              // ZM_MASK_ADD
      .b(0x5f).d(0x1234).b(4).d(0xff).d(0x01).d(0x1800).d(0xffffff00)   // COPY_NV_REG: ((R >> 4) & 0xff) ^ 1 into the low byte
      .b(0x96).d(0x1234).b(0).b(0x0f).b(0).d(0x1900).d(0xffffff00).b(0)   // XLAT: table 0 entry (R & 0xf) = 0xaf
      .b(0x6f).b(0)                                         // MACRO 0
      .b(0x5a).d(0x1a00).w(kMacro + 4)                      // ZM_REG_INDIRECT: R = VBIOS[addr]
      .b(0x92).b(0x8c).b(0x8d).b(0xaa).b(1).b(2).b(3)       // reserved / reset begun / end: skipped
      .done()
      .zm_reg(0x9999, 1);                                   // after DONE: not run
  Run r;
  r.regs.regs[0x1100] = 0xaaaa1111;
  r.regs.regs[0x1500] = 0xff;
  r.regs.regs[0x1700] = 0x1234fff8;
  r.regs.regs[0x1800] = 0x12345678;
  CHECK(r.run(v, kCode));
  CHECK(r.regs.get(0x1234) == 0xdeadbeef);
  CHECK(r.regs.get(0x200) == 0x11);
  CHECK(r.regs.get(0x1100) == 0xaaaa5678);
  CHECK(r.regs.get(0x1200) == 1 && r.regs.get(0x1204) == 2 && r.regs.get(0x1208) == 3);
  CHECK(r.regs.get(0x1300) == 9);
  uint32_t group = 0;
  for (auto& w : r.regs.writes) group += w.first == 0x1300;
  CHECK(group == 3);
  CHECK(r.regs.get(0x1400) == 0xbeef);
  CHECK(r.regs.get(0x1500) == 0xf0);
  CHECK(r.regs.get(0x1504) == 0xf0);
  CHECK(r.regs.get(0x1600) == 0xdeadbeef);
  CHECK(r.regs.get(0x1700) == 0x12340008);   // the low half wraps on its own, the high half is kept
  CHECK(r.regs.get(0x1800) == 0x123456ef);   // ((0xdeadbeef >> 4) & 0xff) ^ 1
  CHECK(r.regs.get(0x1900) == 0xaf);
  CHECK(r.regs.get(0x2000) == 0x77);
  CHECK(r.regs.get(0x1a00) == 0x77);
  CHECK(r.regs.regs.count(0x9999) == 0);
  CHECK(r.result.ok());
}

static void test_conditions() {
  Bytes v = base_image();
  script_at(v, kCode)
      .b(0x75).b(0)                // CONDITION 0: false (R[0x1000] = 0x41)
      .zm_reg(0x3000, 1)           //   skipped
      .b(0x38)                     // NOT: on again
      .zm_reg(0x3004, 1)           //   done
      .b(0x38)                     // NOT: off
      .zm_reg(0x3008, 1)           //   skipped
      .b(0x6b).b(0)                // SUB while off: not followed
      .b(0x72)                     // RESUME
      .zm_reg(0x300c, 1)           //   done
      .b(0x75).b(1)                // CONDITION 1: true (R[0x1004] = 3)
      .zm_reg(0x3010, 1)           //   done
      .b(0x76).b(0)                // IO_CONDITION 0: CR55 & 0xf == 5 (CR55 = 0x35): true
      .zm_reg(0x3014, 1)           //   done
      .b(0x73).d(0x3c).d(0x08)     // STRAP_CONDITION: (R[0x101000] & 0x3c) == 0x08: false (strap 0x0c)
      .zm_reg(0x3018, 1)           //   skipped
      .b(0x5c).w(0)                // JUMP while off: not taken
      .b(0x72)
      .b(0x6d).b(0x0f).b(0x03)     // RAM_CONDITION: (R[0x100000] & 0xf) == 3: true
      .zm_reg(0x301c, 1)
      .b(0x75).b(5)                // CONDITION 5: past the table's real entries but inside the image: reads zeros, (R[0] & 0) == 0
      .zm_reg(0x3020, 1)
      .done();
  Run r;
  r.regs.regs[0x1000] = 0x41;
  r.regs.regs[0x1004] = 3;
  r.regs.regs[0x101000] = 0x0c;
  r.regs.regs[0x100000] = 0x13;
  r.vga.indexed[0x6013d4][0x55] = 0x35;
  CHECK(r.run(v, kCode));
  CHECK(r.regs.regs.count(0x3000) == 0);
  CHECK(r.regs.get(0x3004) == 1);
  CHECK(r.regs.regs.count(0x3008) == 0);
  CHECK(r.regs.get(0x300c) == 1);
  CHECK(r.regs.get(0x3010) == 1);
  CHECK(r.regs.get(0x3014) == 1);
  CHECK(r.regs.regs.count(0x3018) == 0);
  CHECK(r.regs.get(0x301c) == 1);
  CHECK(r.regs.get(0x3020) == 1);

  // A condition met turns nothing off; a CONDITION_TIME that never comes true polls (20 ms, capped retries) then turns execution off.
  Bytes w = base_image();
  script_at(w, kCode).b(0x56).b(0).b(1).zm_reg(0x3100, 1).b(0x72).b(0x56).b(1).b(1).zm_reg(0x3104, 1).done();
  Run r2;
  r2.regs.regs[0x1004] = 1;
  CHECK(r2.run(w, kCode));
  CHECK(r2.regs.regs.count(0x3100) == 0);
  CHECK(r2.regs.get(0x3104) == 1);
  CHECK(r2.regs.delays == 50 && r2.regs.delayed == 50 * 20000);
}

static void test_vga() {
  Bytes v = base_image();
  script_at(v, kCode)
      .b(0x53).b(0x1c).b(0x80)                     // ZM_CR: CR1c = 0x80
      .b(0x52).b(0x1d).b(0xf0).b(0x05)             // CR: CR1d = (CR1d & 0xf0) | 5
      .b(0x54).b(2).b(0x20).b(0x11).b(0x21).b(0x22)   // ZM_CR_GROUP
      .b(0x62).w(0x3c4).b(0x06).b(0x99)            // ZM_INDEX_IO: SR06 = 0x99
      .b(0x78).w(0x3ce).b(0x05).b(0x0f).b(0x40)    // INDEX_IO: GR05 = (GR05 & 0xf) | 0x40
      .b(0x69).w(0x3c2).b(0x0f).b(0x60)            // IO: port 0x3c2 = (port & 0xf) | 0x60
      .b(0x37).d(0x1234).b(0x04).b(0x0f).w(0x3d4).b(0x30).b(0xf0)   // COPY: CR30 = (CR30 & 0xf0) | ((R >> 4) & 0xf)
      .b(0x51).b(0x40).b(0x41).b(0x10).b(2).b(0xaa).b(0xbb)        // CR_INDEX_ADDRESS_LATCHED
      .b(0x32).w(0x3d4).b(0x55).b(0x30).b(4).b(4).d(0x4000).d(10).d(11).d(12).d(13)   // IO_RESTRICT_PROG: (CR55 & 0x30) >> 4 = 3 -> 13
      .b(0x3c).b(0x60)                             // IO_OR: CR60 |= 1 << or (or = 2)
      .b(0x3b).b(0x61)                             // IO_MASK_OR: CR61 &= ~(1 << or)
      .done();
  Run r;
  r.options.or_index = 2;
  r.vga.indexed[0x6013d4][0x1d] = 0xa3;
  r.vga.indexed[0x6013d4][0x30] = 0x5a;
  r.vga.indexed[0x6013d4][0x55] = 0x35;
  r.vga.indexed[0x6013d4][0x61] = 0xff;
  r.vga.indexed[0x6013d4][0x40] = 0x07;
  r.vga.indexed[0x6013c4][0x06] = 0;
  r.vga.indexed[0x6013ce][0x05] = 0x1f;
  r.vga.ports[0x6013c2] = 0x23;
  r.regs.regs[0x1234] = 0x000000c0;
  CHECK(r.run(v, kCode));
  CHECK(r.vga.cr(0x1c) == 0x80);
  CHECK(r.vga.cr(0x1d) == 0xa5);
  CHECK(r.vga.cr(0x20) == 0x11 && r.vga.cr(0x21) == 0x22);
  CHECK(r.vga.indexed[0x6013c4][0x06] == 0x99);   // NV50+: everything at PRMCIO 0x601000 + port
  CHECK(r.vga.indexed[0x6013ce][0x05] == 0x4f);
  CHECK(r.vga.ports[0x6013c2] == 0x63);
  CHECK(r.vga.cr(0x30) == 0x5c);
  CHECK(r.vga.cr(0x41) == 0xbb && r.vga.cr(0x40) == 0x07);   // the data went through CR41 (CR40 latching 0x10, 0x11), CR40 restored
  CHECK(r.regs.get(0x4000) == 13);
  CHECK(r.vga.cr(0x60) == 0x04);
  CHECK(r.vga.cr(0x61) == 0xfb);

  // Before NV50 the sequencer lives in PRMVIO (0x0c03c4) and the CRTC per head; a CR44 = 3 write selects head 1.
  Bytes w = base_image();
  script_at(w, kCode).b(0x62).w(0x3c4).b(0x01).b(0x21).b(0x53).b(0x44).b(3).b(0x53).b(0x19).b(0x77).done();
  Run r2;
  CHECK(r2.run(w, kCode, chip_of(0x046000a1)));
  CHECK(r2.vga.indexed[0x0c03c4][0x01] == 0x21);
  CHECK(r2.vga.indexed[0x6013d4][0x44] == 3);
  CHECK(r2.vga.indexed[0x6033d4][0x19] == 0x77);

  // The 32-bit adapter: a byte write keeps its neighbours, an index/data pair is one write carrying both bytes.
  FakeRegs regs;
  regs.regs[0x6013d4] = 0x44332211;
  RegistersNvVgaIo io(regs);
  CHECK(io.read8(0x6013d5) == 0x22);
  io.write8(0x6013d6, 0xee);
  CHECK(regs.get(0x6013d4) == 0x44ee2211);
  regs.writes.clear();
  io.write_indexed(0x6013d4, 0x1f, 0x57);
  CHECK(regs.writes.size() == 1 && regs.get(0x6013d4) == 0x44ee571f);
  regs.regs[0x0c03cc] = 0x11223344;
  io.write_indexed(0x0c03ce, 0x05, 0x40);
  CHECK(regs.get(0x0c03cc) == 0x40053344);
}

static void test_subs_and_loops() {
  Bytes v = base_image();
  // script table: 0 -> A, 1 -> B, 2 -> 0
  const uint32_t a = kCode, b = kCode + 0x100, c = kCode + 0x200;
  put16(v, kScripts + 0, a);
  put16(v, kScripts + 2, b);
  put16(v, kScripts + 4, 0);
  script_at(v, a).zm_reg(0x5000, 1).b(0x6b).b(1).zm_reg(0x5004, 1).b(0x5b).w(c).zm_reg(0x5008, 1).done();
  script_at(v, b).b(0x97).d(0x5100).d(0).d(1).done();   // B: R[0x5100] += 1
  script_at(v, c)
      .b(0x33).b(3)                                // REPEAT 3
      .b(0x97).d(0x5200).d(0).d(1)                 //   R[0x5200] += 1
      .b(0x36)                                     // END_REPEAT
      .b(0x5c).w(c + 0x40)                         // JUMP over the next write
      .zm_reg(0x5300, 1);
  script_at(v, c + 0x40).zm_reg(0x5304, 1).done();
  Run r;
  CHECK(r.run(v, 0, kKepler, true));   // post: A (which calls B and C), then B again
  CHECK(r.result.scripts == 2);
  CHECK(r.regs.get(0x5000) == 1 && r.regs.get(0x5004) == 1 && r.regs.get(0x5008) == 1);
  CHECK(r.regs.get(0x5100) == 2);
  CHECK(r.regs.get(0x5200) == 3);
  CHECK(r.regs.regs.count(0x5300) == 0);
  CHECK(r.regs.get(0x5304) == 1);
}

static void test_time() {
  Bytes v = base_image();
  script_at(v, kCode)
      .b(0x74).w(500)          // 500 us
      .b(0x74).w(60000)        // 60 ms (rounded as mdelay((us + 900) / 1000))
      .b(0x57).w(65535)        // LTIME 65.5 s: capped to 100 ms
      .b(0x75).b(0).b(0x74).w(900).b(0x72)   // not executing: no delay
      .done();
  Run r;
  CHECK(r.run(v, kCode));
  CHECK(r.regs.delayed == 500 + 60000 + 100000);
  CHECK(r.regs.delays == 3);
  CHECK(r.result.delay_us == 160500);
  CHECK(r.trace.count(NvInitAccess::Delay) == 3);

  // Past the run's total: an error, not a hang.
  Bytes w = base_image();
  script_at(w, kCode).b(0x57).w(1000).b(0x5c).w(kCode);
  Run r2;
  r2.options.total_delay_us = 5000000;
  CHECK(!r2.run(w, kCode));
  CHECK(r2.result.error == NvInitError::DelayBudget);
  CHECK(r2.regs.delayed <= 5000000);
}

static void test_hostile_control_flow() {
  {   // JUMP to itself
    Bytes v = base_image();
    script_at(v, kCode).zm_reg(0x6000, 1).b(0x5c).w(kCode);
    Run r;
    r.options.step_budget = 100000;
    CHECK(!r.run(v, kCode));
    CHECK(r.result.error == NvInitError::StepBudget);
    CHECK(r.result.steps == 100000);
  }
  {   // SUB 0 from script 0
    Bytes v = base_image();
    put16(v, kScripts, kCode);
    put16(v, kScripts + 2, 0);
    script_at(v, kCode).b(0x6b).b(0).done();
    Run r;
    CHECK(!r.run(v, 0, kKepler, true));
    CHECK(r.result.error == NvInitError::TooDeep);
  }
  {   // SUB_DIRECT to itself
    Bytes v = base_image();
    script_at(v, kCode).b(0x5b).w(kCode).done();
    Run r;
    CHECK(!r.run(v, kCode));
    CHECK(r.result.error == NvInitError::TooDeep);
  }
  {   // REPEAT 255 nested in REPEAT 255 nested ...: bounded by the budget
    Bytes v = base_image();
    Script s = script_at(v, kCode);
    for (int i = 0; i < 8; ++i) s.b(0x33).b(255);
    s.b(0x36).done();
    Run r;
    r.options.step_budget = 200000;
    CHECK(!r.run(v, kCode));
    CHECK(r.result.error == NvInitError::StepBudget);
  }
  {   // an unknown opcode
    Bytes v = base_image();
    script_at(v, kCode).zm_reg(0x6100, 1).b(0x00);
    Run r;
    CHECK(!r.run(v, kCode));
    CHECK(r.result.error == NvInitError::UnknownOpcode);
    CHECK(r.result.error_offset == kCode + 9);
    CHECK(r.result.opcode == 0x00);
    CHECK(r.regs.get(0x6100) == 1);
  }
  {   // INIT_CONFIGURE_MEM on a modern BIOS ends the script (nouveau: init_done)
    Bytes v = base_image();
    script_at(v, kCode).b(0x66).zm_reg(0x6200, 1);
    Run r;
    CHECK(r.run(v, kCode));
    CHECK(r.regs.regs.count(0x6200) == 0);
  }
  {   // a register outside BAR0 (or with unknown bits) is not touched
    Bytes v = base_image();
    script_at(v, kCode).zm_reg(0x01000000, 1).zm_reg(0x00fffffc, 2).done();
    Run r;
    CHECK(r.run(v, kCode));
    CHECK(r.regs.writes.size() == 1 && r.regs.get(0x00fffffc) == 2);
    CHECK(r.result.warnings >= 1);
  }
}

static void test_hooks_and_ram() {
  Bytes v = base_image();
  script_at(v, kCode)
      .b(0x79).d(0x4004).w(27000)                  // PLL: 270 MHz
      .b(0x4b).d(0x4008).d(405000)                 // PLL2
      .b(0x4d).b(2).b(0xa0).b(2).b(0x10).b(0x01).b(0x11).b(0x02)   // ZM_I2C_BYTE bus 2, address 0x50
      .b(0x4c).b(2).b(0xa0).b(1).b(0x10).b(0xf0).b(0x08)          // I2C_BYTE: reg 0x10 = (0x01 & 0xf0) | 8
      .b(0x5e).b(2).b(0x50).b(0x11).b(0xff).b(0x02)               // I2C_IF: reg 0x11 == 2 -> stays on
      .zm_reg(0x7000, 1)
      .b(0x5e).b(2).b(0x50).b(0x11).b(0xff).b(0x03)               // I2C_IF false
      .zm_reg(0x7004, 1)
      .b(0x72)
      .b(0x99).d(0x600).b(2).b(0x01).b(0x02)                      // ZM_AUXCH: DPCD 0x600 = 1, then 2
      .b(0x98).d(0x601).b(1).b(0x0f).b(0x30)                      // AUXCH
      .b(0x8e)                                                    // GPIO: reset all
      .b(0xa9).b(2).b(0x07).b(0x08)                               // GPIO_NE: reset every function but 0x07 and 0x08
      .b(0x63)                                                    // COMPUTE_MEM
      .b(0x8f).d(0x7100).b(4).b(2).d(0x10).d(0x11).d(0x12).d(0x13).d(0x20).d(0x21).d(0x22).d(0x23)   // RAM_RESTRICT_ZM_REG_GROUP
      .b(0x87).b(0x04).d(100).d(200).d(300).d(400)                // RAM_RESTRICT_PLL
      .b(0x3a).b(5).b(0)                                          // GENERIC_CONDITION ASSR: DPCD 0x0d bit 0 clear -> off
      .zm_reg(0x7200, 1)
      .b(0x72)
      .done();
  Run r;
  BoardInfo board;
  NvBios bios(view(v));
  (void)parse_board(bios, kKepler, &board);
  r.options.board = &board;
  r.options.output.present = true;
  r.options.output.i2c_index = 0;
  r.regs.regs[0x101000] = 2 << 2;   // RAM strap 2
  CHECK(r.run(v, kCode));
  CHECK(r.hooks.plls.size() == 3);
  CHECK(r.hooks.plls[0] == std::make_pair(0x4004u, 270000u));
  CHECK(r.hooks.plls[1] == std::make_pair(0x4008u, 405000u));
  CHECK(r.hooks.plls[2] == std::make_pair(0x04u, 300u));
  CHECK(r.hooks.bus.regs[0x5010] == 0x08);
  CHECK(r.hooks.bus.regs[0x5011] == 0x02);
  CHECK(r.regs.get(0x7000) == 1);
  CHECK(r.regs.regs.count(0x7004) == 0);
  CHECK(r.hooks.aux.dpcd[0x600] == 2);
  CHECK(r.hooks.aux.dpcd[0x601] == 0x30);
  CHECK(r.hooks.gpio.size() >= 3 && r.hooks.gpio[0] == kInitGpioAll);
  bool reset_hpd2 = false, reset_hpd0 = false;
  for (uint8_t f : r.hooks.gpio) { reset_hpd2 |= f == 0x51; reset_hpd0 |= f == 0x07; }
  CHECK(reset_hpd2 && !reset_hpd0);
  CHECK(r.hooks.meminits == 1);
  CHECK(r.regs.get(0x7100) == 0x12 && r.regs.get(0x7104) == 0x22);
  CHECK(r.regs.regs.count(0x7200) == 0);

  // Without hooks the PLLs are counted, not programmed, and the run goes on.
  Run r2;
  NvBios bios2(view(v));
  NvInitScript s(bios2, kKepler, r2.regs, &r2.vga, nullptr, nullptr, NvInitOptions{});
  CHECK(s.run(kCode));
  CHECK(s.result().plls_unsupported == 3);
  CHECK(s.result().gpio_unsupported == 1);
}

static Bytes sample_image(uint32_t* code_end) {
  Bytes v = base_image();
  put16(v, kScripts, kCode);
  put16(v, kScripts + 2, kCode + 0x200);
  put16(v, kScripts + 4, 0);
  Script s = script_at(v, kCode);
  s.zm_reg(0x1000, 0x42).b(0x75).b(0).nv_reg(0x1100, 0xff00ff00, 0x11).b(0x58).d(0x1200).b(2).d(5).d(6)
      .b(0x53).b(0x1c).b(0x80).b(0x62).w(0x3c4).b(6).b(1).b(0x69).w(0x3c2).b(0x0f).b(0x60)
      .b(0x6b).b(1).b(0x5b).w(kCode + 0x100).b(0x6f).b(0).b(0x33).b(2).b(0x97).d(0x1300).d(0).d(1).b(0x36)
      .b(0x74).w(10).b(0x76).b(0).b(0x38).b(0x91).d(0x1400).b(2).d(1).d(2).b(0x72)
      .b(0x8f).d(0x1500).b(4).b(1).d(1).d(2).d(3).d(4).b(0x96).d(0x1234).b(0).b(0x0f).b(0).d(0x1900).d(0xffffff00).b(0)
      .b(0x4d).b(2).b(0xa0).b(1).b(0x10).b(0x01).b(0x79).d(0x4004).w(2700).b(0x5c).w(kCode + 0x180).done();
  *code_end = static_cast<uint32_t>(s.at);
  script_at(v, kCode + 0x100).zm_reg(0x1600, 1).done();
  script_at(v, kCode + 0x180).b(0x99).d(0x600).b(1).b(1).b(0x8e).done();
  script_at(v, kCode + 0x200).zm_reg(0x1700, 1).done();
  return v;
}

static void test_sample_and_dry_run() {
  uint32_t end = 0;
  Bytes v = sample_image(&end);
  Run live;
  CHECK(live.run(v, 0, kKepler, true));
  CHECK(live.result.scripts == 2);
  CHECK(live.regs.get(0x1000) == 0x42 && live.regs.get(0x1700) == 1 && live.regs.get(0x1300) == 2);

  Run dry;
  dry.options.dry_run = true;
  CHECK(dry.run(v, 0, kKepler, true));
  CHECK(dry.regs.writes.empty());
  CHECK(dry.regs.delays == 0);
  CHECK(dry.vga.writes == 0);
  CHECK(dry.hooks.plls.empty() && dry.hooks.bus.transfers == 0 && dry.hooks.gpio.empty());
  // The same writes, in the same order, as the live run made (the shadow makes the condition and the ZM_MASK_ADD loop come out the same).
  std::vector<std::pair<uint32_t, uint32_t>> dry_mmio;
  for (const auto& w : dry.trace.all)
    if (w.kind == NvInitAccess::Mmio) dry_mmio.push_back({w.address, w.value});
  CHECK(dry_mmio == live.regs.writes);
  CHECK(dry.trace.count(NvInitAccess::VgaIndexed) == live.trace.count(NvInitAccess::VgaIndexed));
  CHECK(dry.trace.count(NvInitAccess::Pll) == 1);

  // Parse mode (nvbios_post with execute = false): nothing at all.
  Run parse;
  parse.options.execute = false;
  CHECK(parse.run(v, 0, kKepler, true));
  CHECK(parse.regs.writes.empty() && parse.trace.all.empty());
}

static void test_truncation_and_flips() {
  uint32_t end = 0;
  const Bytes full = sample_image(&end);
  // Every truncation of the image inside the scripts (and the tables before them).
  uint32_t errors = 0;
  for (uint32_t len = kInit; len <= kCode + 0x210; ++len) {
    Bytes cut(full.begin(), full.begin() + len);
    Run r;
    if (!r.run(cut, 0, kKepler, true)) ++errors;
    CHECK(r.result.steps <= NvInitOptions::kDefaultStepBudget);
  }
  CHECK(errors > 0);
  // Random bit flips in the init table, the tables and the scripts.
  uint32_t seed = 12345;
  auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  for (int round = 0; round < 400; ++round) {
    Bytes v = full;
    const int flips = 1 + round % 8;
    for (int i = 0; i < flips; ++i) {
      const uint32_t span = kCode + 0x210 - kInit;
      const uint32_t at = kInit + rnd() % span;
      v[at] ^= static_cast<uint8_t>(1u << (rnd() % 8));
    }
    Run r;
    r.options.step_budget = 200000;
    r.options.total_delay_us = 2000000;
    BoardInfo board;
    (void)parse_board(NvBios(view(v)), kKepler, &board);
    r.options.board = &board;
    r.options.output.present = round % 2 == 0;
    r.options.head = round % 3 == 0 ? 0 : -1;
    r.options.dry_run = round % 5 == 0;
    (void)r.run(v, 0, kKepler, true);
    CHECK(r.result.steps <= 200000);
    CHECK(r.result.delay_us <= 2000000);
  }
}

static void test_devinit_post() {
  uint32_t end = 0;
  Bytes v = sample_image(&end);
  NvBios bios(view(v));
  {   // a Kepler that was not POSTed: the scripts run
    FakeRegs regs;
    FakeVga vga;
    FakeHooks hooks;
    PostInputs in;
    in.bios = &bios;
    in.registers = &regs;
    in.vga = &vga;
    in.chip = kKepler;
    in.hooks = &hooks;
    const PostReport rep = NvDevinit::post(in);
    CHECK(rep.outcome == PostOutcome::Ran);
    CHECK(rep.needed);
    CHECK(rep.script.ok() && rep.script.scripts == 2);
    CHECK(vga.cr(0x3f) == 0x57);   // the extended CRTC registers unlocked first
    CHECK(regs.get(0x1700) == 1);
    CHECK(rep.posted_after_known && !rep.posted_after);   // (the model does not set 0x2240c)
    CHECK(rep.encoder_scripts_pending);
  }
  {   // already POSTed: nothing (beyond the CRTC unlock) is written
    FakeRegs regs;
    regs.regs[NvDevinit::kPostedGf100] = NvDevinit::kPostedBit;
    FakeVga vga;
    PostInputs in;
    in.bios = &bios;
    in.registers = &regs;
    in.vga = &vga;
    in.chip = kKepler;
    const PostReport rep = NvDevinit::post(in);
    CHECK(rep.outcome == PostOutcome::AlreadyPosted);
    CHECK(regs.writes.empty());
    in.force = true;
    CHECK(NvDevinit::post(in).outcome == PostOutcome::Ran);
  }
  {   // the bit is clear but the CRTC shows the VBIOS ran (gf100_devinit_preinit's fallback)
    FakeRegs regs;
    FakeVga vga;
    vga.indexed[0x6013d4][0x00] = 0x5f;
    PostInputs in;
    in.bios = &bios;
    in.registers = &regs;
    in.vga = &vga;
    in.chip = kKepler;
    CHECK(NvDevinit::post(in).outcome == PostOutcome::AlreadyPosted);
  }
  {   // a Tesla board is judged by CR00/CR1A
    FakeRegs regs;
    FakeVga vga;
    PostInputs in;
    in.bios = &bios;
    in.registers = &regs;
    in.vga = &vga;
    in.chip = chip_of(0x084000a2);
    CHECK(NvDevinit::post(in).outcome == PostOutcome::Ran);
    in.vga = nullptr;
    CHECK(NvDevinit::post(in).outcome == PostOutcome::Refused);
  }
  {   // dry run
    FakeRegs regs;
    Trace trace;
    PostInputs in;
    in.bios = &bios;
    in.registers = &regs;
    in.chip = kKepler;
    in.trace = &trace;
    in.dry_run = true;
    const PostReport rep = NvDevinit::post(in);
    CHECK(rep.outcome == PostOutcome::DryRun);
    CHECK(regs.writes.empty());
    CHECK(trace.count(NvInitAccess::Mmio) > 5);
  }
  {   // refused generations: nothing written
    const uint32_t boots[] = {0x124000a1 /* GM204 */, 0x134000a1 /* GP104 */, 0x046000a1 /* NV46 */};
    for (uint32_t boot0 : boots) {
      FakeRegs regs;
      FakeVga vga;
      PostInputs in;
      in.bios = &bios;
      in.registers = &regs;
      in.vga = &vga;
      in.chip = chip_of(boot0, boot0 >= 0x160000a1 ? 0x16400000 : 0);
      const PostReport rep = NvDevinit::post(in);
      CHECK(rep.outcome == PostOutcome::Refused);
      CHECK(regs.writes.empty() && vga.writes == 0);
    }
  }
  {   // a failing script reports where
    Bytes w = base_image();
    put16(w, kScripts, kCode);
    put16(w, kScripts + 2, 0);
    script_at(w, kCode).zm_reg(0x1000, 1).b(0xff);
    NvBios bad(view(w));
    FakeRegs regs;
    PostInputs in;
    in.bios = &bad;
    in.registers = &regs;
    in.chip = kKepler;
    const PostReport rep = NvDevinit::post(in);
    CHECK(rep.outcome == PostOutcome::Failed);
    CHECK(rep.script.error == NvInitError::UnknownOpcode && rep.script.opcode == 0xff && rep.script.error_offset == kCode + 9);
  }
  {   // no init table
    Bytes w = kepler_image(false);
    put16(w, 0x100 + 12 + 4, 0x7f00);   // 'I' points at zeros
    NvBios none(view(w));
    FakeRegs regs;
    PostInputs in;
    in.bios = &none;
    in.registers = &regs;
    in.chip = kKepler;
    CHECK(NvDevinit::post(in).outcome == PostOutcome::Failed);
  }
}

// --- GM20x .. GV100: the DEVINIT application on the PMU falcon ----------------------------------------------------------------------------

// The PMU falcon as gm200_devinit's upload sees it: the IMEM port (0x10a180 control: address, bit 24 auto-increment, bit 28 secure;
// 0x10a184 data; 0x10a188 tag), the DMEM port (0x10a1c0: address, bit 24 auto-increment on write, bit 25 on read; 0x10a1c4 data), the
// memory scrub status (0x10a10c bits 1-2), HWCFG (0x10a108), CPUCTL start (0x10a100 bit 1) with the boot vector (0x10a104), and the
// application's completion: mailbox 0x10a040 bit 13 set after `halt_after` polls following a start (never with -1).
struct FakeFalcon final : INvRegisters {
  std::map<uint32_t, uint32_t> regs, imem, dmem;
  std::map<uint32_t, bool> imem_secure;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::vector<uint32_t> tags, boot_vectors;
  uint32_t imem_addr = 0, dmem_addr = 0;
  bool imem_inc = false, imem_sec = false, dmem_winc = false, dmem_rinc = false;
  bool started = false, scrub_stuck = false;
  int halt_after = 3, polls = 0;
  uint32_t hwcfg = 0x00020100;   // 64 KiB of IMEM, 64 KiB of DMEM
  uint32_t read32(uint32_t off) override {
    switch (off) {
      case 0x10a1c4: {
        auto it = dmem.find(dmem_addr);
        const uint32_t v = it == dmem.end() ? 0 : it->second;
        if (dmem_rinc) dmem_addr += 4;
        return v;
      }
      case 0x10a10c: return scrub_stuck ? 6u : (regs[off] & ~6u);
      case 0x10a108: return hwcfg;
      case 0x10a040:
        if (started && halt_after >= 0 && ++polls >= halt_after) regs[off] |= 0x2000;
        return regs[off];
      default: {
        auto it = regs.find(off);
        return it == regs.end() ? 0 : it->second;
      }
    }
  }
  void write32(uint32_t off, uint32_t v) override {
    writes.push_back({off, v});
    switch (off) {
      case 0x10a180: imem_addr = v & 0xfffffc; imem_inc = v >> 24 & 1; imem_sec = v >> 28 & 1; break;
      case 0x10a184:
        imem[imem_addr] = v;
        imem_secure[imem_addr] = imem_sec;
        if (imem_inc) imem_addr += 4;
        break;
      case 0x10a188: tags.push_back(v); break;
      case 0x10a1c0: dmem_addr = v & 0xfffffc; dmem_winc = v >> 24 & 1; dmem_rinc = v >> 25 & 1; break;
      case 0x10a1c4:
        dmem[dmem_addr] = v;
        if (dmem_winc) dmem_addr += 4;
        break;
      case 0x10a100:
        if (v & 2) {
          started = true;
          polls = 0;
          boot_vectors.push_back(regs[0x10a104]);
        }
        break;
      default: break;
    }
    regs[off] = v;
  }
  uint32_t count_writes(uint32_t off) const {
    uint32_t n = 0;
    for (const auto& w : writes) n += w.first == off;
    return n;
  }
  size_t first_write(uint32_t off) const {
    for (size_t i = 0; i < writes.size(); ++i)
      if (writes[i].first == off) return i;
    return SIZE_MAX;
  }
};

constexpr uint32_t kPmuBit = 0x4000, kPmuTable = 0x4010, kDevinitApp = 0x4400, kPreOsApp = 0x4800, kPmuTables = 0x6000,
                   kPmuScripts = 0x6100;

static uint32_t boot_word(uint32_t i) { return 0xb0000000u + i; }
static uint32_t code_word(uint32_t i) { return 0xc0000000u + i; }

static void pmu_descriptor(Bytes& v, uint32_t at, uint32_t init, uint32_t args, uint32_t boot_start, uint32_t boot_end, uint32_t code_size,
                           uint32_t data_off, uint32_t data_pmu, uint32_t data_size) {
  put32(v, at + 0x08, init); put32(v, at + 0x0c, args); put32(v, at + 0x10, 0); put32(v, at + 0x14, 0);
  put32(v, at + 0x18, boot_start); put32(v, at + 0x1c, boot_end); put32(v, at + 0x20, code_size);
  put32(v, at + 0x24, data_off); put32(v, at + 0x28, data_pmu); put32(v, at + 0x2c, data_size);
}

// The base image with BIT 'I' long enough for the PMU fields (+0x14 .. +0x1a), and a BIT 'p' (version 2) table of three applications: a
// decoy of type 0x02 without a descriptor, PRE_OS and DEVINIT.
static Bytes pmu_image() {
  Bytes v = base_image();
  put16(v, 0x100 + 12 + 2, 0x1c);   // 'I' length
  put16(v, kInit + 0x14, kPmuTables); put16(v, kInit + 0x16, 0x10);
  put16(v, kInit + 0x18, kPmuScripts); put16(v, kInit + 0x1a, 0x0a);
  for (uint32_t i = 0; i < 0x10; ++i) put8(v, kPmuTables + i, 0x10 + i);
  for (uint32_t i = 0; i < 0x0c; ++i) put8(v, kPmuScripts + i, 0x50 + i);
  add_bit_entry(v, 0x100, 'p', 2, 4, kPmuBit);
  put32(v, kPmuBit, kPmuTable);
  put8(v, kPmuTable, 0x10); put8(v, kPmuTable + 1, 4); put8(v, kPmuTable + 2, 6); put8(v, kPmuTable + 3, 3);
  put8(v, kPmuTable + 4, 0x02); put32(v, kPmuTable + 6, 0);
  put8(v, kPmuTable + 10, kPmuAppPreOs); put32(v, kPmuTable + 12, kPreOsApp);
  put8(v, kPmuTable + 16, kPmuAppDevinit); put32(v, kPmuTable + 18, kDevinitApp);
  // DEVINIT: boot 0x40 bytes at IMEM 0x100, code 0x80 bytes (at 0x140, secure), data 0x30 bytes at DMEM 0x800 holding the argument block:
  // [0x808] = 0x810, [0x818] = 0x1000 (the init tables' target), [0x820] = 0x1800 (the boot scripts').
  pmu_descriptor(v, kDevinitApp, 0x100, 0x800, 0x100, 0x140, 0x80, 0xc0, 0x800, 0x30);
  for (uint32_t i = 0; i < 0x10; ++i) put32(v, kDevinitApp + 0x30 + i * 4, boot_word(i));
  for (uint32_t i = 0; i < 0x20; ++i) put32(v, kDevinitApp + 0x70 + i * 4, code_word(i));
  const uint32_t data = kDevinitApp + 0x30 + 0xc0;
  for (uint32_t i = 0; i < 12; ++i) put32(v, data + i * 4, 0);
  put32(v, data + 0x00, 0xd0);
  put32(v, data + 0x08, 0x810);
  put32(v, data + 0x18, 0x1000);
  put32(v, data + 0x20, 0x1800);
  // PRE_OS: boot 0x100 at IMEM 0x200, code 0x100, data 0x10 at DMEM 0x900.
  pmu_descriptor(v, kPreOsApp, 0x200, 0x900, 0x200, 0x300, 0x100, 0x200, 0x900, 0x10);
  return v;
}

static const Chip kGm204 = chip_of(0x124000a1);

struct PmuRun {
  FakeFalcon regs;
  Trace trace;
  PostInputs in;
  PostReport rep;
  PmuRun(const NvBios& bios, const Chip& chip) {
    in.bios = &bios;
    in.registers = &regs;
    in.chip = chip;
    in.trace = &trace;
  }
  PostReport& run() { return rep = NvDevinit::post(in); }
};

static void test_pmu_table() {
  Bytes v = pmu_image();
  NvBios bios(view(v));
  NvPmuApp app;
  CHECK(find_pmu_app(bios, kPmuAppDevinit, &app) == NvPmuAppStatus::Found);
  CHECK(app.descriptor == kDevinitApp && app.init_addr_pmu == 0x100 && app.args_addr_pmu == 0x800);
  CHECK(app.boot_addr == kDevinitApp + 0x30 && app.boot_addr_pmu == 0x100 && app.boot_size == 0x40);
  CHECK(app.code_addr == kDevinitApp + 0x70 && app.code_addr_pmu == 0x140 && app.code_size == 0x80);
  CHECK(app.data_addr == kDevinitApp + 0xf0 && app.data_addr_pmu == 0x800 && app.data_size == 0x30);
  CHECK(find_pmu_app(bios, kPmuAppPreOs, &app) == NvPmuAppStatus::Found && app.descriptor == kPreOsApp && app.boot_size == 0x100);
  CHECK(find_pmu_app(bios, 0x02, &app) == NvPmuAppStatus::NotFound);   // (the entry without a descriptor is skipped)
  CHECK(find_pmu_app(bios, 0x07, &app) == NvPmuAppStatus::NotFound);
  {   // no 'p' entry / wrong version / a zero pointer
    Bytes w = base_image();
    NvBios b(view(w));
    CHECK(find_pmu_app(b, kPmuAppDevinit, &app) == NvPmuAppStatus::NoTable);
    Bytes x = pmu_image();
    put8(x, 0x100 + 12 + 4 * 6 + 1, 1);
    NvBios bx(view(x));
    CHECK(find_pmu_app(bx, kPmuAppDevinit, &app) == NvPmuAppStatus::NoTable);
    Bytes y = pmu_image();
    put32(y, kPmuBit, 0);
    NvBios by(view(y));
    CHECK(find_pmu_app(by, kPmuAppDevinit, &app) == NvPmuAppStatus::NoTable);
  }
  {   // sections outside the image, or a boot end before its start
    Bytes w = pmu_image();
    put32(w, kDevinitApp + 0x24, 0xfffffff0);
    NvBios b(view(w));
    CHECK(find_pmu_app(b, kPmuAppDevinit, &app) == NvPmuAppStatus::BadSections);
    Bytes x = pmu_image();
    put32(x, kDevinitApp + 0x1c, 0x80);
    NvBios bx(view(x));
    CHECK(find_pmu_app(bx, kPmuAppDevinit, &app) == NvPmuAppStatus::BadSections);
    Bytes y = pmu_image();
    put32(y, kDevinitApp + 0x20, 0x10000);
    NvBios by(view(y));
    CHECK(find_pmu_app(by, kPmuAppDevinit, &app) == NvPmuAppStatus::BadSections);
  }
  {   // more entries than the image holds; a descriptor cut off
    Bytes w = pmu_image();
    put8(w, kPmuTable + 3, 0xff);
    put8(w, kPmuTable + 2, 0xff);
    NvBios b(view(w));
    CHECK(find_pmu_app(b, 0x07, &app) == NvPmuAppStatus::Truncated);
    Bytes x(v.begin(), v.begin() + kDevinitApp + 0x20);
    NvBios bx(view(x));
    CHECK(find_pmu_app(bx, kPmuAppDevinit, &app) == NvPmuAppStatus::Truncated);
  }
}

static void test_pmu_post() {
  Bytes v = pmu_image();
  NvBios bios(view(v));
  {   // GM204, not POSTed, without PRE_OS: the whole upload and the start, checked word by word
    PmuRun r(bios, kGm204);
    r.in.run_preos = false;
    const PostReport& rep = r.run();
    CHECK(rep.outcome == PostOutcome::Ran);
    CHECK(rep.pmu.used && !rep.pmu.engine_reset && rep.pmu.devinit_status == NvPmuAppStatus::Found);
    CHECK(rep.pmu.started && rep.pmu.completed && !rep.pmu.preos_started && rep.pmu.preos_status == NvPmuAppStatus::Found);
    CHECK(rep.pmu.dmem_targets_known && rep.pmu.init_table_dmem == 0x1000 && rep.pmu.boot_scripts_dmem == 0x1800);
    CHECK(rep.pmu.imem_words == 128 && rep.pmu.dmem_words == 12 + 4 + 3);
    CHECK(rep.encoder_scripts_pending && rep.posted_after_known);
    FakeFalcon& f = r.regs;
    // the reset through PMC_ENABLE (bit 13 off, then on) before the first upload
    CHECK(f.count_writes(0x000200) == 2 && f.count_writes(0x10a3c0) == 0);
    CHECK(f.first_write(0x000200) < f.first_write(0x10a180));
    CHECK(f.regs[0x000200] & 0x2000);
    CHECK(f.regs[0x10a014] == 0xffffffff);
    // IMEM: the boot code (not secure), the code (secure), its padding to 256 bytes
    bool imem_ok = true;
    for (uint32_t i = 0; i < 0x10; ++i) imem_ok &= f.imem[0x100 + i * 4] == boot_word(i) && !f.imem_secure[0x100 + i * 4];
    for (uint32_t i = 0; i < 0x20; ++i) imem_ok &= f.imem[0x140 + i * 4] == code_word(i) && f.imem_secure[0x140 + i * 4];
    for (uint32_t a = 0x1c0; a < 0x240; a += 4) imem_ok &= f.imem[a] == 0;
    CHECK(imem_ok);
    CHECK(f.tags.size() == 2 && f.tags[0] == 1 && f.tags[1] == 1);
    // DMEM: the data section, the init tables, the boot scripts (the last word read whole)
    CHECK(f.dmem[0x800] == 0xd0 && f.dmem[0x808] == 0x810 && f.dmem[0x818] == 0x1000);
    CHECK(f.dmem[0x1000] == 0x13121110 && f.dmem[0x100c] == 0x1f1e1d1c);
    CHECK(f.dmem[0x1800] == 0x53525150 && f.dmem[0x1808] == 0x5b5a5958 && f.dmem.count(0x180c) == 0);
    // the start: mailbox 0x5000, the boot vector, DMACTL 0, CPUCTL start, after the uploads
    CHECK(f.boot_vectors.size() == 1 && f.boot_vectors[0] == 0x100);
    const size_t start = f.first_write(0x10a100);
    CHECK(start != SIZE_MAX && start > f.first_write(0x10a1c4));
    bool mailbox = false;
    for (size_t i = 0; i < start; ++i) mailbox |= f.writes[i].first == 0x10a040 && f.writes[i].second == 0x5000;
    CHECK(mailbox);
    // only the falcon and PMC_ENABLE were written
    bool only_pmu = true;
    for (const auto& w : f.writes) only_pmu &= (w.first >= 0x10a000 && w.first < 0x10b000) || w.first == 0x000200;
    CHECK(only_pmu);
    CHECK(r.trace.all.size() == f.writes.size());
  }
  {   // with PRE_OS: the falcon is reset again and PRE_OS started (not waited for)
    PmuRun r(bios, kGm204);
    const PostReport& rep = r.run();
    CHECK(rep.outcome == PostOutcome::Ran && rep.pmu.preos_started);
    CHECK(r.regs.boot_vectors.size() == 2 && r.regs.boot_vectors[1] == 0x200);
    CHECK(r.regs.count_writes(0x000200) == 4);
    CHECK(rep.pmu.imem_words == 128 + 128);
  }
  {   // GP104 and GV100 reset the falcon through its engine reset register (two pulses); GP100 still through PMC_ENABLE
    const uint32_t boots[] = {0x134000a1, 0x140000a1};
    for (uint32_t boot0 : boots) {
      PmuRun r(bios, chip_of(boot0));
      r.in.run_preos = false;
      const PostReport& rep = r.run();
      CHECK(rep.outcome == PostOutcome::Ran && rep.pmu.engine_reset);
      CHECK(r.regs.count_writes(0x10a3c0) == 4 && r.regs.count_writes(0x000200) == 0);
    }
    PmuRun r(bios, chip_of(0x130000a1));
    CHECK(r.run().outcome == PostOutcome::Ran && !r.rep.pmu.engine_reset && r.regs.count_writes(0x000200) >= 2);
  }
  {   // the application never finishes: a bounded wait, Failed, no PRE_OS
    PmuRun r(bios, kGm204);
    r.regs.halt_after = -1;
    r.in.pmu_timeout_us = 1000;
    r.in.pmu_poll_us = 10;
    const PostReport& rep = r.run();
    CHECK(rep.outcome == PostOutcome::Failed && rep.pmu.started && !rep.pmu.completed && rep.pmu.waited_us == 1000);
    CHECK(!rep.pmu.preos_started && r.regs.boot_vectors.size() == 1);
    CHECK(strstr(rep.reason, "timeout") != nullptr);
  }
  {   // the memory scrub never finishes: nothing uploaded
    PmuRun r(bios, kGm204);
    r.regs.scrub_stuck = true;
    CHECK(r.run().outcome == PostOutcome::Failed && r.regs.count_writes(0x10a180) == 0 && !r.rep.pmu.started);
  }
  {   // the application does not fit the falcon (256 bytes of IMEM), or the argument block points the tables past DMEM
    PmuRun r(bios, kGm204);
    r.regs.hwcfg = 0x00000001 | (0x10000 << 1);
    CHECK(r.run().outcome == PostOutcome::Failed && r.regs.count_writes(0x10a184) == 0);
    Bytes w = pmu_image();
    put32(w, kDevinitApp + 0x30 + 0xc0 + 0x18, 0xfff8);
    NvBios b(view(w));
    PmuRun r2(b, kGm204);
    CHECK(r2.run().outcome == PostOutcome::Failed && r2.regs.boot_vectors.empty() && r2.regs.dmem.count(0xfff8) == 0);
  }
  {   // dry run: nothing written, the plan reported (the DMEM targets from the image's data section)
    PmuRun r(bios, kGm204);
    r.in.dry_run = true;
    const PostReport& rep = r.run();
    CHECK(rep.outcome == PostOutcome::DryRun && r.regs.writes.empty() && r.trace.all.empty());
    CHECK(rep.pmu.dmem_targets_known && rep.pmu.init_table_dmem == 0x1000 && rep.pmu.boot_scripts_dmem == 0x1800);
    CHECK(rep.pmu.imem_words == 128 && rep.pmu.dmem_words == 19 && !rep.pmu.started);
    CHECK(rep.pmu.preos_status == NvPmuAppStatus::Found);
  }
  {   // already POSTed (0x2240c bit 1): nothing done; forced: run
    PmuRun r(bios, kGm204);
    r.regs.regs[NvDevinit::kPostedGf100] = NvDevinit::kPostedBit;
    CHECK(r.run().outcome == PostOutcome::AlreadyPosted && r.regs.writes.empty());
    r.in.force = true;
    CHECK(r.run().outcome == PostOutcome::Ran);
  }
  {   // refused before anything is written: a short BIT 'I', no DEVINIT application, tables outside the image
    Bytes w = pmu_image();
    put16(w, 0x100 + 12 + 2, 0x12);
    NvBios b(view(w));
    PmuRun r(b, kGm204);
    CHECK(r.run().outcome == PostOutcome::Refused && r.regs.writes.empty());
    Bytes x = pmu_image();
    put8(x, kPmuTable + 16, 0x05);
    NvBios bx(view(x));
    PmuRun rx(bx, kGm204);
    CHECK(rx.run().outcome == PostOutcome::Refused && rx.regs.writes.empty() && rx.rep.pmu.devinit_status == NvPmuAppStatus::NotFound);
    rx.regs.regs[NvDevinit::kPostedGf100] = NvDevinit::kPostedBit;   // (POSTed anyway: reported so)
    CHECK(rx.run().outcome == PostOutcome::AlreadyPosted);
    Bytes y = pmu_image();
    put16(y, kInit + 0x18, 0x7ff8);
    NvBios by(view(y));
    PmuRun ry(by, kGm204);
    CHECK(ry.run().outcome == PostOutcome::Refused && ry.regs.writes.empty());
  }
  {   // Turing takes the GFW_BOOT wait, not the PMU path: nothing written (the model's 0x118128 reads 0: never unprotected)
    PmuRun r(bios, chip_of(0x164000a1, 0x16400000));
    r.in.gfw_timeout_us = 50;
    r.in.gfw_poll_us = 10;
    const PostReport& rep = r.run();
    CHECK(rep.outcome == PostOutcome::Failed && !rep.pmu.used && rep.gfw.used && r.regs.writes.empty());
  }
}

static void check_pmu_writes(const FakeFalcon& f, size_t image) {
  bool ok = true;
  for (const auto& w : f.writes) ok &= (w.first >= 0x10a000 && w.first < 0x10b000) || w.first == 0x000200 || w.first == 0x2240c;
  CHECK(ok);
  // no more words than the image (each section padded to 256 bytes, twice for PRE_OS)
  CHECK(f.count_writes(0x10a184) + f.count_writes(0x10a1c4) <= image / 4 * 2 + 0x400);
  for (const auto& m : f.imem) CHECK(m.first < 0x10000);
  for (const auto& m : f.dmem) CHECK(m.first < 0x10000);
}

static void test_pmu_hostile() {
  const Bytes full = pmu_image();
  // cut at every length from the 'p' pointer to past the boot scripts
  for (size_t n = kPmuBit - 0x10; n <= kPmuScripts + 0x10; ++n) {
    Bytes cut(full.begin(), full.begin() + n);
    NvBios bios(view(cut));
    NvPmuApp app;
    (void)find_pmu_app(bios, kPmuAppDevinit, &app);
    PmuRun r(bios, kGm204);
    r.in.pmu_timeout_us = 100;
    const PostReport& rep = r.run();
    if (n < kPmuScripts + 0x0c) CHECK(rep.outcome == PostOutcome::Refused && r.regs.writes.empty());
    check_pmu_writes(r.regs, n);
  }
  srand(0x9a11);
  for (int round = 0; round < 400; ++round) {
    Bytes v = full;
    const int flips = 1 + rand() % 6;
    for (int i = 0; i < flips; ++i) {
      const uint32_t regions[5][2] = {{kPmuBit, 4}, {kPmuTable, 22}, {kDevinitApp, 0x30}, {kPreOsApp, 0x30}, {kInit + 0x14, 8}};
      const auto& g = regions[rand() % 5];
      v[g[0] + static_cast<uint32_t>(rand()) % g[1]] ^= static_cast<uint8_t>(1u << (rand() % 8));
    }
    if (round % 9 == 0) put32(v, kDevinitApp + 0x30 + 0xc0 + 0x08, static_cast<uint32_t>(rand()));   // a wild argument pointer
    NvBios bios(view(v));
    PmuRun r(bios, round % 2 ? kGm204 : chip_of(0x134000a1));
    r.in.pmu_timeout_us = 100;
    r.in.dry_run = round % 4 == 0;
    r.regs.halt_after = round % 3 == 0 ? -1 : 2;
    const PostReport& rep = r.run();
    if (r.in.dry_run) CHECK(r.regs.writes.empty());
    if (rep.outcome == PostOutcome::Refused) CHECK(r.regs.writes.empty());
    check_pmu_writes(r.regs, v.size());
  }
}

// --- Turing and later: the wait for the GPU's own boot (GFW_BOOT) -----------------------------------------------------------------------------

// 0x118128 bit 0 goes up after `unprotect_after` reads of it, 0x118234 reads 0xff after `done_after` reads of it (a value below 0xff before
// that, the progress); -1 = never. Reading 0x118234 while still protected counts as a mistake.
struct GfwRegs final : INvRegisters {
  int unprotect_after = 0, done_after = 0;
  int level_reads = 0, progress_reads = 0, protected_reads = 0, writes = 0;
  uint64_t delayed = 0;
  bool dead = false;
  uint32_t read32(uint32_t offset) override {
    if (dead) return 0xFFFFFFFFu;
    if (offset == NvDevinit::kDevinitStatusTu102) {
      ++level_reads;
      return unprotect_after >= 0 && level_reads > unprotect_after ? 0xfffffff1u /* other bits set too */ : 0xfffffff0u;
    }
    if (offset == NvDevinit::kDevinitDoneTu102) {
      if (!(unprotect_after >= 0 && level_reads > unprotect_after)) ++protected_reads;
      ++progress_reads;
      return done_after >= 0 && progress_reads > done_after ? 0x123400ffu : 0x12340080u;
    }
    return 0;
  }
  void write32(uint32_t, uint32_t) override { ++writes; }
  void delay_us(uint32_t us) override { delayed += us; }
};

static void test_gfw_boot() {
  Bytes v = pmu_image();
  NvBios bios(view(v));
  const uint32_t turings[] = {0x164000a1 /* TU104 */, 0x172000a1 /* GA102 */, 0x194000a1 /* AD104 */};
  for (uint32_t boot0 : turings) {
    auto inputs = [&](GfwRegs& g) {
      PostInputs in;
      in.bios = &bios;
      in.registers = &g;
      in.chip = chip_of(boot0, boot0 & 0xfff00000u);
      in.gfw_timeout_us = 1000;
      in.gfw_poll_us = 10;
      return in;
    };
    {   // done at the first look
      GfwRegs g;
      const PostReport rep = NvDevinit::post(inputs(g));
      CHECK(rep.outcome == PostOutcome::AlreadyPosted && rep.gfw.used && rep.gfw.completed && rep.gfw.polls == 1);
      CHECK(rep.gfw.protection_lowered && rep.gfw.progress == 0xff && !rep.needed && g.writes == 0 && g.delayed == 0);
      CHECK(!rep.pmu.used && rep.script.scripts == 0);
    }
    {   // the protection drops after 3 reads (detect() makes the first), the progress reaches 0xff on its 5th read
      GfwRegs g;
      g.unprotect_after = 3;
      g.done_after = 4;
      const PostReport rep = NvDevinit::post(inputs(g));
      CHECK(rep.outcome == PostOutcome::Ran && rep.needed && rep.gfw.completed);
      CHECK(rep.gfw.polls == 7 && rep.gfw.waited_us == 60 && g.delayed == 60);
      CHECK(g.protected_reads == 0 && g.writes == 0 && rep.posted_after);
    }
    {   // never finishes: Failed after the timeout, the progress reported
      GfwRegs g;
      g.done_after = -1;
      const PostReport rep = NvDevinit::post(inputs(g));
      CHECK(rep.outcome == PostOutcome::Failed && !rep.gfw.completed && rep.gfw.protection_lowered && rep.gfw.progress == 0x80);
      CHECK(rep.gfw.waited_us == 1000 && g.delayed == 1000 && rep.gfw.polls == 101 && g.writes == 0);
    }
    {   // read-protected for good: Failed, and the progress register is never read
      GfwRegs g;
      g.unprotect_after = -1;
      const PostReport rep = NvDevinit::post(inputs(g));
      CHECK(rep.outcome == PostOutcome::Failed && !rep.gfw.protection_lowered && g.progress_reads == 0 && g.protected_reads == 0);
      CHECK(rep.gfw.waited_us == 1000);
    }
    {   // a dry run reads once and does not wait
      GfwRegs g;
      g.unprotect_after = 5;
      PostInputs in = inputs(g);
      in.dry_run = true;
      const PostReport rep = NvDevinit::post(in);
      CHECK(rep.outcome == PostOutcome::DryRun && rep.gfw.polls == 1 && g.delayed == 0 && g.writes == 0);
      GfwRegs done;
      in.registers = &done;
      CHECK(NvDevinit::post(in).outcome == PostOutcome::AlreadyPosted);
    }
    {   // a GPU that does not answer
      GfwRegs g;
      g.dead = true;
      const PostReport rep = NvDevinit::post(inputs(g));
      CHECK(rep.outcome == PostOutcome::Failed && rep.gfw.polls == 1 && g.delayed == 0);
    }
    {   // a poll interval that does not divide the timeout ends exactly at the timeout; a zero interval still terminates
      GfwRegs g;
      g.done_after = -1;
      PostInputs in = inputs(g);
      in.gfw_poll_us = 300;
      CHECK(NvDevinit::post(in).gfw.waited_us == 1000);
      GfwRegs z;
      z.done_after = -1;
      in.registers = &z;
      in.gfw_poll_us = 0;
      in.gfw_timeout_us = 5;
      CHECK(NvDevinit::post(in).outcome == PostOutcome::Failed && z.delayed == 5);
    }
  }
}

int main() {
  test_tables();
  test_register_opcodes();
  test_conditions();
  test_vga();
  test_subs_and_loops();
  test_time();
  test_hostile_control_flow();
  test_hooks_and_ram();
  test_sample_and_dry_run();
  test_truncation_and_flips();
  test_devinit_post();
  test_pmu_table();
  test_pmu_post();
  test_pmu_hostile();
  test_gfw_boot();
  if (g_failed) {
    printf("NvInitScriptTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("NvInitScriptTest: all passed\n");
  return 0;
}
