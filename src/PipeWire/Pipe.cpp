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


#include "PipeWire/Pipe.hpp"

namespace pipewire {

namespace {
constexpr uint64_t kMaxMove = 64 * 1024;      // (bytes moved at most each pump)
}  // namespace

bool pulse_format_named(StringView name, SampleFormat* out) {
  static const struct {
    const char* name;
    SampleFormat format;
  } kNames[] = {
      {"u8", SampleFormat::U8},           {"s16le", SampleFormat::S16Le},     {"s16be", SampleFormat::S16Be},     {"s16ne", SampleFormat::S16Le},
      {"s16", SampleFormat::S16Le},       {"s24le", SampleFormat::S24Le},     {"s24be", SampleFormat::S24Be},     {"s24ne", SampleFormat::S24Le},
      {"s24-32le", SampleFormat::S24_32Le}, {"s24-32ne", SampleFormat::S24_32Le}, {"s32le", SampleFormat::S32Le}, {"s32be", SampleFormat::S32Be},
      {"s32ne", SampleFormat::S32Le},     {"float32le", SampleFormat::F32Le}, {"float32be", SampleFormat::F32Be}, {"float32ne", SampleFormat::F32Le},
      {"float32", SampleFormat::F32Le},   {"alaw", SampleFormat::Alaw},       {"ulaw", SampleFormat::Ulaw},
  };
  for (const auto& n : kNames)
    if (name == StringView(n.name)) {
      *out = n.format;
      return true;
    }
  return false;
}

// --- PipeSink -----------------------------------------------------------------------------------------------------------------------------------------------

bool PipeSink::open(uint32_t client, uint32_t sink, const PcmFormat& format, UniquePointer<IPipeFile> file) {
  close();
  if (!file || !format.valid() || format.frame_size() == 0 || format.frame_size() > IPipeFiles::kAtomicBytes) return false;
  Properties props(pending_.allocator());
  props.set("media.name", "pipe-sink");
  props.set("node.name", "pipe-sink");
  props.set("application.name", "Pipe");
  props.set("state.restore", "false");
  record_node_ = graph_.create_record(client, format, props, &record_, true, sink);
  if (!record_node_ || !record_) {
    close();
    return false;
  }
  frame_ = format.frame_size();
  file_ = static_cast<UniquePointer<IPipeFile>&&>(file);
  written_ = dropped_ = 0;
  return true;
}

void PipeSink::close() {
  if (record_node_) graph_.destroy_node(record_node_);
  record_node_ = 0;
  record_ = SharedPointer<RecordStream>();
  file_ = UniquePointer<IPipeFile>();
  pending_.clear();
}

uint64_t PipeSink::pump() {
  if (!record_ || !file_) return 0;
  const uint32_t piece = IPipeFiles::kAtomicBytes / frame_ * frame_;      // (whole frames, written whole)
  uint64_t moved = 0;
  for (uint64_t rounds = 0; rounds < kMaxMove / piece + 1; ++rounds) {
    const uint64_t available = record_->available_bytes();
    if (available < frame_) break;
    const uint64_t n = available < piece ? available / frame_ * frame_ : piece;
    pending_.clear();
    if (!pending_.put_zeros(n)) break;
    const uint32_t got = record_->read(pending_.writable_view()) / frame_ * frame_;
    if (got == 0) break;
    const uint32_t wrote = file_->write(pending_.view().first(got));
    if (wrote == got) {
      written_ += wrote;
      moved += wrote;
    } else {
      dropped_ += got;      // (the pipe is full: nobody reads it, or not fast enough)
    }
  }
  return moved;
}

// --- PipeSource ---------------------------------------------------------------------------------------------------------------------------------------------

bool PipeSource::open(uint32_t client, uint32_t source, const PcmFormat& format, UniquePointer<IPipeFile> file, uint32_t latency_ms) {
  close();
  if (!file || !format.valid() || format.frame_size() == 0 || format.channels > 2) return false;
  if (latency_ms == 0) latency_ms = kDefaultLatencyMs;
  format_ = format;
  // (mono played as stereo: each sample twice)
  const PcmFormat play_format{format.format, format.rate, 2};
  latency_bytes_ = uint64_t{format.rate} * latency_ms / 1000 * play_format.frame_size();
  Properties props(scratch_.allocator());
  props.set("media.name", "pipe-source");
  props.set("node.name", "pipe-source");
  props.set("application.name", "Pipe");
  props.set("state.restore", "false");
  play_node_ = graph_.create_playback(client, play_format, props, &play_, source);
  if (!play_node_ || !play_) {
    close();
    return false;
  }
  file_ = static_cast<UniquePointer<IPipeFile>&&>(file);
  carry_ = 0;
  read_ = 0;
  return true;
}

void PipeSource::close() {
  if (play_node_) graph_.destroy_node(play_node_);
  play_node_ = 0;
  play_ = SharedPointer<PlaybackStream>();
  file_ = UniquePointer<IPipeFile>();
  scratch_.clear();
  carry_ = 0;
}

uint64_t PipeSource::pump() {
  if (!play_ || !file_) return 0;
  const uint32_t frame = format_.frame_size(), sample = format_.sample_size();
  const bool mono = format_.channels == 1;
  const uint64_t waiting = play_->waiting_bytes();
  if (waiting >= latency_bytes_) return 0;      // (enough queued: the rest waits in the pipe)
  uint64_t want = (latency_bytes_ - waiting) / (mono ? 2 : 1);      // (pipe bytes for that much played)
  if (want > kMaxMove) want = kMaxMove;
  want = want / frame * frame;
  if (want <= carry_) return 0;
  // scratch_: [carried part of a frame][what is read now], with room behind to make mono stereo in place
  const uint64_t total = mono ? 2 * want : want;
  if (scratch_.size() < total && !scratch_.put_zeros(total - scratch_.size())) return 0;
  ByteView view = scratch_.writable_view();
  const uint32_t got = file_->read(view.subview(carry_, want - carry_));
  if (got == 0) return 0;
  read_ += got;
  const uint64_t have = uint64_t{carry_} + got;
  const uint64_t whole = have / frame * frame;
  // the part of a frame left over is kept aside (making mono stereo writes over it) and goes to the front for the next read
  uint8_t tail[2 * 8];      // (less than a frame: at most two samples of 8 bytes)
  carry_ = static_cast<uint32_t>(have - whole);
  ByteView(tail, carry_).copy_from(ConstByteView(view.subview(whole, carry_)));
  uint64_t played = whole;
  if (mono) {
    for (uint64_t at = whole; at >= sample; at -= sample) {      // (backwards: each sample becomes a frame of two, the later ones moved first)
      const uint64_t from = at - sample, to = 2 * from;
      for (uint32_t b = sample; b > 0; --b) view[to + sample + b - 1] = view[to + b - 1] = view[from + b - 1];
    }
    played = 2 * whole;
  }
  if (played) play_->write(view.first(played));
  view.first(carry_).copy_from(ConstByteView(tail, carry_));
  return got;
}

}  // namespace pipewire
