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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelDisplay.hpp"

namespace hardware {
namespace intel_display {

namespace {
constexpr uint32_t kMinSize = 64;           // smaller pictures cannot be scaled by the hardware
constexpr uint32_t kMaxSize = 8192;
constexpr uint32_t kMaxUpscaleNumerator = 8;  // the pipe scaler stretches at most 8x
}  // namespace

bool supported(IntelGpuArchitecture architecture, bool discrete) {
  if (discrete) return false;  // DG1 and Arc keep their display in a different place
  return architecture == IntelGpuArchitecture::Gen9_5 || architecture == IntelGpuArchitecture::Gen11 || architecture == IntelGpuArchitecture::XeLp;
}

uint32_t Display::pipe_count() const { return architecture_ == IntelGpuArchitecture::XeLp ? 4 : 3; }

uint32_t Display::source_width() const { return active_ ? (r_.read32(pipe_reg(kPipeSrcA, kTransStride)) >> 16) + 1 : 0; }
uint32_t Display::source_height() const { return active_ ? (r_.read32(pipe_reg(kPipeSrcA, kTransStride)) & 0xFFFF) + 1 : 0; }

bool Display::probe() {
  active_ = false;
  original_ = Snapshot();
  for (uint32_t p = 0; p < pipe_count(); ++p) {
    pipe_ = p;
    if (!(r_.read32(pipe_reg(kPipeConfA, kPipeStride)) & kPipeEnable)) continue;
    if (!(r_.read32(pipe_reg(kPlaneCtlA, kPipeStride)) & kPlaneEnable)) continue;
    // The panel's size: the active part of the transcoder's timings (the low half of each total register holds active - 1).
    const uint32_t w = (r_.read32(pipe_reg(kTransHtotalA, kTransStride)) & 0xFFFF) + 1;
    const uint32_t h = (r_.read32(pipe_reg(kTransVtotalA, kTransStride)) & 0xFFFF) + 1;
    if (w < kMinSize || h < kMinSize || w > kMaxSize || h > kMaxSize) continue;
    native_w_ = w;
    native_h_ = h;
    active_ = true;
    original_ = save();
    return true;
  }
  return false;
}

bool Display::can_show(uint32_t width, uint32_t height) const {
  if (!active_ || width < kMinSize || height < kMinSize || width > native_w_ || height > native_h_) return false;
  // Limits of the scaler: at most 8x up in either direction.
  return uint64_t{native_w_} <= uint64_t{width} * kMaxUpscaleNumerator && uint64_t{native_h_} <= uint64_t{height} * kMaxUpscaleNumerator;
}

Snapshot Display::save() const {
  Snapshot s;
  s.valid = true;
  s.pipe_src = r_.read32(pipe_reg(kPipeSrcA, kTransStride));
  s.plane_ctl = r_.read32(pipe_reg(kPlaneCtlA, kPipeStride));
  s.plane_stride = r_.read32(pipe_reg(kPlaneStrideA, kPipeStride));
  s.plane_pos = r_.read32(pipe_reg(kPlanePosA, kPipeStride));
  s.plane_size = r_.read32(pipe_reg(kPlaneSizeA, kPipeStride));
  s.plane_surf = r_.read32(pipe_reg(kPlaneSurfA, kPipeStride));
  s.plane_offset = r_.read32(pipe_reg(kPlaneOffsetA, kPipeStride));
  s.scaler_ctl = r_.read32(pipe_reg(kScalerCtlA, kScalerStride));
  s.scaler_pos = r_.read32(pipe_reg(kScalerPosA, kScalerStride));
  s.scaler_size = r_.read32(pipe_reg(kScalerSizeA, kScalerStride));
  return s;
}

void Display::restore(const Snapshot& s) {
  if (!active_ || !s.valid) return;
  // The scaler and the pipe size first, the plane's registers next, and its surface last: that write is what makes the plane's registers take effect.
  r_.write32(pipe_reg(kScalerCtlA, kScalerStride), s.scaler_ctl);
  r_.write32(pipe_reg(kScalerPosA, kScalerStride), s.scaler_pos);
  r_.write32(pipe_reg(kScalerSizeA, kScalerStride), s.scaler_size);
  r_.write32(pipe_reg(kPipeSrcA, kTransStride), s.pipe_src);
  r_.write32(pipe_reg(kPlaneCtlA, kPipeStride), s.plane_ctl);
  r_.write32(pipe_reg(kPlaneStrideA, kPipeStride), s.plane_stride);
  r_.write32(pipe_reg(kPlanePosA, kPipeStride), s.plane_pos);
  r_.write32(pipe_reg(kPlaneSizeA, kPipeStride), s.plane_size);
  r_.write32(pipe_reg(kPlaneOffsetA, kPipeStride), s.plane_offset);
  r_.write32(pipe_reg(kPlaneSurfA, kPipeStride), s.plane_surf);
}

bool Display::set_mode(uint32_t width, uint32_t height, uint64_t surface, uint32_t stride_bytes) {
  if (!can_show(width, height) || surface == 0 || surface >= (uint64_t{1} << 32) || (surface & 0xFFF) || stride_bytes % 64 != 0 ||
      stride_bytes < uint64_t{width} * 4)
    return false;
  const Snapshot before = save();

  uint32_t ctl = before.plane_ctl;
  ctl &= ~(kPlaneFormatMask | kPlaneRgbOrder | kPlaneTilingMask | kPlaneRenderCompression);
  if (architecture_ == IntelGpuArchitecture::XeLp) ctl &= ~kPlaneMediaCompression;
  ctl |= kPlaneFormatXrgb8888 | kPlaneEnable;

  const bool scaled = width != native_w_ || height != native_h_;
  r_.write32(pipe_reg(kScalerCtlA, kScalerStride), scaled ? kScalerEnable : 0);
  r_.write32(pipe_reg(kScalerPosA, kScalerStride), 0);
  r_.write32(pipe_reg(kScalerSizeA, kScalerStride), scaled ? (native_w_ << 16) | native_h_ : 0);
  r_.write32(pipe_reg(kPipeSrcA, kTransStride), ((width - 1) << 16) | (height - 1));
  r_.write32(pipe_reg(kPlaneCtlA, kPipeStride), ctl);
  r_.write32(pipe_reg(kPlaneStrideA, kPipeStride), stride_bytes / 64);
  r_.write32(pipe_reg(kPlanePosA, kPipeStride), 0);
  r_.write32(pipe_reg(kPlaneSizeA, kPipeStride), ((height - 1) << 16) | (width - 1));
  r_.write32(pipe_reg(kPlaneOffsetA, kPipeStride), 0);
  r_.write32(pipe_reg(kPlaneSurfA, kPipeStride), static_cast<uint32_t>(surface));

  const bool took = r_.read32(pipe_reg(kPipeSrcA, kTransStride)) == (((width - 1) << 16) | (height - 1)) &&
                    (r_.read32(pipe_reg(kPlaneSurfA, kPipeStride)) & ~0xFFFu) == surface && r_.read32(pipe_reg(kPlaneStrideA, kPipeStride)) == stride_bytes / 64 &&
                    (r_.read32(pipe_reg(kPlaneCtlA, kPipeStride)) & kPlaneEnable) && (r_.read32(pipe_reg(kPipeConfA, kPipeStride)) & kPipeEnable);
  if (!took) {
    restore(before);
    return false;
  }
  return true;
}

}  // namespace intel_display
}  // namespace hardware
