#pragma once
#include "hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// AMD display controller (PCI vendor 0x1002). Modes and the framebuffer still
// come from the firmware (UEFI GOP); native mode setting for these parts is
// future work, and this class is where it will go.
class AmdGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x1002;

  explicit AmdGpu(const GpuContext& context) : UefiGopGpu(context) {}

  const char* name() const override { return "AMD graphics (UEFI framebuffer)"; }
};

}  // namespace hardware
