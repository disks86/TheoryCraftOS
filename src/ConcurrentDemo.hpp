#pragma once
#include "Allocators/Allocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"
#include "hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"

// Fills the Concurrent* containers from scheduler tasks running on every core at
// once, then checks nothing was lost, duplicated or leaked. `shared` must be the
// thread-safe front of `allocator` that the rest of the system uses (the default
// allocator is not safe to share between cores). Logs one line per check and
// returns true if all of them pass.
bool run_concurrent_demo(allocators::Allocator& allocator, allocators::IAllocator& shared,
                         hardware::ICentralProcessingUnit& cpu, scheduling::ICPUScheduler& scheduler);
