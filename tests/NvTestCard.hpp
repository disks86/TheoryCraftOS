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

// The NVIDIA card of the host tests that run the native driver's start (NvidiaNativeTest, NvidiaSecondaryGpuTest): an allocator that counts
// what is live, BAR0 and BAR1 as host memory behind a fake PCI bus and register mapper, and the GPU context for it.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Hardware/RegisterMapper.hpp"

using namespace hardware;
using namespace hardware::nv;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// A card: BAR0 as host memory (writes counted by comparing with a copy), BAR1 as host memory.
struct FakeCard final : IPciBus, IRegisterMapper {
  std::vector<uint32_t> bar0 = std::vector<uint32_t>(MmioNvRegisters::kBar0Bytes / 4, 0);
  std::vector<uint8_t> bar1;
  bool map_ok = true;
  uint32_t maps = 0;
  explicit FakeCard(uint64_t bar1_bytes) : bar1(bar1_bytes, 0) {}
  uint64_t bar0_address() const { return reinterpret_cast<uint64_t>(bar0.data()); }
  uint64_t bar1_address() const { return reinterpret_cast<uint64_t>(bar1.data()); }
  // IPciBus
  const char* name() const override { return "fake card"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t) const override { return 0; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t index) const override { return index == 0 ? bar0_address() : index == 1 ? bar1_address() : 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
  // IRegisterMapper
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    ++maps;
    if (!map_ok) return false;
    // Only the card's own BARs exist here (the legacy shadow at 0xC0000, which the fetch also asks for, is not host memory).
    auto inside = [&](uint64_t base, uint64_t bytes) { return physical >= base && length <= bytes && physical - base <= bytes - length; };
    if (!inside(bar0_address(), bar0.size() * 4) && !inside(bar1_address(), bar1.size())) return false;
    *region = MmioRegion(physical, length);
    return true;
  }
};

inline GpuContext context_for(FakeCard& card, TestAllocator& a) {
  GpuContext c{};
  c.st = nullptr;
  c.pci = &card;
  c.device.vendor_id = 0x10DE;
  c.device.device_id = 0x1180;
  c.allocator = &a;
  c.registers = &card;
  return c;
}
