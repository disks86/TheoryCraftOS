// Declarations for the assembly in port.S (private to hardware/).
#pragma once
#include <stdint.h>

extern "C" {
void tcos_outb(uint16_t port, uint8_t value);
uint8_t tcos_inb(uint16_t port);
void tcos_outw(uint16_t port, uint16_t value);
uint16_t tcos_inw(uint16_t port);
void tcos_outl(uint16_t port, uint32_t value);
uint32_t tcos_inl(uint16_t port);
}
