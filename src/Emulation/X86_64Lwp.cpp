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
// event (LWPVAL, EventId 1) and the programmed event (LWPINS, EventId 255), the hardware events that make sense without a cache model - instructions retired (2), branches retired (3, with the
// filters for absolute, conditional and unconditional relative branches and the TKN bit; no prediction information, PRV = 0, and no fused branches), CPU clocks not halted (5) and reference clocks
// not halted (6), both counting one clock per retired instruction - the IP range filter, the performance time stamp, the threshold, and SLWPCB's flush of the head, counters and MissedEvents.
// Not modelled: the DCache miss event (there is no cache; cpuid does not offer it and LLWPCB clears its flag) and the interrupt itself: when the ring buffer fill reaches the threshold with
// LWPCB.Flags.INT set, lwp_interrupt_pending() becomes true and the host decides what to deliver. The state belongs to the CPU object, as the AMX tiles do; it is saved by XSAVE as component 62
// in a layout of this emulator's own (the architectural one is not public).
#include "Emulation/X86_64Cpu.hpp"

namespace emulation {

namespace {

constexpr uint32_t kEventOffset = 88;                       // LwpEventOffset: EventInterval1 follows the fixed part of the control block
constexpr uint32_t kMaxEvents = 6;                          // LwpMaxEvents
constexpr uint32_t kControlBlockBytes = kEventOffset + 8 * kMaxEvents;      // 136 bytes, 17 quadwords (LwpCbSize)
constexpr uint32_t kRecordBytes = 32;                       // LwpEventSize
constexpr uint32_t kMinBufferRecords = 32;                  // LwpMinBufferSize = 1: 32 records at least
constexpr uint32_t kFlagVal = 1u << 1, kFlagIre = 1u << 2, kFlagBre = 1u << 3, kFlagCnh = 1u << 5, kFlagRnh = 1u << 6, kFlagCont = 1u << 29, kFlagPtsc = 1u << 30, kFlagInt = 1u << 31;
constexpr uint32_t kAvailableFlags = kFlagVal | kFlagIre | kFlagBre | kFlagCnh | kFlagRnh | kFlagCont | kFlagPtsc | kFlagInt;
constexpr uint32_t kHardwareFlags = kFlagIre | kFlagBre | kFlagCnh | kFlagRnh;
constexpr int32_t kMinimumInterval = 1;      // the smallest interval of a hardware event (the value sample may use 0)
// LWPCB.Filters
constexpr uint32_t kFilterNab = 1u << 27, kFilterNcb = 1u << 28, kFilterNrb = 1u << 29, kFilterIpi = 1u << 30, kFilterIpf = 1u << 31;

// LWPCB field offsets
constexpr uint32_t kFlagsAt = 0, kBufferSizeAt = 4, kBufferBaseAt = 8, kHeadAt = 16, kMissedAt = 24, kThresholdAt = 32, kFiltersAt = 36, kBaseIpAt = 40, kLimitIpAt = 48, kTailAt = 64;
uint32_t event_at(uint32_t event_id) { return kEventOffset + 8 * (event_id - 1); }

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

// The LWPCB.Flags bit that enables the event with this EventId (0 for the ones that cannot be enabled).
uint32_t flag_of(uint32_t id) { return id == 1 ? kFlagVal : id == 2 ? kFlagIre : id == 3 ? kFlagBre : id == 5 ? kFlagCnh : id == 6 ? kFlagRnh : 0; }

}  // namespace

// Writes the counter, the head pointer and MissedEvents of the running session back to the control block.
bool X86_64Cpu::lwp_flush(GuestMemory& m) {
  if (lwp_.cb == 0) return true;
  uint8_t block[kControlBlockBytes] = {};
  ByteView view(block, sizeof(block));
  if (!m.read(lwp_.cb, view)) { fail(14, lwp_.cb, 0); return false; }
  put32(view, kHeadAt, lwp_.head);
  put64(view, kMissedAt, lwp_.missed);
  for (uint32_t id = 1; id <= kMaxEvents; ++id) {
    if (!(lwp_.flags & flag_of(id))) continue;
    const uint32_t counter_at = event_at(id) + 4;
    put32(view, counter_at, (get32(view, counter_at) & ~0x03FFFFFFu) | (static_cast<uint32_t>(lwp_.counter[id]) & 0x03FFFFFFu));
  }
  if (!m.write(lwp_.cb, ConstByteView(block, sizeof(block)))) { fail(14, lwp_.cb, 2); return false; }
  return true;
}

// Stores one event record at the head of the ring buffer (or over the last one when a synchronized buffer is full). `full` reports the second case. The record is
// EventId, CoreId (0), Flags, Data1, InstructionAddress, Data2 and the time stamp.
bool X86_64Cpu::lwp_insert(GuestMemory& m, uint8_t event_id, uint16_t flags, uint32_t data1, uint64_t instruction_address, uint64_t data2, bool* full) {
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
  put64(view, 8, instruction_address);
  put64(view, 16, data2);
  put64(view, 24, (lwp_.flags & kFlagPtsc) ? retired_ * 4 : 0);
  if (!m.write(lwp_.buffer_base + at, ConstByteView(record, sizeof(record)))) { fail(14, lwp_.buffer_base + at, 2); return false; }
  if (*full) {
    ++lwp_.missed;
  } else {
    lwp_.head = next;
    if (continuous && next == tail) ++lwp_.missed;      // (the buffer wrapped)
    if ((lwp_.flags & kFlagInt) && lwp_.threshold <= lwp_.buffer_size && (next + lwp_.buffer_size - tail) % lwp_.buffer_size >= lwp_.threshold) lwp_interrupt_ = true;
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
    for (uint32_t id = 1; id <= kMaxEvents; ++id) {
      if (!(enabled & flag_of(id))) continue;
      int32_t interval = signed26(get32(view, event_at(id))), counter = signed26(get32(view, event_at(id) + 4));
      if (interval < (id == 1 ? 0 : kMinimumInterval)) interval = id == 1 ? 0 : kMinimumInterval;      // (the value sample may use zero)
      if (counter < 0) counter = 0;
      put32(view, event_at(id), (get32(view, event_at(id)) & ~0x03FFFFFFu) | static_cast<uint32_t>(interval));
      lwp_.counter[id] = counter;
      lwp_.interval[id] = interval;
    }
    if (!m.write(address, ConstByteView(block, sizeof(block)))) return fail(14, address, 2);
    uint32_t head = get32(view, kHeadAt);
    head -= head % kRecordBytes;
    lwp_.cb = address;
    lwp_.buffer_base = buffer_base;
    lwp_.buffer_size = records * kRecordBytes;
    lwp_.flags = enabled;
    lwp_.threshold = get32(view, kThresholdAt) - get32(view, kThresholdAt) % kRecordBytes;
    lwp_.filters = get32(view, kFiltersAt);
    lwp_.base_ip = get64(view, kBaseIpAt);
    lwp_.limit_ip = get64(view, kLimitIpAt);
    lwp_.random_bits = block[7] >> 4;
    lwp_.head = head < lwp_.buffer_size ? head : 0;
    lwp_.missed = get64(view, kMissedAt);
    s_.rip += d_.position;
    return Step::Ok;
  }
  // lwpins (/0) and lwpval (/1): vvvv is the Data2 register, the r/m operand Data1, a 32-bit immediate the flags
  if (kind > 1) return fail(6);
  const uint32_t imm = static_cast<uint32_t>(next_imm(4, false));
  if (!finish_decode()) return bad_decode();
  if (vex_.wide) return fail(6);
  const bool insert = kind == 0;      // lwpins; lwpval counts down first
  const bool running = lwp_.cb != 0 && (insert || ((lwp_.flags & kFlagVal) && lwp_ip_eligible(s_.rip)));
  const bool store_record = running && (insert || lwp_.counter[1] - 1 < 0);
  if (!store_record) {
    if (running) --lwp_.counter[1];      // (a value sample that is not stored does not touch its memory operand)
    if (insert) s_.rflags &= ~kCarry;
    s_.rip += d_.position;
    return Step::Ok;
  }
  uint64_t data1 = 0;
  if (!read_rm(m, 4, &data1)) return Step::Fault;
  bool full = false;
  if (!lwp_insert(m, insert ? 255 : 1, static_cast<uint16_t>(imm), static_cast<uint32_t>(data1), s_.rip, s_.r[vvvv] & mask, &full)) return Step::Fault;
  if (insert) {
    s_.rflags = (s_.rflags & ~kCarry) | (full ? kCarry : 0);
  } else {
    --lwp_.counter[1];
    if (!full) lwp_reload(1);
  }
  s_.rip += d_.position;
  return Step::Ok;
}

// Restarts an event's counter from its interval, the low Random bits randomized.
void X86_64Cpu::lwp_reload(uint32_t id) {
  const uint32_t random_mask = lwp_.random_bits >= 32 ? ~0u : (1u << lwp_.random_bits) - 1;
  lwp_.counter[id] = static_cast<int32_t>((static_cast<uint32_t>(lwp_.interval[id]) & ~random_mask) | (static_cast<uint32_t>(next_random()) & random_mask));
}

// IP filtering: with IPF set only instructions inside [BaseIP, LimitIP] (outside with IPI) are eligible.
bool X86_64Cpu::lwp_ip_eligible(uint64_t ip) const {
  if (!(lwp_.filters & kFilterIpf)) return true;
  const bool inside = ip >= lwp_.base_ip && ip <= lwp_.limit_ip;
  return (lwp_.filters & kFilterIpi) ? !inside : inside;
}

namespace {

// What kind of control transfer the instruction in the decoder is: 0 none, 1 conditional (Jcc, LOOPcc, JrCXZ), 2 unconditional relative (JMP, CALL, RET), 3 through a register or memory
// (JMP /4 and CALL /2). Far transfers and everything else are not branches for LWP.
uint32_t branch_kind(const uint8_t* bytes, uint32_t length, int* condition_code) {
  *condition_code = -1;
  uint32_t i = 0;
  while (i < length) {
    const uint8_t b = bytes[i];
    if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0xF0 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 || (b >= 0x40 && b <= 0x4F)) ++i;
    else if (b == 0xD5) i += 2;      // REX2
    else break;
  }
  if (i >= length) return 0;
  const uint8_t op = bytes[i];
  if (op >= 0x70 && op <= 0x7F) { *condition_code = op & 15; return 1; }
  if (op >= 0xE0 && op <= 0xE3) return 1;
  if (op == 0x0F) {
    if (i + 1 >= length || bytes[i + 1] < 0x80 || bytes[i + 1] > 0x8F) return 0;
    *condition_code = bytes[i + 1] & 15;
    return 1;
  }
  if (op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xC2 || op == 0xC3) return 2;
  if (op == 0xFF && i + 1 < length) {
    const uint32_t kind = (bytes[i + 1] >> 3) & 7;
    return kind == 2 || kind == 4 ? 3 : 0;
  }
  return 0;
}

}  // namespace

// Called after every retired instruction while a session with hardware events runs: instructions retired, branches retired, and the two clock events (a clock per instruction) count down and store
// a record when they go negative. `ip` is the address of the instruction that retired.
bool X86_64Cpu::lwp_retire(GuestMemory& m, uint64_t ip) {
  if (!lwp_ip_eligible(ip)) return true;
  const uint32_t length = d_.position < sizeof(d_.bytes) ? d_.position : static_cast<uint32_t>(sizeof(d_.bytes));
  for (uint32_t id = 2; id <= 6; ++id) {
    if (!(lwp_.flags & flag_of(id))) continue;
    uint64_t target = 0;
    uint16_t flags = 0;
    if (id == 3) {
      int condition_code = -1;
      const uint32_t kind = branch_kind(d_.bytes, length, &condition_code);
      if (kind == 0) continue;
      if ((kind == 1 && (lwp_.filters & kFilterNcb)) || (kind == 2 && (lwp_.filters & kFilterNrb)) || (kind == 3 && (lwp_.filters & kFilterNab))) continue;
      target = s_.rip;
      const bool taken = kind != 1 || (condition_code >= 0 ? condition(static_cast<uint32_t>(condition_code)) : s_.rip != ip + length);
      flags = taken ? 0x8000 : 0;      // TKN (bit 15 of the flags word, bit 7 of byte 3); PRV and FUS are clear
    }
    if (--lwp_.counter[id] >= 0) continue;
    bool full = false;
    if (!lwp_insert(m, static_cast<uint8_t>(id), flags, 0, ip, target, &full)) return false;
    if (!full) lwp_reload(id);
  }
  return true;
}

}  // namespace emulation
