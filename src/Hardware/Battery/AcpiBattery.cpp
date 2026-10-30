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

#include "Hardware/Battery/AcpiBattery.hpp"

namespace hardware {

namespace {
constexpr uint32_t kInfoRefresh = 30;      // the capacities are read again every this many reads
constexpr uint32_t kMaxFailures = 5;       // reads that failed in a row before the status is given up on
constexpr uint32_t kUnknown = 0xFFFFFFFFu;

// The integer of element `i` of a package, or kUnknown.
uint64_t element(const acpi::Value& package, uint32_t i) {
  if (package.kind != acpi::ValueKind::Package || i >= package.length) return kUnknown;
  const acpi::Value& v = package.elements[i];
  return v.kind == acpi::ValueKind::Integer ? v.integer : kUnknown;
}

// The first two I/O ports of a resource template (the controller's data port, then its command port).
bool controller_ports(const acpi::Value& crs, uint16_t* data, uint16_t* command) {
  if (crs.kind != acpi::ValueKind::Buffer) return false;
  uint16_t ports[2];
  uint32_t found = 0;
  uint32_t at = 0;
  while (at < crs.length && found < 2) {
    const uint8_t tag = crs.bytes[at];
    if (tag & 0x80) {   // a large item: skip it
      if (at + 3 > crs.length) return false;
      at += 3u + (crs.bytes[at + 1] | (uint32_t{crs.bytes[at + 2]} << 8));
      continue;
    }
    const uint32_t type = (tag >> 3) & 0xF, length = tag & 7;
    if (type == 0xF) break;
    if (at + 1 + length > crs.length) return false;
    if (type == 0x8 && length >= 7) ports[found++] = static_cast<uint16_t>(crs.bytes[at + 2] | (crs.bytes[at + 3] << 8));      // IO: minimum base
    else if (type == 0x9 && length >= 3) ports[found++] = static_cast<uint16_t>(crs.bytes[at + 1] | (crs.bytes[at + 2] << 8));  // FixedIO
    at += 1 + length;
  }
  if (found < 2) return false;
  *data = ports[0];
  *command = ports[1];
  return true;
}
}  // namespace

// A method or name that `device` itself has (acpi::AmlInterpreter::find also looks in the scopes above).
acpi::Node* AcpiBattery::own(acpi::Node* device, const char* name) const {
  acpi::Node* n = vm_.find(name, device);
  return n && vm_.parent(n) == device ? n : nullptr;
}

void AcpiBattery::find_batteries(acpi::Node* node, uint32_t depth, acpi::Node** ec) {
  for (acpi::Node* n = vm_.first_child(node); n; n = vm_.next_sibling(n)) {
    if (vm_.is_device(n)) {
      if (battery_count_ < kMaxBatteries && vm_.device_has_id(n, "PNP0C0A")) {
        Battery& b = batteries_[battery_count_++];
        b.device = n;
        b.status = own(n, "_STA");
        b.state = own(n, "_BST");
        b.info = own(n, "_BIX");
        b.extended = b.info != nullptr;
        if (!b.info) b.info = own(n, "_BIF");
      } else if (!*ec && vm_.device_has_id(n, "PNP0C09")) {
        *ec = n;
      }
    }
    if (depth < 10 && !vm_.is_method(n)) find_batteries(n, depth + 1, ec);
  }
}

void AcpiBattery::set_up_controller(acpi::Node* ec) {
  uint16_t data = 0x62, command = 0x66;   // where PCs have it unless the tables say otherwise
  acpi::Value crs;
  if (ec) {
    if (acpi::Node* node = own(ec, "_CRS")) {
      if (vm_.evaluate(node, nullptr, 0, &crs)) controller_ports(crs, &data, &command);
    }
  }
  host_.configure_ec(data, command);
  if (ec) {   // "the controller's region can be used now"
    if (acpi::Node* reg = own(ec, "_REG")) {
      acpi::Value args[2];
      args[0].kind = args[1].kind = acpi::ValueKind::Integer;
      args[0].integer = static_cast<uint64_t>(acpi::RegionSpace::EmbeddedControl);
      args[1].integer = 1;
      vm_.evaluate(reg, args, 2, nullptr);
    }
  }
}

// Runs the _INI methods on the way from the root to a device (the firmware sets its own variables up there), if the device is there.
void AcpiBattery::run_init(acpi::Node* device) {
  acpi::Node* chain[16];
  uint32_t n = 0;
  for (acpi::Node* p = device; p && n < 16; p = vm_.parent(p)) chain[n++] = p;
  while (n) {
    acpi::Node* p = chain[--n];
    if (!vm_.is_device(p) && p != vm_.find("\\_SB")) continue;   // (the _SB scope has an _INI too)
    uint64_t sta = 0xF;
    if (acpi::Node* s = own(p, "_STA")) {
      if (!vm_.evaluate_integer(s, &sta)) sta = 0;
    }
    if (!(sta & 1)) continue;
    acpi::Node* ini = own(p, "_INI");
    if (ini && vm_.is_method(ini)) vm_.evaluate(ini, nullptr, 0, nullptr);
  }
}

bool AcpiBattery::initialize(const ConstByteView* tables, uint32_t count) {
  if (!vm_.root()) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (vm_.load_table(tables[i])) {
      ++tables_loaded_;
    } else {
      ++tables_faulty_;
      load_error_ = vm_.last_error();
    }
  }
  acpi::Node* ec = nullptr;
  find_batteries(vm_.root(), 0, &ec);
  if (battery_count_ == 0) return false;     // nothing to read: touch no hardware
  set_up_controller(ec);
  for (uint32_t i = 0; i < battery_count_; ++i) run_init(batteries_[i].device);
  return true;
}

bool AcpiBattery::initialize(const AcpiTables& acpi) {
  ConstByteView tables[kMaxTables];
  uint32_t count = 0;
  const ConstByteView dsdt = acpi.dsdt();
  if (!dsdt.empty()) tables[count++] = dsdt;
  for (uint32_t i = 0; count < kMaxTables; ++i) {
    const ConstByteView ssdt = acpi.find_nth("SSDT", i);
    if (ssdt.empty()) break;
    tables[count++] = ssdt;
  }
  for (uint32_t i = 0; count < kMaxTables; ++i) {   // (the tables of a second processor kind)
    const ConstByteView psdt = acpi.find_nth("PSDT", i);
    if (psdt.empty()) break;
    tables[count++] = psdt;
  }
  return initialize(tables, count);
}

BatteryStatus AcpiBattery::read() {
  kernel::ScopedMutex guard(read_mutex_);
  BatteryStatus out;
  if (battery_count_ == 0) return out;
  uint64_t remaining = 0, full = 0;
  bool charging = false, discharging = false, any_present = false, any_known = false;
  bool ok = true;
  for (uint32_t i = 0; i < battery_count_; ++i) {
    Battery& b = batteries_[i];
    uint64_t sta = 0x1F;
    if (b.status && !vm_.evaluate_integer(b.status, &sta)) {
      ok = false;
      continue;
    }
    if (!(sta & 0x10)) continue;   // no battery in the bay
    any_present = true;
    if (!b.state) continue;
    acpi::Value bst;
    if (!vm_.evaluate(b.state, nullptr, 0, &bst)) {
      ok = false;
      continue;
    }
    const uint64_t state = element(bst, 0), rem = element(bst, 2);
    if (b.info && (b.info_age >= kInfoRefresh || b.last_full == 0)) {
      acpi::Value info;
      if (vm_.evaluate(b.info, nullptr, 0, &info)) {
        const uint32_t at = b.extended ? 2 : 1;   // _BIX: revision, unit, design, last full; _BIF: unit, design, last full
        b.design = element(info, at);
        b.last_full = element(info, at + 1);
        b.info_age = 0;
      }
    } else if (b.info_age != ~0u) {
      ++b.info_age;
    }
    uint64_t capacity = b.last_full && b.last_full != kUnknown ? b.last_full : b.design;
    if (rem == kUnknown || !capacity || capacity == kUnknown) continue;
    any_known = true;
    remaining += rem;
    full += capacity;
    if (state != kUnknown) {
      if (state & 2) charging = true;
      else if (state & 1) discharging = true;
    }
  }
  failures_ = ok ? 0 : failures_ + 1;
  out.present = any_present;
  if (!any_present || !any_known || failures_ >= kMaxFailures) return out;
  const uint64_t percent = full ? (remaining * 100 + full / 2) / full : 0;
  out.percent = static_cast<int32_t>(percent > 100 ? 100 : percent);
  out.state = charging ? BatteryState::Charging
              : discharging ? BatteryState::Discharging
              : out.percent >= 99 ? BatteryState::Full
                                  : BatteryState::NotCharging;
  return out;
}

}  // namespace hardware
