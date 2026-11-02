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

void RiscV64Machine::plic_refresh() {
  for (uint32_t i = 1; i < kSources; ++i) {
    const uint32_t word = i / 32, bit = uint32_t{1} << (i % 32);
    const bool line = source_line(i), rose = line && !(plic_line_[word] & bit);      // (the PLIC latches a rising edge; a claim is the only thing that clears it)
    if (line) plic_line_[word] |= bit;
    else plic_line_[word] &= ~bit;
    if (rose) plic_pending_[word] |= bit;
  }
}

uint32_t RiscV64Machine::plic_best(uint32_t context) const {
  uint32_t best = 0, best_priority = 0;
  for (uint32_t i = 1; i < kSources; ++i) {
    const uint32_t word = i / 32, bit = uint32_t{1} << (i % 32);
    if (!(plic_pending_[word] & bit) || (plic_claimed_[word] & bit) || !(plic_enable_[context][word] & bit)) continue;
    if (plic_priority_[i] > plic_threshold_[context] && plic_priority_[i] > best_priority) {
      best = i;
      best_priority = plic_priority_[i];
    }
  }
  return best;
}

uint64_t RiscV64Machine::interrupts() {
  plic_refresh();
  uint64_t pending = 0;
  if (plic_best(0)) pending |= uint64_t{1} << 11;
  if (plic_best(1)) pending |= uint64_t{1} << 9;
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
      case 2:
        if ((uart_ier_ & 2) && uart_thr_pending_) { v = 0x02; uart_thr_pending_ = false; }
        else v = 0x01;
        break;
      case 3: v = uart_lcr_; break;
      case 4: v = uart_mcr_; break;
      case 5: v = 0x60; break;
      case 6: v = 0xB0; break;
      default: v = uart_scr_; break;
    }
    *out = v;
    return size == 1;
  }
  if (address >= 0xC000000ull && address < 0x10000000ull) {      // the PLIC
    plic_refresh();
    const uint64_t offset = address - 0xC000000ull;
    uint64_t v = 0;
    if (offset < 4 * kSources) v = plic_priority_[offset / 4];
    else if (offset >= 0x1000 && offset < 0x1000 + 4 * (kSources / 32)) v = plic_pending_[(offset - 0x1000) / 4];
    else if (offset >= 0x2000 && offset < 0x2000 + 0x80 * kContexts) {
      const uint64_t word = ((offset - 0x2000) % 0x80) / 4;
      if (word < kSources / 32) v = plic_enable_[(offset - 0x2000) / 0x80][word];
    } else if (offset >= 0x200000 && offset < 0x200000 + 0x1000 * kContexts) {
      const uint32_t context = static_cast<uint32_t>((offset - 0x200000) / 0x1000);
      if ((offset & 0xFFF) == 0) v = plic_threshold_[context];
      else if ((offset & 0xFFF) == 4) {      // claim: the best source, now pending no more
        v = plic_best(context);
        if (v) {
          plic_pending_[v / 32] &= ~(uint32_t{1} << (v % 32));
          plic_claimed_[v / 32] |= uint32_t{1} << (v % 32);
        }
      }
    }
    *out = v;
    return true;
  }
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
      case 0: if (uart_lcr_ & 0x80) uart_dll_ = v; else { uart_out_.put8(v); uart_thr_pending_ = true; } break;
      case 1:
        if (uart_lcr_ & 0x80) { uart_dlm_ = v; break; }
        if ((v & 2) && !(uart_ier_ & 2)) uart_thr_pending_ = true;      // (enabling the interrupt with an empty holding register raises it)
        if (!(v & 2)) uart_thr_pending_ = false;
        uart_ier_ = v & 0xF;
        break;
      case 3: uart_lcr_ = v; break;
      case 4: uart_mcr_ = v & 0x1F; break;
      case 7: uart_scr_ = v; break;
      default: break;
    }
    return true;
  }
  if (address >= 0xC000000ull && address < 0x10000000ull) {
    const uint64_t offset = address - 0xC000000ull;
    const uint32_t v = static_cast<uint32_t>(value);
    if (offset < 4 * kSources) plic_priority_[offset / 4] = offset < 4 ? 0 : v & 7;
    else if (offset >= 0x2000 && offset < 0x2000 + 0x80 * kContexts) {
      const uint64_t word = ((offset - 0x2000) % 0x80) / 4;
      if (word < kSources / 32) plic_enable_[(offset - 0x2000) / 0x80][word] = v;
    } else if (offset >= 0x200000 && offset < 0x200000 + 0x1000 * kContexts) {
      const uint32_t context = static_cast<uint32_t>((offset - 0x200000) / 0x1000);
      if ((offset & 0xFFF) == 0) plic_threshold_[context] = v & 7;
      else if ((offset & 0xFFF) == 4 && v < kSources) plic_claimed_[v / 32] &= ~(uint32_t{1} << (v % 32));      // complete
    }
    return true;
  }
  if (address == kFinisher && size == 4) {
    const uint32_t v = static_cast<uint32_t>(value);
    if ((v & 0xFFFF) == 0x5555) { finished_ = true; exit_code_ = 0; }
    else if ((v & 0xFFFF) == 0x3333) { finished_ = true; exit_code_ = v >> 16; }
    return true;
  }
  return false;
}

}  // namespace emulation
