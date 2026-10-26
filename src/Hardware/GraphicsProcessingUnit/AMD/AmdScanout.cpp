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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdScanout.hpp"
#include "Hardware/GraphicsProcessingUnit/FramebufferFlush.hpp"

namespace hardware {
namespace amd_dcn1 {

namespace {
constexpr uint64_t kMiB = 1ull << 20;

// The one place a mapped window becomes a view of bytes. The window is memory the mapper handed out for this driver alone (it refuses a range the OS owns), at an
// address that is the same for the CPU as the OS's identity map says, and it outlives the view: the mapping is never torn down.
ByteView view_of(const MmioRegion& window, uint64_t length) {
  return ByteView(reinterpret_cast<uint8_t*>(window.address()), length);
}
// The firmware's framebuffer, which the firmware identity-maps (the UEFI graphics output gave its address).
ByteView firmware_view(const FirmwareFramebuffer& fb, uint64_t length) {
  return ByteView(reinterpret_cast<uint8_t*>(fb.physical), length);
}
}  // namespace

uint64_t Scanout::region_bytes(uint64_t max_framebuffer_bytes) { return (max_framebuffer_bytes + kMiB - 1) / kMiB * kMiB; }

bool Scanout::engage(const FirmwareFramebuffer& fb, ByteView* memory) {
  disengage();
  if (!map_.read || fb.width == 0 || fb.height == 0 || fb.pitch_pixels < fb.width || fb.physical == 0) return false;
  if (fb.pitch_pixels > UINT32_MAX / 4) return false;
  const uint64_t picture = uint64_t{fb.pitch_pixels} * 4 * fb.height;
  if (picture > fb.bytes || picture > reserved_) return false;
  const AmdDisplayRegion region = amd_plan_display_region(map_, reserved_, fb.physical, fb.bytes);
  if (!region.ok || fb.physical < map_.cpu_base) return false;

  // The firmware's framebuffer as the GPU addresses it must be what the display engine is fetching now: this is what ties the registers to the memory map.
  const uint64_t firmware_gpu = map_.gpu_base + (fb.physical - map_.cpu_base);
  if (!plane_.probe(firmware_gpu, fb.pitch_pixels) || !plane_.in_aperture(region.gpu_address, picture)) return false;

  MmioRegion window;
  if (!mapper_.map_registers(region.physical, region.bytes, &window) || window.length() < picture) return false;
  const ByteView ours = view_of(window, window.length());
  if (!ours.first(picture).copy_from(firmware_view(fb, picture))) return false;
  flush(ours, fb.pitch_pixels * 4, 0, 0, fb.width, fb.height);
  if (!plane_.set_surface(region.gpu_address, picture)) return false;

  firmware_ = fb;
  memory_ = ours.first(picture);
  *memory = memory_;
  engaged_ = true;
  return true;
}

void Scanout::disengage() {
  if (!engaged_) return;
  engaged_ = false;
  // What was drawn since goes back into the firmware's framebuffer, so the picture does not jump when the display engine goes back to it.
  firmware_view(firmware_, memory_.size()).copy_from(memory_);
  flush(firmware_view(firmware_, memory_.size()), firmware_.pitch_pixels * 4, 0, 0, firmware_.width, firmware_.height);
  plane_.restore();
  memory_ = ByteView();
}

void Scanout::flush(ByteView memory, uint32_t pitch_bytes, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  flush_framebuffer_rect(memory, pitch_bytes, x, y, width, height);
}

}  // namespace amd_dcn1
}  // namespace hardware
