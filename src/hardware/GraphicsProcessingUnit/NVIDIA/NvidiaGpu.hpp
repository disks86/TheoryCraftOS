#pragma once
#include "hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// NVIDIA display controller (PCI vendor 0x10DE). Modes and the framebuffer still
// come from the firmware (UEFI GOP); native mode setting for these parts is
// future work, and this class is where it will go.
class NvidiaGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;

  explicit NvidiaGpu(const GpuContext& context) : UefiGopGpu(context) {}

  const char* name() const override { return "NVIDIA graphics (UEFI framebuffer)"; }
};

}  // namespace hardware
