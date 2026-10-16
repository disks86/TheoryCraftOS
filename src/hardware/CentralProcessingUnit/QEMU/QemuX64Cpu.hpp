#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// A CPU emulated by QEMU's TCG (no hardware passthrough). It borrows an
// Intel or AMD vendor string, so it is recognized by the hypervisor leaf
// instead. Cores are uniform. With KVM the real vendor class is used.
class QemuX64Cpu final : public X86_64Cpu {
 public:
  explicit QemuX64Cpu(efi::SystemTable* st) : X86_64Cpu(st) {}
  const char* name() const override { return "QEMU x64 CPU"; }
};

}  // namespace hardware
