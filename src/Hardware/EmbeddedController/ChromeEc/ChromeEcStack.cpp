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

#include "Hardware/EmbeddedController/ChromeEc/ChromeEcStack.hpp"

namespace hardware {

void ChromeEcStack::reset() {
  backlight_ready_ = monitor_ready_ = false;
  backlight_.reset();
  monitor_.reset();
  framework_.reset();
  ec_.reset();
  mec_.reset();
  lpc_.reset();
}

bool ChromeEcStack::try_start(EcTransportKind kind, uint16_t memmap_base, IEcFirmwareLock* firmware_lock) {
  reset();
  ChromeEcTransport* transport = nullptr;
  if (kind == EcTransportKind::Mec) transport = &mec_.emplace(ports_, memmap_base);
  else transport = &lpc_.emplace(ports_, memmap_base);
  ChromeEc& ec = ec_.emplace(*transport);
  ec.set_firmware_lock(firmware_lock);
  if (!ec.start()) {
    problem_ = ec.problem();
    reset();
    return false;
  }
  framework_.emplace(ec);
  if (!framework_->charge_limit()) framework_.reset();   // (not a Framework EC, or one without the command)
  monitor_ready_ = monitor_.emplace(ec).initialize();
  if (!monitor_ready_) monitor_.reset();
  backlight_ready_ = backlight_.emplace(ec).initialize();
  if (!backlight_ready_) backlight_.reset();
  problem_ = nullptr;
  return true;
}

bool ChromeEcStack::start(bool acpi_id_present, IEcFirmwareLock* firmware_lock) {
  reset();
  const EcDetection d = detect_chrome_ec(ports_, acpi_id_present);
  if (!d.found) {
    problem_ = d.reason;
    return false;
  }
  if (d.kind == EcTransportKind::Mec) {
    if (try_start(EcTransportKind::Mec, d.mec_plain_memmap_base, firmware_lock)) return true;
    // The EMI answered "EC" but host commands through it did not work, and a plain map was seen too: an LPC EC after all.
    return d.lpc_memmap_base && try_start(EcTransportKind::Lpc, d.lpc_memmap_base, firmware_lock);
  }
  if (try_start(EcTransportKind::Lpc, d.lpc_memmap_base, firmware_lock)) return true;
  // A map remapped to plain I/O at 0xE00 (cros_ec_lpc's Framework quirk) in front of a MEC whose EMI does not show it: host commands go
  // through the EMI, the map is read where it was found. (The EC is identified by then, so the EMI may be written.)
  const char* lpc_problem = problem_;
  if (d.lpc_memmap_base == chromeec::kPortFrameworkMemmap && try_start(EcTransportKind::Mec, d.lpc_memmap_base, firmware_lock)) return true;
  problem_ = lpc_problem;
  return false;
}

Optional<FrameworkChargeLimit> ChromeEcStack::charge_limit() {
  if (!framework_) return {};
  return framework_->charge_limit();
}

bool ChromeEcStack::set_charge_limit(uint8_t max_percent, IChargeLimitStore* store) {
  if (!framework_ || !framework_->set_charge_limit(max_percent)) return false;
  if (store) (void)store->store(max_percent);   // (the EC has it either way; a store that failed only loses it at the next start)
  return true;
}

bool ChromeEcStack::restore_charge_limit(IChargeLimitStore& store) {
  if (!framework_) return false;
  const Optional<uint8_t> saved = store.load();
  if (!saved) return false;
  return framework_->set_charge_limit(*saved);
}

Optional<FrameworkPrivacySwitches> ChromeEcStack::privacy_switches() {
  if (!framework_) return {};
  return framework_->privacy_switches();
}

void ChromeEcStack::describe(IPropertySink& sink) {
  if (!present()) return;
  if (monitor_ready_) monitor_->describe(sink);
  else {
    sink.add_text("Embedded controller firmware (read-only)", ec_->version().read_only.c_str());
    sink.add_text("Embedded controller firmware (read-write)", ec_->version().read_write.c_str());
  }
  if (!framework_) return;
  if (Optional<FrameworkChargeLimit> limit = framework_->charge_limit()) sink.add_number("Battery charge limit", limit->max_percent, "%");
  if (Optional<FrameworkPrivacySwitches> p = framework_->privacy_switches()) {
    sink.add_text("Camera", p->camera_connected ? "connected" : "cut off by the privacy switch");
    sink.add_text("Microphone", p->microphone_connected ? "connected" : "cut off by the privacy switch");
  }
  if (Optional<FrameworkChassisIntrusion> c = framework_->chassis_intrusion()) sink.add_number("Chassis opened", c->open_count, "times");
  if (Optional<FrameworkExpansionBay> bay = framework_->expansion_bay())
    sink.add_text("Expansion bay", bay->fault ? "fault" : bay->enabled ? "module enabled" : bay->door_closed ? "closed, module off" : "open");
}

}  // namespace hardware
