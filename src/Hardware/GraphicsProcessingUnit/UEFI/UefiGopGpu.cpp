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

bool UefiGopGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (connector != 0 || !out) return false;
  *out = ConnectorInfo();
  out->connected = gop_ != nullptr;
  out->type = ConnectorType::Unknown;   // (the firmware does not say which connector it is)
  const char name[] = "Display-0";
  for (uint32_t i = 0; i < sizeof name; ++i) out->name[i] = name[i];
  out->preferred_width = width();
  out->preferred_height = height();
  return true;
}

// The firmware keeps the EDID it is using (active; it may be one it was given) and the one it read from the display (discovered).
uint32_t UefiGopGpu::read_edid(uint32_t connector, ByteView out) {
  efi::BootServices* bs = st_ ? st_->boot_services : nullptr;
  if (connector != 0 || !bs) return 0;
  const efi::Guid* guids[2] = {&efi::kEdidActiveGuid, &efi::kEdidDiscoveredGuid};
  for (const efi::Guid* guid : guids) {
    efi::EdidProtocol* edid = nullptr;
    if (bs->locate_protocol(guid, nullptr, reinterpret_cast<void**>(&edid)) != efi::kSuccess || !edid || !edid->edid || edid->size_of_edid < 128) continue;
    const uint32_t n = edid->size_of_edid < out.size() ? edid->size_of_edid : static_cast<uint32_t>(out.size());
    for (uint32_t i = 0; i < n; ++i) out[i] = edid->edid[i];
    return n;
  }
  return 0;
}

bool UefiGopGpu::mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
  if (connector != 0) return false;
  for (uint32_t i = 0; i < mode_count_; ++i)
    if (size_[i].width == width && size_[i].height == height) return true;
  return false;
}

uint32_t UefiGopGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
  if (connector != 0) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < mode_count_ && n < capacity; ++i) {
    bool seen = false;
    for (uint32_t k = 0; k < n; ++k) seen = seen || (out[k].width == size_[i].width && out[k].height == size_[i].height);
    if (seen) continue;
    out[n] = MonitorMode();
    out[n].width = size_[i].width;
    out[n].height = size_[i].height;
    ++n;
  }
  return n;
}

bool UefiGopGpu::set_display_mode(uint32_t connector, uint32_t width, uint32_t height) {
  if (connector != 0) return false;
  for (uint32_t i = 0; i < mode_count_; ++i)
    if (size_[i].width == width && size_[i].height == height) return set_mode(i);
  return false;
}

bool UefiGopGpu::set_mode(uint32_t index) {
  if (!gop_ || index >= mode_count_) return false;
  if (gop_->set_mode(gop_, gop_mode_[index]) != efi::kSuccess) return false;
  current_ = index;
  return apply_current_mode();
}

}  // namespace hardware
