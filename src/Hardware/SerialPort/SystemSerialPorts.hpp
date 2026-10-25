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

#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/SerialPort/ISerialPort.hpp"
#include "Hardware/SerialPort/Uart16550.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace hardware {

// Finds every serial port the machine has: the four legacy COM ports (a 16550 at 0x3F8, 0x2F8, 0x3E8 and 0x2E8) that answer. Ports are only
// found, not opened: a program picks one, calls open() with the framing it wants and then reads and writes. poll() moves the bytes of all
// of them along; it is meant to be called regularly from one thread.
//
// A program that uses a port holds share() (not the pointer at() hands out, which is only good while the list stays as it is).
class SystemSerialPorts {
 public:
  static constexpr uint64_t kPollIntervalUs = 1000;

  SystemSerialPorts(ISerialPortIo& io, allocators::IAllocator& allocator) : io_(io), allocator_(allocator), ports_(allocator) {}
  ~SystemSerialPorts() { shutdown(); }

  // Probes the legacy addresses.
  void initialize();
  // Adds a port found some other way (a USB adapter, a PCI card); it is initialized here. False if it is not there.
  bool add(SharedPointer<ISerialPort> port);
  void shutdown();

  uint32_t count() const;
  ISerialPort* at(uint32_t index) const;
  SharedPointer<ISerialPort> share(uint32_t index) const;

  // One round of the polling, for callers that service the ports themselves.
  void poll();
  // Runs poll() every millisecond from a thread ("serial"): a 16550's FIFO holds 16 bytes, which at 115200 baud is under 1.4 ms of line time.
  bool start_service();
  void stop_service();

 private:
  static void service_main(void* self);

  ISerialPortIo& io_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  List<SharedPointer<ISerialPort>> ports_;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace hardware
