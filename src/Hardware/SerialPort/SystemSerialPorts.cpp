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

#include "Hardware/SerialPort/SystemSerialPorts.hpp"

namespace hardware {

void SystemSerialPorts::initialize() {
  static constexpr uint16_t kBases[] = {Uart16550::kCom1, Uart16550::kCom2, Uart16550::kCom3, Uart16550::kCom4};
  static constexpr const char* kNames[] = {"COM1", "COM2", "COM3", "COM4"};
  for (uint32_t i = 0; i < 4; ++i) {
    auto uart = SharedPointer<Uart16550>::make(allocator_, allocator_, io_, kBases[i], kNames[i]);
    if (uart) add(SharedPointer<ISerialPort>(uart));
  }
}

bool SystemSerialPorts::add(SharedPointer<ISerialPort> port) {
  if (!port || !port->initialize()) return false;
  kernel::ScopedMutex guard(mutex_);
  if (ports_.push_back(port)) return true;
  port->shutdown();
  return false;
}

void SystemSerialPorts::shutdown() {
  stop_service();
  kernel::ScopedMutex guard(mutex_);
  for (uint64_t i = ports_.count(); i > 0; --i) ports_[i - 1]->shutdown();   // newest first
  ports_.clear();
}

uint32_t SystemSerialPorts::count() const {
  kernel::ScopedMutex guard(mutex_);
  return static_cast<uint32_t>(ports_.count());
}

ISerialPort* SystemSerialPorts::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < ports_.count() ? ports_[index].get() : nullptr;
}

SharedPointer<ISerialPort> SystemSerialPorts::share(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < ports_.count() ? ports_[index] : SharedPointer<ISerialPort>();
}

void SystemSerialPorts::poll() {
  List<SharedPointer<ISerialPort>> snapshot(allocator_);
  {
    kernel::ScopedMutex guard(mutex_);
    for (const SharedPointer<ISerialPort>& p : ports_.view()) snapshot.push_back(p);
  }
  for (const SharedPointer<ISerialPort>& p : snapshot.view()) p->poll();   // (outside the lock: polling touches hardware)
}

bool SystemSerialPorts::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "serial");
  return service_ != nullptr;
}

void SystemSerialPorts::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemSerialPorts::service_main(void* self) {
  SystemSerialPorts* ports = static_cast<SystemSerialPorts*>(self);
  while (!__atomic_load_n(&ports->stop_, __ATOMIC_ACQUIRE)) {
    ports->poll();
    kernel::sleep_us(kPollIntervalUs);
  }
}

}  // namespace hardware
