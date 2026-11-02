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
#include "Emulation/RiscV64Machine.hpp"

namespace emulation {

RiscV64Machine::RiscV64Machine(allocators::IAllocator& allocator, process::AddressSpace& placeholder_space, uint64_t ram_bytes)
    : placeholder_(placeholder_space), ram_(allocator), uart_out_(allocator), ram_size_(ram_bytes) {
  uint8_t zeros[4096] = {};
  ram_ok_ = true;
  for (uint64_t done = 0; done < ram_bytes && ram_ok_; done += sizeof(zeros)) ram_ok_ = ram_.append(ConstByteView(zeros, sizeof(zeros)));
}

bool RiscV64Machine::load(uint64_t physical_address, ConstByteView image) {
  if (physical_address < kRamBase || physical_address - kRamBase > ram_size_ || image.size() > ram_size_ - (physical_address - kRamBase)) return false;
  ram_.writable_view().subview(physical_address - kRamBase, image.size()).copy_from(image);
  return true;
}

bool RiscV64Machine::run(uint64_t max_instructions) {
  for (uint64_t done = 0; done < max_instructions && !finished_; done += 64) cpu_.run_system(64);
  return finished_;
}

uint64_t RiscV64Machine::interrupts() {
  uint64_t pending = 0;
  if (msip_ & 1) pending |= uint64_t{1} << 3;
  if (time() >= mtimecmp_) pending |= uint64_t{1} << 7;
  return pending;
}

void RiscV64Machine::idle(uint64_t wake_time) {
  uint64_t target = mtimecmp_ < wake_time ? mtimecmp_ : wake_time;
  if (target != ~uint64_t{0} && target > time()) ticks_ = target * kTicksPerTime;
}

bool RiscV64Machine::read(uint64_t address, uint32_t size, uint64_t* out) {
  if (address >= kRamBase) {
    const uint64_t offset = address - kRamBase;
    if (offset > ram_size_ || size > ram_size_ - offset) return false;
    ConstByteView bytes = ram_.view().subview(offset, size);
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; ++i) v |= uint64_t{bytes[i]} << (8 * i);
    *out = v;
    return true;
  }
  if (address >= kClint && address < kClint + 0x10000) {
    const uint64_t offset = address - kClint;
    uint64_t whole;
    if (offset < 4) whole = msip_;
    else if (offset >= 0x4000 && offset < 0x4008) whole = mtimecmp_;
    else if (offset >= 0xBFF8 && offset < 0xC000) whole = time();
    else return false;
    const uint64_t base = offset < 4 ? 0 : offset >= 0xBFF8 ? 0xBFF8 : 0x4000;
    const uint64_t shift = 8 * (offset - base);
    if (shift + 8 * size > (offset < 4 ? 32u : 64u)) return false;
    *out = size == 8 ? whole : (whole >> shift) & ((uint64_t{1} << (8 * size)) - 1);
    return true;
  }
  if (address >= kUart && address < kUart + 8) {
    uint64_t v = 0;
    switch (address - kUart) {
      case 0: v = (uart_lcr_ & 0x80) ? uart_dll_ : 0; break;
      case 1: v = (uart_lcr_ & 0x80) ? uart_dlm_ : uart_ier_; break;
      case 2: v = 0xC1; break;
      case 3: v = uart_lcr_; break;
      case 4: v = uart_mcr_; break;
      case 5: v = 0x60; break;
      case 6: v = 0xB0; break;
      default: v = uart_scr_; break;
    }
    *out = v;
    return size == 1;
  }
  if (address >= 0xC000000ull && address < 0x10000000ull) { *out = 0; return true; }      // the PLIC: no sources
  if (address == kFinisher) { *out = 0; return size == 4; }
  return false;
}

bool RiscV64Machine::write(uint64_t address, uint32_t size, uint64_t value) {
  if (address >= kRamBase) {
    const uint64_t offset = address - kRamBase;
    if (offset > ram_size_ || size > ram_size_ - offset) return false;
    ByteView bytes = ram_.writable_view().subview(offset, size);
    for (uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    return true;
  }
  if (address >= kClint && address < kClint + 0x10000) {
    const uint64_t offset = address - kClint;
    if (offset == 0 && size == 4) { msip_ = static_cast<uint32_t>(value) & 1; return true; }
    if (offset == 0x4000 && size == 8) { mtimecmp_ = value; return true; }
    if (offset == 0x4000 && size == 4) { mtimecmp_ = (mtimecmp_ & ~0xFFFFFFFFull) | (value & 0xFFFFFFFFull); return true; }
    if (offset == 0x4004 && size == 4) { mtimecmp_ = (mtimecmp_ & 0xFFFFFFFFull) | (value << 32); return true; }
    if (offset == 0xBFF8 && size == 8) { ticks_ = value * kTicksPerTime; return true; }
    return false;
  }
  if (address >= kUart && address < kUart + 8) {
    if (size != 1) return false;
    const uint8_t v = static_cast<uint8_t>(value);
    switch (address - kUart) {
      case 0: if (uart_lcr_ & 0x80) uart_dll_ = v; else uart_out_.put8(v); break;
      case 1: if (uart_lcr_ & 0x80) uart_dlm_ = v; else uart_ier_ = v & 0xF; break;
      case 3: uart_lcr_ = v; break;
      case 4: uart_mcr_ = v & 0x1F; break;
      case 7: uart_scr_ = v; break;
      default: break;
    }
    return true;
  }
  if (address >= 0xC000000ull && address < 0x10000000ull) return true;
  if (address == kFinisher && size == 4) {
    const uint32_t v = static_cast<uint32_t>(value);
    if ((v & 0xFFFF) == 0x5555) { finished_ = true; exit_code_ = 0; }
    else if ((v & 0xFFFF) == 0x3333) { finished_ = true; exit_code_ = v >> 16; }
    return true;
  }
  return false;
}

}  // namespace emulation
