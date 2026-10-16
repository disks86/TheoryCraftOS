#include "hardware/Mouse/Ps2/Ps2Mouse.hpp"

namespace hardware {

bool Ps2Mouse::initialize() {
  filled_ = 0;
  packet_length_ = 3;
  if (!controller_.send_mouse(0xF6)) return false;  // set defaults

  // The magic sample-rate sequence 200, 100, 80 switches on the wheel; the
  // device then identifies as 3 instead of 0.
  const uint8_t rates[3] = {200, 100, 80};
  for (uint8_t rate : rates)
    if (!controller_.send_mouse(0xF3) || !controller_.send_mouse(rate)) return false;
  if (!controller_.send_mouse(0xF2)) return false;  // get device ID
  uint8_t id = 0;
  for (uint32_t i = 0; i < 200000; ++i)
    if (controller_.read_mouse(&id)) break;
  if (id == 3 || id == 4) packet_length_ = 4;

  return controller_.send_mouse(0xF4);  // enable streaming
}

void Ps2Mouse::fetch() {
  uint8_t byte;
  while (controller_.read_mouse(&byte)) {
    if (filled_ == 0 && !(byte & 0x08)) continue;  // bit 3 is always set in byte 0: resync
    packet_[filled_++] = byte;
    if (filled_ < packet_length_) continue;
    filled_ = 0;

    const uint8_t flags = packet_[0];
    MouseEvent e{};
    e.buttons = flags & 0x07;
    e.dx = static_cast<int32_t>(packet_[1]) - ((flags << 4) & 0x100);  // 9-bit two's complement
    e.dy = -(static_cast<int32_t>(packet_[2]) - ((flags << 3) & 0x100));  // PS/2 +y is up
    if (packet_length_ == 4) e.wheel = -static_cast<int8_t>(packet_[3]);   // PS/2 +z is toward the user
    e.absolute = false;
    push(e);
  }
}

}  // namespace hardware
