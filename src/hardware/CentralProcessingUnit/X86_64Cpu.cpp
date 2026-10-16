#include "hardware/CentralProcessingUnit/X86_64Cpu.hpp"

// Implemented in X86_64Cpu.S.
extern "C" {
void tcos_halt();
void tcos_pause();
void tcos_breakpoint();
}

namespace hardware {
void X86_64Cpu::halt() const { tcos_halt(); }
void X86_64Cpu::pause() const { tcos_pause(); }
void X86_64Cpu::breakpoint() const { tcos_breakpoint(); }
}  // namespace hardware
