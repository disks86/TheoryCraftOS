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
#include "NvBiosImages.hpp"
#include "NvTestCard.hpp"

namespace hardware {
void delay_us(uint64_t) {}   // (the drivers' pauses: nothing to wait for against host memory)
}  // namespace hardware

using namespace hardware;
using namespace hardware::nv;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

// The video BIOS read at start through the real fetch (the PROM source here: the image where BAR0 0x300000 shows the ROM), the board's
// connectors listed from it, and the registers the fetch touches put back as they were.
static void test_bios_and_connectors() {
  TestAllocator a;
  {
    FakeCard card(1u << 20);
    card.bar0[0] = 0x0e4000a1;                                    // GK104
    const Bytes image = kepler_image(false);
    memcpy(reinterpret_cast<uint8_t*>(card.bar0.data()) + 0x300000, image.data(), image.size());
    card.bar0[0x088050 / 4] = 1;                                  // the firmware left the ROM shadow on
    card.bar0[0x001700 / 4] = 0x1234;                             // and the PRAMIN window here
    NvidiaNative native(context_for(card, a), a);
    CHECK(native.start());
    CHECK(native.bios_source() == BiosSource::Prom && native.board_result().has_dcb);
    CHECK(native.board().output_count >= 4 && native.board().connector_count == 3 && native.board().i2c_count >= 3);
    CHECK(card.bar0[0x088050 / 4] == 1 && card.bar0[0x001700 / 4] == 0x1234);   // put back
    CHECK(native.connector_count() == 3);
    const NvidiaNative::Connector* dp = native.connector(0);
    const NvidiaNative::Connector* hdmi = native.connector(1);
    const NvidiaNative::Connector* dvi = native.connector(2);
    CHECK(dp && dp->dcb_type == 0x46 && dp->displayport && hdmi && hdmi->dcb_type == 0x61 && !hdmi->displayport && dvi && dvi->dcb_type == 0x30);
    CHECK(!dp->connected && dp->edid_bytes == 0);                 // (nothing answers on these buses: no monitor)
    CHECK(!native.connector(3) && native.connectors().size() == 3);
    CHECK(!native.refresh_connectors());                          // nothing changed
  }
  {
    // No image anywhere: no board tables, no connectors; the chip and the rest still work.
    FakeCard card(1u << 20);
    card.bar0[0] = 0x0e4000a1;
    NvidiaNative native(context_for(card, a), a);
    CHECK(native.start() && native.bios_source() == BiosSource::None && native.connector_count() == 0 && native.board().connector_count == 0);
  }
  CHECK(a.live == 0);
}

static void test_connector_helpers() {
  CHECK(connector_type_for_dcb(0x46) == ConnectorType::DisplayPort && connector_type_for_dcb(0x61) == ConnectorType::Hdmi &&
        connector_type_for_dcb(0x30) == ConnectorType::Dvi && connector_type_for_dcb(0x47) == ConnectorType::Internal &&
        connector_type_for_dcb(0x00) == ConnectorType::Vga && connector_type_for_dcb(0xEE) == ConnectorType::Unknown);
  char name[16];
  connector_label(ConnectorType::DisplayPort, 2, ArrayView<char>(name));
  CHECK(strcmp(name, "DP-2") == 0);
  connector_label(ConnectorType::Internal, 1, ArrayView<char>(name));
  CHECK(strcmp(name, "eDP-1") == 0);
  connector_label(ConnectorType::Hdmi, 12, ArrayView<char>(name));
  CHECK(strcmp(name, "HDMI-12") == 0);
  char tiny[4];
  connector_label(ConnectorType::DisplayPort, 123, ArrayView<char>(tiny));
  CHECK(strlen(tiny) == 3);                                        // cut to fit, terminated

  // Order: the firmware's display (its EDID) first, then the other connected ones, then the rest.
  static NvidiaNative::Connector list[4];
  for (auto& c : list) c = NvidiaNative::Connector();
  list[0].connected = false;
  list[1].connected = true;
  list[1].edid_bytes = 128;
  memset(list[1].edid, 0x11, 128);
  list[2].connected = true;
  list[2].edid_bytes = 128;
  memset(list[2].edid, 0x22, 128);
  list[3].connected = false;
  uint8_t firmware[128];
  memset(firmware, 0x22, sizeof firmware);
  uint8_t order[8];
  CHECK(order_connectors(ArrayView<const NvidiaNative::Connector>(list), ConstByteView(firmware), ArrayView<uint8_t>(order)) == 4);
  CHECK(order[0] == 2 && order[1] == 1 && order[2] == 0 && order[3] == 3);
  CHECK(order_connectors(ArrayView<const NvidiaNative::Connector>(list), ConstByteView(), ArrayView<uint8_t>(order)) == 4);
  CHECK(order[0] == 1 && order[1] == 2 && order[2] == 0);           // (no firmware EDID: the connected ones in the board's order)
  memset(firmware, 0x33, sizeof firmware);
  CHECK(order_connectors(ArrayView<const NvidiaNative::Connector>(list), ConstByteView(firmware), ArrayView<uint8_t>(order)) == 4 && order[0] == 1);
  uint8_t two[2];
  CHECK(order_connectors(ArrayView<const NvidiaNative::Connector>(list), ConstByteView(), ArrayView<uint8_t>(two)) == 2);   // cut to fit
}

int main() {
  test_start();
  test_bar1_heap();
  test_engage_refusals();
  test_bios_and_connectors();
  test_connector_helpers();
  printf("NvidiaNativeTest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
