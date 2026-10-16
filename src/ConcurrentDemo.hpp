#pragma once
#include "Allocators/Allocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

// Fills the Concurrent* containers from scheduler tasks running on every core at
// once (through a ThreadSafeAllocator, since the default allocator is not safe to
// share), then checks nothing was lost, duplicated or leaked. Logs one line per
// check and returns true if all of them pass.
bool run_concurrent_demo(allocators::Allocator& allocator, hardware::ICentralProcessingUnit& cpu,
                         scheduling::ICPUScheduler& scheduler);
