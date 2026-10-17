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

#include "Hardware/AudioDevice/Usb/UsbAudioDevice.hpp"

namespace hardware {

namespace {

constexpr uint32_t kLeadMicroseconds = 24000;   // how far ahead of the controller the packets are queued
constexpr uint32_t kMaxFramesPerPacket = 256;
constexpr uint8_t kSetInterface = 11;

// Class request types: host to device / device to host, class, interface or endpoint
constexpr uint8_t kClassInterfaceOut = 0x21, kClassInterfaceIn = 0xA1, kClassEndpointOut = 0x22;
constexpr uint8_t kRequestSetCur = 0x01, kRequestGetCur = 0x01, kRequestRange = 0x02, kRequestGetMin = 0x82, kRequestGetMax = 0x83;
constexpr uint8_t kControlMute = 1, kControlVolume = 2;

bool usable(const UacFormat& f) {
  if (!f.pcm || !(f.rate_48000 || f.rate_unknown) || !f.endpoint || f.channels < 1 || f.channels > 8 || f.max_packet == 0) return false;
  return (f.subslot == 2 && f.bits == 16) || (f.subslot == 3 && f.bits == 24) || (f.subslot == 4 && (f.bits == 24 || f.bits == 32));
}

// Lower is better: 16 bits before more, stereo before mono before more channels.
uint32_t score(const UacFormat& f) { return (f.subslot == 2 ? 0 : 10) + (f.channels == 2 ? 0 : f.channels == 1 ? 1 : 2); }

int16_t le16s(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

// Stereo 16-bit frames to the device's format, with the gain (percent) applied.
void to_device(const uint8_t* pcm, uint32_t frames, const UacFormat& f, uint32_t gain, uint8_t* out) {
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t l = le16s(pcm + 4 * i), r = le16s(pcm + 4 * i + 2);
    if (gain < 100) {
      l = l * static_cast<int32_t>(gain) / 100;
      r = r * static_cast<int32_t>(gain) / 100;
    }
    const int32_t mono = (l + r) / 2;
    for (uint32_t ch = 0; ch < f.channels; ++ch) {
      const int32_t v = f.channels == 1 ? mono : ch == 0 ? l : ch == 1 ? r : 0;
      if (f.subslot == 2) {
        *out++ = static_cast<uint8_t>(v);
        *out++ = static_cast<uint8_t>(v >> 8);
      } else if (f.subslot == 3) {
        *out++ = 0;
        *out++ = static_cast<uint8_t>(v);
        *out++ = static_cast<uint8_t>(v >> 8);
      } else {
        *out++ = 0;
        *out++ = 0;
        *out++ = static_cast<uint8_t>(v);
        *out++ = static_cast<uint8_t>(v >> 8);
      }
    }
  }
}

// The device's frames to stereo 16-bit.
void from_device(const uint8_t* in, uint32_t frames, const UacFormat& f, uint8_t* pcm) {
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t v[2] = {0, 0};
    for (uint32_t ch = 0; ch < f.channels; ++ch) {
      int32_t s;
      if (f.subslot == 2) s = le16s(in);
      else if (f.subslot == 3) s = static_cast<int16_t>(in[1] | (in[2] << 8));   // the top 16 bits of the 24
      else s = static_cast<int16_t>(in[2] | (in[3] << 8));                       // the top 16 bits of the 32 (or 24 left-justified)
      in += f.subslot;
      if (ch < 2) v[ch] = s;
    }
    if (f.channels == 1) v[1] = v[0];
    pcm[4 * i] = static_cast<uint8_t>(v[0]);
    pcm[4 * i + 1] = static_cast<uint8_t>(v[0] >> 8);
    pcm[4 * i + 2] = static_cast<uint8_t>(v[1]);
    pcm[4 * i + 3] = static_cast<uint8_t>(v[1] >> 8);
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the capture side

const char* UsbAudioCapture::name() const { return device_.name(); }

bool UsbAudioCapture::start(allocators::IAllocator& allocator) {
  kernel::ScopedMutex guard(mutex_);
  if (!ring_.allocate(allocator, UsbAudioDevice::kRingBytes)) return false;
  running_ = true;
  return true;
}

void UsbAudioCapture::stop(allocators::IAllocator& allocator) {
  kernel::ScopedMutex guard(mutex_);
  running_ = false;
  ring_.release(allocator);
}

void UsbAudioCapture::refresh() {
  if (running_) device_.drain_input(ring_);
}
bool UsbAudioCapture::hardware_gain() const { return device_.hardware_input_gain(); }
void UsbAudioCapture::apply_gain() { device_.apply_input_gain(); }

// ------------------------------------------------------------------------------------------------ finding devices

int UsbAudioDevice::pick_stream(const UacDevice& model, bool input) {
  for (uint32_t i = 0; i < model.stream_count; ++i) {
    if (model.streams[i].input != input) continue;
    for (uint32_t k = 0; k < model.streams[i].format_count; ++k)
      if (usable(model.streams[i].formats[k])) return static_cast<int>(i);
  }
  return -1;
}

UsbAudioDevice::UsbAudioDevice(IUsbHostController& host, uint32_t port, const UacDevice& model, const UsbDeviceDescriptor& descriptor, allocators::IAllocator& allocator)
    : host_(host), port_(port), allocator_(allocator), model_(model), capture_(*this) {
  out_.stream = pick_stream(model_, false);
  in_.stream = pick_stream(model_, true);
  read_name(descriptor);
}

UsbAudioDevice::~UsbAudioDevice() { shutdown(); }

void UsbAudioDevice::discover(IUsbBus& usb, allocators::IAllocator& allocator, List<UniquePointer<UsbAudioDevice>>& out) {
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    IUsbHostController* host = usb.host_controller(c);
    if (!host || !host->supports_isochronous()) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) {
      UsbDeviceDescriptor descriptor;
      uint8_t blob[2048];
      uint16_t length = 0;
      if (!host->device_descriptor(port, &descriptor) || !host->configuration_descriptor(port, ByteView(blob), &length)) continue;
      UacDevice model;
      if (!parse_uac_configuration(ConstByteView(blob).at_most(length), &model)) continue;
      UniquePointer<UsbAudioDevice> device = UniquePointer<UsbAudioDevice>::make(allocator, *host, port, model, descriptor, allocator);
      if (!device || (device->out_.stream < 0 && device->in_.stream < 0)) continue;
      if (device->initialize()) out.push_back(util::move(device));   // (a device that does not come up has put the interfaces back already)
    }
  }
}

void UsbAudioDevice::read_name(const UsbDeviceDescriptor& d) {
  const char prefix[] = "USB audio: ";
  uint32_t n = 0;
  for (; prefix[n]; ++n) name_[n] = prefix[n];
  uint8_t buffer[64];
  if (d.product_string && host_.control_in(port_, 0x80, 6, static_cast<uint16_t>((3u << 8) | d.product_string), 0x0409, ByteView(buffer)) && buffer[0] >= 4 && buffer[1] == 3) {
    const uint32_t length = buffer[0] < sizeof buffer ? buffer[0] : sizeof buffer;
    uint32_t m = n;
    for (uint32_t i = 2; i + 1 < length && m + 1 < sizeof name_; i += 2) {
      const uint8_t ch = buffer[i];
      if (buffer[i + 1] == 0 && ch >= 32 && ch < 127) name_[m++] = static_cast<char>(ch);
    }
    if (m > n) {
      name_[m] = 0;
      return;
    }
  }
  static const char digits[] = "0123456789abcdef";
  const uint16_t ids[2] = {d.vendor_id, d.product_id};
  n = 0;
  const char fallback[] = "USB audio device ";
  for (; fallback[n]; ++n) name_[n] = fallback[n];
  for (int k = 0; k < 2; ++k) {
    for (int shift = 12; shift >= 0; shift -= 4) name_[n++] = digits[(ids[k] >> shift) & 15];
    if (k == 0) name_[n++] = ':';
  }
  name_[n] = 0;
}

// ------------------------------------------------------------------------------------------------ bring-up

bool UsbAudioDevice::initialize() {
  if (!host_.set_configuration(port_, model_.configuration)) return false;
  configured_ = true;
  if (out_.stream >= 0 && !bring_up(out_, false)) out_.stream = -1;
  if (in_.stream >= 0 && !bring_up(in_, true)) in_.stream = -1;
  if (!out_.running && !in_.running) return false;
  if (out_.running) {
    if (!ring_.allocate(allocator_, kRingBytes)) {
      shutdown();
      return false;
    }
    running_ = true;
    find_controls(out_, false);
    apply_volume();
  }
  if (in_.running) {
    find_controls(in_, true);
    capture_started_ = capture_.start(allocator_);
    if (capture_started_) apply_input_gain();
    else tear_down(in_, true);
  }
  if (!out_.running && !in_.running) return false;
  return true;
}

void UsbAudioDevice::shutdown() {
  if (capture_started_) capture_.stop(allocator_);
  capture_started_ = false;
  running_ = false;
  if (out_.running) tear_down(out_, false);
  if (in_.running) tear_down(in_, true);
  ring_.release(allocator_);
}

void UsbAudioDevice::tear_down(Path& path, bool input) {
  (void)input;
  const UacStream& stream = model_.streams[path.stream];
  const UacFormat& f = stream.formats[path.format];
  host_.control_out(port_, 0x01, kSetInterface, 0, stream.interface);   // back to the setting without bandwidth
  if (path.feedback) host_.stop_iso_endpoint(port_, path.feedback_endpoint);
  host_.stop_iso_endpoint(port_, f.endpoint);
  path.running = false;
  path.feedback = false;
}

bool UsbAudioDevice::bring_up(Path& path, bool input) {
  const UacStream& stream = model_.streams[path.stream];
  uint8_t order[UacStream::kMaxFormats];
  uint32_t n = 0;
  for (uint32_t k = 0; k < stream.format_count; ++k) {
    if (!usable(stream.formats[k])) continue;
    uint32_t at = n++;
    while (at > 0 && score(stream.formats[order[at - 1]]) > score(stream.formats[k])) {
      order[at] = order[at - 1];
      --at;
    }
    order[at] = static_cast<uint8_t>(k);
  }
  for (uint32_t i = 0; i < n; ++i)
    if (try_format(path, input, order[i])) return true;
  return false;
}

// UAC2: the stream's clock must run at 48 kHz (set it if the device lets us).
bool UsbAudioDevice::set_rate_v2(const UacStream& stream) {
  const UacEntity* clock = uac_clock_source(model_, stream);
  if (!clock) return false;
  const uint16_t index = static_cast<uint16_t>((clock->id << 8) | model_.control_interface);
  const uint16_t value = 0x0100;   // CS_SAM_FREQ_CONTROL
  // what the clock can do
  uint8_t range[2 + 12 * 8] = {};
  bool known = false, supported = false;
  if (host_.control_in(port_, kClassInterfaceIn, kRequestRange, value, index, ByteView(range))) {
    const uint32_t count = range[0] | (range[1] << 8);
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      const uint8_t* r = range + 2 + 12 * i;
      const uint32_t lo = r[0] | (r[1] << 8) | (r[2] << 16) | (static_cast<uint32_t>(r[3]) << 24);
      const uint32_t hi = r[4] | (r[5] << 8) | (r[6] << 16) | (static_cast<uint32_t>(r[7]) << 24);
      const uint32_t step = r[8] | (r[9] << 8) | (r[10] << 16) | (static_cast<uint32_t>(r[11]) << 24);
      known = true;
      if (lo <= 48000 && 48000 <= hi && (step == 0 || (48000 - lo) % step == 0)) supported = true;
    }
  }
  if (known && !supported) return false;
  if (clock->clock_programmable) {
    const uint8_t rate[4] = {0x80, 0xBB, 0x00, 0x00};
    if (!host_.control_out_data(port_, kClassInterfaceOut, kRequestSetCur, value, index, ConstByteView(rate, 4))) return false;
  }
  uint8_t current[4] = {};
  if (!host_.control_in(port_, kClassInterfaceIn, kRequestGetCur, value, index, ByteView(current))) return false;
  return (current[0] | (current[1] << 8) | (current[2] << 16) | (static_cast<uint32_t>(current[3]) << 24)) == 48000;
}

bool UsbAudioDevice::try_format(Path& path, bool input, uint32_t format_index) {
  const UacStream& stream = model_.streams[path.stream];
  const UacFormat& f = stream.formats[format_index];
  if (model_.version == 2 && !set_rate_v2(stream)) return false;
  if (!host_.start_iso_endpoint(port_, f.endpoint, f.max_packet, f.interval)) return false;
  const uint32_t interval_us = host_.iso_interval_us(port_, f.endpoint);
  const uint32_t frames = interval_us % 125 == 0 ? interval_us / 125 * 6 : 0;   // 48000 frames a second
  const uint32_t frame_bytes = uint32_t{f.channels} * f.subslot;
  bool ok = interval_us != 0 && frames != 0 && frames <= kMaxFramesPerPacket && frames * frame_bytes <= f.max_packet;
  bool feedback = false;
  if (ok && !input && f.sync == 1 && f.feedback_endpoint) feedback = host_.start_iso_endpoint(port_, f.feedback_endpoint, f.feedback_max_packet, f.feedback_interval);
  if (ok && !host_.control_out(port_, 0x01, kSetInterface, f.alternate, stream.interface)) ok = false;
  if (!ok) {
    if (feedback) host_.stop_iso_endpoint(port_, f.feedback_endpoint);
    host_.stop_iso_endpoint(port_, f.endpoint);
    return false;
  }
  if (model_.version == 1 && f.rate_control) {   // the rate is set on the endpoint, which exists now that the setting is selected
    const uint8_t rate[3] = {0x80, 0xBB, 0x00};
    host_.control_out_data(port_, kClassEndpointOut, kRequestSetCur, 0x0100, f.endpoint, ConstByteView(rate, 3));   // (a device with only 48 kHz need not accept it)
  }
  path.format = static_cast<int>(format_index);
  path.interval_us = interval_us;
  path.frames_per_packet = frames;
  path.frame_bytes = frame_bytes;
  path.feedback = feedback;
  path.feedback_endpoint = f.feedback_endpoint;
  path.feedback_value = 0;
  path.accumulator = 0;
  path.capacity = input ? 0 : host_.iso_free_packets(port_, f.endpoint);
  path.running = true;
  return true;
}

// ------------------------------------------------------------------------------------------------ volume and mute

bool UsbAudioDevice::unit_get(const Path& path, uint8_t request, uint8_t selector, uint8_t channel, ByteView out) {
  return host_.control_in(port_, kClassInterfaceIn, request, static_cast<uint16_t>((selector << 8) | channel),
                          static_cast<uint16_t>((path.unit->id << 8) | model_.control_interface), out);
}

bool UsbAudioDevice::unit_set(const Path& path, uint8_t selector, uint8_t channel, ConstByteView value) {
  return host_.control_out_data(port_, kClassInterfaceOut, kRequestSetCur, static_cast<uint16_t>((selector << 8) | channel),
                                static_cast<uint16_t>((path.unit->id << 8) | model_.control_interface), value);
}

void UsbAudioDevice::find_controls(Path& path, bool input) {
  path.unit = uac_feature_unit(model_, model_.streams[path.stream]);
  (void)input;
  if (!path.unit) return;
  const UacEntity& u = *path.unit;
  path.volume_master = u.volume[0];
  path.mute_master = u.mute[0];
  path.volume_control = u.volume[0] || (u.volume[1] && u.volume[2]);
  path.mute_control = u.mute[0] || (u.mute[1] && u.mute[2]);
  if (path.volume_control) {
    const uint8_t channel = path.volume_master ? 0 : 1;
    uint8_t lo[2] = {}, hi[2] = {};
    bool ok;
    if (model_.version == 1) {
      ok = unit_get(path, kRequestGetMin, kControlVolume, channel, ByteView(lo)) && unit_get(path, kRequestGetMax, kControlVolume, channel, ByteView(hi));
    } else {
      uint8_t range[2 + 6] = {};
      ok = unit_get(path, kRequestRange, kControlVolume, channel, ByteView(range)) && (range[0] | (range[1] << 8)) >= 1;
      for (int i = 0; i < 2; ++i) {
        lo[i] = range[2 + i];
        hi[i] = range[4 + i];
      }
    }
    path.volume_min = le16s(lo);
    path.volume_max = le16s(hi);
    if (!ok || path.volume_max <= path.volume_min) path.volume_control = false;
  }
}

// Volume 0..100 as a position in the top 60 dB of the device's range (or all of it, if smaller), 0 as the bottom of the range.
void UsbAudioDevice::set_unit_volume(Path& path, uint8_t percent, bool muted) {
  if (!path.unit) return;
  const uint8_t channels[2] = {path.volume_master ? uint8_t{0} : uint8_t{1}, 2};
  if (path.volume_control) {
    const int32_t span = int32_t{path.volume_max} - path.volume_min;
    const int32_t window = span < 60 * 256 ? span : 60 * 256;
    int32_t raw = path.volume_max - (100 - percent) * window / 100;
    if (percent == 0 || (muted && !path.mute_control)) raw = path.volume_min;
    if (raw != path.last_volume) {
      const uint8_t data[2] = {static_cast<uint8_t>(raw), static_cast<uint8_t>(raw >> 8)};
      bool ok = true;
      for (uint32_t i = 0; i < (path.volume_master ? 1u : 2u); ++i) ok = unit_set(path, kControlVolume, channels[i], ConstByteView(data, 2)) && ok;
      if (ok) path.last_volume = raw;
    }
  }
  if (path.mute_control) {
    const int32_t want = muted ? 1 : 0;
    if (want != path.last_mute) {
      const uint8_t data[1] = {static_cast<uint8_t>(want)};
      bool ok = true;
      for (uint32_t i = 0; i < (path.mute_master ? 1u : 2u); ++i) ok = unit_set(path, kControlMute, path.mute_master ? uint8_t{0} : channels[i], ConstByteView(data, 1)) && ok;
      if (ok) path.last_mute = want;
    }
  }
}

void UsbAudioDevice::apply_volume() {
  if (!out_.running) return;
  set_unit_volume(out_, volume_, muted_);
  soft_gain_ = out_.volume_control ? 100 : volume_;
  if (muted_ && !out_.mute_control && !out_.volume_control) soft_gain_ = 0;
}

void UsbAudioDevice::apply_input_gain() {
  if (!in_.running) return;
  set_unit_volume(in_, capture_.gain(), capture_.muted());   // (without hardware gain the samples are scaled when they are read)
}

// ------------------------------------------------------------------------------------------------ streaming

void UsbAudioDevice::refresh() {
  if (running_ && out_.running) pump_output();
}

void UsbAudioDevice::poll_feedback() {
  Path& p = out_;
  if (!p.feedback) return;
  const UacFormat& f = model_.streams[p.stream].formats[p.format];
  uint8_t packet[8];
  uint16_t length = 0;
  while (host_.iso_receive(port_, p.feedback_endpoint, ByteView(packet), &length)) {
    if (length != 3 && length != 4) continue;   // 3 bytes: 10.14 samples per frame; 4 bytes: 16.16 per microframe
    uint32_t value = packet[0] | (packet[1] << 8) | (packet[2] << 16);
    uint32_t frac = 14;
    if (length == 4) {
      value |= static_cast<uint32_t>(packet[3]) << 24;
      frac = 16;
    }
    // per packet: the value is per frame (1 ms) or per microframe (125 us)
    const uint64_t per_packet = (uint64_t{value} * (p.interval_us / (frac == 16 ? 125 : 1000))) >> frac;
    const uint64_t nominal = p.frames_per_packet;
    (void)f;
    if (per_packet * 8 < nominal * 7 || per_packet * 8 > nominal * 9) continue;   // not believable (more than 12.5 % off)
    p.feedback_value = value;
    p.feedback_frac_bits = frac;
  }
}

uint32_t UsbAudioDevice::next_frames() {
  Path& p = out_;
  if (!p.feedback || !p.feedback_value) return p.frames_per_packet;
  const uint32_t frac = p.feedback_frac_bits;
  p.accumulator += uint64_t{p.feedback_value} * (p.interval_us / (frac == 16 ? 125 : 1000));
  uint32_t frames = static_cast<uint32_t>(p.accumulator >> frac);
  p.accumulator &= (uint64_t{1} << frac) - 1;
  const UacFormat& f = model_.streams[p.stream].formats[p.format];
  const uint32_t most = f.max_packet / p.frame_bytes;
  if (frames > most) frames = most;
  return frames;
}

void UsbAudioDevice::pump_output() {
  Path& p = out_;
  const UacFormat& f = model_.streams[p.stream].formats[p.format];
  poll_feedback();
  uint32_t lead = kLeadMicroseconds / p.interval_us;
  if (lead < 4) lead = 4;
  if (lead > p.capacity) lead = p.capacity;
  uint32_t free = host_.iso_free_packets(port_, f.endpoint);
  while (free > 0 && p.capacity - free < lead) {
    const uint32_t frames = next_frames();
    uint8_t pcm[kMaxFramesPerPacket * kAudioFrameSize + 64] = {};
    const uint32_t bytes = frames * kAudioFrameSize;
    ring_.peek(ByteView(pcm, bytes));                  // whatever is queued; the rest of the packet is silence
    uint8_t packet[1024];
    if (frames * p.frame_bytes > sizeof packet) break;
    to_device(pcm, frames, f, soft_gain_, packet);
    if (!host_.iso_submit(port_, f.endpoint, ConstByteView(packet, frames * p.frame_bytes))) break;
    ring_.advance_by(bytes);
    --free;
  }
}

void UsbAudioDevice::drain_input(CaptureRing& ring) {
  Path& p = in_;
  const UacFormat& f = model_.streams[p.stream].formats[p.format];
  uint8_t packet[1024];
  uint8_t pcm[kMaxFramesPerPacket * kAudioFrameSize + 4096];
  uint16_t length = 0;
  for (uint32_t guard = 0; guard < 256 && host_.iso_receive(port_, f.endpoint, ByteView(packet), &length); ++guard) {
    uint32_t frames = length / p.frame_bytes;
    if (length == 0) frames = p.frames_per_packet;   // an interval with nothing: keep the time by recording silence for it
    if (frames > sizeof pcm / kAudioFrameSize) frames = sizeof pcm / kAudioFrameSize;
    if (length == 0) {
      for (uint32_t i = 0; i < frames * kAudioFrameSize; ++i) pcm[i] = 0;
    } else {
      from_device(packet, frames, f, pcm);
    }
    ring.push(ConstByteView(pcm, frames * kAudioFrameSize));
  }
}

}  // namespace hardware
