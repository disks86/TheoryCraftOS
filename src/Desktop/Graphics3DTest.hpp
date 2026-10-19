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

// The 3D test application: a window with a spinning, depth-tested, colored cube drawn on the machine's 3D device, the frame rate, and
// an indicator of where the drawing happens - green "GPU accelerated" when the host's graphics hardware does it (virgl on QEMU, SVGA3D on
// VMware / VirtualBox), amber "Software (CPU)" when the OS's own rasterizer does. A check box switches between the two when a GPU device is
// there, so the speeds can be compared.
//
// Each frame is drawn from paint(), which runs on the window's render thread - the only thread that uses the 3D device - so the cube needs no
// thread or lock of its own. The picture is read back from the device (small, so that reading it back is cheap) and shown like any image.
// "FPS" is how many of those frames are finished per second; "render" is the time the device and the readback took for the last one.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Graphics/SystemGraphics.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Utility/FixedString.hpp"

namespace desktop {

class CubeView : public gui::Control {
 public:
  static constexpr uint32_t kMaxWidth = 400, kMaxHeight = 300;

  explicit CubeView(allocators::IAllocator& allocator) : Control(allocator) {}
  ~CubeView();

  // The machine's graphics (nullptr: the view says there is none). It must outlive the view.
  void set_graphics(graphics::SystemGraphics* graphics);
  // Whether the cube is drawn on the GPU device (when there is one) or on the CPU one.
  void set_use_gpu(bool use_gpu);
  bool use_gpu() const { return use_gpu_; }
  bool gpu_available() const { return graphics_ && graphics_->accelerated(); }

  // The measurements, for the status line and for tests.
  uint32_t fps() const { return fps_; }
  uint64_t frame_microseconds() const { return frame_us_; }
  uint64_t frames() const { return total_frames_; }
  bool accelerated() const { return active_accelerated_; }
  // Why the GPU is not in use (empty when it is, or when the check box chose software).
  const char* software_reason() const { return graphics_ ? graphics_->software_reason() : ""; }

  void paint(gui::Painter& painter) override;

 protected:
  gui::Size default_size() const override { return {kMaxWidth, kMaxHeight}; }

 private:
  struct Resources;
  bool ensure_resources(uint32_t width, uint32_t height);
  void release_resources();
  bool draw_frame(float angle);

  graphics::SystemGraphics* graphics_ = nullptr;
  Resources* resources_ = nullptr;
  graphics::IGraphicsDevice* resources_device_ = nullptr;
  bool use_gpu_ = true;
  bool active_accelerated_ = false;
  bool failed_ = false;
  FixedString<48> device_name_;
  uint32_t fps_ = 0, frames_in_second_ = 0, seconds_ = 0;
  uint64_t second_start_us_ = 0, frame_us_ = 0, total_frames_ = 0, start_us_ = 0;
};

class Graphics3DTest : public gui::Frame {
 public:
  explicit Graphics3DTest(allocators::IAllocator& allocator);

  void set_graphics(graphics::SystemGraphics* graphics);
  CubeView& view() { return view_; }
  gui::CheckBox& gpu_box() { return gpu_box_; }

 private:
  static void on_gpu_changed(void* self, gui::Control&);

  CubeView view_;
  gui::Panel bar_;
  gui::CheckBox gpu_box_;
};

}  // namespace desktop
