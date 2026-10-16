#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/NeuralProcessingUnit/INeuralProcessingUnit.hpp"

// Checks an NPU against straightforward reference code on the calling core:
// an 8-bit and a floating-point matrix product, then softmax. Logs one line per
// check to the console and returns true if all of them pass.
bool run_npu_demo(hardware::INeuralProcessingUnit& npu, allocators::IAllocator& allocator);
