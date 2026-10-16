#include "hardware/IoPort.hpp"
#include "hardware/x86_64/asm.hpp"

namespace hardware {
uint8_t IoPort::read8() const { return tcos_inb(port_); }
void IoPort::write8(uint8_t value) const { tcos_outb(port_, value); }
}  // namespace hardware
