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

#include "Hardware/EmbeddedController/ChromeEc/SystemChromeEc.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/HardwareMonitor/SystemHardwareMonitor.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

uint8_t SystemChromeEcPorts::in8(uint16_t port) { return IoPort(port).read8(); }
void SystemChromeEcPorts::out8(uint16_t port, uint8_t value) { IoPort(port).write8(value); }
void SystemChromeEcPorts::delay_us(uint32_t us) { hardware::delay_us(us); }

namespace {

SystemChromeEc* g_chrome_ec = nullptr;

// A device anywhere in the namespace (depth-limited, as the other ACPI walks) whose _HID or _CID is one of the Chrome EC's ids.
bool namespace_has_chrome_ec(acpi::AmlInterpreter& vm) {
  struct Finder {
    acpi::AmlInterpreter& vm;
    bool found = false;
    void visit(acpi::Node* node, uint32_t depth) {
      if (found || depth > 8) return;
      for (acpi::Node* child = vm.first_child(node); child && !found; child = vm.next_sibling(child)) {
        if (vm.is_device(child)) {
          for (const char* id : chromeec::kAcpiIds)
            if (vm.device_has_id(child, id)) found = true;
        }
        if (!found) visit(child, depth + 1);
      }
    }
  } finder{vm};
  kernel::ScopedRecursiveMutex guard(vm.mutex());
  finder.visit(vm.root(), 0);
  return finder.found;
}

}  // namespace

void SystemChromeEc::AmlLocks::lock() {
  for (uint32_t i = 0; i < count; ++i) ArrayView<kernel::RecursiveMutex*>(mutexes)[i]->lock();
}

void SystemChromeEc::AmlLocks::unlock() {
  for (uint32_t i = count; i > 0; --i) ArrayView<kernel::RecursiveMutex*>(mutexes)[i - 1]->unlock();
}

SystemChromeEc::~SystemChromeEc() {
  if (system_chrome_ec() == this) set_system_chrome_ec(nullptr);
  if (SystemHardwareMonitor* board = system_hardware_monitor())
    if (stack_.monitor() && board->embedded_controller() == stack_.monitor()) board->set_embedded_controller(nullptr);
}

bool SystemChromeEc::probe(acpi::AmlInterpreter* vm) {
  found_in_acpi_ = vm && namespace_has_chrome_ec(*vm);
  IEcFirmwareLock* lock = nullptr;
  if (found_in_acpi_) {
    one_.mutexes[0] = &vm->mutex();
    one_.count = 1;
    lock = &one_;
  }
  if (!stack_.start(found_in_acpi_, lock)) return false;
  if (SystemHardwareMonitor* board = system_hardware_monitor())
    if (ChromeEcHardwareMonitor* monitor = stack_.monitor()) board->set_embedded_controller(monitor);
  set_system_chrome_ec(this);
  return true;
}

void SystemChromeEc::add_interpreter(acpi::AmlInterpreter& vm) {
  if (!found_in_acpi_ || !stack_.ec() || one_.count != 1 || two_.count != 0 || one_.mutexes[0] == &vm.mutex()) return;
  two_.mutexes[0] = one_.mutexes[0];
  two_.mutexes[1] = &vm.mutex();
  two_.count = 2;
  stack_.ec()->set_firmware_lock(&two_);   // (swapped under the EC's mutex: no transaction is half-way through the old set)
}

SystemChromeEc* system_chrome_ec() { return __atomic_load_n(&g_chrome_ec, __ATOMIC_ACQUIRE); }

void set_system_chrome_ec(SystemChromeEc* ec) { __atomic_store_n(&g_chrome_ec, ec, __ATOMIC_RELEASE); }

}  // namespace hardware
