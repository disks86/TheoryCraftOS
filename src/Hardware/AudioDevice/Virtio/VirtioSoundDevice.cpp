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

#include "Hardware/AudioDevice/Virtio/VirtioSoundDevice.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// Control requests and their status codes.
constexpr uint32_t kPcmInfo = 0x0100, kPcmSetParams = 0x0101, kPcmPrepare = 0x0102, kPcmRelease = 0x0103,
                   kPcmStart = 0x0104, kPcmStop = 0x0105;
constexpr uint32_t kStatusOk = 0x8000;
constexpr uint32_t kDirectionOutput = 0, kDirectionInput = 1;
constexpr uint32_t kFormatS16 = 5, kRate48k = 7;  // codes of virtio_snd_pcm_fmt / _rate
constexpr uint32_t kInfoSize = 32;
constexpr uint32_t kMaxStreamsQueried = 16;
constexpr uint32_t kConfigStreams = 4;  // jacks, then streams, then chmaps

constexpr uint32_t kResponseOffset = 2048;
constexpr uint32_t kControlTimeoutUs = 1000000;

// Layout of a transmit slot: header (stream id), data, status (result, latency).
constexpr uint32_t kSlotHeader = 0, kSlotStatus = 16, kSlotData = 64;
constexpr uint32_t kSlotStride = kSlotData + VirtioSoundDevice::kPeriodBytes;

void put32(ByteView at, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) at[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t get32(ConstByteView at) { return at[0] | (at[1] << 8) | (at[2] << 16) | (uint32_t{at[3]} << 24); }
uint64_t get64(ConstByteView at) { return get32(at) | (uint64_t{get32(at.subview(4))} << 32); }
}  // namespace

ByteView VirtioSoundDevice::slot_data(uint32_t slot) const {
  return slots_.subview(uint64_t{slot} * kSlotStride + kSlotData, kPeriodBytes);
}

// Sends the request in the control buffer and waits for the device's answer in the response area.
bool VirtioSoundDevice::control(uint32_t request_bytes, uint32_t response_bytes) {
  Virtqueue& queue = queues_[kControlQueue];
  const Virtqueue::Segment chain[2] = {{control_buffer_.data(), request_bytes, false},
                                       {control_buffer_.data() + kResponseOffset, response_bytes, true}};
  queue.post_chain(0, chain, 2);
  transport_.notify(kControlQueue);
  uint16_t id;
  uint32_t written;
  for (uint32_t waited = 0; waited < kControlTimeoutUs; waited += 50) {
    if (queue.take_used(&id, &written)) return get32(ConstByteView(control_buffer_).subview(kResponseOffset)) == kStatusOk;
    hardware::delay_us(50);
  }
  return false;
}

// A request that is just a header and a stream id.
bool VirtioSoundDevice::simple_request(uint32_t code, uint32_t stream) {
  put32(control_buffer_, code);
  put32(control_buffer_.subview(4), stream);
  return control(8, 4);
}

// Picks the first output stream (and the first input stream, if any) that can do 48 kHz, 16-bit, two-channel audio.
bool VirtioSoundDevice::choose_stream() {
  uint32_t streams = 0;
  for (uint32_t i = 0; i < 4; ++i) streams |= uint32_t{transport_.read_config8(kConfigStreams + i)} << (8 * i);
  if (streams == 0) return false;
  if (streams > kMaxStreamsQueried) streams = kMaxStreamsQueried;
  put32(control_buffer_, kPcmInfo);
  put32(control_buffer_.subview(4), 0);        // first stream
  put32(control_buffer_.subview(8), streams);  // how many
  put32(control_buffer_.subview(12), kInfoSize);
  if (!control(16, 4 + streams * kInfoSize)) return false;
  bool have_output = false;
  for (uint32_t s = 0; s < streams; ++s) {
    const ConstByteView info = ConstByteView(control_buffer_).subview(kResponseOffset + 4 + uint64_t{s} * kInfoSize, kInfoSize);
    const uint64_t formats = get64(info.subview(8)), rates = get64(info.subview(16));
    const uint8_t direction = info[24], channels_min = info[25], channels_max = info[26];
    if (!((formats >> kFormatS16 & 1) && (rates >> kRate48k & 1) && channels_min <= 2 && channels_max >= 2)) continue;
    if (direction == kDirectionOutput && !have_output) {
      stream_id_ = s;
      have_output = true;
    } else if (direction == kDirectionInput && !has_capture_stream_) {
      capture_stream_id_ = s;
      has_capture_stream_ = true;
    }
  }
  return have_output;
}

bool VirtioSoundDevice::initialize() {
  if (!transport_.initialize()) {
    transport_.shutdown();
    return false;
  }
  control_buffer_ = allocate_view<uint8_t>(allocator_, 4096, 4096);
  slots_ = allocate_view<uint8_t>(allocator_, uint64_t{kMaxPeriods} * kSlotStride, 4096);
  bool queues_ok = !control_buffer_.empty() && !slots_.empty();
  for (uint16_t q = 0; q < 4 && queues_ok; ++q) queues_ok = transport_.setup_queue(q, &queues_[q]);
  if (!queues_ok || queues_[kControlQueue].size() < 2) {
    shutdown();
    return false;
  }
  periods_ = queues_[kTransmitQueue].size() / 3;  // three descriptors per period
  if (periods_ > kMaxPeriods) periods_ = kMaxPeriods;
  if (periods_ < 2 || !transport_.start()) {
    shutdown();
    return false;
  }
  started_ = true;

  const uint32_t buffer_bytes = periods_ * kPeriodBytes;
  if (!choose_stream()) {
    shutdown();
    return false;
  }
  put32(control_buffer_, kPcmSetParams);
  put32(control_buffer_.subview(4), stream_id_);
  put32(control_buffer_.subview(8), buffer_bytes);
  put32(control_buffer_.subview(12), kPeriodBytes);
  put32(control_buffer_.subview(16), 0);  // features
  control_buffer_[20] = kAudioChannels;
  control_buffer_[21] = kFormatS16;
  control_buffer_[22] = kRate48k;
  control_buffer_[23] = 0;
  if (!control(24, 4) || !simple_request(kPcmPrepare, stream_id_) || !simple_request(kPcmStart, stream_id_)) {
    shutdown();
    return false;
  }
  capture_.start();  // the input is optional
  return true;
}

void VirtioSoundDevice::shutdown() {
  capture_.stop();
  if (started_) {
    simple_request(kPcmStop, stream_id_);
    simple_request(kPcmRelease, stream_id_);
  }
  started_ = false;
  transport_.shutdown();  // reset first: the device must stop using our memory
  for (Virtqueue& queue : queues_) queue.release(allocator_);
  release_view(allocator_, control_buffer_);
  release_view(allocator_, slots_);
  busy_mask_ = 0;
  in_flight_bytes_ = 0;
  filling_ = false;
  current_bytes_ = 0;
}

// Takes back the periods the device has finished playing.
void VirtioSoundDevice::reclaim() {
  uint16_t id;
  uint32_t written;
  bool any = false;
  while (queues_[kTransmitQueue].take_used(&id, &written)) {
    const uint32_t slot = id / 3;
    if (id % 3 != 0 || slot >= periods_ || !(busy_mask_ & (1u << slot))) continue;  // not one of ours
    busy_mask_ &= ~(1u << slot);
    in_flight_bytes_ -= slot_bytes_[slot];
    stats_.frames_played += slot_bytes_[slot] / kAudioFrameSize;
    slot_bytes_[slot] = 0;
    any = true;
  }
  if (any && busy_mask_ == 0 && !filling_) ++stats_.underruns;
  if (filling_ && busy_mask_ == 0) submit_current();  // nothing playing: a part-filled period must not wait for the rest
}

// Hands the slot being filled to the device.
void VirtioSoundDevice::submit_current() {
  const uint32_t slot = current_slot_;
  const ByteView base = slots_.subview(uint64_t{slot} * kSlotStride, kSlotStride);
  put32(base.subview(kSlotHeader), stream_id_);
  base.subview(kSlotStatus, 8).fill(0);
  const Virtqueue::Segment chain[3] = {{base.data() + kSlotHeader, 4, false},
                                       {base.data() + kSlotData, current_bytes_, false},
                                       {base.data() + kSlotStatus, 8, true}};
  busy_mask_ |= 1u << slot;
  slot_bytes_[slot] = current_bytes_;
  in_flight_bytes_ += current_bytes_;
  filling_ = false;
  current_bytes_ = 0;
  queues_[kTransmitQueue].post_chain(static_cast<uint16_t>(slot * 3), chain, 3);
  transport_.notify(kTransmitQueue);
}

uint32_t VirtioSoundDevice::write(ConstByteView pcm) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) {
    ++stats_.write_failures;
    return 0;
  }
  reclaim();
  const uint32_t whole = static_cast<uint32_t>(pcm.size() - pcm.size() % kAudioFrameSize);
  uint32_t taken = 0;
  while (taken < whole) {
    if (!filling_) {
      if (busy_mask_ & (1u << next_slot_)) break;  // every period is still with the device
      current_slot_ = next_slot_;
      next_slot_ = (next_slot_ + 1) % periods_;
      current_bytes_ = 0;
      filling_ = true;
    }
    const uint32_t chunk = whole - taken < kPeriodBytes - current_bytes_ ? whole - taken : kPeriodBytes - current_bytes_;
    const ByteView out = slot_data(current_slot_).subview(current_bytes_, chunk);
    const bool silent = muted_ || volume_ == 0;
    for (uint32_t i = 0; i < chunk; i += kAudioBytesPerSample) {
      const int32_t sample = static_cast<int16_t>(pcm[taken + i] | (pcm[taken + i + 1] << 8));
      const int32_t scaled = silent ? 0 : sample * volume_ / kAudioMaxVolume;
      out[i] = static_cast<uint8_t>(scaled);
      out[i + 1] = static_cast<uint8_t>(scaled >> 8);
    }
    taken += chunk;
    current_bytes_ += chunk;
    if (current_bytes_ == kPeriodBytes) submit_current();
  }
  if (filling_ && busy_mask_ == 0) submit_current();  // nothing playing: do not make this wait for a full period
  if (taken == 0 && whole > 0) ++stats_.write_failures;
  stats_.frames_queued += taken / kAudioFrameSize;
  return taken;
}

uint32_t VirtioSoundDevice::queued_bytes() {
  kernel::ScopedMutex guard(mutex_);
  if (started_) reclaim();
  return in_flight_bytes_ + (filling_ ? current_bytes_ : 0);
}

uint32_t VirtioSoundDevice::free_bytes() {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return 0;
  reclaim();
  uint32_t free_periods = 0;
  for (uint32_t i = 0; i < periods_; ++i)
    if (!(busy_mask_ & (1u << i)) && !(filling_ && i == current_slot_)) ++free_periods;
  return free_periods * kPeriodBytes + (filling_ ? kPeriodBytes - current_bytes_ : 0);
}

AudioStatistics VirtioSoundDevice::statistics() {
  kernel::ScopedMutex guard(mutex_);
  if (started_) reclaim();
  return stats_;
}

ByteView VirtioSoundDevice::Capture::slot_base(uint32_t slot) const {
  return slots_.subview(uint64_t{slot} * kSlotStride, kSlotStride);
}

// Offers one empty period buffer to the device: stream id in, then room for the audio and the status.
void VirtioSoundDevice::Capture::post(uint32_t slot) {
  const ByteView base = slot_base(slot);
  put32(base.subview(kSlotHeader), owner_.capture_stream_id_);
  base.subview(kSlotStatus, 8).fill(0);
  const Virtqueue::Segment chain[3] = {{base.data() + kSlotHeader, 4, false},
                                       {base.data() + kSlotData, kPeriodBytes, true},
                                       {base.data() + kSlotStatus, 8, true}};
  owner_.queues_[kReceiveQueue].post_chain(static_cast<uint16_t>(slot * 3), chain, 3);
}

bool VirtioSoundDevice::Capture::start() {
  VirtioSoundDevice& d = owner_;
  if (!d.has_capture_stream_ || !d.started_) return false;
  periods_ = d.queues_[kReceiveQueue].size() / 3;
  if (periods_ > kMaxPeriods) periods_ = kMaxPeriods;
  slots_ = allocate_view<uint8_t>(d.allocator_, uint64_t{kMaxPeriods} * kSlotStride, 4096);
  if (periods_ < 2 || slots_.empty() || !ring_.allocate(d.allocator_, kRingBytes)) {
    stop();
    return false;
  }
  put32(d.control_buffer_, kPcmSetParams);
  put32(d.control_buffer_.subview(4), d.capture_stream_id_);
  put32(d.control_buffer_.subview(8), periods_ * kPeriodBytes);
  put32(d.control_buffer_.subview(12), kPeriodBytes);
  put32(d.control_buffer_.subview(16), 0);
  d.control_buffer_[20] = kAudioChannels;
  d.control_buffer_[21] = kFormatS16;
  d.control_buffer_[22] = kRate48k;
  d.control_buffer_[23] = 0;
  if (!d.control(24, 4) || !d.simple_request(kPcmPrepare, d.capture_stream_id_)) {
    stop();
    return false;
  }
  for (uint32_t slot = 0; slot < periods_; ++slot) post(slot);
  if (!d.simple_request(kPcmStart, d.capture_stream_id_)) {
    stop();
    return false;
  }
  stream_started_ = true;
  d.transport_.notify(kReceiveQueue);
  running_ = true;
  return true;
}

void VirtioSoundDevice::Capture::stop() {
  VirtioSoundDevice& d = owner_;
  running_ = false;
  if (stream_started_ && d.started_) {
    d.simple_request(kPcmStop, d.capture_stream_id_);
    d.simple_request(kPcmRelease, d.capture_stream_id_);
  }
  stream_started_ = false;
  ring_.release(d.allocator_);
  release_view(d.allocator_, slots_);
}

// Collects the periods the device has filled and hands the buffers back.
void VirtioSoundDevice::Capture::refresh() {
  if (!running_) return;
  uint16_t id;
  uint32_t written;
  bool any = false;
  while (owner_.queues_[kReceiveQueue].take_used(&id, &written)) {
    const uint32_t slot = id / 3;
    if (id % 3 != 0 || slot >= periods_) continue;  // not one of ours
    const uint32_t bytes = written > 8 ? written - 8 : 0;  // the status takes the last 8
    if (bytes > 0 && bytes <= kPeriodBytes) ring_.push(ConstByteView(slot_base(slot)).subview(kSlotData, bytes));
    post(slot);
    any = true;
  }
  if (any) owner_.transport_.notify(kReceiveQueue);
}

}  // namespace hardware
