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
constexpr uint64_t kFlagsUser = 0x0CD5;                      // CF PF AF ZF SF DF OF: what a program may set
constexpr uint32_t kMxcsrUsable = 0xFFBF;

uint32_t mxcsr_of(const uint8_t* fpu) { return uint32_t(fpu[24]) | uint32_t(fpu[25]) << 8 | uint32_t(fpu[26]) << 16 | uint32_t(fpu[27]) << 24; }

}  // namespace

bool fault_to_exception(uint64_t vector, uint64_t error, uint64_t cr2, X86Context* at, ExceptionInfo* out) {
  *out = ExceptionInfo();
  out->address = at->rip;
  switch (vector & 0xFF) {
    case 0: out->code = 0xC0000094u; return true;      // STATUS_INTEGER_DIVIDE_BY_ZERO
    case 1: out->code = 0x80000004u; return true;      // STATUS_SINGLE_STEP
    case 3:
      out->code = 0x80000003u;      // STATUS_BREAKPOINT
      at->rip -= 1;
      out->address = at->rip;
      out->count = 1;
      return true;
    case 4: out->code = 0xC0000095u; return true;      // STATUS_INTEGER_OVERFLOW
    case 5: out->code = 0xC000008Cu; return true;      // STATUS_ARRAY_BOUNDS_EXCEEDED
    case 6: case 7: out->code = 0xC000001Du; return true;      // STATUS_ILLEGAL_INSTRUCTION
    case 13: out->code = 0xC0000096u; return true;      // STATUS_PRIVILEGED_INSTRUCTION
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

bool apply_context(Process& process, uint64_t address, X86Context* context) {
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
