// Declarations for the assembly in port.S (private to hardware/).
#pragma once
#include <stdint.h>

extern "C" {
void tcos_outb(uint16_t port, uint8_t value);
uint8_t tcos_inb(uint16_t port);
}
