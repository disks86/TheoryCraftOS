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

// The power sequence of a built-in panel on an NVIDIA GPU's SOR (a muxed laptop's discrete GPU): the panel's power line, the waits a panel
// needs between its steps, and the backlight's PWM.
//
//   - Power: the DCB GPIO function 0x01 (DCB_GPIO_PANEL_POWER), as nouveau's nvkm_dp_enable drives it for an eDP panel before any AUX
//     transaction: only switched on when it reads off, and then switched off again by power_off() only if this switched it on (nouveau's
//     aux_pwr_pu). A board without the function has the panel's power on another path (the VBIOS's scripts, the embedded controller): the
//     waits are made anyway, as nouveau does.
//   - Waits: the panel's VBIOS table would give them; it is not read here, so the fallbacks are the upper limits of the specifications, as
//     Linux's intel_pps.c uses them when the VBT has none: eDP (eDP 1.4 table "panel power sequence timing parameters") T1+T3 210 ms, T8
//     50 ms, T9 50 ms, T10 500 ms, T11+T12 510 ms; LVDS (SPWG) T1+T2 60 ms, backlight 200 ms either way, T5 50 ms, the power cycle 510 ms.
//     nouveau waits 300 ms after an eDP panel's power regardless ("some laptop panels having a significant resume delay").
//   - Power cycle: a panel switched off must stay off T11+T12 before it comes on again: with a clock (`now_us`) the rest of that time is
//     waited, without one the whole of it whenever this object switched the panel off before.
//   - Backlight: the SOR's PWM (nouveau_backlight.c, nva3_set_intensity: NV50_PDISP_SOR_PWM_DIV 0x61c080 and _CTL 0x61c084 + sor * 0x800,
//     the level written with bit 31 (NEW) and bit 30): the level the panel had (or full, the divider, when it had none) on, 0 off. A divider
//     reading 0 means the panel's backlight is not on the SOR's PWM (an eDP panel driven through DPCD, or the embedded controller): nothing is
//     written. [S]: the register layout is nouveau's, not checked on hardware.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayMode.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvGpio.hpp"

namespace hardware {
namespace nv {

struct PanelPowerTimings {
  uint32_t power_up_ms = 210;       // eDP T1+T3 (LVDS T1+T2): power on until the panel answers on AUX / takes data
  uint32_t backlight_on_ms = 50;    // eDP T8 (LVDS T5): valid video until the backlight goes on
  uint32_t backlight_off_ms = 50;   // eDP T9 (LVDS T6): backlight off until the video stops
  uint32_t power_down_ms = 500;     // eDP T10 (LVDS T3): the video stopped until the power goes
  uint32_t power_cycle_ms = 510;    // eDP T11+T12 (LVDS T4): power off until it may come on again
  static PanelPowerTimings edp() { return PanelPowerTimings{}; }
  static PanelPowerTimings lvds() { return PanelPowerTimings{60, 200, 200, 50, 510}; }
};

// SOR backlight PWM registers (nouveau nv50_display.h NV50_PDISP_SOR_PWM_*), + sor * kSorStride.
constexpr uint32_t kSorPwmDiv = 0x61c080, kSorPwmCtl = 0x61c084;   // [S]
constexpr uint32_t kSorPwmCtlNew = 0x80000000, kSorPwmCtlUnk = 0x40000000, kSorPwmCtlValue = 0x00ffffff;   // [S]

// INvGpio over the board's GPIO table (NvGpio: nvkm_gpio_set / nvkm_gpio_get by function).
class NvBoardGpio final : public INvGpio {
 public:
  NvBoardGpio(INvRegisters& registers, const Chip& chip, const BoardInfo& board) : gpio_(registers, chip, board) {}
  [[nodiscard]] bool set_function(uint8_t function, bool on) override { return gpio_.set(function, on); }
  [[nodiscard]] Optional<bool> get_function(uint8_t function) override { return gpio_.get(function); }

 private:
  NvGpio gpio_;
};

class NvPanelPower {
 public:
  using ClockFn = uint64_t (*)();   // a monotonic clock in microseconds

  // `gpio` may be null (no power line: only the waits). `registers` waits (delay_us) and reaches the PWM.
  NvPanelPower(INvRegisters& registers, INvGpio* gpio, const PanelPowerTimings& timings, ClockFn now_us = nullptr)
      : r_(registers), gpio_(gpio), t_(timings), now_(now_us) {}

  // The rest of the power cycle time, the power line on (if it was off), then T1+T3. False when the line exists, reads off and would not
  // switch on (nothing then needs undoing).
  [[nodiscard]] bool power_on();
  // T10, then the power line off if power_on() switched it on, and the power cycle clock started. Nothing if not powered.
  void power_off();
  // T8, then the backlight's PWM on `sor` at the level it had (or full).
  void backlight_on(uint32_t sor);
  // The PWM to 0 (the level kept for backlight_on), then T9. Nothing if backlight_on() did not turn it on.
  void backlight_off();

  bool powered() const { return powered_; }
  bool switched_line() const { return switched_; }   // power_on() drove the power line (it was off)
  bool has_line() const { return line_; }            // the board has the power line
  bool backlight() const { return backlight_; }
  bool pwm() const { return pwm_; }                  // the backlight is on the SOR's PWM
  uint32_t level() const { return level_; }
  // Waited in all (milliseconds), for the tests and the log.
  uint64_t waited_ms() const { return waited_ms_; }
  const PanelPowerTimings& timings() const { return t_; }

 private:
  void wait_ms(uint32_t ms);

  INvRegisters& r_;
  INvGpio* gpio_;
  PanelPowerTimings t_;
  ClockFn now_;
  bool powered_ = false, switched_ = false, line_ = false;
  bool backlight_ = false, pwm_ = false;
  uint32_t sor_ = 0, level_ = 0;
  bool was_off_ = false;            // this object switched the panel off before (the power cycle applies)
  uint64_t off_at_us_ = 0;
  uint64_t waited_ms_ = 0;
};

}  // namespace nv
}  // namespace hardware
