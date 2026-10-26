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

// Native mode setting for the display engine of Intel integrated GPUs (Gen9.5, Gen11 and Xe-LP: "display version" 9 to 12), after the register layouts of the
// MIT-licensed i915 driver (display/intel_display_reg_defs.h, skl_universal_plane_regs.h, skl_scaler.c).
//
// The firmware has already lit the screen: a pipe feeds a transcoder, a DDI port, a PLL and the panel or monitor, all running at the panel's own timings.
// This driver leaves all of that alone (no PLL, DDI or link programming, which is where blind register writes can turn a screen black for good) and changes
// only what comes before the transcoder: the size of the pipe's source, the primary plane that reads the picture (its surface address, stride and size), and
// the pipe scaler, which stretches a smaller picture over the panel's own timings. So a mode is any size up to the panel's, shown scaled to fit, and the framebuffer
// is ours (system memory mapped into the global GTT, see IntelScanout), not the firmware's.
//
// What it does not do: change the panel's own timings (a different refresh rate or a monitor mode beyond what the firmware lit), drive a second pipe, handle
// DSC or tiled/compressed firmware planes (the plane is put back to linear), or run on Meteor Lake and later (their display engine is laid out differently). A
// GPU or state it does not understand is refused (probe() fails) and the UEFI framebuffer stays in use. Every change is read back, and the state before it is
// put back if the hardware did not take it. It has not been run on a real GPU: the host tests drive it against a simulated register file.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"

namespace hardware {
namespace intel_display {

// Which GPUs this driver handles: the integrated ones from Gen9.5 to Xe-LP.
bool supported(IntelGpuArchitecture architecture, bool discrete);

constexpr uint32_t kMaxPipes = 4;
constexpr uint64_t kRegisterWindow = 0x80000;  // the display registers are in the first 512 KiB of the register BAR

// Per-pipe registers: pipe A at the offset given, each next pipe `stride` further.
constexpr uint64_t kTransHtotalA = 0x60000, kTransVtotalA = 0x6000C, kPipeSrcA = 0x6001C, kTransStride = 0x1000;
constexpr uint64_t kPipeConfA = 0x70008, kPipeStride = 0x1000;
constexpr uint64_t kPlaneCtlA = 0x70180, kPlaneStrideA = 0x70188, kPlanePosA = 0x7018C, kPlaneSizeA = 0x70190, kPlaneSurfA = 0x7019C, kPlaneOffsetA = 0x701A4;
constexpr uint64_t kScalerCtlA = 0x68180, kScalerPosA = 0x68170, kScalerSizeA = 0x68174, kScalerStride = 0x800;

constexpr uint32_t kPipeEnable = 1u << 31;
constexpr uint32_t kPlaneEnable = 1u << 31;
constexpr uint32_t kPlaneFormatMask = 0xFu << 24, kPlaneFormatXrgb8888 = 0x4u << 24;
constexpr uint32_t kPlaneRgbOrder = 1u << 20;     // R and B swapped (memory order R, G, B, x)
constexpr uint32_t kPlaneTilingMask = 7u << 10;   // 0: linear
constexpr uint32_t kPlaneRenderCompression = 1u << 15;
constexpr uint32_t kPlaneMediaCompression = 1u << 4;  // Xe-LP only
constexpr uint32_t kScalerEnable = 1u << 31;      // pipe scaler, dynamic mode, medium filter

// What a mode change touches, so it can be put back.
struct Snapshot {
  bool valid = false;
  uint32_t pipe_src = 0, plane_ctl = 0, plane_stride = 0, plane_pos = 0, plane_size = 0, plane_surf = 0, plane_offset = 0;
  uint32_t scaler_ctl = 0, scaler_pos = 0, scaler_size = 0;
};

class Display {
 public:
  Display(intel_gt::IRegisterBank& registers, IntelGpuArchitecture architecture) : r_(registers), architecture_(architecture) {}

  // Looks for the pipe the firmware is showing the screen on (enabled, with its primary plane on) and reads its panel size from the transcoder timings.
  // False if there is none, or the numbers make no sense.
  bool probe();
  bool active() const { return active_; }
  uint32_t pipe() const { return pipe_; }
  // The size the panel is driven at, which is the largest mode and the one that needs no scaling.
  uint32_t native_width() const { return native_w_; }
  uint32_t native_height() const { return native_h_; }
  // The size of the picture the plane reads now, from the pipe's source size.
  uint32_t source_width() const;
  uint32_t source_height() const;
  bool can_show(uint32_t width, uint32_t height) const;

  // The state to go back to (taken by probe()).
  const Snapshot& original() const { return original_; }
  Snapshot save() const;
  void restore(const Snapshot& state);

  // Shows `width` x `height` pixels read from the graphics address `surface` (4 KiB aligned, below 4 GiB) with `stride_bytes` between rows (a multiple of 64),
  // linear XRGB8888 in memory order B, G, R, x. Everything is read back; on a mismatch the state before the call is put back and false returned.
  bool set_mode(uint32_t width, uint32_t height, uint64_t surface, uint32_t stride_bytes);

 private:
  uint64_t pipe_reg(uint64_t pipe_a, uint64_t stride) const { return pipe_a + stride * pipe_; }
  uint32_t pipe_count() const;

  intel_gt::IRegisterBank& r_;
  IntelGpuArchitecture architecture_;
  bool active_ = false;
  uint32_t pipe_ = 0;
  uint32_t native_w_ = 0, native_h_ = 0;
  Snapshot original_;
};

}  // namespace intel_display
}  // namespace hardware
