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

// Host-side tests for rendering on, and presenting to, any number of GPUs: a pool of N fake GPUs (1 to 4 here) with fake display drivers, the power lease
// around rendering, the policy choosing the GPU, an image rendered on one GPU shown on the screen of another, and a 2D renderer on a GPU of its own.
#include <stdio.h>
#include <vector>
#include "FakeGpuPci.hpp"
#include "Graphics/GraphicsPool.hpp"
#include <stdlib.h>

using namespace graphics;
using namespace fakegpu;

// The fake GPUs have no 3D engine: RenderNodeAccel.cpp (virgl, SVGA3D and the drivers under them) is not built here, and these stand in for it.
void RenderNode::build_accelerated(hardware::VirtioGpu*, hardware::VmwareSvgaGpu*, hardware::IntelGpu*, allocators::IAllocator&, bool, bool) { reason_ = "fake GPU"; }
void RenderNode::destroy_accelerated() {}


static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {
struct HostAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size)) return nullptr;
    ++live;
    return p;
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// A display driver that keeps its screen in memory.
class FakeScreen final : public hardware::IGraphicsProcessingUnit {
 public:
  FakeScreen(uint32_t w, uint32_t h) : w_(w), h_(h), pixels_(uint64_t{w} * h, 0) {}
  const char* name() const override { return "fake screen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t i, hardware::DisplayMode* out) const override { if (i) return false; *out = {w_, h_}; return true; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t i) override { return i == 0; }
  uint32_t width() const override { return w_; }
  uint32_t height() const override { return h_; }
  void put_pixel(uint32_t x, uint32_t y, uint32_t c) override { if (x < w_ && y < h_) pixels_[uint64_t{y} * w_ + x] = c; }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) override {
    for (uint32_t j = 0; j < h; ++j) for (uint32_t i = 0; i < w; ++i) put_pixel(x + i, y + j, c);
  }
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t c) override { for (uint32_t& p : pixels_) p = c; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
  void present() override { ++presents; }

  uint32_t at(uint32_t x, uint32_t y) const { return pixels_[uint64_t{y} * w_ + x]; }
  uint32_t presents = 0;

 private:
  uint32_t w_, h_;
  std::vector<uint32_t> pixels_;
};

// The display GPU's graphics, as SystemGraphics would be.
class FakeDisplayGraphics final : public IDisplayGraphics {
 public:
  FakeDisplayGraphics(hardware::IGraphicsProcessingUnit* gpu, allocators::IAllocator& a) : node_(gpu, nullptr, nullptr, a), renderer_(a) {}
  RenderNode& render_node() override { return node_; }
  IRenderer2D* renderer_2d() override { return renderer_.get(node_, node_.gpu(), true); }
 private:
  RenderNode node_;
  Renderer2DSlot renderer_;
};

constexpr uint32_t kGreen = 0xFF00FF00;

// Renders a solid green 8x6 image on `lease` and hands it to `present`.
bool render_green(RenderLease& lease, GraphicsPool& pool, uint32_t target, uint32_t x, uint32_t y) {
  IGraphicsDevice& d = lease.device();
  IImage* image = d.create_image({{8, 6}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
  ICommandBuffer* commands = d.create_command_buffer();
  if (!image || !commands) return false;
  RenderPassBegin pass;
  pass.color = image;
  pass.clear_color_value = {0, 1, 0, 1};
  commands->begin();
  commands->begin_render_pass(pass);
  commands->end_render_pass();
  bool ok = commands->end() && d.submit(commands) && pool.present_image(*image, target, x, y);
  d.destroy_command_buffer(commands);
  d.destroy_image(image);
  return ok;
}

// A machine with `n` GPUs: the last is the integrated one the firmware shows the screen on, the others are discrete cards. `with_driver[i]` says
// whether GPU i has a display driver.
void run_machine(uint32_t n, const bool* with_driver) {
  FakePci pci;
  for (uint32_t i = 0; i + 1 < n; ++i) pci.add(static_cast<uint8_t>(i + 1), kNvidia, kRtx4060Laptop, hardware::kPciClassDisplay, 0, true);
  pci.add(static_cast<uint8_t>(n), kIntel, kIntelTigerLake, hardware::kPciClassDisplay, 0x7, true);

  HostAllocator allocator;
  hardware::GpuPool gpus(pci, allocator);
  gpus.initialize();
  CHECK(gpus.count() == n);
  const uint32_t display = n - 1;
  CHECK(static_cast<uint32_t>(gpus.manager().display_index()) == display);

  std::vector<FakeScreen*> screens(n, nullptr);
  uint32_t drivers = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (!with_driver[i]) continue;
    screens[i] = new FakeScreen(32 + i, 24);  // each GPU has a screen of its own size
    CHECK(gpus.adopt(i, screens[i]));
    ++drivers;
  }
  if (screens[display]) CHECK(!gpus.adopt(display, screens[display]));  // a GPU that has a driver takes no second one
  CHECK(gpus.driver_count() == drivers);

  {
    FakeDisplayGraphics display_graphics(screens[display], allocator);
    GraphicsPool pool(gpus, display_graphics, allocator);
    CHECK(pool.count() == n);
    CHECK(pool.render_node(display) == &display_graphics.render_node());
    for (uint32_t i = 0; i + 1 < n; ++i) CHECK(pool.render_node(i) && pool.render_node(i) != pool.render_node(display));
    CHECK(pool.render_node(n) == nullptr);
    CHECK(pool.accelerated_count() == 0);  // the fakes have no 3D engine

    // Discrete GPUs sleep until leased.
    for (uint32_t i = 0; i + 1 < n; ++i) CHECK(gpus.manager().asleep(i));

    // Policy: a game on AC goes to a discrete GPU (and wakes it); the desktop stays on the integrated one and wakes nothing.
    hardware::GpuSelectionInput game;
    game.workload = hardware::GpuWorkload::Game;
    game.power = hardware::PowerSource::Ac;
    {
      RenderLease lease = pool.acquire(game);
      if (n > 1) {
        CHECK(lease && lease.node() == 0 && !gpus.manager().asleep(0) && !lease.accelerated());
        for (uint32_t t = 0; t < n; ++t) {
          if (!with_driver[t]) { CHECK(!render_green(lease, pool, t, 2, 3)); continue; }
          // Rendered on GPU 0, shown on GPU t.
          CHECK(render_green(lease, pool, t, 2, 3));
          CHECK(screens[t]->at(2, 3) == kGreen && screens[t]->at(9, 8) == kGreen);
          CHECK(screens[t]->at(1, 3) == 0 && screens[t]->at(10, 3) == 0 && screens[t]->at(2, 9) == 0);
          CHECK(screens[t]->presents >= 1);
        }
        // Moved and clipped: the image hangs over the screen's edge.
        if (with_driver[display]) CHECK(render_green(lease, pool, display, screens[display]->width() - 2, screens[display]->height() - 2));
        if (with_driver[display]) CHECK(screens[display]->at(screens[display]->width() - 1, screens[display]->height() - 1) == kGreen);
      }
      RenderLease moved = static_cast<RenderLease&&>(lease);
      CHECK(!lease && (n == 1 || moved));
    }
    for (uint32_t i = 0; i + 1 < n; ++i) CHECK(gpus.manager().asleep(i) && gpus.manager().leases(i) == 0);

    hardware::GpuSelectionInput desktop;
    desktop.power = hardware::PowerSource::Battery;
    {
      RenderLease lease = pool.acquire(desktop);
      CHECK(lease && lease.node() == display);
      for (uint32_t i = 0; i + 1 < n; ++i) CHECK(gpus.manager().asleep(i));
    }
    // Low charge: even a game stays on the integrated GPU.
    game.power = hardware::PowerSource::Battery;
    game.battery_percent = 10;
    { RenderLease lease = pool.acquire(game); CHECK(lease && lease.node() == display); }

    // 2D drawing on each GPU's own screen (the display GPU's renderer is the one the display graphics has).
    for (uint32_t i = 0; i < n; ++i) {
      IRenderer2D* r = pool.renderer_2d(i);
      CHECK((r != nullptr) == with_driver[i]);
      if (!r) continue;
      CHECK(r->extent().width == screens[i]->width() && r->extent().height == screens[i]->height());
      r->fill_rect({1, 1, 3, 2}, 0xFFFF0000);
      r->present();
      CHECK(screens[i]->at(1, 1) == 0xFFFF0000 && screens[i]->at(3, 2) == 0xFFFF0000 && screens[i]->at(4, 1) == 0);
      CHECK(pool.renderer_2d(i) == r);  // created once
    }
    CHECK(pool.renderer_2d(n) == nullptr);

    // A lease on a named GPU; bad indices give nothing.
    for (uint32_t i = 0; i < n; ++i) {
      RenderLease lease = pool.acquire_node(i);
      CHECK(lease && lease.node() == i && gpus.manager().leases(i) == 1);
    }
    CHECK(!pool.acquire_node(n));

    // Presenting to a GPU without a driver, or an empty image, fails.
    for (uint32_t t = 0; t < n; ++t) {
      RenderLease lease = pool.acquire_node(display);
      IImage* image = lease.device().create_image({{4, 4}, Format::B8G8R8A8Unorm, kImageColorAttachment | kImagePresent});
      CHECK(pool.present_image(*image, t, 0, 0) == with_driver[t]);
      lease.device().destroy_image(image);
    }
  }
  for (FakeScreen* s : screens) delete s;
  CHECK(allocator.live == 0);  // nothing leaked, by the pool or the nodes it built
}
}  // namespace

int main() {
  const bool all[4] = {true, true, true, true};
  for (uint32_t n = 1; n <= 4; ++n) run_machine(n, all);
  const bool some[3] = {false, true, true};  // the discrete card has no driver: it renders (on the CPU) but shows nothing
  run_machine(3, some);
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("GraphicsPoolTest: ok\n");
  return 0;
}
