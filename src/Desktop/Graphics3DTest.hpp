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

// The 3D test application: a window with a spinning, depth-tested, colored cube drawn through the kernel-mode Direct3D driver (CubeRenderer: the
// same calls a Direct3D program's user-mode driver makes) on one of its adapters, the frame rate, and
// an indicator of where the drawing happens - green "GPU accelerated" when the host's graphics hardware does it (virgl on QEMU, SVGA3D on
// VMware / VirtualBox), amber "Software (CPU)" when the OS's own rasterizer does. A check box switches between the two when a GPU device is
// there, so the speeds can be compared.
//
// Each frame is drawn from paint(), which runs on the window's render thread - the only thread that uses the 3D device - so the cube needs no
// thread or lock of its own. The picture is read back from the device (small, so that reading it back is cheap) and shown like any image.
// Each face of the cube shows the OS logo as a texture, which exercises the sampler as well as the vertices and colors; a check box turns that
// off for flat per-face colors, and a device that cannot sample textures falls back to them by itself.
// "FPS" is how many of those frames are finished per second; "render" is the time the driver and the readback took for the last one, split into
// "submit" (Kernel::render) and "readback" (Kernel::lock); the next line gives the average, best and worst frame since the last switch - so the
// window doubles as a benchmark of the kernel path, with the GPU and the CPU adapter one click apart.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Desktop/CubeRenderer.hpp"
#include "Graphics/SystemGraphics.hpp"
#include "Kernel/Dxgkrnl/DxgKernel.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

class CubeView : public gui::Control {
 public:
  static constexpr uint32_t kMaxWidth = 400, kMaxHeight = 300;
  static constexpr uint64_t kFrameIntervalUs = 16667;   // 60 frames a second: the cube is drawn at the pace of a display, not as often as the machine can

  explicit CubeView(allocators::IAllocator& allocator) : Control(allocator) {}

  // The kernel-mode Direct3D driver to draw with (nullptr: the view says there is none) and the machine's graphics, which only say why there is no GPU. Both must
  // outlive the view.
  void set_graphics(dxgk::Kernel* kernel, graphics::SystemGraphics* graphics);
  // Whether the cube is drawn on the GPU device (when there is one) or on the CPU one.
  void set_use_gpu(bool use_gpu);
  bool use_gpu() const { return use_gpu_; }
  // Whether the faces show the logo (when the device can sample it) or flat colors.
  void set_use_texture(bool use_texture);
  bool use_texture() const { return use_texture_; }
  bool gpu_available() const { return kernel_ && CubeRenderer::find_adapter(*kernel_, true) != dxgk::kInvalidIndex; }

  // The measurements, for the status line and for tests.
  uint32_t fps() const { return fps_; }
  uint64_t frame_microseconds() const { return frame_us_; }
  uint64_t frames() const { return total_frames_; }
  // The benchmark: frames drawn (and failed) since the adapter was chosen, and their times in microseconds.
  uint64_t bench_frames() const { return bench_frames_; }
  uint64_t bench_average_us() const { return bench_frames_ ? bench_total_us_ / bench_frames_ : 0; }
  uint64_t bench_best_us() const { return bench_best_us_; }
  uint64_t bench_worst_us() const { return bench_worst_us_; }
  uint64_t submit_microseconds() const { return submit_us_; }
  uint64_t readback_microseconds() const { return readback_us_; }
  bool accelerated() const { return active_accelerated_; }
  // Whether the last frame was drawn with the logo on the faces.
  bool textured() const { return active_textured_; }
  // Whether the device in use could be given the logo (known once a frame has been drawn).
  bool texture_available() const { return texture_available_; }
  // Why the GPU is not in use (empty when it is, or when the check box chose software).
  const char* software_reason() const { return graphics_ ? graphics_->software_reason() : ""; }

  void paint(gui::Painter& painter) override;

 protected:
  gui::Size default_size() const override { return {kMaxWidth, kMaxHeight}; }

 private:
  bool ensure_renderer(uint32_t width, uint32_t height);
  void restart_benchmark();

  static constexpr dxgk::ProcessId kProcessId = 0x3D3D3D3D;   // (the process the 3D test is to the kernel-mode driver)

  graphics::SystemGraphics* graphics_ = nullptr;
  dxgk::Kernel* kernel_ = nullptr;
  UniquePointer<CubeRenderer> renderer_;
  bool renderer_accelerated_ = false;
  bool use_gpu_ = true;
  bool use_texture_ = true;
  bool active_textured_ = false;
  bool texture_available_ = false;
  bool active_accelerated_ = false;
  bool failed_ = false;
  bool share_refused_ = false;   // the painter's renderer could not show the frame where it is on the GPU: frames are read back
  FixedString<48> device_name_;
  uint32_t fps_ = 0, frames_in_second_ = 0, seconds_ = 0;
  uint64_t second_start_us_ = 0, frame_us_ = 0, total_frames_ = 0, start_us_ = 0;
  uint64_t submit_us_ = 0, readback_us_ = 0;
  uint64_t bench_frames_ = 0, bench_total_us_ = 0, bench_best_us_ = 0, bench_worst_us_ = 0;
};

class Graphics3DTest : public gui::Frame {
 public:
  explicit Graphics3DTest(allocators::IAllocator& allocator);

  void set_graphics(dxgk::Kernel* kernel, graphics::SystemGraphics* graphics);
  CubeView& view() { return view_; }
  gui::CheckBox& gpu_box() { return gpu_box_; }
  gui::CheckBox& texture_box() { return texture_box_; }

 private:
  static void on_gpu_changed(void* self, gui::Control&);
  static void on_texture_changed(void* self, gui::Control&);

  CubeView view_;
  gui::Panel bar_;
  gui::CheckBox gpu_box_;
  gui::CheckBox texture_box_;
};

}  // namespace desktop
