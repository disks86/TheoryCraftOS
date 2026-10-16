// Picks the hardware classes for the architecture this image is built for.
// SystemCpu picks Intel/AMD at runtime; SystemRam is the RAM class for the
// architecture. main.cpp never names a platform.
#pragma once

#if defined(__x86_64__)
#include "hardware/CentralProcessingUnit/SystemCpu.hpp"
#include "hardware/RandomAccessMemory/UefiRandomAccessMemory.hpp"

namespace hardware {
using SystemRam = UefiRandomAccessMemory;
}  // namespace hardware
#else
#error "No hardware classes for this architecture"
#endif
