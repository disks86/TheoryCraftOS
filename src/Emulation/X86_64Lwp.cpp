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

// AMD Lightweight Profiling (LWP): llwpcb, slwpcb, lwpval and lwpins (XOP map 9 / A, opcode 12), from AMD64 Architecture Programmer's Manual volume 2 section 13.4 and volume 3.
//
// What is modelled: the control block (LWPCB), the event ring buffer in guest memory with its head, tail and MissedEvents bookkeeping, synchronized and continuous mode, the programmed value sample
// event (LWPVAL, EventId 1) with its counter and interval, the programmed event (LWPINS, EventId 255), the performance time stamp, and SLWPCB's flush of the counter and head pointer into the
// control block. What is not: the hardware events (instructions retired, branches, cache misses, clocks) and threshold interrupts. Their LWPCB.Flags bits are cleared by LLWPCB, as for a
// processor that does not have them, and cpuid (leaf 0x8000001C) says so. The state belongs to the CPU object, as the AMX tiles do; it is saved by XSAVE as component 62 in a layout of this
// emulator's own (the architectural one is not public).
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {

constexpr uint32_t kEventOffset = 88;                       // LwpEventOffset: EventInterval1 follows the fixed part of the control block
constexpr uint32_t kMaxEvents = 6;                          // LwpMaxEvents
constexpr uint32_t kControlBlockBytes = kEventOffset + 8 * kMaxEvents;      // 136 bytes, 17 quadwords (LwpCbSize)
constexpr uint32_t kRecordBytes = 32;                       // LwpEventSize
constexpr uint32_t kMinBufferRecords = 32;                  // LwpMinBufferSize = 1: 32 records at least
constexpr uint32_t kFlagVal = 1u << 1, kFlagCont = 1u << 29, kFlagPtsc = 1u << 30;
constexpr uint32_t kAvailableFlags = kFlagVal | kFlagCont | kFlagPtsc;

// LWPCB field offsets
constexpr uint32_t kFlagsAt = 0, kBufferSizeAt = 4, kBufferBaseAt = 8, kHeadAt = 16, kMissedAt = 24, kThresholdAt = 32, kTailAt = 64;

template <typename View>
uint32_t get32(const View& v, uint32_t at) {
  uint32_t x = 0;
  for (uint32_t i = 0; i < 4; ++i) x |= uint32_t{v[at + i]} << (8 * i);
  return x;
}

template <typename View>
uint64_t get64(const View& v, uint32_t at) { return get32(v, at) | (uint64_t{get32(v, at + 4)} << 32); }

void put32(ByteView v, uint32_t at, uint32_t x) {
  for (uint32_t i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i));
}

void put64(ByteView v, uint32_t at, uint64_t x) {
  put32(v, at, static_cast<uint32_t>(x));
  put32(v, at + 4, static_cast<uint32_t>(x >> 32));
}

// A 26-bit signed counter or interval field (bits 25:0; the rest is reserved).
int32_t signed26(uint32_t field) { return static_cast<int32_t>(field << 6) >> 6; }

}  // namespace

// Writes the counter, the head pointer and MissedEvents of the running session back to the control block.
bool X86_64Cpu::lwp_flush(GuestMemory& m) {
  if (lwp_.cb == 0) return true;
  uint8_t block[kControlBlockBytes] = {};
  ByteView view(block, sizeof(block));
  if (!m.read(lwp_.cb, view)) { fail(14, lwp_.cb, 0); return false; }
  put32(view, kHeadAt, lwp_.head);
  put64(view, kMissedAt, lwp_.missed);
  const uint32_t counter_at = kEventOffset + 4;
  put32(view, counter_at, (get32(view, counter_at) & ~0x03FFFFFFu) | (static_cast<uint32_t>(lwp_.counter) & 0x03FFFFFFu));
  if (!m.write(lwp_.cb, ConstByteView(block, sizeof(block)))) { fail(14, lwp_.cb, 2); return false; }
  return true;
}

// Stores one event record at the head of the ring buffer (or over the last one when a synchronized buffer is full). `full` reports the second case. The record is
// EventId, CoreId (0), Flags, Data1, InstructionAddress, Data2 and the time stamp.
bool X86_64Cpu::lwp_insert(GuestMemory& m, uint8_t event_id, uint16_t flags, uint32_t data1, uint64_t data2, bool* full) {
  uint8_t tail_bytes[4] = {};
  if (!m.read(lwp_.cb + kTailAt, ByteView(tail_bytes, sizeof(tail_bytes)))) { fail(14, lwp_.cb + kTailAt, 0); return false; }
  uint32_t tail = get32(ByteView(tail_bytes, sizeof(tail_bytes)), 0);
  tail = tail < lwp_.buffer_size ? tail - tail % kRecordBytes : 0;
  const uint32_t next = lwp_.head + kRecordBytes >= lwp_.buffer_size ? 0 : lwp_.head + kRecordBytes;
  const bool continuous = lwp_.flags & kFlagCont;
  *full = next == tail && !continuous;
  const uint32_t at = *full ? (lwp_.head == 0 ? lwp_.buffer_size - kRecordBytes : lwp_.head - kRecordBytes) : lwp_.head;
  uint8_t record[kRecordBytes] = {};
  ByteView view(record, sizeof(record));
  view[0] = event_id;
  view[2] = static_cast<uint8_t>(flags);
  view[3] = static_cast<uint8_t>(flags >> 8);
  put32(view, 4, data1);
  put64(view, 8, s_.rip);
  put64(view, 16, data2);
  put64(view, 24, (lwp_.flags & kFlagPtsc) ? retired_ * 4 : 0);
  if (!m.write(lwp_.buffer_base + at, ConstByteView(record, sizeof(record)))) { fail(14, lwp_.buffer_base + at, 2); return false; }
  if (*full) {
    ++lwp_.missed;
  } else {
    lwp_.head = next;
    if (continuous && next == tail) ++lwp_.missed;      // (the buffer wrapped)
  }
  return true;
}

X86_64Cpu::Step X86_64Cpu::lwp_instruction(GuestMemory& m, uint32_t map, uint32_t vvvv) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  if (!decode_modrm()) return bad_decode();
  const uint32_t kind = d_.reg & 7;
  const uint32_t size = d_.rex_w ? 8 : 4;
  const uint64_t mask = d_.rex_w ? ~uint64_t{0} : 0xFFFFFFFFu;
  if (map == 9) {      // llwpcb reg (/0), slwpcb reg (/1): the register form only, with VEX.vvvv = 1111
    if (!finish_decode()) return bad_decode();
    if (d_.mod != 3 || vvvv != 0 || vex_.wide || kind > 1) return fail(6);
    if (kind == 1) {      // slwpcb
      if (!lwp_flush(m)) return Step::Fault;
      set_reg(d_.rm, size, lwp_.cb & mask);
      s_.rip += d_.position;
      return Step::Ok;
    }
    // llwpcb: flush the running session, then start the one in the control block at the address in the register
    if (!lwp_flush(m)) return Step::Fault;
    const uint64_t address = get_reg(d_.rm, size) & mask;
    if (address == 0) {
      lwp_ = Lwp();
      s_.rip += d_.position;
      return Step::Ok;
    }
    lwp_ = Lwp();      // (LWP is left disabled if any of the checks fails)
    uint8_t block[kControlBlockBytes] = {};
    ByteView view(block, sizeof(block));
    if (!m.read(address, view)) return fail(14, address, 0);
    const uint32_t flags = get32(view, kFlagsAt), buffer_size = get32(view, kBufferSizeAt) & 0x0FFFFFFFu;
    const uint64_t buffer_base = get64(view, kBufferBaseAt);
    const uint32_t records = buffer_size / kRecordBytes;
    if (records < kMinBufferRecords) return fail(13);
    if (buffer_base + records * kRecordBytes < buffer_base) return fail(13);      // (the buffer wraps around the address space)
    const uint32_t enabled = flags & kAvailableFlags;
    put32(view, kFlagsAt, enabled);
    int32_t interval = signed26(get32(view, kEventOffset)), counter = signed26(get32(view, kEventOffset + 4));
    if (interval < 0) interval = 0;      // (the minimum interval for LWPVAL is zero)
    if (counter < 0) counter = 0;
    put32(view, kEventOffset, (get32(view, kEventOffset) & ~0x03FFFFFFu) | static_cast<uint32_t>(interval));
    if (!m.write(address, ConstByteView(block, sizeof(block)))) return fail(14, address, 2);
    uint32_t head = get32(view, kHeadAt);
    head -= head % kRecordBytes;
    lwp_.cb = address;
    lwp_.buffer_base = buffer_base;
    lwp_.buffer_size = records * kRecordBytes;
    lwp_.flags = enabled;
    lwp_.threshold = get32(view, kThresholdAt);
    lwp_.random_bits = block[7] >> 4;
    lwp_.head = head < lwp_.buffer_size ? head : 0;
    lwp_.missed = get64(view, kMissedAt);
    lwp_.counter = counter;
    lwp_.interval = interval;
    s_.rip += d_.position;
    return Step::Ok;
  }
  // lwpins (/0) and lwpval (/1): vvvv is the Data2 register, the r/m operand Data1, a 32-bit immediate the flags
  if (kind > 1) return fail(6);
  const uint32_t imm = static_cast<uint32_t>(next_imm(4, false));
  if (!finish_decode()) return bad_decode();
  if (vex_.wide) return fail(6);
  const bool insert = kind == 0;      // lwpins; lwpval counts down first
  const bool running = lwp_.cb != 0 && (insert || (lwp_.flags & kFlagVal));
  const bool store_record = running && (insert || lwp_.counter - 1 < 0);
  if (!store_record) {
    if (running) --lwp_.counter;      // (a value sample that is not stored does not touch its memory operand)
    if (insert) s_.rflags &= ~kCarry;
    s_.rip += d_.position;
    return Step::Ok;
  }
  uint64_t data1 = 0;
  if (!read_rm(m, 4, &data1)) return Step::Fault;
  bool full = false;
  if (!lwp_insert(m, insert ? 255 : 1, static_cast<uint16_t>(imm), static_cast<uint32_t>(data1), s_.r[vvvv] & mask, &full)) return Step::Fault;
  if (insert) {
    s_.rflags = (s_.rflags & ~kCarry) | (full ? kCarry : 0);
  } else {
    --lwp_.counter;
    if (!full) {      // the counter restarts from the interval, its low bits randomized
      const uint32_t random_mask = lwp_.random_bits >= 32 ? ~0u : (1u << lwp_.random_bits) - 1;
      lwp_.counter = static_cast<int32_t>((static_cast<uint32_t>(lwp_.interval) & ~random_mask) | (static_cast<uint32_t>(next_random()) & random_mask));
    }
  }
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
