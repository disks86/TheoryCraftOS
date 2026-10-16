#pragma once
#include "hardware/GraphicsProcessingUnit/FramebufferGpu.hpp"
#include "hardware/GraphicsProcessingUnit/GpuContext.hpp"

namespace hardware {

// Fallback GPU: drives the framebuffer the firmware set up through the UEFI
// Graphics Output Protocol. Mode setting is delegated to the firmware, so it
// works on any display hardware but only offers the modes the firmware lists
// (32-bit RGB/BGR ones). The vendor GPU classes build on this until they
// grow native drivers.
class UefiGopGpu : public FramebufferGpu {
 public:
  static constexpr uint32_t kMaxModes = 64;

  explicit UefiGopGpu(const GpuContext& context) : st_(context.st) {}

  const char* name() const override { return "UEFI framebuffer"; }
  bool initialize() override;
  void shutdown() override {}

  uint32_t mode_count() const override { return mode_count_; }
  bool mode_info(uint32_t index, DisplayMode* out) const override;
  uint32_t current_mode() const override { return current_; }
  bool set_mode(uint32_t index) override;

 private:
  bool apply_current_mode();

  efi::SystemTable* st_;
  efi::GraphicsOutput* gop_ = nullptr;
  uint32_t gop_mode_[kMaxModes] = {};  // our index -> GOP mode number
  DisplayMode size_[kMaxModes] = {};
  uint32_t mode_count_ = 0;
  uint32_t current_ = 0;
};

}  // namespace hardware
