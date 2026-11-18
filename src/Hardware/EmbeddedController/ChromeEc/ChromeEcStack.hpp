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

// A found Chrome EC with everything built on it: the transport detection chose, the host command layer, the Framework commands, the hardware
// monitor and the keyboard backlight. Lives in place (the layers hold references to each other) and needs no allocation. SystemChromeEc runs
// one on the machine's ports; the tests run one on a model.
//
// The battery charge limit is the OS's setting, not only the EC's: set_charge_limit() keeps it in an IChargeLimitStore (the system database,
// /AppStore/System/system.db, in the running system) and restore_charge_limit() hands it to the EC again at start-up, so a limit survives an
// EC reset or a firmware that forgets it.
#pragma once
#include <stdint.h>
#include "Hardware/EmbeddedController/ChromeEc/ChromeEcDevices.hpp"
#include "Hardware/EmbeddedController/ChromeEc/FrameworkEc.hpp"

namespace hardware {

class IChargeLimitStore {
 public:
  // The limit kept last time (percent), empty when none was kept.
  virtual Optional<uint8_t> load() = 0;
  [[nodiscard]] virtual bool store(uint8_t max_percent) = 0;

 protected:
  constexpr IChargeLimitStore() = default;
  ~IChargeLimitStore() = default;
};

class ChromeEcStack {
 public:
  explicit ChromeEcStack(IChromeEcPorts& ports) : ports_(ports) {}
  ChromeEcStack(const ChromeEcStack&) = delete;
  ChromeEcStack& operator=(const ChromeEcStack&) = delete;

  // Detects the EC (see detect_chrome_ec: nothing is written to a port before it is identified) and starts it. False when there is none or it
  // does not answer; why: problem().
  [[nodiscard]] bool start(bool acpi_id_present, IEcFirmwareLock* firmware_lock = nullptr);
  bool present() const { return ec_ && ec_->started(); }
  const char* problem() const { return problem_; }

  ChromeEc* ec() { return present() ? &*ec_ : nullptr; }
  // The Framework commands, when the EC answered the charge limit command at start (a Framework EC).
  FrameworkEc* framework() { return framework_ ? &*framework_ : nullptr; }
  ChromeEcHardwareMonitor* monitor() { return monitor_ready_ ? &*monitor_ : nullptr; }
  ChromeEcKeyboardBacklight* keyboard_backlight() { return backlight_ready_ ? &*backlight_ : nullptr; }

  [[nodiscard]] Optional<FrameworkChargeLimit> charge_limit();
  // Sets the limit on the EC and, when that worked, keeps it in `store` (may be nullptr).
  [[nodiscard]] bool set_charge_limit(uint8_t max_percent, IChargeLimitStore* store);
  // Gives the EC the limit `store` kept. False when there is none, or no Framework EC, or it refused.
  [[nodiscard]] bool restore_charge_limit(IChargeLimitStore& store);

  [[nodiscard]] Optional<FrameworkPrivacySwitches> privacy_switches();

  // Firmware versions, chip, transport, and the Framework features that answered.
  void describe(IPropertySink& sink);

 private:
  bool try_start(EcTransportKind kind, uint16_t memmap_base, IEcFirmwareLock* firmware_lock);
  void reset();

  IChromeEcPorts& ports_;
  Optional<LpcTransport> lpc_;
  Optional<MecTransport> mec_;
  Optional<ChromeEc> ec_;
  Optional<FrameworkEc> framework_;
  Optional<ChromeEcHardwareMonitor> monitor_;
  Optional<ChromeEcKeyboardBacklight> backlight_;
  bool monitor_ready_ = false, backlight_ready_ = false;
  const char* problem_ = "not started";
};

}  // namespace hardware
