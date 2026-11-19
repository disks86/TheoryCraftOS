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

// After nouveau (MIT license): nvkm/engine/disp/dp.c (nvkm_dp_enable: the eDP panel's power line before AUX), nouveau_backlight.c
// (nva3_get/set_intensity: the SOR PWM); the waits as Linux's intel_pps.c falls back to the specifications' limits.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvPanelPower.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayHw.hpp"

namespace hardware {
namespace nv {

void NvPanelPower::wait_ms(uint32_t ms) {
  if (ms == 0) return;
  waited_ms_ += ms;
  // (in steps of at most one second: delay_us takes 32 bits of microseconds)
  while (ms > 0) {
    const uint32_t step = ms > 1000 ? 1000 : ms;
    r_.delay_us(step * 1000);
    ms -= step;
  }
}

bool NvPanelPower::power_on() {
  if (powered_) return true;
  // The power cycle: a panel switched off must stay off this long.
  if (was_off_) {
    uint32_t rest = t_.power_cycle_ms;
    if (now_) {
      const uint64_t now = now_();
      const uint64_t gone_ms = now >= off_at_us_ ? (now - off_at_us_) / 1000 : 0;
      rest = gone_ms >= t_.power_cycle_ms ? 0 : static_cast<uint32_t>(t_.power_cycle_ms - gone_ms);
    }
    wait_ms(rest);
  }
  switched_ = false;
  line_ = false;
  if (gpio_) {
    const Optional<bool> state = gpio_->get_function(disp_hw::kGpioPanelPower);
    if (state) {
      line_ = true;
      if (!*state) {
        if (!gpio_->set_function(disp_hw::kGpioPanelPower, true)) return false;
        switched_ = true;
      }
    } else if (gpio_->set_function(disp_hw::kGpioPanelPower, true)) {
      // (an INvGpio that cannot read the line back, but drives it: taken as switched on by us)
      line_ = true;
      switched_ = true;
    }
  }
  powered_ = true;
  wait_ms(t_.power_up_ms);   // (also when the line was on already or there is none: the panel may still be waking, as nouveau does)
  return true;
}

void NvPanelPower::power_off() {
  if (!powered_) return;
  if (backlight_) backlight_off();
  wait_ms(t_.power_down_ms);
  if (switched_ && gpio_) (void)gpio_->set_function(disp_hw::kGpioPanelPower, false);
  powered_ = false;
  switched_ = false;
  was_off_ = true;
  off_at_us_ = now_ ? now_() : 0;
}

void NvPanelPower::backlight_on(uint32_t sor) {
  if (backlight_) return;
  wait_ms(t_.backlight_on_ms);
  backlight_ = true;
  sor_ = sor;
  const uint32_t base = sor * disp_hw::kSorStride;
  const uint32_t div = r_.read32(kSorPwmDiv + base);
  pwm_ = div != 0 && div != 0xffffffffu;
  if (!pwm_) return;
  if (level_ == 0) level_ = r_.read32(kSorPwmCtl + base) & kSorPwmCtlValue;
  if (level_ == 0 || level_ > div) level_ = div & kSorPwmCtlValue;
  r_.write32(kSorPwmCtl + base, level_ | kSorPwmCtlNew | kSorPwmCtlUnk);
}

void NvPanelPower::backlight_off() {
  if (!backlight_) return;
  backlight_ = false;
  if (pwm_) r_.write32(kSorPwmCtl + sor_ * disp_hw::kSorStride, kSorPwmCtlNew | kSorPwmCtlUnk);
  wait_ms(t_.backlight_off_ms);
}

}  // namespace nv
}  // namespace hardware
