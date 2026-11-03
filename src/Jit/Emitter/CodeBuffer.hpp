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
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"

namespace jit {

// A label in a CodeBuffer: a position that jumps can name before it is known.
struct Label {
  uint32_t id = 0xFFFFFFFFu;
  bool valid() const { return id != 0xFFFFFFFFu; }
};

// How a jump to a label is encoded, i.e. which bits of the instruction hold the distance and in what unit.
enum class FixupKind : uint8_t {
  X86Rel32,          // four bytes at the fixup, relative to the end of them
  A64Branch26,       // B/BL: imm26, in instructions, relative to the instruction
  A64Branch19,       // B.cond/CBZ/CBNZ: imm19 at bit 5, in instructions
  RvJal20,           // JAL: the J-type immediate, in bytes (a multiple of 2)
  RvBranch12,        // BEQ/BNE/BLT/...: the B-type immediate, in bytes (a multiple of 2)
  LaB26,             // LoongArch B/BL: offs26 (bits 9:0 at bit 10, bits 25:16 at bit 0), in instructions
  LaBranch16,        // LoongArch BEQ/BNE/BLT/BGE/BLTU/BGEU: offs16 at bit 10, in instructions
};

// The bytes of a function being generated, with labels. Emitters (X86_64Assembler, AArch64Assembler, RiscV64Assembler, LoongArch64Assembler) append instructions;
// finish() patches every jump once all labels are bound and says whether each distance fitted its encoding. Every method that grows
// the buffer returns false when memory ran out; once one has failed the buffer is `failed()` and finish() fails too, so an emitter may
// ignore the results of single calls and check once at the end.
class CodeBuffer {
 public:
  explicit CodeBuffer(allocators::IAllocator& allocator) : bytes_(allocator), labels_(allocator), fixups_(allocator) {}

  uint64_t size() const { return bytes_.size(); }
  ConstByteView view() const { return bytes_.view(); }
  bool failed() const { return failed_; }

  bool emit8(uint8_t v) { return note(bytes_.put8(v)); }
  bool emit16(uint32_t v) { return note(bytes_.put16(v)); }
  bool emit32(uint32_t v) { return note(bytes_.put32(v)); }
  bool emit64(uint64_t v) { return emit32(static_cast<uint32_t>(v)) && emit32(static_cast<uint32_t>(v >> 32)); }
  bool emit(ConstByteView data) { return note(bytes_.append(data)); }
  // Pads with `fill` up to a multiple of `alignment` (a power of two).
  bool align(uint32_t alignment, uint8_t fill);

  // Labels. new_label() creates an unbound one; bind() binds it to the current position (once).
  Label new_label();
  bool bind(Label label);
  bool is_bound(Label label) const;
  // The offset a bound label was bound at.
  bool offset_of(Label label, uint64_t* out) const;

  // Records that the bytes at `at` (already emitted, as a placeholder) are a jump of `kind` to `label`. `base` is the offset the distance is measured from
  // for kinds that do not measure from the fixup itself (X86Rel32: the end of the instruction).
  bool add_fixup(uint64_t at, FixupKind kind, Label label, uint64_t base);
  // Patches every fixup. False if a label was never bound or a distance does not fit; the code must not be used then.
  bool finish();

  // Reads / overwrites a 32-bit word already emitted (little endian).
  bool read32(uint64_t offset, uint32_t* out) const;
  bool patch32(uint64_t offset, uint32_t v) { return bytes_.patch32(offset, v); }

 private:
  struct Fixup {
    uint64_t at;
    uint64_t base;
    uint32_t label;
    FixupKind kind;
  };
  struct LabelState {
    uint64_t offset;
    bool bound;
  };
  bool note(bool ok) {
    if (!ok) failed_ = true;
    return ok;
  }
  bool apply(const Fixup& fixup, uint64_t target);

  ByteBuffer bytes_;
  List<LabelState> labels_;
  List<Fixup> fixups_;
  bool failed_ = false;
};

}  // namespace jit
