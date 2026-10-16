#pragma once
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

namespace hardware {

// Owns the CPU class matching the machine we booted on. The class is chosen
// from the CPUID vendor and family/model/stepping (a microarchitecture such
// as IntelSkylakeClientCpu or AmdZen3Cpu), falling back to the vendor class,
// or to QemuX64Cpu for QEMU's generic models. Storage is inline because there
// is no heap yet; every CPU class adds no data over X86_64Cpu.
class SystemCpu {
 public:
  explicit SystemCpu(efi::SystemTable* st);

  ICentralProcessingUnit& cpu() { return *active_; }

 private:
  alignas(16) unsigned char storage_[sizeof(X86_64Cpu)];
  ICentralProcessingUnit* active_;
};

}  // namespace hardware
