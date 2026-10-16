#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// System memory as seen by the OS. Implementations live beside this file.
class IRandomAccessMemory : public IHardware {
 public:
  virtual uint64_t total_bytes() const = 0;   // all installed memory
  virtual uint64_t usable_bytes() const = 0;  // memory the OS may allocate
  // Start of one contiguous, writable region of usable_bytes() bytes.
  virtual void* base() const = 0;

 protected:
  constexpr IRandomAccessMemory() = default;
  ~IRandomAccessMemory() = default;
};

}  // namespace hardware
