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
      } else if (box.type == fourcc("pssh")) {
        if (init->pssh.size() + box.whole.size() > (1u << 20) || !init->pssh.append(box.whole)) return false;
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
    ByteBuffer clear(allocator_);
    if (!unprotect(stsd, entry_size, info->kind, info, &clear)) return false;
    info->entry_size = clear.size();
    return init->entries.append(clear.view());
  }

  static bool put32(ByteBuffer* b, uint32_t v) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return b->append(ConstByteView(bytes, 4));
  }

  // The sample description as it was before it was protected: an 'encv' / 'enca' entry becomes the entry of its original format (the 'frma' of its 'sinf')
  // without the 'sinf', and the scheme and the 'tenc' go to info->protection. An entry that is not protected is copied as it is.
  bool unprotect(ConstByteView stsd, uint64_t entry_size, MseTrackKind kind, MseTrackInfo* info, ByteBuffer* out) {
    const ConstByteView entry = stsd.subview(16, entry_size);
    const ConstByteView body = entry.subview(8);
    const uint32_t type = (uint32_t{entry[4]} << 24) | (uint32_t{entry[5]} << 16) | (uint32_t{entry[6]} << 8) | entry[7];
    const uint32_t wanted = kind == MseTrackKind::Video ? fourcc("encv") : fourcc("enca");
    if (type != wanted) return out->append(stsd);
    uint64_t first_child = 78;
    if (kind == MseTrackKind::Audio) {
      first_child = 28;
      if (body.size() >= 10) {
        const uint32_t version = (uint32_t{body[8]} << 8) | body[9];
        first_child += version == 1 ? 16 : version == 2 ? 36 : 0;
      }
    }
    if (first_child > body.size()) return false;
    ConstByteView sinf;
    uint64_t sinf_at = 0;
    {
      ByteReader c(body.subview(first_child));
      BoxRef child;
      uint64_t at = first_child;
      while (next_box(c, &child)) {
        if (child.type == fourcc("sinf") && sinf.empty()) {
          sinf = child.body;
          sinf_at = at;
          sinf_size_ = child.whole.size();
        }
        at += child.whole.size();
      }
    }
    if (sinf.empty()) return false;
    uint32_t original = 0;
    if (!parse_sinf(sinf, &info->protection, &original)) return false;
    info->codec = original;
    // the entry again: header, the body up to the 'sinf', what follows it
    const uint64_t new_entry = entry_size - sinf_size_;
    if (!put32(out, static_cast<uint32_t>(16 + new_entry)) || !out->append(stsd.subview(4, 4)) || !out->append(stsd.subview(8, 4)) || !put32(out, 1)) return false;
    if (!put32(out, static_cast<uint32_t>(new_entry)) || !put32(out, original)) return false;
    if (!out->append(body.first(sinf_at)) || !out->append(body.subview(sinf_at + sinf_size_))) return false;
    return true;
  }

  // 'sinf': the original format ('frma'), the scheme ('schm') and the track defaults ('tenc' inside 'schi').
  static bool parse_sinf(ConstByteView sinf, MseProtection* p, uint32_t* original) {
    ByteReader r(sinf);
    BoxRef box;
    bool have_tenc = false;
    *original = 0;
    while (next_box(r, &box)) {
      ByteReader b(box.body);
      if (box.type == fourcc("frma")) {
        *original = b.be32();
      } else if (box.type == fourcc("schm")) {
        b.skip(4);
        const uint32_t scheme = b.be32();
        if (!b.ok()) return false;
        p->scheme = scheme == fourcc("cenc") ? MseScheme::Cenc : scheme == fourcc("cens") ? MseScheme::Cens : scheme == fourcc("cbc1") ? MseScheme::Cbc1 : scheme == fourcc("cbcs") ? MseScheme::Cbcs : MseScheme::None;
        if (p->scheme == MseScheme::None) return false;   // (a protection scheme this machine does not know)
      } else if (box.type == fourcc("schi")) {
        ByteReader c(box.body);
        BoxRef inner;
        while (next_box(c, &inner)) {
          if (inner.type != fourcc("tenc")) continue;
          ByteReader t(inner.body);
          const uint8_t version = t.u8();
          t.skip(3);
          t.skip(1);
          const uint8_t pattern = t.u8();
          p->is_protected = t.u8() != 0;
          p->iv_size = t.u8();
          for (uint8_t& k : p->kid) k = t.u8();
          if (version >= 1) {
            p->crypt_blocks = pattern >> 4;
            p->skip_blocks = pattern & 15;
          }
          if (p->is_protected && p->iv_size == 0) {
            p->constant_iv_size = t.u8();
            if (p->constant_iv_size != 8 && p->constant_iv_size != 16) return false;
            for (uint32_t i = 0; i < p->constant_iv_size; ++i) p->constant_iv[i] = t.u8();
          }
          if (!t.ok() || (p->iv_size != 0 && p->iv_size != 8 && p->iv_size != 16)) return false;
          have_tenc = true;
        }
      }
    }
    return p->scheme != MseScheme::None && have_tenc && *original != 0;
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
    {   // 'pssh' boxes in the movie fragment: the keys of what follows
      ByteReader scan(moof.body);
      ByteBuffer pssh(allocator_);
      while (next_box(scan, &box))
        if (box.type == fourcc("pssh") && !pssh.append(box.whole)) return false;
      if (pssh.size() > 0) sink.protection_data(pssh.view());
    }
    while (next_box(inside, &box)) {
      if (box.type != fourcc("traf")) continue;
      if (!parse_traf(box.body, whole, data_start, &previous_end, first_traf, sink)) return false;
      first_traf = false;
    }
    return true;
  }

  // One entry of a 'seig' sample group: protection that holds for some samples of a fragment.
  struct Seig {
    bool is_protected = false;
    uint8_t iv_size = 0, crypt = 0, skip = 0, constant_size = 0;
    uint8_t kid[16] = {};
    uint8_t constant_iv[16] = {};
  };
  struct Run {
    uint32_t count, index;
  };

  bool parse_traf(ConstByteView body, ConstByteView whole, uint64_t data_start, uint64_t* previous_end, bool first_traf, IMseSink& sink) {
    // what the traf has, before its samples are read (the boxes about the encryption come after the 'trun' boxes)
    ConstByteView tfhd, tfdt, senc, saiz, saio, sgpd, sbgp;
    List<ConstByteView> truns(allocator_);
    {
      ByteReader r(body);
      BoxRef box;
      while (next_box(r, &box)) {
        const bool seig = box.body.size() >= 8 && (uint32_t{box.body[4]} << 24 | uint32_t{box.body[5]} << 16 | uint32_t{box.body[6]} << 8 | box.body[7]) == fourcc("seig");
        if (box.type == fourcc("tfhd")) tfhd = box.body;
        else if (box.type == fourcc("tfdt")) tfdt = box.body;
        else if (box.type == fourcc("trun")) {
          if (truns.count() >= 64 || !truns.push_back(box.body)) return false;
        } else if (box.type == fourcc("senc")) senc = box.body;
        else if (box.type == fourcc("saiz")) saiz = box.body;
        else if (box.type == fourcc("saio")) saio = box.body;
        else if (box.type == fourcc("sgpd") && seig) sgpd = box.body;
        else if (box.type == fourcc("sbgp") && seig) sbgp = box.body;
      }
    }
    if (truns.count() == 0) return true;
    if (tfhd.empty()) return false;
    uint32_t track_id = 0, flags = 0, default_duration = 0, default_size = 0, default_flags = 0;
    bool have_duration = false, have_size = false, have_flags = false;
    {
      ByteReader t(tfhd);
      flags = t.be32() & 0xFFFFFF;
      track_id = t.be32();
      if (flags & 0x1) t.skip(8);   // an absolute offset means nothing for a segment appended on its own: the moof's start is used
      if (flags & 0x2) t.skip(4);
      if (flags & 0x8) { default_duration = t.be32(); have_duration = true; }
      if (flags & 0x10) { default_size = t.be32(); have_size = true; }
      if (flags & 0x20) { default_flags = t.be32(); have_flags = true; }
      if (!t.ok()) return false;
    }
    uint32_t track_index = 0;
    bool known = false;
    for (uint32_t i = 0; i < tracks_.count(); ++i) {
      if (tracks_[i].id == track_id) {
        track_index = i;
        known = true;
      }
    }
    if (!known || (flags & 0x10000) != 0) return true;
    const MseTrackInfo& info = tracks_[track_index];
    if (!have_duration) default_duration = info.default_duration;
    if (!have_size) default_size = info.default_size;
    if (!have_flags) default_flags = info.default_flags;
    const uint64_t base_offset = ((flags & 0x1) != 0 || (flags & 0x20000) != 0 || first_traf) ? 0 : *previous_end;
    uint64_t cursor = base_offset;
    int64_t base_decode = -1;
    if (!tfdt.empty()) {
      ByteReader t(tfdt);
      const uint8_t version = t.u8();
      t.skip(3);
      const uint64_t value = version == 1 ? read64(t) : t.be32();
      if (!t.ok() || value > (uint64_t{1} << 62)) return false;
      base_decode = static_cast<int64_t>(value);
    }
    if (base_decode >= 0) next_dts_[track_index] = base_decode;

    // --- the protection of the samples
    const bool protected_track = info.protection.scheme != MseScheme::None;
    List<Seig> seigs(allocator_);
    List<Run> runs(allocator_);
    ByteReader senc_reader(senc);
    uint32_t senc_samples = 0;
    bool senc_subsamples = false;
    if (protected_track && !sgpd.empty()) {
      ByteReader g(sgpd);
      const uint32_t vf = g.be32();
      const uint8_t version = static_cast<uint8_t>(vf >> 24);
      g.skip(4);
      uint32_t default_length = 0;
      if (version >= 1) default_length = g.be32();
      if (version >= 2) g.skip(4);
      const uint32_t count = g.be32();
      if (!g.ok() || count > 1024) return false;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t length = default_length;
        if (version >= 1 && default_length == 0) length = g.be32();
        const ConstByteView entry = g.take(length);
        if (!g.ok() || length < 20) return false;
        ByteReader e(entry);
        Seig seig;
        e.skip(1);
        const uint8_t pattern = e.u8();
        seig.is_protected = e.u8() != 0;
        seig.iv_size = e.u8();
        seig.crypt = pattern >> 4;
        seig.skip = pattern & 15;
        for (uint8_t& k : seig.kid) k = e.u8();
        if (seig.is_protected && seig.iv_size == 0) {
          seig.constant_size = e.u8();
          if ((seig.constant_size != 8 && seig.constant_size != 16) || e.remaining() < seig.constant_size) return false;
          for (uint32_t k = 0; k < seig.constant_size; ++k) seig.constant_iv[k] = e.u8();
        }
        if (!e.ok() || (seig.iv_size != 0 && seig.iv_size != 8 && seig.iv_size != 16) || !seigs.push_back(seig)) return false;
      }
    }
    if (protected_track && !sbgp.empty()) {
      ByteReader g(sbgp);
      const uint32_t vf = g.be32();
      g.skip(4);
      if ((vf >> 24) >= 1) g.skip(4);
      const uint32_t count = g.be32();
      if (!g.ok() || count > (1u << 20)) return false;
      for (uint32_t i = 0; i < count; ++i) {
        Run run;
        run.count = g.be32();
        run.index = g.be32();
        if (!g.ok() || !runs.push_back(run)) return false;
      }
    }
    if (protected_track && !senc.empty()) {
      const uint32_t vf = senc_reader.be32();
      senc_subsamples = (vf & 2) != 0;
      senc_samples = senc_reader.be32();
      if (!senc_reader.ok()) return false;
      // Some muxers (FFmpeg's, for one) write the entries of the fragments before this one in front of its own: the box then has more entries than the
      // fragment has samples, and the last ones are the ones that are meant.
      uint64_t total = 0;
      for (const ConstByteView& trun : truns.view()) {
        ByteReader c(trun);
        c.skip(4);
        total += c.be32();
      }
      if (senc_samples > total) {
        const uint32_t surplus = senc_samples - static_cast<uint32_t>(total);
        const uint32_t iv_bytes = info.protection.iv_size;
        for (uint32_t k = 0; k < surplus; ++k) {
          senc_reader.skip(iv_bytes);
          if (senc_subsamples) {
            const uint32_t subsamples = senc_reader.be16();
            senc_reader.skip(uint64_t{subsamples} * 6);
          }
        }
        if (!senc_reader.ok()) return false;
        senc_samples = static_cast<uint32_t>(total);
      }
    }
    // the sample information of 'saiz' / 'saio', when there is no 'senc'
    uint64_t aux_at = 0;
    uint32_t saiz_default = 0, saiz_count = 0;
    ConstByteView saiz_sizes;
    bool have_aux = false;
    if (protected_track && senc.empty() && !saiz.empty() && !saio.empty()) {
      ByteReader z(saiz);
      const uint32_t vf = z.be32();
      if (vf & 1) z.skip(8);
      saiz_default = z.u8();
      saiz_count = z.be32();
      if (saiz_default == 0) saiz_sizes = z.take(saiz_count);
      ByteReader o(saio);
      const uint32_t of = o.be32();
      if (of & 1) o.skip(8);
      const uint32_t entries = o.be32();
      aux_at = (of >> 24) == 1 ? read64(o) : o.be32();
      if (!z.ok() || !o.ok() || entries == 0) return false;
      have_aux = true;
    }
    uint32_t run_index = 0, run_used = 0, sample_number = 0;
    uint64_t aux_cursor = aux_at;

    // The protection of one sample: false if the segment does not say enough. `crypto->scheme` stays None for a sample in the clear.
    auto protection_of = [&](uint32_t size, MseSampleCrypto* crypto) -> bool {
      const uint32_t n = sample_number++;
      if (!protected_track) return true;
      MseProtection effective = info.protection;
      while (run_index < runs.count() && run_used >= runs[run_index].count) {
        ++run_index;
        run_used = 0;
      }
      if (run_index < runs.count()) {
        const uint32_t group = runs[run_index].index;
        ++run_used;
        if (group >= 1 && group <= seigs.count()) {
          const Seig& g = seigs[group - 1];
          effective.is_protected = g.is_protected;
          effective.iv_size = g.iv_size;
          effective.crypt_blocks = g.crypt;
          effective.skip_blocks = g.skip;
          effective.constant_iv_size = g.constant_size;
          for (int k = 0; k < 16; ++k) {
            effective.kid[k] = g.kid[k];
            effective.constant_iv[k] = g.constant_iv[k];
          }
        }
      }
      // the sample's own information: its IV, and the subsamples
      ConstByteView information;   // (IV and subsample entries as they are stored)
      uint8_t iv[16] = {};
      uint32_t iv_bytes = effective.iv_size;
      ConstByteView subsamples;
      uint32_t subsample_count = 0;
      if (!senc.empty()) {
        if (n >= senc_samples) return false;
        for (uint32_t k = 0; k < iv_bytes; ++k) iv[k] = senc_reader.u8();
        if (senc_subsamples) {
          subsample_count = senc_reader.be16();
          subsamples = senc_reader.take(uint64_t{subsample_count} * 6);
        }
        if (!senc_reader.ok()) return false;
      } else if (have_aux) {
        if (n >= saiz_count) return false;
        const uint32_t length = saiz_default ? saiz_default : saiz_sizes[n];
        if (aux_cursor > whole.size() || length > whole.size() - aux_cursor || length < iv_bytes) return false;
        ByteReader a(whole.subview(aux_cursor, length));
        for (uint32_t k = 0; k < iv_bytes; ++k) iv[k] = a.u8();
        if (length > iv_bytes) {
          subsample_count = a.be16();
          subsamples = a.take(uint64_t{subsample_count} * 6);
          if (!a.ok()) return false;
        }
        aux_cursor += length;
      } else if (effective.is_protected && iv_bytes != 0) {
        return false;   // protected, and nothing says how
      }
      if (!effective.is_protected) return true;
      if (iv_bytes == 0) {
        iv_bytes = effective.constant_iv_size;
        for (uint32_t k = 0; k < iv_bytes; ++k) iv[k] = effective.constant_iv[k];
      }
      // the subsamples must cover the sample
      if (subsample_count > 0) {
        uint64_t total = 0;
        ByteReader u(subsamples);
        for (uint32_t k = 0; k < subsample_count; ++k) {
          const uint64_t clear = u.be16();
          const uint64_t encrypted = u.be32();
          total += clear + encrypted;
        }
        if (!u.ok() || total != size) return false;
      }
      crypto->scheme = effective.scheme;
      for (int k = 0; k < 16; ++k) {
        crypto->kid[k] = effective.kid[k];
        crypto->iv[k] = iv[k];   // (an 8-byte IV is the first half of the counter block; the rest is zero)
      }
      crypto->crypt_blocks = effective.crypt_blocks;
      crypto->skip_blocks = effective.skip_blocks;
      crypto->subsample_count = subsample_count;
      crypto->subsamples = subsamples;
      return true;
    };

    // --- the samples
    int64_t dts_ticks = next_dts_[track_index];
    for (const ConstByteView& trun : truns.view()) {
      ByteReader t(trun);
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
      int64_t position = have_offset ? static_cast<int64_t>(base_offset) + data_offset : static_cast<int64_t>(cursor);
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
        MseSampleCrypto crypto;
        if (!protection_of(size, &crypto)) return false;
        if (!sink.frame(track_index, ticks_to_us(pts_ticks, info.timescale), dts_us, duration_us, sync, whole.subview(static_cast<uint64_t>(position), size),
                        crypto.scheme == MseScheme::None ? nullptr : &crypto))
          return false;
        position += size;
        dts_ticks += duration;
      }
      cursor = static_cast<uint64_t>(position);
      *previous_end = cursor;
    }
    next_dts_[track_index] = dts_ticks;
    return true;
  }

  allocators::IAllocator& allocator_;
  ByteBuffer pending_;
  uint64_t consumed_ = 0;
  List<MseTrackInfo> tracks_;
  List<int64_t> next_dts_;
  bool tracks_ready_ = false;
  uint64_t sinf_size_ = 0;   // (the size of the 'sinf' unprotect() found)
};

// --- WebM (Matroska): EBML elements ---------------------------------------------------------------------------------------------------------

// The id of an EBML element (1-4 bytes, the length marker kept) and its size (1-8 bytes; `unknown` when all bits are 1, as live streams write). Result: 1 done,
// 0 more bytes are needed, -1 it is not an element.
int ebml_header(ConstByteView d, uint32_t* id, uint64_t* size, bool* unknown, uint32_t* length) {
  if (d.empty()) return 0;
  uint32_t id_length = 1;
  while (id_length <= 4 && !(d[0] & (0x80 >> (id_length - 1)))) ++id_length;
  if (id_length > 4) return -1;
  if (d.size() < id_length + 1u) return 0;
  uint32_t value = 0;
  for (uint32_t i = 0; i < id_length; ++i) value = (value << 8) | d[i];
  const uint8_t first = d[id_length];
  uint32_t size_length = 1;
  while (size_length <= 8 && !(first & (0x80 >> (size_length - 1)))) ++size_length;
  if (size_length > 8) return -1;
  if (d.size() < id_length + size_length) return 0;
  uint64_t v = first & (0xFFu >> size_length);
  bool all_ones = v == (0xFFu >> size_length);
  for (uint32_t i = 1; i < size_length; ++i) {
    v = (v << 8) | d[id_length + i];
    all_ones = all_ones && d[id_length + i] == 0xFF;
  }
  *id = value;
  *size = v;
  *unknown = all_ones;
  *length = id_length + size_length;
  return 1;
}

// A variable length integer (a track number, a lace size) at the reader's place; `signed_diff` reads the signed form of EBML lacing.
bool ebml_vint(ByteReader& r, uint64_t* out, uint32_t* length = nullptr) {
  uint8_t first;
  if (!r.peek_u8(0, &first)) return false;
  uint32_t n = 1;
  while (n <= 8 && !(first & (0x80 >> (n - 1)))) ++n;
  if (n > 8) return false;
  uint64_t v = first & (0xFFu >> n);
  r.u8();
  for (uint32_t i = 1; i < n; ++i) v = (v << 8) | r.u8();
  if (length) *length = n;
  *out = v;
  return r.ok();
}

uint64_t ebml_uint(ConstByteView d) {
  uint64_t v = 0;
  for (uint64_t i = 0; i < d.size() && i < 8; ++i) v = (v << 8) | d[i];
  return v;
}

double ebml_float(ConstByteView d) {
  if (d.size() == 4) {
    const uint32_t bits = static_cast<uint32_t>(ebml_uint(d));
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
  }
  if (d.size() == 8) {
    const uint64_t bits = ebml_uint(d);
    double f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
  }
  return 0;
}

constexpr uint32_t kIdSegment = 0x18538067, kIdCluster = 0x1F43B675, kIdInfo = 0x1549A966, kIdTracks = 0x1654AE6B, kIdTimecode = 0xE7, kIdSimpleBlock = 0xA3,
                   kIdBlockGroup = 0xA0, kIdBlock = 0xA1, kIdBlockDuration = 0x9B, kIdReferenceBlock = 0xFB, kIdTimecodeScale = 0x2AD7B1, kIdDuration = 0x4489,
                   kIdTrackEntry = 0xAE, kIdTrackNumber = 0xD7, kIdTrackType = 0x83, kIdCodecId = 0x86, kIdCodecPrivate = 0x63A2, kIdDefaultDuration = 0x23E383,
                   kIdVideo = 0xE0, kIdAudio = 0xE1, kIdPixelWidth = 0xB0, kIdPixelHeight = 0xBA, kIdSamplingFrequency = 0xB5, kIdChannels = 0x9F,
                   kIdContentEncodings = 0x6D80;

class WebmParser final : public IMseParser {
 public:
  explicit WebmParser(allocators::IAllocator& allocator) : allocator_(allocator), pending_(allocator), tracks_(allocator) {}

  bool append(ConstByteView bytes, IMseSink& sink) override {
    if (!pending_.append(bytes)) return false;
    const bool ok = process(sink);
    if (consumed_ > 0) {
      ByteView all = pending_.writable_view();
      const uint64_t left = all.size() - consumed_;
      if (left) all.first(left).copy_from(all.subview(consumed_));
      pending_.truncate(left);
      consumed_ = 0;
    }
    if (!ok) {
      pending_.clear();
      skip_ = 0;
    }
    return ok;
  }
  void reset() override {
    pending_.clear();
    consumed_ = 0;
    skip_ = 0;
    have_cluster_ = false;
  }
  uint64_t pending() const override { return pending_.size(); }

 private:
  struct Track {
    uint64_t number = 0;
    MseTrackKind kind = MseTrackKind::Audio;
    int64_t default_duration_us = 0, last_duration_us = 0;
  };
  static constexpr uint64_t kMaxElement = 64u << 20;

  bool process(IMseSink& sink) {
    for (;;) {
      if (skip_ > 0) {   // the rest of an element that is not looked at
        const uint64_t have = pending_.size() - consumed_;
        const uint64_t take = have < skip_ ? have : skip_;
        consumed_ += take;
        skip_ -= take;
        if (skip_ > 0) return true;
        continue;
      }
      const ConstByteView data = pending_.view().subview(consumed_);
      uint32_t id, length;
      uint64_t size;
      bool unknown;
      const int h = ebml_header(data, &id, &size, &unknown, &length);
      if (h == 0) return true;
      if (h < 0) return false;
      if (id == kIdSegment || id == kIdCluster) {   // (looked into, not waited for: they can be as long as the stream)
        consumed_ += length;
        if (id == kIdCluster) have_cluster_ = false;
        continue;
      }
      const bool looked_at = id == kIdInfo || id == kIdTracks || id == kIdSimpleBlock || id == kIdBlockGroup || id == kIdTimecode;
      if (!looked_at) {
        if (unknown) return false;
        consumed_ += length;
        skip_ = size;
        continue;
      }
      if (unknown || size > kMaxElement) return false;
      if (data.size() < length + size) return true;
      const ConstByteView body = data.subview(length, size);
      bool ok = true;
      if (id == kIdInfo) ok = parse_info(body);
      else if (id == kIdTracks) ok = parse_tracks(body, sink);
      else if (id == kIdTimecode) {
        cluster_timecode_ = static_cast<int64_t>(ebml_uint(body));
        have_cluster_ = true;
      } else if (id == kIdSimpleBlock) ok = parse_block(body, true, true, -1, sink);
      else ok = parse_group(body, sink);
      if (!ok) return false;
      consumed_ += length + size;
    }
  }

  bool parse_info(ConstByteView body) {
    uint64_t at = 0;
    double duration = -1;
    while (at < body.size()) {
      uint32_t id, length;
      uint64_t size;
      bool unknown;
      if (ebml_header(body.subview(at), &id, &size, &unknown, &length) != 1 || unknown || size > body.size() - at - length) return false;
      const ConstByteView v = body.subview(at + length, size);
      if (id == kIdTimecodeScale) scale_ns_ = ebml_uint(v);
      else if (id == kIdDuration) duration = ebml_float(v);
      at += length + size;
    }
    if (scale_ns_ == 0 || scale_ns_ > 1000000000u) return false;
    duration_us_ = duration > 0 && duration < 9e12 ? static_cast<int64_t>(duration * static_cast<double>(scale_ns_) / 1000.0) : -1;
    return true;
  }

  static bool put_u32(ByteBuffer* b, uint32_t v) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    return b->append(ConstByteView(bytes, 4));
  }

  // The sample description a VP9 track gets in the MP4 file the video player opens: a 'vp09' entry with a 'vpcC' box.
  static bool vp9_entry(ByteBuffer* out, uint32_t width, uint32_t height) {
    const uint32_t entry = 8 + 78 + 8 + 12;
    const uint8_t vpcc[12] = {1, 0, 0, 0, 0, 10, 0x82, 2, 2, 2, 0, 0};   // version 1, profile 0, level 1.0, 8 bit 4:2:0, unspecified colour, no init data
    if (!put_u32(out, 16 + entry) || !out->append(ConstByteView(reinterpret_cast<const uint8_t*>("stsd"), 4)) || !put_u32(out, 0) || !put_u32(out, 1)) return false;
    if (!put_u32(out, entry) || !out->append(ConstByteView(reinterpret_cast<const uint8_t*>("vp09"), 4)) || !out->put_zeros(6)) return false;
    const uint8_t head[] = {0, 1};   // data reference index
    if (!out->append(ConstByteView(head, 2)) || !out->put_zeros(16)) return false;
    const uint8_t size[] = {static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width), static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height)};
    if (!out->append(ConstByteView(size, 4)) || !put_u32(out, 0x00480000) || !put_u32(out, 0x00480000) || !put_u32(out, 0)) return false;
    const uint8_t frames[] = {0, 1};
    if (!out->append(ConstByteView(frames, 2)) || !out->put_zeros(32)) return false;
    const uint8_t depth[] = {0, 0x18, 0xFF, 0xFF};
    if (!out->append(ConstByteView(depth, 4))) return false;
    if (!put_u32(out, 20) || !out->append(ConstByteView(reinterpret_cast<const uint8_t*>("vpcC"), 4)) || !out->append(ConstByteView(vpcc, 12))) return false;
    return true;
  }

  bool parse_tracks(ConstByteView body, IMseSink& sink) {
    auto init = UniquePointer<MseInit>::make(allocator_, allocator_);
    if (!init) return false;
    tracks_.clear();
    uint64_t at = 0;
    while (at < body.size()) {
      uint32_t id, length;
      uint64_t size;
      bool unknown;
      if (ebml_header(body.subview(at), &id, &size, &unknown, &length) != 1 || unknown || size > body.size() - at - length) return false;
      if (id == kIdTrackEntry && !parse_entry(body.subview(at + length, size), init.get())) return false;
      at += length + size;
    }
    if (init->tracks.count() == 0) return false;   // (nothing in it that can be played)
    init->duration_us = duration_us_;
    return sink.init_segment(util::move(init));
  }

  bool parse_entry(ConstByteView body, MseInit* init) {
    Track track;
    uint64_t type = 0;
    uint32_t width = 0, height = 0, channels = 1;
    double rate = 8000;
    ConstByteView codec, priv;
    bool encrypted = false;
    uint64_t at = 0;
    while (at < body.size()) {
      uint32_t id, length;
      uint64_t size;
      bool unknown;
      if (ebml_header(body.subview(at), &id, &size, &unknown, &length) != 1 || unknown || size > body.size() - at - length) return false;
      const ConstByteView v = body.subview(at + length, size);
      if (id == kIdTrackNumber) track.number = ebml_uint(v);
      else if (id == kIdTrackType) type = ebml_uint(v);
      else if (id == kIdCodecId) codec = v;
      else if (id == kIdCodecPrivate) priv = v;
      else if (id == kIdDefaultDuration) track.default_duration_us = static_cast<int64_t>(ebml_uint(v) / 1000);
      else if (id == kIdContentEncodings) encrypted = true;
      else if (id == kIdVideo || id == kIdAudio) {
        uint64_t p = 0;
        while (p < v.size()) {
          uint32_t cid, clen;
          uint64_t csize;
          bool cunknown;
          if (ebml_header(v.subview(p), &cid, &csize, &cunknown, &clen) != 1 || cunknown || csize > v.size() - p - clen) return false;
          const ConstByteView cv = v.subview(p + clen, csize);
          if (cid == kIdPixelWidth) width = static_cast<uint32_t>(ebml_uint(cv));
          else if (cid == kIdPixelHeight) height = static_cast<uint32_t>(ebml_uint(cv));
          else if (cid == kIdChannels) channels = static_cast<uint32_t>(ebml_uint(cv));
          else if (cid == kIdSamplingFrequency) rate = ebml_float(cv);
          p += clen + csize;
        }
      }
      at += length + size;
    }
    const auto is = [&](const char* name) { return StringView(reinterpret_cast<const char*>(codec.data()), codec.size()).equals(name); };
    MseTrackInfo info;
    info.id = static_cast<uint32_t>(track.number);
    info.timescale = 1000000;
    info.entry_offset = init->entries.size();
    if (type == 1 && is("V_VP9")) {
      if (width == 0 || height == 0 || width > 16384 || height > 16384) return false;
      info.kind = MseTrackKind::Video;
      info.codec = fourcc("vp09");
      info.container = kContainerMp4;
      info.width = width;
      info.height = height;
      ByteBuffer entry(allocator_);
      if (!vp9_entry(&entry, width, height) || !init->entries.append(entry.view())) return false;
      info.entry_size = entry.size();
    } else if (type == 2 && (is("A_OPUS") || is("A_VORBIS"))) {
      const bool opus = is("A_OPUS");
      info.kind = MseTrackKind::Audio;
      info.codec = opus ? fourcc("Opus") : fourcc("vorb");
      info.container = opus ? kContainerOpus : kContainerVorbis;
      info.channels = channels;
      info.sample_rate = opus ? 48000 : static_cast<uint32_t>(rate);
      if (info.sample_rate == 0 || channels == 0 || channels > 8) return false;
      if (opus && priv.size() < 19) {   // (no 'OpusHead': one is made, the way the standard's defaults say)
        ByteBuffer head(allocator_);
        const uint8_t bytes[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<uint8_t>(channels), 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0};
        if (!head.append(ConstByteView(bytes, sizeof bytes)) || !init->entries.append(head.view())) return false;
        info.entry_size = head.size();
      } else {
        if (priv.empty() || !init->entries.append(priv)) return false;
        info.entry_size = priv.size();
      }
    } else {
      return true;   // another track (VP8, AV1, subtitles ...): not kept
    }
    if (encrypted) return false;   // (WebM's own encryption is not handled)
    if (info.id == 0 || init->tracks.count() >= 8) return false;
    track.kind = info.kind;
    if (!tracks_.push_back(track) || !init->tracks.push_back(info)) return false;
    return true;
  }

  bool parse_group(ConstByteView body, IMseSink& sink) {
    ConstByteView block;
    int64_t duration = -1;
    bool reference = false;
    uint64_t at = 0;
    while (at < body.size()) {
      uint32_t id, length;
      uint64_t size;
      bool unknown;
      if (ebml_header(body.subview(at), &id, &size, &unknown, &length) != 1 || unknown || size > body.size() - at - length) return false;
      const ConstByteView v = body.subview(at + length, size);
      if (id == kIdBlock) block = v;
      else if (id == kIdBlockDuration) duration = static_cast<int64_t>(ebml_uint(v));
      else if (id == kIdReferenceBlock) reference = true;
      at += length + size;
    }
    if (block.empty()) return true;
    return parse_block(block, false, !reference, duration, sink);
  }

  // A block: the track, a time relative to the cluster, flags, and one frame or several laced ones.
  bool parse_block(ConstByteView body, bool simple, bool key, int64_t duration_units, IMseSink& sink) {
    if (tracks_.count() == 0 || !have_cluster_) return false;   // (media before the tracks, or before the cluster's time)
    ByteReader r(body);
    uint64_t number;
    if (!ebml_vint(r, &number)) return false;
    const int16_t relative = static_cast<int16_t>(r.be16());
    const uint8_t flags = r.u8();
    if (!r.ok()) return false;
    uint32_t track_index = 0;
    bool found = false;
    for (uint32_t i = 0; i < tracks_.count(); ++i)
      if (tracks_[i].number == number) {
        track_index = i;
        found = true;
      }
    if (!found) return true;   // (a track that is not kept)
    Track& track = tracks_[track_index];
    if (simple) key = (flags & 0x80) != 0;
    if (track.kind == MseTrackKind::Audio) key = true;
    // the frames
    const uint32_t lacing = (flags >> 1) & 3;
    ConstByteView frames[256];
    uint32_t count = 1;
    if (lacing == 0) {
      frames[0] = r.rest();
    } else {
      count = static_cast<uint32_t>(r.u8()) + 1;
      uint64_t sizes[256];
      if (lacing == 1) {   // Xiph: sizes as runs of 255 and a smaller byte, all but the last
        for (uint32_t i = 0; i + 1 < count; ++i) {
          uint64_t total = 0;
          uint8_t b;
          do {
            b = r.u8();
            total += b;
          } while (b == 255 && r.ok());
          sizes[i] = total;
        }
      } else if (lacing == 3) {   // EBML: the first size, then differences
        uint64_t first;
        uint32_t n;
        if (!ebml_vint(r, &first, &n)) return false;
        sizes[0] = first;
        for (uint32_t i = 1; i + 1 < count; ++i) {
          uint64_t raw;
          if (!ebml_vint(r, &raw, &n)) return false;
          const int64_t bias = (int64_t{1} << (7 * n - 1)) - 1;
          const int64_t size = static_cast<int64_t>(sizes[i - 1]) + (static_cast<int64_t>(raw) - bias);
          if (size < 0) return false;
          sizes[i] = static_cast<uint64_t>(size);
        }
      }
      if (!r.ok()) return false;
      const uint64_t rest = r.remaining();
      if (lacing == 2) {   // fixed size
        if (rest % count != 0) return false;
        for (uint32_t i = 0; i + 1 < count; ++i) sizes[i] = rest / count;
      }
      uint64_t used = 0;
      for (uint32_t i = 0; i + 1 < count; ++i) {
        if (sizes[i] > rest - used) return false;
        used += sizes[i];
      }
      sizes[count - 1] = rest - used;
      for (uint32_t i = 0; i < count; ++i) {
        frames[i] = r.take(sizes[i]);
        if (!r.ok()) return false;
      }
    }
    // when it is: the cluster's time and the block's, in ticks of the timecode scale
    const int64_t ticks = cluster_timecode_ + relative;
    if (ticks > (int64_t{1} << 50) || ticks < -(int64_t{1} << 50)) return false;
    const int64_t pts = ticks * static_cast<int64_t>(scale_ns_) / 1000;
    int64_t duration = duration_units >= 0 ? duration_units * static_cast<int64_t>(scale_ns_) / 1000 / count : track.default_duration_us;
    if (duration <= 0) duration = track.last_duration_us > 0 ? track.last_duration_us : (track.kind == MseTrackKind::Audio ? 20000 : 33333);
    track.last_duration_us = duration;
    for (uint32_t i = 0; i < count; ++i) {
      const int64_t t = pts + static_cast<int64_t>(i) * duration;
      if (!sink.frame(track_index, t, t, duration, key, frames[i], nullptr)) return false;
    }
    return true;
  }

  allocators::IAllocator& allocator_;
  ByteBuffer pending_;
  uint64_t consumed_ = 0, skip_ = 0;
  List<Track> tracks_;
  uint64_t scale_ns_ = 1000000;
  int64_t duration_us_ = -1;
  int64_t cluster_timecode_ = 0;
  bool have_cluster_ = false;
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
      info.container = kContainerRaw;
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
    return sink.frame(0, pts, pts, end - pts, true, data, nullptr);
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
    const uint32_t length = (uint32_t(d[3] & 3) << 11) | (uint32_t{d[4]} << 3) | (d[5] >> 5);
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
  if (format == MseFormat::Webm) {
    if (codec.equals_ignore_case("vp9") || starts_with_ci(codec, "vp09.")) {   // (VP8 and AV1 have no decoder on this machine)
      if (!is_video_type) return false;
      *video = true;
      return true;
    }
    return codec.equals_ignore_case("opus") || codec.equals_ignore_case("vorbis");
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
  } else if (main.equals_ignore_case("video/webm")) {
    f = MseFormat::Webm;
    video_type = true;
  } else if (main.equals_ignore_case("audio/webm")) {
    f = MseFormat::Webm;
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
    case MseFormat::Webm: return UniquePointer<IMseParser>::make_as<WebmParser>(allocator, allocator);
  }
  return UniquePointer<IMseParser>();
}

}  // namespace media
