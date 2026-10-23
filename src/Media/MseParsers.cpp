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

// The byte stream parsers of the Media Source Extensions (see Mse.hpp): fragmented MP4, MPEG audio and ADTS.
#include "Media/Mse.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr uint32_t fourcc(const char* s) { return (uint32_t{static_cast<uint8_t>(s[0])} << 24) | (uint32_t{static_cast<uint8_t>(s[1])} << 16) | (uint32_t{static_cast<uint8_t>(s[2])} << 8) | static_cast<uint8_t>(s[3]); }

uint64_t read64(ByteReader& r) {
  const uint64_t high = r.be32();
  return (high << 32) | r.be32();
}

// ticks of 1/timescale seconds as microseconds (rounded down; negative values mirror the positive ones)
int64_t ticks_to_us(int64_t ticks, uint32_t timescale) {
  if (timescale == 0) return 0;
  const bool negative = ticks < 0;
  const uint64_t magnitude = negative ? uint64_t{0} - static_cast<uint64_t>(ticks) : static_cast<uint64_t>(ticks);
  if (magnitude > (uint64_t{1} << 60)) return negative ? -(int64_t{1} << 60) : (int64_t{1} << 60);
  const uint64_t us = (magnitude / timescale) * 1000000u + (magnitude % timescale) * 1000000u / timescale;
  const int64_t value = us > (uint64_t{1} << 60) ? (int64_t{1} << 60) : static_cast<int64_t>(us);
  return negative ? -value : value;
}

struct BoxRef {
  uint32_t type = 0;
  ConstByteView body, whole;
};

// The next box inside a view of boxes; false at the end or if the next one does not fit.
bool next_box(ByteReader& r, BoxRef* box) {
  if (r.remaining() < 8) return false;
  const ConstByteView rest = r.rest();
  ByteReader h(rest);
  uint64_t size = h.be32();
  box->type = h.be32();
  uint64_t header = 8;
  if (size == 1) {
    if (rest.size() < 16) return false;
    size = read64(h);
    header = 16;
  } else if (size == 0) {
    size = rest.size();
  }
  if (size < header || size > rest.size()) return false;
  box->whole = rest.first(size);
  box->body = rest.subview(header, size - header);
  return r.skip(size);
}

// --- Fragmented MP4 -------------------------------------------------------------------------------------------------------------------------

class Mp4Parser final : public IMseParser {
 public:
  explicit Mp4Parser(allocators::IAllocator& allocator) : allocator_(allocator), pending_(allocator), tracks_(allocator), next_dts_(allocator) {}

  bool append(ConstByteView bytes, IMseSink& sink) override {
    if (!pending_.append(bytes)) return false;
    const bool ok = process(sink);
    // drop what was consumed
    if (consumed_ > 0) {
      ByteView all = pending_.writable_view();
      const uint64_t left = all.size() - consumed_;
      if (left) all.first(left).copy_from(all.subview(consumed_));
      pending_.truncate(left);
      consumed_ = 0;
    }
    if (!ok) pending_.clear();
    return ok;
  }
  void reset() override {
    pending_.clear();
    consumed_ = 0;
  }
  uint64_t pending() const override { return pending_.size(); }

 private:
  static constexpr uint64_t kMaxBox = 128u << 20;

  bool process(IMseSink& sink) {
    for (;;) {
      const ConstByteView data = pending_.view().subview(consumed_);
      if (data.size() < 8) return true;
      ByteReader h(data);
      uint64_t size = h.be32();
      const uint32_t type = h.be32();
      uint64_t header = 8;
      if (size == 1) {
        if (data.size() < 16) return true;
        size = read64(h);
        header = 16;
      } else if (size == 0) {
        return false;   // a box that runs to the end of the stream cannot be part of a segment
      }
      if (size < header || size > kMaxBox) return false;
      if (data.size() < size) return true;   // the rest is on its way
      if (type == fourcc("moov")) {
        if (!parse_moov(data.subview(header, size - header), sink)) return false;
        consumed_ += size;
      } else if (type == fourcc("moof")) {
        // the media data follows: both are needed
        const ConstByteView after = data.subview(size);
        if (after.size() < 8) return true;
        ByteReader n(after);
        uint64_t next_size = n.be32();
        const uint32_t next_type = n.be32();
        uint64_t next_header = 8;
        if (next_size == 1) {
          if (after.size() < 16) return true;
          next_size = read64(n);
          next_header = 16;
        } else if (next_size == 0) {
          return false;
        }
        if (next_type != fourcc("mdat") || next_size < next_header || next_size > kMaxBox) return false;
        if (after.size() < next_size) return true;
        if (!tracks_ready_) return false;   // media before the initialization segment
        if (!parse_moof(data.first(size + next_size), size, size + next_header, sink)) return false;
        consumed_ += size + next_size;
      } else {
        consumed_ += size;   // ftyp, styp, sidx, emsg, free, an mdat that no moof asked for ...
      }
    }
  }

  // --- the initialization segment
  bool parse_moov(ConstByteView body, IMseSink& sink) {
    auto init = UniquePointer<MseInit>::make(allocator_, allocator_);
    if (!init) return false;
    uint32_t movie_timescale = 0;
    uint64_t movie_duration = 0;
    uint64_t fragment_duration = 0;
    bool have_fragment_duration = false;
    struct Defaults {
      uint32_t id, duration, size, flags;
    };
    List<Defaults> defaults(allocator_);
    ByteReader r(body);
    BoxRef box;
    while (next_box(r, &box)) {
      if (box.type == fourcc("mvhd")) {
        ByteReader m(box.body);
        const uint8_t version = m.u8();
        m.skip(3);
        if (version == 1) {
          m.skip(16);
          movie_timescale = m.be32();
          movie_duration = read64(m);
        } else {
          m.skip(8);
          movie_timescale = m.be32();
          movie_duration = m.be32();
        }
      } else if (box.type == fourcc("mvex")) {
        ByteReader x(box.body);
        BoxRef inner;
        while (next_box(x, &inner)) {
          ByteReader t(inner.body);
          if (inner.type == fourcc("trex")) {
            t.skip(4);
            Defaults d;
            d.id = t.be32();
            t.skip(4);
            d.duration = t.be32();
            d.size = t.be32();
            d.flags = t.be32();
            if (t.ok() && !defaults.push_back(d)) return false;
          } else if (inner.type == fourcc("mehd")) {
            const uint8_t version = t.u8();
            t.skip(3);
            fragment_duration = version == 1 ? read64(t) : t.be32();
            have_fragment_duration = t.ok();
          }
        }
      }
    }
    ByteReader again(body);
    while (next_box(again, &box)) {
      if (box.type != fourcc("trak")) continue;
      MseTrackInfo info;
      if (!parse_trak(box.body, init.get(), &info)) continue;
      for (const Defaults& d : defaults.view()) {
        if (d.id != info.id) continue;
        info.default_duration = d.duration;
        info.default_size = d.size;
        info.default_flags = d.flags;
      }
      if (init->tracks.count() < 8 && !init->tracks.push_back(info)) return false;
    }
    if (init->tracks.count() == 0) return false;
    const uint64_t shown = have_fragment_duration ? fragment_duration : movie_duration;
    if (movie_timescale && shown && shown != 0xFFFFFFFFu) init->duration_us = ticks_to_us(static_cast<int64_t>(shown), movie_timescale);
    // keep what the media segments need
    tracks_.clear();
    next_dts_.clear();
    for (const MseTrackInfo& t : init->tracks.view()) {
      if (!tracks_.push_back(t) || !next_dts_.push_back(int64_t{0})) return false;
    }
    tracks_ready_ = true;
    return sink.init_segment(util::move(init));
  }

  bool parse_trak(ConstByteView body, MseInit* init, MseTrackInfo* info) {
    ByteReader r(body);
    BoxRef box;
    uint32_t handler = 0;
    uint32_t tkhd_width = 0, tkhd_height = 0;
    ConstByteView stsd;
    while (next_box(r, &box)) {
      if (box.type == fourcc("tkhd")) {
        ByteReader t(box.body);
        const uint8_t version = t.u8();
        t.skip(3);
        t.skip(version == 1 ? 16 : 8);
        info->id = t.be32();
        t.skip(4);
        t.skip(version == 1 ? 8 : 4);
        t.skip(8 + 8 + 36);
        tkhd_width = t.be32() >> 16;
        tkhd_height = t.be32() >> 16;
        if (!t.ok()) return false;
      } else if (box.type == fourcc("mdia")) {
        ByteReader m(box.body);
        BoxRef inner;
        while (next_box(m, &inner)) {
          if (inner.type == fourcc("mdhd")) {
            ByteReader d(inner.body);
            const uint8_t version = d.u8();
            d.skip(3);
            d.skip(version == 1 ? 16 : 8);
            info->timescale = d.be32();
            if (!d.ok()) return false;
          } else if (inner.type == fourcc("hdlr")) {
            ByteReader d(inner.body);
            d.skip(8);
            handler = d.be32();
          } else if (inner.type == fourcc("minf")) {
            ByteReader n(inner.body);
            BoxRef stbl;
            while (next_box(n, &stbl)) {
              if (stbl.type != fourcc("stbl")) continue;
              ByteReader s(stbl.body);
              BoxRef entry;
              while (next_box(s, &entry))
                if (entry.type == fourcc("stsd")) stsd = entry.whole;
            }
          }
        }
      }
    }
    if (handler == fourcc("vide")) info->kind = MseTrackKind::Video;
    else if (handler == fourcc("soun")) info->kind = MseTrackKind::Audio;
    else return false;
    if (info->id == 0 || info->timescale == 0 || stsd.size() < 24) return false;
    // the first sample entry: codec and sizes
    ByteReader e(stsd.subview(16));
    const uint64_t entry_size = e.be32();
    info->codec = e.be32();
    if (!e.ok() || entry_size < 8 || entry_size - 8 > e.remaining()) return false;
    const ConstByteView entry_body = e.take(entry_size - 8);
    ByteReader q(entry_body);
    if (info->kind == MseTrackKind::Video) {
      q.skip(24);
      info->width = q.be16();
      info->height = q.be16();
      if (!q.ok()) {
        info->width = tkhd_width;
        info->height = tkhd_height;
      }
    } else {
      q.skip(16);
      info->channels = q.be16();
      q.skip(6);
      info->sample_rate = q.be32() >> 16;
    }
    info->entry_offset = init->entries.size();
    info->entry_size = stsd.size();
    return init->entries.append(stsd);
  }

  // --- a media segment: `whole` is the moof and the mdat, the moof's size is `moof_size` and the mdat's data starts at `data_start`
  bool parse_moof(ConstByteView whole, uint64_t moof_size, uint64_t data_start, IMseSink& sink) {
    ByteReader r(whole.first(moof_size));
    BoxRef moof;
    if (!next_box(r, &moof)) return false;
    ByteReader inside(moof.body);
    BoxRef box;
    uint64_t previous_end = 0;   // where the data of the traf before ended
    bool first_traf = true;
    while (next_box(inside, &box)) {
      if (box.type != fourcc("traf")) continue;
      if (!parse_traf(box.body, whole, data_start, &previous_end, first_traf, sink)) return false;
      first_traf = false;
    }
    return true;
  }

  bool parse_traf(ConstByteView body, ConstByteView whole, uint64_t data_start, uint64_t* previous_end, bool first_traf, IMseSink& sink) {
    ByteReader r(body);
    BoxRef box;
    uint32_t track_id = 0, flags = 0;
    uint32_t default_duration = 0, default_size = 0, default_flags = 0;
    bool have_duration = false, have_size = false, have_flags = false, empty = false, have_tfhd = false;
    uint64_t base_offset = 0;
    int64_t base_decode = -1;
    uint64_t cursor = 0;   // where the next trun without a data offset starts
    uint32_t track_index = 0;
    bool known = false;
    while (next_box(r, &box)) {
      ByteReader t(box.body);
      if (box.type == fourcc("tfhd")) {
        const uint32_t vf = t.be32();
        flags = vf & 0xFFFFFF;
        track_id = t.be32();
        if (flags & 0x1) {
          t.skip(8);   // an absolute offset means nothing for a segment appended on its own: the moof's start is used
        }
        if (flags & 0x2) t.skip(4);
        if (flags & 0x8) { default_duration = t.be32(); have_duration = true; }
        if (flags & 0x10) { default_size = t.be32(); have_size = true; }
        if (flags & 0x20) { default_flags = t.be32(); have_flags = true; }
        if (!t.ok()) return false;
        empty = (flags & 0x10000) != 0;
        have_tfhd = true;
        for (uint32_t i = 0; i < tracks_.count(); ++i) {
          if (tracks_[i].id == track_id) {
            track_index = i;
            known = true;
          }
        }
        if (known) {
          const MseTrackInfo& info = tracks_[track_index];
          if (!have_duration) default_duration = info.default_duration;
          if (!have_size) default_size = info.default_size;
          if (!have_flags) default_flags = info.default_flags;
          base_offset = ((flags & 0x1) != 0 || (flags & 0x20000) != 0 || first_traf) ? 0 : *previous_end;
          cursor = base_offset;
        }
      } else if (box.type == fourcc("tfdt")) {
        const uint8_t version = t.u8();
        t.skip(3);
        const uint64_t value = version == 1 ? read64(t) : t.be32();
        if (!t.ok()) return false;
        if (value > (uint64_t{1} << 62)) return false;
        base_decode = static_cast<int64_t>(value);
      } else if (box.type == fourcc("trun")) {
        if (!have_tfhd) return false;
        if (!known || empty) continue;
        const uint8_t version = t.u8();
        const uint32_t trun_flags = t.be16() << 8 | t.u8();
        const uint32_t count = t.be32();
        if (!t.ok()) return false;
        int64_t data_offset = 0;
        bool have_offset = false;
        if (trun_flags & 0x1) {
          data_offset = static_cast<int32_t>(t.be32());
          have_offset = true;
        }
        uint32_t first_flags = 0;
        bool have_first_flags = false;
        if (trun_flags & 0x4) {
          first_flags = t.be32();
          have_first_flags = true;
        }
        if (!t.ok()) return false;
        const uint64_t per_sample = 4u * (((trun_flags & 0x100) != 0) + ((trun_flags & 0x200) != 0) + ((trun_flags & 0x400) != 0) + ((trun_flags & 0x800) != 0));
        if (count > (1u << 22) || (per_sample && uint64_t{count} > t.remaining() / per_sample)) return false;
        const MseTrackInfo& info = tracks_[track_index];
        // where the samples start, in `whole`
        int64_t position;
        if (have_offset) {
          position = static_cast<int64_t>(base_offset) + data_offset;
        } else {
          position = static_cast<int64_t>(cursor);
        }
        if (base_decode >= 0) {
          next_dts_[track_index] = base_decode;
          base_decode = -1;
        }
        int64_t dts_ticks = next_dts_[track_index];
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t duration = (trun_flags & 0x100) ? t.be32() : default_duration;
          const uint32_t size = (trun_flags & 0x200) ? t.be32() : default_size;
          uint32_t sample_flags = (trun_flags & 0x400) ? t.be32() : default_flags;
          if (i == 0 && have_first_flags) sample_flags = first_flags;
          int64_t cto = 0;
          if (trun_flags & 0x800) cto = version == 0 ? int64_t{t.be32()} : int64_t{static_cast<int32_t>(t.be32())};
          if (!t.ok()) return false;
          if (position < static_cast<int64_t>(data_start) || static_cast<uint64_t>(position) > whole.size() || size > whole.size() - static_cast<uint64_t>(position)) return false;
          const int64_t pts_ticks = dts_ticks + cto;
          const int64_t dts_us = ticks_to_us(dts_ticks, info.timescale);
          const int64_t duration_us = ticks_to_us(dts_ticks + duration, info.timescale) - dts_us;
          const bool sync = info.kind == MseTrackKind::Audio || (sample_flags & 0x10000) == 0;
          if (!sink.frame(track_index, ticks_to_us(pts_ticks, info.timescale), dts_us, duration_us, sync, whole.subview(static_cast<uint64_t>(position), size))) return false;
          position += size;
          dts_ticks += duration;
        }
        next_dts_[track_index] = dts_ticks;
        cursor = static_cast<uint64_t>(position);
        *previous_end = cursor;
      }
    }
    return true;
  }

  allocators::IAllocator& allocator_;
  ByteBuffer pending_;
  uint64_t consumed_ = 0;
  List<MseTrackInfo> tracks_;
  List<int64_t> next_dts_;
  bool tracks_ready_ = false;
};

// --- MPEG audio and ADTS: a stream of frames with no initialization segment --------------------------------------------------------------------

class FramedAudioParser : public IMseParser {
 public:
  FramedAudioParser(allocators::IAllocator& allocator, uint32_t codec) : allocator_(allocator), pending_(allocator), codec_(codec) {}

  bool append(ConstByteView bytes, IMseSink& sink) override {
    if (!pending_.append(bytes)) return false;
    bool ok = true;
    uint64_t at = 0;
    for (;;) {
      const ConstByteView data = pending_.view().subview(at);
      uint64_t take = 0;   // bytes to drop (skipped), or the frame length
      FrameInfo frame;
      const Step step = look(data, &frame, &take);
      if (step == Step::NeedMore) break;
      if (step == Step::Skip) {
        at += take;
        synced_ = false;
        continue;
      }
      if (!emit(frame, data.first(take), sink)) {
        ok = false;
        break;
      }
      synced_ = true;
      at += take;
    }
    ByteView all = pending_.writable_view();
    const uint64_t left = all.size() - at;
    if (left) all.first(left).copy_from(all.subview(at));
    pending_.truncate(left);
    if (left > kMaxPending) return false;
    return ok;
  }
  void reset() override {
    pending_.clear();
    synced_ = false;
    epoch_us_ = 0;
    epoch_samples_ = 0;
  }
  uint64_t pending() const override { return pending_.size(); }

 protected:
  struct FrameInfo {
    uint32_t sample_rate = 0, channels = 0, samples = 0, length = 0, variant = 0;
  };
  enum class Step { NeedMore, Skip, Frame };
  virtual Step look(ConstByteView data, FrameInfo* frame, uint64_t* take) = 0;
  static constexpr uint64_t kMaxPending = 32u << 20;
  bool synced_ = false;

 private:
  bool emit(const FrameInfo& f, ConstByteView data, IMseSink& sink) {
    if (!announced_ || f.sample_rate != rate_ || f.channels != channels_ || f.variant != variant_) {
      if (announced_) {   // a new timeline piece: the old one's end is where this starts
        epoch_us_ += static_cast<int64_t>(epoch_samples_ * 1000000u / (rate_ ? rate_ : 1));
        epoch_samples_ = 0;
      }
      auto init = UniquePointer<MseInit>::make(allocator_, allocator_);
      if (!init) return false;
      MseTrackInfo info;
      info.id = 1;
      info.kind = MseTrackKind::Audio;
      info.codec = codec_;
      info.timescale = f.sample_rate;
      info.channels = f.channels;
      info.sample_rate = f.sample_rate;
      if (!init->tracks.push_back(info)) return false;
      if (!sink.init_segment(util::move(init))) return false;
      announced_ = true;
      rate_ = f.sample_rate;
      channels_ = f.channels;
      variant_ = f.variant;
    }
    const int64_t pts = epoch_us_ + static_cast<int64_t>(epoch_samples_ * 1000000u / f.sample_rate);
    epoch_samples_ += f.samples;
    const int64_t end = epoch_us_ + static_cast<int64_t>(epoch_samples_ * 1000000u / f.sample_rate);
    return sink.frame(0, pts, pts, end - pts, true, data);
  }

  allocators::IAllocator& allocator_;
  ByteBuffer pending_;
  uint32_t codec_;
  bool announced_ = false;
  uint32_t rate_ = 0, channels_ = 0, variant_ = 0;
  int64_t epoch_us_ = 0;
  uint64_t epoch_samples_ = 0;
};

// Layer I-III audio of MPEG-1, 2 and 2.5.
class MpegParser final : public FramedAudioParser {
 public:
  explicit MpegParser(allocators::IAllocator& allocator) : FramedAudioParser(allocator, fourcc(".mp3")) {}

 private:
  static bool header(ConstByteView d, FrameInfo* f) {
    if (d.size() < 4 || d[0] != 0xFF || (d[1] & 0xE0) != 0xE0) return false;
    const uint32_t version = (d[1] >> 3) & 3, layer = (d[1] >> 1) & 3, bitrate_index = d[2] >> 4, rate_index = (d[2] >> 2) & 3, padding = (d[2] >> 1) & 1;
    if (version == 1 || layer == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return false;
    static const uint16_t kRate1[3] = {44100, 48000, 32000};
    static const uint16_t kBitrates[2][3][14] = {
        {{32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, {32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
         {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
         {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    const bool mpeg1 = version == 3;
    uint32_t rate = kRate1[rate_index];
    if (version == 2) rate /= 2;
    else if (version == 0) rate /= 4;
    const uint32_t layer_index = layer == 3 ? 0 : layer == 2 ? 1 : 2;   // I, II, III
    const uint32_t bitrate = kBitrates[mpeg1 ? 0 : 1][layer_index][bitrate_index - 1] * 1000u;
    if (layer_index == 0) {
      f->length = (12 * bitrate / rate + padding) * 4;
      f->samples = 384;
    } else if (layer_index == 1 || mpeg1) {
      f->length = 144 * bitrate / rate + padding;
      f->samples = 1152;
    } else {
      f->length = 72 * bitrate / rate + padding;
      f->samples = 576;
    }
    f->sample_rate = rate;
    f->channels = ((d[3] >> 6) & 3) == 3 ? 1 : 2;
    f->variant = (version << 2) | layer;
    return f->length >= 4;
  }

  Step look(ConstByteView d, FrameInfo* f, uint64_t* take) override {
    if (d.empty()) return Step::NeedMore;
    // an ID3v2 tag
    if (d[0] == 'I') {
      if (d.size() < 10) return Step::NeedMore;
      if (d[1] == 'D' && d[2] == '3' && d[3] != 0xFF && d[4] != 0xFF && ((d[6] | d[7] | d[8] | d[9]) & 0x80) == 0) {
        uint64_t size = (uint64_t{d[6]} << 21) | (uint64_t{d[7]} << 14) | (uint64_t{d[8]} << 7) | d[9];
        size += 10 + ((d[5] & 0x10) ? 10 : 0);
        if (size > (16u << 20)) {
          *take = 1;
          return Step::Skip;
        }
        if (d.size() < size) return Step::NeedMore;
        *take = size;
        return Step::Skip;
      }
    }
    if (d[0] != 0xFF) {
      uint64_t n = 1;
      while (n < d.size() && d[n] != 0xFF && d[n] != 'I') ++n;
      *take = n;
      return Step::Skip;
    }
    if (d.size() < 4) return Step::NeedMore;
    if (!header(d, f)) {
      *take = 1;
      return Step::Skip;
    }
    if (d.size() < f->length) return Step::NeedMore;
    // right after a resync, the next frame has to look like one too (or the frame was only bytes that looked like a header)
    if (!synced_) {
      if (d.size() < uint64_t{f->length} + 4) return Step::NeedMore;
      FrameInfo next;
      if (!header(d.subview(f->length), &next)) {
        *take = 1;
        return Step::Skip;
      }
    }
    *take = f->length;
    return Step::Frame;
  }
};

// AAC in ADTS.
class AdtsParser final : public FramedAudioParser {
 public:
  explicit AdtsParser(allocators::IAllocator& allocator) : FramedAudioParser(allocator, fourcc("mp4a")) {}

 private:
  static bool header(ConstByteView d, FrameInfo* f) {
    if (d.size() < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return false;
    static const uint32_t kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    const uint32_t index = (d[2] >> 2) & 15;
    if (index >= 13) return false;
    const uint32_t config = ((d[2] & 1) << 2) | (d[3] >> 6);
    const uint32_t length = (uint32_t{d[3] & 3} << 11) | (uint32_t{d[4]} << 3) | (d[5] >> 5);
    const uint32_t header_size = (d[1] & 1) ? 7 : 9;
    if (length < header_size) return false;
    f->length = length;
    f->sample_rate = kRates[index];
    f->channels = config == 0 ? 2 : config == 7 ? 8 : config;
    f->samples = 1024u * ((d[6] & 3) + 1);
    f->variant = (d[2] >> 6) & 3;
    return true;
  }

  Step look(ConstByteView d, FrameInfo* f, uint64_t* take) override {
    if (d.empty()) return Step::NeedMore;
    if (d[0] != 0xFF) {
      uint64_t n = 1;
      while (n < d.size() && d[n] != 0xFF) ++n;
      *take = n;
      return Step::Skip;
    }
    if (d.size() < 7) return Step::NeedMore;
    if (!header(d, f)) {
      *take = 1;
      return Step::Skip;
    }
    if (d.size() < f->length) return Step::NeedMore;
    if (!synced_) {
      if (d.size() < uint64_t{f->length} + 7) return Step::NeedMore;
      FrameInfo next;
      if (!header(d.subview(f->length), &next)) {
        *take = 1;
        return Step::Skip;
      }
    }
    *take = f->length;
    return Step::Frame;
  }
};

// --- Types ------------------------------------------------------------------------------------------------------------------------------------

bool starts_with_ci(StringView text, const char* prefix) {
  const StringView p(prefix);
  return text.size() >= p.size() && text.substr(0, p.size()).equals_ignore_case(p);
}

// One codec name from a "codecs" list: is it one that format can carry and the machine can play, and is it a video codec?
bool codec_ok(MseFormat format, StringView codec, bool is_video_type, bool* video) {
  codec = codec.trimmed();
  if (codec.empty()) return false;
  if (format == MseFormat::Mp4) {
    if (starts_with_ci(codec, "avc1.") || codec.equals_ignore_case("avc1")) {
      if (!is_video_type) return false;
      *video = true;
      return true;
    }
    if (codec.equals_ignore_case("mp4a.40.2") || codec.equals_ignore_case("mp4a.40.5") || codec.equals_ignore_case("mp4a.40.29") || codec.equals_ignore_case("mp4a.40.02") ||
        codec.equals_ignore_case("mp4a.69") || codec.equals_ignore_case("mp4a.6b"))
      return true;
    return false;
  }
  if (format == MseFormat::Mpeg) return codec.equals_ignore_case("mp3") || codec.equals_ignore_case("mp4a.69") || codec.equals_ignore_case("mp4a.6b");
  return codec.equals_ignore_case("mp4a.40.2") || codec.equals_ignore_case("mp4a.40.5") || codec.equals_ignore_case("mp4a.40.29");
}

}  // namespace

bool mse_type_supported(StringView type, MseFormat* format, bool* has_video) {
  type = type.trimmed();
  uint64_t semicolon = 0;
  while (semicolon < type.size() && type[semicolon] != ';') ++semicolon;
  const StringView main = type.substr(0, semicolon).trimmed();
  MseFormat f;
  bool video_type = false;
  if (main.equals_ignore_case("video/mp4")) {
    f = MseFormat::Mp4;
    video_type = true;
  } else if (main.equals_ignore_case("audio/mp4")) {
    f = MseFormat::Mp4;
  } else if (main.equals_ignore_case("audio/mpeg") || main.equals_ignore_case("audio/mp3")) {
    f = MseFormat::Mpeg;
  } else if (main.equals_ignore_case("audio/aac")) {
    f = MseFormat::Adts;
  } else {
    return false;
  }
  bool video = false;
  bool saw_codecs = false;
  // the parameters: only codecs=... is understood; another one is not an error
  for (uint64_t at = semicolon; at < type.size();) {
    ++at;   // the ';'
    uint64_t end = at;
    bool quoted = false;
    while (end < type.size() && (quoted || type[end] != ';')) {
      if (type[end] == '"') quoted = !quoted;
      ++end;
    }
    StringView parameter = type.substr(at, end - at).trimmed();
    at = end;
    if (!starts_with_ci(parameter, "codecs")) continue;
    uint64_t eq = 6;
    while (eq < parameter.size() && parameter[eq] == ' ') ++eq;
    if (eq >= parameter.size() || parameter[eq] != '=') return false;
    StringView value = parameter.substr(eq + 1).trimmed();
    if (value.size() >= 2 && value[0] == '"' && value[value.size() - 1] == '"') value = value.substr(1, value.size() - 2);
    else if (!value.empty() && value[0] == '"') return false;
    saw_codecs = true;
    if (value.trimmed().empty()) return false;
    for (uint64_t c = 0; c < value.size();) {
      uint64_t e = c;
      while (e < value.size() && value[e] != ',') ++e;
      if (!codec_ok(f, value.substr(c, e - c), video_type, &video)) return false;
      c = e + 1;
    }
  }
  if (!saw_codecs) video = video_type;
  if (format) *format = f;
  if (has_video) *has_video = video;
  return true;
}

UniquePointer<IMseParser> make_mse_parser(allocators::IAllocator& allocator, MseFormat format) {
  switch (format) {
    case MseFormat::Mp4: return UniquePointer<IMseParser>::make_as<Mp4Parser>(allocator, allocator);
    case MseFormat::Mpeg: return UniquePointer<IMseParser>::make_as<MpegParser>(allocator, allocator);
    case MseFormat::Adts: return UniquePointer<IMseParser>::make_as<AdtsParser>(allocator, allocator);
  }
  return UniquePointer<IMseParser>();
}

}  // namespace media
