#pragma once
#include <stdint.h>
#include "uefi.hpp"

// Output goes to the UEFI text console and the QEMU debugcon (port 0x402).
namespace console {
void init(efi::SystemTable* st);
void puts(const char* s);
void put_hex(uint64_t v);
}  // namespace console
