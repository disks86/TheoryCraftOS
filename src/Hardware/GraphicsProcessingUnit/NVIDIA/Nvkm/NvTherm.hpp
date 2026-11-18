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

// The GPU's thermal sensor and fan (nouveau's nvkm/subdev/therm: nv50.c, g84.c, gt215.c, gf119.c, gm107.c, gp100.c, fan.c, fanpwm.c):
//   temperature  Nv50 (G80): the raw sensor at 0x020014, calibrated with the video BIOS's sensor constants (ThermSensorCalibration)
//                G84 .. GM2xx: whole degrees at 0x020400, valid when the fuse at 0x1a8 says the sensor is calibrated
//                GP100 and later: 0x020460, 1/32 degree steps in bits 16:3 with a valid bit (29)
//   fan speed    GT215 .. GM2xx: the tachometer counter (0x00e720 control, 0x00e724 the gate in crystal ticks, 0x00e728 the count), which
//                counts the fan-sense line's pulses over one second; otherwise the fan-sense GPIO is timed (four changes per turn) on PTIMER
//   fan PWM      Tesla .. GF110: the GPIO PWM units at 0x00e114/0x00e118 (divider, duty), enabled per line at 0x00e100/0x00e28c
//                GF119 / Kepler: the same units chosen by the line's GPIO function (0x00d610 + 4 * line), or the PTHERM one at 0x0200d8/dc
//                GM107 .. GM2xx: 0x10eb10/0x10eb14 + 4 * line, always routed
//                Pascal and later: nouveau has no fan control (the PMU firmware owns it); none here either
//
// "Automatic" restores the divider, duty and routing the firmware left when initialize() ran (the VBIOS's boot setting, or the hardware
// PTHERM controller where that drives the fan): this driver has no fan curve of its own. Every register value is untrusted: a reading that
// is out of range or flagged invalid is an empty Optional.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvGpio.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/Optional.hpp"

namespace hardware {
namespace nv {

// nvbios_therm_sensor: the G80 sensor's calibration, from the video BIOS's thermal table (not part of BoardInfo). All zero: not calibrated.
struct ThermSensorCalibration {
  int32_t slope_mult = 0, slope_div = 0, offset_num = 0, offset_den = 0, offset_constant = 0;
};

// The decoders, separate so that they can be tested on any value.
namespace therm_decode {
constexpr int32_t kMaxPlausibleCelsius = 150;   // above this a reading is taken as garbage (the GPUs shut down near 105)
[[nodiscard]] Optional<int32_t> nv50_millicelsius(uint32_t raw, const ThermSensorCalibration& cal);
[[nodiscard]] Optional<int32_t> g84_millicelsius(uint32_t raw);
[[nodiscard]] Optional<int32_t> gp100_millicelsius(uint32_t raw);
// gt215_therm_fan_sense: the count is pulses per second, two per turn.
[[nodiscard]] Optional<uint32_t> gt215_tach_rpm(uint32_t control, uint32_t count);
// fanpwm.c: the duty (0..255) a divider and duty register pair mean; `inverted` when the fan GPIO's off level is high (log0 bit 0).
[[nodiscard]] Optional<uint8_t> pwm_duty_255(uint32_t divs, uint32_t duty, bool inverted);
// The duty register value for `duty255` of `divs`.
uint32_t pwm_duty_register(uint32_t divs, uint8_t duty255, bool inverted);
}  // namespace therm_decode

class NvTherm {
 public:
  enum class Sensor : uint8_t { None, Nv50, G84, Gp100 };
  enum class Pwm : uint8_t { None, Nv50, Gf119, Gm107 };
  enum class Tach : uint8_t { None, Gt215, Gf119 };
  static Sensor sensor_for(const Chip& chip);
  static Pwm pwm_for(const Chip& chip);
  static Tach tach_for(const Chip& chip);

  // PTIMER, for timing the fan-sense line: nanoseconds, low word at 0x009400, high at 0x009410.
  static constexpr uint32_t kTimerLow = 0x009400, kTimerHigh = 0x009410;
  static constexpr uint32_t kTachPollUs = 500, kTachMaxPolls = 500, kTachWindowNs = 250000000;

  NvTherm(INvRegisters& regs, const Chip& chip, NvGpio& gpio, const ThermSensorCalibration& calibration = {});

  // Sets the sensor up (nv50/g84_sensor_setup), starts the tachometer counter (gt215/gf119_therm_init) and remembers the PWM as the
  // firmware left it. True when there is a sensor, a fan sense or a fan PWM.
  [[nodiscard]] bool initialize();

  bool has_temperature() const { return sensor_ != Sensor::None; }
  bool has_fan_sense() const { return fan_sense_ != nullptr; }
  bool has_pwm() const { return pwm_unit_ >= 0; }

  [[nodiscard]] Optional<int32_t> temperature_millicelsius();
  // RPM, 0 when the fan stands still. (The counter needs a second after initialize() before it has counted anything.)
  [[nodiscard]] Optional<uint32_t> fan_rpm();
  // The duty (0..255) now, and whether this driver set it (Manual) or the firmware's setting is in force (Automatic).
  [[nodiscard]] Optional<PwmState> pwm();
  // Manual mode at `duty255` with the divider (the PWM frequency) the firmware chose. False without a PWM or a known divider.
  [[nodiscard]] bool set_pwm_duty(uint8_t duty255);
  // Back to what initialize() found. False without a PWM or when nothing was saved.
  [[nodiscard]] bool restore_firmware_pwm();

  // The fuse at `offset` (nvkm_fuse_read); 0 on chips without the block.
  uint32_t read_fuse(uint32_t offset);

 private:
  struct PwmRegs {
    uint32_t divs = 0, duty = 0;
    uint32_t route = 0, route_mask = 0, route_on = 0;   // the enable/routing bits (0 mask: hardwired)
    uint32_t commit = 0x80000000;                       // ORed into a duty write
    uint32_t field_mask = 0xffffffff;                    // the bits of divs/duty that hold the value
  };
  bool pwm_regs(PwmRegs* out, int32_t* unit_out = nullptr);
  bool read_pwm_raw(uint32_t* divs, uint32_t* duty, bool* routed);
  uint64_t timer_ns();
  Optional<uint32_t> gpio_tach_rpm();

  INvRegisters& regs_;
  Chip chip_;
  NvGpio& gpio_;
  ThermSensorCalibration cal_;
  Sensor sensor_;
  Pwm pwm_kind_;
  Tach tach_;
  const GpioEntry* fan_ = nullptr;
  const GpioEntry* fan_sense_ = nullptr;
  int32_t pwm_unit_ = -1;          // nouveau's pwm_info index (Nv50: 0/1, Gf119: 0/1/2, Gm107: 0); -1 no PWM
  bool saved_ = false;
  uint32_t saved_divs_ = 0, saved_duty_ = 0, saved_route_ = 0;
  bool manual_ = false;
};

// The sensor and fan as a hardware monitor (one temperature "GPU", one fan "GPU fan", one PWM output), for SystemHardwareMonitor.
// Thread-safe: a mutex around every access to the therm state.
class NvThermMonitor final : public IHardwareMonitor {
 public:
  NvThermMonitor(INvRegisters& regs, const Chip& chip, NvGpio& gpio, const ThermSensorCalibration& calibration = {})
      : therm_(regs, chip, gpio, calibration) {}

  const char* name() const override { return "NVIDIA GPU sensors"; }
  bool initialize() override;
  void shutdown() override;

  uint32_t channel_count(MonitorChannel kind) const override;
  const char* label(MonitorChannel kind, uint32_t channel) const override;
  [[nodiscard]] Optional<int32_t> temperature_millicelsius(uint32_t channel) override;
  [[nodiscard]] Optional<uint32_t> fan_rpm(uint32_t channel) override;
  [[nodiscard]] Optional<int32_t> voltage_millivolts(uint32_t channel) override { (void)channel; return {}; }
  [[nodiscard]] Optional<PwmState> pwm(uint32_t channel) override;
  [[nodiscard]] bool set_pwm_duty(uint32_t channel, uint8_t duty) override;
  [[nodiscard]] bool set_pwm_mode(uint32_t channel, PwmMode mode) override;

  NvTherm& therm() { return therm_; }

 private:
  NvTherm therm_;
  bool initialized_ = false;
  kernel::Mutex mutex_;
};

}  // namespace nv
}  // namespace hardware
