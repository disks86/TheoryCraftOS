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

#include "Hardware/Acpi/AcpiEvents.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

bool acpi_parse_fadt(ConstByteView table, AcpiFadt* out) {
  // (offsets of the ACPI specification's FADT: all inside the 1.0 part, which every revision has)
  if (table.size() < 96 || table[0] != 'F' || table[1] != 'A' || table[2] != 'C' || table[3] != 'P') return false;
  ByteReader r(table);
  r.skip(46);
  AcpiFadt f;
  f.sci_interrupt = r.le16();
  f.smi_command = r.le32();
  f.acpi_enable = r.u8();
  f.acpi_disable = r.u8();
  r.skip(2);                      // S4BIOS_REQ, PSTATE_CNT
  r.skip(4);                      // PM1a_EVT_BLK
  r.skip(4);                      // PM1b_EVT_BLK
  f.pm1a_control = r.le32();
  r.skip(4 + 4 + 4);              // PM1b_CNT_BLK, PM2_CNT_BLK, PM_TMR_BLK
  f.gpe0_block = r.le32();
  f.gpe1_block = r.le32();
  r.skip(1 + 1 + 1 + 1);          // PM1_EVT_LEN, PM1_CNT_LEN, PM2_CNT_LEN, PM_TMR_LEN
  f.gpe0_length = r.u8();
  f.gpe1_length = r.u8();
  f.gpe1_base = r.u8();
  if (!r.ok() || f.pm1a_control > 0xFFFF || f.gpe0_block > 0xFFFF || f.gpe1_block > 0xFFFF || f.smi_command > 0xFFFF) return false;   // (I/O ports)
  *out = f;
  return true;
}

bool AcpiEvents::initialize(const ConstByteView* tables, uint32_t count, ConstByteView fadt) {
  if (!vm_.root() || !acpi_parse_fadt(fadt, &fadt_)) return false;
  if ((fadt_.gpe0_block == 0 || fadt_.gpe0_length < 2) && (fadt_.gpe1_block == 0 || fadt_.gpe1_length < 2)) return false;   // no events to wait for
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (vm_.load_table(tables[i])) ++loaded;
  ready_ = loaded != 0;
  return ready_;
}

bool AcpiEvents::initialize(const AcpiTables& acpi) {
  ConstByteView tables[kMaxTables];
  uint32_t count = 0;
  const ConstByteView dsdt = acpi.dsdt();
  if (dsdt.empty()) return false;
  tables[count++] = dsdt;
  for (uint32_t i = 0; count < kMaxTables; ++i) {
    const ConstByteView ssdt = acpi.find_nth("SSDT", i);
    if (ssdt.empty()) break;
    tables[count++] = ssdt;
  }
  return initialize(tables, count, acpi.find("FACP"));
}

bool AcpiEvents::acpi_mode() { return fadt_.pm1a_control != 0 && (ports_.in16(static_cast<uint16_t>(fadt_.pm1a_control)) & kScienable) != 0; }

bool AcpiEvents::enable_acpi_mode() {
  if (!ready_ || fadt_.pm1a_control == 0) return false;
  if (acpi_mode()) return true;
  if (fadt_.smi_command == 0 || fadt_.acpi_enable == 0) return false;   // (a machine that is always in ACPI mode has neither)
  ports_.out8(static_cast<uint16_t>(fadt_.smi_command), fadt_.acpi_enable);
  for (uint32_t waited = 0; waited < 3000000; waited += 1000) {
    if (acpi_mode()) return true;
    ports_.delay_us(1000);
  }
  return false;
}

// --- finding the device

bool AcpiEvents::is_within(const acpi::Node* ancestor, const acpi::Node* device) const {
  for (uint32_t depth = 0; device && depth < 64; ++depth) {
    if (device == ancestor) return true;
    device = vm_.parent(device);
  }
  return false;
}

bool AcpiEvents::find_in(acpi::Node* parent, const IPciBus& pci, uint8_t bus, PciAddress wanted, uint32_t depth, acpi::Node** found) {
  if (depth > 8) return false;
  for (acpi::Node* child = vm_.first_child(parent); child; child = vm_.next_sibling(child)) {
    if (!vm_.is_device(child)) continue;
    uint64_t adr = 0;
    acpi::Node* adr_node = vm_.find("_ADR", child);
    if (!adr_node || vm_.parent(adr_node) != child || !vm_.evaluate_integer(adr_node, &adr)) continue;
    const PciAddress here = {bus, static_cast<uint8_t>((adr >> 16) & 0x1F), static_cast<uint8_t>(adr & 7)};
    if (here.bus == wanted.bus && here.device == wanted.device && here.function == wanted.function) {
      *found = child;
      return true;
    }
    // a bridge: its secondary bus (header type 1)
    const uint32_t header = pci.read32(here, 0x0C), buses = pci.read32(here, 0x18);
    if (((header >> 16) & 0x7F) == 1) {
      const uint8_t secondary = static_cast<uint8_t>(buses >> 8);
      if (secondary != 0 && secondary != bus && find_in(child, pci, secondary, wanted, depth + 1, found)) return true;
    }
  }
  return false;
}

acpi::Node* AcpiEvents::find_pci_device(const IPciBus& pci, PciAddress address) {
  if (!ready_) return nullptr;
  // the root bridges: devices with the PCI Express or PCI root bridge ids, on the bus _BBN says (0 without one)
  struct Walker {
    AcpiEvents& self;
    const IPciBus& pci;
    PciAddress wanted;
    acpi::Node* found = nullptr;
    void visit(acpi::Node* node, uint32_t depth) {
      if (found || depth > 6) return;
      for (acpi::Node* child = self.vm_.first_child(node); child && !found; child = self.vm_.next_sibling(child)) {
        if (self.vm_.is_device(child) && (self.vm_.device_has_id(child, "PNP0A08") || self.vm_.device_has_id(child, "PNP0A03"))) {
          uint64_t bus = 0;
          acpi::Node* bbn = self.vm_.find("_BBN", child);
          if (bbn && self.vm_.parent(bbn) == child) self.vm_.evaluate_integer(bbn, &bus);
          self.find_in(child, pci, static_cast<uint8_t>(bus), wanted, 0, &found);
        } else {
          visit(child, depth + 1);
        }
      }
    }
  } walker{*this, pci, address};
  walker.visit(vm_.root(), 0);
  return walker.found;
}

// --- wake events

bool AcpiEvents::wake_source(acpi::Node* device, WakeSource* out) {
  if (!ready_ || !device) return false;
  acpi::Node* prw = vm_.find("_PRW", device);
  if (!prw || vm_.parent(prw) != device) return false;
  acpi::Value package;
  if (!vm_.evaluate(prw, nullptr, 0, &package) || package.kind != acpi::ValueKind::Package || package.length < 2) return false;
  acpi::Value first = package.elements[0];
  if (first.kind == acpi::ValueKind::Package) {                        // { GPE block device, number }
    if (first.length < 2) return false;
    first = first.elements[1];
  }
  const acpi::Value state = package.elements[1];
  if (first.kind != acpi::ValueKind::Integer || state.kind != acpi::ValueKind::Integer) return false;   // (an SCI-style bit of a PM1 block is not supported)
  out->gpe = static_cast<uint32_t>(first.integer);
  out->deepest_state = static_cast<uint8_t>(state.integer);
  return first.integer < 0x10000;
}

bool AcpiEvents::locate(uint32_t gpe, uint16_t* status_port, uint16_t* enable_port, uint8_t* bit) const {
  struct Block { uint32_t port, length, base; };
  const Block blocks[2] = {{fadt_.gpe0_block, fadt_.gpe0_length, 0}, {fadt_.gpe1_block, fadt_.gpe1_length, fadt_.gpe1_base}};
  for (const Block& block : blocks) {
    const uint32_t registers = block.length / 2;                       // (the status registers first, the enable registers after them)
    if (block.port == 0 || registers == 0 || registers > kMaxBlockBytes || gpe < block.base || gpe - block.base >= registers * 8) continue;
    const uint32_t index = (gpe - block.base) / 8;
    *status_port = static_cast<uint16_t>(block.port + index);
    *enable_port = static_cast<uint16_t>(block.port + registers + index);
    *bit = static_cast<uint8_t>(1u << ((gpe - block.base) % 8));
    return true;
  }
  return false;
}

bool AcpiEvents::enable_wake(acpi::Node* device) {
  WakeSource source;
  uint16_t status = 0, enable = 0;
  uint8_t bit = 0;
  if (!wake_source(device, &source) || !locate(source.gpe, &status, &enable, &bit)) return false;
  for (uint32_t i = 0; i < wake_count_; ++i)
    if (wake_[i].device == device) return true;
  if (wake_count_ >= kMaxWake) return false;
  acpi::Node* psw = vm_.find("_PSW", device);
  if (psw && vm_.parent(psw) == device) {
    acpi::Value on;
    on.kind = acpi::ValueKind::Integer;
    on.integer = 1;
    vm_.evaluate(psw, &on, 1, nullptr);
  }
  wake_[wake_count_++] = Wake{device, source.gpe};
  ports_.out8(status, bit);                                            // (a stale event would wake us at once)
  ports_.out8(enable, static_cast<uint8_t>(ports_.in8(enable) | bit));
  return true;
}

void AcpiEvents::disable_wake(acpi::Node* device) {
  for (uint32_t i = 0; i < wake_count_; ++i) {
    if (wake_[i].device != device) continue;
    uint16_t status = 0, enable = 0;
    uint8_t bit = 0;
    bool shared = false;                                               // (another device on the same event keeps it armed)
    for (uint32_t k = 0; k < wake_count_; ++k) shared = shared || (k != i && wake_[k].gpe == wake_[i].gpe);
    if (!shared && locate(wake_[i].gpe, &status, &enable, &bit)) ports_.out8(enable, static_cast<uint8_t>(ports_.in8(enable) & ~bit));
    acpi::Node* psw = vm_.find("_PSW", device);
    if (psw && vm_.parent(psw) == device) {
      acpi::Value off;
      off.kind = acpi::ValueKind::Integer;
      off.integer = 0;
      vm_.evaluate(psw, &off, 1, nullptr);
    }
    wake_[i] = wake_[--wake_count_];
    wake_[wake_count_] = Wake();
    return;
  }
}

// --- events

void AcpiEvents::deliver() {
  // (the listener may call back into the events: take the list first)
  acpi::Node* nodes[kMaxNotifies];
  uint32_t values[kMaxNotifies], count = bridge_.count;
  for (uint32_t i = 0; i < count; ++i) {
    nodes[i] = bridge_.nodes[i];
    values[i] = bridge_.values[i];
  }
  bridge_.count = 0;
  if (listener_)
    for (uint32_t i = 0; i < count; ++i) listener_->on_notify(nodes[i], values[i]);
}

static char hex_digit(uint32_t v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

void AcpiEvents::handle(uint32_t gpe, uint16_t status_port, uint16_t enable_port, uint8_t bit) {
  ++handled_;
  // \_GPE._Lxx (level: the source stays until the method has dealt with it, so the event is masked meanwhile) or ._Exx (edge: acknowledged first)
  char name[16] = {'\\', '_', 'G', 'P', 'E', '.', '_', 'L', hex_digit((gpe >> 4) & 0xF), hex_digit(gpe & 0xF), 0};
  acpi::Node* method = gpe < 0x100 ? vm_.find(name) : nullptr;
  bool level = true;
  if (!method) {
    name[7] = 'E';
    method = gpe < 0x100 ? vm_.find(name) : nullptr;
    level = false;
  }
  if (!method) {                                                       // nothing answers it: silence it, or it would fire for ever
    ++unanswered_;
    ports_.out8(enable_port, static_cast<uint8_t>(ports_.in8(enable_port) & ~bit));
    ports_.out8(status_port, bit);
    return;
  }
  if (level) ports_.out8(enable_port, static_cast<uint8_t>(ports_.in8(enable_port) & ~bit));
  else ports_.out8(status_port, bit);
  vm_.evaluate(method, nullptr, 0, nullptr);
  if (level) {
    ports_.out8(status_port, bit);
    ports_.out8(enable_port, static_cast<uint8_t>(ports_.in8(enable_port) | bit));
  }
  deliver();
}

uint32_t AcpiEvents::poll() {
  if (!ready_) return 0;
  uint32_t before = handled_;
  struct Block { uint32_t port, length, base; };
  const Block blocks[2] = {{fadt_.gpe0_block, fadt_.gpe0_length, 0}, {fadt_.gpe1_block, fadt_.gpe1_length, fadt_.gpe1_base}};
  for (const Block& block : blocks) {
    const uint32_t registers = block.length / 2;
    if (block.port == 0 || registers == 0 || registers > kMaxBlockBytes) continue;
    for (uint32_t i = 0; i < registers; ++i) {
      const uint16_t status_port = static_cast<uint16_t>(block.port + i), enable_port = static_cast<uint16_t>(block.port + registers + i);
      const uint8_t pending = static_cast<uint8_t>(ports_.in8(status_port) & ports_.in8(enable_port));
      for (uint32_t bit = 0; bit < 8; ++bit)
        if (pending & (1u << bit)) handle(block.base + i * 8 + bit, status_port, enable_port, static_cast<uint8_t>(1u << bit));
    }
  }
  return handled_ - before;
}

}  // namespace hardware
