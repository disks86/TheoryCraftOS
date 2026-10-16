#pragma once
#include "Allocators/Allocator.hpp"
#include "CPUScheduler/ICPUScheduler.hpp"

// Exercises UniquePointer, SharedPointer, Optional and Variant: ownership of a
// derived object through a base without a virtual destructor, a SharedPointer
// copied and dropped concurrently on every core, and value types holding
// nontrivial contents. Logs one line per check and verifies that the allocator
// ends with the memory it started with. Returns true if everything passed.
bool run_utility_demo(allocators::Allocator& allocator, scheduling::ICPUScheduler& scheduler);
