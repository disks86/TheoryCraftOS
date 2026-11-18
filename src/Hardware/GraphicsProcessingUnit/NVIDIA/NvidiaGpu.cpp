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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaNative.hpp"

namespace hardware {

NvidiaGpu::NvidiaGpu(const GpuContext& context) : UefiGopGpu(context), info_(nvidia_gpu_identify(context.device.device_id)), context_(context) {}

NvidiaGpu::~NvidiaGpu() = default;

const char* NvidiaGpu::name() const {
  // A chip the registers named, when the device ID table does not know the card (anything before Pascal, or an ID newer than the table).
  if (info_.architecture == NvidiaGpuArchitecture::Unknown && name_[0]) return name_;
  return info_.name;
}

bool NvidiaGpu::initialize() {
  if (!UefiGopGpu::initialize()) return false;
  if (context_.allocator && context_.registers) {
    native_ = UniquePointer<nv::NvidiaNative>::make(*context_.allocator, context_, *context_.allocator);
    if (native_ && !native_->start()) native_ = UniquePointer<nv::NvidiaNative>();
  }
  if (native_) {
    const nv::Chip& chip = native_->chip();
    uint32_t n = 0;
    for (const char* p = "NVIDIA "; *p && n + 1 < sizeof name_; ++p) name_[n++] = *p;
    for (const char* p = chip.name; *p && n + 1 < sizeof name_; ++p) name_[n++] = *p;
    for (const char* p = " ("; *p && n + 1 < sizeof name_; ++p) name_[n++] = *p;
    for (const char* p = nv::family_name(chip.family); *p && n + 1 < sizeof name_; ++p) name_[n++] = *p;
    if (n + 1 < sizeof name_) name_[n++] = ')';
    name_[n] = 0;
    if (native_->vram_bytes()) set_video_memory_total(native_->vram_bytes());
  }
  engage();
  return true;
}

bool NvidiaGpu::native_scanout() const { return native_ && native_->engaged(); }

void NvidiaGpu::engage() {
#ifdef TCOS_NVIDIA_NATIVE
  if (!native_ || native_scanout()) return;
  uint64_t base = 0, bytes = 0;
  if (!UefiGopGpu::framebuffer_range(&base, &bytes) || pitch_bytes() % 4 != 0) return;
  nv::NvFirmwareFramebuffer fb;
  // (The firmware identity-maps its framebuffer: framebuffer_range's address is a pointer as well.)
  fb.pixels = ByteView(reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(base)), bytes);
  fb.width = width();
  fb.height = height();
  fb.pitch_bytes = pitch_bytes();
  ByteView memory;
  if (!native_->engage(fb, base, &memory)) return;     // (stays on the firmware's framebuffer; native()->status() says why)
  set_framebuffer(memory, width(), height(), pitch_bytes(), bgr());
#endif
}

void NvidiaGpu::release() {
  if (!native_scanout()) return;
  native_->disengage();
  apply_current_mode();
}

bool NvidiaGpu::set_mode(uint32_t index) {
  release();                                            // (the firmware changes the mode on its own framebuffer)
  const bool ok = UefiGopGpu::set_mode(index);
  engage();
  return ok;
}

void NvidiaGpu::shutdown() {
  release();
  native_ = UniquePointer<nv::NvidiaNative>();
  UefiGopGpu::shutdown();
}

int32_t NvidiaGpu::temperature_millicelsius() { return native_ ? native_->temperature_millicelsius() : -1; }

}  // namespace hardware
