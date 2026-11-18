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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvGpio.hpp"

// After nouveau nvkm/subdev/gpio (MIT license), written from memory of it: no register here was checked against the source or hardware.

namespace hardware {
namespace nv {

namespace {

// ---- The registers, all in one place. Every value is as remembered from nouveau; the ones marked (?) are the least certain. ----
// Nv50 / G94: four bits per line, eight lines per register: bit 0 the output level, bit 1 the "not driven" (input) bit, bit 2 the sensed level.
constexpr uint32_t kNv50LineRegs[4] = {0x00e104, 0x00e108, 0x00e280, 0x00e284};
constexpr uint32_t kNv50Sense = 0x4;
// Gf119 / Gk104: one register per line; bit 12 the output level, bit 13 "not driven", bit 14 the sensed level.
constexpr uint32_t kGf119LineBase = 0x00d610;
constexpr uint32_t kGf119Update = 0x00d604;          // bit 0 written after a change ("update?" in nouveau too)
// Ga102: the same layout as Gf119, moved (?), and nouveau still writes the Gf119 update register (?).
constexpr uint32_t kGa102LineBase = 0x021200;
constexpr uint32_t kGf119Out = 1u << 12, kGf119Input = 1u << 13, kGf119SenseBit = 1u << 14;
// The interrupt registers: status (write 1 to clear) and enable, for lines 0..15 (bits 15:0 high, 31:16 low) and 16..31.
struct IntrRegs {
  uint32_t stat0, en0, stat1, en1;   // stat1 == 0: only the first pair (Nv50)
};
constexpr IntrRegs kNv50Intr = {0x00e054, 0x00e050, 0, 0};
constexpr IntrRegs kG94Intr = {0x00e054, 0x00e050, 0x00e074, 0x00e070};
constexpr IntrRegs kGk104Intr = {0x00dc00, 0x00dc08, 0x00dc80, 0x00dc88};
constexpr IntrRegs kGa102Intr = {0x021640, 0x021648, 0x02164c, 0x021654};   // (?)

const IntrRegs* intr_regs(NvGpio::Variant v) {
  switch (v) {
    case NvGpio::Variant::Nv50: return &kNv50Intr;
    case NvGpio::Variant::G94: case NvGpio::Variant::Gf119: return &kG94Intr;
    case NvGpio::Variant::Gk104: return &kGk104Intr;
    case NvGpio::Variant::Ga102: return &kGa102Intr;
    default: return nullptr;
  }
}

}  // namespace

NvGpio::Variant NvGpio::variant_for(const Chip& chip) {
  switch (chip.family) {
    case Family::Tesla:
      // nouveau: G80, G84, G86, G92 and GT200 (0xa0) have nv50_gpio, G94 and later g94_gpio (GT200's is the least certain)
      return chip.chipset < 0x94 || chip.chipset == 0xa0 ? Variant::Nv50 : Variant::G94;
    case Family::Fermi:
      return chip.chipset >= 0xd0 ? Variant::Gf119 : Variant::G94;   // GF117/GF119 vs GF100..GF110
    case Family::Kepler: case Family::Maxwell: case Family::Pascal: case Family::Volta: case Family::Turing:
      return Variant::Gk104;
    case Family::Ampere:
      return chip.chipset == 0x170 ? Variant::Gk104 : Variant::Ga102;   // GA100 kept gk104_gpio
    case Family::Hopper: case Family::Ada: case Family::Blackwell:
      return Variant::Ga102;   // (?) nouveau drives these through the GSP; the registers are assumed to be GA102's
    default:
      return Variant::None;
  }
}

NvGpio::NvGpio(INvRegisters& regs, const Chip& chip, const BoardInfo& board) : regs_(regs), board_(board), variant_(variant_for(chip)) {}

uint32_t NvGpio::line_count() const {
  switch (variant_) {
    case Variant::None: return 0;
    case Variant::Nv50: return 16;
    default: return 32;
  }
}

bool NvGpio::drive(uint32_t line, bool output, bool level) {
  if (line >= line_count()) return false;
  const uint32_t not_driven = output ? 0 : 1, out = level ? 1 : 0;
  switch (variant_) {
    case Variant::Nv50: case Variant::G94: {
      const uint32_t reg = kNv50LineRegs[line >> 3], shift = (line & 7) << 2;
      regs_.mask(reg, 3u << shift, ((not_driven << 1) | out) << shift);
      return true;
    }
    case Variant::Gf119: case Variant::Gk104: case Variant::Ga102: {
      const uint32_t base = variant_ == Variant::Ga102 ? kGa102LineBase : kGf119LineBase;
      regs_.mask(base + line * 4, kGf119Out | kGf119Input, (not_driven ? kGf119Input : 0) | (out ? kGf119Out : 0));
      regs_.mask(kGf119Update, 1, 1);
      return true;
    }
    default:
      return false;
  }
}

Optional<bool> NvGpio::sense(uint32_t line) {
  if (line >= line_count()) return {};
  switch (variant_) {
    case Variant::Nv50: case Variant::G94: {
      const uint32_t reg = kNv50LineRegs[line >> 3], shift = (line & 7) << 2;
      return (regs_.read32(reg) & (kNv50Sense << shift)) != 0;
    }
    case Variant::Gf119: case Variant::Gk104: case Variant::Ga102: {
      const uint32_t base = variant_ == Variant::Ga102 ? kGa102LineBase : kGf119LineBase;
      return (regs_.read32(base + line * 4) & kGf119SenseBit) != 0;
    }
    default:
      return {};
  }
}

const GpioEntry* NvGpio::find(uint8_t function, uint8_t line) const {
  if (function == gpio_function::kUnused) return nullptr;
  const uint32_t count = board_.gpio_count < BoardInfo::kMaxGpio ? board_.gpio_count : BoardInfo::kMaxGpio;
  for (uint32_t i = 0; i < count; ++i) {
    const GpioEntry& e = board_.gpio[i];
    if (e.function == function && (line == 0xff || e.line == line) && e.line != 0xff) return &e;
  }
  return nullptr;
}

Optional<bool> NvGpio::get(uint8_t function, uint8_t line) {
  const GpioEntry* e = find(function, line);
  if (!e) return {};
  const Optional<bool> level = sense(e->line);
  if (!level.has_value()) return {};
  return level.value() == ((e->log1 & 1) != 0);
}

bool NvGpio::set(uint8_t function, bool on, uint8_t line) {
  const GpioEntry* e = find(function, line);
  if (!e) return false;
  const uint8_t log = on ? e->log1 : e->log0;
  return drive(e->line, (log & 2) != 0, (log & 1) != 0);
}

NvGpio::Changes NvGpio::poll_interrupts() {
  Changes c;
  const IntrRegs* r = intr_regs(variant_);
  if (!r) return c;
  const uint32_t intr0 = regs_.read32(r->stat0);
  const uint32_t stat0 = regs_.read32(r->en0) & intr0;
  c.rising = stat0 & 0xffff;
  c.falling = stat0 >> 16;
  regs_.write32(r->stat0, intr0);
  if (r->stat1) {
    const uint32_t intr1 = regs_.read32(r->stat1);
    const uint32_t stat1 = regs_.read32(r->en1) & intr1;
    c.rising |= stat1 << 16;
    c.falling |= stat1 & 0xffff0000u;
    regs_.write32(r->stat1, intr1);
  }
  if (line_count() < 32) {
    const uint32_t lines = (1u << line_count()) - 1;
    c.rising &= lines;
    c.falling &= lines;
  }
  return c;
}

void NvGpio::set_interrupts(uint32_t lines, bool rising, bool falling, bool enable) {
  const IntrRegs* r = intr_regs(variant_);
  if (!r) return;
  if (line_count() < 32) lines &= (1u << line_count()) - 1;
  const uint32_t data = enable ? lines : 0;
  uint32_t inte0 = regs_.read32(r->en0);
  if (falling) inte0 = (inte0 & ~(lines << 16)) | (data << 16);
  if (rising) inte0 = (inte0 & ~(lines & 0xffff)) | (data & 0xffff);
  regs_.write32(r->en0, inte0);
  if (r->stat1) {
    const uint32_t hi_lines = lines >> 16, hi_data = data >> 16;
    uint32_t inte1 = regs_.read32(r->en1);
    if (falling) inte1 = (inte1 & ~(hi_lines << 16)) | (hi_data << 16);
    if (rising) inte1 = (inte1 & ~hi_lines) | hi_data;
    regs_.write32(r->en1, inte1);
  }
}

bool NvGpio::watch(uint8_t function) {
  const GpioEntry* e = find(function);
  if (!e || e->line >= line_count()) return false;
  set_interrupts(1u << e->line, true, true, true);
  return true;
}

bool NvGpio::changed(const Changes& changes, uint8_t function) const {
  const GpioEntry* e = find(function);
  return e && changes.line_changed(e->line);
}

Optional<uint8_t> NvGpio::voltage_id() {
  uint8_t vid = 0;
  bool any = false;
  for (uint32_t i = 0; i < sizeof gpio_function::kVid; ++i) {
    const Optional<bool> bit = get(gpio_function::kVid[i]);
    if (!bit.has_value()) continue;
    any = true;
    if (bit.value()) vid = static_cast<uint8_t>(vid | (1u << i));
  }
  if (!any) return {};
  return vid;
}

}  // namespace nv
}  // namespace hardware
