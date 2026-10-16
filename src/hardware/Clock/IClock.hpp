#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// A monotonic clock: time since some fixed moment (boot), never going backwards.
// It tells how long things took, not what the date is.
class IClock : public IHardware {
 public:
  virtual uint64_t now_us() const = 0;
  uint64_t now_ms() const { return now_us() / 1000; }

 protected:
  constexpr IClock() = default;
  ~IClock() = default;
};

}  // namespace hardware
