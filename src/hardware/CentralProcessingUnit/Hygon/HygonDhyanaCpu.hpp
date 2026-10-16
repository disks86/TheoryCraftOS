#pragma once
#include "hardware/CentralProcessingUnit/Hygon/HygonX64Cpu.hpp"

namespace hardware {

// Zen-based Hygon C86. QEMU: Dhyana.
class HygonDhyanaCpu final : public HygonX64Cpu {
 public:
  explicit HygonDhyanaCpu(efi::SystemTable* st) : HygonX64Cpu(st) {}
  const char* name() const override { return "Hygon Dhyana"; }
};

}  // namespace hardware
