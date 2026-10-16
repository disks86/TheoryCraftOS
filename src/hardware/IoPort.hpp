#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"

namespace hardware {

// One x86 I/O port (in/out instructions).
class IoPort final : public IHardware {
 public:
  explicit constexpr IoPort(uint16_t port) : port_(port) {}

  const char* name() const override { return "I/O port"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  uint16_t port() const { return port_; }
  uint8_t read8() const;
  void write8(uint8_t value) const;

 private:
  uint16_t port_;
};

}  // namespace hardware
