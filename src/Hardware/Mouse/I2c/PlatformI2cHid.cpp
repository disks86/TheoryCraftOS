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


#include "Hardware/Mouse/I2c/PlatformI2cHid.hpp"

namespace hardware {

namespace {

// the HID over I2C device-specific method: UUID 3CDFF6F7-4267-4555-AD05-B30A3D8938DE, revision 1, function 1 gives the register of the HID descriptor
uint8_t g_hid_uuid[16] = {0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE};

void append(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  while (n < capacity && out[n]) ++n;
  for (uint32_t i = 0; text[i] && n + 1 < capacity; ++i) out[n++] = text[i];
  if (n < capacity) out[n] = 0;
}

}  // namespace

const char* PlatformI2cHid::outcome_name(Outcome o) {
  switch (o) {
    case Outcome::Started: return "started";
    case Outcome::NoController: return "no I2C controller found for it";
    case Outcome::ControllerFailed: return "its I2C controller would not start";
    case Outcome::NoDescriptorAddress: return "the tables give no HID descriptor address";
    case Outcome::DeviceFailed: return "the device did not answer as a HID device";
    case Outcome::NotAPointer: return "not a pointing device";
    case Outcome::MouseFull: return "no room for another mouse";
  }
  return "?";
}

acpi::Node* PlatformI2cHid::child(acpi::Node* parent, const char* name) const {
  acpi::Node* n = vm_.find(name, parent);
  return n && vm_.parent(n) == parent ? n : nullptr;
}

bool PlatformI2cHid::integer_of(acpi::Node* node, const char* name, uint64_t* out) {
  acpi::Node* n = child(node, name);
  return n && vm_.evaluate_integer(n, out);
}

bool PlatformI2cHid::hid_string(acpi::Node* node, char out[16]) {
  out[0] = 0;
  acpi::Node* n = child(node, "_HID");
  acpi::Value v;
  if (!n || !vm_.evaluate(n, nullptr, 0, &v)) return false;
  if (v.kind == acpi::ValueKind::Integer) {
    char text[8];
    acpi::eisa_id_to_string(static_cast<uint32_t>(v.integer), text);
    for (uint32_t i = 0; i < 7; ++i) out[i] = text[i];
    out[7] = 0;
    return true;
  }
  if (v.kind == acpi::ValueKind::String) {
    uint32_t i = 0;
    for (; i < v.length && i < 15; ++i) out[i] = static_cast<char>(v.bytes[i]);
    out[i] = 0;
    return true;
  }
  return false;
}

// SSCN / FMCN: a package of the high count, the low count and the SDA hold of the clock
void PlatformI2cHid::timing(acpi::Node* node, const char* method, uint32_t* hcnt, uint32_t* lcnt, uint32_t* sda) {
  *hcnt = *lcnt = *sda = 0;
  acpi::Node* n = child(node, method);
  acpi::Value v;
  if (!n || !vm_.evaluate(n, nullptr, 0, &v) || v.kind != acpi::ValueKind::Package || v.length < 2) return;
  if (v.elements[0].kind == acpi::ValueKind::Integer) *hcnt = static_cast<uint32_t>(v.elements[0].integer);
  if (v.elements[1].kind == acpi::ValueKind::Integer) *lcnt = static_cast<uint32_t>(v.elements[1].integer);
  if (v.length >= 3 && v.elements[2].kind == acpi::ValueKind::Integer) *sda = static_cast<uint32_t>(v.elements[2].integer);
}

PlatformI2cHid::Controller* PlatformI2cHid::controller_for(acpi::Node* node, const AcpiResources::I2c& i2c, Outcome* outcome) {
  for (uint64_t i = 0; i < controllers_.count(); ++i)
    if (controllers_[i]->node == node) return controllers_[i].get();
  uint64_t adr = 0;
  if (!integer_of(node, "_ADR", &adr)) { *outcome = Outcome::NoController; return nullptr; }
  const PciAddress address = {0, static_cast<uint8_t>((adr >> 16) & 0x1F), static_cast<uint8_t>(adr & 7)};
  UniquePointer<Controller> c = UniquePointer<Controller>::make(allocator_);
  if (!c) { *outcome = Outcome::ControllerFailed; return nullptr; }
  DesignWareI2c::Config config;
  config.bus_hz = i2c.speed_hz >= 400000 ? 400000 : 100000;
  uint32_t sda = 0;
  timing(node, "SSCN", &config.ss_hcnt, &config.ss_lcnt, &sda);
  uint32_t fast_sda = 0;
  timing(node, "FMCN", &config.fs_hcnt, &config.fs_lcnt, &fast_sda);
  config.sda_hold_register = config.bus_hz >= 400000 ? fast_sda : sda;
  c->node = node;
  uint64_t bar = 0;
  II2cRegisters* registers = nullptr;
  if (!LpssI2cController::prepare(pci_, address, &bar) || !(registers = i2c_registers_.open(bar)) || !c->lpss.initialize(*registers, address, config)) {
    *outcome = Outcome::ControllerFailed;
    return nullptr;
  }
  Controller* raw = c.get();
  if (!controllers_.push_back(util::move(c))) { *outcome = Outcome::ControllerFailed; return nullptr; }
  return raw;
}

IntelPchGpio* PlatformI2cHid::gpio_for(acpi::Node* node) {
  for (uint64_t i = 0; i < gpios_.count(); ++i)
    if (gpios_[i]->node == node) return &gpios_[i]->pads;
  if (!vm_.device_has_id(node, "INT344B")) return nullptr;
  acpi::Node* crs = child(node, "_CRS");
  acpi::Value v;
  AcpiResources resources;
  if (!crs || !vm_.evaluate(crs, nullptr, 0, &v) || v.kind != acpi::ValueKind::Buffer || !parse_acpi_resources(ConstByteView(v.bytes, v.length), &resources)) return nullptr;
  UniquePointer<Gpio> g = UniquePointer<Gpio>::make(allocator_);
  if (!g) return nullptr;
  g->node = node;
  for (uint32_t i = 0; i < resources.memory_count && i < IntelPchGpio::kSptLpCommunities; ++i) {
    MmioRegion region;
    if (resources.memory[i].length == 0 || !mapper_.map_registers(resources.memory[i].base, resources.memory[i].length, &region)) continue;
    g->pads.add_community(region, IntelPchGpio::sunrise_point_lp_pin_base(i), IntelPchGpio::sunrise_point_lp_pins(i));
  }
  IntelPchGpio* raw = &g->pads;
  if (!gpios_.push_back(util::move(g))) return nullptr;
  return raw;
}

bool PlatformI2cHid::setup_device(acpi::Node* device, CompositeMouse& mice, Report* report) {
  // the firmware's own set-up on the way down, and the device's power
  {
    acpi::Node* chain[16];
    uint32_t n = 0;
    for (acpi::Node* p = device; p && n < 16; p = vm_.parent(p)) chain[n++] = p;
    while (n) {
      acpi::Node* p = chain[--n];
      if (!vm_.is_device(p) && p != vm_.find("\\_SB")) continue;
      uint64_t sta = 0xF;
      if (acpi::Node* s = child(p, "_STA")) {
        if (!vm_.evaluate_integer(s, &sta)) sta = 0;
      }
      if (!(sta & 1)) continue;
      if (acpi::Node* ini = child(p, "_INI")) {
        if (vm_.is_method(ini)) vm_.evaluate(ini, nullptr, 0, nullptr);
      }
    }
  }
  char hid[16];
  hid_string(device, hid);
  const char* leaf = vm_.name_of(device);
  report->device[0] = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const char c[2] = {leaf[i], 0};
    append(report->device, sizeof report->device, c);
  }
  append(report->device, sizeof report->device, " ");
  append(report->device, sizeof report->device, hid);

  uint64_t sta = 0xF;
  if (acpi::Node* s = child(device, "_STA")) {
    if (!vm_.evaluate_integer(s, &sta)) return false;
  }
  if (!(sta & 1) || !(sta & 2)) return false;                     // not present, or not enabled: not an error, nothing to start
  if (acpi::Node* ps0 = child(device, "_PS0")) vm_.evaluate(ps0, nullptr, 0, nullptr);

  AcpiResources res;
  acpi::Value crs;
  acpi::Node* crs_node = child(device, "_CRS");
  if (!crs_node || !vm_.evaluate(crs_node, nullptr, 0, &crs) || crs.kind != acpi::ValueKind::Buffer || !parse_acpi_resources(ConstByteView(crs.bytes, crs.length), &res) || !res.i2c.present ||
      res.i2c.ten_bit) {
    report->outcome = Outcome::NoController;
    return true;
  }
  // the descriptor register
  uint64_t descriptor_register = 0;
  {
    acpi::Node* dsm = child(device, "_DSM");
    acpi::Value args[4];
    args[0].kind = acpi::ValueKind::Buffer;
    args[0].length = 16;
    args[0].bytes = g_hid_uuid;
    args[1].kind = acpi::ValueKind::Integer;
    args[1].integer = 1;
    args[2].kind = acpi::ValueKind::Integer;
    args[2].integer = 1;
    args[3].kind = acpi::ValueKind::Package;
    args[3].length = 0;
    acpi::Value out;
    if (!dsm || !vm_.evaluate(dsm, args, 4, &out) || out.kind != acpi::ValueKind::Integer || out.integer == 0 || out.integer > 0xFFFF) {
      report->outcome = Outcome::NoDescriptorAddress;
      return true;
    }
    descriptor_register = out.integer;
  }
  acpi::Node* controller_node = vm_.find(res.i2c.controller);
  Outcome outcome = Outcome::NoController;
  Controller* controller = controller_node ? controller_for(controller_node, res.i2c, &outcome) : nullptr;
  if (!controller) {
    report->outcome = outcome;
    return true;
  }
  UniquePointer<Pointer> p = UniquePointer<Pointer>::make(allocator_);
  if (!p) { report->outcome = Outcome::DeviceFailed; return true; }
  for (uint32_t i = 0; i < sizeof p->name; ++i) p->name[i] = 0;
  append(p->name, sizeof p->name, "I2C HID ");
  append(p->name, sizeof p->name, hid);
  p->pointer = UniquePointer<I2cHidPointer>::make(allocator_, allocator_, controller->lpss.bus(), res.i2c.address, static_cast<uint16_t>(descriptor_register), clock_, p->name);
  if (!p->pointer) { report->outcome = Outcome::DeviceFailed; return true; }
  // the interrupt pin, read as a level
  if (res.gpio_interrupt.present) {
    if (acpi::Node* gnode = vm_.find(res.gpio_interrupt.controller)) {
      if (IntelPchGpio* pads = gpio_for(gnode)) {
        if (pads->has_pin(res.gpio_interrupt.pin)) {
          UniquePointer<GpioAttentionLine> line = UniquePointer<GpioAttentionLine>::make(allocator_, *pads, res.gpio_interrupt.pin, res.gpio_interrupt.active_low);
          if (line) {
            p->pointer->set_attention(line.get());
            lines_.push_back(util::move(line));
          }
        }
      }
    }
  }
  if (!p->pointer->initialize()) {
    report->outcome = Outcome::DeviceFailed;
    return true;
  }
  report->kind = p->pointer->kind();
  // a pen and sensors on the same device
  if (pens_) {
    p->pen = UniquePointer<I2cHidPen>::make(allocator_, *p->pointer, p->name);
    if (p->pen && p->pen->initialize() && pens_->add(p->pen.get())) report->pen = true;
    else p->pen.reset();
  }
  if (sensors_) {
    SensorKind kinds[HidSensor::kMaxPerDevice];
    uint8_t ids[HidSensor::kMaxPerDevice];
    const uint32_t found = HidSensor::discover(p->pointer->map(), kinds, ids);
    for (uint32_t i = 0; i < found; ++i) {
      append(p->sensor_names[i], sizeof p->sensor_names[i], p->name);
      append(p->sensor_names[i], sizeof p->sensor_names[i], " ");
      append(p->sensor_names[i], sizeof p->sensor_names[i], sensor_kind_name(kinds[i]));
      p->sensors[i] = UniquePointer<HidSensor>::make(allocator_, *p->pointer, kinds[i], ids[i], p->sensor_names[i]);
      if (p->sensors[i] && p->sensors[i]->initialize() && sensors_->add(p->sensors[i].get())) ++report->sensors;
      else p->sensors[i].reset();
    }
  }
  p->as_mouse = report->kind != I2cHidPointer::Kind::Other;
  if (!p->as_mouse && !report->pen && report->sensors == 0) {                       // nothing of the device is used
    report->outcome = Outcome::NotAPointer;
    p->pointer->shutdown();
    return true;
  }
  if (p->as_mouse && !mice.add(p->pointer.get())) {
    report->outcome = Outcome::MouseFull;
    if (p->pen) { pens_->remove(p->pen.get()); p->pen->shutdown(); }
    for (auto& sensor : p->sensors)
      if (sensor) { sensors_->remove(sensor.get()); sensor->shutdown(); }
    p->pointer->shutdown();
    return true;
  }
  report->outcome = Outcome::Started;
  pointers_.push_back(util::move(p));
  return true;
}

void PlatformI2cHid::visit(acpi::Node* node, uint32_t depth, CompositeMouse& mice, uint32_t* added) {
  for (acpi::Node* n = vm_.first_child(node); n; n = vm_.next_sibling(n)) {
    if (vm_.is_device(n) && (vm_.device_has_id(n, "PNP0C50") || vm_.device_has_id(n, "ACPI0C50")) && !vm_.device_has_id(n, "CHPN0001") && !vm_.device_has_id(n, "IDEA5002") &&
        report_count_ < kMaxReports) {
      Report& r = reports_[report_count_];
      r = Report();
      if (setup_device(n, mice, &r)) {
        ++report_count_;
        if (r.outcome == Outcome::Started) ++*added;
      }
    }
    if (depth < 10 && !vm_.is_method(n)) visit(n, depth + 1, mice, added);
  }
}

uint32_t PlatformI2cHid::run(CompositeMouse& mice) {
  if (!vm_.root()) return 0;
  uint32_t added = 0;
  visit(vm_.root(), 0, mice, &added);
  return added;
}

uint32_t PlatformI2cHid::start(const ConstByteView* tables, uint32_t count, CompositeMouse& mice) {
  if (!vm_.root()) return 0;
  for (uint32_t i = 0; i < count; ++i)
    if (vm_.load_table(tables[i])) ++tables_loaded_;
  return run(mice);
}

uint32_t PlatformI2cHid::start(const AcpiTables& acpi, CompositeMouse& mice) {
  ConstByteView tables[64];
  uint32_t count = 0;
  const ConstByteView dsdt = acpi.dsdt();
  if (!dsdt.empty()) tables[count++] = dsdt;
  for (uint32_t i = 0; count < 64; ++i) {
    const ConstByteView ssdt = acpi.find_nth("SSDT", i);
    if (ssdt.empty()) break;
    tables[count++] = ssdt;
  }
  return start(tables, count, mice);
}

void PlatformI2cHid::stop(CompositeMouse& mice) {
  for (uint64_t i = 0; i < pointers_.count(); ++i) {
    Pointer& p = *pointers_[i];
    if (p.as_mouse) mice.remove(p.pointer.get());
    if (p.pen) { if (pens_) pens_->remove(p.pen.get()); p.pen->shutdown(); }
    for (auto& sensor : p.sensors)
      if (sensor) { if (sensors_) sensors_->remove(sensor.get()); sensor->shutdown(); }
    p.pointer->shutdown();
  }
  pointers_.clear();
}

}  // namespace hardware
