#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// Something devices hang off: PCI, PCI Express, USB, ... Each bus type adds
// its own addressing and access functions in a more specific interface.
class IBus : public IHardware {
 public:
  // Devices found by initialize().
  virtual uint32_t device_count() const = 0;

 protected:
  constexpr IBus() = default;
  ~IBus() = default;
};

}  // namespace hardware
