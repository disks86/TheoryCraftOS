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

// The stream of GL commands from a user-mode library to the kernel. Every personality's library (opengl32.dll, a Linux libGL) writes the same words, so the kernel has one decoder.
//
// A command is a header word (opcode in the low 16 bits, length in words including the header in the high 16 bits) and its arguments: one word per value, two (low first) for a
// 64-bit value or an address, and arrays of a size the registry fixes inline, rounded up to words. Opcodes are generated (scripts/gen-opengl.py) and only have to agree within one
// build of the system: the libraries are part of it.
//
// A command that is not followed by anything the caller waits for stays in the library's buffer until the buffer is full or a command comes that has a result or reads the program's
// memory; that one is sent with everything before it, and the kernel runs it before the call returns. So the kernel may read through an address only while it runs that one command.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace opengl {

// An address in the calling program. Not a pointer: reached only through IClientMemory, which checks it and survives any value.
struct UserPtr {
  uint64_t address = 0;
  bool null() const { return address == 0; }
  UserPtr plus(uint64_t n) const { return UserPtr{address + n}; }
};

constexpr uint32_t kMaxStreamWords = 16384;      // what one submission may hold
constexpr uint32_t kMaxStreamBytes = kMaxStreamWords * 4;

class StreamReader {
 public:
  explicit StreamReader(ConstByteView bytes) : bytes_(bytes) {}
  uint64_t remaining() const { return bytes_.size() - at_; }
  bool done() const { return at_ >= bytes_.size(); }
  bool failed() const { return failed_; }
  void fail() { failed_ = true; }
  uint32_t u32() {
    if (bytes_.size() - at_ < 4) { failed_ = true; at_ = bytes_.size(); return 0; }
    const uint8_t* p = bytes_.data() + at_;
    at_ += 4;
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
  }
  float f32() { return __builtin_bit_cast(float, u32()); }
  uint64_t u64() {
    const uint64_t low = u32();
    return low | uint64_t{u32()} << 32;
  }
  double f64() { return __builtin_bit_cast(double, u64()); }
  // An array of a fixed size, whose bytes sit rounded up to whole words (a short array of 3 takes 2 words).
  template <class T, uint64_t N>
  void array(T (&out)[N]) {
    // (Reading into a local array of a trivial type through its bytes: right size and any alignment by construction.)
    const uint64_t n = sizeof(out);
    const uint64_t rounded = (n + 3) & ~uint64_t{3};
    if (bytes_.size() - at_ < rounded) { failed_ = true; at_ = bytes_.size(); for (uint64_t i = 0; i < N; ++i) out[i] = T(); return; }
    __builtin_memcpy(out, bytes_.data() + at_, n);
    at_ += rounded;
  }
  uint64_t position() const { return at_; }
  void seek(uint64_t at) { at_ = at > bytes_.size() ? bytes_.size() : at; }
  ConstByteView bytes() const { return bytes_; }

 private:
  ConstByteView bytes_;
  uint64_t at_ = 0;
  bool failed_ = false;
};

// The program's memory, as this call may reach it. Reads and writes fail cleanly on a bad address; allocation gives memory the program can use (a mapped buffer, a string the GL
// keeps). A display list's copies of client data use addresses of their own (GlApi::read_client).
class IClientMemory {
 public:
  virtual bool read(uint64_t address, ByteView out) = 0;
  virtual bool write(uint64_t address, ConstByteView data) = 0;
  // Pages of the program's address space, readable and writable; 0 if there are none.
  virtual uint64_t allocate(uint64_t bytes) = 0;
  virtual void release(uint64_t address, uint64_t bytes) = 0;

 protected:
  ~IClientMemory() = default;
};

}  // namespace opengl
