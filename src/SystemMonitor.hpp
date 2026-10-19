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

// The computer manager's view of the running machine: the kernel's threads and cores and the allocator's memory.
#pragma once
#include "Allocators/Allocator.hpp"
#include "Allocators/ThreadSafeAllocator.hpp"
#include "Desktop/ComputerManager.hpp"
#include "Hardware/Battery/AcpiBattery.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

class SystemMonitor final : public desktop::ISystemMonitor {
 public:
  // `shared` is the thread-safe front of `heap`; reading the heap's statistics takes its lock.
  SystemMonitor(allocators::Allocator& heap, allocators::ThreadSafeAllocator& shared) : heap_(heap), shared_(shared) {}

  uint32_t ticks_per_second() const override;
  uint32_t core_count() const override;
  bool core(uint32_t index, desktop::TaskCore* out) override;
  uint32_t threads(desktop::TaskThread* out, uint32_t capacity) override;
  desktop::TaskMemory memory() override;
  int32_t cpu_temperature_millicelsius() override;
  desktop::TaskGpu gpu() override;
  desktop::TaskBattery battery() override;

  // The graphics adapter to report on and the PCI bus (the AMD temperature sensor is behind it); both may be left out.
  void set_gpu(hardware::IGraphicsProcessingUnit* gpu) { gpu_ = gpu; }
  void set_pci(const hardware::IPciBus* pci) { pci_ = pci; }
  // The batteries, if the machine has any (nullptr: none).
  void set_battery(hardware::AcpiBattery* battery) { battery_ = battery; }

 private:
  allocators::Allocator& heap_;
  allocators::ThreadSafeAllocator& shared_;
  hardware::IGraphicsProcessingUnit* gpu_ = nullptr;
  const hardware::IPciBus* pci_ = nullptr;
  hardware::AcpiBattery* battery_ = nullptr;
};
