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

#include "Midi/MidiFile.hpp"

namespace midi {

namespace {

struct Raw {
  uint64_t tick = 0;
  uint8_t status = 0, data1 = 0, data2 = 0;
  uint32_t tempo = 0;   // for status 0xFF: microseconds per quarter note
};

uint32_t be32(ConstByteView d, uint64_t at) { return (uint32_t{d[at]} << 24) | (uint32_t{d[at + 1]} << 16) | (uint32_t{d[at + 2]} << 8) | d[at + 3]; }
uint16_t be16(ConstByteView d, uint64_t at) { return static_cast<uint16_t>((d[at] << 8) | d[at + 1]); }
bool tag_is(ConstByteView d, uint64_t at, const char* tag) {
  if (at + 4 > d.size()) return false;
  for (int i = 0; i < 4; ++i)
    if (d[at + i] != static_cast<uint8_t>(tag[i])) return false;
  return true;
}

// A variable length quantity; false at the end of the data or when it is longer than four bytes.
bool read_varlen(ConstByteView d, uint64_t* at, uint64_t end, uint32_t* out) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    if (*at >= end) return false;
    const uint8_t b = d[(*at)++];
    value = (value << 7) | (b & 0x7F);
    if (!(b & 0x80)) {
      *out = value;
      return true;
    }
  }
  return false;
}

// One track chunk (d[begin, end)): its channel messages and tempo changes, in the order they are in the file. Returns the track's length
// in ticks through *length. A damaged track keeps what was read before the damage.
bool read_track(allocators::IAllocator& allocator, ConstByteView d, uint64_t begin, uint64_t end, List<Raw>* out, uint64_t* length) {
  uint64_t at = begin, tick = 0;
  uint8_t running = 0;
  while (at < end) {
    uint32_t delta;
    if (!read_varlen(d, &at, end, &delta)) break;
    tick += delta;
    if (at >= end) break;
    uint8_t status = d[at];
    if (status < 0x80) {   // running status
      if (!running) break;
      status = running;
    } else {
      ++at;
      if (status < 0xF0) running = status;
    }
    if (status >= 0xF0) {
      running = 0;
      if (status == 0xFF) {   // meta event
        if (at >= end) break;
        const uint8_t type = d[at++];
        uint32_t len;
        if (!read_varlen(d, &at, end, &len) || at + len > end) break;
        if (type == 0x51 && len == 3) {
          Raw r;
          r.tick = tick;
          r.status = 0xFF;
          r.tempo = (uint32_t{d[at]} << 16) | (uint32_t{d[at + 1]} << 8) | d[at + 2];
          if (r.tempo && !out->push_back(r)) return false;
        }
        at += len;
        if (type == 0x2F) break;   // end of track
      } else if (status == 0xF0 || status == 0xF7) {   // system exclusive: skipped
        uint32_t len;
        if (!read_varlen(d, &at, end, &len) || at + len > end) break;
        at += len;
      } else {   // other system messages have no length here: stop reading this track
        break;
      }
      continue;
    }
    const uint8_t kind = status & 0xF0;
    const bool two = kind != 0xC0 && kind != 0xD0;
    if (at + (two ? 2 : 1) > end) break;
    Raw r;
    r.tick = tick;
    r.status = status;
    r.data1 = d[at++] & 0x7F;
    r.data2 = two ? (d[at++] & 0x7F) : 0;
    if (!out->push_back(r)) return false;
  }
  (void)allocator;
  *length = tick;
  return true;
}

}  // namespace

bool looks_like_midi(ConstByteView d) {
  if (tag_is(d, 0, "MThd")) return true;
  return tag_is(d, 0, "RIFF") && tag_is(d, 8, "RMID");
}

bool Sequence::parse(ConstByteView file) {
  events_.clear();
  format_ = tracks_ = 0;
  uint64_t start = 0;
  if (tag_is(file, 0, "RIFF") && tag_is(file, 8, "RMID")) {   // a RIFF wrapper: the file is the "data" chunk
    uint64_t at = 12;
    bool found = false;
    while (at + 8 <= file.size()) {
      const uint32_t size = static_cast<uint32_t>(file[at + 4] | (file[at + 5] << 8) | (file[at + 6] << 16) | (uint32_t{file[at + 7]} << 24));
      if (tag_is(file, at, "data")) {
        start = at + 8;
        found = true;
        break;
      }
      at += 8 + uint64_t{size} + (size & 1);
    }
    if (!found) return false;
  }
  const ConstByteView d = file.subview(start, file.size() - start);
  if (d.size() < 14 || !tag_is(d, 0, "MThd")) return false;
  const uint32_t header_length = be32(d, 4);
  if (header_length < 6 || 8 + uint64_t{header_length} > d.size()) return false;
  const uint16_t format = be16(d, 8), track_count = be16(d, 10), division = be16(d, 12);
  if (format > 2 || division == 0 || track_count == 0 || track_count > kMaxTracks) return false;
  // Read the tracks one after the other into one list, remembering where each one starts.
  List<Raw> raw(allocator_);
  List<uint64_t> starts(allocator_);
  uint64_t offset = 0;   // format 2: the songs follow each other
  uint64_t position = 8 + uint64_t{header_length};
  while (position + 8 <= d.size() && starts.count() < track_count) {
    const uint32_t length = be32(d, position + 4);
    const uint64_t body = position + 8, end = body + length < d.size() ? body + length : d.size();
    position = body + length;
    if (!tag_is(d, position - length - 8, "MTrk")) continue;
    const uint64_t first = raw.count();
    uint64_t ticks = 0;
    if (!starts.push_back(first) || !read_track(allocator_, d, body, end, &raw, &ticks)) return false;
    if (offset)
      for (uint64_t i = first; i < raw.count(); ++i) raw[i].tick += offset;
    if (format == 2) offset += ticks;
    if (raw.count() > kMaxEvents) return false;
  }
  if (starts.count() == 0) return false;
  if (!starts.push_back(raw.count())) return false;
  format_ = format;
  tracks_ = static_cast<uint16_t>(starts.count() - 1);

  // Merge the tracks by time (the track with the lower number first for the same tick) and turn ticks into microseconds.
  const bool smpte = (division & 0x8000) != 0;
  uint32_t fps_thousandths = 30000, ticks_per_frame = 1;
  if (smpte) {
    const int fps = 256 - (division >> 8);
    fps_thousandths = fps == 29 ? 29970 : static_cast<uint32_t>(fps) * 1000;
    ticks_per_frame = division & 0xFF;
    if (fps_thousandths == 0 || ticks_per_frame == 0) return false;
  }
  List<uint64_t> next(allocator_);
  for (uint64_t t = 0; t + 1 < starts.count(); ++t)
    if (!next.push_back(starts[t])) return false;
  uint64_t segment_tick = 0, segment_us = 0;
  uint32_t tempo = 500000;
  auto time_of = [&](uint64_t tick) -> uint64_t {
    if (smpte) return tick * 1000000000ull / (uint64_t{fps_thousandths} * ticks_per_frame);
    return segment_us + (tick - segment_tick) * tempo / division;
  };
  for (;;) {
    int64_t best = -1;
    for (uint64_t t = 0; t < next.count(); ++t) {
      if (next[t] >= starts[t + 1]) continue;
      if (best < 0 || raw[next[t]].tick < raw[next[static_cast<uint64_t>(best)]].tick) best = static_cast<int64_t>(t);
    }
    if (best < 0) break;
    const Raw& r = raw[next[static_cast<uint64_t>(best)]++];
    if (r.status == 0xFF) {   // a tempo change (ignored with SMPTE timing)
      if (!smpte) {
        segment_us = time_of(r.tick);
        segment_tick = r.tick;
        tempo = r.tempo;
      }
      continue;
    }
    Event e;
    e.time_us = time_of(r.tick);
    e.status = r.status;
    e.data1 = r.data1;
    e.data2 = r.data2;
    if (events_.count() >= kMaxEvents || !events_.push_back(e)) return false;
  }
  return events_.count() > 0;
}

}  // namespace midi
