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

#include "Process/WindowsFonts.hpp"
#include "Process/Objects.hpp"
#include "Utility/Utf16.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidParameter = 0xC000000Du, kNoMemory = 0xC0000017u, kNotSupported = 0xC00000BBu,
                   kBufferTooSmall = 0xC0000023u, kInvalidImageFormat = 0xC000007Bu;
constexpr uint32_t kRequestSize = 64, kMaxCount = 65536, kInfoSize = 64, kBoxSize = 20;
constexpr uint32_t kMaxChunk = 1u << 20;
constexpr uint64_t kMaxFontFile = 8ull * 1024 * 1024;

enum Operation : uint32_t { kFaceCount = 1, kFaceInfo, kFaceString, kMapCodepoints, kGlyphBoxes, kKerning, kRasterize, kOutline, kFontData, kAddFont };

IFontService* g_service = nullptr;

uint32_t le32(ConstByteView b, uint32_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; }
uint64_t le64(ConstByteView b, uint32_t at) { return uint64_t(le32(b, at)) | uint64_t(le32(b, at + 4)) << 32; }

struct Request {
  ConstByteView raw;
  uint32_t op() const { return le32(raw, 4); }
  uint32_t face() const { return le32(raw, 8); }
  uint32_t x() const { return le32(raw, 12); }
  uint32_t y() const { return le32(raw, 16); }
  uint32_t count() const { return le32(raw, 20); }
  uint64_t in() const { return le64(raw, 24); }
  uint64_t out() const { return le64(raw, 32); }
  uint32_t capacity() const { return le32(raw, 40); }
};

// The four results go back into the program's structure.
bool write_results(Process& p, uint64_t at, uint32_t r0, uint32_t r1 = 0, uint32_t r2 = 0, uint32_t r3 = 0) {
  uint8_t bytes[16];
  const uint32_t values[4] = {r0, r1, r2, r3};
  for (uint32_t i = 0; i < 4; ++i)
    for (uint32_t j = 0; j < 4; ++j) bytes[i * 4 + j] = uint8_t(values[i] >> (8 * j));
  return p.write_user(at + 44, ConstByteView(bytes, 16));
}

// `size` bytes of the program's memory at `address` as a buffer of ours.
uint32_t read_block(Process& p, uint64_t address, uint64_t size, ByteBuffer* out) {
  if (!out->put_zeros(size)) return kNoMemory;
  return p.read_user(address, out->writable_view()) ? kSuccess : kAccessViolation;
}

uint32_t write_block(Process& p, uint64_t address, ConstByteView data) { return data.empty() || p.write_user(address, data) ? kSuccess : kAccessViolation; }

}  // namespace

void set_font_service(IFontService* service) { g_service = service; }
IFontService* font_service() { return g_service; }

uint32_t nt_tcos_font(Process& p, const SyscallRequest& r) {
  uint8_t storage[kRequestSize];
  if (!p.read_user(r.args[0], ByteView(storage, kRequestSize))) return kAccessViolation;
  const Request q{ConstByteView(storage, kRequestSize)};
  if (le32(q.raw, 0) != kRequestSize) return kInvalidParameter;
  IFontService* fonts = g_service;
  if (!fonts) return kNotSupported;
  const uint64_t request_at = r.args[0];
  ObjectManager* objects = system_objects();
  if (objects) objects->sync().lock();
  struct Unlock {
    ObjectManager* objects;
    ~Unlock() { if (objects) objects->sync().unlock(); }
  } unlock{objects};

  allocators::IAllocator& allocator = fonts->allocator();
  const uint32_t face = q.face();
  switch (q.op()) {
    case kFaceCount: return write_results(p, request_at, fonts->face_count()) ? kSuccess : kAccessViolation;
    case kFaceInfo: {
      FontFaceInfo info;
      if (!fonts->face_info(face, &info)) return kInvalidParameter;
      ByteBuffer out(allocator);
      const uint32_t flags = (info.italic ? 1u : 0) | (info.bold ? 2u : 0) | (info.bundled ? 4u : 0) | (info.monospaced ? 8u : 0);
      if (!out.put32(info.units_per_em) || !out.put32(uint32_t(info.ascent)) || !out.put32(uint32_t(info.descent)) || !out.put32(uint32_t(info.line_gap)) ||
          !out.put32(uint32_t(info.x_height)) || !out.put32(uint32_t(info.cap_height)) || !out.put32(info.glyph_count) || !out.put16(info.weight) || !out.put16(info.stretch) ||
          !out.put32(flags) || !out.put_zeros(kInfoSize - out.size()))
        return kNoMemory;
      return write_block(p, q.out(), out.view());
    }
    case kFaceString: {
      String text(allocator);
      if (q.x() > 3 || !fonts->face_string(face, q.x(), &text)) return kInvalidParameter;
      ByteBuffer units(allocator);
      if (!util::append_utf16le(&units, text.view())) return kNoMemory;
      const uint32_t length = uint32_t(units.size() / 2);
      if (!write_results(p, request_at, length)) return kAccessViolation;
      if (uint64_t{length} * 2 + 2 > q.capacity()) return kBufferTooSmall;
      if (!units.put16(0)) return kNoMemory;
      return write_block(p, q.out(), units.view());
    }
    case kMapCodepoints: {
      if (q.count() > kMaxCount) return kInvalidParameter;
      ByteBuffer in(allocator), out(allocator);
      uint32_t status = read_block(p, q.in(), uint64_t{q.count()} * 4, &in);
      if (status != kSuccess) return status;
      for (uint32_t i = 0; i < q.count(); ++i) {
        uint32_t glyph = 0;
        if (!fonts->map_codepoint(face, le32(in.view(), i * 4), &glyph)) return kInvalidParameter;
        if (!out.put16(glyph > 0xFFFF ? 0 : glyph)) return kNoMemory;
      }
      return write_block(p, q.out(), out.view());
    }
    case kGlyphBoxes: {
      if (q.count() > kMaxCount) return kInvalidParameter;
      ByteBuffer in(allocator), out(allocator);
      uint32_t status = read_block(p, q.in(), uint64_t{q.count()} * 2, &in);
      if (status != kSuccess) return status;
      for (uint32_t i = 0; i < q.count(); ++i) {
        FontGlyphBox box;
        const uint32_t glyph = uint32_t(in.view()[i * 2]) | uint32_t(in.view()[i * 2 + 1]) << 8;
        if (!fonts->glyph_box(face, glyph, &box)) box = FontGlyphBox();
        if (!out.put32(uint32_t(box.advance)) || !out.put32(uint32_t(box.x_min)) || !out.put32(uint32_t(box.y_min)) || !out.put32(uint32_t(box.x_max)) || !out.put32(uint32_t(box.y_max)))
          return kNoMemory;
      }
      return write_block(p, q.out(), out.view());
    }
    case kKerning: {
      int32_t units = 0;
      if (!fonts->kerning(face, q.x(), q.y(), &units)) return kInvalidParameter;
      return write_results(p, request_at, uint32_t(units)) ? kSuccess : kAccessViolation;
    }
    case kRasterize: {
      if (q.y() > 3) return kInvalidParameter;
      FontBitmap bitmap(allocator);
      if (!fonts->rasterize(face, q.x(), int32_t(q.count()), q.y(), &bitmap)) return kInvalidParameter;
      if (!write_results(p, request_at, uint32_t(bitmap.left), uint32_t(bitmap.top), bitmap.width, bitmap.height)) return kAccessViolation;
      if (uint64_t{bitmap.width} * bitmap.height > q.capacity()) return q.capacity() == 0 ? kSuccess : kBufferTooSmall;
      return write_block(p, q.out(), bitmap.pixels.view());
    }
    case kOutline: {
      ByteBuffer ends(allocator), points(allocator), out(allocator);
      if (!fonts->outline(face, q.x(), &ends, &points)) return kInvalidParameter;
      if (!out.put32(uint32_t(ends.size() / 4)) || !out.put32(uint32_t(points.size() / 12)) || !out.append(ends.view()) || !out.append(points.view())) return kNoMemory;
      if (!write_results(p, request_at, uint32_t(out.size()))) return kAccessViolation;
      if (out.size() > q.capacity()) return kBufferTooSmall;
      return write_block(p, q.out(), out.view());
    }
    case kFontData: {
      const uint32_t want = q.capacity() < kMaxChunk ? q.capacity() : kMaxChunk;      // (the program asks again for the rest)
      ByteBuffer out(allocator);
      if (!out.put_zeros(want)) return kNoMemory;
      uint64_t total = 0, copied = 0;
      if (!fonts->font_data(face, q.x(), out.writable_view(), &total, &copied)) return kInvalidParameter;
      if (!write_results(p, request_at, uint32_t(total), uint32_t(copied))) return kAccessViolation;
      return write_block(p, q.out(), out.view().first(copied));
    }
    case kAddFont: {
      if (q.count() == 0 || q.count() > kMaxFontFile) return kInvalidParameter;
      ByteBuffer data(allocator);
      uint32_t status = read_block(p, q.in(), q.count(), &data);
      if (status != kSuccess) return status;
      uint32_t added = 0;
      if (!fonts->add_font(data.view(), &added)) return kInvalidImageFormat;
      return write_results(p, request_at, added) ? kSuccess : kAccessViolation;
    }
    default: return kInvalidParameter;
  }
}

}  // namespace process::windowsabi
