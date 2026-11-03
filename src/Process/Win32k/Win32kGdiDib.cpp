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
#include "Process/Win32k/Win32kGdiInternal.hpp"

// Device-independent bitmaps: BITMAPINFO parsing, DIB sections, CreateDIBitmap, GetDIBits, SetDIBitsToDevice, StretchDIBits and DIB brushes.
namespace process::win32k::gdi {

constexpr uint32_t kMaxDibDimension = 32768;
constexpr uint64_t kMaxDibBytes = 1u << 28;

uint64_t dib_stride(uint32_t width, uint32_t bpp) { return ((uint64_t{width} * bpp + 31) / 32) * 4; }

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }

}  // namespace

bool read_bitmap_info(Call& c, uint64_t address, uint32_t usage, uint32_t max_info, DibInfo* out, const Dc* dc) {
  uint8_t h[124] = {};
  uint32_t size = 0;
  if (!c.read_u32(address, &size)) return false;
  if (size != 12 && size != 40 && size != 52 && size != 56 && size != 108 && size != 124) return false;
  if (max_info != 0 && max_info < size) return false;
  if (!c.read(address, ByteView(h, size))) return false;
  out->header_size = size;
  uint32_t clr_entries_size = 4;
  if (size == 12) {
    out->core = true;
    out->width = int32_t(le16(h + 4));
    out->height = int32_t(le16(h + 6));
    out->planes = le16(h + 8);
    out->bpp = le16(h + 10);
    clr_entries_size = 3;
  } else {
    out->width = int32_t(le32(h + 4));
    out->height = int32_t(le32(h + 8));
    out->planes = le16(h + 12);
    out->bpp = le16(h + 14);
    out->compression = le32(h + 16);
    out->size_image = le32(h + 20);
    out->x_ppm = int32_t(le32(h + 24));
    out->y_ppm = int32_t(le32(h + 28));
    out->clr_used = le32(h + 32);
    out->clr_important = le32(h + 36);
  }
  if (out->height < 0) { out->bottom_up = false; out->height = out->height == INT32_MIN ? INT32_MAX : -out->height; }
  else out->bottom_up = true;
  if (out->width <= 0 || out->height <= 0 || uint32_t(out->width) > kMaxDibDimension || uint32_t(out->height) > kMaxDibDimension) return false;
  if (out->planes != 1) return false;
  const uint32_t bpp = out->bpp;
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return false;
  if (out->compression > 3) return false;
  if (out->compression == 1 && (bpp != 8 || !out->bottom_up)) return false;
  if (out->compression == 2 && (bpp != 4 || !out->bottom_up)) return false;
  if (out->compression == 3 && bpp != 16 && bpp != 32) return false;
  if (dib_stride(uint32_t(out->width), bpp) * uint32_t(out->height) > kMaxDibBytes) return false;
  uint64_t table_at = address + size;
  uint32_t extra = 0;
  if (out->compression == 3) {
    if (size >= 52) { for (int i = 0; i < 3; ++i) out->masks[i] = le32(h + 40 + 4 * i); }
    else {
      uint8_t m[12];
      if (max_info != 0 && max_info < size + 12) return false;
      if (!c.read(address + size, ByteView(m, 12))) return false;
      for (int i = 0; i < 3; ++i) out->masks[i] = le32(m + 4 * i);
      extra = 12;
      table_at += 12;
    }
  } else if (size >= 52 && out->compression == 3) {
    for (int i = 0; i < 3; ++i) out->masks[i] = le32(h + 40 + 4 * i);
  }
  uint32_t count = 0;
  if (bpp <= 8) {
    count = out->clr_used ? out->clr_used : (1u << bpp);
    if (count > (1u << bpp)) return false;
  } else if (out->clr_used && usage != 2) {
    count = out->clr_used > 256 ? 256 : out->clr_used;      // an optimisation table for true colour: present but not used
  }
  const uint64_t bytes = usage == 1 ? uint64_t{count} * 2 : uint64_t{count} * clr_entries_size;
  out->colors_bytes = bytes;
  out->bits_offset = uint64_t{size} + extra + bytes;
  if (bpp <= 8) {
    if (usage == 2 || usage == 1) {
      Palette* pal = dc ? find_palette(dc->s.palette) : nullptr;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = i;
        if (usage == 1) { uint16_t w = 0; if (!c.read_u16(table_at + uint64_t{i} * 2, &w)) return false; idx = w; }
        out->colors.push_back(pal && idx < pal->entries.count() ? pal->entries[idx] & 0xFFFFFF : 0);
      }
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        uint8_t q[4] = {};
        if (max_info != 0 && uint64_t{size} + extra + uint64_t{i + 1} * clr_entries_size > max_info) { out->colors.push_back(0); continue; }
        if (!c.read(table_at + uint64_t{i} * clr_entries_size, ByteView(q, clr_entries_size))) return false;
        out->colors.push_back(make_rgb(q[2], q[1], q[0]));
      }
    }
    while (out->colors.count() < (1u << bpp)) out->colors.push_back(0);
  }
  return true;
}

namespace {

// A DIB's pixels in a form Surface can read.
struct Decoded {
  explicit Decoded(allocators::IAllocator& a) : buf(a), colors(a) {}
  ByteBuffer buf;
  List<uint32_t> colors;
  Surface s;
};

bool rle_decode(const DibInfo& info, ConstByteView bits, Decoded* out) {
  const uint64_t stride = dib_stride(uint32_t(info.width), info.bpp);
  if (!out->buf.put_zeros(stride * uint32_t(info.height))) return false;
  ByteView dst = out->buf.writable_view();
  const bool rle4 = info.compression == 2;
  auto set = [&](int64_t x, int64_t y, uint32_t v) {
    if (x < 0 || y < 0 || x >= info.width || y >= info.height) return;
    if (!rle4) { dst[uint64_t(y) * stride + uint64_t(x)] = uint8_t(v); return; }
    const uint64_t o = uint64_t(y) * stride + uint64_t(x) / 2;
    dst[o] = uint8_t((x & 1) ? ((dst[o] & 0xF0) | (v & 0xF)) : ((dst[o] & 0x0F) | (v << 4)));
  };
  int64_t x = 0, y = 0;
  uint64_t i = 0;
  while (i + 1 < bits.size()) {
    const uint8_t n = bits[i], v = bits[i + 1];
    i += 2;
    if (n > 0) {
      for (uint32_t k = 0; k < n; ++k) set(x + k, y, rle4 ? ((k & 1) ? (v & 0xF) : (v >> 4)) : v);
      x += n;
    } else if (v == 0) { x = 0; ++y; }
    else if (v == 1) break;
    else if (v == 2) { if (i + 1 >= bits.size()) break; x += bits[i]; y += bits[i + 1]; i += 2; }
    else {
      const uint32_t bytes = rle4 ? (v + 1u) / 2 : v;
      if (bytes > bits.size() - i) break;
      for (uint32_t k = 0; k < v; ++k) set(x + k, y, rle4 ? ((k & 1) ? (bits[i + k / 2] & 0xF) : (bits[i + k / 2] >> 4)) : bits[i + k]);
      x += v;
      i += bytes + (bytes & 1);
    }
  }
  return true;
}

bool decode(allocators::IAllocator& al, const DibInfo& info, ConstByteView bits, Decoded* out) {
  const uint64_t stride = dib_stride(uint32_t(info.width), info.bpp);
  (void)al;
  if (info.compression == 1 || info.compression == 2) {
    if (!rle_decode(info, bits, out)) return false;
  } else {
    const uint64_t need = stride * uint32_t(info.height);
    if (bits.size() < need || !out->buf.append(bits.first(need))) return false;
  }
  for (uint32_t col : info.colors.view()) out->colors.push_back(col);
  out->s.bytes = out->buf.writable_view();
  out->s.width = info.width;
  out->s.height = info.height;
  out->s.bpp = info.bpp;
  out->s.stride = uint32_t(stride);
  out->s.bottom_up = info.bottom_up || info.compression == 1 || info.compression == 2;
  out->s.colors = ArrayView<const uint32_t>(out->colors.view());
  if (info.compression == 3) for (int i = 0; i < 3; ++i) out->s.masks[i] = info.masks[i];
  return out->s.valid();
}

bool read_bits(Call& c, uint64_t address, const DibInfo& info, uint32_t max_bits, uint32_t usage_rows, ByteBuffer* out) {
  uint64_t size = dib_stride(uint32_t(info.width), info.bpp) * usage_rows;
  if (info.compression == 1 || info.compression == 2) {
    size = info.size_image;
    if (size == 0) size = max_bits;
  }
  if (max_bits != 0 && size > max_bits) size = max_bits;
  if (size > kMaxDibBytes || !out->put_zeros(size)) return false;
  return size == 0 || c.read(address, out->writable_view());
}

// Writes `colors` (the colour table of a surface) into BITMAPINFO at `address`, after the header.
bool write_color_table(Call& c, uint64_t address, uint32_t header_size, ArrayView<const uint32_t> colors, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t col = i < colors.size() ? colors[i] : 0;
    if (!c.write_u32(address + header_size + uint64_t{i} * 4, cr_b(col) | cr_g(col) << 8 | cr_r(col) << 16)) return false;
  }
  return true;
}

}  // namespace

bool decode_dib(const DibInfo& info, ConstByteView bits, List<uint32_t>* pixels) {
  allocators::IAllocator* al = alloc();
  if (!al) return false;
  Decoded d(*al);
  if (!decode(*al, info, bits, &d)) return false;
  for (int32_t y = 0; y < info.height; ++y) for (int32_t x = 0; x < info.width; ++x) if (!pixels->push_back(d.s.get(x, y))) return false;
  return true;
}

bool bitmap_from_dib(Call& c, Bitmap* b, const DibInfo& info, ConstByteView bits) {
  allocators::IAllocator* al = alloc();
  (void)c;
  if (!al) return false;
  Decoded d(*al);
  if (!decode(*al, info, bits, &d)) return false;
  dib_sync_in(*b);
  Surface dst = surface_of(*b);
  for (int32_t y = 0; y < b->height && y < info.height; ++y) for (int32_t x = 0; x < b->width && x < info.width; ++x) dst.put(x, y, d.s.get(x, y));
  dib_sync_out(*b);
  return true;
}

namespace {

uint64_t NtGdiCreateDIBSection(Call& c) {
  if (c.arg32(0) && !dc_arg(c, c.arg32(0))) return 0;
  const uint32_t usage = c.arg32(4), header_size = c.arg32(5);
  const uint64_t bits_ptr = c.arg(8);
  if (c.arg(1) != 0) return c.fail(kErrorNotSupported);      // a file-mapping section as the storage
  if (usage > 2) return c.fail(kErrorInvalidParameter);
  DibInfo info(*alloc());
  if (!read_bitmap_info(c, c.arg(3), usage, header_size, &info, c.arg32(0) ? find_dc(c.arg32(0)) : nullptr) || info.compression == 1 || info.compression == 2) return c.fail(kErrorInvalidParameter);
  Bitmap* b = make_object<Bitmap>(kObjBitmap, c.pid());
  if (!b) return c.fail(kErrorNotEnoughMemory);
  b->width = info.width;
  b->height = info.height;
  b->bpp = info.bpp;
  b->dib = true;
  b->bottom_up = info.bottom_up;
  b->compression = info.compression;
  b->stride = uint32_t(dib_stride(uint32_t(info.width), info.bpp));
  b->x_ppm = uint32_t(info.x_ppm); b->y_ppm = uint32_t(info.y_ppm); b->clr_important = info.clr_important;
  b->dib_usage = usage;
  for (int i = 0; i < 3; ++i) b->masks[i] = info.masks[i];
  for (uint32_t col : info.colors.view()) b->colors.push_back(col);
  const uint64_t bytes = uint64_t{b->stride} * uint32_t(b->height);
  if (!b->data.put_zeros(bytes) || !allocate_dib_memory(c.process(), *b, bytes)) { destroy_object(b->handle); return c.fail(kErrorNotEnoughMemory); }
  dib_sync_out(*b);
  if (bits_ptr != 0 && !c.write_u64(bits_ptr, b->user_address)) { destroy_object(b->handle); return c.fail(kErrorInvalidParameter); }
  return b->handle;
}

uint64_t NtGdiCreateDIBitmapInternal(Call& c) {
  Dc* dc = c.arg32(0) ? dc_arg(c, c.arg32(0)) : nullptr;
  if (c.arg32(0) && !dc) return 0;
  const int32_t w = c.argi(1), h = c.argi(2);
  const uint32_t init = c.arg32(3), usage = c.arg32(6), max_info = c.arg32(7), max_bits = c.arg32(8);
  if (w < 0 || h < 0) return c.fail(kErrorInvalidParameter);
  uint32_t bpp = 32;
  if (dc && dc->memory) if (Bitmap* cur = find_bitmap(dc->bitmap)) bpp = cur->bpp;
  Bitmap* b = (w == 0 || h == 0) ? new_bitmap(c.pid(), 1, 1, 1) : new_bitmap(c.pid(), w, h, bpp);
  if (!b) return c.fail(kErrorInvalidParameter);
  if ((init & 4) != 0 && c.arg(5) != 0 && c.arg(4) != 0) {
    DibInfo info(*alloc());
    ByteBuffer bits(*alloc());
    if (!read_bitmap_info(c, c.arg(5), usage, max_info, &info, dc) || !read_bits(c, c.arg(4), info, max_bits, uint32_t(info.height), &bits) || !bitmap_from_dib(c, b, info, bits.view())) {
      destroy_object(b->handle);
      return c.fail(kErrorInvalidParameter);
    }
  }
  return b->handle;
}

uint64_t NtGdiGetDIBitsInternal(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  Bitmap* bm = static_cast<Bitmap*>(find_object(c.arg32(1), kObjBitmap));
  if (!dc || !bm) return c.fail(kErrorInvalidHandle);
  const uint32_t start = c.arg32(2), lines = c.arg32(3), usage = c.arg32(6), max_bits = c.arg32(7), max_info = c.arg32(8);
  const uint64_t bits = c.arg(4), info_at = c.arg(5);
  uint8_t hdr[40] = {};
  uint32_t hsize = 0;
  if (info_at == 0 || !c.read_u32(info_at, &hsize) || hsize < 40 || hsize > 124 || !c.read(info_at, ByteView(hdr, 40))) return c.fail(kErrorInvalidParameter);
  if (max_info != 0 && max_info < hsize) return c.fail(kErrorInvalidParameter);
  uint32_t req_bpp = le16(hdr + 14), req_comp = le32(hdr + 16);
  int32_t req_h = int32_t(le32(hdr + 8));
  dib_sync_in(*bm);
  Surface src = surface_of(*bm);
  if (bits == 0) {      // only the header (and colour table) is wanted
    const uint32_t bpp = req_bpp == 0 ? (bm->bpp == 16 ? 16 : bm->bpp) : req_bpp;
    uint8_t out[40] = {};
    auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) out[o + i] = uint8_t(v >> (8 * i)); };
    p32(0, hsize); p32(4, uint32_t(bm->width)); p32(8, req_h < 0 ? uint32_t(-bm->height) : uint32_t(bm->height));
    out[12] = 1; out[14] = uint8_t(bpp); out[15] = uint8_t(bpp >> 8);
    p32(16, req_bpp == 0 ? 0 : req_comp);
    p32(20, uint32_t(dib_stride(uint32_t(bm->width), bpp) * uint32_t(bm->height)));
    if (!c.write(info_at + 4, ConstByteView(out + 4, 36))) return 0;
    if (req_bpp == 0 && bpp <= 8 && usage == 0) write_color_table(c, info_at, hsize, src.colors, 1u << bpp);
    return 1;
  }
  if (req_bpp != 1 && req_bpp != 4 && req_bpp != 8 && req_bpp != 16 && req_bpp != 24 && req_bpp != 32) return c.fail(kErrorInvalidParameter);
  if (req_comp > 3 || (req_comp == 3 && req_bpp != 16 && req_bpp != 32) || req_comp == 1 || req_comp == 2) return c.fail(kErrorInvalidParameter);
  if (start >= uint32_t(bm->height)) return 0;
  uint32_t n = lines;
  if (n > uint32_t(bm->height) - start) n = uint32_t(bm->height) - start;
  if (n == 0) return 0;
  const bool top_down = req_h < 0;
  const uint64_t stride = dib_stride(uint32_t(bm->width), req_bpp);
  const uint64_t total = stride * n;
  if (total > kMaxDibBytes || (max_bits != 0 && total > max_bits)) return c.fail(kErrorInvalidParameter);
  Decoded out(*alloc());
  if (!out.buf.put_zeros(total)) return c.fail(kErrorNotEnoughMemory);
  out.s.bytes = out.buf.writable_view();
  out.s.width = bm->width; out.s.height = int32_t(n); out.s.bpp = req_bpp; out.s.stride = uint32_t(stride); out.s.bottom_up = !top_down;
  if (req_bpp <= 8) {
    if (usage == 0 && req_bpp == bm->bpp && bm->bpp <= 8) { for (uint32_t col : src.colors) out.colors.push_back(col); }
    else if (usage == 0) {
      // the caller's table when it set one, else the default palette of that depth
      List<uint32_t> d(*alloc());
      fill_default_colors(req_bpp, &d);
      for (uint32_t i = 0; i < d.count(); ++i) {
        uint32_t q = 0;
        if (c.read_u32(info_at + hsize + uint64_t{i} * 4, &q) && q != 0) out.colors.push_back(make_rgb((q >> 16) & 0xFF, (q >> 8) & 0xFF, q & 0xFF));
        else out.colors.push_back(d[i]);
      }
    } else {
      Palette* pal = find_palette(dc->s.palette);
      for (uint32_t i = 0; i < (1u << req_bpp); ++i) out.colors.push_back(pal && i < pal->entries.count() ? pal->entries[i] & 0xFFFFFF : 0);
    }
    out.s.colors = ArrayView<const uint32_t>(out.colors.view());
  } else if (req_comp == 3 && c.arg(5) != 0) {
    for (uint32_t i = 0; i < 3; ++i) { uint32_t m = 0; c.read_u32(info_at + hsize + uint64_t{i} * 4, &m); out.s.masks[i] = m; }
  }
  for (uint32_t row = 0; row < n; ++row) {
    // scan lines count from the bottom of the bitmap (top-down requests count from the top)
    const int32_t src_y = top_down ? int32_t(start + row) : bm->height - 1 - int32_t(start + row);
    for (int32_t x = 0; x < bm->width; ++x) out.s.put(x, top_down ? int32_t(row) : int32_t(n - 1 - row), src.get(x, src_y));
  }
  if (!c.write(bits, out.buf.view())) return 0;
  if (req_bpp <= 8 && usage == 0) write_color_table(c, info_at, hsize, out.s.colors, 1u << req_bpp);
  return n;
}

uint64_t NtGdiSetDIBitsToDeviceInternal(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const int32_t xd = c.argi(1), yd = c.argi(2), cx = int32_t(c.arg32(3)), cy = int32_t(c.arg32(4)), xs = c.argi(5), ys = c.argi(6);
  const uint32_t start = c.arg32(7), lines = c.arg32(8), usage = c.arg32(11), max_bits = c.arg32(12), max_info = c.arg32(13);
  if (cx <= 0 || cy <= 0 || lines == 0) return 0;
  DibInfo info(*alloc());
  if (!read_bitmap_info(c, c.arg(10), usage, max_info, &info, dc)) return c.fail(kErrorInvalidParameter);
  const int32_t full_h = info.height;
  info.height = int32_t(lines > uint32_t(info.height) ? uint32_t(info.height) : lines);
  if (info.compression == 1 || info.compression == 2) info.height = full_h;
  ByteBuffer bits(*alloc());
  if (c.arg(9) == 0 || !read_bits(c, c.arg(9), info, max_bits, uint32_t(info.height), &bits)) return c.fail(kErrorInvalidParameter);
  Decoded d(*alloc());
  if (!decode(*alloc(), info, bits.view(), &d)) return c.fail(kErrorInvalidParameter);
  Painter p(*dc);
  if (!p.ok()) return lines;
  const Point dest = lp_to_dp(*dc, Point{xd, yd});
  for (int32_t dy = 0; dy < cy; ++dy) {
    // destination row dy shows scan line ys + cy - 1 - dy (bottom-up DIB) or ys + dy (top-down)
    const int64_t scan = info.bottom_up ? int64_t{ys} + cy - 1 - dy : int64_t{ys} + dy;
    if (scan < int64_t{start} || scan >= int64_t{start} + info.height) continue;
    const int64_t row_in_buf = info.bottom_up ? scan - start : scan - start;
    const int32_t sy = info.bottom_up ? info.height - 1 - int32_t(row_in_buf) : int32_t(row_in_buf);
    for (int32_t dx = 0; dx < cx; ++dx) {
      const int64_t sx = int64_t{xs} + dx;
      if (sx < 0 || sx >= info.width) continue;
      p.plot_raw(sat_add(dest.x, dx), sat_add(dest.y, dy), d.s.get(int32_t(sx), sy));
    }
  }
  return lines;
}

uint64_t NtGdiStretchDIBitsInternal(Call& c) {
  Dc* dc = dc_arg(c, c.arg32(0));
  if (!dc) return 0;
  const int32_t xd = c.argi(1), yd = c.argi(2), wd = c.argi(3), hd = c.argi(4), xs = c.argi(5), ys = c.argi(6), ws = c.argi(7), hs = c.argi(8);
  const uint32_t usage = c.arg32(11), rop = c.arg32(12), max_info = c.arg32(13), max_bits = c.arg32(14);
  if (ws == 0 || hs == 0 || wd == 0 || hd == 0) return 0;
  DibInfo info(*alloc());
  ByteBuffer bits(*alloc());
  if (!read_bitmap_info(c, c.arg(10), usage, max_info, &info, dc) || c.arg(9) == 0 || !read_bits(c, c.arg(9), info, max_bits, uint32_t(info.height), &bits)) return c.fail(kErrorInvalidParameter);
  Decoded d(*alloc());
  if (!decode(*alloc(), info, bits.view(), &d)) return c.fail(kErrorInvalidParameter);
  if (ws < 0 || hs < 0) return c.fail(kErrorInvalidParameter);
  const int32_t top = d.s.bottom_up ? info.height - ys - hs : ys;      // the source rectangle in top-down rows
  const Rect src{xs, top, sat_add(xs, ws), sat_add(top, hs)};
  const Point a = lp_to_dp(*dc, Point{xd, yd}), b = lp_to_dp(*dc, Point{sat_add(xd, wd), sat_add(yd, hd)});
  if (!stretch_surface(*dc, Rect{a.x, a.y, b.x, b.y}, d.s, src, rop, dc->s.text_color, dc->s.bk_color)) return 0;
  return uint32_t(hs);
}

uint64_t NtGdiCreateDIBBrush(Call& c) {
  const uint32_t usage = c.arg32(1), size = c.arg32(2);
  const bool is_8x8 = c.arg_bool(3);
  if (usage > 2 || size < 12) return c.fail(kErrorInvalidParameter);
  DibInfo info(*alloc());
  if (!read_bitmap_info(c, c.arg(0), usage, size, &info, nullptr)) return c.fail(kErrorInvalidParameter);
  ByteBuffer bits(*alloc());
  if (info.bits_offset >= size) return c.fail(kErrorInvalidParameter);
  const uint64_t avail = size - info.bits_offset;
  if (!bits.put_zeros(avail) || !c.read(c.arg(0) + info.bits_offset, bits.writable_view())) return c.fail(kErrorInvalidParameter);
  Decoded d(*alloc());
  if (!decode(*alloc(), info, bits.view(), &d)) return c.fail(kErrorInvalidParameter);
  Brush* b = make_object<Brush>(kObjBrush, c.pid());
  if (!b) return c.fail(kErrorNotEnoughMemory);
  b->style = is_8x8 ? 8 : 5;
  b->usage = usage;
  b->pw = info.width;
  b->ph = info.height;
  b->mono = false;
  for (int32_t y = 0; y < info.height; ++y) for (int32_t x = 0; x < info.width; ++x) if (!b->pattern.push_back(d.s.get(x, y))) { destroy_object(b->handle); return c.fail(kErrorNotEnoughMemory); }
  return b->handle;
}

}  // namespace

void register_dib_services() {
  GDI_REGISTER(CreateDIBSection, NtGdiCreateDIBSection);
  GDI_REGISTER(CreateDIBitmapInternal, NtGdiCreateDIBitmapInternal);
  GDI_REGISTER(GetDIBitsInternal, NtGdiGetDIBitsInternal);
  GDI_REGISTER(SetDIBitsToDeviceInternal, NtGdiSetDIBitsToDeviceInternal);
  GDI_REGISTER(StretchDIBitsInternal, NtGdiStretchDIBitsInternal);
  GDI_REGISTER(CreateDIBBrush, NtGdiCreateDIBBrush);
}

}  // namespace process::win32k::gdi
