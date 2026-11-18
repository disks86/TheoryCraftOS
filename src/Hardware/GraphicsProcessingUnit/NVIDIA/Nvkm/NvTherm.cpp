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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvTherm.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvClk.hpp"

// After nouveau nvkm/subdev/therm and nvkm/subdev/fuse (MIT license), written from memory of them; nothing here was checked on hardware.

namespace hardware {
namespace nv {

namespace {

// ---- The registers, all in one place ((?) marks the least certain). ----
// sensors
constexpr uint32_t kNv50SensorRaw = 0x020014, kNv50SensorCtl = 0x020010;              // raw bits 13:0; ctl bit 30 cleared to run it
constexpr uint32_t kG84Temp = 0x020400;                                                // whole degrees
constexpr uint32_t kG84SensorCtl0 = 0x020008, kG84SensorCtl1 = 0x02000c;               // (?) g84_sensor_setup's "insane defaults" fix
constexpr uint32_t kGp100Temp = 0x020460;                                              // bits 16:8 degrees, 7:3 1/32 degrees; bit 29 valid, 30 shadowed
constexpr uint32_t kGp100Valid = 0x20000000, kGp100TempMask = 0x0001fff8;
constexpr uint32_t kSensorFuse = 0x1a8;                                                // 1: the sensor is calibrated (G84 .. GM2xx)
// fuses (nvkm/subdev/fuse): Tesla 0x021000 + offset with 0x001084 bit 11 set around it; Fermi/Kepler 0x021100 + offset with 0x022400 bit 11
// and 0x021000 bit 0 set around it; Maxwell and later read 0x021100 + offset directly
constexpr uint32_t kFuseNv50Base = 0x021000, kFuseNv50Enable = 0x001084;
constexpr uint32_t kFuseGf100Base = 0x021100, kFuseGf100Enable = 0x022400, kFuseGf100Unk = 0x021000;
// tachometer (gt215.c / gf119.c)
constexpr uint32_t kTachCtl = 0x00e720, kTachGate = 0x00e724, kTachCount = 0x00e728;   // ctl bit 0 counting, bit 1 reset; gt215: line in 20:16
constexpr uint32_t kGf119TachLine = 0x00d79c;                                          // (?) gf119: the line routed to the counter, bits 7:0
// PWM
constexpr uint32_t kNv50PwmCtl = 0x00e100, kNv50PwmCtl2 = 0x00e28c;                    // enable bit per line (bit 16+n: (?) cleared on enable)
constexpr uint32_t kGpioPwmDivs = 0x00e114, kGpioPwmDuty = 0x00e118, kGpioPwmStride = 8;
constexpr uint32_t kGf119GpioBase = 0x00d610;                                          // the line's GPIO function: bits 7:6 mode, 4:0 function
constexpr uint32_t kPthermPwmDivs = 0x0200d8, kPthermPwmDuty = 0x0200dc;               // gf119 "indx 2", 13 bits, commit bit 30
constexpr uint32_t kGm107PwmDivs = 0x10eb10, kGm107PwmDuty = 0x10eb14;                 // (?) + 4 * line (nouveau reads 0x10eb20/24, the same for line 4)

int64_t div_trunc(int64_t a, int64_t b) { return b == 0 ? 0 : a / b; }

}  // namespace

namespace therm_decode {

Optional<int32_t> nv50_millicelsius(uint32_t raw, const ThermSensorCalibration& cal) {
  if (!cal.slope_div || !cal.slope_mult || !cal.offset_num || !cal.offset_den) return {};
  // nv50_temp_get, in thousandths: raw * mult / div + num / den + constant - 8
  const int64_t value = int64_t{raw & 0x3fff};
  int64_t milli = div_trunc(value * cal.slope_mult * 1000, cal.slope_div);
  milli += div_trunc(int64_t{cal.offset_num} * 1000, cal.offset_den);
  milli += (int64_t{cal.offset_constant} - 8) * 1000;
  if (milli < 0) milli = 0;   // (nouveau: negative temperatures are reserved for errors)
  if (milli > int64_t{kMaxPlausibleCelsius} * 1000) return {};
  return static_cast<int32_t>(milli);
}

Optional<int32_t> g84_millicelsius(uint32_t raw) {
  if (raw > static_cast<uint32_t>(kMaxPlausibleCelsius)) return {};
  return static_cast<int32_t>(raw) * 1000;
}

Optional<int32_t> gp100_millicelsius(uint32_t raw) {
  if (!(raw & kGp100Valid)) return {};
  // bits 16:8 whole degrees, bits 7:3 thirty-seconds (nouveau keeps only the whole degrees: inttemp >> 8)
  const uint32_t fixed = raw & kGp100TempMask;
  const uint32_t milli = static_cast<uint32_t>(uint64_t{fixed} * 1000 / 256);
  if (milli > static_cast<uint32_t>(kMaxPlausibleCelsius) * 1000) return {};
  return static_cast<int32_t>(milli);
}

Optional<uint32_t> gt215_tach_rpm(uint32_t control, uint32_t count) {
  if (!(control & 1)) return {};
  return (count & 0xffff) * 60 / 2;
}

Optional<uint8_t> pwm_duty_255(uint32_t divs, uint32_t duty, bool inverted) {
  if (divs == 0) return {};
  if (duty > divs) divs = duty;   // (nouveau: divs = max(divs, duty))
  if (inverted) duty = divs - duty;
  return static_cast<uint8_t>(uint64_t{duty} * 255 / divs);
}

uint32_t pwm_duty_register(uint32_t divs, uint8_t duty255, bool inverted) {
  uint32_t duty = static_cast<uint32_t>((uint64_t{divs} * duty255 + 254) / 255);
  if (duty > divs) duty = divs;
  return inverted ? divs - duty : duty;
}

}  // namespace therm_decode

NvTherm::Sensor NvTherm::sensor_for(const Chip& chip) {
  switch (chip.family) {
    case Family::Tesla: return chip.chipset == 0x50 ? Sensor::Nv50 : Sensor::G84;
    case Family::Fermi: case Family::Kepler: case Family::Maxwell: return Sensor::G84;
    case Family::Pascal: case Family::Volta: case Family::Turing: return Sensor::Gp100;
    case Family::Ampere: case Family::Hopper: case Family::Ada: case Family::Blackwell: return Sensor::Gp100;   // (?) only through the GSP in nouveau
    default: return Sensor::None;
  }
}

NvTherm::Pwm NvTherm::pwm_for(const Chip& chip) {
  switch (chip.family) {
    case Family::Tesla: return Pwm::Nv50;
    case Family::Fermi: return chip.chipset >= 0xd0 ? Pwm::Gf119 : Pwm::Nv50;   // GF100..GF110 run gt215_therm
    case Family::Kepler: return Pwm::Gf119;
    case Family::Maxwell: return Pwm::Gm107;
    default: return Pwm::None;
  }
}

NvTherm::Tach NvTherm::tach_for(const Chip& chip) {
  switch (chip.family) {
    case Family::Tesla: return chip.chipset >= 0xa3 ? Tach::Gt215 : Tach::None;
    case Family::Fermi: return chip.chipset >= 0xd0 ? Tach::Gf119 : Tach::Gt215;
    case Family::Kepler: case Family::Maxwell: return Tach::Gf119;
    default: return Tach::None;
  }
}

NvTherm::NvTherm(INvRegisters& regs, const Chip& chip, NvGpio& gpio, const ThermSensorCalibration& calibration)
    : regs_(regs), chip_(chip), gpio_(gpio), cal_(calibration), sensor_(sensor_for(chip)), pwm_kind_(pwm_for(chip)), tach_(tach_for(chip)) {
  fan_ = gpio_.find(gpio_function::kFan);
  fan_sense_ = gpio_.find(gpio_function::kFanSense);
  if (!fan_sense_) fan_sense_ = gpio_.find(gpio_function::kFanSenseAlt);
}

uint32_t NvTherm::read_fuse(uint32_t offset) {
  if (offset > 0xffc) return 0;
  switch (chip_.family) {
    case Family::Tesla: {
      const uint32_t enable = regs_.mask(kFuseNv50Enable, 0x800, 0x800);
      const uint32_t v = regs_.read32(kFuseNv50Base + offset);
      regs_.write32(kFuseNv50Enable, enable);
      return v;
    }
    case Family::Fermi: case Family::Kepler: {
      const uint32_t enable = regs_.mask(kFuseGf100Enable, 0x800, 0x800);
      const uint32_t unk = regs_.mask(kFuseGf100Unk, 0x1, 0x1);
      const uint32_t v = regs_.read32(kFuseGf100Base + offset);
      regs_.write32(kFuseGf100Unk, unk);
      regs_.write32(kFuseGf100Enable, enable);
      return v;
    }
    case Family::Unknown: case Family::Nv04: case Family::Nv10: case Family::Nv20: case Family::Nv30: case Family::Nv40:
      return 0;
    default:
      return regs_.read32(kFuseGf100Base + offset);
  }
}

bool NvTherm::pwm_regs(PwmRegs* out, int32_t* unit_out) {
  if (!fan_ || fan_->line >= 32) return false;
  const uint32_t line = fan_->line;
  PwmRegs r;
  int32_t found_unit = -1;
  switch (pwm_kind_) {
    case Pwm::Nv50: {
      // nv50.c pwm_info: only lines 4, 9 and 16 have a PWM unit
      uint32_t ctl, bit, unit;
      if (line == 0x04) { ctl = kNv50PwmCtl; bit = 4; unit = 0; }
      else if (line == 0x09) { ctl = kNv50PwmCtl; bit = 9; unit = 1; }
      else if (line == 0x10) { ctl = kNv50PwmCtl2; bit = 0; unit = 0; }
      else return false;
      r.divs = kGpioPwmDivs + unit * kGpioPwmStride;
      r.duty = kGpioPwmDuty + unit * kGpioPwmStride;
      r.route = ctl;
      r.route_mask = 0x00010001u << bit;
      r.route_on = 1u << bit;
      found_unit = static_cast<int32_t>(unit);
      break;
    }
    case Pwm::Gf119: {
      // gf119.c pwm_info: the line's GPIO function says which unit drives it
      const uint32_t gpio = regs_.read32(kGf119GpioBase + line * 4);
      int32_t unit = -1;
      if ((gpio & 0xc0) == 0x00 || (gpio & 0xc0) == 0x40) {
        switch (gpio & 0x1f) {
          case 0x00: unit = 2; break;
          case 0x19: unit = 1; break;
          case 0x1c: unit = 0; break;
          case 0x1e: unit = 2; break;
          default: break;
        }
      }
      if (unit < 0) return false;
      if (unit < 2) {
        r.divs = kGpioPwmDivs + static_cast<uint32_t>(unit) * kGpioPwmStride;
        r.duty = kGpioPwmDuty + static_cast<uint32_t>(unit) * kGpioPwmStride;
        r.route = kGf119GpioBase + line * 4;
        r.route_mask = 0xc0;
        r.route_on = 0x40;
      } else {
        r.divs = kPthermPwmDivs;
        r.duty = kPthermPwmDuty;
        r.commit = 0x40000000;
        r.field_mask = 0x1fff;
      }
      found_unit = unit;
      break;
    }
    case Pwm::Gm107:
      r.divs = kGm107PwmDivs + line * 4;
      r.duty = kGm107PwmDuty + line * 4;
      r.field_mask = 0x1fff;
      found_unit = 0;
      break;
    default:
      return false;
  }
  *out = r;
  if (unit_out) *unit_out = found_unit;
  return true;
}

bool NvTherm::read_pwm_raw(uint32_t* divs, uint32_t* duty, bool* routed) {
  PwmRegs r;
  if (!pwm_regs(&r)) return false;
  *divs = regs_.read32(r.divs) & r.field_mask & 0x7fffffff;
  *duty = regs_.read32(r.duty) & r.field_mask & 0x3fffffff;   // (the commit bits are not part of the value)
  *routed = r.route_mask == 0 || (regs_.read32(r.route) & r.route_on) != 0;
  return true;
}

bool NvTherm::initialize() {
  if (sensor_ == Sensor::Nv50) {
    regs_.mask(kNv50SensorCtl, 0x40000000, 0);
    regs_.delay_us(20000);   // (nouveau: mdelay(20) for the reading to settle)
  } else if (sensor_ == Sensor::G84 && read_fuse(kSensorFuse) == 1) {
    regs_.mask(kG84SensorCtl0, 0x80008000, 0x80000000);
    regs_.mask(kG84SensorCtl1, 0x80000003, 0);
    regs_.delay_us(20000);
  }
  if (tach_ != Tach::None) {
    // count revolutions per second: reset, route the fan-sense line, the gate is one second of crystal ticks, then count
    regs_.mask(kTachCtl, 0x3, 0x2);
    if (fan_sense_ && fan_sense_->line < 32) {
      if (tach_ == Tach::Gf119) regs_.mask(kGf119TachLine, 0xff, fan_sense_->line);
      regs_.write32(kTachGate, crystal_khz_from_strap(regs_.read32(kStrapRegister), chip_) * 1000);
      if (tach_ == Tach::Gt215) regs_.mask(kTachCtl, 0x001f0000, uint32_t{fan_sense_->line} << 16);
      regs_.mask(kTachCtl, 0x1, 0x1);
    }
    regs_.mask(kTachCtl, 0x2, 0);
  }
  uint32_t divs = 0, duty = 0;
  bool routed = false;
  pwm_unit_ = -1;
  saved_ = false;
  if (read_pwm_raw(&divs, &duty, &routed)) {
    PwmRegs r;
    if (pwm_regs(&r, &pwm_unit_)) {
      saved_divs_ = divs;
      saved_duty_ = duty;
      saved_route_ = r.route_mask ? regs_.read32(r.route) & r.route_mask : 0;
      saved_ = true;
    }
  }
  manual_ = false;
  return has_temperature() || has_fan_sense() || has_pwm();
}

Optional<int32_t> NvTherm::temperature_millicelsius() {
  switch (sensor_) {
    case Sensor::Nv50: return therm_decode::nv50_millicelsius(regs_.read32(kNv50SensorRaw), cal_);
    case Sensor::G84:
      if (read_fuse(kSensorFuse) != 1) return {};
      return therm_decode::g84_millicelsius(regs_.read32(kG84Temp));
    case Sensor::Gp100: return therm_decode::gp100_millicelsius(regs_.read32(kGp100Temp));
    default: return {};
  }
}

uint64_t NvTherm::timer_ns() {
  // nv04_timer_read: the high word read again until it did not change under the low one (bounded)
  uint32_t hi = 0, lo = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    hi = regs_.read32(kTimerHigh);
    lo = regs_.read32(kTimerLow);
    if (hi == regs_.read32(kTimerHigh)) break;
  }
  return uint64_t{hi} << 32 | lo;
}

// nvkm_therm_fan_sense: four changes of the fan-sense line per turn; timed from the first change to the fifth
Optional<uint32_t> NvTherm::gpio_tach_rpm() {
  Optional<bool> prev = gpio_.get(fan_sense_->function, fan_sense_->line);
  if (!prev.has_value()) return {};
  const uint64_t begin = timer_ns();
  uint64_t start = 0, end = 0;
  uint32_t changes = 0;
  for (uint32_t poll = 0; poll < kTachMaxPolls && changes < 5; ++poll) {
    regs_.delay_us(kTachPollUs);
    const Optional<bool> cur = gpio_.get(fan_sense_->function, fan_sense_->line);
    if (!cur.has_value()) return {};
    const uint64_t now = timer_ns();
    if (cur.value() != prev.value()) {
      if (changes == 0) start = now;
      ++changes;
      end = now;
      prev = cur;
    }
    if (now - begin >= kTachWindowNs) break;
  }
  if (changes < 5) return 0u;   // standing still (or slower than the window shows)
  if (end <= start) return {};  // a timer that does not move
  const uint64_t rpm = 60000000000ull / (end - start);
  if (rpm > 100000) return {};
  return static_cast<uint32_t>(rpm);
}

Optional<uint32_t> NvTherm::fan_rpm() {
  if (!fan_sense_) return {};
  if (tach_ != Tach::None) return therm_decode::gt215_tach_rpm(regs_.read32(kTachCtl), regs_.read32(kTachCount));
  return gpio_tach_rpm();
}

Optional<PwmState> NvTherm::pwm() {
  uint32_t divs = 0, duty = 0;
  bool routed = false;
  if (!read_pwm_raw(&divs, &duty, &routed)) return {};
  PwmState state;
  state.mode = manual_ ? PwmMode::Manual : PwmMode::Automatic;
  if (!routed) {
    // fanpwm.c falls back to the GPIO: the fan is fully on or off
    const Optional<bool> on = gpio_.get(fan_->function, fan_->line);
    if (!on.has_value()) return {};
    state.duty = on.value() ? 255 : 0;
    return state;
  }
  const Optional<uint8_t> d = therm_decode::pwm_duty_255(divs, duty, (fan_->log0 & 1) != 0);
  if (!d.has_value()) return {};
  state.duty = d.value();
  return state;
}

bool NvTherm::set_pwm_duty(uint8_t duty255) {
  PwmRegs r;
  if (!pwm_regs(&r)) return false;
  uint32_t divs = regs_.read32(r.divs) & r.field_mask & 0x7fffffff;
  if (divs == 0) divs = saved_divs_;
  if (divs == 0) return false;   // no PWM frequency known (it is in the video BIOS's fan table, which is not read here)
  const uint32_t duty = therm_decode::pwm_duty_register(divs, duty255, (fan_->log0 & 1) != 0);
  if (r.field_mask != 0xffffffff) regs_.mask(r.divs, r.field_mask, divs);
  else regs_.write32(r.divs, divs);
  regs_.write32(r.duty, duty | r.commit);
  if (r.route_mask) regs_.mask(r.route, r.route_mask, r.route_on);
  manual_ = true;
  return true;
}

bool NvTherm::restore_firmware_pwm() {
  PwmRegs r;
  if (!saved_ || !pwm_regs(&r)) return false;
  if (r.field_mask != 0xffffffff) regs_.mask(r.divs, r.field_mask, saved_divs_);
  else regs_.write32(r.divs, saved_divs_);
  regs_.write32(r.duty, saved_duty_ | r.commit);
  if (r.route_mask) regs_.mask(r.route, r.route_mask, saved_route_);
  manual_ = false;
  return true;
}

// ---- NvThermMonitor ----

bool NvThermMonitor::initialize() {
  kernel::ScopedMutex guard(mutex_);
  const bool ok = therm_.initialize();
  __atomic_store_n(&initialized_, ok, __ATOMIC_RELEASE);
  return ok;
}

void NvThermMonitor::shutdown() {
  kernel::ScopedMutex guard(mutex_);
  if (__atomic_load_n(&initialized_, __ATOMIC_ACQUIRE) && therm_.has_pwm()) (void)therm_.restore_firmware_pwm();   // the firmware's fan setting back, whatever was set
}

uint32_t NvThermMonitor::channel_count(MonitorChannel kind) const {
  if (!__atomic_load_n(&initialized_, __ATOMIC_ACQUIRE)) return 0;   // (what initialize() found does not change afterwards)
  switch (kind) {
    case MonitorChannel::Temperature: return therm_.has_temperature() ? 1 : 0;
    case MonitorChannel::Fan: return therm_.has_fan_sense() ? 1 : 0;
    case MonitorChannel::Pwm: return therm_.has_pwm() ? 1 : 0;
    default: return 0;
  }
}

const char* NvThermMonitor::label(MonitorChannel kind, uint32_t channel) const {
  if (channel >= channel_count(kind)) return nullptr;
  switch (kind) {
    case MonitorChannel::Temperature: return "GPU";
    case MonitorChannel::Fan: return "GPU fan";
    case MonitorChannel::Pwm: return "GPU fan";
    default: return nullptr;
  }
}

Optional<int32_t> NvThermMonitor::temperature_millicelsius(uint32_t channel) {
  kernel::ScopedMutex guard(mutex_);
  if (channel >= channel_count(MonitorChannel::Temperature)) return {};
  return therm_.temperature_millicelsius();
}

Optional<uint32_t> NvThermMonitor::fan_rpm(uint32_t channel) {
  kernel::ScopedMutex guard(mutex_);
  if (channel >= channel_count(MonitorChannel::Fan)) return {};
  return therm_.fan_rpm();
}

Optional<PwmState> NvThermMonitor::pwm(uint32_t channel) {
  kernel::ScopedMutex guard(mutex_);
  if (channel >= channel_count(MonitorChannel::Pwm)) return {};
  return therm_.pwm();
}

bool NvThermMonitor::set_pwm_duty(uint32_t channel, uint8_t duty) {
  kernel::ScopedMutex guard(mutex_);
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  return therm_.set_pwm_duty(duty);
}

bool NvThermMonitor::set_pwm_mode(uint32_t channel, PwmMode mode) {
  kernel::ScopedMutex guard(mutex_);
  if (channel >= channel_count(MonitorChannel::Pwm)) return false;
  if (mode == PwmMode::Automatic) return therm_.restore_firmware_pwm();
  const Optional<PwmState> now = therm_.pwm();
  if (!now.has_value()) return false;
  return therm_.set_pwm_duty(now.value().duty);
}

}  // namespace nv
}  // namespace hardware
