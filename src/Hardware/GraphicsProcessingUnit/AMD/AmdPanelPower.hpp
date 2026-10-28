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

#pragma once

#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"

namespace hardware {

// Whether something is connected to a hot plug pin (the display engine's DC_HPDx_INT_STATUS.DC_HPDx_SENSE).
class IHpdSense {
 public:
  virtual bool hpd_sense(uint32_t pin) = 0;

 protected:
  IHpdSense() = default;
  ~IHpdSense() = default;
};

// The power of an eDP panel, switched through the video BIOS: the AtomBIOS command table UNIPHYTransmitterControl (DIG1TransmitterControl) with the action POWER_ON or POWER_OFF.
// Port of amdgpu_atombios_encoder_set_edp_panel_power (amdgpu_atombios.c / atombios_encoders.c; Copyright Advanced Micro Devices, Inc.; MIT license): the driver powers the panel up
// before the first AUX access (the panel does not answer on the AUX channel without power) and waits for the hot plug pin to show it.
//
// The driver gives the panel 300 ms (polled every millisecond), then logs "Panel power up timed out" and goes on; so does power_on() (it returns false), the caller tries the AUX
// channel anyway. A connector without a hot plug pin has nothing to wait on and does not wait. The table's parameters are DIG_TRANSMITTER_CONTROL_PARAMETERS (8 bytes): usPixelClock,
// ucConfig and ucAction (byte 3) and four reserved bytes, only the action is set.
class AmdPanelPower {
 public:
  static constexpr uint32_t kCommandTable = 76;     // GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl) = DIG1TransmitterControl
  static constexpr uint8_t kActionPowerOn = 12, kActionPowerOff = 13;   // ATOM_TRANSMITTER_ACTION_POWER_ON, _OFF
  static constexpr uint32_t kSenseTries = 300;      // one millisecond apart
  static constexpr uint8_t kNoHpd = 0xFF;

  // The table with the action: false if the interpreter failed.
  static bool run(AtomBios& bios, uint8_t action);
  // Power the panel up and wait for its hot plug pin (hpd, 0..5, or kNoHpd) to sense it. False if the table failed or the pin did not show the panel within kSenseTries milliseconds.
  static bool power_on(IAtomHost& host, AtomBios& bios, IHpdSense& sense, uint8_t hpd);
  static bool power_off(AtomBios& bios) { return run(bios, kActionPowerOff); }
};

}  // namespace hardware
