#pragma once
#include "hardware/Bus/Ps2/I8042Controller.hpp"
#include "hardware/Mouse/MouseBase.hpp"

namespace hardware {

// A PS/2 mouse on the i8042's second port, with the IntelliMouse extension
// (scroll wheel) when the mouse supports it.
class Ps2Mouse final : public MouseBase {
 public:
  explicit constexpr Ps2Mouse(I8042Controller& controller) : controller_(controller) {}

  const char* name() const override { return "PS/2 mouse"; }
  bool initialize() override;  // false if no mouse answers
  void shutdown() override {}

  bool is_absolute() const override { return false; }
  uint32_t max_x() const override { return 0; }
  uint32_t max_y() const override { return 0; }

 private:
  void fetch() override;

  I8042Controller& controller_;
  uint8_t packet_[4] = {};
  uint32_t packet_length_ = 3;
  uint32_t filled_ = 0;
};

}  // namespace hardware
