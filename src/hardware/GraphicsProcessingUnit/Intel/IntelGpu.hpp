#pragma once
#include "hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// Intel display controller (PCI vendor 0x8086). Modes and the framebuffer still
// come from the firmware (UEFI GOP); native mode setting for these parts is
// future work, and this class is where it will go.
class IntelGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  explicit IntelGpu(const GpuContext& context) : UefiGopGpu(context) {}

  const char* name() const override { return "Intel graphics (UEFI framebuffer)"; }
};

}  // namespace hardware
