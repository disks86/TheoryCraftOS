#pragma once
#include "hardware/StorageDevice/SystemStorageDevices.hpp"

// Exercises every storage device: reports what it is and how big, reads its first
// and last blocks, checks that one large read equals the same range read in
// pieces, and - only on a device whose first block begins with the marker
// "TCOS-SCRATCH" - writes, flushes and reads back a test pattern on its last
// block (restoring it afterwards). Other devices are never written to.
// Returns false if any check failed.
bool run_storage_demo(hardware::SystemStorageDevices& storage, allocators::IAllocator& allocator);
