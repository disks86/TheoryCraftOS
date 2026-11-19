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

// Host-side tests for a desktop that spans the monitors of several GPUs: SystemMonitors with the display device and other GPUs (ids and order,
// the default layout, each GPU's own surface, mode changes through the right driver, a GPU without a monitor source, hot plug), and MonitorDisplay
// copying each monitor's part of the desktop to the screen of the GPU that drives it (pixels, damage only, presents only where drawn), also while
// another thread draws on that screen under its guard. A muxed laptop's built-in panel moving between the GPUs (SystemMonitors::move_panel): its
// monitor leaves one GPU's monitors and joins the other's in the same place, at the same scale, still primary, and the next frame is copied to the
// GPU that drives it now; and back.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "FakeMonitors.hpp"
#include "Hardware/Monitor/MonitorDisplay.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

using namespace hardware;
using namespace fakemon;

[[noreturn]] void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  printf("BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return aligned_alloc(64, (size + 63) / 64 * 64); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (!size) return nullptr;
    ++live;
    if (alignment < 64) alignment = 64;
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static uint32_t pixel_of(uint32_t x, uint32_t y) { return 0xFF000000u | (y << 14) | x; }
static std::vector<uint32_t> desktop_picture(uint32_t w, uint32_t h) {
  std::vector<uint32_t> p(size_t{w} * h);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) p[size_t{y} * w + x] = pixel_of(x, y);
  return p;
}

// A GPU without a monitor source, with modes of its own (set_mode() switches the screen); it notices when two threads draw on it at once.
struct ModeGpu final : IGraphicsProcessingUnit {
  std::vector<DisplayMode> modes = {{1024, 768}, {1280, 720}, {800, 600}};
  uint32_t mode = 0;
  int modes_set = 0;
  std::vector<uint32_t> screen;
  std::atomic<int> blits{0}, presents{0}, inside{0};
  std::atomic<bool> overlapped{false};
  const char* name() const override { return "second gpu"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return static_cast<uint32_t>(modes.size()); }
  bool mode_info(uint32_t i, DisplayMode* out) const override { if (i >= modes.size()) return false; *out = modes[i]; return true; }
  uint32_t current_mode() const override { return mode; }
  bool set_mode(uint32_t i) override {
    if (i >= modes.size()) return false;
    mode = i;
    ++modes_set;
    screen.assign(size_t{width()} * height(), 0);
    return true;
  }
  uint32_t width() const override { return modes[mode].width; }
  uint32_t height() const override { return modes[mode].height; }
  void put_pixel(uint32_t, uint32_t, uint32_t) override {}
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t) override {}
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
  void enter() { if (inside.fetch_add(1) != 0) overlapped = true; }
  void leave() { inside.fetch_sub(1); }
  void blit(uint32_t x, uint32_t y, uint32_t bw, uint32_t bh, const uint32_t* pixels, uint32_t pitch) override {
    enter();
    ++blits;
    const uint32_t w = width(), h = height();
    if (screen.size() != size_t{w} * h) screen.assign(size_t{w} * h, 0);
    for (uint32_t j = 0; j < bh; ++j)
      for (uint32_t i = 0; i < bw; ++i)
        if (x + i < w && y + j < h) screen[size_t{y + j} * w + x + i] = pixels[size_t{j} * pitch + i];
    leave();
  }
  void present() override {
    enter();
    ++presents;
    leave();
  }
};

struct MutexGuard final : IScreenGuard {
  std::mutex mutex;
  void acquire() override { mutex.lock(); }
  void release() override { mutex.unlock(); }
};

static void sized(FakeSource& src, uint32_t index, uint32_t w, uint32_t h) {
  src.connectors[index].info.preferred_width = w;
  src.connectors[index].info.preferred_height = h;
}

// The display device alone behaves as before: adding no GPU changes nothing, and the display device is refused as a second GPU.
static void test_one_gpu() {
  TestAllocator a;
  {
    FakeSource src;
    src.add("DP-1", dell_edid());
    src.add("HDMI-1", Bytes(), false, ConnectorType::Hdmi);
    src.add("HDMI-2", small_edid(), true, ConnectorType::Hdmi);
    FakeGpu gpu;
    gpu.source = &src;
    SystemMonitors sm(&gpu, a);
    CHECK(!sm.add_gpu(&gpu, 1) && !sm.add_gpu(nullptr, 1) && sm.gpu_count() == 1);
    sm.initialize();
    CHECK(sm.count() == 3 && sm.layout().count() == 2 && sm.layout().at(0).id == 0 && sm.layout().at(1).id == 2 && !sm.spans_gpus());
    CHECK(sm.monitor(0).gpu() == 0 && sm.gpu_of(2) == 0 && sm.device_of(2) == &gpu && strcmp(sm.device_name(0), "fake gpu") == 0);
    MonitorTarget targets[4];
    CHECK(sm.targets(targets, 4) == 0);
    CHECK(sm.apply() && !sm.needs_compositing() && src.last.size() == 2 && src.last[1].connector == 2 && src.last[1].x == 2560 && sm.surface_width() == 4480);
    // MonitorDisplay with no targets: everything goes to the display device.
    MonitorDisplay display(gpu, a);
    ScanoutPlacement places[4];
    const uint32_t n = sm.physical_placements(places, 4);
    CHECK(display.update(sm.layout(), places, n, 100, ArrayView<const MonitorTarget>(targets, 0)) && display.target_count() == 0 && !display.scaling());
  }
  CHECK(a.live == 0);
}

// The display device (two monitors and an empty connector) and a second GPU (two monitors): ids, order, the default row and each GPU's surface.
static void test_two_gpus_layout() {
  TestAllocator a;
  {
    FakeSource src;
    src.add("DP-1", dell_edid());
    src.add("HDMI-1", Bytes(), false, ConnectorType::Hdmi);
    src.add("HDMI-2", small_edid(), true, ConnectorType::Hdmi);
    FakeGpu gpu;
    gpu.source = &src;
    FakeSource second;
    second.add("Virtual-0", small_edid(), true, ConnectorType::Virtual);
    second.add("Virtual-1", Bytes(), true, ConnectorType::Virtual);
    sized(second, 1, 1280, 1024);
    FakeGpu other;
    other.source = &second;
    SystemMonitors sm(&gpu, a);
    sm.set_display_gpu_index(2);
    CHECK(sm.add_gpu(&other, 5) && !sm.add_gpu(&other, 5) && sm.gpu_count() == 2);
    sm.initialize();
    CHECK(sm.count() == 5 && sm.connected_count() == 4 && sm.layout().count() == 4);
    // The display device's monitors first, with their ids; then the other GPU's at 16 + connector.
    CHECK(sm.monitor(0).id() == 0 && sm.monitor(2).id() == 2 && sm.monitor(3).id() == 16 && sm.monitor(4).id() == 17);
    CHECK(sm.monitor(0).gpu() == 2 && sm.monitor(3).gpu() == 5 && sm.gpu_of(17) == 5 && sm.device_of(16) == &other && sm.device_of(1) == &gpu);
    CHECK(strcmp(sm.monitor(3).name(), "S24F350") == 0 && strcmp(sm.monitor(4).name(), "Display 5") == 0 && strcmp(sm.monitor(1).name(), "Display 2") == 0);
    CHECK(sm.layout().at(0).id == 0 && sm.layout().at(1).id == 2 && sm.layout().at(2).id == 16 && sm.layout().at(3).id == 17);
    CHECK(sm.layout().at(2).rect == MonitorRect({4480, 0, 1920, 1080}) && sm.layout().at(3).rect == MonitorRect({6400, 0, 1280, 1024}));
    CHECK(sm.spans_gpus() && sm.needs_compositing());
    CHECK(sm.apply() && sm.desktop_width() == 7680 && sm.desktop_height() == 1440);
    // Each GPU was given its own monitors only, by connector, in a row from 0.
    CHECK(src.last.size() == 2 && src.last[0].connector == 0 && src.last[1].connector == 2 && src.last_w == 4480 && sm.surface_width() == 4480);
    CHECK(second.applies == 1 && second.last.size() == 2 && second.last[0].connector == 0 && second.last[0].x == 0 && second.last[1].connector == 1 &&
          second.last[1].x == 1920 && second.last_w == 3200 && second.last_h == 1080);
    CHECK(sm.surface_width_of(16) == 3200 && sm.surface_height_of(17) == 1080 && sm.surface_width_of(0) == 4480);
    ScanoutPlacement places[8];
    CHECK(sm.physical_placements(places, 8) == 4 && places[2].connector == 16 && places[2].x == 0 && places[3].connector == 17 && places[3].x == 1920);
    MonitorTarget targets[8];
    CHECK(sm.targets(targets, 8) == 2 && targets[0].id == 16 && targets[0].gpu == &other && targets[0].gpu_index == 5 && targets[1].id == 17);

    // Arranged like any other monitor: the second GPU's last monitor moved left of the display device's, and made the primary.
    CHECK(sm.move_monitor(17, -1280, 0) && sm.layout().set_primary(17) && sm.reapply());
    CHECK(sm.layout().at(sm.layout().find(17)).rect.x == 0 && sm.layout().primary()->id == 17);
    CHECK(second.applies == 2 && second.last[0].connector == 1 && second.last[0].x == 0 && second.last[1].connector == 0 && second.last[1].x == 1280);
    CHECK(src.last[0].connector == 0 && src.last[0].x == 0);
    CHECK(sm.physical_placements(places, 8) == 4 && places[0].connector == 17 && places[0].x == 0 && places[1].connector == 0 && places[1].x == 0);
    // Scales work across GPUs too.
    CHECK(sm.set_scale(17, 200) && sm.layout().at(sm.layout().find(17)).rect.width == 640 && sm.apply());

    // Hot plug on the second GPU: pulled out, its place stays; a new connector is added to the right and shown at once.
    second.connectors[0].info.connected = false;
    second.changed = true;
    CHECK(sm.poll() && sm.layout().enabled_count() == 3 && second.last.size() == 1 && second.last[0].connector == 1);
    CHECK(sm.targets(targets, 8) == 1 && targets[0].id == 17);
    second.connectors[0].info.connected = true;
    second.add("Virtual-2", Bytes(), true, ConnectorType::Virtual);
    sized(second, 2, 1024, 768);
    second.changed = true;
    CHECK(sm.poll() && sm.count() == 6 && sm.monitor(5).id() == 18 && sm.monitor(5).gpu() == 5 && sm.layout().count() == 5 && sm.waiting_for_restart() == 0);
    CHECK(second.last.size() == 3 && second.last_w == 1920 + 1280 + 1024 && sm.targets(targets, 8) == 3);

    // A second GPU that refuses the layout shows its first monitor only, at the size its screen has; the display device is unaffected.
    FakeSource refusing;
    refusing.add("A", small_edid());
    refusing.add("B", small_edid());
    refusing.accepts = false;
    FakeGpu third;
    third.source = &refusing;
    third.w = 800;
    third.h = 600;
    SystemMonitors sm2(&gpu, a);
    CHECK(sm2.add_gpu(&third, 1));
    sm2.initialize();
    CHECK(sm2.layout().count() == 4 && !sm2.apply());
    CHECK(sm2.layout().count() == 3 && sm2.layout().find(16) >= 0 && sm2.layout().find(17) < 0 && sm2.layout().at(sm2.layout().find(16)).physical_width == 800);
    CHECK(sm2.layout().connected() && sm2.desktop_width() == 4480 + 800 && src.last.size() == 2);
  }
  CHECK(a.live == 0);
}

// Mode changes on the second GPU go to its driver: a single-display monitor source switches the screen; a GPU without one sets one of its modes.
static void test_secondary_modes() {
  TestAllocator a;
  {
    FakeGpu gpu;   // the display device: no monitor source, the 1024x768 screen
    FakeSource vga;
    vga.add("VGA-1", Bytes());
    vga.multi = false;
    for (uint32_t w : {1024u, 1280u, 1920u}) {
      MonitorMode m;
      m.width = w;
      m.height = w == 1024 ? 768 : w == 1280 ? 720 : 1080;
      vga.offered.push_back(m);
    }
    FakeGpu second;
    second.source = &vga;
    second.w = 1024;
    second.h = 768;
    ModeGpu bare;   // no monitor source at all: the screen is one monitor
    SystemMonitors sm(&gpu, a);
    CHECK(sm.add_gpu(&second, 1) && sm.add_gpu(&bare, 2));
    sm.initialize();
    CHECK(sm.count() == 3 && sm.layout().count() == 3 && sm.monitor(0).id() == 0 && sm.monitor(1).id() == 16 && sm.monitor(2).id() == 32);
    CHECK(strcmp(sm.monitor(2).name(), "Display") == 0 && sm.monitor(2).gpu() == 2 && sm.monitor(2).current_mode().width == 1024 && !sm.monitor(2).edid());
    CHECK(sm.layout().at(1).rect == MonitorRect({1024, 0, 1024, 768}) && sm.layout().at(2).rect == MonitorRect({2048, 0, 1024, 768}));
    CHECK(sm.apply() && vga.display_modes_set == 0 && bare.modes_set == 0 && sm.desktop_width() == 3072);
    // The display device without a monitor source still offers no modes (as with one GPU); the others offer their driver's.
    MonitorMode modes[8];
    CHECK(sm.compatible_resolutions(0, modes, 8) == 1 && !sm.set_resolution(0, 1280, 720));
    CHECK(sm.compatible_resolutions(16, modes, 8) == 3 && sm.compatible_resolutions(32, modes, 8) == 3 && !sm.can_set_refresh(32));
    CHECK(sm.set_resolution(16, 1280, 720) && sm.set_resolution(32, 800, 600) && vga.display_modes_set == 0 && bare.modes_set == 0);
    CHECK(sm.apply() && vga.display_modes_set == 1 && vga.last_mode_w == 1280 && vga.last_mode_h == 720);
    CHECK(bare.modes_set == 1 && bare.width() == 800 && bare.height() == 600 && sm.surface_width_of(32) == 800);
    CHECK(sm.layout().connected() && sm.layout().at(sm.layout().find(32)).rect == MonitorRect({2048, 720, 800, 600}) && sm.desktop_width() == 2048 + 800);   // (moved next to the bigger one)
    // A mode the driver refuses: apply() says so (the caller puts the old setting back).
    CHECK(sm.set_resolution(16, 1920, 1080));
    vga.unavailable.push_back({1920, 1080});
    CHECK(!sm.apply());
    CHECK(!sm.set_resolution(32, 640, 400));   // (not among the driver's modes)
  }
  CHECK(a.live == 0);
}

// MonitorDisplay: the display device's monitor 800x400, the second GPU's 400x300 to its right (at 0, 0 of that GPU's own screen).
static void test_composition() {
  TestAllocator a;
  {
    FakeGpu display_gpu;
    display_gpu.w = 800;
    display_gpu.h = 400;
    ModeGpu second;
    second.modes = {{400, 300}};
    second.screen.assign(size_t{400} * 300, 0);
    MonitorDisplay display(display_gpu, a);
    MonitorLayout layout(a);
    CHECK(layout.add(0, 800, 400) && layout.add(16, 400, 300));
    ScanoutPlacement places[2] = {{0, 0, 0, 800, 400}, {16, 0, 0, 400, 300}};
    MonitorTarget target;
    target.id = 16;
    target.gpu = &second;
    target.gpu_index = 1;
    // A target without a driver is refused.
    MonitorTarget broken = target;
    broken.gpu = nullptr;
    CHECK(!display.update(layout, places, 2, 100, ArrayView<const MonitorTarget>(&broken, 1)));
    CHECK(display.update(layout, places, 2, 100, ArrayView<const MonitorTarget>(&target, 1)) && display.target_count() == 1 && display.width() == 1200 && display.height() == 400);
    std::vector<uint32_t> picture = desktop_picture(1200, 400);
    display.blit(0, 0, 1200, 400, picture.data(), 1200);
    CHECK(display_gpu.blits == 1 && second.blits == 1);
    CHECK(display_gpu.screen[size_t{10} * 800 + 20] == pixel_of(20, 10) && display_gpu.screen[size_t{399} * 800 + 799] == pixel_of(799, 399));
    CHECK(second.screen[size_t{7} * 400 + 5] == pixel_of(805, 7) && second.screen[size_t{299} * 400 + 399] == pixel_of(1199, 299));
    display.present();
    CHECK(display_gpu.presents == 1 && second.presents == 1);
    display.present();   // (nothing drawn on the second GPU since)
    CHECK(display_gpu.presents == 2 && second.presents == 1);
    // Damage: a rectangle on the second GPU's monitor touches only those pixels there, and nothing on the display device.
    display_gpu.blits = 0;
    second.blits = 0;
    second.screen.assign(size_t{400} * 300, 0);
    display.blit(900, 50, 10, 10, picture.data() + 50 * 1200 + 900, 1200);
    CHECK(display_gpu.blits == 0 && second.blits == 1);
    CHECK(second.screen[size_t{55} * 400 + 105] == pixel_of(905, 55) && second.screen[size_t{50} * 400 + 100] == pixel_of(900, 50));
    CHECK(second.screen[size_t{49} * 400 + 100] == 0 && second.screen[size_t{50} * 400 + 110] == 0 && second.screen[size_t{60} * 400 + 100] == 0);
    display.present();
    CHECK(second.presents == 2);
    // A rectangle across the seam: each part to its GPU.
    display_gpu.blits = 0;
    second.blits = 0;
    display.blit(790, 100, 20, 10, picture.data() + 100 * 1200 + 790, 1200);
    CHECK(display_gpu.blits == 1 && second.blits == 1 && display_gpu.screen[size_t{105} * 800 + 795] == pixel_of(795, 105) && second.screen[size_t{105} * 400 + 5] == pixel_of(805, 105));
    // The pointer is part of the desktop's picture: drawn where it is, it reaches the second GPU's screen like everything else.
    std::vector<uint32_t> arrow(16 * 16, 0xFFFFFFFFu);
    display.blit(1000, 200, 16, 16, arrow.data(), 16);
    CHECK(second.screen[size_t{200} * 400 + 200] == 0xFFFFFFFFu && second.screen[size_t{215} * 400 + 215] == 0xFFFFFFFFu);

    // The second GPU's monitor at 200 %: 400x300 pixels show 200x150 of the desktop, each desktop pixel as a 2x2 block.
    MonitorLayout scaled(a);
    CHECK(scaled.add(0, 800, 400) && scaled.add(16, 400, 300) && scaled.set_mode(16, 400, 300, 200));
    CHECK(display.update(scaled, places, 2, 100, ArrayView<const MonitorTarget>(&target, 1)) && display.width() == 1000 && display.height() == 400);
    std::vector<uint32_t> small = desktop_picture(1000, 400);
    second.screen.assign(size_t{400} * 300, 0);
    display.blit(0, 0, 1000, 400, small.data(), 1000);
    CHECK(second.screen[0] == pixel_of(800, 0) && second.screen[1] == pixel_of(800, 0) && second.screen[2] == pixel_of(801, 0) && second.screen[size_t{2} * 400] == pixel_of(800, 1));
    CHECK(second.screen[size_t{299} * 400 + 399] == pixel_of(999, 149));

    // An end-to-end run: SystemMonitors gives the places and targets, MonitorDisplay copies.
    FakeSource src;
    src.add("A", Bytes());
    sized(src, 0, 800, 400);
    FakeGpu gpu;
    gpu.source = &src;
    gpu.w = 800;
    gpu.h = 400;
    ModeGpu bare;
    bare.modes = {{640, 480}};
    SystemMonitors sm(&gpu, a);
    CHECK(sm.add_gpu(&bare, 3));
    sm.initialize();
    CHECK(sm.apply() && sm.desktop_width() == 1440 && sm.desktop_height() == 480);
    ScanoutPlacement p[4];
    MonitorTarget t[4];
    const uint32_t np = sm.physical_placements(p, 4), nt = sm.targets(t, 4);
    MonitorDisplay spanned(gpu, a);
    CHECK(np == 2 && nt == 1 && spanned.update(sm.layout(), p, np, sm.render_percent(), ArrayView<const MonitorTarget>(t, nt)));
    std::vector<uint32_t> full = desktop_picture(1440, 480);
    spanned.blit(0, 0, 1440, 480, full.data(), 1440);
    spanned.present();
    CHECK(bare.screen.size() == size_t{640} * 480 && bare.screen[size_t{479} * 640 + 639] == pixel_of(1439, 479) && bare.screen[0] == pixel_of(800, 0) && bare.presents == 1);
    CHECK(gpu.screen[size_t{399} * 800 + 799] == pixel_of(799, 399));
  }
  CHECK(a.live == 0);
}

// Several threads: the desktop's thread copies to the second GPU and presents while another one presents its own pictures there (as
// GraphicsPool::present_image does) under the same guard. The GPU must never be entered by two at once.
static void test_threads() {
  TestAllocator a;
  {
    FakeGpu display_gpu;
    display_gpu.w = 256;
    display_gpu.h = 64;
    ModeGpu second;
    second.modes = {{128, 64}};
    second.screen.assign(size_t{128} * 64, 0);
    MutexGuard guard;
    MonitorDisplay display(display_gpu, a);
    MonitorLayout layout(a);
    CHECK(layout.add(0, 256, 64) && layout.add(16, 128, 64));
    ScanoutPlacement places[2] = {{0, 0, 0, 256, 64}, {16, 0, 0, 128, 64}};
    MonitorTarget target;
    target.id = 16;
    target.gpu = &second;
    target.guard = &guard;
    CHECK(display.update(layout, places, 2, 100, ArrayView<const MonitorTarget>(&target, 1)));
    std::vector<uint32_t> picture = desktop_picture(384, 64);
    std::vector<uint32_t> other(size_t{128} * 64, 0xFF123456u);
    constexpr int kRounds = 3000;
    std::thread desktop([&] {
      for (int i = 0; i < kRounds; ++i) {
        const uint32_t x = 256 + static_cast<uint32_t>(i % 100);
        display.blit(x, 8, 20, 20, picture.data() + 8 * 384 + x, 384);
        display.present();
      }
    });
    std::thread presenter([&] {
      for (int i = 0; i < kRounds; ++i) {
        guard.acquire();
        second.blit(0, 40, 128, 4, other.data(), 128);
        second.present();
        guard.release();
      }
    });
    desktop.join();
    presenter.join();
    CHECK(!second.overlapped && second.blits == 2 * kRounds && second.presents == 2 * kRounds && display_gpu.blits == 0);
    CHECK(second.screen[size_t{8} * 128 + 0] == pixel_of(256, 8) && second.screen[size_t{41} * 128 + 3] == 0xFF123456u);
  }
  CHECK(a.live == 0);
}

// The built-in panel moves from the display device to a second GPU and back (the display mux switch moved it; see Multi/GpuMuxSwitch).
static void test_panel_moves() {
  TestAllocator a;
  {
    // The display device drives the panel (eDP, Internal) and an external monitor; the second GPU waits with nothing lit (a zero-sized screen),
    // its eDP connector empty and an HDMI monitor plugged in that it does not drive yet.
    FakeSource isrc, dsrc;
    isrc.add("eDP-1", small_edid(), true, ConnectorType::Internal);
    isrc.add("DP-1", dell_edid());
    dsrc.multi = false;
    dsrc.add("eDP-1", small_edid(), false, ConnectorType::Internal);
    dsrc.add("HDMI-1", dell_edid(), true, ConnectorType::Hdmi);
    FakeGpu igpu, dgpu;
    igpu.source = &isrc;
    igpu.w = 4480;
    igpu.h = 1440;
    dgpu.source = &dsrc;
    dgpu.w = dgpu.h = 0;
    SystemMonitors sm(&igpu, a);
    CHECK(sm.add_gpu(&dgpu, 1));
    sm.initialize();
    CHECK(sm.layout().count() == 2 && sm.layout().find(0) >= 0 && sm.layout().find(1) >= 0 && sm.layout().find(16) < 0 && sm.layout().find(17) < 0);   // (nothing lit there: none of its monitors)
    CHECK(sm.set_scale(0, 150) && sm.layout().set_primary(0) && sm.apply() && !sm.spans_gpus());
    const MonitorRect panel_rect = sm.layout().at(static_cast<uint32_t>(sm.layout().find(0))).rect;
    const MonitorRect dell_rect = sm.layout().at(static_cast<uint32_t>(sm.layout().find(1))).rect;
    const uint32_t gen = sm.generation();
    // Nothing on the new GPU yet (the panel is not lit there): refused, nothing changed.
    CHECK(!sm.move_panel(0, 1) && sm.layout().count() == 2 && sm.find(0)->connected() && sm.generation() == gen);
    CHECK(!sm.move_panel(0, 0) && !sm.move_panel(5, 1));
    // The second GPU lit the panel: its screen has the panel's size, the connector is connected.
    dgpu.w = 1920;
    dgpu.h = 1080;
    dsrc.connectors[0].info.connected = true;
    CHECK(sm.move_panel(0, 1) && sm.panel_id() == 16 && sm.generation() != gen);
    CHECK(sm.layout().count() == 2 && sm.layout().find(0) < 0 && sm.layout().find(17) < 0);   // (the HDMI monitor the second GPU does not drive stays out)
    const int32_t slot = sm.layout().find(16);
    CHECK(slot >= 0);
    const MonitorSlot& moved = sm.layout().at(static_cast<uint32_t>(slot));
    CHECK(moved.scale_percent == 150 && moved.primary && moved.rect.x == panel_rect.x && moved.rect.y == panel_rect.y && moved.rect.width == panel_rect.width &&
          moved.physical_width == 1920 && moved.physical_height == 1080);
    CHECK(sm.layout().at(static_cast<uint32_t>(sm.layout().find(1))).rect.x == dell_rect.x);   // (the other monitor keeps its place)
    CHECK(!sm.find(0)->connected() && sm.find(16)->connected() && sm.find(16)->gpu() == 1 && sm.gpu_of(16) == 1 && sm.device_of(16) == &dgpu);
    MonitorTarget targets[4];
    CHECK(sm.targets(targets, 4) == 1 && targets[0].id == 16 && targets[0].gpu == &dgpu);
    CHECK(sm.apply() && sm.spans_gpus() && sm.needs_compositing() && isrc.last.size() == 1 && isrc.last[0].connector == 1);   // (the display device shows the DP monitor alone)
    // The next frame: the panel's part of the desktop goes to the second GPU's screen, scaled; the display device gets only the DP monitor's part.
    ScanoutPlacement places[4];
    const uint32_t n = sm.physical_placements(places, 4);
    MonitorDisplay display(igpu, a);
    const uint32_t k = sm.render_percent();
    CHECK(display.update(sm.layout(), places, n, k, ArrayView<const MonitorTarget>(targets, 1)) && display.target_count() == 1);
    const uint32_t dw = display.width(), dh = display.height();
    std::vector<uint32_t> picture = desktop_picture(dw, dh);
    display.blit(0, 0, dw, dh, picture.data(), dw);
    display.present();
    CHECK(dgpu.blits > 0 && dgpu.presents == 1 && dgpu.screen.size() == size_t{1920} * 1080 && dgpu.screen[0] != 0);
    // A poll that finds the display device's eDP connector still saying "connected" does not take the panel back.
    isrc.changed = true;
    (void)sm.poll();
    CHECK(sm.layout().find(0) < 0 && sm.layout().find(16) >= 0);
    // And back: the second GPU let the panel go, the display device lit it.
    dsrc.connectors[0].info.connected = false;
    dgpu.w = dgpu.h = 0;
    CHECK(sm.move_panel(1, 0) && sm.panel_id() == 0);
    const int32_t back = sm.layout().find(0);
    CHECK(back >= 0 && sm.layout().find(16) < 0 && sm.layout().at(static_cast<uint32_t>(back)).scale_percent == 150 && sm.layout().at(static_cast<uint32_t>(back)).primary);
    CHECK(sm.layout().at(static_cast<uint32_t>(back)).rect.x == panel_rect.x && sm.layout().at(static_cast<uint32_t>(back)).physical_width == 1920);
    CHECK(sm.targets(targets, 4) == 0 && !sm.spans_gpus() && sm.find(0)->connected() && !sm.find(16)->connected());
    CHECK(sm.apply() && isrc.last.size() == 2);
    // Off again, and a poll of the second GPU that sees its eDP connected (a stale report) does not bring the panel's monitor there back.
    dgpu.w = 1920;
    dgpu.h = 1080;
    dsrc.connectors[0].info.connected = true;
    CHECK(sm.move_panel(0, 1));
    dsrc.connectors[0].info.connected = false;
    dgpu.w = dgpu.h = 0;
    CHECK(sm.move_panel(1, 0));
    dsrc.connectors[0].info.connected = true;
    dsrc.changed = true;
    (void)sm.poll();
    CHECK(sm.layout().find(16) < 0 && sm.layout().find(0) >= 0);
  }
  {
    // The panel alone on the display device, which shows one display, and a second GPU the monitors were not told about until the panel moved.
    FakeSource isrc, dsrc;
    isrc.multi = false;
    isrc.add("Display-0", small_edid(), true, ConnectorType::Unknown);
    dsrc.multi = false;
    dsrc.add("eDP-1", small_edid(), true, ConnectorType::Internal);
    FakeGpu igpu, dgpu;
    igpu.source = &isrc;
    igpu.w = 1920;
    igpu.h = 1080;
    dgpu.source = &dsrc;
    dgpu.w = 1920;
    dgpu.h = 1080;
    SystemMonitors sm(&igpu, a);
    sm.initialize();
    CHECK(sm.apply() && sm.layout().count() == 1);
    CHECK(!sm.move_panel(0, 2) && sm.move_panel(0, 2, &dgpu) && sm.gpu_count() == 2 && sm.layout().count() == 1 && sm.gpu_of(sm.layout().at(0).id) == 2);
    CHECK(sm.apply() && sm.desktop_width() == 1920 && sm.surface_width_of(sm.layout().at(0).id) == 1920);   // (the display device keeps its screen; nothing of it is shown)
    MonitorTarget targets[2];
    ScanoutPlacement places[2];
    CHECK(sm.targets(targets, 2) == 1 && sm.physical_placements(places, 2) == 1 && places[0].x == 0);
    MonitorDisplay display(igpu, a);
    CHECK(display.update(sm.layout(), places, 1, sm.render_percent(), ArrayView<const MonitorTarget>(targets, 1)));
    std::vector<uint32_t> picture = desktop_picture(1920, 1080);
    display.blit(0, 0, 1920, 1080, picture.data(), 1920);
    CHECK(igpu.blits == 0 && dgpu.blits > 0 && dgpu.screen[size_t{5} * 1920 + 7] == pixel_of(7, 5));
    // Two monitors in use on the old GPU and neither an Internal connector: which is the panel is not known, nothing moves.
    FakeSource two;
    two.add("DP-1", dell_edid());
    two.add("DP-2", small_edid());
    FakeGpu many;
    many.source = &two;
    SystemMonitors sm2(&many, a);
    CHECK(sm2.add_gpu(&dgpu, 1));
    sm2.initialize();
    CHECK(sm2.layout().count() == 3 && !sm2.move_panel(0, 1) && sm2.layout().count() == 3);
  }
  CHECK(a.live == 0);
}

int main() {
  test_one_gpu();
  test_two_gpus_layout();
  test_secondary_modes();
  test_composition();
  test_threads();
  test_panel_moves();
  if (g_failed) { printf("%d check(s) failed\n", g_failed.load()); return 1; }
  printf("multi-GPU monitor tests passed\n");
  return 0;
}
