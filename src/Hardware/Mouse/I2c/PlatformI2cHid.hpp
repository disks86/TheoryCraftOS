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


// The touchpad and the touchscreen of a machine like the Dell XPS 15 9575, found the way an operating system finds them. The ACPI tables describe each as a device with the ID
// PNP0C50 / ACPI0C50 (HID over I2C): its _CRS holds the I2C serial bus connection (the address on the bus, the speed, and the path of the controller it is behind) and the GPIO
// interrupt connection (the pin of the platform's GPIO controller that goes low when the device has a report, and the path of that controller); its _DSM (the HID-over-I2C UUID,
// function 1) returns the register of the HID descriptor. The controller is an Intel LPSS I2C function on PCI (named by the _ADR of the path's device), the GPIO controller the
// Sunrise Point-LP one (INT344B) whose memory ranges are its pad communities.
//
// start() runs the AML needed (_STA, _CRS, _DSM, _PS0 of the device, SSCN / FMCN of the controller), brings up the controllers and the GPIO pads, the devices, and adds each pointer
// it got to the system's mouse. What it could not do is told in the reports. Not done: interrupts (the pins are polled), keyboards and sensors on the same links (HID over I2C
// devices of other kinds are left alone), power management, devices behind other GPIO controllers.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AcpiHost.hpp"
#include "Hardware/Acpi/AcpiResources.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Bus/I2c/IntelGpio.hpp"
#include "Hardware/Bus/I2c/LpssI2c.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Mouse/CompositeMouse.hpp"
#include "Hardware/Mouse/I2c/I2cHidPointer.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class PlatformI2cHid {
 public:
  enum class Outcome : uint8_t { Started, NoController, ControllerFailed, NoDescriptorAddress, DeviceFailed, NotAPointer, MouseFull };
  struct Report {
    char device[32] = {};            // "TPD0 ELAN0501"
    Outcome outcome = Outcome::DeviceFailed;
    I2cHidPointer::Kind kind = I2cHidPointer::Kind::None;
  };
  static constexpr uint32_t kMaxReports = 8;
  static const char* outcome_name(Outcome o);

  PlatformI2cHid(allocators::IAllocator& allocator, IPciBus& pci, II2cRegisterSource& i2c_registers, IRegisterMapper& mapper, IClock& clock, IAcpiHost& host)
      : allocator_(allocator), pci_(pci), i2c_registers_(i2c_registers), mapper_(mapper), clock_(clock), vm_(allocator, host), controllers_(allocator), gpios_(allocator), lines_(allocator), pointers_(allocator) {}

  // Loads the tables, finds the devices, brings them up. Returns how many pointers were added to `mice`.
  uint32_t start(const AcpiTables& tables, CompositeMouse& mice);
  // Loads the tables given (for tests).
  uint32_t start(const ConstByteView* tables, uint32_t count, CompositeMouse& mice);
  void stop(CompositeMouse& mice);

  uint32_t report_count() const { return report_count_; }
  const Report& report(uint32_t i) const { return reports_[i < kMaxReports ? i : 0]; }
  uint32_t tables_loaded() const { return tables_loaded_; }

 private:
  struct Controller {
    LpssI2cController lpss;
    acpi::Node* node = nullptr;
  };
  struct Gpio {
    IntelPchGpio pads;
    acpi::Node* node = nullptr;
  };
  struct Pointer {
    char name[40] = {};
    UniquePointer<I2cHidPointer> pointer;
  };

  uint32_t run(CompositeMouse& mice);
  void visit(acpi::Node* node, uint32_t depth, CompositeMouse& mice, uint32_t* added);
  bool setup_device(acpi::Node* device, CompositeMouse& mice, Report* report);
  Controller* controller_for(acpi::Node* node, const AcpiResources::I2c& i2c, Outcome* outcome);
  IntelPchGpio* gpio_for(acpi::Node* node);
  acpi::Node* child(acpi::Node* parent, const char* name) const;
  bool integer_of(acpi::Node* node, const char* name, uint64_t* out);
  bool hid_string(acpi::Node* node, char out[16]);
  void timing(acpi::Node* node, const char* method, uint32_t* hcnt, uint32_t* lcnt, uint32_t* sda);

  allocators::IAllocator& allocator_;
  IPciBus& pci_;
  II2cRegisterSource& i2c_registers_;
  IRegisterMapper& mapper_;
  IClock& clock_;
  acpi::AmlInterpreter vm_;
  List<UniquePointer<Controller>> controllers_;
  List<UniquePointer<Gpio>> gpios_;
  List<UniquePointer<GpioAttentionLine>> lines_;
  List<UniquePointer<Pointer>> pointers_;
  Report reports_[kMaxReports];
  uint32_t report_count_ = 0, tables_loaded_ = 0;
};

}  // namespace hardware
