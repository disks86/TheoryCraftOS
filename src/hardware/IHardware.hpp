// Base interface for everything that touches hardware (CPU, ports, devices,
// firmware services). Freestanding: no RTTI, exceptions or heap, so the
// destructor is protected and non-virtual - never delete through IHardware.
#pragma once

namespace hardware {

class IHardware {
 public:
  // Short human-readable device name, for logs.
  virtual const char* name() const = 0;
  // Bring the device to a usable state. Returns false if it is unavailable.
  virtual bool initialize() = 0;
  // Quiesce the device. Safe to call when not initialized.
  virtual void shutdown() = 0;

 protected:
  constexpr IHardware() = default;
  ~IHardware() = default;
};

}  // namespace hardware
