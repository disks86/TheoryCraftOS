#pragma once
#include "hardware/IHardware.hpp"

namespace hardware {

// A processor core the OS runs on. Implementations live beside this file.
class ICentralProcessingUnit : public IHardware {
 public:
  virtual void halt() const = 0;        // stop until the next interrupt
  virtual void pause() const = 0;       // spin-wait hint
  virtual void breakpoint() const = 0;  // trap into an attached debugger

 protected:
  constexpr ICentralProcessingUnit() = default;
  ~ICentralProcessingUnit() = default;
};

}  // namespace hardware
