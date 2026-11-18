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

// The NVIDIA driver's GPIO, I2C, AUX, thermal, fan and clock code against a register model of the GPU: GPIO lines per family (levels,
// directions, functions from the GPIO table, interrupt status), I2C slaves on the bit-banged pads written independently of the master (start
// and stop seen on the lines, bits sampled on the rising clock, acknowledges and data driven on the falling one, clock stretching: an EDID
// EEPROM with its segment pointer, a missing device that never acknowledges, one that holds the clock forever), an AUX channel engine with a
// DisplayPort sink behind it (native DPCD, I2C over AUX with the middle-of-transaction flag, scripted DEFERs, timeouts, wire errors, short
// replies, lengths that make no sense, random replies), the EDID end to end through both into the Edid parser, the temperature sensors per
// family (with garbage), the fan tachometer and PWM units, and the clock tree.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvAux.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvClk.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvGpio.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2c.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2cEdid.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvTherm.hpp"
#include "Hardware/Monitor/Edid.hpp"

using namespace hardware;
using namespace hardware::nv;

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
static TestAllocator g_alloc;

static Chip chip_of(Family family, uint32_t chipset) {
  Chip c;
  c.family = family;
  c.chipset = chipset;
  c.name = "test";
  return c;
}

static uint32_t g_rng = 0x1234567;
static uint32_t rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 7; }

// An EDID with a name, a manufacturer and `extensions` extension blocks, each block checksummed.
static std::vector<uint8_t> make_edid(uint32_t extensions) {
  std::vector<uint8_t> e(128 * (1 + extensions), 0);
  const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memcpy(e.data(), header, 8);
  e[8] = 0x10; e[9] = 0xAC;        // "DEL"
  e[10] = 0x34; e[11] = 0x12;      // product 0x1234
  e[18] = 1; e[19] = 4;
  e[20] = 0xa5;                    // digital, 8 bits
  e[21] = 60; e[22] = 34;
  // a detailed timing: 1920x1080 at 148.5 MHz
  uint8_t* d = &e[54];
  d[0] = 0x02; d[1] = 0x3a; d[2] = 0x80; d[3] = 0x18; d[4] = 0x71; d[5] = 0x38; d[6] = 0x2d; d[7] = 0x40; d[8] = 0x58; d[9] = 0x2c;
  d[10] = 0x45; d[11] = 0x00; d[12] = 0x56; d[13] = 0x50; d[14] = 0x21; d[15] = 0x00; d[16] = 0x00; d[17] = 0x1e;
  // the name descriptor
  uint8_t* n = &e[72];
  n[3] = 0xfc;
  const char* name = "NV TEST\n";
  for (int i = 0; i < 13; ++i) n[5 + i] = i < 8 ? static_cast<uint8_t>(name[i]) : 0x20;
  e[126] = static_cast<uint8_t>(extensions);
  for (uint32_t b = 0; b <= extensions; ++b) {
    if (b > 0) { e[b * 128] = 0x02; e[b * 128 + 1] = 3; e[b * 128 + 2] = 4; for (int i = 4; i < 127; ++i) e[b * 128 + i] = static_cast<uint8_t>(b * 16 + i); }
    uint8_t sum = 0;
    for (int i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + e[b * 128 + i]);
    e[b * 128 + 127] = static_cast<uint8_t>(0 - sum);
  }
  return e;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The model: plain registers, plus the devices that react to some of them.

// A bit-level I2C slave set on one bus register: an EDID EEPROM at 0x50 with the E-DDC segment pointer at 0x30.
struct I2cSlave {
  bool gf119 = false;              // the register layout: Nv50 (bits 0/1 drive and read back) or Gf119 (bits 0/1 drive, 4/5 read back)
  uint32_t reg = 0;
  uint32_t value = 0;              // what the master last wrote
  std::vector<uint8_t> edid;
  bool present = true;             // false: nobody acknowledges anything
  uint32_t stretch_polls = 0;      // at each address acknowledge the slave holds the clock for this many reads of the register
  bool stretch_forever = false;
  bool scl_stuck_low = false;      // the clock line shorted low
  uint32_t flip_reads = 0;         // the next this many read messages have byte 5 flipped
  // seen
  uint32_t starts = 0, stops = 0, repeated = 0;
  std::vector<uint8_t> addresses;
  // lines
  bool scl = true, sda = true, slave_scl_low = false, slave_sda_low = false;
  uint32_t stretch_left = 0;
  enum class S { Idle, Address, AddrAck, WriteData, WriteAck, ReadData, ReadAck } state = S::Idle;
  uint32_t bits = 0;
  uint8_t shift = 0, address = 0, pointer = 0, segment = 0, out = 0;
  bool reading = false, first_write = true, master_acked = false, flip_this = false;
  uint32_t bytes_in_read = 0;

  bool master_scl() const { return value & 1; }
  bool master_sda() const { return value & 2; }
  uint32_t read() {
    if (stretch_left && !stretch_forever) { if (--stretch_left == 0) { slave_scl_low = false; update(); } }
    const uint32_t lines = (scl ? 1u : 0u) | (sda ? 2u : 0u);
    return gf119 ? (value & 3) | lines << 4 : (value & ~3u) | lines;
  }
  void write(uint32_t v) { value = v & 7; update(); }
  void update() {
    const bool ns = master_scl() && !slave_scl_low && !scl_stuck_low, nd = master_sda() && !slave_sda_low;
    const bool os = scl, od = sda;
    scl = ns; sda = nd;
    if (!present) return;
    if (os && ns && od && !nd) { if (state != S::Idle) ++repeated; ++starts; state = S::Address; bits = 0; shift = 0; slave_sda_low = false; return; }
    if (os && ns && !od && nd) { ++stops; state = S::Idle; slave_sda_low = false; segment = 0; return; }
    if (!os && ns) rising();
    if (os && !ns) falling();
  }
  void rising() {
    if (state == S::Address || state == S::WriteData) { shift = static_cast<uint8_t>(shift << 1 | (sda ? 1 : 0)); ++bits; }
    else if (state == S::ReadAck) master_acked = !sda;
  }
  void load() {
    const size_t at = size_t{segment} * 256 + pointer;
    out = at < edid.size() ? edid[at] : 0xFF;
    if (flip_this && bytes_in_read == 5) out ^= 0x08;
    ++bytes_in_read;
  }
  void drive_bit() { slave_sda_low = !((out >> (7 - bits)) & 1); }
  void falling() {
    switch (state) {
      case S::Address:
        if (bits == 8) {
          addresses.push_back(shift);
          address = shift >> 1;
          reading = shift & 1;
          if (address == 0x50 || (address == 0x30 && !reading)) {
            state = S::AddrAck; slave_sda_low = true; first_write = true;
            if (stretch_polls) { stretch_left = stretch_polls; slave_scl_low = true; }
            if (stretch_forever) slave_scl_low = true;
          } else {
            state = S::Idle;
          }
        }
        break;
      case S::AddrAck:
        slave_sda_low = false;
        if (reading) {
          flip_this = flip_reads > 0;
          if (flip_reads) --flip_reads;
          bytes_in_read = 0;
          state = S::ReadData; bits = 0; load(); drive_bit();
        } else {
          state = S::WriteData; bits = 0; shift = 0;
        }
        break;
      case S::WriteData:
        if (bits == 8) {
          if (first_write) { if (address == 0x30) segment = shift; else pointer = shift; first_write = false; }
          state = S::WriteAck; slave_sda_low = true;
        }
        break;
      case S::WriteAck: slave_sda_low = false; state = S::WriteData; bits = 0; shift = 0; break;
      case S::ReadData:
        ++bits;
        if (bits == 8) { slave_sda_low = false; state = S::ReadAck; } else drive_bit();
        break;
      case S::ReadAck:
        if (master_acked) { ++pointer; load(); state = S::ReadData; bits = 0; drive_bit(); }
        else { state = S::Idle; slave_sda_low = false; }
        break;
      default: break;
    }
    sda = master_sda() && !slave_sda_low;
  }
};

// The AUX channel engine of one channel, with a DisplayPort sink behind it.
struct AuxEngine {
  bool gm200 = false;
  uint32_t base = 0;
  uint32_t ctrl = 0, address = 0, status = 0;
  uint32_t out[4] = {}, in[4] = {};
  bool sink_present = true;
  std::vector<uint8_t> dpcd, edid;
  uint32_t segment = 0, offset = 0;
  bool in_transaction = false;
  // the script
  uint32_t native_defers = 0, i2c_defers = 0, never_complete = 0, timeouts = 0, wire_errors = 0, short_reads = 0, garbage_lengths = 0;
  bool never_idle = false, never_granted = false, always_short = false, random = false, nack_native = false;
  // seen
  struct Req { uint8_t type; uint32_t address; uint32_t size; bool address_only; };
  std::vector<Req> requests;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes;

  AuxEngine() { dpcd.resize(0x800); for (size_t i = 0; i < dpcd.size(); ++i) dpcd[i] = static_cast<uint8_t>(i * 3 + 1); }
  uint32_t claim_field() const { return gm200 ? 0x00700000u : 0x00300000u; }
  bool owns(uint32_t r) const { return r >= base && r < base + 0x40; }
  uint32_t read(uint32_t r) {
    const uint32_t off = r - base;
    if (off >= 0x10 && off < 0x20) return in[(off - 0x10) / 4];
    if (off < 0x10) return out[off / 4];
    if (off == 0x20) return address;
    if (off == 0x24) {
      uint32_t v = ctrl & ~(gm200 ? 0x07000000u : 0x03000000u);
      if ((ctrl & claim_field()) == 0x00100000 && !never_granted) v |= 0x01000000;
      if (never_idle) v |= 0x00010000;
      return v;
    }
    if (off == 0x28) {
      uint32_t v = status & ~0x10000000u;
      const bool flag = gm200 ? sink_present : !sink_present;
      return v | (flag ? 0x10000000u : 0u);
    }
    return other[(off / 4) & 15];
  }
  uint32_t other[16] = {};   // the registers the engine does not act on (0x38: the automatic DPCD polling)
  void write(uint32_t r, uint32_t v) {
    const uint32_t off = r - base;
    if (off < 0x10) { out[off / 4] = v; return; }
    if (off == 0x20) { address = v; return; }
    if (off == 0x28) return;   // (the read-back write of the status)
    if (off != 0x24) { other[(off / 4) & 15] = v; return; }
    if (off == 0x24) {
      ctrl = v & ~(gm200 ? 0x07000000u : 0x03000000u);
      if (v & 0x00010000) {
        if (never_complete) { --never_complete; return; }   // Go stays set
        run();
        ctrl &= ~0x00010000u;
      }
    }
  }
  void reply(uint32_t code, const std::vector<uint8_t>& data) {
    uint32_t length = static_cast<uint32_t>(data.size());
    for (uint32_t i = 0; i < 4; ++i) in[i] = 0;
    for (size_t i = 0; i < data.size() && i < 16; ++i) in[i / 4] |= uint32_t{data[i]} << (8 * (i % 4));
    if (garbage_lengths) { --garbage_lengths; length = 17 + rnd() % 15; }
    status = code << 16 | (length & 0x1f);
  }
  void run() {
    const uint8_t type = static_cast<uint8_t>(ctrl >> 12 & 0xf);
    const bool bare = ctrl & 0x100;
    const uint32_t size = bare ? 0 : (ctrl & 0xf) + 1;
    requests.push_back({type, address, size, bare});
    if (timeouts) { --timeouts; status = 0x100; return; }
    if (wire_errors) { --wire_errors; status = 0x400; return; }
    if (random) {
      for (uint32_t& w : in) w = rnd() << 3 ^ rnd();
      status = rnd() << 2 ^ rnd();
      return;
    }
    uint8_t payload[16];
    for (uint32_t i = 0; i < 16; ++i) payload[i] = static_cast<uint8_t>(out[i / 4] >> (8 * (i % 4)));
    std::vector<uint8_t> data;
    if (type == 9 || type == 8) {
      if (native_defers) { --native_defers; reply(2, {}); return; }
      if (nack_native || address + size > dpcd.size()) { reply(1, {}); return; }
      if (type == 9) {
        uint32_t n = size;
        if (always_short && n > 1) n = n / 2;
        if (short_reads && n > 1) { --short_reads; n = n / 2; }
        for (uint32_t i = 0; i < n; ++i) data.push_back(dpcd[address + i]);
      } else {
        writes.push_back({address, std::vector<uint8_t>(payload, payload + size)});
        for (uint32_t i = 0; i < size; ++i) dpcd[address + i] = payload[i];
      }
      reply(0, data);
      return;
    }
    // I2C over AUX
    if (i2c_defers) { --i2c_defers; reply(8, {}); return; }
    if (address != 0x50 && address != 0x30) { reply(4, {}); return; }
    const bool mot = type & 4;
    const bool read = (type & 3) == 1;
    if (bare) {
      in_transaction = mot;
      if (!mot) segment = 0;
      reply(0, {});
      return;
    }
    if (!read) {
      if (address == 0x30) segment = payload[0]; else offset = payload[0];
    } else if (address == 0x50) {
      uint32_t n = size;
      if (short_reads && n > 1) { --short_reads; n = n / 2; }
      for (uint32_t i = 0; i < n; ++i) { const size_t at = segment * 256 + offset + i; data.push_back(at < edid.size() ? edid[at] : 0xff); }
      offset += n;
    }
    if (!mot) segment = 0;   // (a G94-style transaction ends with the last piece)
    reply(0, data);
  }
};

struct Model final : INvRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<I2cSlave*> slaves;
  std::vector<AuxEngine*> aux;
  uint64_t delayed_us = 0;
  uint64_t reads = 0, writes_count = 0;
  // GPIO: the level outside each line (what is read when the GPU does not drive it)
  bool external[32] = {};
  bool gf119_gpio = false, ga102 = false, nv50_gpio = false;
  // PTIMER: advances by timer_step_ns at every read of the low word
  uint64_t timer_ns = 0, timer_step_ns = 0;
  // fan sense: the line toggles every half_period_ns of PTIMER time (0: never)
  int fan_line = -1;
  uint64_t fan_half_period_ns = 0;
  std::map<uint32_t, uint32_t> write_log;

  uint32_t get(uint32_t r) const { auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }
  bool line_level(uint32_t line) {
    if (fan_line >= 0 && line == static_cast<uint32_t>(fan_line) && fan_half_period_ns) return (timer_ns / fan_half_period_ns) & 1;
    return external[line];
  }
  std::mutex mu;   // (one register access at a time, for the threads test)
  uint32_t read32(uint32_t r) override {
    std::lock_guard<std::mutex> lock(mu);
    ++reads;
    for (I2cSlave* s : slaves) if (s->reg == r) return s->read();
    for (AuxEngine* a : aux) if (a->owns(r)) return a->read(r);
    if (r == 0x009400) { timer_ns += timer_step_ns; return static_cast<uint32_t>(timer_ns); }
    if (r == 0x009410) return static_cast<uint32_t>(timer_ns >> 32);
    // GPIO sense
    if (nv50_gpio) {
      static const uint32_t kRegs[4] = {0x00e104, 0x00e108, 0x00e280, 0x00e284};
      for (uint32_t i = 0; i < 4; ++i) {
        if (r != kRegs[i]) continue;
        uint32_t v = get(r) & ~0x44444444u;
        for (uint32_t l = 0; l < 8; ++l) {
          const uint32_t line = i * 8 + l, nib = (get(r) >> (l * 4)) & 0xf;
          const bool driven = !(nib & 2);
          const bool level = driven ? (nib & 1) : line_level(line);
          if (level) v |= 4u << (l * 4);
        }
        return v;
      }
    }
    if (gf119_gpio) {
      const uint32_t base = ga102 ? 0x021200 : 0x00d610;
      if (r >= base && r < base + 32 * 4 && (r - base) % 4 == 0) {
        const uint32_t line = (r - base) / 4, v = get(r) & ~0x4000u;
        const bool driven = !(v & 0x2000);
        const bool level = driven ? (v & 0x1000) : line_level(line);
        return v | (level ? 0x4000u : 0u);
      }
    }
    return get(r);
  }
  void write32(uint32_t r, uint32_t v) override {
    std::lock_guard<std::mutex> lock(mu);
    ++writes_count;
    write_log[r] = v;
    for (I2cSlave* s : slaves) if (s->reg == r) { s->write(v); return; }
    for (AuxEngine* a : aux) if (a->owns(r)) { a->write(r, v); return; }
    // the sensed level bits of the GPIO registers are read-only
    if (nv50_gpio && (r == 0x00e104 || r == 0x00e108 || r == 0x00e280 || r == 0x00e284)) v &= ~0x44444444u;
    if (gf119_gpio) {
      const uint32_t base = ga102 ? 0x021200 : 0x00d610;
      if (r >= base && r < base + 32 * 4) v &= ~0x4000u;
    }
    regs[r] = v;
  }
  void delay_us(uint32_t us) override { std::lock_guard<std::mutex> lock(mu); delayed_us += us; }
};

// ---------------------------------------------------------------------------------------------------------------------------------------

static BoardInfo board_with_gpio(std::initializer_list<GpioEntry> entries) {
  BoardInfo b;
  for (const GpioEntry& e : entries) b.gpio[b.gpio_count++] = e;
  return b;
}
static GpioEntry gpio_entry(uint8_t line, uint8_t function, uint8_t log0, uint8_t log1) {
  GpioEntry e;
  e.line = line; e.function = function; e.log0 = log0; e.log1 = log1;
  return e;
}

static void test_gpio_variants() {
  CHECK(NvGpio::variant_for(chip_of(Family::Tesla, 0x50)) == NvGpio::Variant::Nv50);
  CHECK(NvGpio::variant_for(chip_of(Family::Tesla, 0x92)) == NvGpio::Variant::Nv50);
  CHECK(NvGpio::variant_for(chip_of(Family::Tesla, 0xa0)) == NvGpio::Variant::Nv50);
  CHECK(NvGpio::variant_for(chip_of(Family::Tesla, 0x96)) == NvGpio::Variant::G94);
  CHECK(NvGpio::variant_for(chip_of(Family::Fermi, 0xc0)) == NvGpio::Variant::G94);
  CHECK(NvGpio::variant_for(chip_of(Family::Fermi, 0xd9)) == NvGpio::Variant::Gf119);
  CHECK(NvGpio::variant_for(chip_of(Family::Kepler, 0xe4)) == NvGpio::Variant::Gk104);
  CHECK(NvGpio::variant_for(chip_of(Family::Turing, 0x164)) == NvGpio::Variant::Gk104);
  CHECK(NvGpio::variant_for(chip_of(Family::Ampere, 0x172)) == NvGpio::Variant::Ga102);
  CHECK(NvGpio::variant_for(chip_of(Family::Nv40, 0x40)) == NvGpio::Variant::None);
}

static void test_gpio_nv50() {
  Model m;
  m.nv50_gpio = true;
  BoardInfo b = board_with_gpio({gpio_entry(5, gpio_function::kPanelPower, 0x2, 0x3), gpio_entry(7, 0x51, 0, 1), gpio_entry(12, gpio_function::kVid[0], 0, 1),
                                 gpio_entry(13, gpio_function::kVid[1], 0, 1), gpio_entry(20, 0x52, 0, 1)});
  m.regs[0x00e108] = 0x22222222;   // lines 8..15 not driven
  NvGpio g(m, chip_of(Family::Tesla, 0x84), b);
  CHECK(g.line_count() == 16);
  // drive line 5 high as an output: nibble 1 of 0xe104 = 0b01 (driven, high)
  m.regs[0x00e104] = 0xbbbbbbbb;
  CHECK(g.drive(5, true, true));
  CHECK(((m.get(0x00e104) >> 20) & 3) == 1);
  CHECK((m.get(0x00e104) & ~(3u << 20)) == (0xbbbbbbbbu & ~(3u << 20)));   // the other lines untouched
  CHECK(g.sense(5).value() == true);
  CHECK(g.drive(5, false, false));
  CHECK(((m.get(0x00e104) >> 20) & 3) == 2);
  m.external[5] = false;
  CHECK(g.sense(5).value() == false);
  m.external[5] = true;
  CHECK(g.sense(5).value() == true);
  // line 12 is in the second register (0x00e108, nibble 4)
  m.external[12] = true;
  CHECK(g.sense(12).value() == true);
  CHECK(!g.sense(16).has_value());           // nv50 has 16 lines
  CHECK(!g.drive(20, true, true));
  // the function: panel power on is log1 = 3 (driven high), off log0 = 2 (released, low)
  CHECK(g.set(gpio_function::kPanelPower, true));
  CHECK(((m.get(0x00e104) >> 20) & 3) == 1);
  CHECK(g.get(gpio_function::kPanelPower).value() == true);
  CHECK(g.set(gpio_function::kPanelPower, false));
  CHECK(((m.get(0x00e104) >> 20) & 3) == 0);   // (log0 2: dir bit set -> driven, low)
  CHECK(g.get(gpio_function::kPanelPower).value() == false);
  CHECK(!g.set(0x77, true));
  CHECK(!g.get(0x77).has_value());
  // hot plug on line 7
  m.external[7] = true;
  CHECK(g.hot_plug(0x51).value() == true);
  m.external[7] = false;
  CHECK(g.hot_plug(0x51).value() == false);
  CHECK(!g.hot_plug(0x52).has_value());     // line 20 does not exist on nv50
  // voltage ID: line 12 high, 13 low -> 1
  m.external[13] = false;
  CHECK(g.voltage_id().value() == 1);
  // interrupts: 0x00e050 enable, 0x00e054 status; bits 15:0 rising, 31:16 falling
  CHECK(g.watch(0x51));
  CHECK(m.get(0x00e050) == ((1u << 7) | (1u << 23)));
  m.regs[0x00e054] = (1u << 7) | (1u << 20) | (1u << 3);   // line 7 rose; line 4 fell (not enabled); line 3 rose (not enabled)
  NvGpio::Changes c = g.poll_interrupts();
  CHECK(c.rising == (1u << 7));
  CHECK(c.falling == 0);
  CHECK(g.changed(c, 0x51));
  CHECK(!g.changed(c, gpio_function::kPanelPower));
  CHECK(m.write_log[0x00e054] == ((1u << 7) | (1u << 20) | (1u << 3)));   // acknowledged what was read
  g.set_interrupts(1u << 7, true, false, false);
  CHECK(m.get(0x00e050) == (1u << 23));
}

static void test_gpio_g94_second_bank() {
  Model m;
  m.nv50_gpio = true;
  BoardInfo b = board_with_gpio({gpio_entry(20, 0x52, 0, 1), gpio_entry(30, gpio_function::kFanSense, 1, 0)});
  m.regs[0x00e280] = 0x22222222;   // lines 16..31 not driven
  m.regs[0x00e284] = 0x22222222;
  NvGpio g(m, chip_of(Family::Tesla, 0x96), b);
  CHECK(g.line_count() == 32);
  m.external[20] = true;
  CHECK(g.hot_plug(0x52).value() == true);
  // inverted function: log1 bit 0 is 0, so the low level means on
  m.external[30] = false;
  CHECK(g.get(gpio_function::kFanSense).value() == true);
  CHECK(g.drive(30, true, true));
  CHECK(((m.get(0x00e284) >> 24) & 3) == 1);
  CHECK(g.watch(0x52));
  CHECK(m.get(0x00e070) == ((1u << 4) | (1u << 20)));
  CHECK(m.get(0x00e050) == 0);
  m.regs[0x00e074] = (1u << 20);            // line 20 fell
  m.regs[0x00e054] = 0xffffffff;            // (nothing of the first bank enabled)
  NvGpio::Changes c = g.poll_interrupts();
  CHECK(c.rising == 0);
  CHECK(c.falling == (1u << 20));
  CHECK(g.changed(c, 0x52));
  CHECK(m.write_log[0x00e074] == (1u << 20));
  CHECK(m.write_log[0x00e054] == 0xffffffffu);
}

static void test_gpio_gf119_family(Family family, uint32_t chipset, bool ga102, uint32_t intr0, uint32_t en0, uint32_t intr1, uint32_t en1) {
  Model m;
  m.gf119_gpio = true;
  m.ga102 = ga102;
  const uint32_t base = ga102 ? 0x021200 : 0x00d610;
  BoardInfo b = board_with_gpio({gpio_entry(3, gpio_function::kFan, 0x2, 0x3), gpio_entry(17, 0x5e, 0, 1)});
  NvGpio g(m, chip_of(family, chipset), b);
  CHECK(g.line_count() == 32);
  m.regs[base + 3 * 4] = 0x000000c9;        // the function bits (low byte) are kept
  CHECK(g.drive(3, true, true));
  CHECK(m.get(base + 3 * 4) == (0x000000c9u | 0x1000u));
  CHECK((m.get(0x00d604) & 1) == 1);
  CHECK(g.sense(3).value() == true);
  CHECK(g.drive(3, false, false));
  CHECK(m.get(base + 3 * 4) == (0x000000c9u | 0x2000u));
  m.external[3] = true;
  CHECK(g.sense(3).value() == true);
  CHECK(g.set(gpio_function::kFan, false));
  CHECK((m.get(base + 3 * 4) & 0x3000) == 0x0000);
  m.regs[base + 17 * 4] = 0x2000;   // not driven
  m.external[17] = true;
  CHECK(g.hot_plug(0x5e).value() == true);
  CHECK(!g.sense(32).has_value());
  CHECK(g.watch(0x5e));
  CHECK(m.get(en1) == ((1u << 1) | (1u << 17)));
  m.regs[intr1] = (1u << 1);                // line 17 rose
  m.regs[intr0] = 0;
  const NvGpio::Changes c = g.poll_interrupts();
  CHECK(c.rising == (1u << 17));
  CHECK(c.falling == 0);
  CHECK(m.write_log[intr1] == (1u << 1));
  (void)en0;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// I2C

static void test_i2c_registers() {
  CHECK(i2c_variant_for(chip_of(Family::Tesla, 0x84)) == I2cVariant::Nv50);
  CHECK(i2c_variant_for(chip_of(Family::Tesla, 0xa3)) == I2cVariant::G94);
  CHECK(i2c_variant_for(chip_of(Family::Fermi, 0xc4)) == I2cVariant::G94);
  CHECK(i2c_variant_for(chip_of(Family::Fermi, 0xd9)) == I2cVariant::Gf119);
  CHECK(i2c_variant_for(chip_of(Family::Kepler, 0xe7)) == I2cVariant::Gf119);
  CHECK(i2c_variant_for(chip_of(Family::Maxwell, 0x117)) == I2cVariant::Gf119);
  CHECK(i2c_variant_for(chip_of(Family::Maxwell, 0x124)) == I2cVariant::Gm200);
  CHECK(i2c_variant_for(chip_of(Family::Ampere, 0x172)) == I2cVariant::Gm200);
  CHECK(i2c_variant_for(chip_of(Family::Nv40, 0x40)) == I2cVariant::None);
  Model m;
  CHECK(NvI2cBus(m, I2cVariant::Nv50, 0).bus_register() == 0x00e138);
  CHECK(NvI2cBus(m, I2cVariant::Nv50, 3).bus_register() == 0x00e180);
  CHECK(NvI2cBus(m, I2cVariant::Nv50, 9).bus_register() == 0x00e7b8);
  CHECK(!NvI2cBus(m, I2cVariant::Nv50, 10).valid());
  CHECK(NvI2cBus(m, I2cVariant::Gf119, 2).bus_register() == 0x00d054);
  CHECK(NvI2cBus(m, I2cVariant::Gm200, 15).bus_register() == 0x00d014 + 15 * 0x20);
  CHECK(!NvI2cBus(m, I2cVariant::Gm200, 16).valid());
  CHECK(!NvI2cBus(m, I2cVariant::None, 0).valid());
  // pads
  m.regs[0x00e550] = 0xffff0003; m.regs[0x00e55c] = 0x1;
  CHECK(set_pad_mode(m, I2cVariant::G94, 1, PadMode::I2c));
  CHECK(m.get(0x00e550) == ((0xffff0003u & ~0xc003u) | 0xc001u));
  CHECK(m.get(0x00e55c) == 0);
  CHECK(set_pad_mode(m, I2cVariant::Gf119, 1, PadMode::Aux));
  CHECK((m.get(0x00e550) & 0xc003) == 0x0002);
  CHECK(set_pad_mode(m, I2cVariant::Gm200, 2, PadMode::Aux));
  CHECK((m.get(0x00d970 + 0xa0) & 0xc003) == 0x0002);
  CHECK(set_pad_mode(m, I2cVariant::Gm200, 2, PadMode::Off));
  CHECK((m.get(0x00d97c + 0xa0) & 1) == 1);
  CHECK(!set_pad_mode(m, I2cVariant::Nv50, 0, PadMode::I2c));
  CHECK(!set_pad_mode(m, I2cVariant::Gm200, kMaxHybridPads, PadMode::I2c));
}

static void test_i2c_edid(I2cVariant variant, uint8_t drive, uint32_t reg, bool gf119, uint8_t pad) {
  Model m;
  I2cSlave s;
  s.gf119 = gf119;
  s.reg = reg;
  s.edid = make_edid(3);
  m.slaves.push_back(&s);
  NvI2cBus bus(m, variant, drive, pad);
  CHECK(bus.valid());
  CHECK(bus.bus_register() == reg);
  ByteBuffer edid(g_alloc);
  CHECK(i2c_edid::read(bus, &edid));
  CHECK(bus.last_status() == I2cStatus::Ok);
  CHECK(edid.size() == s.edid.size());
  CHECK(edid.size() == 512 && memcmp(edid.view().data(), s.edid.data(), 512) == 0);
  // the segment pointer was written for blocks 2 and 3 (one transfer each: segment, offset, read with repeated STARTs)
  uint32_t segment_writes = 0;
  for (uint8_t a : s.addresses) if (a == 0x60) ++segment_writes;
  CHECK(segment_writes == 2);
  CHECK(s.repeated > 0);
  CHECK(s.stops == s.starts - s.repeated);
  CHECK(s.scl && s.sda);                    // released at the end
  EdidInfo info;
  CHECK(parse_edid(edid.view(), &info));
  CHECK(info.valid && info.checksum_ok);
  CHECK(strcmp(info.manufacturer, "DEL") == 0);
  CHECK(info.product_code == 0x1234);
  CHECK(info.extension_blocks == 3);
  if (pad != kNoPad) {
    const uint32_t mode = variant == I2cVariant::Gm200 ? 0x00d970 : 0x00e500;
    CHECK((m.get(mode + pad * 0x50) & 0xc003) == 0xc001);
  }
}

static void test_i2c_faults() {
  // a byte flipped on the wire in the first read: the block checksum fails, the read is tried again
  {
    Model m;
    I2cSlave s;
    s.reg = 0x00e150;
    s.edid = make_edid(1);
    s.flip_reads = 1;
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Nv50, 1);
    ByteBuffer edid(g_alloc);
    CHECK(i2c_edid::read(bus, &edid));
    CHECK(edid.size() == 256 && memcmp(edid.view().data(), s.edid.data(), 256) == 0);
  }
  // nobody at the address: the address byte is not acknowledged, the transfer ends with a STOP
  {
    Model m;
    I2cSlave s;
    s.reg = 0x00e150;
    s.present = false;
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Nv50, 1);
    uint8_t byte = 0;
    II2cBus::Message msg;
    msg.address = 0x50;
    msg.data = ByteView(&byte, 1);
    CHECK(!bus.transfer(ArrayView<II2cBus::Message>(&msg, 1)));
    CHECK(bus.last_status() == I2cStatus::Nack);
    CHECK(s.scl && s.sda);
    ByteBuffer edid(g_alloc);
    CHECK(!i2c_edid::read(bus, &edid));
  }
  // a device at another address only: 0x50 NACKs
  {
    Model m;
    I2cSlave s;
    s.reg = 0x00e150;
    s.edid = make_edid(0);
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Nv50, 1);
    uint8_t byte = 0;
    II2cBus::Message msg;
    msg.address = 0x37;
    msg.read = true;
    msg.data = ByteView(&byte, 1);
    CHECK(!bus.transfer(ArrayView<II2cBus::Message>(&msg, 1)));
    CHECK(bus.last_status() == I2cStatus::Nack);
    msg.address = 0x80;     // not a 7-bit address
    CHECK(!bus.transfer(ArrayView<II2cBus::Message>(&msg, 1)));
    CHECK(bus.last_status() == I2cStatus::Invalid);
    CHECK(!bus.transfer(ArrayView<II2cBus::Message>()));
  }
  // clock stretching within the limit
  {
    Model m;
    I2cSlave s;
    s.gf119 = true;
    s.reg = 0x00d034;
    s.edid = make_edid(0);
    s.stretch_polls = 1000;   // 1 ms of the 2.2 ms each rise may take
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Gf119, 1);
    ByteBuffer edid(g_alloc);
    CHECK(i2c_edid::read(bus, &edid));
    CHECK(edid.size() == 128);
  }
  // a slave that holds the clock forever: every rise times out, the reads give up, bounded
  {
    Model m;
    I2cSlave s;
    s.gf119 = true;
    s.reg = 0x00d034;
    s.edid = make_edid(0);
    s.stretch_forever = true;
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Gf119, 1);
    uint8_t buf[4] = {};
    II2cBus::Message msg;
    msg.address = 0x50;
    msg.read = true;
    msg.data = ByteView(buf, 4);
    const uint64_t before = m.reads;
    CHECK(!bus.transfer(ArrayView<II2cBus::Message>(&msg, 1)));
    CHECK(bus.last_status() == I2cStatus::Timeout);
    CHECK(m.reads - before < 20000);
    ByteBuffer edid(g_alloc);
    CHECK(!i2c_edid::read(bus, &edid));
  }
  // a clock line shorted low: the START cannot be made
  {
    Model m;
    I2cSlave s;
    s.reg = 0x00e138;
    s.scl_stuck_low = true;
    s.update();
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Nv50, 0);
    uint8_t byte = 0;
    II2cBus::Message msg;
    msg.address = 0x50;
    msg.data = ByteView(&byte, 1);
    CHECK(!bus.transfer(ArrayView<II2cBus::Message>(&msg, 1)));
    CHECK(bus.last_status() == I2cStatus::Busy || bus.last_status() == I2cStatus::Timeout);
  }
  // an EEPROM that is not an EDID (all ones past a header): nothing is accepted
  {
    Model m;
    I2cSlave s;
    s.reg = 0x00e138;
    s.edid = make_edid(0);
    s.edid.resize(64);
    m.slaves.push_back(&s);
    NvI2cBus bus(m, I2cVariant::Nv50, 0);
    ByteBuffer edid(g_alloc);
    CHECK(!i2c_edid::read(bus, &edid));
  }
  // random line levels from a broken pad: no crash, bounded
  {
    struct Noise final : INvRegisters {
      uint64_t n = 0;
      uint32_t read32(uint32_t) override { ++n; return rnd(); }
      void write32(uint32_t, uint32_t) override {}
    } noise;
    NvI2cBus bus(noise, I2cVariant::Gf119, 0);
    for (int i = 0; i < 20; ++i) {
      ByteBuffer edid(g_alloc);
      (void)i2c_edid::read(bus, &edid);
      CHECK(edid.size() % 128 == 0);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// AUX

static void test_aux_basic(I2cVariant variant, uint8_t channel, uint32_t base, bool gm200) {
  Model m;
  AuxEngine a;
  a.gm200 = gm200;
  a.base = base;
  a.edid = make_edid(2);
  // a DPCD that parses: DP 1.2, HBR2, 4 lanes, enhanced framing, TPS3
  a.dpcd[0] = 0x12; a.dpcd[1] = 0x14; a.dpcd[2] = 0xc4; a.dpcd[3] = 0x01; a.dpcd[4] = 0; a.dpcd[5] = 0; a.dpcd[6] = 0x01; a.dpcd[7] = 0;
  m.aux.push_back(&a);
  NvAuxChannel aux(m, variant, channel);
  CHECK(aux.valid());
  // a native read of 20 bytes: two transactions (16 + 4)
  uint8_t buf[20] = {};
  CHECK(aux.dpcd_read(0x100, ByteView(buf, 20)));
  CHECK(aux.last_status() == AuxStatus::Ok);
  for (uint32_t i = 0; i < 20; ++i) CHECK(buf[i] == a.dpcd[0x100 + i]);
  CHECK(a.requests.size() == 2);
  CHECK(a.requests[0].type == 9 && a.requests[0].size == 16 && a.requests[0].address == 0x100);
  CHECK(a.requests[1].size == 4 && a.requests[1].address == 0x110);
  // the claim was released and the pad put in AUX mode
  CHECK((a.ctrl & (gm200 ? 0x00700000u : 0x00300000u)) == 0);
  const uint32_t pad_mode = gm200 ? 0x00d970 : 0x00e500;
  CHECK((m.get(pad_mode + channel * 0x50) & 0xc003) == 0x0002);
  if (variant != I2cVariant::G94) CHECK((a.read(base + 0x38) & 0x10000) == 0x10000);   // the automatic DPCD polling back on
  // the shared DPCD parser
  Dpcd::Capabilities caps;
  CHECK(Dpcd::read_capabilities(aux, false, &caps));
  CHECK(caps.valid && caps.revision == 0x12 && caps.max_link_rate_code == 0x14 && caps.max_lanes == 4 && caps.enhanced_framing && caps.tps3);
  // a native write of 3 bytes, data in the out registers little-endian
  const uint8_t w[3] = {0x0a, 0x84, 0x01};
  a.requests.clear();
  CHECK(aux.dpcd_write(0x100, ConstByteView(w, 3)));
  CHECK(a.writes.size() == 1 && a.writes[0].first == 0x100 && a.writes[0].second == std::vector<uint8_t>(w, w + 3));
  CHECK(a.requests.size() == 1 && a.requests[0].type == 8 && a.requests[0].size == 3);
  CHECK(a.out[0] == 0x0001840a);
  // EDID over I2C over AUX
  a.requests.clear();
  ByteBuffer edid(g_alloc);
  CHECK(aux.read_edid(&edid));
  CHECK(edid.size() == 384 && memcmp(edid.view().data(), a.edid.data(), 384) == 0);
  EdidInfo info;
  CHECK(parse_edid(edid.view(), &info));
  CHECK(info.valid && info.extension_blocks == 2 && info.product_code == 0x1234);
  uint32_t bare = 0, segment = 0;
  for (const AuxEngine::Req& r : a.requests) {
    if (r.address_only) ++bare;
    if (r.address == 0x30 && !r.address_only) ++segment;
    CHECK(r.size <= 16);
  }
  if (variant == I2cVariant::G94) CHECK(bare == 0);   // G94 cannot send address-only requests
  else CHECK(bare > 0);
  CHECK(segment == 1);
}

static void test_aux_faults(I2cVariant variant, uint32_t base, bool gm200) {
  auto fresh = [&](Model& m, AuxEngine& a) { a.gm200 = gm200; a.base = base; a.edid = make_edid(1); m.aux.push_back(&a); };
  uint8_t buf[16] = {};
  // DEFERs: a few are retried, too many give up
  {
    Model m; AuxEngine a; fresh(m, a);
    NvAuxChannel aux(m, variant, 0);
    a.native_defers = 5;
    CHECK(aux.dpcd_read(0, ByteView(buf, 8)));
    CHECK(a.requests.size() == 6);
    CHECK(m.delayed_us >= 5 * NvAuxChannel::kRetryDelayUs);
    a.native_defers = 1000;
    a.requests.clear();
    CHECK(!aux.dpcd_read(0, ByteView(buf, 8)));
    CHECK(aux.last_status() == AuxStatus::Defer);
    CHECK(a.requests.size() == NvAuxChannel::kNativeTries);
    a.native_defers = 0;
    a.i2c_defers = 3;
    ByteBuffer edid(g_alloc);
    CHECK(aux.read_edid(&edid));
    CHECK(edid.size() == 256);
    a.i2c_defers = 100000;
    CHECK(!aux.read_edid(&edid));
    a.i2c_defers = 0;
  }
  // a transaction that never completes, the status's timeout bit, wire errors: retried, then reported
  {
    Model m; AuxEngine a; fresh(m, a);
    NvAuxChannel aux(m, variant, 0);
    a.never_complete = 2;
    CHECK(aux.dpcd_read(0, ByteView(buf, 4)));
    a.never_complete = 1000;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 4)));
    CHECK(aux.last_status() == AuxStatus::Timeout);
    a.never_complete = 0;
    a.timeouts = 3;
    CHECK(aux.dpcd_read(0, ByteView(buf, 4)));
    a.timeouts = 1000;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 4)));
    CHECK(aux.last_status() == AuxStatus::Timeout);
    a.timeouts = 0;
    a.wire_errors = 1000;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 4)));
    CHECK(aux.last_status() == AuxStatus::WireError);
    a.wire_errors = 0;
    uint8_t reply = 0; uint32_t done = 0;
    a.timeouts = 1;
    CHECK(aux.xfer(NvAuxChannel::kNativeRead, 0, ByteView(buf, 1), &reply, &done) == AuxStatus::Timeout);
    CHECK(done == 0);
    a.timeouts = 0;
  }
  // short replies: retried until whole; always short fails; the EDID read goes on from where a short I2C read stopped
  {
    Model m; AuxEngine a; fresh(m, a);
    NvAuxChannel aux(m, variant, 0);
    a.short_reads = 2;
    CHECK(aux.dpcd_read(0x20, ByteView(buf, 16)));
    for (uint32_t i = 0; i < 16; ++i) CHECK(buf[i] == a.dpcd[0x20 + i]);
    a.always_short = true;
    CHECK(!aux.dpcd_read(0x20, ByteView(buf, 16)));
    CHECK(aux.last_status() == AuxStatus::Protocol);
    a.always_short = false;
    a.short_reads = 5;
    ByteBuffer edid(g_alloc);
    CHECK(aux.read_edid(&edid));
    CHECK(edid.size() == 256 && memcmp(edid.view().data(), a.edid.data(), 256) == 0);
  }
  // reply lengths that make no sense (more than asked for, up to 31): never copied, a protocol error
  {
    Model m; AuxEngine a; fresh(m, a);
    NvAuxChannel aux(m, variant, 0);
    uint8_t reply = 0; uint32_t done = 0;
    uint8_t small[2] = {0xee, 0xee};
    a.garbage_lengths = 1;
    CHECK(aux.xfer(NvAuxChannel::kNativeRead, 0, ByteView(small, 2), &reply, &done) == AuxStatus::Protocol);
    CHECK(done == 0 && small[0] == 0xee && small[1] == 0xee);
    a.garbage_lengths = 1000;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 16)));
    CHECK(aux.last_status() == AuxStatus::Protocol);
    a.garbage_lengths = 0;
  }
  // NACK, no sink, a channel that never goes idle or is never granted
  {
    Model m; AuxEngine a; fresh(m, a);
    NvAuxChannel aux(m, variant, 0);
    a.nack_native = true;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 1)));
    CHECK(aux.last_status() == AuxStatus::Nack);
    CHECK(a.requests.size() == 1);
    a.nack_native = false;
    CHECK(!aux.dpcd_read(0x7ff, ByteView(buf, 2)));   // past the sink's DPCD: NACK
    a.sink_present = false;
    a.requests.clear();
    CHECK(!aux.dpcd_read(0, ByteView(buf, 1)));
    CHECK(aux.last_status() == AuxStatus::NoSink);
    CHECK(a.requests.empty());
    ByteBuffer edid(g_alloc);
    CHECK(!aux.read_edid(&edid));
    a.sink_present = true;
    a.never_idle = true;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 1)));
    CHECK(aux.last_status() == AuxStatus::Busy);
    a.never_idle = false;
    a.never_granted = true;
    CHECK(!aux.dpcd_read(0, ByteView(buf, 1)));
    CHECK(aux.last_status() == AuxStatus::Busy);
    CHECK((a.ctrl & (gm200 ? 0x00700000u : 0x00300000u)) == 0);   // the request taken back
    a.never_granted = false;
    // requests the channel cannot carry
    uint8_t big[17] = {};
    uint8_t reply = 0; uint32_t done = 0;
    CHECK(aux.xfer(NvAuxChannel::kNativeRead, 0, ByteView(big, 17), &reply, &done) == AuxStatus::Invalid);
    CHECK(aux.xfer(NvAuxChannel::kNativeRead, 0x100000, ByteView(big, 1), &reply, &done) == AuxStatus::Invalid);
    CHECK(!aux.dpcd_read(0xffff8, ByteView(big, 16)));
    CHECK(!NvAuxChannel(m, variant, NvAuxChannel::kMaxChannels).valid());
    CHECK(!NvAuxChannel(m, I2cVariant::Nv50, 0).valid());
  }
  // random replies from a broken engine: no crash, the buffers stay in bounds, every call returns
  {
    Model m; AuxEngine a; fresh(m, a);
    a.random = true;
    NvAuxChannel aux(m, variant, 0);
    for (int i = 0; i < 300; ++i) {
      uint8_t data[16] = {};
      const uint32_t n = 1 + rnd() % 16;
      if (rnd() & 1) (void)aux.dpcd_read(rnd() % 0x1000, ByteView(data, n));
      else (void)aux.dpcd_write(rnd() % 0x1000, ConstByteView(data, n));
    }
    for (int i = 0; i < 30; ++i) {
      ByteBuffer edid(g_alloc);
      if (aux.read_edid(&edid)) CHECK(edid.size() % 128 == 0 && edid.size() >= 128);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// EDID by connector

static void test_connector_edid() {
  Model m;
  // DDC bus 1 on GF119 registers (0x00d034) for connector 0 (HDMI); AUX channel 0 for connector 1 (DP); connector 2 is DP++ on a PMGR entry
  // (bus 3 sharing AUX channel 1) with an HDMI monitor (the AUX side has no sink)
  I2cSlave hdmi; hdmi.gf119 = true; hdmi.reg = 0x00d034; hdmi.edid = make_edid(1);
  I2cSlave dpp; dpp.gf119 = true; dpp.reg = 0x00d074; dpp.edid = make_edid(0); dpp.edid[11] = 0x56; dpp.edid[127] = static_cast<uint8_t>(dpp.edid[127] + 0x12 - 0x56);
  AuxEngine dp; dp.base = 0x00e4c0; dp.edid = make_edid(2);
  AuxEngine dpp_aux; dpp_aux.base = 0x00e4c0 + 0x50; dpp_aux.sink_present = false;
  m.slaves = {&hdmi, &dpp};
  m.aux = {&dp, &dpp_aux};
  BoardInfo b;
  b.i2c_count = 3;
  b.i2c[0].type = I2cPortType::NvioBit; b.i2c[0].drive = 1;
  b.i2c[1].type = I2cPortType::NvioAux; b.i2c[1].aux = 0;
  b.i2c[2].type = I2cPortType::Pmgr; b.i2c[2].drive = 3; b.i2c[2].aux = 1;
  b.output_count = 4;
  b.outputs[0].type = DcbOutputType::Tmds; b.outputs[0].connector = 0; b.outputs[0].i2c_index = 0;
  b.outputs[1].type = DcbOutputType::Dp; b.outputs[1].connector = 1; b.outputs[1].i2c_index = 1;
  b.outputs[2].type = DcbOutputType::Tmds; b.outputs[2].connector = 2; b.outputs[2].i2c_index = 2;
  b.outputs[3].type = DcbOutputType::Dp; b.outputs[3].connector = 2; b.outputs[3].i2c_index = 2;
  b.connector_count = 4;
  const Chip chip = chip_of(Family::Kepler, 0xe4);

  const ConnectorRoute r2 = connector_route(b, 2);
  CHECK(r2.found && r2.displayport && r2.route.drive == 3 && r2.route.aux == 1 && r2.route.pad == 1);
  CHECK(!connector_route(b, 3).found);
  CHECK(!i2c_route(b, 9).has_bus());

  ByteBuffer edid(g_alloc);
  EdidInfo info;
  CHECK(read_connector_edid(m, chip, b, 0, &edid) == EdidSource::Ddc);
  CHECK(edid.size() == 256 && parse_edid(edid.view(), &info) && info.extension_blocks == 1);
  CHECK(read_connector_edid(m, chip, b, 1, &edid) == EdidSource::Aux);
  CHECK(edid.size() == 384 && parse_edid(edid.view(), &info) && info.extension_blocks == 2);
  CHECK(read_connector_edid(m, chip, b, 2, &edid) == EdidSource::Ddc);
  CHECK(edid.size() == 128 && parse_edid(edid.view(), &info) && info.product_code == 0x5634);
  CHECK((m.get(0x00e500 + 0x50) & 0xc003) == 0xc001);   // the hybrid pad switched to I2C for the DDC read
  CHECK(read_connector_edid(m, chip, b, 3, &edid) == EdidSource::None);
  // nothing on connector 0's bus
  hdmi.present = false;
  CHECK(read_connector_edid(m, chip, b, 0, &edid) == EdidSource::None);
  // garbage tables: indices past the end, types not handled
  BoardInfo junk;
  junk.output_count = 200; junk.i2c_count = 200;
  for (uint32_t i = 0; i < BoardInfo::kMaxOutputs; ++i) { junk.outputs[i].type = static_cast<DcbOutputType>(rnd() % 16); junk.outputs[i].connector = static_cast<uint8_t>(rnd() % 4); junk.outputs[i].i2c_index = static_cast<uint8_t>(rnd()); }
  for (uint32_t i = 0; i < BoardInfo::kMaxI2c; ++i) { junk.i2c[i].type = static_cast<I2cPortType>(rnd() % 8 == 0 ? 0x80 : rnd() % 8); junk.i2c[i].drive = static_cast<uint8_t>(rnd()); junk.i2c[i].aux = static_cast<uint8_t>(rnd()); junk.i2c[i].share = static_cast<uint8_t>(rnd()); }
  for (uint32_t c = 0; c < 4; ++c) (void)read_connector_edid(m, chip, junk, c, &edid);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Thermal sensor, fan and PWM

static void test_temperature_decoding() {
  ThermSensorCalibration none;
  CHECK(!therm_decode::nv50_millicelsius(1000, none).has_value());
  ThermSensorCalibration cal;
  cal.slope_mult = 1; cal.slope_div = 2; cal.offset_num = 10; cal.offset_den = 1; cal.offset_constant = 0;
  // 100 * 1 / 2 + 10 + 0 - 8 = 52
  CHECK(therm_decode::nv50_millicelsius(100, cal).value() == 52000);
  CHECK(therm_decode::nv50_millicelsius(0, cal).value() == 2000);
  cal.offset_constant = -50;
  CHECK(therm_decode::nv50_millicelsius(0, cal).value() == 0);   // clamped, not negative
  cal.offset_constant = 0;
  CHECK(!therm_decode::nv50_millicelsius(0x3fff, cal).has_value());   // 8200 degrees: garbage
  CHECK(therm_decode::nv50_millicelsius(0xffffc000u | 100, cal).value() == 52000);   // bits above 13 ignored
  CHECK(therm_decode::g84_millicelsius(45).value() == 45000);
  CHECK(therm_decode::g84_millicelsius(0).value() == 0);
  CHECK(!therm_decode::g84_millicelsius(151).has_value());
  CHECK(!therm_decode::g84_millicelsius(0xffffffff).has_value());
  CHECK(therm_decode::gp100_millicelsius(0x20000000 | (45u << 8)).value() == 45000);
  CHECK(therm_decode::gp100_millicelsius(0x20000000 | (45u << 8) | (16u << 3)).value() == 45500);   // 16/32 of a degree
  CHECK(!therm_decode::gp100_millicelsius(45u << 8).has_value());                                   // not valid
  CHECK(!therm_decode::gp100_millicelsius(0x2001ff00).has_value());                                 // 511 degrees
  CHECK(!therm_decode::gp100_millicelsius(0xffffffff).has_value());
  for (int i = 0; i < 100000; ++i) {
    const uint32_t raw = rnd() << 7 ^ rnd();
    const Optional<int32_t> a = therm_decode::gp100_millicelsius(raw), b = therm_decode::g84_millicelsius(raw), c = therm_decode::nv50_millicelsius(raw, cal);
    if (a.has_value()) CHECK(a.value() >= 0 && a.value() <= 150000);
    if (b.has_value()) CHECK(b.value() >= 0 && b.value() <= 150000);
    if (c.has_value()) CHECK(c.value() >= 0 && c.value() <= 150000);
    ThermSensorCalibration r;
    r.slope_mult = static_cast<int32_t>(rnd()); r.slope_div = static_cast<int32_t>(rnd()) - 1000; r.offset_num = static_cast<int32_t>(rnd());
    r.offset_den = static_cast<int32_t>(rnd() % 7) - 3; r.offset_constant = static_cast<int32_t>(rnd()) - (1 << 23);
    const Optional<int32_t> d = therm_decode::nv50_millicelsius(raw, r);
    if (d.has_value()) CHECK(d.value() >= 0 && d.value() <= 150000);
  }
}

static void test_temperature_per_family() {
  BoardInfo b;
  // G84 (Tesla): the fuse at 0x021000 + 0x1a8 must be 1
  {
    Model m;
    NvGpio g(m, chip_of(Family::Tesla, 0x84), b);
    NvTherm t(m, chip_of(Family::Tesla, 0x84), g);
    m.regs[0x020400] = 61;
    CHECK(t.initialize());
    CHECK(!t.temperature_millicelsius().has_value());
    m.regs[0x0211a8] = 1;
    CHECK(t.temperature_millicelsius().value() == 61000);
    CHECK((m.get(0x001084) & 0x800) == 0);   // the fuse enable put back
  }
  // Kepler: the fuse at 0x021100 + 0x1a8
  {
    Model m;
    NvGpio g(m, chip_of(Family::Kepler, 0xe4), b);
    NvTherm t(m, chip_of(Family::Kepler, 0xe4), g);
    m.regs[0x0212a8] = 1;
    m.regs[0x020400] = 72;
    CHECK(t.initialize());
    CHECK((m.get(0x020008) & 0x80008000) == 0x80000000);   // g84_sensor_setup
    CHECK(t.temperature_millicelsius().value() == 72000);
    m.regs[0x020400] = 0xdeadbeef;
    CHECK(!t.temperature_millicelsius().has_value());
  }
  // G80 with its calibration
  {
    Model m;
    ThermSensorCalibration cal;
    cal.slope_mult = 1; cal.slope_div = 4; cal.offset_num = 30; cal.offset_den = 1;
    NvGpio g(m, chip_of(Family::Tesla, 0x50), b);
    NvTherm t(m, chip_of(Family::Tesla, 0x50), g, cal);
    m.regs[0x020010] = 0x40000000;
    m.regs[0x020014] = 160;   // 40 + 30 - 8 = 62
    CHECK(t.initialize());
    CHECK((m.get(0x020010) & 0x40000000) == 0);
    CHECK(t.temperature_millicelsius().value() == 62000);
  }
  // Pascal / Turing / Ampere: 0x020460
  for (Family f : {Family::Pascal, Family::Turing, Family::Ampere}) {
    Model m;
    NvGpio g(m, chip_of(f, 0x134), b);
    NvTherm t(m, chip_of(f, 0x134), g);
    m.regs[0x020460] = 0x20000000 | (55u << 8) | (8u << 3);
    CHECK(t.initialize());
    CHECK(t.temperature_millicelsius().value() == 55250);
    m.regs[0x020460] = 55u << 8;
    CHECK(!t.temperature_millicelsius().has_value());
    CHECK(!t.has_pwm());
  }
  // a chip without a sensor here
  {
    Model m;
    NvGpio g(m, chip_of(Family::Nv40, 0x40), b);
    NvTherm t(m, chip_of(Family::Nv40, 0x40), g);
    CHECK(!t.initialize());
    CHECK(!t.temperature_millicelsius().has_value());
  }
}

static void test_fan_tach() {
  // GT215: the counter; initialize() routes line 14 to it (bits 20:16) and gates it on one second of a 27 MHz crystal
  {
    Model m;
    m.nv50_gpio = true;
    m.regs[0x101000] = 0x00400000;
    BoardInfo b = board_with_gpio({gpio_entry(14, gpio_function::kFanSense, 0, 1)});
    NvGpio g(m, chip_of(Family::Tesla, 0xa3), b);
    NvTherm t(m, chip_of(Family::Tesla, 0xa3), g);
    CHECK(t.initialize());
    CHECK(t.has_fan_sense());
    CHECK(m.get(0x00e724) == 27000000);
    CHECK(((m.get(0x00e720) >> 16) & 0x1f) == 14);
    CHECK((m.get(0x00e720) & 3) == 1);
    m.regs[0x00e728] = 40;
    CHECK(t.fan_rpm().value() == 1200);
    m.regs[0x00e728] = 0xffff0000;
    CHECK(t.fan_rpm().value() == 0);
    m.regs[0x00e720] = 0;
    CHECK(!t.fan_rpm().has_value());
  }
  // GF119: the line goes to 0x00d79c instead
  {
    Model m;
    m.gf119_gpio = true;
    m.regs[0x101000] = 0x00400040;
    BoardInfo b = board_with_gpio({gpio_entry(9, gpio_function::kFanSenseAlt, 0, 1)});
    NvGpio g(m, chip_of(Family::Kepler, 0xe4), b);
    NvTherm t(m, chip_of(Family::Kepler, 0xe4), g);
    CHECK(t.initialize());
    CHECK((m.get(0x00d79c) & 0xff) == 9);
    CHECK(m.get(0x00e724) == 25000000);
    m.regs[0x00e728] = 55;
    CHECK(t.fan_rpm().value() == 1650);
  }
  // G84 (no counter): the fan-sense line timed on PTIMER; 1200 RPM = a change every 12.5 ms
  {
    Model m;
    m.nv50_gpio = true;
    m.timer_step_ns = 500000;   // 500 us per read of the low word (one per poll, as the polls are 500 us apart)
    m.regs[0x00e104] = 2u << 24;  // line 6 not driven
    m.fan_line = 6;
    m.fan_half_period_ns = 12500000;
    BoardInfo b = board_with_gpio({gpio_entry(6, gpio_function::kFanSense, 0, 1)});
    NvGpio g(m, chip_of(Family::Tesla, 0x84), b);
    NvTherm t(m, chip_of(Family::Tesla, 0x84), g);
    CHECK(t.initialize());
    const Optional<uint32_t> rpm = t.fan_rpm();
    CHECK(rpm.has_value() && rpm.value() > 1180 && rpm.value() < 1220);
    m.fan_half_period_ns = 0;   // standing still
    CHECK(t.fan_rpm().value() == 0);
    m.fan_half_period_ns = 1;   // a timer that does not move but a line that changes at every read: no number made up
    m.timer_step_ns = 0;
    m.timer_ns = 0;
    const Optional<uint32_t> stuck = t.fan_rpm();
    CHECK(!stuck.has_value() || stuck.value() == 0);
  }
  // no fan sense on the board
  {
    Model m;
    BoardInfo b;
    NvGpio g(m, chip_of(Family::Kepler, 0xe4), b);
    NvTherm t(m, chip_of(Family::Kepler, 0xe4), g);
    (void)t.initialize();
    CHECK(!t.has_fan_sense());
    CHECK(!t.fan_rpm().has_value());
  }
}

static void test_pwm() {
  CHECK(therm_decode::pwm_duty_255(100, 50, false).value() == 127);
  CHECK(therm_decode::pwm_duty_255(100, 50, true).value() == 127);
  CHECK(therm_decode::pwm_duty_255(100, 25, true).value() == 191);
  CHECK(therm_decode::pwm_duty_255(100, 300, false).value() == 255);   // duty beyond the divider: full
  CHECK(!therm_decode::pwm_duty_255(0, 0, false).has_value());
  CHECK(therm_decode::pwm_duty_register(100, 255, false) == 100);
  CHECK(therm_decode::pwm_duty_register(100, 0, false) == 0);
  CHECK(therm_decode::pwm_duty_register(100, 0, true) == 100);
  for (uint32_t d = 0; d < 256; ++d) {
    const uint32_t reg = therm_decode::pwm_duty_register(1000, static_cast<uint8_t>(d), false);
    const uint8_t back = therm_decode::pwm_duty_255(1000, reg, false).value();
    CHECK(uint32_t{back} + 1 >= d && back <= d);
  }
  // Tesla, fan on line 9: unit 1 (0x00e11c / 0x00e120), enabled by bit 9 of 0x00e100
  {
    Model m;
    m.nv50_gpio = true;
    BoardInfo b = board_with_gpio({gpio_entry(9, gpio_function::kFan, 0, 1)});
    NvGpio g(m, chip_of(Family::Tesla, 0x96), b);
    NvTherm t(m, chip_of(Family::Tesla, 0x96), g);
    m.regs[0x00e100] = 1u << 9;
    m.regs[0x00e11c] = 200;
    m.regs[0x00e120] = 0x80000000 | 50;
    CHECK(t.initialize());
    CHECK(t.has_pwm());
    PwmState s = t.pwm().value();
    CHECK(s.duty == 63 && s.mode == PwmMode::Automatic);
    CHECK(t.set_pwm_duty(255));
    CHECK(m.get(0x00e11c) == 200 && m.get(0x00e120) == (0x80000000u | 200));
    CHECK(t.pwm().value().duty == 255 && t.pwm().value().mode == PwmMode::Manual);
    CHECK(t.set_pwm_duty(128));
    CHECK((m.get(0x00e120) & 0xffff) == 101);
    CHECK(t.restore_firmware_pwm());
    CHECK(m.get(0x00e120) == (0x80000000u | 50) && m.get(0x00e100) == (1u << 9));
    CHECK(t.pwm().value().mode == PwmMode::Automatic);
    // PWM off: the fan's GPIO level tells full or nothing
    m.regs[0x00e100] = 0;
    m.regs[0x00e108] = 0x00000020;   // line 9: nibble 1 of the second register, released
    m.external[9] = true;
    CHECK(t.pwm().value().duty == 255);
  }
  // Tesla, a fan line without a PWM unit
  {
    Model m;
    BoardInfo b = board_with_gpio({gpio_entry(5, gpio_function::kFan, 0, 1)});
    NvGpio g(m, chip_of(Family::Tesla, 0x96), b);
    NvTherm t(m, chip_of(Family::Tesla, 0x96), g);
    (void)t.initialize();
    CHECK(!t.has_pwm());
    CHECK(!t.pwm().has_value());
    CHECK(!t.set_pwm_duty(10));
  }
  // GF119/Kepler: the unit by the line's GPIO function; 0x19 is unit 1, 0x00/0x1e the PTHERM unit, others none
  {
    Model m;
    m.gf119_gpio = true;
    BoardInfo b = board_with_gpio({gpio_entry(4, gpio_function::kFan, 1, 0)});   // inverted: off is high
    NvGpio g(m, chip_of(Family::Kepler, 0xe4), b);
    NvTherm t(m, chip_of(Family::Kepler, 0xe4), g);
    m.regs[0x00d610 + 16] = 0x40 | 0x19;
    m.regs[0x00e11c] = 1000;
    m.regs[0x00e120] = 250;     // inverted: 75 % on
    CHECK(t.initialize());
    CHECK(t.pwm().value().duty == 191);
    CHECK(t.set_pwm_duty(0));
    CHECK(m.get(0x00e120) == (0x80000000u | 1000));
    CHECK(t.restore_firmware_pwm());
    CHECK(m.get(0x00e120) == (0x80000000u | 250));
    // the PTHERM unit: 13-bit fields, the high bits of the divider kept, commit bit 30
    m.regs[0x00d610 + 16] = 0x1e;
    m.regs[0x0200d8] = 0xabc00000 | 0x800;
    m.regs[0x0200dc] = 0x400;
    CHECK(t.initialize());
    CHECK(t.pwm().value().duty == 127);
    CHECK(t.set_pwm_duty(255));
    CHECK(m.get(0x0200d8) == (0xabc00000u | 0x800) && m.get(0x0200dc) == 0x40000000u);
    m.regs[0x00d610 + 16] = 0x80 | 0x19;    // a mode nouveau does not know
    CHECK(!t.initialize() || !t.has_pwm());
    CHECK(!t.has_pwm());
  }
  // GM107: 0x10eb10 + 4 * line, hardwired
  {
    Model m;
    m.gf119_gpio = true;
    BoardInfo b = board_with_gpio({gpio_entry(4, gpio_function::kFan, 0, 1)});
    NvGpio g(m, chip_of(Family::Maxwell, 0x117), b);
    NvTherm t(m, chip_of(Family::Maxwell, 0x117), g);
    m.regs[0x10eb20] = 0xe000 | 0x100;
    m.regs[0x10eb24] = 0x80;
    CHECK(t.initialize());
    CHECK(t.pwm().value().duty == 127);
    CHECK(t.set_pwm_duty(255));
    CHECK(m.get(0x10eb20) == (0xe000u | 0x100) && m.get(0x10eb24) == (0x80000000u | 0x100));
    // a divider of zero: no frequency to set a duty with (the saved one is used while there is one)
    m.regs[0x10eb20] = 0;
    CHECK(t.set_pwm_duty(10));
    m.regs[0x10eb20] = 0;
    CHECK(t.initialize());
    CHECK(!t.pwm().has_value());
    CHECK(!t.set_pwm_duty(10));
  }
}

static void test_monitor() {
  Model m;
  m.gf119_gpio = true;
  m.regs[0x101000] = 0x00400000;
  BoardInfo b = board_with_gpio({gpio_entry(4, gpio_function::kFan, 0, 1), gpio_entry(9, gpio_function::kFanSense, 0, 1)});
  const Chip chip = chip_of(Family::Kepler, 0xe7);
  NvGpio g(m, chip, b);
  NvThermMonitor mon(m, chip, g);
  CHECK(mon.channel_count(MonitorChannel::Temperature) == 0);   // nothing before initialize()
  m.regs[0x0212a8] = 1;
  m.regs[0x020400] = 48;
  m.regs[0x00d610 + 16] = 0x40 | 0x1c;   // unit 0
  m.regs[0x00e114] = 100;
  m.regs[0x00e118] = 40;
  CHECK(mon.initialize());
  CHECK(strcmp(mon.name(), "NVIDIA GPU sensors") == 0);
  CHECK(mon.channel_count(MonitorChannel::Temperature) == 1);
  CHECK(mon.channel_count(MonitorChannel::Fan) == 1);
  CHECK(mon.channel_count(MonitorChannel::Pwm) == 1);
  CHECK(mon.channel_count(MonitorChannel::Voltage) == 0);
  CHECK(strcmp(mon.label(MonitorChannel::Temperature, 0), "GPU") == 0);
  CHECK(mon.label(MonitorChannel::Temperature, 1) == nullptr);
  CHECK(mon.temperature_millicelsius(0).value() == 48000);
  CHECK(!mon.temperature_millicelsius(1).has_value());
  m.regs[0x00e728] = 30;
  CHECK(mon.fan_rpm(0).value() == 900);
  CHECK(!mon.voltage_millivolts(0).has_value());
  CHECK(mon.pwm(0).value().duty == 102);
  CHECK(mon.set_pwm_mode(0, PwmMode::Manual));
  CHECK(mon.pwm(0).value().mode == PwmMode::Manual);
  CHECK(mon.set_pwm_duty(0, 200));
  CHECK((m.get(0x00e118) & 0xffff) == 79);
  CHECK(!mon.set_pwm_duty(1, 200));
  CHECK(mon.set_pwm_mode(0, PwmMode::Automatic));
  CHECK(m.get(0x00e118) == (0x80000000u | 40));
  CHECK(mon.pwm(0).value().mode == PwmMode::Automatic);
  CHECK(mon.set_pwm_duty(0, 10));
  mon.shutdown();
  CHECK(m.get(0x00e118) == (0x80000000u | 40));   // the firmware's setting back at shutdown
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Clocks

static void test_clocks() {
  CHECK(crystal_khz_from_strap(0x00000000, Chip()) == 13500);
  CHECK(crystal_khz_from_strap(0x00000040, Chip()) == 14318);
  CHECK(crystal_khz_from_strap(0x00400000, Chip()) == 27000);
  CHECK(crystal_khz_from_strap(0xffffffff, Chip()) == 25000);
  // Kepler: SPPLL0 = 27 MHz * 40 = 1080 MHz; the domain PLL's reference: a VCO through a divider of (4 + 2) / 2 -> 360 MHz;
  // PLL 0x137000: 360 * 5 / 2 = 900 MHz; the core's last divider (bits 13:8 = 2 -> 4) halves it: 450 MHz
  {
    Model m;
    m.regs[0x101000] = 0x00400000;
    m.regs[0x00e800] = 1; m.regs[0x00e804] = (40u << 8) | 1;
    m.regs[0x137120] = 3; m.regs[0x137140] = 0x80000000 | 4;
    m.regs[0x137000] = 1; m.regs[0x137004] = (5u << 8) | 2;
    m.regs[0x137100] = 1;
    m.regs[0x137250] = 0x80000000 | (2u << 8);
    // memory: select 2 -> MPLL 0x132000 (N 2, M 1, P 2 by bit 28) from 0x132020 (N 20 + the fraction 0x1000 -> one more) on the 100 MHz reference
    m.regs[0x1373f4] = 2;
    m.regs[0x132000] = 1; m.regs[0x132004] = 0x10000000 | (2u << 8) | 1;
    m.regs[0x132020] = 1; m.regs[0x132024] = (20u << 8) | 1; m.regs[0x132030] = 0x10000000;
    m.regs[0x137320] = 2;
    NvClk clk(m, chip_of(Family::Kepler, 0xe4));
    CHECK(clk.variant() == NvClk::Variant::Gk104);
    CHECK(clk.read_khz(ClockDomain::Crystal).value() == 27000);
    CHECK(clk.read_khz(ClockDomain::Core).value() == 450000);
    CHECK(clk.read_khz(ClockDomain::Memory).value() == 2100000);
    m.regs[0x137250] = 0;
    CHECK(clk.read_khz(ClockDomain::Core).value() == 900000);
    // the divider path of domain 1 (ROP): source 2, the 100 MHz reference, divided by (6 + 2) / 2
    m.regs[0x137164] = 2; m.regs[0x137254] = 0x80000000 | 6;
    CHECK(clk.read_khz(ClockDomain::Rop).value() == 25000);
    // garbage: M = 0, a PLL that is off, a source that does not exist
    m.regs[0x137004] = (5u << 8);
    CHECK(!clk.read_khz(ClockDomain::Core).has_value());
    m.regs[0x137000] = 0;
    CHECK(!clk.read_khz(ClockDomain::Core).has_value());
    m.regs[0x1373f4] = 7;
    CHECK(!clk.read_khz(ClockDomain::Memory).has_value());
  }
  // Fermi: no fractional N; the memory clock through 0x1373f0 bit 1
  {
    Model m;
    m.regs[0x101000] = 0x00400000;
    m.regs[0x1373f0] = 2;
    m.regs[0x132000] = 1; m.regs[0x132004] = (3u << 16) | (6u << 8) | 1;   // P 3
    m.regs[0x132020] = 1; m.regs[0x132024] = (20u << 8) | 2;
    m.regs[0x137320] = 2;
    NvClk clk(m, chip_of(Family::Fermi, 0xc4));
    CHECK(clk.variant() == NvClk::Variant::Gf100);
    // 100 MHz * 20 / 2 = 1000 MHz; * 6 / 1 / 3 = 2000 MHz
    CHECK(clk.read_khz(ClockDomain::Memory).value() == 2000000);
    m.regs[0x1373f0] = 0;
    m.regs[0x137300] = 0;   // the divider path from the crystal
    CHECK(clk.read_khz(ClockDomain::Memory).value() == 27000);
  }
  // no clock tree here
  {
    Model m;
    NvClk clk(m, chip_of(Family::Pascal, 0x134));
    CHECK(clk.variant() == NvClk::Variant::None);
    CHECK(clk.read_khz(ClockDomain::Crystal).has_value());
    CHECK(!clk.read_khz(ClockDomain::Core).has_value());
  }
  // random registers: every read returns, no division by zero, no overflow (UBSan)
  {
    struct Noise final : INvRegisters {
      uint32_t read32(uint32_t) override { return rnd() << 5 ^ rnd(); }
      void write32(uint32_t, uint32_t) override {}
    } noise;
    for (Family f : {Family::Fermi, Family::Kepler, Family::Maxwell}) {
      NvClk clk(noise, chip_of(f, f == Family::Maxwell ? 0x117 : 0xe4));
      for (int i = 0; i < 20000; ++i) (void)clk.read_khz(static_cast<ClockDomain>(rnd() % 9));
    }
    BoardInfo b = board_with_gpio({gpio_entry(4, gpio_function::kFan, 0, 1), gpio_entry(9, gpio_function::kFanSense, 0, 1)});
    for (Family f : {Family::Tesla, Family::Fermi, Family::Kepler, Family::Maxwell, Family::Pascal}) {
      const Chip chip = chip_of(f, f == Family::Tesla ? 0x84 : f == Family::Fermi ? 0xd9 : 0x117);
      NvGpio g(noise, chip, b);
      NvTherm t(noise, chip, g);
      for (int i = 0; i < 2000; ++i) {
        (void)t.initialize();
        const Optional<int32_t> temp = t.temperature_millicelsius();
        if (temp.has_value()) CHECK(temp.value() >= 0 && temp.value() <= 150000);
        (void)t.fan_rpm();
        (void)t.pwm();
        (void)t.set_pwm_duty(static_cast<uint8_t>(rnd()));
        (void)t.restore_firmware_pwm();
        (void)g.poll_interrupts();
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Several threads on one channel, one bus and one monitor: the mutexes keep the transactions whole.

static void test_threads() {
  Model m;
  AuxEngine a;
  a.base = 0x00d930;
  a.gm200 = true;
  a.edid = make_edid(1);
  m.aux.push_back(&a);
  I2cSlave s;
  s.gf119 = true;
  s.reg = 0x00d014;
  s.edid = make_edid(1);
  m.slaves.push_back(&s);
  NvAuxChannel aux(m, I2cVariant::Gm200, 0);
  NvI2cBus bus(m, I2cVariant::Gm200, 0);
  // (the model itself is not thread-safe: the AUX channel and the bus touch disjoint parts of it, and each is used under its own mutex)
  int aux_bad = 0, bus_bad = 0;
  auto aux_worker = [&] {
    for (int i = 0; i < 200; ++i) {
      uint8_t buf[32] = {};
      if (!aux.dpcd_read(0x40, ByteView(buf, 32))) { ++aux_bad; continue; }
      for (uint32_t j = 0; j < 32; ++j) if (buf[j] != a.dpcd[0x40 + j]) { ++aux_bad; break; }
      if (i % 50 == 0) {
        ByteBuffer edid(g_alloc);
        if (!aux.read_edid(&edid) || edid.size() != 256) ++aux_bad;
      }
    }
  };
  auto bus_worker = [&] {
    for (int i = 0; i < 4; ++i) {
      ByteBuffer edid(g_alloc);
      if (!i2c_edid::read(bus, &edid) || edid.size() != 256 || memcmp(edid.view().data(), s.edid.data(), 256) != 0) ++bus_bad;
    }
  };
  std::thread t1(aux_worker), t2(aux_worker), t3(bus_worker), t4(bus_worker);
  t1.join(); t2.join(); t3.join(); t4.join();
  CHECK(aux_bad == 0);
  CHECK(bus_bad == 0);
}

int main() {
  test_gpio_variants();
  test_gpio_nv50();
  test_gpio_g94_second_bank();
  test_gpio_gf119_family(Family::Fermi, 0xd9, false, 0x00e054, 0x00e050, 0x00e074, 0x00e070);
  test_gpio_gf119_family(Family::Kepler, 0xe4, false, 0x00dc00, 0x00dc08, 0x00dc80, 0x00dc88);
  test_gpio_gf119_family(Family::Ampere, 0x172, true, 0x021640, 0x021648, 0x02164c, 0x021654);
  test_i2c_registers();
  test_i2c_edid(I2cVariant::Nv50, 1, 0x00e150, false, kNoPad);
  test_i2c_edid(I2cVariant::G94, 7, 0x00e780, false, 2);
  test_i2c_edid(I2cVariant::Gf119, 2, 0x00d054, true, 1);
  test_i2c_edid(I2cVariant::Gm200, 5, 0x00d0b4, true, 3);
  test_i2c_faults();
  test_aux_basic(I2cVariant::G94, 1, 0x00e4c0 + 0x50, false);
  test_aux_basic(I2cVariant::Gf119, 0, 0x00e4c0, false);
  test_aux_basic(I2cVariant::Gm200, 2, 0x00d930 + 0xa0, true);
  test_aux_faults(I2cVariant::G94, 0x00e4c0, false);
  test_aux_faults(I2cVariant::Gf119, 0x00e4c0, false);
  test_aux_faults(I2cVariant::Gm200, 0x00d930, true);
  test_connector_edid();
  test_temperature_decoding();
  test_temperature_per_family();
  test_fan_tach();
  test_pwm();
  test_monitor();
  test_clocks();
  test_threads();
  if (g_failed) {
    printf("NvI2cTest: %d checks FAILED\n", g_failed);
    return 1;
  }
  printf("NvI2cTest: all passed\n");
  return 0;
}
