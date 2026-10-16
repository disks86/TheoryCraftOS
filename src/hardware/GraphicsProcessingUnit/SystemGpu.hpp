#pragma once
#include "hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.hpp"
#include "hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

constexpr unsigned long GpuMaxOf(unsigned long a, unsigned long b) { return a > b ? a : b; }

// Finds the display controller on the PCI bus and brings up the best GPU class
// for it: a vendor/model class where one exists, else the UEFI framebuffer.
// Storage is inline because there is no heap yet.
class SystemGpu {
 public:
  // Tries candidates in order and keeps the first whose initialize() succeeds.
  SystemGpu(efi::SystemTable* st, const PciBus& pci);

  // The initialized GPU, or nullptr if no usable display was found.
  IGraphicsProcessingUnit* gpu() { return active_; }

 private:
  static constexpr unsigned long kSize =
      GpuMaxOf(sizeof(UefiGopGpu), sizeof(QemuStdVgaGpu));

  alignas(16) unsigned char storage_[kSize];
  IGraphicsProcessingUnit* active_ = nullptr;
};

}  // namespace hardware
