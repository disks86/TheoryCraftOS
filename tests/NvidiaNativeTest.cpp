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

// Host-side tests for NvidiaNative, the glue that puts the Nvkm parts together for NvidiaGpu: the read-only start (BAR0 mapped, the chip from
// BOOT_0, nothing written), the GPUs it leaves alone (powered down, unknown, no mapper), the BAR1 VRAM heap of the display takeover, and the
// takeover's refusals before any register is written (Ampere and later, a firmware framebuffer outside BAR1, no room).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <vector>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaNative.hpp"
#include "Hardware/RegisterMapper.hpp"

namespace hardware {
void delay_us(uint64_t) {}   // (the drivers' pauses: nothing to wait for against host memory)
}  // namespace hardware

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

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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
    *region = MmioRegion(physical, length);
    return true;
  }
};

static GpuContext context_for(FakeCard& card, TestAllocator& a) {
  GpuContext c{};
  c.st = nullptr;
  c.pci = &card;
  c.device.vendor_id = 0x10DE;
  c.device.device_id = 0x1180;
  c.allocator = &a;
  c.registers = &card;
  return c;
}

static void test_start() {
  TestAllocator a;
  {
    // A Kepler GK104 (chipset 0xe4): identified, nothing written to BAR0.
    FakeCard card(1u << 20);
    card.bar0[0] = 0x0e4000a1;
    const std::vector<uint32_t> before = card.bar0;
    NvidiaNative native(context_for(card, a), a);
    CHECK(native.start());
    CHECK(native.chip().family == Family::Kepler && native.chip().chipset == 0xe4 && native.chip().display_native && !native.chip().needs_gsp);
    CHECK(strcmp(native.status(), "identified") == 0);
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) changed += before[i] != card.bar0[i];
    CHECK(changed == 0);                                           // read-only
    CHECK(native.temperature_millicelsius() >= -1);               // (whatever the zeroed sensor says, no crash)
  }
  {
    // Powered down (all ones) or not answering at all (zero): left alone.
    FakeCard card(1u << 20);
    card.bar0[0] = 0xFFFFFFFFu;
    NvidiaNative native(context_for(card, a), a);
    CHECK(!native.start() && strstr(native.status(), "does not answer"));
    card.bar0[0] = 0;
    NvidiaNative zero(context_for(card, a), a);
    CHECK(!zero.start());
  }
  {
    // A chipset nobody knows; no register mapper; BAR0 that cannot be mapped.
    FakeCard card(1u << 20);
    card.bar0[0] = 0x1ff000a1;
    NvidiaNative native(context_for(card, a), a);
    CHECK(!native.start() && strstr(native.status(), "does not know"));
    GpuContext none = context_for(card, a);
    none.registers = nullptr;
    NvidiaNative unmapped(none, a);
    CHECK(!unmapped.start());
    card.map_ok = false;
    card.bar0[0] = 0x0e4000a1;
    NvidiaNative refused(context_for(card, a), a);
    CHECK(!refused.start() && strstr(refused.status(), "BAR0"));
  }
  CHECK(a.live == 0);
}

struct MutexLock final : INvLock {
  std::mutex m;
  void lock() override { m.lock(); }
  void unlock() override { m.unlock(); }
};

static void test_bar1_heap() {
  std::vector<uint8_t> window(8u << 20, 0xEE);
  MutexLock lock;
  Bar1VramMemory memory(ByteView(window.data(), window.size()), 2u << 20, 8u << 20, lock);
  CHECK(memory.init() && memory.vram_bytes() == (8u << 20));
  GpuMemory a = memory.allocate(MemoryTarget::Vram, 65536, 65536);
  CHECK(a.valid() && a.gpu_address >= (2u << 20) && a.gpu_address % 65536 == 0 && a.cpu.size() == 65536);
  CHECK(a.cpu.data() == window.data() + a.gpu_address && a.cpu[0] == 0 && a.cpu[65535] == 0);   // zeroed, at BAR1 + offset
  GpuMemory b = memory.allocate(MemoryTarget::Vram, 4096, 256);
  CHECK(b.valid() && (b.gpu_address + 4096 <= a.gpu_address || b.gpu_address >= a.gpu_address + 65536));
  CHECK(!memory.allocate(MemoryTarget::SystemCoherent, 4096, 256).valid());          // (no system memory here)
  CHECK(!memory.allocate(MemoryTarget::Vram, 7u << 20, 256).valid());                 // more than the window has
  CHECK(!memory.allocate(MemoryTarget::Vram, 0, 256).valid());
  memory.release(a);
  CHECK(!a.valid());
  GpuMemory c = memory.allocate(MemoryTarget::Vram, 65536, 65536);
  CHECK(c.valid());
  memory.release(c);
  memory.release(b);
  memory.release(b);                                                                   // (twice: nothing happens)
  Bar1VramMemory nothing(ByteView(window.data(), window.size()), 9u << 20, 8u << 20, lock);
  CHECK(!nothing.init());                                                              // the first offset past the window
}

static void test_engage_refusals() {
  TestAllocator a;
  {
    // Ampere (GA102, chipset 0x172): known, but its display is behind the GSP: refused, BAR0 untouched.
    FakeCard card(1u << 20);
    card.bar0[0] = 0x172000a1;
    NvidiaNative native(context_for(card, a), a);
    CHECK(native.start() && native.chip().needs_gsp && !native.chip().display_native);
    const std::vector<uint32_t> before = card.bar0;
    NvFirmwareFramebuffer fb;
    std::vector<uint8_t> pixels(1024 * 768 * 4);
    fb.pixels = ByteView(pixels.data(), pixels.size());
    fb.width = 1024;
    fb.height = 768;
    fb.pitch_bytes = 4096;
    ByteView memory;
    CHECK(!native.engage(fb, card.bar1_address(), &memory) && strstr(native.status(), "GSP") && !native.engaged());
    size_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) changed += before[i] != card.bar0[i];
    CHECK(changed == 0);
  }
  {
    // Kepler, but the firmware's framebuffer is not in BAR1, or there is no room after it in BAR1's first 256 MiB.
    FakeCard card(1u << 20);
    card.bar0[0] = 0x0e4000a1;
    NvidiaNative native(context_for(card, a), a);
    CHECK(native.start());
    std::vector<uint8_t> pixels(1024 * 768 * 4);
    NvFirmwareFramebuffer fb;
    fb.pixels = ByteView(pixels.data(), pixels.size());
    fb.width = 1024;
    fb.height = 768;
    fb.pitch_bytes = 4096;
    ByteView memory;
    CHECK(!native.engage(fb, card.bar1_address() - 4096, &memory) && strstr(native.status(), "not in BAR1"));
    CHECK(!native.engage(fb, card.bar1_address() + (250u << 20), &memory) && strstr(native.status(), "no room"));
    CHECK(!native.engaged());
    native.disengage();                                                                // (nothing engaged: nothing to do)
  }
  CHECK(a.live == 0);
}

int main() {
  test_start();
  test_bar1_heap();
  test_engage_refusals();
  printf("NvidiaNativeTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
