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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdScanout.hpp"
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// AMD display controller (PCI vendor 0x1002), identified by device ID down to its architecture (Vega APUs, RDNA 1 to 4, Polaris; see AmdGpuGenerations.hpp).
//
// Modes and the first framebuffer come from the firmware (UEFI GOP), and so does every mode change. On the Raven-family APUs (Ryzen 2000/3000: device 15D8 and 15DD)
// the display engine (DCN 1.0) is then pointed at a framebuffer of the OS's own in the video memory carve-out (AmdDcn1, AmdScanout): the firmware's pixel format,
// pitch, timings and everything the Display Mode Library computed for its mode stay as they are, and only the plane's surface address changes. After each mode change
// the framebuffer is taken over again. Anything that does not look as expected (another GPU, a register that disagrees with the memory map, no room in the carve-out, the
// engine not taking the address) leaves the firmware's framebuffer in use, as before. Resolution choices beyond the firmware's modes, and the other AMD GPUs, need the
// Display Mode Library, which this OS does not have.
class AmdGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x1002;

  explicit AmdGpu(const GpuContext& context) : UefiGopGpu(context), info_(amd_gpu_identify(context.device.device_id)), context_(context) {}

  const char* name() const override { return info_.name; }
  const AmdGpuInfo& generation() const { return info_; }

  bool initialize() override;
  void shutdown() override;
  bool set_mode(uint32_t index) override;
  // The firmware's framebuffer, also while the display engine shows ours: that is the memory the firmware's part of video memory covers.
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override;

  // True while the display engine shows the OS's framebuffer.
  bool native_scanout() const { return (scanout_ && scanout_->engaged()) || (native_display_ && native_display_->display_engaged()); }
  // True if the display engine was found and could be driven.
  bool native_available() const { return scanout_ != nullptr || native_display_ != nullptr; }
  // A display engine of another kind that shows the OS's framebuffer (the Radeon RX Vega M's DCE 11.2: AmdVegaM). Not owned: it must stay until it is taken away again
  // with nullptr. The screen is taken over at once for the current mode; taking it away gives the firmware's framebuffer back.
  void set_native_display(amd_dce11::INativeDisplay* display);
  // Bytes of video memory the OS's framebuffer needs, to be kept clear of other uses (the video engine's memory); 0 if it is not used.
  uint64_t reserved_video_memory() const { return reserved_; }

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
  void prepare_native();
  void engage();
  void flush_dirty();

  AmdGpuInfo info_;
  GpuContext context_;
  MmioVcnRegisters registers_{MmioRegion()};
  UniquePointer<amd_dcn1::Scanout> scanout_;
  amd_dce11::INativeDisplay* native_display_ = nullptr;
  uint64_t reserved_ = 0;
  uint64_t firmware_base_ = 0, firmware_bytes_ = 0;
  ByteView native_memory_;
  uint32_t native_pitch_ = 0;
};

}  // namespace hardware
