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
  latency_bytes_ = uint64_t{kRate} * latency_ms / 1000 * 4;      // (of the playback, always stereo)
  const PcmFormat format{SampleFormat::S16Le, kRate, channels};
  // (played in stereo even when mono: the mixer pans a mono stream at equal power, 3 dB down on each side, and a mono sink's monitor would then hear
  // it 3 dB quieter than it was recorded)
  const PcmFormat play_format{SampleFormat::S16Le, kRate, 2};
  Properties props(scratch_.allocator());
  props.set("media.name", name);
  props.set("node.name", name);
  props.set("application.name", "Loopback");
  props.set("state.restore", "false");      // (the module's own streams: their volumes are not remembered)
  record_node_ = graph_.create_record(client, format, props, &record_, from.monitor, from.monitor ? from.sink : 0);
  if (!record_node_ || !record_) {
    close();
    return false;
  }
  play_node_ = graph_.create_playback(client, play_format, props, &play_, to);
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
  const uint64_t frame = 2ull * channels_;      // (recorded; played: 4)
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
  uint64_t n = (latency_bytes_ - waiting) / 4 * frame;      // (recorded bytes for that much played)
  if (n > available) n = available;
  if (n > kMaxMove) n = kMaxMove;
  n = n / frame * frame;
  if (n == 0) return 0;
  scratch_.clear();
  if (!scratch_.put_zeros(channels_ == 1 ? 2 * n : n)) return 0;      // (room to make mono stereo in place)
  ByteView view = scratch_.writable_view();
  const uint32_t got = record_->read(view.first(n)) / frame * frame;
  if (channels_ == 1)
    for (uint64_t at = got; at >= 2; at -= 2) {      // (backwards: each sample becomes a frame of two, the later ones moved first)
      const uint8_t lo = view[at - 2], hi = view[at - 1];
      view[2 * at - 4] = view[2 * at - 2] = lo;
      view[2 * at - 3] = view[2 * at - 1] = hi;
    }
  const uint32_t played = channels_ == 1 ? 2 * got : got;
  if (swap_)
    for (uint64_t at = 0; at + 4 <= got; at += 4) {      // (left and right exchanged: a frame is two 16-bit samples)
      const uint8_t l0 = view[at], l1 = view[at + 1];
      view[at] = view[at + 2];
      view[at + 1] = view[at + 3];
      view[at + 2] = l0;
      view[at + 3] = l1;
    }
  const uint32_t taken = played ? play_->write(view.first(played)) : 0;
  moved_ += taken;
  return taken;
}

}  // namespace pipewire

namespace pipewire {

namespace {
constexpr uint32_t kEchoRate = 48000;
constexpr uint32_t kRingSamples = kEchoRate;      // (a second of each end waiting at most)

float sample_of(ConstByteView pcm, uint64_t at) { return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(pcm[at] | (pcm[at + 1] << 8)))) / 32768.0f; }

void put_sample(ByteView pcm, uint64_t at, float v) {
  float scaled = v * 32768.0f;
  if (!(scaled == scaled)) scaled = 0.0f;
  const int32_t s = scaled >= 32767.0f ? 32767 : scaled <= -32768.0f ? -32768 : static_cast<int32_t>(scaled);
  pcm[at] = static_cast<uint8_t>(s & 0xFF);
  pcm[at + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
}
}  // namespace

bool EchoCancelLoop::Ring::size(uint64_t capacity) {
  data.clear();
  head = count = 0;
  if (!data.reserve(capacity)) return false;
  for (uint64_t i = 0; i < capacity; ++i)
    if (!data.push_back(0.0f)) return false;
  return true;
}

void EchoCancelLoop::Ring::push(float v) {
  const uint64_t n = data.count();
  if (n == 0) return;
  if (count == n) {      // (full: the oldest goes)
    head = (head + 1) % n;
    --count;
  }
  data[(head + count) % n] = v;
  ++count;
}

float EchoCancelLoop::Ring::pop() {
  if (count == 0) return 0.0f;
  const float v = data[head];
  head = (head + 1) % data.count();
  --count;
  return v;
}

EchoCancelLoop::EchoCancelLoop(Graph& graph, allocators::IAllocator& allocator)
    : graph_(graph), allocator_(allocator), aec_(allocator), scratch_(allocator), far_ring_(allocator), mic_ring_(allocator) {}

bool EchoCancelLoop::open(uint32_t client, Loopback::From mic, uint32_t master, uint32_t sink, uint32_t source, uint32_t channels, bool passthrough,
                          const EchoCanceller::Options& options, uint32_t partitions) {
  close();
  if (channels < 1 || channels > 2 || !sink || !source) return false;
  if (mic.monitor && (mic.sink == sink || mic.sink == source)) return false;      // (it would hear itself)
  if (master == sink || master == source) return false;
  channels_ = channels;
  passthrough_ = passthrough;
  if (!aec_.initialize(partitions, kEchoRate / 2) || !far_ring_.size(kRingSamples) || !mic_ring_.size(kRingSamples)) return false;
  aec_.set_options(options);
  idle_samples_ = far_samples_ = near_samples_ = 0;
  Properties props(allocator_);
  props.set("application.name", "Echo-Cancel");
  props.set("state.restore", "false");
  // the far end: the echo-cancel sink's monitor, on into the sink master
  props.set("media.name", "echo-cancel playback");
  props.set("node.name", "echo-cancel.playback");
  const PcmFormat stereo{SampleFormat::S16Le, kEchoRate, 2}, mono{SampleFormat::S16Le, kEchoRate, 1};
  far_record_node_ = graph_.create_record(client, stereo, props, &far_record_, true, sink);
  far_play_node_ = far_record_node_ ? graph_.create_playback(client, stereo, props, &far_play_, master) : 0;
  // the near end: the source master, through the canceller into the echo-cancel source
  props.set("media.name", "echo-cancel capture");
  props.set("node.name", "echo-cancel.capture");
  mic_record_node_ = far_play_node_ ? graph_.create_record(client, mono, props, &mic_record_, mic.monitor, mic.monitor ? mic.sink : 0) : 0;
  // (in stereo whatever the source's channels: a mono stream would be panned 3 dB down, see Loopback::open)
  out_play_node_ = mic_record_node_ ? graph_.create_playback(client, stereo, props, &out_play_, source) : 0;
  if (!out_play_node_ || !far_record_ || !far_play_ || !mic_record_ || !out_play_) {
    close();
    return false;
  }
  return true;
}

void EchoCancelLoop::close() {
  uint32_t* const nodes[] = {&far_record_node_, &far_play_node_, &mic_record_node_, &out_play_node_};
  for (uint32_t* node : nodes) {
    if (*node) graph_.destroy_node(*node);
    *node = 0;
  }
  far_record_ = mic_record_ = SharedPointer<RecordStream>();
  far_play_ = out_play_ = SharedPointer<PlaybackStream>();
  far_ring_.clear();
  mic_ring_.clear();
}

uint64_t EchoCancelLoop::pump() {
  if (!far_record_ || !mic_record_ || !far_play_ || !out_play_ || far_record_->closed() || mic_record_->closed()) return 0;
  const uint64_t latency = uint64_t{kEchoRate} * kLatencyMs / 1000;      // (frames)
  // 1. the far end: on to the sink master, and its mono mix (what the speakers get) to the canceller
  bool far_arrived = false;
  for (int rounds = 0; rounds < 8; ++rounds) {
    uint64_t frames = far_record_->available_bytes() / 4;
    if (frames == 0) break;
    if (frames > 4096) frames = 4096;
    scratch_.clear();
    if (!scratch_.put_zeros(frames * 4)) return 0;
    ByteView view = scratch_.writable_view();
    const uint32_t got = far_record_->read(view) / 4 * 4;
    if (got == 0) break;
    // (a sink master that cannot take it all loses the rest, as a loopback does: the far end stays real time)
    const uint64_t waiting = far_play_->waiting_bytes() / 4;
    uint64_t room = waiting < 2 * latency ? 2 * latency - waiting : 0;
    if (room > got / 4) room = got / 4;
    if (room) far_play_->write(view.first(room * 4));
    for (uint64_t at = 0; at < got; at += 4) far_ring_.push(0.5f * (sample_of(view, at) + sample_of(view, at + 2)));
    far_samples_ += got / 4;
    far_arrived = true;
    idle_samples_ = 0;
  }
  // 2. the near end, in blocks paired with the far end's
  uint64_t moved = 0;
  for (int rounds = 0; rounds < 8; ++rounds) {
    uint64_t frames = mic_record_->available_bytes() / 2;
    if (frames == 0) break;
    if (frames > 4096) frames = 4096;
    scratch_.clear();
    if (!scratch_.put_zeros(frames * 2)) return moved;
    ByteView view = scratch_.writable_view();
    const uint32_t got = mic_record_->read(view) / 2 * 2;
    if (got == 0) break;
    for (uint64_t at = 0; at < got; at += 2) mic_ring_.push(sample_of(view, at));
    near_samples_ += got / 2;
    if (!far_arrived) idle_samples_ += got / 2;
  }
  constexpr uint32_t kBlock = EchoCanceller::kBlock;
  float far[kBlock], near[kBlock], out[kBlock];
  while (mic_ring_.count >= kBlock) {
    // (the far end not here yet: wait for it a little; after kIdleMs of microphone without it, nothing is playing into the sink: silence)
    if (far_ring_.count < kBlock && idle_samples_ < uint64_t{kEchoRate} * kIdleMs / 1000) break;
    for (uint32_t i = 0; i < kBlock; ++i) {
      near[i] = mic_ring_.pop();
      far[i] = far_ring_.count ? far_ring_.pop() : 0.0f;
    }
    if (passthrough_ || !aec_.process(ArrayView<const float>(far, kBlock), ArrayView<const float>(near, kBlock), ArrayView<float>(out, kBlock)))
      for (uint32_t i = 0; i < kBlock; ++i) out[i] = near[i];
    // into the echo-cancel source (what is queued there kept under twice the latency: a source nobody records from fills no further)
    const uint64_t frame = 4;
    if (out_play_->waiting_bytes() / frame < 2 * latency) {
      scratch_.clear();
      if (!scratch_.put_zeros(uint64_t{kBlock} * frame)) return moved;
      ByteView pcm = scratch_.writable_view();
      for (uint32_t i = 0; i < kBlock; ++i)
        for (uint32_t ch = 0; ch < 2; ++ch) put_sample(pcm, (uint64_t{i} * 2 + ch) * 2, out[i]);
      out_play_->write(pcm);
    }
    moved += kBlock;
  }
  return moved;
}

}  // namespace pipewire
