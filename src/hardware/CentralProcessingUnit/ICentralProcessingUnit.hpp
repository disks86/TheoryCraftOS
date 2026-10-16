#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// Performance cores for heavy work, efficiency cores for background work.
// CPUs without hybrid cores report every core as Performance.
enum class CoreKind : uint8_t { Performance, Efficiency };

using CoreFunction = void (*)(void* context);

// The processor package(s) the OS runs on. A "core" is a logical processor
// (hardware thread); core 0 is the one that booted the OS. Implementations
// live beside this file.
class ICentralProcessingUnit : public IHardware {
 public:
  virtual const char* model() const = 0;  // e.g. the CPUID brand string

  virtual uint32_t core_count() const = 0;           // logical processors
  virtual uint32_t current_core() const = 0;         // core running this call
  virtual CoreKind core_kind(uint32_t core) const = 0;

  // Starts fn(context) on another core and returns without waiting. The
  // function must not call firmware services. False if the core is the
  // caller, unusable, busy or out of range.
  virtual bool start_on_core(uint32_t core, CoreFunction fn, void* context) = 0;
  // True while a function started on `core` is still running.
  virtual bool core_busy(uint32_t core) = 0;

  virtual void halt() const = 0;        // stop until the next interrupt
  virtual void pause() const = 0;       // spin-wait hint
  virtual void breakpoint() const = 0;  // trap into an attached debugger

 protected:
  constexpr ICentralProcessingUnit() = default;
  ~ICentralProcessingUnit() = default;
};

}  // namespace hardware
