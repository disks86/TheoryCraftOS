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

// The GPU's I2C buses (nouveau's nvkm/subdev/i2c: bit.c, busnv50.c, busgf119.c, padg94.c, padgf119.c, padgm200.c), bit-banged through the
// bus registers of the display's DDC pads: a transfer of messages with START, repeated START and STOP, the address byte and the acknowledge
// of every byte, the clock stretching of slow slaves (each rise of the clock waits at most kRiseTimeoutUs for the slave to let go), as
// nvkm_i2c_bit_xfer does it.
//
// The register blocks per implementation (see NvI2cPads::variant_for()):
//   Nv50 bus   G80 .. GF110: one register per bus at 0x00e138, 0x00e150, ... (ten of them): bit 0 drives SCL, bit 1 SDA; they read back the lines
//   Gf119 bus  GF117 and later: 0x00d014 + 0x20 * bus: bits 0/1 drive, bits 4/5 read back
// A pad that is shared with a DisplayPort AUX channel (a "hybrid" pad, the DP++ connectors) is switched between I2C and AUX mode first:
// G94 .. GM10x at 0x00e500/0x00e50c + 0x50 * pad, GM200 and later at 0x00d970/0x00d97c + 0x50 * pad.
//
// Thread-safe: a transfer holds the bus's mutex (it sleeps between clock edges, so not a spin lock). The pad is shared with the AUX channel of
// the same connector; NvAuxChannel switches it back and takes its own mutex, so the users of one connector must not interleave a DDC transfer
// and an AUX transfer on it from different threads (the connector's owner, the display code, serializes them).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace nv {

// Which implementation of the I2C subsystem a chip has (nouveau's nv50_i2c, g94_i2c, gf119_i2c / gk104_i2c / gk110_i2c, gm200_i2c).
enum class I2cVariant : uint8_t { None, Nv50, G94, Gf119, Gm200 };
I2cVariant i2c_variant_for(const Chip& chip);

// The hybrid pads (shared by an I2C bus and an AUX channel).
enum class PadMode : uint8_t { Off, I2c, Aux };
// Switches pad `pad` (the AUX channel number it belongs to) to `mode`. False when the chip has no hybrid pads (Nv50) or no such pad.
[[nodiscard]] bool set_pad_mode(INvRegisters& regs, I2cVariant variant, uint8_t pad, PadMode mode);
constexpr uint8_t kNoPad = 0xff;
constexpr uint32_t kMaxHybridPads = 16;   // (the AUX channels nouveau knows of are fewer; this only bounds the register arithmetic)

enum class I2cStatus : uint8_t {
  Ok,
  Nack,        // a byte was not acknowledged (no device at the address, or it refused the data)
  Timeout,     // the clock never came up (a slave stretching it forever, or a short)
  Busy,        // the bus was not idle at the first START and could not be freed
  Invalid,     // no such bus on this chip, or a message the bus cannot carry
};

class NvI2cBus final : public II2cBus {
 public:
  // nouveau bit.c: T_TIMEOUT 2.2 ms per clock rise in steps of T_RISEFALL (1 us), T_HOLD 5 us.
  static constexpr uint32_t kRiseTimeoutUs = 2200, kRiseFallUs = 1, kHoldUs = 5;
  static constexpr uint32_t kNv50Buses = 10, kGf119Buses = 16;   // (Gf119: a bound on the arithmetic; real boards have fewer)

  // `drive`: the bus's number (I2cPort::drive); `pad`: the hybrid pad shared with an AUX channel (I2cPort::share), or kNoPad.
  NvI2cBus(INvRegisters& regs, I2cVariant variant, uint8_t drive, uint8_t pad = kNoPad);

  bool valid() const { return reg_ != 0; }
  // II2cBus: the messages with repeated STARTs between them and a STOP at the end (also after a failure); true when every byte went.
  bool transfer(ArrayView<Message> messages) override;
  // The result of the last transfer.
  I2cStatus last_status() const { return last_; }
  uint32_t bus_register() const { return reg_; }

 private:
  void drive_scl(bool high);
  void drive_sda(bool high);
  bool sense_scl();
  bool sense_sda();
  bool raise_scl();
  I2cStatus start();
  void stop();
  I2cStatus bit_write(bool bit);
  I2cStatus bit_read(bool* bit);
  I2cStatus put_byte(uint8_t byte);
  I2cStatus get_byte(uint8_t* byte, bool last);
  void delay(uint32_t us) { regs_.delay_us(us); }

  INvRegisters& regs_;
  I2cVariant variant_;
  uint8_t pad_;
  uint32_t reg_ = 0;
  uint32_t nv50_data_ = 0x7;   // (Nv50 bus) the value last written: nouveau keeps it rather than reading back
  bool initialized_ = false;
  I2cStatus last_ = I2cStatus::Ok;
  kernel::Mutex mutex_;
};

}  // namespace nv
}  // namespace hardware
