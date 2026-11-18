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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2c.hpp"

// After nouveau nvkm/subdev/i2c bit.c, busnv50.c, busgf119.c, padg94.c, padgf119.c, padgm200.c (MIT license), written from memory of them.

namespace hardware {
namespace nv {

namespace {

// ---- The registers, all in one place (as remembered from nouveau; (?) marks the least certain). ----
// busnv50.c: the bus registers by DCB "drive" number.
constexpr uint32_t kNv50BusRegs[NvI2cBus::kNv50Buses] = {0x00e138, 0x00e150, 0x00e168, 0x00e180, 0x00e254,
                                                         0x00e274, 0x00e764, 0x00e780, 0x00e79c, 0x00e7b8};
constexpr uint32_t kNv50Scl = 0x1, kNv50Sda = 0x2, kNv50Init = 0x7;   // bit 2: (?) nouveau writes 7 at init and keeps it set
// busgf119.c
constexpr uint32_t kGf119BusBase = 0x00d014, kGf119BusStride = 0x20;
constexpr uint32_t kGf119DriveScl = 0x1, kGf119DriveSda = 0x2, kGf119SenseScl = 0x10, kGf119SenseSda = 0x20, kGf119Init = 0x7;
// padg94.c / padgf119.c (the same registers) and padgm200.c: the mode register (bits 15:14 and 1:0) and the power-down bit (bit 0)
constexpr uint32_t kPadStride = 0x50;
constexpr uint32_t kG94PadMode = 0x00e500, kG94PadPower = 0x00e50c;
constexpr uint32_t kGm200PadMode = 0x00d970, kGm200PadPower = 0x00d97c;   // (?)
constexpr uint32_t kPadModeMask = 0x0000c003, kPadModeI2c = 0x0000c001, kPadModeAux = 0x00000002, kPadOff = 0x1;

}  // namespace

I2cVariant i2c_variant_for(const Chip& chip) {
  switch (chip.family) {
    case Family::Tesla:
      return chip.chipset < 0x94 || chip.chipset == 0xa0 ? I2cVariant::Nv50 : I2cVariant::G94;   // (GT200 is the least certain)
    case Family::Fermi:
      return chip.chipset >= 0xd0 ? I2cVariant::Gf119 : I2cVariant::G94;
    case Family::Kepler:
      return I2cVariant::Gf119;
    case Family::Maxwell:
      return chip.chipset < 0x120 ? I2cVariant::Gf119 : I2cVariant::Gm200;   // GM107/GM108 have gk110_i2c (gf119 pads)
    case Family::Pascal: case Family::Volta: case Family::Turing: case Family::Ampere:
    case Family::Hopper: case Family::Ada: case Family::Blackwell:
      return I2cVariant::Gm200;   // (Hopper and later: nouveau only through the GSP; assumed unchanged)
    default:
      return I2cVariant::None;
  }
}

bool set_pad_mode(INvRegisters& regs, I2cVariant variant, uint8_t pad, PadMode mode) {
  if (pad >= kMaxHybridPads) return false;
  uint32_t mode_reg, power_reg;
  switch (variant) {
    case I2cVariant::G94: case I2cVariant::Gf119: mode_reg = kG94PadMode; power_reg = kG94PadPower; break;
    case I2cVariant::Gm200: mode_reg = kGm200PadMode; power_reg = kGm200PadPower; break;
    default: return false;
  }
  const uint32_t base = uint32_t{pad} * kPadStride;
  switch (mode) {
    case PadMode::Off:
      regs.mask(power_reg + base, kPadOff, kPadOff);
      break;
    case PadMode::I2c:
      regs.mask(mode_reg + base, kPadModeMask, kPadModeI2c);
      regs.mask(power_reg + base, kPadOff, 0);
      break;
    case PadMode::Aux:
      regs.mask(mode_reg + base, kPadModeMask, kPadModeAux);
      regs.mask(power_reg + base, kPadOff, 0);
      break;
  }
  return true;
}

NvI2cBus::NvI2cBus(INvRegisters& regs, I2cVariant variant, uint8_t drive, uint8_t pad) : regs_(regs), variant_(variant), pad_(pad) {
  switch (variant) {
    case I2cVariant::Nv50: case I2cVariant::G94:
      if (drive < kNv50Buses) reg_ = kNv50BusRegs[drive];
      break;
    case I2cVariant::Gf119: case I2cVariant::Gm200:
      if (drive < kGf119Buses) reg_ = kGf119BusBase + uint32_t{drive} * kGf119BusStride;
      break;
    default:
      break;
  }
  if (variant == I2cVariant::Nv50) pad_ = kNoPad;   // (no hybrid pads before G94)
}

void NvI2cBus::drive_scl(bool high) {
  if (variant_ == I2cVariant::Nv50 || variant_ == I2cVariant::G94) {
    nv50_data_ = high ? (nv50_data_ | kNv50Scl) : (nv50_data_ & ~kNv50Scl);
    regs_.write32(reg_, nv50_data_);
  } else {
    regs_.mask(reg_, kGf119DriveScl, high ? kGf119DriveScl : 0);
  }
}

void NvI2cBus::drive_sda(bool high) {
  if (variant_ == I2cVariant::Nv50 || variant_ == I2cVariant::G94) {
    nv50_data_ = high ? (nv50_data_ | kNv50Sda) : (nv50_data_ & ~kNv50Sda);
    regs_.write32(reg_, nv50_data_);
  } else {
    regs_.mask(reg_, kGf119DriveSda, high ? kGf119DriveSda : 0);
  }
}

bool NvI2cBus::sense_scl() {
  const uint32_t bit = variant_ == I2cVariant::Nv50 || variant_ == I2cVariant::G94 ? kNv50Scl : kGf119SenseScl;
  return (regs_.read32(reg_) & bit) != 0;
}

bool NvI2cBus::sense_sda() {
  const uint32_t bit = variant_ == I2cVariant::Nv50 || variant_ == I2cVariant::G94 ? kNv50Sda : kGf119SenseSda;
  return (regs_.read32(reg_) & bit) != 0;
}

// nvkm_i2c_raise_scl: lets the clock go and waits for it to come up (a slave may hold it low: clock stretching).
bool NvI2cBus::raise_scl() {
  drive_scl(true);
  for (uint32_t waited = 0; waited < kRiseTimeoutUs; waited += kRiseFallUs) {
    delay(kRiseFallUs);
    if (sense_scl()) return true;
  }
  return false;
}

// i2c_start: a START, or a repeated START (from the clock low after a byte).
I2cStatus NvI2cBus::start() {
  I2cStatus status = I2cStatus::Ok;
  if (!sense_scl() || !sense_sda()) {
    drive_scl(false);
    drive_sda(true);
    if (!raise_scl()) status = I2cStatus::Busy;
  }
  drive_sda(false);
  delay(kHoldUs);
  drive_scl(false);
  delay(kHoldUs);
  return status;
}

void NvI2cBus::stop() {
  drive_scl(false);
  drive_sda(false);
  delay(kRiseFallUs);
  drive_scl(true);
  delay(kHoldUs);
  drive_sda(true);
  delay(kHoldUs);
}

I2cStatus NvI2cBus::bit_write(bool bit) {
  drive_sda(bit);
  delay(kRiseFallUs);
  if (!raise_scl()) return I2cStatus::Timeout;
  delay(kHoldUs);
  drive_scl(false);
  delay(kHoldUs);
  return I2cStatus::Ok;
}

I2cStatus NvI2cBus::bit_read(bool* bit) {
  drive_sda(true);
  delay(kRiseFallUs);
  if (!raise_scl()) return I2cStatus::Timeout;
  delay(kHoldUs);
  *bit = sense_sda();
  drive_scl(false);
  delay(kHoldUs);
  return I2cStatus::Ok;
}

I2cStatus NvI2cBus::put_byte(uint8_t byte) {
  for (int i = 7; i >= 0; --i) {
    const I2cStatus s = bit_write((byte >> i) & 1);
    if (s != I2cStatus::Ok) return s;
  }
  bool nack = true;
  const I2cStatus s = bit_read(&nack);
  if (s != I2cStatus::Ok) return s;
  return nack ? I2cStatus::Nack : I2cStatus::Ok;
}

I2cStatus NvI2cBus::get_byte(uint8_t* byte, bool last) {
  uint8_t value = 0;
  for (int i = 7; i >= 0; --i) {
    bool bit = false;
    const I2cStatus s = bit_read(&bit);
    if (s != I2cStatus::Ok) return s;
    value = static_cast<uint8_t>(value | (bit ? 1u << i : 0u));
  }
  *byte = value;
  return bit_write(last);   // the master's acknowledge (low), or the NACK (high) that ends a read
}

bool NvI2cBus::transfer(ArrayView<Message> messages) {
  kernel::ScopedMutex guard(mutex_);
  if (!valid() || messages.size() == 0) {
    last_ = I2cStatus::Invalid;
    return false;
  }
  for (const Message& m : messages) {
    if (m.address > 0x7f) {
      last_ = I2cStatus::Invalid;
      return false;
    }
  }
  if (pad_ != kNoPad && !set_pad_mode(regs_, variant_, pad_, PadMode::I2c)) {
    last_ = I2cStatus::Invalid;
    return false;
  }
  if (!initialized_) {
    // (nv50_i2c_bus_init / gf119_i2c_bus_init) both lines released
    nv50_data_ = kNv50Init;
    regs_.write32(reg_, variant_ == I2cVariant::Nv50 || variant_ == I2cVariant::G94 ? kNv50Init : kGf119Init);
    initialized_ = true;
  }
  I2cStatus status = I2cStatus::Ok;
  for (const Message& m : messages) {
    status = start();
    if (status == I2cStatus::Ok) status = put_byte(static_cast<uint8_t>(m.address << 1 | (m.read ? 1 : 0)));
    const ByteView data = m.data;
    const uint32_t n = static_cast<uint32_t>(data.size());
    for (uint32_t i = 0; i < n && status == I2cStatus::Ok; ++i) {
      if (m.read) {
        uint8_t byte = 0;
        status = get_byte(&byte, i + 1 == n);
        if (status == I2cStatus::Ok) data[i] = byte;
      } else {
        status = put_byte(data[i]);
      }
    }
    if (status != I2cStatus::Ok) break;
  }
  stop();
  last_ = status;
  return status == I2cStatus::Ok;
}

}  // namespace nv
}  // namespace hardware
