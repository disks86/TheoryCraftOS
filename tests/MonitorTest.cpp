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

// Host-side tests for the monitor layer: reading EDIDs (identity, size, every kind of timing, CTA extensions, damaged data), the layout of the
// monitors on the desktop, and SystemMonitors finding monitors through a fake display device, arranging them and following hot-plug.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "FakeMonitors.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

using namespace hardware;
using namespace fakemon;

[[noreturn]] void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  printf("BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

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

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static ConstByteView view(const Bytes& b) { return ConstByteView(b.data(), b.size()); }

static void test_edid() {
  EdidInfo e;
  const Bytes dell = dell_edid();
  CHECK(looks_like_edid(view(dell)) && parse_edid(view(dell), &e) && e.valid && e.checksum_ok);
  CHECK(strcmp(e.manufacturer, "DEL") == 0 && strcmp(edid_vendor_name(e.manufacturer), "Dell") == 0 && e.product_code == 0xA0C7 && e.serial_number == 0x12345678);
  CHECK(e.week == 10 && e.year == 2020 && e.version == 1 && e.revision == 4);
  CHECK(strcmp(e.name, "DELL U2720Q") == 0 && strcmp(e.serial_text, "ABC123") == 0);
  CHECK(e.digital && e.bits_per_color == 8 && e.width_mm == 600 && e.height_mm == 340 && e.gamma_x100 == 220 && e.srgb);
  CHECK(e.min_vertical_hz == 48 && e.max_vertical_hz == 86 && e.min_horizontal_khz == 30 && e.max_horizontal_khz == 140 && e.max_pixel_clock_mhz == 1080);
  CHECK(e.extension_blocks == 1 && e.has_audio && e.hdmi);
  // The modes: the detailed timing first and preferred; the others once each, however many places they were listed in.
  CHECK(e.timing_count == 8 && e.preferred == 0 && e.timings[0].preferred);
  CHECK(e.timings[0].width == 2560 && e.timings[0].height == 1440 && e.timings[0].pixel_clock_khz == 241500);
  CHECK(e.timings[0].refresh_millihz > 59900 && e.timings[0].refresh_millihz < 60000);
  CHECK(e.find_timing(1920, 1080, 60000) >= 0 && e.find_timing(1920, 1080, 50000) >= 0 && e.find_timing(1920, 1080, 60000) != e.find_timing(1920, 1080, 50000));
  CHECK(e.timings[e.find_timing(1920, 1080, 50000)].refresh_millihz == 50000);
  CHECK(e.find_timing(1280, 720) >= 0 && e.find_timing(3840, 2160) >= 0 && e.find_timing(640, 480) >= 0 && e.find_timing(1024, 768) >= 0 && e.find_timing(800, 600) >= 0);
  CHECK(e.find_timing(1366, 768) < 0);
  int count_720 = 0;
  for (uint32_t i = 0; i < e.timing_count; ++i) count_720 += e.timings[i].width == 1280 && e.timings[i].height == 720;
  CHECK(count_720 == 1);
  EdidTiming best;
  CHECK(e.best_timing(&best) && best.width == 2560);
  CHECK(e.diagonal_inches_x10() >= 270 && e.diagonal_inches_x10() <= 273 && e.dpi() >= 107 && e.dpi() <= 110);
  char name[64];
  edid_display_name(e, name, sizeof name);
  CHECK(strcmp(name, "DELL U2720Q") == 0);

  // Without the extension there is no HDMI, no audio and fewer modes.
  const Bytes plain = dell_edid(false);
  EdidInfo p;
  CHECK(parse_edid(view(plain), &p) && p.extension_blocks == 0 && !p.hdmi && !p.has_audio && p.timing_count == 6);

  // A wrong checksum is read but noted; a flipped extension checksum skips the extension; damaged headers and short data are not EDIDs.
  Bytes bad = dell;
  bad[100] ^= 0x55;
  CHECK(parse_edid(view(bad), &e) && !e.checksum_ok && e.timing_count == 8);
  bad = dell;
  bad[255] ^= 1;
  CHECK(parse_edid(view(bad), &e) && e.checksum_ok && !e.hdmi && e.timing_count == 6);
  bad = dell;
  bad[3] = 0;
  CHECK(!looks_like_edid(view(bad)) && !parse_edid(view(bad), &e) && !e.valid);
  CHECK(!parse_edid(ConstByteView(dell.data(), 127), &e) && !parse_edid(ConstByteView(), &e));
  bad = dell;
  bad[18] = 2;   // not EDID 1.x
  CHECK(!parse_edid(view(bad), &e));
  Bytes cut(dell.begin(), dell.begin() + 200);   // declares an extension that is not all there
  CHECK(parse_edid(view(cut), &e) && e.extension_blocks == 0 && e.timing_count == 6);
  // Garbage in the extension must not run off the block.
  bad = dell;
  bad[128 + 2] = 120;
  for (int i = 4; i < 120; ++i) bad[128 + i] = 0xE5;
  fix_checksum(&bad[128]);
  CHECK(parse_edid(view(bad), &e));
  bad = dell;
  bad[128] = 0x70;   // a DisplayID extension: left alone
  fix_checksum(&bad[128]);
  CHECK(parse_edid(view(bad), &e) && e.extension_blocks == 1 && e.timing_count == 6);

  // A bare minimum: header and checksum, nothing else.
  Bytes bare(128, 0);
  memcpy(bare.data(), dell.data(), 8);
  bare[18] = 1;
  bare[19] = 3;
  fix_checksum(bare.data());
  CHECK(parse_edid(view(bare), &e) && e.timing_count == 0 && !e.best_timing(&best) && e.name[0] == 0 && e.dpi() == 0 && e.diagonal_inches_x10() == 0);
  edid_display_name(e, name, sizeof name);
  CHECK(strcmp(name, "Monitor") != 0 || e.manufacturer[0] == 0);
  CHECK(strcmp(edid_vendor_name("ZZZ"), "ZZZ") == 0 && strcmp(edid_vendor_name("SAM"), "Samsung") == 0 && strcmp(edid_vendor_name("RHT"), "Red Hat") == 0);

  // An interlaced detailed timing (1080i: the numbers are per field) and the sizes in the descriptor when the header has none.
  Bytes i1080 = dell;
  i1080[21] = i1080[22] = 0;
  put_dtd(&i1080[54], 7425, 1920, 280, 540, 22, 509, 286, true);
  fix_checksum(i1080.data());
  CHECK(parse_edid(view(i1080), &e) && e.timings[0].width == 1920 && e.timings[0].height == 1080 && e.timings[0].interlaced && e.width_mm == 509 && e.height_mm == 286);
  CHECK(e.timings[0].refresh_millihz > 60000 && e.timings[0].refresh_millihz < 60100);
  // Old EDIDs (before 1.3) read the first standard timing aspect 00 as 1:1.
  Bytes old = dell;
  old[19] = 2;
  old[38] = 0x61;   // 1024
  old[39] = 0x00;
  fix_checksum(old.data());
  CHECK(parse_edid(view(old), &e));
  CHECK(e.find_timing(1024, 1024) >= 0);
  // A name made of the vendor alone.
  Bytes nameless = dell;
  nameless[72 + 3] = 0x10;
  fix_checksum(nameless.data());
  CHECK(parse_edid(view(nameless), &e) && e.name[0] == 0);
  edid_display_name(e, name, sizeof name);
  CHECK(strcmp(name, "Dell monitor") == 0);
  edid_display_name(e, name, 5);
  CHECK(strcmp(name, "Dell") == 0);
}

// --- The layout -------------------------------------------------------------------------------------

static void test_layout() {
  TestAllocator a;
  {
    MonitorLayout l(a);
    CHECK(l.count() == 0 && l.primary() == nullptr && l.bounds().empty() && l.monitor_at(0, 0) < 0 && l.nearest(0, 0) < 0);
    CHECK(l.add(7, 1920, 1080) && l.add(3, 1280, 1024) && l.add(9, 800, 600));
    CHECK(!l.add(3, 100, 100) && !l.add(5, 0, 100));
    CHECK(l.count() == 3 && l.at(0).rect == MonitorRect({0, 0, 1920, 1080}) && l.at(1).rect == MonitorRect({1920, 0, 1280, 1024}) && l.at(2).rect == MonitorRect({3200, 0, 800, 600}));
    CHECK(l.primary()->id == 7 && l.bounds() == MonitorRect({0, 0, 4000, 1080}));
    // Where a point is.
    CHECK(l.monitor_at(10, 10) == 0 && l.monitor_at(1920, 0) == 1 && l.monitor_at(3300, 500) == 2 && l.monitor_at(3300, 700) < 0 && l.monitor_at(4000, 0) < 0);
    CHECK(l.nearest(3500, 700) == 2 && l.nearest(-50, 2000) == 0 && l.nearest(2500, 1070) == 1);
    int32_t x = 3500, y = 700;
    CHECK(l.clamp(&x, &y) && x == 3500 && y == 599);
    x = 5000; y = -3;
    CHECK(l.clamp(&x, &y) && x == 3999 && y == 0);
    x = 100; y = 100;
    CHECK(l.clamp(&x, &y) && x == 100 && y == 100);
    CHECK(l.best_for({1800, 100, 400, 200}) == 0 || l.best_for({1800, 100, 400, 200}) == 1);
    CHECK(l.best_for({1900, 100, 400, 200}) == 1 && l.best_for({100, 100, 200, 200}) == 0 && l.best_for({9000, 0, 100, 100}) == 2);

    // Primary, and what happens to it.
    CHECK(l.set_primary(3) && l.primary()->id == 3 && !l.set_primary(99));
    CHECK(l.set_enabled(3, false) && l.primary()->id == 7);   // a primary that is unplugged: the first one there is
    CHECK(l.monitor_at(2000, 10) < 0 && l.nearest(2000, 10) == 0 && l.enabled_count() == 2 && l.bounds().width == 4000);   // (its place stays)
    CHECK(l.best_for({1920, 0, 100, 100}) == 0 || l.best_for({1920, 0, 100, 100}) == 2);
    CHECK(l.set_enabled(3, true));

    // Order and sizes: the row is packed again, tops aligned.
    CHECK(l.move_to_position(9, 0));
    CHECK(l.at(0).id == 9 && l.at(0).rect == MonitorRect({0, 0, 800, 600}) && l.at(1).id == 7 && l.at(1).rect.x == 800 && l.at(2).id == 3 && l.at(2).rect.x == 2720);
    CHECK(l.bounds().width == 4000 && l.bounds().height == 1080);   // (the same size, whatever the order)
    CHECK(l.move_to_position(9, 99) && l.at(2).id == 9 && l.at(2).rect.x == 3200);
    CHECK(l.resize(7, 2560, 1440) && l.bounds().width == 2560 + 1280 + 800 && l.bounds().height == 1440 && !l.resize(7, 0, 10));
    // Free placement and normalizing.
    CHECK(l.set_position(9, -500, -100) && l.bounds().x == -500 && l.bounds().y == -100);
    l.normalize();
    CHECK(l.bounds().x == 0 && l.bounds().y == 0 && l.at(l.find(9)).rect.x == 0 && l.at(l.find(9)).rect.y == 0);
    // Removing the primary hands it on.
    CHECK(l.set_primary(7) && l.remove(7) && l.count() == 2 && l.primary() != nullptr && l.find(7) < 0 && !l.remove(7));
    uint32_t primaries = 0;
    for (uint32_t i = 0; i < l.count(); ++i) primaries += l.at(i).primary;
    CHECK(primaries == 1);
    // A full layout.
    MonitorLayout many(a);
    for (uint32_t i = 0; i < MonitorLayout::kMaxMonitors; ++i) CHECK(many.add(i, 100, 100));
    CHECK(!many.add(99, 100, 100));
  }
  CHECK(a.live == 0);
}

// --- Finding monitors -----------------------------------------------------------------------------------

static void test_system_monitors() {
  TestAllocator a;
  {
    // A device that tells nothing: the screen is the one monitor.
    FakeGpu plain;
    SystemMonitors m(&plain, a);
    m.initialize();
    CHECK(m.count() == 1 && strcmp(m.monitor(0).name(), "Display") == 0 && m.layout().count() == 1 && m.layout().bounds() == MonitorRect({0, 0, 1024, 768}));
    CHECK(!m.multi_display() && !m.apply() && m.desktop_width() == 1024 && m.desktop_height() == 768);
    SystemMonitors none(nullptr, a);
    none.initialize();
    CHECK(none.count() == 0 && none.layout().count() == 0);

    // Two monitors and an empty connector: a row of the two, in connector order, each at its native size.
    FakeSource src;
    src.add("DP-1", dell_edid());
    src.add("HDMI-1", Bytes(), false, ConnectorType::Hdmi);
    src.add("HDMI-2", small_edid(), true, ConnectorType::Hdmi);
    FakeGpu gpu;
    gpu.source = &src;
    SystemMonitors sm(&gpu, a);
    sm.initialize();
    CHECK(sm.count() == 3 && sm.connected_count() == 2 && sm.multi_display() && sm.layout().count() == 2);
    CHECK(strcmp(sm.monitor(0).name(), "DELL U2720Q") == 0 && strcmp(sm.monitor(2).name(), "S24F350") == 0 && strcmp(sm.monitor(1).name(), "Display 2") == 0);
    CHECK(sm.monitor(0).connector_type() == ConnectorType::DisplayPort && strcmp(sm.monitor(0).connector_name(), "DP-1") == 0 && sm.monitor(0).edid() && !sm.monitor(1).edid());
    CHECK(sm.monitor(0).current_mode().width == 2560 && sm.monitor(2).current_mode().width == 1920 && sm.monitor(0).mode_count() == 8);
    CHECK(sm.layout().at(0).id == 0 && sm.layout().at(1).id == 2 && sm.layout().at(1).rect == MonitorRect({2560, 0, 1920, 1080}));
    CHECK(sm.apply() && sm.desktop_width() == 4480 && sm.desktop_height() == 1440);
    CHECK(src.applies == 1 && src.last_w == 4480 && src.last_h == 1440 && src.last.size() == 2);
    CHECK(src.last[0].connector == 0 && src.last[0].x == 0 && src.last[0].width == 2560 && src.last[1].connector == 2 && src.last[1].x == 2560 && src.last[1].height == 1080);

    // Reordering and the primary: the same desktop, other rectangles.
    CHECK(sm.layout().move_to_position(2, 0) && sm.layout().set_primary(2) && sm.reapply());
    CHECK(src.applies == 2 && src.last_w == 4480 && src.last[0].connector == 2 && src.last[0].x == 0 && src.last[1].connector == 0 && src.last[1].x == 1920);
    CHECK(sm.layout().primary()->id == 2 && sm.layout().bounds() == MonitorRect({0, 0, 4480, 1440}));

    // Hot-plug: the second monitor is pulled out - its place stays, it is shown no more; plugged in again it gets the place back.
    const uint32_t generation = sm.generation();
    src.connectors[2].info.connected = false;
    src.changed = true;
    CHECK(sm.poll() && sm.generation() > generation && sm.connected_count() == 1 && sm.layout().enabled_count() == 1 && sm.layout().count() == 2);
    CHECK(src.last.size() == 1 && src.last[0].connector == 0 && src.last_w == 4480 && sm.layout().primary()->id == 0);
    CHECK(!sm.poll());   // (nothing changed since)
    src.connectors[2].info.connected = true;
    src.changed = true;
    CHECK(sm.poll() && sm.layout().enabled_count() == 2 && src.last.size() == 2 && sm.waiting_for_restart() == 0);
    // A monitor that was not there at start-up has no room.
    src.add("HDMI-3", dell_edid(), true, ConnectorType::Hdmi);
    src.changed = true;
    CHECK(sm.poll() && sm.count() == 4 && sm.waiting_for_restart() == 1 && sm.layout().count() == 2 && src.last.size() == 2);
    src.connectors[3].info.connected = false;
    src.changed = true;
    CHECK(sm.poll() && sm.waiting_for_restart() == 0);

    // A device that refuses the layout stays a single display, the size it has.
    FakeSource refusing;
    refusing.add("A", dell_edid());
    refusing.add("B", small_edid());
    refusing.accepts = false;
    gpu.source = &refusing;
    SystemMonitors fallback(&gpu, a);
    fallback.initialize();
    CHECK(fallback.layout().count() == 2 && !fallback.apply());
    CHECK(fallback.layout().count() == 1 && fallback.layout().bounds() == MonitorRect({0, 0, 1024, 768}) && !fallback.multi_display() && fallback.desktop_width() == 1024);

    // A device that has several connectors but can show only one display.
    FakeSource single;
    single.add("A", dell_edid());
    single.add("B", small_edid());
    single.multi = false;
    gpu.source = &single;
    SystemMonitors one(&gpu, a);
    one.initialize();
    CHECK(one.count() == 2 && one.layout().count() == 1 && !one.multi_display() && !one.apply() && single.applies == 0);
    CHECK(one.layout().bounds() == MonitorRect({0, 0, 1024, 768}) && one.monitor(0).current_mode().width == 1024 && one.monitor(0).current_mode().height == 768);

    // Too much for the surface: monitors beyond the pixel budget are left out.
    FakeSource big;
    Bytes uhd = dell_edid(false);
    put_dtd(&uhd[54], 59400, 3840, 560, 2160, 90, 597, 336, false);
    fix_checksum(uhd.data());
    for (int i = 0; i < 3; ++i) big.add("DP", uhd);
    gpu.source = &big;
    SystemMonitors huge(&gpu, a);
    huge.initialize();
    CHECK(huge.connected_count() == 3 && huge.layout().count() == 2 && huge.layout().bounds().width == 7680);

    // Nothing connected: the first connector is shown anyway.
    FakeSource empty;
    empty.add("A", Bytes(), false);
    gpu.source = &empty;
    SystemMonitors none_connected(&gpu, a);
    none_connected.initialize();
    CHECK(none_connected.layout().count() == 1);
  }
  CHECK(a.live == 0);
}

int main() {
  test_edid();
  test_layout();
  test_system_monitors();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("monitor tests passed\n");
  return 0;
}
