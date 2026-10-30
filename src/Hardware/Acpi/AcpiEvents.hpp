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

// ACPI events: the general purpose events (GPEs) the chipset raises, and the AML methods that answer them. A device that can wake the machine names its GPE in _PRW; when that event fires the
// firmware's _Lxx or _Exx method (under \_GPE) runs, finds out which device it was and says Notify(device, 2). That is how a PCIe PME from the Thunderbolt controller becomes news for the driver.
//
// There is no SCI interrupt handler here: the status registers latch the events whether or not the interrupt is delivered, so poll() reads them. It also turns the machine into ACPI mode when
// the firmware has not (SCI_EN in PM1a_CNT, ACPI_ENABLE written to SMI_CMD, as acpi_enable does it), which is what makes the chipset route events to the GPE registers.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AcpiHost.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The I/O ports of the event registers.
class IAcpiPorts {
 public:
  virtual uint8_t in8(uint16_t port) = 0;
  virtual void out8(uint16_t port, uint8_t value) = 0;
  virtual uint16_t in16(uint16_t port) = 0;
  virtual void out16(uint16_t port, uint16_t value) = 0;
  virtual void delay_us(uint32_t us) = 0;

 protected:
  IAcpiPorts() = default;
  ~IAcpiPorts() = default;
};

// The fields of the FADT ("FACP") the events use.
struct AcpiFadt {
  uint16_t sci_interrupt = 0;
  uint32_t smi_command = 0;
  uint8_t acpi_enable = 0, acpi_disable = 0;
  uint32_t pm1a_control = 0, gpe0_block = 0, gpe1_block = 0;
  uint8_t gpe0_length = 0, gpe1_length = 0, gpe1_base = 0;
};
bool acpi_parse_fadt(ConstByteView table, AcpiFadt* out);

class IAcpiNotifyListener {
 public:
  virtual void on_notify(acpi::Node* device, uint32_t value) = 0;

 protected:
  IAcpiNotifyListener() = default;
  ~IAcpiNotifyListener() = default;
};

class AcpiEvents {
 public:
  static constexpr uint32_t kMaxTables = 64, kMaxWake = 8, kMaxNotifies = 16, kMaxBlockBytes = 32;
  static constexpr uint16_t kScienable = 1;       // SCI_EN, bit 0 of PM1a_CNT
  struct WakeSource {
    uint32_t gpe = 0;
    uint8_t deepest_state = 0;                    // the deepest sleep state the device can wake the machine from
  };

  AcpiEvents(IAcpiHost& host, IAcpiPorts& ports, allocators::IAllocator& allocator) : bridge_(host), ports_(ports), vm_(allocator, bridge_) {}

  // Loads the definition blocks and reads the FADT. False without a DSDT, a FADT or any event register block (then no event hardware is touched). `tables` must stay valid.
  bool initialize(const ConstByteView* tables, uint32_t count, ConstByteView fadt);
  bool initialize(const AcpiTables& acpi);
  uint32_t object_count() const { return vm_.node_count(); }
  // The interpreter, for the caller that wants to read a value of the namespace.
  acpi::AmlInterpreter& interpreter() { return vm_; }
  const AcpiFadt& fadt() const { return fadt_; }

  // ACPI mode: SCI_EN set. Turns it on if the firmware left it off (false if it does not come on within a few seconds).
  bool acpi_mode();
  bool enable_acpi_mode();

  // The ACPI device of a PCI function: a root bridge's children are matched by _ADR (device in the high word, function in the low), and the bridges below by the secondary bus in their configuration space.
  acpi::Node* find_pci_device(const IPciBus& pci, PciAddress address);
  // Is `device` the node `ancestor` or below it? (A root port's notification is news for the devices behind it.)
  bool is_within(const acpi::Node* ancestor, const acpi::Node* device) const;

  // _PRW of the device: the event it raises to wake the machine and the deepest sleep state it works in.
  bool wake_source(acpi::Node* device, WakeSource* out);
  // Arms the device's wake event: _PSW(1) if the device has one, the event's status cleared and its enable bit set. False if it has no _PRW, the event is outside the register blocks, or the list is full.
  bool enable_wake(acpi::Node* device);
  void disable_wake(acpi::Node* device);
  uint32_t wake_count() const { return wake_count_; }

  void set_listener(IAcpiNotifyListener* listener) { listener_ = listener; }
  // Looks at the event registers and runs the methods of what is pending; the notifications they make go to the listener. Returns the number of events handled.
  uint32_t poll();
  uint32_t events_handled() const { return handled_; }
  uint32_t events_without_method() const { return unanswered_; }

 private:
  class Bridge final : public acpi::IAmlHost {
   public:
    explicit Bridge(IAcpiHost& host) : host_(host) {}
    bool region_read(const acpi::RegionAccess& access, uint64_t* value) override { return host_.region_read(access, value); }
    bool region_write(const acpi::RegionAccess& access, uint64_t value) override { return host_.region_write(access, value); }
    void stall_us(uint32_t us) override { host_.stall_us(us); }
    void sleep_ms(uint32_t ms) override { host_.sleep_ms(ms); }
    uint64_t timer_100ns() override { return host_.timer_100ns(); }
    void notify(acpi::Node* device, uint32_t value) override {
      if (count < kMaxNotifies) {
        nodes[count] = device;
        values[count] = value;
        ++count;
      }
    }
    IAcpiHost& host_;
    acpi::Node* nodes[kMaxNotifies] = {};
    uint32_t values[kMaxNotifies] = {};
    uint32_t count = 0;
  };

  struct Wake {
    acpi::Node* device = nullptr;
    uint32_t gpe = 0;
  };

  bool locate(uint32_t gpe, uint16_t* status_port, uint16_t* enable_port, uint8_t* bit) const;
  bool find_in(acpi::Node* parent, const IPciBus& pci, uint8_t bus, PciAddress wanted, uint32_t depth, acpi::Node** found);
  void handle(uint32_t gpe, uint16_t status_port, uint16_t enable_port, uint8_t bit);
  void deliver();

  Bridge bridge_;
  IAcpiPorts& ports_;
  acpi::AmlInterpreter vm_;
  AcpiFadt fadt_;
  bool ready_ = false;
  Wake wake_[kMaxWake];
  uint32_t wake_count_ = 0;
  IAcpiNotifyListener* listener_ = nullptr;
  uint32_t handled_ = 0, unanswered_ = 0;
};

}  // namespace hardware
