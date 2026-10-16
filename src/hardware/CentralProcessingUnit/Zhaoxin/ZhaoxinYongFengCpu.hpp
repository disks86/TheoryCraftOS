#pragma once
#include "hardware/CentralProcessingUnit/Zhaoxin/ZhaoxinX64Cpu.hpp"

namespace hardware {

// KX-7000 series. QEMU: YongFeng.
class ZhaoxinYongFengCpu final : public ZhaoxinX64Cpu {
 public:
  explicit ZhaoxinYongFengCpu(efi::SystemTable* st) : ZhaoxinX64Cpu(st) {}
  const char* name() const override { return "Zhaoxin YongFeng"; }
};

}  // namespace hardware
