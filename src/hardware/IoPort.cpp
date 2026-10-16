#include "hardware/IoPort.hpp"
#include "hardware/x86_64/asm.hpp"

namespace hardware {
uint8_t IoPort::read8() const { return tcos_inb(port_); }
void IoPort::write8(uint8_t value) const { tcos_outb(port_, value); }
uint16_t IoPort::read16() const { return tcos_inw(port_); }
void IoPort::write16(uint16_t value) const { tcos_outw(port_, value); }
uint32_t IoPort::read32() const { return tcos_inl(port_); }
void IoPort::write32(uint32_t value) const { tcos_outl(port_, value); }
}  // namespace hardware
