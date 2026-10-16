#pragma once
#include "hardware/IHardware.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

// QEMU's isa-debugcon device: bytes written to port 0x402 reach the host.
// Harmless on real hardware (the write is ignored).
class DebugConsole final : public IHardware {
 public:
  constexpr DebugConsole() = default;

  const char* name() const override { return "QEMU debugcon"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  void put(char c) const { port_.write8(static_cast<uint8_t>(c)); }

 private:
  static constexpr uint16_t kPort = 0x402;
  IoPort port_{kPort};
};

}  // namespace hardware
