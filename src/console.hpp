#pragma once
#include <stdint.h>
#include "hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "uefi.hpp"

// Output goes to the QEMU debug console (port 0x402) and to the screen: the
// GPU-drawn console once one is attached, the firmware text console before.
namespace console {
void init(efi::SystemTable* st);
// Routes screen output through `gpu` (clearing it). Call again after changing
// the GPU's mode. nullptr returns to the firmware text console.
void use_gpu(hardware::IGraphicsProcessingUnit* gpu);
void puts(const char* s);
void put_hex(uint64_t v);
void put_dec(int64_t v);
}  // namespace console
