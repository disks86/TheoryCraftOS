#pragma once
#include "Allocators/IAllocator.hpp"
#include "hardware/Bus/Usb/Xhci/XhciRegisters.hpp"

namespace hardware::xhci {

constexpr uint32_t kRingTrbs = 256;  // 4 KiB: never crosses a 64 KiB boundary

// A ring software writes and the controller reads (command and transfer
// rings). The last slot holds a Link TRB back to the start.
class ProducerRing {
 public:
  bool init(allocators::IAllocator& allocator);
  void release(allocators::IAllocator& allocator);

  uint64_t address() const { return reinterpret_cast<uint64_t>(trbs_); }
  uint32_t cycle() const { return cycle_; }

  // Appends a TRB (the cycle bit is added) and returns its address.
  uint64_t push(uint64_t parameter, uint32_t status, uint32_t control);

 private:
  Trb* trbs_ = nullptr;
  uint32_t index_ = 0;
  uint32_t cycle_ = 1;
};

// The ring the controller writes events to and software consumes.
class EventRing {
 public:
  bool init(allocators::IAllocator& allocator);
  void release(allocators::IAllocator& allocator);

  uint64_t address() const { return reinterpret_cast<uint64_t>(trbs_); }
  uint64_t segment_table_address() const { return reinterpret_cast<uint64_t>(table_); }
  // Address of the next TRB to consume (the event ring dequeue pointer).
  uint64_t dequeue_address() const { return address() + uint64_t{index_} * sizeof(Trb); }

  bool pop(Trb* out);

 private:
  struct SegmentEntry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
  };

  Trb* trbs_ = nullptr;
  SegmentEntry* table_ = nullptr;
  uint32_t index_ = 0;
  uint32_t cycle_ = 1;
};

}  // namespace hardware::xhci
