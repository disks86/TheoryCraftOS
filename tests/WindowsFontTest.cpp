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

// Host tests of NtTcosFont (the service behind dwrite.dll): the request structure is read from the program's memory, every operation answers through the system's fonts
// (src/Font/SystemFontService.cpp over the bundled faces), the results and buffers are written back, and damaged requests never go wrong.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Font/SystemFontService.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsFonts.hpp"
#include "Process/WindowsSyscalls.hpp"

using namespace process;

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};
static TestAllocator A;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint64_t kBase = 0x100000, kSize = 0x200000;
struct FakeMemory final : IUserMemory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize, 0);
  uint64_t limit = kSize;
  bool read(uint64_t a, ByteView out) override {
    if (a < kBase || a - kBase > limit || out.size() > limit - (a - kBase)) return false;
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = bytes[a - kBase + i];
    return true;
  }
  bool write(uint64_t a, ConstByteView in) override {
    if (a < kBase || a - kBase > limit || in.size() > limit - (a - kBase)) return false;
    for (uint64_t i = 0; i < in.size(); ++i) bytes[a - kBase + i] = in[i];
    return true;
  }
  void put32(uint64_t a, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  void put64(uint64_t a, uint64_t v) { for (int i = 0; i < 8; ++i) bytes[a - kBase + i] = uint8_t(v >> (8 * i)); }
  uint32_t get32(uint64_t a) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(bytes[a - kBase + i]) << (8 * i); return v; }
  uint32_t get16(uint64_t a) { return uint32_t(bytes[a - kBase]) | uint32_t(bytes[a - kBase + 1]) << 8; }
};

struct Host final : IProcessHost {
  bool write_console(uint32_t, ConstByteView) override { return true; }
  bool read_console(ByteView, uint64_t* got) override { *got = 0; return true; }
  uint64_t realtime_ns() override { return 0; }
  uint64_t monotonic_ns() override { return 0; }
  void sleep_ns(uint64_t) override {}
  void yield() override {}
};

struct Prog {
  FakeMemory memory;
  Host host;
  Process process;
  static constexpr uint64_t kRequest = kBase, kIn = kBase + 0x1000, kOut = kBase + 0x10000;
  Prog() : process(1, ProcessBinding{SyscallAbi::Windows, UserArch::X86_64, syscall_interface(SyscallAbi::Windows, UserArch::X86_64)}, memory, host) {}
  void request(uint32_t op, uint32_t face, uint32_t x, uint32_t y, uint32_t count, uint64_t in, uint64_t out, uint32_t capacity) {
    memory.put32(kRequest, 64);
    memory.put32(kRequest + 4, op);
    memory.put32(kRequest + 8, face);
    memory.put32(kRequest + 12, x);
    memory.put32(kRequest + 16, y);
    memory.put32(kRequest + 20, count);
    memory.put64(kRequest + 24, in);
    memory.put64(kRequest + 32, out);
    memory.put32(kRequest + 40, capacity);
  }
  uint32_t call() {
    SyscallRequest r;
    r.number = uint64_t(WindowsSyscallNumber::NtTcosFont);
    r.args[0] = kRequest;
    r.user_stack = kBase + 0x1F000;
    return uint32_t(syscall_interface(SyscallAbi::Windows, UserArch::X86_64)->dispatch(process, r));
  }
  uint32_t result(int i) { return memory.get32(kRequest + 44 + 4 * i); }
};

constexpr uint32_t kInvalid = 0xC000000Du, kSmall = 0xC0000023u, kViolation = 0xC0000005u;

static void test_catalog(Prog& p) {
  p.request(1, 0, 0, 0, 0, 0, 0, 0);
  CHECK(p.call() == 0);
  const uint32_t faces = p.result(0);
  CHECK(faces >= 13);      // sans, serif, mono (4 each) and the fallback
  // face info of face 0 (a bundled sans)
  p.request(2, 0, 0, 0, 0, 0, Prog::kOut, 64);
  CHECK(p.call() == 0);
  CHECK(p.memory.get32(Prog::kOut) == 2048);      // Liberation's em square
  CHECK(int32_t(p.memory.get32(Prog::kOut + 4)) > 0 && int32_t(p.memory.get32(Prog::kOut + 8)) > 0);
  CHECK(p.memory.get32(Prog::kOut + 24) > 200);      // glyphs
  CHECK(p.memory.get16(Prog::kOut + 28) == 400);
  CHECK((p.memory.get32(Prog::kOut + 32) & 4) != 0);      // bundled
  // family name, and the buffer limits
  p.request(3, 0, 0, 0, 0, 0, Prog::kOut, 64);
  CHECK(p.call() == 0 && p.result(0) == 9 && p.memory.get16(Prog::kOut) == 'T' && p.memory.get16(Prog::kOut + 18) == 0);      // "TCOS Sans"
  p.request(3, 0, 0, 0, 0, 0, Prog::kOut, 18);
  CHECK(p.call() == kSmall && p.result(0) == 9);
  p.request(3, 0, 0, 0, 0, 0, Prog::kOut, 20);
  CHECK(p.call() == 0);
  p.request(3, 0, 9, 0, 0, 0, Prog::kOut, 64);
  CHECK(p.call() == kInvalid);
  p.request(2, faces, 0, 0, 0, 0, Prog::kOut, 64);      // a face that does not exist
  CHECK(p.call() == kInvalid);
  // a monospaced face is flagged
  bool mono_seen = false, proportional_seen = false;
  for (uint32_t f = 0; f < faces; ++f) {
    p.request(2, f, 0, 0, 0, 0, Prog::kOut, 64);
    if (p.call() != 0) continue;
    if (p.memory.get32(Prog::kOut + 32) & 8) mono_seen = true; else proportional_seen = true;
  }
  CHECK(mono_seen && proportional_seen);
}

static void test_glyphs(Prog& p) {
  // 'A', 'B', a space and a code point the font has not map to glyphs; map twice gives the same
  const uint32_t cps[4] = {'A', 'B', ' ', 0x10FFFF};
  for (int i = 0; i < 4; ++i) p.memory.put32(Prog::kIn + 4 * i, cps[i]);
  p.request(4, 0, 0, 0, 4, Prog::kIn, Prog::kOut, 8);
  CHECK(p.call() == 0);
  const uint32_t a = p.memory.get16(Prog::kOut), b = p.memory.get16(Prog::kOut + 2), space = p.memory.get16(Prog::kOut + 4), missing = p.memory.get16(Prog::kOut + 6);
  CHECK(a != 0 && b != 0 && a != b && space != 0 && missing == 0);
  // boxes: A has an outline, the space has an advance and none
  p.memory.bytes[Prog::kIn - kBase] = uint8_t(a); p.memory.bytes[Prog::kIn - kBase + 1] = uint8_t(a >> 8);
  p.memory.bytes[Prog::kIn - kBase + 2] = uint8_t(space); p.memory.bytes[Prog::kIn - kBase + 3] = uint8_t(space >> 8);
  p.request(5, 0, 0, 0, 2, Prog::kIn, Prog::kOut, 40);
  CHECK(p.call() == 0);
  CHECK(int32_t(p.memory.get32(Prog::kOut)) > 1000);      // A's advance in 2048ths
  CHECK(int32_t(p.memory.get32(Prog::kOut + 16)) > int32_t(p.memory.get32(Prog::kOut + 8)));      // a box
  CHECK(int32_t(p.memory.get32(Prog::kOut + 20)) > 0 && p.memory.get32(Prog::kOut + 24) == 0 && p.memory.get32(Prog::kOut + 36) == 0);      // the space: advance, no box
  // kerning exists as a call (a pair may or may not kern)
  p.request(6, 0, a, b, 0, 0, 0, 0);
  CHECK(p.call() == 0);
  // too many code points
  p.request(4, 0, 0, 0, 70000, Prog::kIn, Prog::kOut, 8);
  CHECK(p.call() == kInvalid);
}

static void test_raster_and_outline(Prog& p) {
  p.memory.put32(Prog::kIn, 'H');
  p.request(4, 0, 0, 0, 1, Prog::kIn, Prog::kOut, 2);
  CHECK(p.call() == 0);
  const uint32_t h = p.memory.get16(Prog::kOut);
  // size query: capacity 0
  p.request(7, 0, h, 0, 16 * 64, 0, Prog::kOut, 0);
  CHECK(p.call() == 0);
  const uint32_t w = p.result(2), ht = p.result(3);
  CHECK(w >= 6 && w <= 16 && ht >= 8 && ht <= 16);
  CHECK(int32_t(p.result(1)) >= int32_t(ht) - 2);      // top is about the cap height
  p.request(7, 0, h, 0, 16 * 64, 0, Prog::kOut, w * ht - 1);
  CHECK(p.call() == kSmall);
  p.request(7, 0, h, 0, 16 * 64, 0, Prog::kOut, w * ht);
  CHECK(p.call() == 0);
  uint32_t inked = 0;
  for (uint32_t i = 0; i < w * ht; ++i) inked += p.memory.bytes[Prog::kOut - kBase + i] > 128;
  CHECK(inked > 10 && inked < w * ht);
  p.request(7, 0, h, 4, 16 * 64, 0, Prog::kOut, 0);      // subpixel out of range
  CHECK(p.call() == kInvalid);
  p.request(7, 0, h, 0, 0, 0, Prog::kOut, 0);      // size 0
  CHECK(p.call() == kInvalid);
  // outline: H has one contour of 12 points
  p.request(8, 0, h, 0, 0, 0, Prog::kOut, 4096);
  CHECK(p.call() == 0);
  CHECK(p.memory.get32(Prog::kOut) == 1 && p.memory.get32(Prog::kOut + 4) == 12 && p.result(0) == 8 + 4 + 12 * 12);
  p.request(8, 0, h, 0, 0, 0, Prog::kOut, 20);
  CHECK(p.call() == kSmall);
}

static void test_data_and_add(Prog& p) {
  p.request(9, 0, 0, 0, 0, 0, Prog::kOut, 12);
  CHECK(p.call() == 0 && p.result(1) == 12);
  const uint32_t total = p.result(0);
  CHECK(total > 10000);
  const uint32_t magic = p.memory.get32(Prog::kOut);
  CHECK(magic == 0x00000100u || magic == 0x65757274u);      // a TrueType sfnt (big endian 00010000 or "true", read little endian)
  p.request(9, 0, total - 4, 0, 0, 0, Prog::kOut, 100);      // the end of the file: only 4 bytes left
  CHECK(p.call() == 0 && p.result(1) == 4);
  p.request(9, 0, total + 10, 0, 0, 0, Prog::kOut, 100);
  CHECK(p.call() == 0 && p.result(1) == 0);
  // the font file comes back through ADD_FONT as a new face with the same glyphs
  constexpr uint64_t kCopy = kBase + 0x80000;
  CHECK(total <= 0x80000);
  p.request(9, 0, 0, 0, 0, 0, kCopy, 0x80000);
  CHECK(p.call() == 0 && p.result(1) == total);
  p.request(1, 0, 0, 0, 0, 0, 0, 0);
  CHECK(p.call() == 0);
  const uint32_t before = p.result(0);
  p.request(10, 0, 0, 0, total, kCopy, 0, 0);
  CHECK(p.call() == 0);
  CHECK(p.result(0) == before);      // (the new face is number `before`)
  p.memory.put32(Prog::kIn, 'A');
  p.request(4, before, 0, 0, 1, Prog::kIn, Prog::kOut, 2);
  CHECK(p.call() == 0 && p.memory.get16(Prog::kOut) != 0);
  p.request(3, before, 0, 0, 0, 0, Prog::kOut, 64);
  CHECK(p.call() == 0 && p.result(0) == 9);      // (the bundled subsets have no name table: the face keeps the kernel's family name)
  // garbage is not a font
  p.request(10, 0, 0, 0, 64, Prog::kIn, 0, 0);
  CHECK(p.call() == 0xC000007Bu);
  p.request(10, 0, 0, 0, 0, Prog::kIn, 0, 0);
  CHECK(p.call() == kInvalid);
}

static void test_damage(Prog& p) {
  p.request(99, 0, 0, 0, 0, 0, 0, 0);
  CHECK(p.call() == kInvalid);
  p.memory.put32(Prog::kRequest, 60);      // wrong size
  CHECK(p.call() == kInvalid);
  p.request(2, 0, 0, 0, 0, 0, 0xDEAD0000, 64);      // the buffer is not the program's
  CHECK(p.call() == kViolation);
  p.request(4, 0, 0, 0, 4, 0xDEAD0000, Prog::kOut, 8);
  CHECK(p.call() == kViolation);
  // bit flips in the request never crash
  for (uint32_t byte = 4; byte < 44; ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      p.request(5, 0, 0, 0, 1, Prog::kIn, Prog::kOut, 40);
      p.memory.bytes[Prog::kRequest - kBase + byte] ^= uint8_t(1 << bit);
      p.call();
    }
  // a truncated program memory
  p.request(1, 0, 0, 0, 0, 0, 0, 0);
  p.memory.limit = 32;
  CHECK(p.call() == kViolation);
  p.memory.limit = kSize;
  // no fonts at all
  windowsabi::IFontService* saved = windowsabi::font_service();
  windowsabi::set_font_service(nullptr);
  p.request(1, 0, 0, 0, 0, 0, 0, 0);
  CHECK(p.call() == 0xC00000BBu);
  windowsabi::set_font_service(saved);
}

int main() {
  font::SystemFontService fonts(A);
  windowsabi::set_font_service(&fonts);
  Prog p;
  test_catalog(p);
  test_glyphs(p);
  test_raster_and_outline(p);
  test_data_and_add(p);
  test_damage(p);
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("WindowsFontTest passed\n");
  return 0;
}
