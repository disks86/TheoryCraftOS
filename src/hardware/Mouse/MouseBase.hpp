#pragma once
#include "hardware/Mouse/IMouse.hpp"

namespace hardware {

// Event queue shared by mice. A subclass implements fetch(), which reads
// whatever the device has and calls push() for each event.
class MouseBase : public IMouse {
 public:
  bool poll(MouseEvent* out) override;

 protected:
  constexpr MouseBase() = default;
  ~MouseBase() = default;

  virtual void fetch() = 0;
  void push(const MouseEvent& event);

 private:
  static constexpr uint32_t kCapacity = 64;
  MouseEvent queue_[kCapacity] = {};
  uint32_t head_ = 0, count_ = 0;
};

}  // namespace hardware
