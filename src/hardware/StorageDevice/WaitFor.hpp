#pragma once
#include <stdint.h>
#include "uefi.hpp"

namespace hardware {

// Polls `done()` until it returns true or `timeout_ms` passes (firmware timer).
template <class F>
bool wait_for(efi::SystemTable* st, uint32_t timeout_ms, F&& done) {
  for (uint64_t us = 0; us < uint64_t{timeout_ms} * 1000; us += 10) {
    if (done()) return true;
    st->boot_services->stall(10);
  }
  return done();
}

// Copies a fixed-width, blank-padded device string (ATA, SCSI inquiry) into a
// NUL-terminated buffer with trailing blanks removed. `swapped` undoes ATA's
// byte-swapped 16-bit words.
inline void copy_device_string(char* out, uint32_t capacity, const uint8_t* in, uint32_t length, bool swapped) {
  uint32_t n = length < capacity - 1 ? length : capacity - 1;
  for (uint32_t i = 0; i < n; ++i) out[i] = static_cast<char>(swapped ? in[i ^ 1] : in[i]);
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) --n;
  out[n] = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (static_cast<uint8_t>(out[i]) < 32 || static_cast<uint8_t>(out[i]) > 126) out[i] = '?';
}

}  // namespace hardware
