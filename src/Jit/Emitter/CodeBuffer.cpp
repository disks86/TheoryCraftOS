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
#include "Jit/Emitter/CodeBuffer.hpp"

namespace jit {

namespace {
// Does `v` fit a signed field of `bits` bits?
bool fits_signed(int64_t v, uint32_t bits) {
  const int64_t limit = int64_t{1} << (bits - 1);
  return v >= -limit && v < limit;
}
}  // namespace

bool CodeBuffer::align(uint32_t alignment, uint8_t fill) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return note(false);
  while (bytes_.size() % alignment != 0) {
    if (!emit8(fill)) return false;
  }
  return true;
}

Label CodeBuffer::new_label() {
  Label label;
  if (!note(labels_.push_back(LabelState{0, false}))) return label;
  label.id = static_cast<uint32_t>(labels_.count() - 1);
  return label;
}

bool CodeBuffer::bind(Label label) {
  LabelState* state = label.valid() ? labels_.get(label.id) : nullptr;
  if (!state || state->bound) return note(false);
  state->offset = bytes_.size();
  state->bound = true;
  return true;
}

bool CodeBuffer::is_bound(Label label) const {
  const LabelState* state = label.valid() ? labels_.get(label.id) : nullptr;
  return state && state->bound;
}

bool CodeBuffer::offset_of(Label label, uint64_t* out) const {
  const LabelState* state = label.valid() ? labels_.get(label.id) : nullptr;
  if (!state || !state->bound) return false;
  *out = state->offset;
  return true;
}

bool CodeBuffer::add_fixup(uint64_t at, FixupKind kind, Label label, uint64_t base) {
  if (!label.valid() || label.id >= labels_.count() || at > bytes_.size()) return note(false);
  return note(fixups_.push_back(Fixup{at, base, label.id, kind}));
}

bool CodeBuffer::read32(uint64_t offset, uint32_t* out) const {
  const ConstByteView all = bytes_.view();
  ConstByteView word;
  if (!all.try_subview(offset, 4, &word)) return false;
  *out = static_cast<uint32_t>(word[0]) | static_cast<uint32_t>(word[1]) << 8 | static_cast<uint32_t>(word[2]) << 16 | static_cast<uint32_t>(word[3]) << 24;
  return true;
}

bool CodeBuffer::apply(const Fixup& fixup, uint64_t target) {
  // Offsets are far below 2^62, so the differences cannot overflow int64_t.
  const int64_t from_base = static_cast<int64_t>(target) - static_cast<int64_t>(fixup.base);
  uint32_t word = 0;
  switch (fixup.kind) {
    case FixupKind::X86Rel32:
      return fits_signed(from_base, 32) && bytes_.patch32(fixup.at, static_cast<uint32_t>(from_base));
    case FixupKind::A64Branch26: {
      if (from_base % 4 != 0 || !fits_signed(from_base / 4, 26) || !read32(fixup.at, &word)) return false;
      return patch32(fixup.at, (word & 0xFC000000u) | (static_cast<uint32_t>(from_base / 4) & 0x03FFFFFFu));
    }
    case FixupKind::A64Branch19: {
      if (from_base % 4 != 0 || !fits_signed(from_base / 4, 19) || !read32(fixup.at, &word)) return false;
      return patch32(fixup.at, (word & ~(0x7FFFFu << 5)) | ((static_cast<uint32_t>(from_base / 4) & 0x7FFFFu) << 5));
    }
    case FixupKind::RvJal20: {
      if (from_base % 2 != 0 || !fits_signed(from_base, 21) || !read32(fixup.at, &word)) return false;
      const uint32_t i = static_cast<uint32_t>(from_base);
      const uint32_t imm = ((i >> 20) & 1u) << 31 | ((i >> 1) & 0x3FFu) << 21 | ((i >> 11) & 1u) << 20 | ((i >> 12) & 0xFFu) << 12;
      return patch32(fixup.at, (word & 0xFFFu) | imm);
    }
    case FixupKind::RvBranch12: {
      if (from_base % 2 != 0 || !fits_signed(from_base, 13) || !read32(fixup.at, &word)) return false;
      const uint32_t i = static_cast<uint32_t>(from_base);
      const uint32_t imm = ((i >> 12) & 1u) << 31 | ((i >> 5) & 0x3Fu) << 25 | ((i >> 1) & 0xFu) << 8 | ((i >> 11) & 1u) << 7;
      return patch32(fixup.at, (word & 0x01FFF07Fu) | imm);
    }
  }
  return false;
}

bool CodeBuffer::finish() {
  if (failed_) return false;
  for (const Fixup& fixup : fixups_.view()) {
    const LabelState* state = labels_.get(fixup.label);
    if (!state || !state->bound || !apply(fixup, state->offset)) return false;
  }
  fixups_.clear();
  return true;
}

}  // namespace jit
