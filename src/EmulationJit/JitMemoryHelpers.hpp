/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include <stdint.h>
#include "Emulation/GuestCpu.hpp"
#include "Jit/Ir/Ir.hpp"

// The helpers translated code calls for guest memory: the only way generated code touches the guest's memory, so every access is checked by GuestMemory like the
// interpreter's. A failed access sets the fault fields of the block's GuestJitLink (`State::jit`) and returns 0; the block tests the flag after the helper.
namespace emulation {

template <class State, uint32_t Size>
uint64_t guest_load(void* state, uint64_t address, uint64_t) {
  State& s = *static_cast<State*>(state);      // (the pointer the block was entered with is the State's own address)
  uint8_t raw[8] = {};
  if (!s.jit.memory->read(address, ByteView(raw, Size))) {
    s.jit.fault = 1;
    s.jit.fault_address = address;
    s.jit.fault_error = 0;
    return 0;
  }
  uint64_t v = 0;
  for (uint32_t i = 0; i < Size; ++i) v |= uint64_t{raw[i]} << (8 * i);
  return v;
}

template <class State, uint32_t Size>
uint64_t guest_store(void* state, uint64_t address, uint64_t value) {
  State& s = *static_cast<State*>(state);
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = static_cast<uint8_t>(value >> (8 * i));
  if (!s.jit.memory->write(address, ConstByteView(raw, Size))) {
    s.jit.fault = 1;
    s.jit.fault_address = address;
    s.jit.fault_error = 2;
  }
  return 0;
}

template <class State>
jit::IrHelper load_helper(uint32_t size) {
  switch (size) {
    case 1: return &guest_load<State, 1>;
    case 2: return &guest_load<State, 2>;
    case 4: return &guest_load<State, 4>;
    default: return &guest_load<State, 8>;
  }
}

template <class State>
jit::IrHelper store_helper(uint32_t size) {
  switch (size) {
    case 1: return &guest_store<State, 1>;
    case 2: return &guest_store<State, 2>;
    case 4: return &guest_store<State, 4>;
    default: return &guest_store<State, 8>;
  }
}

}  // namespace emulation
