#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon Scalable 4th gen. QEMU: SapphireRapids.
class IntelSapphireRapidsCpu final : public IntelX64Cpu {
 public:
  explicit IntelSapphireRapidsCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Sapphire Rapids"; }
};

}  // namespace hardware
