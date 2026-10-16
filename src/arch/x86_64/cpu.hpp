// Thin C++ declarations for the assembly helpers in cpu.S.
#pragma once
#include <stdint.h>

extern "C" {
void tcos_outb(uint16_t port, uint8_t value);
uint8_t tcos_inb(uint16_t port);
void tcos_halt();        // sti; hlt
void tcos_pause();       // spin-loop hint
void tcos_breakpoint();  // int3
}
