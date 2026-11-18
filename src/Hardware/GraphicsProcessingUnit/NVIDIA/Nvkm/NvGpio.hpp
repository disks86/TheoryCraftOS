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

// The GPU's general purpose I/O lines (nouveau's nvkm/subdev/gpio: base.c, nv50.c, g94.c, gf119.c, gk104.c, ga102.c): reading and driving a
// line, the board's functions on them as the video BIOS's GPIO table lists them (panel power, fan PWM, fan sense, hot plug detect of each
// connector, the voltage ID bits), and the line change interrupt status, read by polling.
//
// Per family (which implementation a chip gets follows nouveau's device table, see variant_for()):
//   Nv50   G80..G92, GT200: four nibbles per register at 0x00e104/0x00e108/0x00e280/0x00e284, 16 lines; interrupts at 0x00e050/0x00e054
//   G94    G94..GT21x, GF100..GF110: the same lines (32 of them), a second interrupt pair at 0x00e070/0x00e074
//   Gf119  GF117/GF119: one register per line at 0x00d610 + 4 * line, interrupts as G94
//   Gk104  Kepler .. Turing (and GA100): lines as Gf119, interrupts at 0x00dc00/0x00dc08/0x00dc80/0x00dc88
//   Ga102  GA102 and later: one register per line at 0x021200 + 4 * line, interrupts at 0x021640..0x021654
//
// A level is "logical": the GPIO table says which electrical level means on (log1) and off (log0); get() and set() work in those terms, as
// nvkm_gpio_get/nvkm_gpio_set do. Each logN value is nouveau's two bits: bit 0 the output level, bit 1 the direction bit (see drive()).
//
// Not thread-safe by itself: the lines are the board's, and each user (display, therm) owns the lines of its functions. The interrupt status
// is read and acknowledged in one place (poll_interrupts) by whoever polls for hot plug.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Utility/Optional.hpp"

namespace hardware {
namespace nv {

// DCB_GPIO_* function numbers (nouveau include/nvkm/subdev/bios/gpio.h, and the hot plug numbers its connector code uses).
namespace gpio_function {
constexpr uint8_t kPanelPower = 0x01, kFan = 0x09, kTvDac0 = 0x0c, kThermExtPowerEvent = 0x10, kTvDac1 = 0x2d, kFanSense = 0x3d,
                  kPowerAlert = 0x4c, kExtPowerLow = 0x79, kLogoLedPwm = 0x84, kUnused = 0xff;
// The fan tachometer as some tables and the brief for this driver name it; nouveau names 0x3d. Both are looked for (0x3d first).
constexpr uint8_t kFanSenseAlt = 0x29;
// The voltage ID bits, least significant first (nouveau subdev/volt/gpio.c tags[]).
constexpr uint8_t kVid[5] = {0x04, 0x05, 0x06, 0x1a, 0x73};
// Hot plug detect of connectors 0..6 (nouveau nouveau_connector.c hpd[] / nvkm/engine/disp: DCB_GPIO_HPD_*).
constexpr uint8_t kHotPlug[7] = {0x07, 0x08, 0x51, 0x52, 0x5e, 0x5f, 0x60};
}  // namespace gpio_function

class NvGpio {
 public:
  enum class Variant : uint8_t { None, Nv50, G94, Gf119, Gk104, Ga102 };
  static Variant variant_for(const Chip& chip);

  NvGpio(INvRegisters& regs, const Chip& chip, const BoardInfo& board);

  Variant variant() const { return variant_; }
  uint32_t line_count() const;

  // nvkm_gpio_drive: `output` true drives the line (nouveau's dir 1), false leaves it to the outside (dir 0); `level` the electrical level.
  [[nodiscard]] bool drive(uint32_t line, bool output, bool level);
  // nvkm_gpio_sense: the electrical level on the line; empty when the line does not exist on this chip.
  [[nodiscard]] Optional<bool> sense(uint32_t line);

  // nvkm_gpio_find: the table entry of `function` (on `line`, or any line with 0xff); nullptr when the board has none.
  const GpioEntry* find(uint8_t function, uint8_t line = 0xff) const;
  // nvkm_gpio_get: whether `function` is on (its line at the level the table calls on). Empty when the board has no such function.
  [[nodiscard]] Optional<bool> get(uint8_t function, uint8_t line = 0xff);
  // nvkm_gpio_set: drives `function` to its logical on or off state (direction and level from the table).
  [[nodiscard]] bool set(uint8_t function, bool on, uint8_t line = 0xff);

  // Hot plug detect: whether something is plugged into the connector whose HPD function is `function` (ConnectorEntry::hpd).
  [[nodiscard]] Optional<bool> hot_plug(uint8_t function) { return get(function); }

  // The line change interrupts (nvkm_gpio intr_stat): one bit per line that went high (rising) or low (falling) while its interrupt was
  // enabled. Reading acknowledges them. Both zero on a chip without the registers.
  struct Changes {
    uint32_t rising = 0, falling = 0;
    bool any() const { return rising != 0 || falling != 0; }
    bool line_changed(uint32_t line) const { return line < 32 && ((rising | falling) >> line & 1); }
  };
  Changes poll_interrupts();
  // nvkm_gpio intr_mask: enables (or disables) the rising and/or falling interrupt of the lines in `lines`.
  void set_interrupts(uint32_t lines, bool rising, bool falling, bool enable);
  // Enables both edges of the line of `function`; false when the board has no such function.
  [[nodiscard]] bool watch(uint8_t function);
  // Whether the line of `function` is among `changes` (false when the board has no such function).
  bool changed(const Changes& changes, uint8_t function) const;

  // The voltage ID the VID lines carry (nouveau volt/gpio.c nvkm_voltgpio_get): bit i is gpio_function::kVid[i]. Empty when the board has
  // none of them. Turning it into a voltage takes the video BIOS's voltage table, which is not read here.
  [[nodiscard]] Optional<uint8_t> voltage_id();

 private:
  INvRegisters& regs_;
  const BoardInfo& board_;
  Variant variant_;
};

}  // namespace nv
}  // namespace hardware
