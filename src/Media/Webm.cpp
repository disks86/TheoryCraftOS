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
#include "Media/Webm.hpp"
#include "Media/Ebml.hpp"
#include "Media/Zlib.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/StringView.hpp"

namespace media {

namespace {

constexpr uint64_t kMaxTable = 16u << 20;     // the most Info or Tracks may hold
constexpr uint64_t kMaxBlock = 256u << 20;
constexpr uint64_t kMaxFrame = 64u << 20;     // the most one frame may be once decompressed

struct Element {
  uint32_t id = 0;
  bool unknown = false;
  bool truncated = false;   // the element says it goes on past `limit` (a file that was cut short)
  uint64_t payload = 0, size = 0, end = 0;
};

StringView text(ConstByteView v) { return StringView(reinterpret_cast<const char*>(v.data()), v.size()); }

// The ticks of the timecode scale as microseconds; false if the product overflows.
bool ticks_to_us(int64_t ticks, uint64_t scale_ns, int64_t* us) {
  int64_t product;
  if (__builtin_mul_overflow(ticks, static_cast<int64_t>(scale_ns), &product)) return false;
  *us = product / 1000;
  return true;
}

WebmCodec codec_of(StringView id, WebmTrackKind kind) {
  if (kind == WebmTrackKind::Video) {
    if (id.equals("V_VP8")) return WebmCodec::Vp8;
    if (id.equals("V_VP9")) return WebmCodec::Vp9;
    if (id.equals("V_AV1")) return WebmCodec::Av1;
  } else if (kind == WebmTrackKind::Audio) {
    if (id.equals("A_OPUS")) return WebmCodec::Opus;
    if (id.equals("A_VORBIS")) return WebmCodec::Vorbis;
  }
  return WebmCodec::Unknown;
}

}  // namespace

bool WebmTrack::decodable() const {
  if (!enabled || encrypted || compression == WebmCompression::Unsupported) return false;
  return codec == WebmCodec::Vp8 || codec == WebmCodec::Vp9 || codec == WebmCodec::Av1 || codec == WebmCodec::Opus || codec == WebmCodec::Vorbis;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The scan

struct WebmFile::Scan {
  Scan(WebmFile& f, IByteSource& s) : file(f), source(s), cache(s, f.allocator_, 65536), body(f.allocator_), head(f.allocator_) {}

  WebmFile& file;
  IByteSource& source;
  CachedByteSource cache;
  ByteBuffer body, head;   // scratch: a table being parsed, a block's header
  uint64_t scale_ns = 1000000;
  double duration_ticks = -1;

  // The element header at `pos` inside [pos, limit). 1 done, 0 nothing (the end), -1 not an element.
  int header(uint64_t pos, uint64_t limit, Element* e) {
    if (pos >= limit) return 0;
    uint8_t raw[12];
    const uint64_t want = limit - pos < sizeof raw ? limit - pos : sizeof raw;
    const uint64_t got = cache.read(pos, ByteView(raw, want));
    uint32_t length;
    const int r = ebml::header(ConstByteView(raw, got), &e->id, &e->size, &e->unknown, &length);
    if (r != 1) return r == 0 ? 0 : -1;
    e->payload = pos + length;
    if (e->payload > limit) return -1;
    e->truncated = false;
    if (e->unknown) {
      e->end = limit;
    } else if (e->size > limit - e->payload) {
      e->end = limit;
      e->truncated = true;
    } else {
      e->end = e->payload + e->size;
    }
    return 1;
  }

  // The payload of a whole element into `body`.
  bool read_body(const Element& e) {
    body.clear();
    if (e.unknown || e.truncated || e.size > kMaxTable || !body.put_zeros(e.size)) return false;
    return cache.read(e.payload, body.writable_view()) == e.size;
  }

  // The children of the payload `data`, calling `visit(id, payload)`; false if they do not fit in it.
  template <class F>
  static bool children(ConstByteView data, F&& visit) {
    uint64_t at = 0;
    while (at < data.size()) {
      uint32_t id, length;
      uint64_t size;
      bool unknown;
      if (ebml::header(data.subview(at), &id, &size, &unknown, &length) != 1 || unknown || size > data.size() - at - length) return false;
      if (!visit(id, data.subview(at + length, size))) return false;
      at += length + size;
    }
    return true;
  }

  bool parse_info(const Element& e) {
    if (!read_body(e)) return false;
    const bool ok = children(body.view(), [&](uint32_t id, ConstByteView v) {
      if (id == ebml::kTimecodeScale) scale_ns = ebml::to_uint(v);
      else if (id == ebml::kDuration) duration_ticks = ebml::to_float(v);
      return true;
    });
    if (!ok || scale_ns == 0 || scale_ns > 1000000000u) {
      scale_ns = 1000000;
      return false;
    }
    return true;
  }

  bool parse_tracks(const Element& e) {
    if (!read_body(e)) return false;
    return children(body.view(), [&](uint32_t id, ConstByteView v) {
      if (id != ebml::kTrackEntry || file.count_ >= kMaxTracks) return true;
      UniquePointer<WebmTrack> track = UniquePointer<WebmTrack>::make(file.allocator_, file.allocator_);
      if (!track || !parse_entry(v, track.get())) return false;
      file.tracks_[file.count_++] = util::move(track);
      return true;
    });
  }

  bool parse_encodings(ConstByteView v, WebmTrack* t) {
    uint32_t frame_encodings = 0;
    const bool ok = children(v, [&](uint32_t id, ConstByteView enc) {
      if (id != ebml::kContentEncoding) return true;
      uint64_t scope = 1, type = 0, algo = 0;
      ConstByteView settings;
      bool encryption = false;
      const bool parsed = children(enc, [&](uint32_t cid, ConstByteView cv) {
        if (cid == ebml::kContentEncodingScope) scope = ebml::to_uint(cv);
        else if (cid == ebml::kContentEncodingType) type = ebml::to_uint(cv);
        else if (cid == ebml::kContentEncryption) encryption = true;
        else if (cid == ebml::kContentCompression) {
          return children(cv, [&](uint32_t zid, ConstByteView zv) {
            if (zid == ebml::kContentCompAlgo) algo = ebml::to_uint(zv);
            else if (zid == ebml::kContentCompSettings) settings = zv;
            return true;
          });
        }
        return true;
      });
      if (!parsed) return false;
      if (!(scope & 1)) return true;   // (private data only: the frames are plain)
      ++frame_encodings;
      if (type == 1 || encryption) {
        t->encrypted = true;
      } else if (algo == 3) {
        t->compression = WebmCompression::HeaderStripping;
        return t->strip.append(settings);
      } else if (algo == 0) {
        t->compression = WebmCompression::Zlib;
      } else {
        t->compression = WebmCompression::Unsupported;   // (bzlib, lzo)
      }
      return true;
    });
    if (frame_encodings > 1) t->compression = WebmCompression::Unsupported;   // (layers of encodings: one is all there is code for)
    return ok;
  }

  bool parse_entry(ConstByteView body, WebmTrack* t) {
    uint64_t type = 0;
    ConstByteView codec;
    const bool ok = children(body, [&](uint32_t id, ConstByteView v) {
      switch (id) {
        case ebml::kTrackNumber: t->number = ebml::to_uint(v); break;
        case ebml::kTrackType: type = ebml::to_uint(v); break;
        case ebml::kFlagEnabled: t->enabled = ebml::to_uint(v) != 0; break;
        case ebml::kFlagDefault: t->is_default = ebml::to_uint(v) != 0; break;
        case ebml::kCodecId: codec = v; break;
        case ebml::kCodecPrivate: return t->codec_private.append(v);
        case ebml::kDefaultDuration: {
          const uint64_t us = ebml::to_uint(v) / 1000;
          t->default_duration_us = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
          break;
        }
        case ebml::kCodecDelay: t->codec_delay_ns = static_cast<int64_t>(ebml::to_uint(v)); break;
        case ebml::kSeekPreRoll: t->seek_pre_roll_ns = static_cast<int64_t>(ebml::to_uint(v)); break;
        case ebml::kContentEncodings: return parse_encodings(v, t);
        case ebml::kVideo:
          return children(v, [&](uint32_t cid, ConstByteView cv) {
            const uint64_t n = ebml::to_uint(cv);
            const uint32_t n32 = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(n);
            if (cid == ebml::kPixelWidth) t->width = n32;
            else if (cid == ebml::kPixelHeight) t->height = n32;
            else if (cid == ebml::kDisplayWidth) t->display_width = n32;
            else if (cid == ebml::kDisplayHeight) t->display_height = n32;
            return true;
          });
        case ebml::kAudio: {
          double rate = 8000, output = 0;
          const bool good = children(v, [&](uint32_t cid, ConstByteView cv) {
            if (cid == ebml::kChannels) t->channels = static_cast<uint32_t>(ebml::to_uint(cv));
            else if (cid == ebml::kBitDepth) t->bit_depth = static_cast<uint32_t>(ebml::to_uint(cv));
            else if (cid == ebml::kSamplingFrequency) rate = ebml::to_float(cv);
            else if (cid == ebml::kOutputSamplingFrequency) output = ebml::to_float(cv);
            return true;
          });
          const double r = output > 0 ? output : rate;
          t->sample_rate = r > 0 && r < 4e6 ? static_cast<uint32_t>(r) : 0;
          return good;
        }
        default: break;
      }
      return true;
    });
    if (!ok || t->number == 0) return false;
    t->kind = type == 1 ? WebmTrackKind::Video : type == 2 ? WebmTrackKind::Audio : WebmTrackKind::Other;
    t->codec = codec_of(text(codec), t->kind);
    if (t->kind == WebmTrackKind::Video) {
      if (t->display_width == 0 || t->display_height == 0) {
        t->display_width = t->width;
        t->display_height = t->height;
      }
    }
    if (t->kind == WebmTrackKind::Audio && t->channels == 0) t->channels = 1;
    return true;
  }

  // ---- blocks ----

  enum class HeadResult { Ok, NeedMore, Bad };
  struct BlockInfo {
    uint64_t track = 0;
    int16_t relative = 0;
    uint8_t flags = 0;
    uint32_t count = 0;
    uint64_t header_length = 0;
    uint64_t sizes[256];
  };

  static HeadResult parse_block_header(ConstByteView data, uint64_t block_size, BlockInfo* b) {
    const HeadResult short_read = data.size() < block_size ? HeadResult::NeedMore : HeadResult::Bad;
    ByteReader r(data);
    if (!ebml::vint(r, &b->track)) return short_read;
    b->relative = static_cast<int16_t>(r.be16());
    b->flags = r.u8();
    if (!r.ok()) return short_read;
    const uint32_t lacing = (b->flags >> 1) & 3;
    if (lacing == 0) {
      b->count = 1;
      b->header_length = r.position();
      b->sizes[0] = block_size - b->header_length;
      return HeadResult::Ok;
    }
    b->count = static_cast<uint32_t>(r.u8()) + 1;
    if (!r.ok()) return short_read;
    uint64_t sizes_total = 0;   // of all the frames but the last
    if (lacing == 1) {   // Xiph: sizes as runs of 255 and a smaller byte
      for (uint32_t i = 0; i + 1 < b->count; ++i) {
        uint64_t total = 0;
        uint8_t byte;
        do {
          byte = r.u8();
          total += byte;
        } while (byte == 255 && r.ok());
        if (!r.ok()) return short_read;
        b->sizes[i] = total;
      }
    } else if (lacing == 3) {   // EBML: the first size, then signed differences
      uint32_t n;
      if (!ebml::vint(r, &b->sizes[0], &n)) return short_read;
      for (uint32_t i = 1; i + 1 < b->count; ++i) {
        uint64_t raw;
        if (!ebml::vint(r, &raw, &n)) return short_read;
        const int64_t bias = (int64_t{1} << (7 * n - 1)) - 1;
        const int64_t size = static_cast<int64_t>(b->sizes[i - 1]) + (static_cast<int64_t>(raw) - bias);
        if (size < 0) return HeadResult::Bad;
        b->sizes[i] = static_cast<uint64_t>(size);
      }
    }
    b->header_length = r.position();
    if (b->header_length > block_size) return HeadResult::Bad;
    const uint64_t rest = block_size - b->header_length;
    if (lacing == 2) {   // fixed size
      if (rest % b->count != 0) return HeadResult::Bad;
      for (uint32_t i = 0; i + 1 < b->count; ++i) b->sizes[i] = rest / b->count;
    }
    for (uint32_t i = 0; i + 1 < b->count; ++i) {
      if (b->sizes[i] > rest - sizes_total) return HeadResult::Bad;
      sizes_total += b->sizes[i];
    }
    b->sizes[b->count - 1] = rest - sizes_total;
    return HeadResult::Ok;
  }

  WebmTrack* track_by_number(uint64_t number) {
    for (uint32_t i = 0; i < file.count_; ++i)
      if (file.tracks_[i]->number == number) return file.tracks_[i].get();
    return nullptr;
  }

  // One block (its payload at [pos, pos + size)): the frames in it become samples. False only when memory ran out.
  bool add_block(uint64_t pos, uint64_t size, int64_t cluster_ticks, bool simple, bool group_key, int64_t duration_ticks, int64_t discard_ns) {
    if (size == 0 || size > kMaxBlock) return true;
    BlockInfo info;
    uint64_t want = size < 128 ? size : 128;
    for (;;) {
      head.clear();
      if (!head.put_zeros(want)) return false;
      const uint64_t got = cache.read(pos, head.writable_view());
      if (got != want) return true;   // (the file ends inside the block)
      const HeadResult r = parse_block_header(head.view(), size, &info);
      if (r == HeadResult::Ok) break;
      if (r == HeadResult::Bad || want >= size) return true;
      want = want * 8 < size ? want * 8 : size;
    }
    WebmTrack* t = track_by_number(info.track);
    if (!t || t->kind == WebmTrackKind::Other) return true;
    int64_t pts_us;
    if (!ticks_to_us(cluster_ticks + info.relative, scale_ns, &pts_us)) return true;
    int64_t spacing_us = t->default_duration_us;
    int64_t block_us = 0;
    if (duration_ticks > 0 && ticks_to_us(duration_ticks, scale_ns, &block_us) && block_us > 0) spacing_us = block_us / info.count;
    const bool key_block = t->kind == WebmTrackKind::Audio || (simple ? (info.flags & 0x80) != 0 : group_key);
    if (t->kind == WebmTrackKind::Audio) t->last_discard_ns = discard_ns;
    uint64_t offset = pos + info.header_length;
    for (uint32_t i = 0; i < info.count; ++i) {
      const uint64_t frame = info.sizes[i];
      if (frame == 0 || frame > 0xFFFFFFFFu) {
        offset += frame;
        continue;
      }
      if (t->samples.count() >= kMaxSamplesPerTrack) return true;
      WebmSample s;
      s.offset = offset;
      s.size = static_cast<uint32_t>(frame);
      s.pts_us = pts_us + static_cast<int64_t>(i) * spacing_us;
      s.duration_us = spacing_us > 0 && spacing_us < 0xFFFFFFFFll ? static_cast<uint32_t>(spacing_us) : 0;
      s.keyframe = key_block && (t->kind == WebmTrackKind::Audio || i == 0);
      if (!t->samples.push_back(s)) return false;
      offset += frame;
    }
    return true;
  }

  bool group(const Element& e, int64_t cluster_ticks) {
    // The children of a block group are small except for the block itself: look at the headers, keep the position of the block.
    uint64_t block_at = 0, block_size = 0, pos = e.payload;
    bool have_block = false, reference = false;
    int64_t duration = -1, discard = 0;
    while (pos < e.end) {
      Element c;
      if (header(pos, e.end, &c) != 1 || c.unknown || c.truncated) break;
      if (c.id == ebml::kBlock) {
        have_block = true;
        block_at = c.payload;
        block_size = c.size;
      } else if (c.id == ebml::kBlockDuration || c.id == ebml::kDiscardPadding) {
        uint8_t raw[8];
        if (c.size > sizeof raw || cache.read(c.payload, ByteView(raw, c.size)) != c.size) break;
        if (c.id == ebml::kBlockDuration) duration = static_cast<int64_t>(ebml::to_uint(ConstByteView(raw, c.size)));
        else discard = ebml::to_int(ConstByteView(raw, c.size));
      } else if (c.id == ebml::kReferenceBlock) {
        reference = true;
      }
      pos = c.end;
    }
    if (!have_block) return true;
    return add_block(block_at, block_size, cluster_ticks, false, !reference, duration, discard);
  }

  // The children of a cluster, starting at `pos`; returns where the next element after the cluster begins.
  bool cluster(const Element& e, uint64_t* next) {
    int64_t ticks = 0;
    uint64_t pos = e.payload;
    while (pos < e.end) {
      Element c;
      const int r = header(pos, e.end, &c);
      if (r != 1) {   // damaged: a cluster of known size is let go, one of unknown size ends the scan
        *next = e.unknown ? source.size() : e.end;
        return true;
      }
      if (e.unknown && ebml::is_segment_child(c.id)) {
        *next = pos;
        return true;
      }
      if (c.id == ebml::kTimecode) {
        uint8_t raw[8];
        if (!c.unknown && !c.truncated && c.size <= sizeof raw && cache.read(c.payload, ByteView(raw, c.size)) == c.size) {
          const uint64_t value = ebml::to_uint(ConstByteView(raw, c.size));
          ticks = value < (uint64_t{1} << 50) ? static_cast<int64_t>(value) : 0;   // (a time beyond what the scale can hold is no time)
        }
      } else if (c.id == ebml::kSimpleBlock) {
        if (c.truncated) break;
        if (!c.unknown && !add_block(c.payload, c.size, ticks, true, false, -1, 0)) return false;
      } else if (c.id == ebml::kBlockGroup) {
        if (c.truncated) break;
        if (!c.unknown && !group(c, ticks)) return false;
      }
      if (c.unknown) break;   // (nothing but a cluster and the segment may be of unknown size)
      pos = c.end;
    }
    *next = e.end;
    return true;
  }

  // ---- the file ----

  bool run() {
    Element e;
    if (header(0, source.size(), &e) != 1 || e.id != ebml::kEbml || e.unknown || e.truncated) return false;
    if (!read_body(e)) return false;
    ConstByteView doc;
    children(body.view(), [&](uint32_t id, ConstByteView v) {
      if (id == ebml::kDocType) doc = v;
      return true;
    });
    if (doc.empty()) return false;
    const StringView doc_type = text(doc);
    if (!doc_type.equals("webm") && !doc_type.equals("matroska")) return false;
    file.webm_ = doc_type.equals("webm");
    Element segment;
    if (header(e.end, source.size(), &segment) != 1 || segment.id != ebml::kSegment) return false;
    uint64_t pos = segment.payload;
    const uint64_t end = segment.end;
    while (pos < end) {
      Element c;
      if (header(pos, end, &c) != 1) break;
      if (c.id == ebml::kCluster) {
        uint64_t next;
        if (!cluster(c, &next)) return false;
        pos = next > pos ? next : c.end;
        continue;
      }
      if (c.id == ebml::kInfo) {
        if (!parse_info(c) && file.count_ == 0) return false;
      } else if (c.id == ebml::kTracks && file.count_ == 0) {
        if (!parse_tracks(c)) return false;
      }
      if (c.unknown) break;
      pos = c.end;
    }
    return finish();
  }

  bool finish() {
    int64_t content_end = 0;
    bool any = false;
    for (uint32_t i = 0; i < file.count_; ++i) {
      WebmTrack& t = *file.tracks_[i];
      const uint64_t n = t.samples.count();
      if (n == 0) continue;
      const uint32_t fallback = t.default_duration_us ? t.default_duration_us : t.kind == WebmTrackKind::Audio ? 20000 : 33333;
      for (uint64_t k = 0; k < n; ++k) {
        WebmSample& s = t.samples[k];
        if (s.duration_us == 0) {
          // no duration in the file: the time to the next frame
          const int64_t gap = k + 1 < n ? t.samples[k + 1].pts_us - s.pts_us : 0;
          s.duration_us = gap > 0 && gap < 0xFFFFFFFFll ? static_cast<uint32_t>(gap) : (k > 0 ? t.samples[k - 1].duration_us : fallback);
          if (s.duration_us == 0) s.duration_us = fallback;
        }
      }
      if (t.decodable()) {
        const WebmSample& last = t.samples[n - 1];
        const int64_t e = last.pts_us + last.duration_us;
        if (e > content_end) content_end = e;
        any = true;
      }
    }
    if (!any) return false;
    int64_t duration = 0;
    if (duration_ticks > 0 && (duration_ticks >= 9e15 || !ticks_to_us(static_cast<int64_t>(duration_ticks), scale_ns, &duration))) duration = 0;
    file.duration_us_ = duration > content_end ? duration : content_end;
    return true;
  }
};

bool WebmFile::open(IByteSource& source) {
  clear();
  Scan scan(*this, source);
  if (!scan.run()) {
    clear();
    return false;
  }
  return true;
}

void WebmFile::clear() {
  for (UniquePointer<WebmTrack>& t : tracks_) t.reset();
  count_ = 0;
  duration_us_ = 0;
  webm_ = false;
}

const WebmTrack* WebmFile::find(WebmTrackKind kind) const {
  const WebmTrack* first = nullptr;
  for (uint32_t i = 0; i < count_; ++i) {
    const WebmTrack& t = *tracks_[i];
    if (t.kind != kind || !t.decodable() || t.samples.count() == 0) continue;
    if (t.is_default) return &t;
    if (!first) first = &t;
  }
  return first;
}

bool WebmFile::read_sample(IByteSource& source, const WebmTrack& track, uint32_t index, ByteBuffer* out) const {
  if (index >= track.samples.count()) return false;
  const WebmSample& s = track.samples[index];
  if (s.offset > source.size() || s.size > source.size() - s.offset) return false;
  const uint64_t start = out->size();
  if (track.compression == WebmCompression::Zlib) {
    ByteBuffer packed(allocator_);
    if (!packed.put_zeros(s.size) || source.read(s.offset, packed.writable_view()) != s.size) return false;
    if (!inflate(packed.view(), out, kMaxFrame)) {
      out->truncate(start);
      return false;
    }
    return true;
  }
  if (track.compression == WebmCompression::Unsupported) return false;
  if (track.compression == WebmCompression::HeaderStripping && !out->append(track.strip.view())) return false;
  const uint64_t at = out->size();
  if (!out->put_zeros(s.size)) {
    out->truncate(start);
    return false;
  }
  if (source.read(s.offset, out->writable_view().subview(at, s.size)) != s.size) {
    out->truncate(start);
    return false;
  }
  return true;
}

}  // namespace media
