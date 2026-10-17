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

#include "Media/Mp4.hpp"

namespace media {

namespace {
uint32_t be32(ConstByteView d, uint64_t at) { return (uint32_t{d[at]} << 24) | (uint32_t{d[at + 1]} << 16) | (uint32_t{d[at + 2]} << 8) | d[at + 3]; }
uint64_t be64(ConstByteView d, uint64_t at) { return (uint64_t{be32(d, at)} << 32) | be32(d, at + 4); }
uint32_t be16(ConstByteView d, uint64_t at) { return (uint32_t{d[at]} << 8) | d[at + 1]; }
constexpr uint32_t fourcc(const char* s) { return (uint32_t{static_cast<uint8_t>(s[0])} << 24) | (uint32_t{static_cast<uint8_t>(s[1])} << 16) | (uint32_t{static_cast<uint8_t>(s[2])} << 8) | static_cast<uint8_t>(s[3]); }

struct Box {
  uint32_t type;
  uint64_t body, end;  // payload range
};

// The box at `at` inside [at, limit): false if there is none that fits.
bool read_box(ConstByteView d, uint64_t at, uint64_t limit, Box* box) {
  if (at + 8 > limit) return false;
  uint64_t size = be32(d, at);
  uint64_t header = 8;
  box->type = be32(d, at + 4);
  if (size == 1) {
    if (at + 16 > limit) return false;
    size = be64(d, at + 8);
    header = 16;
  } else if (size == 0) {
    size = limit - at;
  }
  if (size < header || at + size > limit) return false;
  box->body = at + header;
  box->end = at + size;
  return true;
}

// An MPEG-4 descriptor length: seven bits per byte, the top bit says another byte follows.
bool read_descriptor_size(ConstByteView d, uint64_t* at, uint64_t end, uint32_t* size) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    if (*at >= end) return false;
    const uint8_t b = d[(*at)++];
    v = (v << 7) | (b & 0x7F);
    if (!(b & 0x80)) {
      *size = v;
      return true;
    }
  }
  return false;
}

// esds: ES_Descriptor (0x03) holding DecoderConfigDescriptor (0x04) holding DecoderSpecificInfo (0x05).
void parse_esds(ConstByteView d, uint64_t at, uint64_t end, Mp4Track* t) {
  at += 4;  // version and flags
  while (at < end) {
    const uint8_t tag = d[at++];
    uint32_t size;
    if (!read_descriptor_size(d, &at, end, &size) || at + size > end) return;
    if (tag == 0x03) {
      if (size < 3) return;
      const uint8_t flags = d[at + 2];
      uint64_t p = at + 3;
      if (flags & 0x80) p += 2;
      if (flags & 0x40 && p < end) p += 1 + d[p];
      if (flags & 0x20) p += 2;
      at = p;  // descend: the decoder config follows inside
      continue;
    }
    if (tag == 0x04) {
      if (size < 13) return;
      t->object_type = d[at];
      at += 13;  // then the specific info inside
      continue;
    }
    if (tag == 0x05) {
      t->config = d.subview(at, size);
      return;
    }
    at += size;
  }
}

template <class T>
bool allocate(allocators::IAllocator& a, ArrayView<T>* view, uint64_t count) {
  *view = allocate_view<T>(a, count ? count : 1);
  if (view->empty()) return false;
  view->fill(T());
  return true;
}

bool parse_stbl(allocators::IAllocator& allocator, ConstByteView d, uint64_t start, uint64_t end, Mp4Track* t) {
  uint64_t stsz = 0, stsc = 0, stco = 0, co64 = 0, stts = 0, ctts = 0, stss = 0;
  Box b;
  for (uint64_t at = start; read_box(d, at, end, &b); at = b.end) {
    if (b.type == fourcc("stsd")) {
      if (b.body + 16 > b.end) return false;
      Box entry;
      if (!read_box(d, b.body + 8, b.end, &entry)) return false;
      t->codec = entry.type;
      if (t->kind == Mp4TrackKind::Audio && entry.body + 28 <= entry.end) {
        const uint32_t version = be16(d, entry.body + 8);
        t->channels = be16(d, entry.body + 16);
        t->sample_rate = be32(d, entry.body + 24) >> 16;
        uint64_t children = entry.body + 28;
        if (version == 1) children += 16;
        else if (version == 2) children += 36;
        Box child;
        for (uint64_t c = children; read_box(d, c, entry.end, &child); c = child.end) {
          if (child.type == fourcc("esds")) {
            parse_esds(d, child.body, child.end, t);
          } else if (child.type == fourcc("wave")) {  // QuickTime wraps the codec's boxes in a 'wave' box
            Box inner;
            for (uint64_t w = child.body; read_box(d, w, child.end, &inner); w = inner.end)
              if (inner.type == fourcc("esds")) parse_esds(d, inner.body, inner.end, t);
          }
        }
      } else if (t->kind == Mp4TrackKind::Video && entry.body + 78 <= entry.end) {
        t->width = be16(d, entry.body + 24);
        t->height = be16(d, entry.body + 26);
        Box child;
        for (uint64_t c = entry.body + 78; read_box(d, c, entry.end, &child); c = child.end)
          if (child.type == fourcc("avcC") || child.type == fourcc("hvcC")) t->config = d.subview(child.body, child.end - child.body);
      }
    } else if (b.type == fourcc("stts")) stts = b.body;
    else if (b.type == fourcc("ctts")) ctts = b.body;
    else if (b.type == fourcc("stss")) stss = b.body;
    else if (b.type == fourcc("stsz")) stsz = b.body;
    else if (b.type == fourcc("stsc")) stsc = b.body;
    else if (b.type == fourcc("stco")) stco = b.body;
    else if (b.type == fourcc("co64")) co64 = b.body;
  }
  if (!stsz || !stsc || (!stco && !co64) || !stts) return false;
  const uint32_t fixed_size = be32(d, stsz + 4), count = be32(d, stsz + 8);
  if (count == 0 || count > (1u << 24)) return false;
  if (fixed_size == 0 && stsz + 12 + uint64_t{count} * 4 > d.size()) return false;
  t->sample_count = count;
  if (!allocate(allocator, &t->offsets, count) || !allocate(allocator, &t->sizes, count) || !allocate(allocator, &t->dts, count) ||
      !allocate(allocator, &t->sync, count))
    return false;
  for (uint32_t i = 0; i < count; ++i) t->sizes[i] = fixed_size ? fixed_size : be32(d, stsz + 12 + uint64_t{i} * 4);
  // Offsets: chunks hold runs of samples.
  const uint32_t chunks = be32(d, stco ? stco + 4 : co64 + 4), runs = be32(d, stsc + 4);
  if (stsc + 8 + uint64_t{runs} * 12 > d.size() || (stco ? stco : co64) + 8 + uint64_t{chunks} * (stco ? 4 : 8) > d.size()) return false;
  uint32_t sample = 0;
  for (uint32_t run = 0; run < runs && sample < count; ++run) {
    const uint32_t first = be32(d, stsc + 8 + run * 12), per = be32(d, stsc + 12 + run * 12);
    const uint32_t last = run + 1 < runs ? be32(d, stsc + 8 + (run + 1) * 12) : chunks + 1;
    for (uint32_t chunk = first; chunk < last && chunk <= chunks && sample < count; ++chunk) {
      uint64_t offset = stco ? be32(d, stco + 8 + uint64_t{chunk - 1} * 4) : be64(d, co64 + 8 + uint64_t{chunk - 1} * 8);
      for (uint32_t i = 0; i < per && sample < count; ++i, ++sample) {
        t->offsets[sample] = offset;
        offset += t->sizes[sample];
      }
    }
  }
  if (sample != count) return false;
  // Decode times.
  const uint32_t entries = be32(d, stts + 4);
  if (stts + 8 + uint64_t{entries} * 8 > d.size()) return false;
  uint32_t i = 0;
  int64_t time = 0;
  for (uint32_t e = 0; e < entries && i < count; ++e) {
    const uint32_t n = be32(d, stts + 8 + e * 8), delta = be32(d, stts + 12 + e * 8);
    for (uint32_t k = 0; k < n && i < count; ++k, ++i) {
      t->dts[i] = time;
      time += delta;
    }
  }
  for (; i < count; ++i) t->dts[i] = time;
  if (ctts) {
    const uint32_t n = be32(d, ctts + 4);
    if (ctts + 8 + uint64_t{n} * 8 > d.size() || !allocate(allocator, &t->cts_offsets, count)) return false;
    uint32_t at = 0;
    for (uint32_t e = 0; e < n && at < count; ++e) {
      const uint32_t runlen = be32(d, ctts + 8 + e * 8);
      const int32_t off = static_cast<int32_t>(be32(d, ctts + 12 + e * 8));
      for (uint32_t k = 0; k < runlen && at < count; ++k, ++at) t->cts_offsets[at] = off;
    }
  }
  if (stss) {
    const uint32_t n = be32(d, stss + 4);
    if (stss + 8 + uint64_t{n} * 4 > d.size()) return false;
    for (uint32_t e = 0; e < n; ++e) {
      const uint32_t s = be32(d, stss + 8 + e * 4);
      if (s >= 1 && s <= count) t->sync[s - 1] = 1;
    }
  } else {
    t->sync.fill(1);
  }
  return true;
}

bool parse_trak(allocators::IAllocator& allocator, ConstByteView d, uint64_t start, uint64_t end, uint32_t movie_scale, Mp4Track* t) {
  Box b;
  uint32_t handler = 0;
  uint64_t stbl_start = 0, stbl_end = 0;
  int64_t media_time = 0, segment = -1;
  for (uint64_t at = start; read_box(d, at, end, &b); at = b.end) {
    if (b.type == fourcc("tkhd")) {
      const uint32_t version = d[b.body];
      t->id = be32(d, b.body + (version == 1 ? 20 : 12));
    } else if (b.type == fourcc("edts")) {
      Box e;
      for (uint64_t p = b.body; read_box(d, p, b.end, &e); p = e.end) {
        if (e.type != fourcc("elst") || e.body + 8 > e.end) continue;
        const uint32_t version = d[e.body], n = be32(d, e.body + 4);
        if (n == 0) continue;
        // The first entry that plays media (media_time >= 0) gives the start offset; an empty edit (-1) first is a delay.
        uint64_t q = e.body + 8;
        const uint64_t entry_size = version == 1 ? 20 : 12;
        for (uint32_t k = 0; k < n && q + entry_size <= e.end; ++k, q += entry_size) {
          const int64_t mt = version == 1 ? static_cast<int64_t>(be64(d, q + 8)) : static_cast<int32_t>(be32(d, q + 4));
          if (mt >= 0) {
            media_time = mt;
            segment = version == 1 ? static_cast<int64_t>(be64(d, q)) : be32(d, q);  // in movie timescale units
            break;
          }
        }
      }
    } else if (b.type == fourcc("mdia")) {
      Box m;
      for (uint64_t p = b.body; read_box(d, p, b.end, &m); p = m.end) {
        if (m.type == fourcc("mdhd")) {
          const uint32_t version = d[m.body];
          if (version == 1) { t->timescale = be32(d, m.body + 20); t->duration = be64(d, m.body + 24); }
          else { t->timescale = be32(d, m.body + 12); t->duration = be32(d, m.body + 16); }
        } else if (m.type == fourcc("hdlr")) {
          handler = be32(d, m.body + 8);
        } else if (m.type == fourcc("minf")) {
          Box n;
          for (uint64_t q = m.body; read_box(d, q, m.end, &n); q = n.end)
            if (n.type == fourcc("stbl")) { stbl_start = n.body; stbl_end = n.end; }
        }
      }
    }
  }
  t->kind = handler == fourcc("soun") ? Mp4TrackKind::Audio : handler == fourcc("vide") ? Mp4TrackKind::Video : Mp4TrackKind::Other;
  if (t->kind == Mp4TrackKind::Other || !stbl_start || t->timescale == 0) return false;
  if (!parse_stbl(allocator, d, stbl_start, stbl_end, t)) return false;
  t->skip = media_time;
  t->play_duration = segment < 0 || movie_scale == 0 ? -1 : segment * t->timescale / movie_scale;  // (the edit list counts in movie units)
  return true;
}
}  // namespace

bool looks_like_mp4(ConstByteView d) {
  if (d.size() < 12) return false;
  const uint32_t type = be32(d, 4);
  return type == fourcc("ftyp") || type == fourcc("moov") || type == fourcc("mdat") || type == fourcc("wide") || type == fourcc("free") || type == fourcc("skip");
}

void Mp4File::clear() {
  for (uint32_t i = 0; i < count_; ++i) {
    release_view(allocator_, tracks_[i].offsets);
    release_view(allocator_, tracks_[i].sizes);
    release_view(allocator_, tracks_[i].dts);
    release_view(allocator_, tracks_[i].cts_offsets);
    release_view(allocator_, tracks_[i].sync);
    tracks_[i] = Mp4Track();
  }
  count_ = 0;
}

bool Mp4File::open(ConstByteView file) {
  clear();
  file_ = file;
  if (!looks_like_mp4(file)) return false;
  Box top;
  for (uint64_t at = 0; read_box(file, at, file.size(), &top); at = top.end) {
    if (top.type != fourcc("moov")) continue;
    Box b;
    uint32_t movie_scale = 0;
    for (uint64_t p = top.body; read_box(file, p, top.end, &b); p = b.end)
      if (b.type == fourcc("mvhd") && b.body + 24 <= b.end) movie_scale = be32(file, b.body + (file[b.body] == 1 ? 20 : 12));
    for (uint64_t p = top.body; read_box(file, p, top.end, &b); p = b.end) {
      if (b.type != fourcc("trak") || count_ >= kMaxTracks) continue;
      Mp4Track& t = tracks_[count_];
      if (parse_trak(allocator_, file, b.body, b.end, movie_scale, &t)) {
        ++count_;
      } else {
        release_view(allocator_, t.offsets);
        release_view(allocator_, t.sizes);
        release_view(allocator_, t.dts);
        release_view(allocator_, t.cts_offsets);
        release_view(allocator_, t.sync);
        t = Mp4Track();
      }
    }
  }
  if (count_ == 0) return false;
  return true;
}

const Mp4Track* Mp4File::find(Mp4TrackKind kind) const {
  for (uint32_t i = 0; i < count_; ++i)
    if (tracks_[i].kind == kind) return &tracks_[i];
  return nullptr;
}

ConstByteView Mp4File::sample(const Mp4Track& t, uint32_t index) const {
  if (index >= t.sample_count) return ConstByteView();
  const uint64_t off = t.offsets[index], size = t.sizes[index];
  if (off > file_.size() || size > file_.size() - off) return ConstByteView();
  return file_.subview(off, size);
}

}  // namespace media
