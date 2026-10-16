#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon 7 (P-cores). QEMU: DiamondRapids.
class IntelDiamondRapidsCpu final : public IntelX64Cpu {
 public:
  explicit IntelDiamondRapidsCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Diamond Rapids"; }
};

}  // namespace hardware
