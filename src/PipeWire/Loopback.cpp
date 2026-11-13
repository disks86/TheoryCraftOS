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

#include "PipeWire/Loopback.hpp"

namespace pipewire {

namespace {
constexpr uint32_t kRate = 48000;
constexpr uint64_t kMaxMove = 64 * 1024;      // (bytes moved at most each pump)
}  // namespace

bool Loopback::open(uint32_t client, From from, uint32_t to, uint32_t channels, uint32_t latency_ms, bool swap, StringView name) {
  close();
  if (channels < 1 || channels > 2) return false;
  if (from.monitor && from.sink == to) return false;      // (its own monitor into itself: it would feed itself)
  if (latency_ms == 0) latency_ms = kDefaultLatencyMs;
  if (latency_ms > kMaxLatencyMs) latency_ms = kMaxLatencyMs;
  channels_ = channels;
  swap_ = swap && channels == 2;
  latency_bytes_ = uint64_t{kRate} * latency_ms / 1000 * 2 * channels;
  const PcmFormat format{SampleFormat::S16Le, kRate, channels};
  Properties props(scratch_.allocator());
  props.set("media.name", name);
  props.set("node.name", name);
  props.set("application.name", "Loopback");
  record_node_ = graph_.create_record(client, format, props, &record_, from.monitor, from.monitor ? from.sink : 0);
  if (!record_node_ || !record_) {
    close();
    return false;
  }
  play_node_ = graph_.create_playback(client, format, props, &play_, to);
  if (!play_node_ || !play_) {
    close();
    return false;
  }
  return true;
}

void Loopback::close() {
  if (record_node_) graph_.destroy_node(record_node_);
  if (play_node_) graph_.destroy_node(play_node_);
  record_node_ = play_node_ = 0;
  record_ = SharedPointer<RecordStream>();
  play_ = SharedPointer<PlaybackStream>();
}

uint64_t Loopback::pump() {
  if (!record_ || !play_ || record_->closed()) return 0;
  const uint64_t frame = 2ull * channels_;
  uint64_t available = record_->available_bytes();
  // (more recorded than the latency holds - the playback could not keep up - is let go, the newest kept: the delay stays bounded)
  while (available > 2 * latency_bytes_) {
    const uint64_t drop = available - latency_bytes_;
    const uint64_t n = (drop < kMaxMove ? drop : kMaxMove) / frame * frame;
    if (n == 0) break;
    scratch_.clear();
    if (!scratch_.put_zeros(n)) return 0;
    if (record_->read(scratch_.writable_view()) == 0) break;
    available = record_->available_bytes();
  }
  const uint64_t waiting = play_->waiting_bytes();
  if (waiting >= latency_bytes_) return 0;
  uint64_t n = latency_bytes_ - waiting;
  if (n > available) n = available;
  if (n > kMaxMove) n = kMaxMove;
  n = n / frame * frame;
  if (n == 0) return 0;
  scratch_.clear();
  if (!scratch_.put_zeros(n)) return 0;
  ByteView view = scratch_.writable_view();
  const uint32_t got = record_->read(view);
  if (swap_)
    for (uint64_t at = 0; at + 4 <= got; at += 4) {      // (left and right exchanged: a frame is two 16-bit samples)
      const uint8_t l0 = view[at], l1 = view[at + 1];
      view[at] = view[at + 2];
      view[at + 1] = view[at + 3];
      view[at + 2] = l0;
      view[at + 3] = l1;
    }
  const uint32_t taken = got ? play_->write(view.first(got)) : 0;
  moved_ += taken;
  return taken;
}

}  // namespace pipewire
