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

#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

namespace {
bool usable(const efi::GraphicsModeInfo& info) {
  return info.pixel_format == efi::PixelFormat::RedGreenBlueReserved8Bit ||
         info.pixel_format == efi::PixelFormat::BlueGreenRedReserved8Bit;
}
}  // namespace

bool UefiGopGpu::initialize() {
  efi::BootServices* bs = st_ ? st_->boot_services : nullptr;
  if (!bs || bs->locate_protocol(&efi::kGraphicsOutputGuid, nullptr,
                                 reinterpret_cast<void**>(&gop_)) != efi::kSuccess)
    return false;

  mode_count_ = 0;
  for (uint32_t m = 0; m < gop_->mode->max_mode && mode_count_ < kMaxModes; ++m) {
    efi::Uintn size = 0;
    efi::GraphicsModeInfo* info = nullptr;
    if (gop_->query_mode(gop_, m, &size, &info) != efi::kSuccess) continue;
    if (usable(*info)) {
      gop_mode_[mode_count_] = m;
      size_[mode_count_] = {info->horizontal_resolution, info->vertical_resolution};
      if (m == gop_->mode->mode) current_ = mode_count_;
      ++mode_count_;
    }
    bs->free_pool(info);
  }
  return mode_count_ != 0 && apply_current_mode();
}

bool UefiGopGpu::apply_current_mode() {
  const efi::GraphicsMode* mode = gop_->mode;
  if (!usable(*mode->info) || mode->frame_buffer_base == 0) return false;
  set_framebuffer(ByteView(reinterpret_cast<uint8_t*>(mode->frame_buffer_base), mode->frame_buffer_size),
                  mode->info->horizontal_resolution, mode->info->vertical_resolution,
                  mode->info->pixels_per_scan_line * 4,
                  mode->info->pixel_format == efi::PixelFormat::BlueGreenRedReserved8Bit);
  return true;
}

bool UefiGopGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (index >= mode_count_ || !out) return false;
  *out = size_[index];
  return true;
}

bool UefiGopGpu::set_mode(uint32_t index) {
  if (!gop_ || index >= mode_count_) return false;
  if (gop_->set_mode(gop_, gop_mode_[index]) != efi::kSuccess) return false;
  current_ = index;
  return apply_current_mode();
}

}  // namespace hardware
