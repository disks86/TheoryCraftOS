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
#include "Process/WindowsException.hpp"
#include "Process/Process.hpp"
#include "Utility/ByteStream.hpp"

namespace process {

namespace {

constexpr uint64_t kMachineFrameBytes = 0x40;      // 40 bytes of frame, padded so that the CONTEXT after it stays 16-aligned
constexpr uint64_t kUserLimit = 0x0000800000000000ull;      // addresses at or above are not canonical user addresses
constexpr uint64_t kFlagsUser = 0x0DD5;                      // CF PF AF ZF SF TF DF OF: what a program may set (TF: single-stepping for a debugger)
constexpr uint32_t kMxcsrUsable = 0xFFBF;

uint32_t mxcsr_of(const uint8_t* fpu) { return uint32_t(fpu[24]) | uint32_t(fpu[25]) << 8 | uint32_t(fpu[26]) << 16 | uint32_t(fpu[27]) << 24; }

// ---- A 32-bit program (i386): its CONTEXT is 0x2CC bytes, its EXCEPTION_RECORD 0x50 ----------------------------------------------------------------------------------
// CONTEXT of i386: ContextFlags 0, Dr0..Dr7 at 4, FloatSave (the FSAVE layout, 112 bytes) at 0x1C, SegGs/Fs/Es/Ds at 0x8C, Edi Esi Ebx Edx Ecx Eax at 0x9C,
// Ebp Eip SegCs EFlags Esp SegSs at 0xB4, ExtendedRegisters (the FXSAVE image) at 0xCC.
constexpr uint32_t kFloatSaveAt = 0x1C, kSegmentsAt = 0x8C, kIntegerAt = 0x9C, kControlAt = 0xB4, kExtendedAt = 0xCC;
constexpr uint64_t kUserLimit32 = 0x80000000ull;
constexpr uint32_t kIntegerOrder32[6] = {X86Context::kRdi, X86Context::kRsi, X86Context::kRbx, X86Context::kRdx, X86Context::kRcx, X86Context::kRax};      // Edi Esi Ebx Edx Ecx Eax
constexpr uint16_t kUserCode32 = 0x3B, kUserData = 0x2B;      // (the selectors a 32-bit thread runs with: Kernel/UserEntry.hpp)

// The x87 state of an FXSAVE image in the older FSAVE layout (FLOATING_SAVE_AREA), which a 32-bit CONTEXT also carries.
void write_float_save(ByteWriter& w, ConstByteView image) {
  ByteReader r(image);
  const uint16_t control = r.le16(), status = r.le16();
  const uint8_t present = r.u8();      // one bit per physical register: in use
  r.skip(1 + 2);
  const uint32_t error_offset = r.le32();
  const uint16_t error_selector = r.le16();
  r.skip(2);
  const uint32_t data_offset = r.le32();
  const uint16_t data_selector = r.le16();
  // The full tag word: two bits per physical register (valid, zero, special, empty), worked out from the register's value.
  const uint32_t top = (status >> 11) & 7;
  uint16_t tags = 0;
  for (uint32_t physical = 0; physical < 8; ++physical) {
    uint32_t tag = 3;
    if (present & (1u << physical)) {
      const ConstByteView value = image.subview(32 + 16 * ((physical - top) & 7), 10);
      const uint32_t exponent = (uint32_t(value[9]) << 8 | value[8]) & 0x7FFF;
      bool zero_mantissa = true;
      for (uint32_t i = 0; i < 8; ++i) zero_mantissa = zero_mantissa && value[i] == 0;
      if (exponent == 0x7FFF || (exponent == 0 && !zero_mantissa) || (exponent != 0 && !(value[7] & 0x80))) tag = 2;
      else if (exponent == 0 && zero_mantissa) tag = 1;
      else tag = 0;
    }
    tags = uint16_t(tags | tag << (2 * physical));
  }
  w.le32(0xFFFF0000u | control);
  w.le32(0xFFFF0000u | status);
  w.le32(0xFFFF0000u | tags);
  w.le32(error_offset);
  w.le32(error_selector);
  w.le32(data_offset);
  w.le32(0xFFFF0000u | data_selector);
  for (uint32_t i = 0; i < 8; ++i) w.bytes(image.subview(32 + 16 * i, 10));
  w.le32(0);      // Cr0NpxState
}

// The other way: the x87 part of an FXSAVE image from a FLOATING_SAVE_AREA (the SSE registers are not in one, and stay).
void read_float_save(ConstByteView area, ByteView fpu) {
  ByteReader r(area);
  const uint32_t control = r.le32(), status = r.le32(), tags = r.le32(), error_offset = r.le32(), error_selector = r.le32(), data_offset = r.le32(),
                 data_selector = r.le32();
  const ConstByteView registers = r.take(80);
  if (!r.ok()) return;
  uint8_t present = 0;
  for (uint32_t physical = 0; physical < 8; ++physical)
    if (((tags >> (2 * physical)) & 3) != 3) present = uint8_t(present | 1u << physical);
  uint8_t head[24] = {};
  ByteWriter w{ByteView(head)};
  w.le16(uint16_t(control));
  w.le16(uint16_t(status));
  w.u8(present);
  w.u8(0);
  w.le16(0);
  w.le32(error_offset);
  w.le16(uint16_t(error_selector));
  w.le16(0);
  w.le32(data_offset);
  w.le16(uint16_t(data_selector));
  w.le16(0);
  fpu.first(sizeof(head)).copy_from(ConstByteView(head));
  for (uint32_t i = 0; i < 8; ++i) {
    const ByteView slot = fpu.subview(32 + 16 * i, 16);
    slot.fill(0);
    slot.first(10).copy_from(registers.subview(10 * i, 10));
  }
}

void write_context32(ByteWriter& w, const X86Context& at, uint32_t flags) {
  w.le32(flags);
  w.zeros(24);      // Dr0..Dr7
  write_float_save(w, ConstByteView(at.fpu, sizeof(at.fpu)));
  w.le32(0); w.le32(kUserData); w.le32(kUserData); w.le32(kUserData);      // gs, fs, es, ds
  for (uint32_t index : kIntegerOrder32) w.le32(uint32_t(at.gpr[index]));
  w.le32(uint32_t(at.gpr[X86Context::kRbp]));
  w.le32(uint32_t(at.rip));
  w.le32(kUserCode32);
  w.le32(uint32_t(at.rflags));
  w.le32(uint32_t(at.gpr[X86Context::kRsp]));
  w.le32(kUserData);
  w.bytes(ConstByteView(at.fpu, sizeof(at.fpu)));
}

bool dispatch_exception32(Process& process, const X86Context& at, const ExceptionInfo& info, uint64_t dispatcher, X86Context* entry) {
  // On the stack, from the new stack pointer up: the two arguments (record, context) as a call leaves them without its return address - which is what
  // KiUserExceptionDispatcher of a 32-bit ntdll finds - then the record, then the CONTEXT.
  constexpr uint64_t kArguments = 8, kTotal = kArguments + kExceptionRecordBytes32 + kContextBytes32;
  const uint64_t end = (at.gpr[X86Context::kRsp] & 0xFFFFFFFFull) & ~uint64_t{3};
  if (end < kTotal + 4096 || end >= kUserLimit32) return false;
  const uint64_t frame_at = (end - kTotal) & ~uint64_t{15};
  const uint64_t record_at = frame_at + kArguments, context_at = record_at + kExceptionRecordBytes32;
  uint8_t buffer[kTotal];
  ByteWriter w{ByteView(buffer)};
  w.le32(uint32_t(record_at));
  w.le32(uint32_t(context_at));
  w.le32(info.code);
  w.le32(info.flags);
  w.le32(0);      // ExceptionRecord
  w.le32(uint32_t(info.address));
  w.le32(info.count);
  for (uint64_t value : info.information) w.le32(uint32_t(value));
  X86Context shown = at;
  if (info.code == 0x80000003u && info.address == at.rip - 1) shown.rip = info.address;      // a breakpoint: on i386 the CONTEXT is at the INT3 too (a handler steps over it)
  write_context32(w, shown, kContextI386 | kContextControl | kContextInteger | kContextSegments | kContextFloatingPoint | kContextExtendedRegisters);
  if (!w.ok() || w.position() != sizeof(buffer)) return false;
  if (!process.write_user(frame_at, ConstByteView(buffer))) return false;
  *entry = at;
  entry->gpr[X86Context::kRsp] = frame_at;
  entry->rip = dispatcher;
  entry->rflags = 0x202;
  return true;
}

bool store_context32(Process& process, uint64_t address, const X86Context& at) {
  uint8_t buffer[kContextBytes32], full[kContextBytes32];
  if (!process.read_user(address, ByteView(buffer))) return false;
  const uint32_t flags = ByteReader{ConstByteView(buffer)}.le32();
  if ((flags & kContextI386) != kContextI386) return false;
  ByteWriter w{ByteView(full)};
  write_context32(w, at, flags);
  if (!w.ok()) return false;
  const ByteView out(buffer);
  const ConstByteView in(full);
  auto copy = [&](uint32_t offset, uint32_t bytes) { out.subview(offset, bytes).copy_from(in.subview(offset, bytes)); };
  if (flags & kContextControl) copy(kControlAt, 24);
  if (flags & kContextSegments) copy(kSegmentsAt, 16);
  if (flags & kContextInteger) copy(kIntegerAt, 24);
  if (flags & kContextFloatingPoint) copy(kFloatSaveAt, 112);
  if (flags & kContextExtendedRegisters) copy(kExtendedAt, 512);
  return process.write_user(address, ConstByteView(buffer));
}

bool apply_context32(Process& process, uint64_t address, X86Context* context) {
  uint8_t buffer[kContextBytes32];
  if (!process.read_user(address, ByteView(buffer))) return false;
  const ConstByteView in(buffer);
  ByteReader r(in);
  const uint32_t flags = r.le32();
  if ((flags & kContextI386) != kContextI386) return false;
  if (flags & kContextControl) {
    ByteReader c(in.subview(kControlAt, 24));
    const uint32_t ebp = c.le32(), eip = c.le32();
    c.skip(4);
    const uint32_t eflags = c.le32(), esp = c.le32();
    if (eip >= kUserLimit32 || esp >= kUserLimit32) return false;
    context->gpr[X86Context::kRbp] = ebp;
    context->rip = eip;
    context->gpr[X86Context::kRsp] = esp;
    context->rflags = (uint64_t{eflags} & kFlagsUser) | 0x202;
  }
  if (flags & kContextInteger) {
    ByteReader g(in.subview(kIntegerAt, 24));
    for (uint32_t index : kIntegerOrder32) context->gpr[index] = g.le32();
  }
  if (flags & kContextExtendedRegisters) {      // the whole FXSAVE image
    ByteView(context->fpu).copy_from(in.subview(kExtendedAt, 512));
    const uint32_t safe = mxcsr_of(context->fpu) & kMxcsrUsable;
    for (uint32_t i = 0; i < 4; ++i) context->fpu[24 + i] = uint8_t(safe >> (8 * i));
  } else if (flags & kContextFloatingPoint) {
    read_float_save(in.subview(kFloatSaveAt, 112), ByteView(context->fpu));
  }
  return true;
}

}  // namespace

bool fault_to_exception(uint64_t vector, uint64_t error, uint64_t cr2, X86Context* at, ExceptionInfo* out) {
  *out = ExceptionInfo();
  out->address = at->rip;
  switch (vector & 0xFF) {
    case 0: out->code = 0xC0000094u; return true;      // STATUS_INTEGER_DIVIDE_BY_ZERO
    case 1: out->code = 0x80000004u; at->rflags &= ~uint64_t{0x100}; return true;      // STATUS_SINGLE_STEP (the trap flag is clear in what the debugger sees)
    case 3:
      out->code = 0x80000003u;      // STATUS_BREAKPOINT: reported at the INT3, the context is after it (as on Windows)
      out->address = at->rip - 1;
      out->count = 1;
      return true;
    case 4: out->code = 0xC0000095u; return true;      // STATUS_INTEGER_OVERFLOW
    case 5: out->code = 0xC000008Cu; return true;      // STATUS_ARRAY_BOUNDS_EXCEEDED
    case 6: case 7: out->code = 0xC000001Du; return true;      // STATUS_ILLEGAL_INSTRUCTION
    case 13:
      if ((error & 3) == 2 && (error >> 3) == 0x2D) {      // INT 2D: a debug service (the service number in rax); it is a breakpoint that the debugger sees
        out->code = 0x80000003u;      // STATUS_BREAKPOINT
        at->rip += 2;
        out->address = at->rip - 2;
        out->count = 1;
        out->information[0] = at->gpr[0];
        return true;
      }
      out->code = 0xC0000096u;      // STATUS_PRIVILEGED_INSTRUCTION
      return true;
    case 14:
      out->code = kStatusAccessViolation;
      out->count = 2;
      out->information[0] = (error & 0x10) ? 8 : (error & 0x2) ? 1 : 0;      // execute, write, read
      out->information[1] = cr2;
      return true;
    case 16: out->code = 0xC0000090u; return true;      // STATUS_FLOAT_INVALID_OPERATION
    case 17: out->code = 0x80000002u; return true;      // STATUS_DATATYPE_MISALIGNMENT
    case 19: {
      const uint32_t mxcsr = mxcsr_of(at->fpu);
      if (mxcsr & 4) out->code = 0xC000008Eu;             // divide by zero
      else if (mxcsr & 1) out->code = 0xC0000090u;        // invalid operation
      else if (mxcsr & 8) out->code = 0xC0000091u;        // overflow
      else if (mxcsr & 0x10) out->code = 0xC0000093u;     // underflow
      else if (mxcsr & 2) out->code = 0xC000008Du;        // denormal operand
      else out->code = 0xC000008Fu;                       // inexact
      return true;
    }
    default: return false;
  }
}

bool dispatch_exception(Process& process, const X86Context& at, const ExceptionInfo& info, uint64_t dispatcher, X86Context* entry) {
  if (dispatcher == 0 || info.count > 15) return false;
  if (process.arch() == UserArch::X86_32) return dispatch_exception32(process, at, info, dispatcher, entry);
  const uint64_t end = at.gpr[X86Context::kRsp] & ~uint64_t{15};
  if (end < kContextBytes + 160 + kMachineFrameBytes + 4096 || end >= kUserLimit) return false;
  const uint64_t record_at = end - 160;                 // 16-aligned: the record is 152 bytes
  const uint64_t context_at = record_at - kContextBytes;
  const uint64_t frame_at = context_at - kMachineFrameBytes;
  uint8_t buffer[kMachineFrameBytes + kContextBytes + kExceptionRecordBytes];
  ByteWriter w{ByteView(buffer)};
  // The machine frame the processor would have pushed (UWOP_PUSH_MACHFRAME in the unwind information of KiUserExceptionDispatcher): it is how the unwinder gets from the
  // handlers' frames back to the code that faulted.
  w.le64(at.rip);
  w.le64(0x33);
  w.le64(at.rflags);
  w.le64(at.gpr[X86Context::kRsp]);
  w.le64(0x2B);
  w.zeros(kMachineFrameBytes - 40);
  // CONTEXT
  w.zeros(0x30);      // P1Home..P6Home
  w.le32(kContextAmd64 | kContextControl | kContextInteger | kContextSegments | kContextFloatingPoint);
  w.le32(mxcsr_of(at.fpu));
  w.le16(0x33); w.le16(0x2B); w.le16(0x2B); w.le16(0x2B); w.le16(0x2B); w.le16(0x2B);      // cs, ds, es, fs, gs, ss (the user selectors)
  w.le32(uint32_t(at.rflags));
  w.zeros(0x30);      // Dr0..Dr7
  for (uint64_t value : at.gpr) w.le64(value);
  w.le64(at.rip);
  w.bytes(ConstByteView(at.fpu, sizeof(at.fpu)));
  w.zeros(26 * 16 + 6 * 8);      // VectorRegister, VectorControl, DebugControl, LastBranch/LastException
  // EXCEPTION_RECORD
  w.le32(info.code);
  w.le32(info.flags);
  w.le64(0);      // ExceptionRecord
  w.le64(info.address);
  w.le32(info.count);
  w.le32(0);
  for (uint64_t value : info.information) w.le64(value);
  if (!w.ok() || w.position() != sizeof(buffer)) return false;
  if (!process.write_user(frame_at, ConstByteView(buffer))) return false;
  *entry = at;
  entry->gpr[X86Context::kRcx] = record_at;
  entry->gpr[X86Context::kRdx] = context_at;
  entry->gpr[X86Context::kRsp] = frame_at;
  entry->rip = dispatcher;
  entry->rflags = 0x202;
  return true;
}

bool store_context(Process& process, uint64_t address, const X86Context& at) {
  if (process.arch() == UserArch::X86_32) return store_context32(process, address, at);
  uint8_t buffer[kContextBytes];
  if (!process.read_user(address, ByteView(buffer))) return false;
  uint32_t flags = uint32_t(buffer[0x30]) | uint32_t(buffer[0x31]) << 8 | uint32_t(buffer[0x32]) << 16 | uint32_t(buffer[0x33]) << 24;
  if ((flags & kContextAmd64) != kContextAmd64) return false;
  auto put = [&](uint32_t offset, uint64_t value, uint32_t bytes) { for (uint32_t i = 0; i < bytes; ++i) buffer[offset + i] = uint8_t(value >> (8 * i)); };
  if (flags & kContextControl) {
    put(0x38, 0x33, 2);      // cs
    put(0x42, 0x2B, 2);      // ss
    put(0x44, at.rflags, 4);
    put(0x98, at.gpr[X86Context::kRsp], 8);
    put(0xF8, at.rip, 8);
  }
  if (flags & kContextSegments) { put(0x3A, 0x2B, 2); put(0x3C, 0x2B, 2); put(0x3E, 0x2B, 2); put(0x40, 0x2B, 2); }
  if (flags & kContextInteger) {
    for (uint32_t i = 0; i < 16; ++i) if (i != X86Context::kRsp) put(0x78 + 8 * i, at.gpr[i], 8);
  }
  if (flags & kContextFloatingPoint) {
    for (uint32_t i = 0; i < 512; ++i) buffer[0x100 + i] = at.fpu[i];
    put(0x34, mxcsr_of(at.fpu), 4);
  }
  return process.write_user(address, ConstByteView(buffer));
}

bool apply_context(Process& process, uint64_t address, X86Context* context) {
  if (process.arch() == UserArch::X86_32) return apply_context32(process, address, context);
  uint8_t buffer[kContextBytes];
  if (!process.read_user(address, ByteView(buffer))) return false;
  ByteReader r{ConstByteView(buffer)};
  r.take(0x30);
  const uint32_t flags = r.le32();
  const uint32_t mxcsr = r.le32();
  r.take(12);      // segments
  const uint32_t eflags = r.le32();
  r.take(0x30);
  uint64_t gpr[16], rip;
  for (uint64_t& value : gpr) value = r.le64();
  rip = r.le64();
  ConstByteView fpu = r.take(512);
  if (!r.ok() || (flags & kContextAmd64) != kContextAmd64) return false;
  if (flags & kContextControl) {
    if (rip >= kUserLimit || gpr[X86Context::kRsp] >= kUserLimit) return false;
    context->rip = rip;
    context->gpr[X86Context::kRsp] = gpr[X86Context::kRsp];
    context->rflags = (uint64_t{eflags} & kFlagsUser) | 0x202;
  }
  if (flags & kContextInteger) {
    for (uint32_t i = 0; i < 16; ++i) if (i != X86Context::kRsp) context->gpr[i] = gpr[i];
  }
  if (flags & kContextFloatingPoint) {
    for (uint32_t i = 0; i < 512; ++i) context->fpu[i] = fpu[i];
    const uint32_t safe = (mxcsr == 0 ? mxcsr_of(context->fpu) : mxcsr) & kMxcsrUsable;
    for (uint32_t i = 0; i < 4; ++i) context->fpu[24 + i] = uint8_t(safe >> (8 * i));
  }
  return true;
}

}  // namespace process
