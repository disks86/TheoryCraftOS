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

#include "Desktop/Graphics3DTest.hpp"
#include "Gui/Window.hpp"
#include "Kernel/Delay.hpp"
#include "console.hpp"

namespace desktop {

using namespace graphics;

namespace {
constexpr float kTwoPi = 6.2831853f;
constexpr uint32_t kBackground = 0xFF10101A;
constexpr uint32_t kGreen = 0xFF40E060, kAmber = 0xFFFFB030, kRed = 0xFFFF5050, kWhite = 0xFFFFFFFF, kShadow = 0xFF000000;

void append_ms(FixedString<96>& line, uint64_t us) {
  line.append_uint(us / 1000);
  line.push_back('.');
  line.append_uint((us % 1000) / 100);
}
}  // namespace

void CubeView::set_graphics(dxgk::Kernel* kernel, SystemGraphics* graphics) {
  gui::UiGuard lock(this);
  renderer_.reset();
  kernel_ = kernel;
  graphics_ = graphics;
  invalidate();
}

void CubeView::set_use_gpu(bool use_gpu) {
  gui::UiGuard lock(this);
  use_gpu_ = use_gpu;
  second_start_us_ = 0;   // the frame rate starts over
  invalidate();
}

void CubeView::set_use_texture(bool use_texture) {
  gui::UiGuard lock(this);
  use_texture_ = use_texture;
  restart_benchmark();
  invalidate();
}

void CubeView::restart_benchmark() {
  bench_frames_ = bench_total_us_ = bench_best_us_ = bench_worst_us_ = 0;
}

// The renderer for this size on the adapter the check box chooses (the GPU's when there is one and it is asked for, the CPU renderer's otherwise).
bool CubeView::ensure_renderer(uint32_t width, uint32_t height) {
  if (!kernel_) return false;
  uint32_t adapter = CubeRenderer::find_adapter(*kernel_, use_gpu_);
  if (adapter == dxgk::kInvalidIndex) adapter = CubeRenderer::find_adapter(*kernel_, !use_gpu_);   // (the machine has only the other kind)
  if (adapter == dxgk::kInvalidIndex) return false;
  if (renderer_ && renderer_->is_open() && renderer_->adapter_index() == adapter && renderer_->width() == width && renderer_->height() == height) return true;
  if (!renderer_) {
    renderer_ = UniquePointer<CubeRenderer>::make(allocator(), allocator(), *kernel_, kProcessId);
    if (!renderer_) return false;
  }
  const bool switched = renderer_->adapter_index() != adapter;
  // (on a GPU the picture stays there when the desktop's renderer draws on that very device: only then is the color target made one it can sample)
  const dxgk::Adapter* chosen = kernel_->adapter(adapter);
  if (!renderer_->open(adapter, width, height, chosen && chosen->caps().accelerated)) return false;
  texture_available_ = renderer_->texture_available();
  share_refused_ = false;
  if (switched) restart_benchmark();   // (a new size alone keeps the average going: the frames are not the same work, but the check boxes are what the numbers compare)
  return true;
}

void CubeView::paint(gui::Painter& painter) {
  const gui::Rect2D area{0, 0, bounds().width, bounds().height};
  painter.fill(area, kBackground);
  if (!kernel_) {
    painter.text_centered(area, "No Direct3D driver.", kRed);
    return;
  }

  const uint64_t frame_start = hardware::uptime_us();
  if (!start_us_) start_us_ = frame_start;
  uint32_t width = static_cast<uint32_t>(area.width > 0 ? area.width : 0), height = static_cast<uint32_t>(area.height > 0 ? area.height : 0);
  if (width > kMaxWidth) width = kMaxWidth;
  if (height > kMaxHeight) height = kMaxHeight;
  const bool ready = width >= 32 && height >= 32 && ensure_renderer(width, height);
  bool drawn = false;
  if (ready) {
    const float angle = static_cast<float>((frame_start - start_us_) % 6000000) * (kTwoPi / 6000000.0f);   // one turn in 6 seconds
    CubeFrameTimes times;
    // A frame that stays on the GPU is composed from there; if the renderer cannot take it (another device, a CPU renderer) it is read back and drawn from memory as before.
    const bool shared = renderer_->shareable() && !share_refused_;
    drawn = renderer_->render(angle, use_texture_, &times, !shared);
    if (drawn) {
      const int32_t px = (area.width - static_cast<int32_t>(width)) / 2, py = (area.height - static_cast<int32_t>(height)) / 2;
      bool shown = false;
      if (shared) {
        graphics::IGraphicsDevice* device = nullptr;
        graphics::IImage* image = nullptr;
        if (renderer_->acquire_target(&device, &image)) {
          shown = painter.device_image(*device, *image, {0, 0, width, height}, px, py);
          renderer_->release_target();
        }
        if (!shown) {
          share_refused_ = true;   // (until the renderer is made again)
          const uint64_t start = hardware::uptime_us();
          drawn = renderer_->fetch_picture();
          times.readback_us = hardware::uptime_us() - start;
        }
      }
      if (drawn && !shown) painter.image(renderer_->picture(), {0, 0, width, height}, px, py);
    }
    if (drawn) {
      active_accelerated_ = renderer_->caps().accelerated;
      active_textured_ = renderer_->last_textured();
      device_name_.assign(renderer_->caps().name);
      submit_us_ = times.submit_us;
      readback_us_ = times.readback_us;
      const uint64_t total = times.total_us();
      bench_total_us_ += total;
      if (!bench_frames_ || total < bench_best_us_) bench_best_us_ = total;
      if (total > bench_worst_us_) bench_worst_us_ = total;
      ++bench_frames_;
    }
  }
  failed_ = !drawn;
  const uint64_t now = hardware::uptime_us();
  frame_us_ = now - frame_start;
  ++total_frames_;
  ++frames_in_second_;
  if (!second_start_us_) second_start_us_ = now;
  if (now - second_start_us_ >= 1000000) {
    fps_ = static_cast<uint32_t>(uint64_t{frames_in_second_} * 1000000 / (now - second_start_us_));
    frames_in_second_ = 0;
    second_start_us_ = now;
    if (++seconds_ % 5 == 1) {   // now and then, on the debug console too
      FixedString<128> report;
      report.assign("3D Test: ");
      report.append_uint(fps_);
      report.append(" FPS, avg ");
      report.append_uint(bench_average_us());
      report.append(" us/frame, ");
      report.append(active_accelerated_ && !failed_ ? "GPU accelerated" : "software");
      report.append(" (");
      report.append(device_name_.view());
      report.append(", Direct3D kernel mode)\n");
      console::debug_puts(report.view());
    }
  }

  // The status, over the picture's corner.
  FixedString<96> line;
  line.assign("FPS: ");
  line.append_uint(fps_);
  line.append("   render: ");
  append_ms(line, frame_us_);
  line.append(" ms");
  painter.text(9, 9, line.view(), kShadow);
  painter.text(8, 8, line.view(), kWhite);

  FixedString<64> faces;
  faces.assign(failed_ ? "" : active_textured_ ? "Faces: logo texture" : texture_available_ ? "Faces: flat colors" : "Faces: flat colors (no texture support)");
  painter.text(9, 63, faces.view(), kShadow);
  painter.text(8, 62, faces.view(), kWhite);

  // The benchmark: where a frame's time goes, and how it has varied since the adapter or the faces were switched.
  if (bench_frames_) {
    FixedString<96> split;
    split.assign("submit: ");
    append_ms(split, submit_us_);
    split.append("   readback: ");
    append_ms(split, readback_us_);
    split.append(" ms");
    painter.text(9, 81, split.view(), kShadow);
    painter.text(8, 80, split.view(), kWhite);
    FixedString<96> spread;
    spread.assign("avg ");
    append_ms(spread, bench_average_us());
    spread.append("  min ");
    append_ms(spread, bench_best_us_);
    spread.append("  max ");
    append_ms(spread, bench_worst_us_);
    spread.append(" ms");
    painter.text(9, 99, spread.view(), kShadow);
    painter.text(8, 98, spread.view(), kWhite);
  }

  const bool gpu = active_accelerated_ && !failed_;
  const uint32_t indicator = failed_ ? kRed : gpu ? kGreen : kAmber;
  painter.fill({8, 28, 12, 12}, indicator);
  painter.outline({8, 28, 12, 12}, kWhite);
  FixedString<96> mode;
  mode.assign(failed_ ? "3D drawing failed" : gpu ? "GPU accelerated" : "Software (CPU)");
  if (!failed_ && device_name_.view().size()) {
    mode.append(" - ");
    mode.append(device_name_.view());
  }
  painter.text(27, 27, mode.view(), kShadow);
  painter.text(26, 26, mode.view(), indicator);
  if (!gpu_available() && !failed_) {   // say why, so the machine's setup can be fixed
    FixedString<128> why;
    why.assign("No GPU: ");
    why.append(software_reason());
    painter.text(9, 45, why.view(), kShadow);
    painter.text(8, 44, why.view(), kAmber);
  }
  if (gui::Window* w = window()) w->request_frame_interval(kFrameIntervalUs);
  invalidate();   // the next frame, as soon as the render loop is ready (at the display's pace)
}

// --- The window ------------------------------------------------------------------------------------

Graphics3DTest::Graphics3DTest(allocators::IAllocator& allocator) : Frame(allocator), view_(allocator), bar_(allocator), gpu_box_(allocator), texture_box_(allocator) {
  set_title("3D Test");
  set_bounds({100, 60, 440, 380});
  set_padding(gui::Insets::all(4));
  set_minimize_enabled(true);

  bar_.set_style(gui::PanelStyle::Sunken);
  bar_.set_dock(gui::DockStyle::Bottom);
  bar_.set_size(0, 26);
  gpu_box_.set_text("Use GPU acceleration");
  gpu_box_.set_dock(gui::DockStyle::Fill);
  gpu_box_.set_checked(true);
  gpu_box_.set_enabled(false);   // until a GPU device is known
  gpu_box_.on_changed({&Graphics3DTest::on_gpu_changed, this});
  texture_box_.set_text("Logo texture");
  texture_box_.set_dock(gui::DockStyle::Left);
  texture_box_.set_size(130, 0);
  texture_box_.set_checked(true);
  texture_box_.on_changed({&Graphics3DTest::on_texture_changed, this});
  bar_.add(&texture_box_);
  bar_.add(&gpu_box_);
  view_.set_dock(gui::DockStyle::Fill);
  add(&bar_);
  add(&view_);
}

void Graphics3DTest::set_graphics(dxgk::Kernel* kernel, SystemGraphics* graphics) {
  view_.set_graphics(kernel, graphics);
  gpu_box_.set_enabled(view_.gpu_available());
  gpu_box_.set_text(view_.gpu_available() ? "Use GPU acceleration" : "Use GPU acceleration (none here)");
}

void Graphics3DTest::on_texture_changed(void* self, gui::Control&) {
  auto* t = static_cast<Graphics3DTest*>(self);
  t->view_.set_use_texture(t->texture_box_.checked());
}

void Graphics3DTest::on_gpu_changed(void* self, gui::Control&) {
  auto* t = static_cast<Graphics3DTest*>(self);
  t->view_.set_use_gpu(t->gpu_box_.checked());
}

}  // namespace desktop
