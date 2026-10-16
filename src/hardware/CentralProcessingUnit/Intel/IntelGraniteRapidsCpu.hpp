#pragma once
#include "hardware/CentralProcessingUnit/Intel/IntelX64Cpu.hpp"

namespace hardware {

// Xeon 6 (P-cores). QEMU: GraniteRapids.
class IntelGraniteRapidsCpu final : public IntelX64Cpu {
 public:
  explicit IntelGraniteRapidsCpu(efi::SystemTable* st) : IntelX64Cpu(st) {}
  const char* name() const override { return "Intel Granite Rapids"; }
};

}  // namespace hardware
