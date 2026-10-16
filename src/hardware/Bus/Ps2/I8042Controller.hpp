#pragma once
#include <stdint.h>
#include "hardware/IHardware.hpp"
#include "hardware/IoPort.hpp"

namespace hardware {

// The 8042 PS/2 controller (data port 0x60, status/command port 0x64): a tiny
// bus carrying one keyboard and one mouse. Both share the output buffer, so the
// controller sorts incoming bytes into one queue per device. Everything is
// polled - the controller's IRQs are left disabled.
//
// The firmware also polls this controller from timer interrupts; mask
// interrupts (ICentralProcessingUnit::disable_interrupts) before relying on it.
class I8042Controller final : public IHardware {
 public:
  constexpr I8042Controller() = default;

  const char* name() const override { return "i8042 PS/2 controller"; }
  bool initialize() override;  // false if there is no controller
  void shutdown() override {}

  // Non-blocking: the next byte the keyboard / mouse sent, if any.
  bool read_keyboard(uint8_t* out);
  bool read_mouse(uint8_t* out);

  // Sends a command byte to a device and waits for its ACK (0xFA).
  bool send_keyboard(uint8_t byte);
  bool send_mouse(uint8_t byte);

 private:
  struct Queue {
    static constexpr uint32_t kCapacity = 32;
    uint8_t bytes[kCapacity] = {};
    uint32_t head = 0, count = 0;
    void push(uint8_t b) {
      if (count < kCapacity) bytes[(head + count++) % kCapacity] = b;
    }
    bool pop(uint8_t* out) {
      if (!count) return false;
      *out = bytes[head];
      head = (head + 1) % kCapacity;
      --count;
      return true;
    }
  };

  void pump();                       // moves pending hardware bytes into the queues
  bool wait_input_clear() const;     // controller ready to accept a byte
  bool write_command(uint8_t command) const;
  bool write_data(uint8_t byte) const;
  bool read_data_blocking(uint8_t* out, bool from_mouse);
  bool send(uint8_t byte, bool to_mouse);

  IoPort data_{0x60};
  IoPort status_command_{0x64};
  Queue keyboard_, mouse_;
};

}  // namespace hardware
