// Picks the hardware classes for the architecture this image is built for.
// main.cpp constructs SystemCpu and SystemRam without naming a platform.
#pragma once

#if defined(__x86_64__)
#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"
#include "hardware/RandomAccessMemory/UefiRandomAccessMemory.hpp"

namespace hardware {
using SystemCpu = X86_64Cpu;
using SystemRam = UefiRandomAccessMemory;
}  // namespace hardware
#else
#error "No hardware classes for this architecture"
#endif
