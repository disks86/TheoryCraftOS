#pragma once
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

namespace hardware {

class X86_64Cpu final : public ICentralProcessingUnit {
 public:
  constexpr X86_64Cpu() = default;

  const char* name() const override { return "x86_64 CPU"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  void halt() const override;
  void pause() const override;
  void breakpoint() const override;
};

}  // namespace hardware
