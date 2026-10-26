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

#pragma once
#include "Hardware/GraphicsProcessingUnit/Intel/IntelDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelScanout.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Intel display controller (PCI vendor 0x8086), identified by device ID down to its architecture (UHD 6xx, Iris Xe, Arc,
// Xe2, Xe3; see IntelGpuGenerations.hpp).
//
// The firmware (UEFI GOP) still starts it: it lists the modes, gives the first framebuffer and the EDID. On the integrated GPUs of Gen9.5 to Xe-LP the display
// engine is then taken over natively (IntelDisplay, IntelScanout): the first mode change moves the screen to a framebuffer of ours, and every mode after
// that is set through the display registers, not through the firmware. Anything native mode setting cannot do (another GPU, a state it does not recognise, the
// mapping failing, the engine not taking a mode before it ever took one) leaves the firmware path in charge, as before.
class IntelGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  explicit IntelGpu(const GpuContext& context)
      : UefiGopGpu(context), info_(intel_gpu_identify(context.device.device_id)), context_(context) {}

  const char* name() const override { return info_.name; }
  const IntelGpuInfo& generation() const { return info_; }

  bool initialize() override;
  void shutdown() override;
  uint32_t current_mode() const override { return native_active_ ? native_mode_ : UefiGopGpu::current_mode(); }
  bool set_mode(uint32_t index) override;
  // True once the screen is driven by the display engine (not the firmware).
  bool native_mode_setting() const { return native_active_; }
  // True if the display engine was found and could be driven (it is used from the first mode change).
  bool native_available() const { return scanout_ && scanout_->ready(); }

  // What is drawn has to be flushed out of the CPU's caches for the display engine to see it.
  void put_pixel(uint32_t x, uint32_t y, uint32_t color) override { UefiGopGpu::put_pixel(x, y, color); flush_dirty(); }
  void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) override { UefiGopGpu::fill_rect(x, y, w, h, color); flush_dirty(); }
  void copy_rect(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) override { UefiGopGpu::copy_rect(sx, sy, dx, dy, w, h); flush_dirty(); }
  void blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels, uint32_t pitch_pixels) override {
    UefiGopGpu::blit(x, y, w, h, pixels, pitch_pixels);
    flush_dirty();
  }
  void draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) override { UefiGopGpu::draw_char(x, y, c, fg, bg); flush_dirty(); }
  void draw_text(uint32_t x, uint32_t y, const char* text, uint32_t fg, uint32_t bg) override { UefiGopGpu::draw_text(x, y, text, fg, bg); flush_dirty(); }

 private:
  void try_native();
  void flush_dirty();

  IntelGpuInfo info_;
  GpuContext context_;
  intel_gt::MmioRegisterBank bank_{MmioRegion()};
  UniquePointer<intel_display::Scanout> scanout_;
  ByteView native_memory_;
  uint32_t native_pitch_ = 0;
  bool native_active_ = false;
  uint32_t native_mode_ = 0;
};

}  // namespace hardware
