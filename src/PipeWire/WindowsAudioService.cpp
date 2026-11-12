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

#include "PipeWire/WindowsAudioService.hpp"
#include "Audio/Pan.hpp"
#include "Process/WindowsAudio.hpp"

namespace pipewire {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNoMemory = 0xC0000017u,
                   kDeviceNotConnected = 0xC000009Du, kTooManyOpened = 0xC000011Fu;
enum : uint64_t { kOpen = 1, kClose, kWrite, kStatus, kFlush, kPause, kSetVolume, kGetVolume, kDevices, kSetMute, kOpenCapture, kRead };
constexpr uint64_t kMaxRead = 1u << 20;
constexpr uint32_t kWaveFormatPcm = 1, kWaveFormatFloat = 3;

WindowsAudioService* g_service = nullptr;

uint32_t handler(process::Process& p, const process::SyscallRequest& r) { return g_service ? g_service->call(p, r) : 0xC00000BBu; }

bool to_format(uint64_t code, uint64_t rate, uint64_t channels, PcmFormat* out) {
  const uint32_t tag = static_cast<uint32_t>(code & 0xFFFF), bits = static_cast<uint32_t>((code >> 16) & 0xFFFF);
  if (rate > 1000000 || channels > PcmFormat::kMaxChannels) return false;
  if (tag == kWaveFormatPcm) {
    if (bits == 8) out->format = SampleFormat::U8;
    else if (bits == 16) out->format = SampleFormat::S16Le;
    else if (bits == 24) out->format = SampleFormat::S24Le;
    else if (bits == 32) out->format = SampleFormat::S32Le;
    else return false;
  } else if (tag == kWaveFormatFloat) {
    if (bits == 32) out->format = SampleFormat::F32Le;
    else if (bits == 64) out->format = SampleFormat::F64Le;
    else return false;
  } else {
    return false;
  }
  out->rate = static_cast<uint32_t>(rate);
  out->channels = static_cast<uint32_t>(channels);
  return out->valid();
}

}  // namespace

void install_windows_audio(WindowsAudioService& service) {
  g_service = &service;
  process::windowsabi::set_audio_handler(&handler);
}

void windows_audio_process_exited(uint64_t pid) {
  if (g_service) g_service->process_exited(pid);
}

WindowsAudioService::~WindowsAudioService() {
  if (g_service == this) {
    process::windowsabi::set_audio_handler(nullptr);
    g_service = nullptr;
  }
  for (UniquePointer<Owner>& o : owners_.view()) graph_.remove_client(o->client);
}

WindowsAudioService::Owner* WindowsAudioService::owner_locked(uint64_t pid, bool create) {
  for (UniquePointer<Owner>& o : owners_.view())
    if (o->pid == pid) return o.get();
  if (!create) return nullptr;
  UniquePointer<Owner> o = UniquePointer<Owner>::make(allocator_, allocator_);
  if (!o) return nullptr;
  o->pid = pid;
  o->client = graph_.add_client(Origin::Windows, pid, "tcos-winmm", "tcos-winmm");
  if (!o->client) return nullptr;
  Owner* raw = o.get();
  if (!owners_.push_back(util::move(o))) {
    graph_.remove_client(raw->client);
    return nullptr;
  }
  return raw;
}

bool WindowsAudioService::owns_locked(uint64_t pid, uint32_t node) {
  Owner* o = owner_locked(pid, false);
  if (!o) return false;
  for (uint32_t n : o->nodes)
    if (n == node) return true;
  return false;
}

uint32_t WindowsAudioService::stream_count() {
  kernel::ScopedMutex guard(mutex_);
  uint32_t n = 0;
  for (UniquePointer<Owner>& o : owners_.view()) n += static_cast<uint32_t>(o->nodes.count());
  return n;
}

void WindowsAudioService::process_exited(uint64_t pid) {
  uint32_t client = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    for (uint64_t i = 0; i < owners_.count(); ++i)
      if (owners_[i]->pid == pid) {
        client = owners_[i]->client;
        owners_.remove_at(i);
        break;
      }
  }
  if (client) graph_.remove_client(client);
}

uint32_t WindowsAudioService::open(uint64_t pid, const PcmFormat& format, StringView name, uint32_t* stream) {
  return open_node(pid, format, name, false, false, stream);
}

uint32_t WindowsAudioService::open_capture(uint64_t pid, const PcmFormat& format, StringView name, uint32_t* stream, bool monitor) {
  return open_node(pid, format, name, true, monitor, stream);
}

uint32_t WindowsAudioService::open_node(uint64_t pid, const PcmFormat& format, StringView name, bool capture, bool monitor, uint32_t* stream) {
  *stream = 0;
  if (capture && !monitor ? !graph_.source_id() : !graph_.mixer()) return kDeviceNotConnected;
  if (!format.valid()) return kInvalidParameter;
  kernel::ScopedMutex guard(mutex_);
  Owner* o = owner_locked(pid, true);
  if (!o) return kNoMemory;
  if (o->nodes.count() >= kMaxStreamsPerProcess) return kTooManyOpened;
  Properties props(allocator_);
  if (!name.empty()) {
    props.set("application.name", name);
    props.set("media.name", name);
    Properties client(allocator_);
    client.set("application.name", name);
    graph_.update_client(o->client, client, Properties::Mode::kReplace);
  }
  props.set("client.api", "tcos-winmm");
  props.set("media.role", capture ? "Communication" : "Game");
  uint32_t node = 0;
  if (capture) {
    SharedPointer<RecordStream> record;
    node = graph_.create_record(o->client, format, props, &record, monitor);
  } else {
    SharedPointer<PlaybackStream> playback;
    node = graph_.create_playback(o->client, format, props, &playback);
  }
  if (!node) return kTooManyOpened;
  if (!o->nodes.push_back(node)) {
    graph_.destroy_node(node);
    return kNoMemory;
  }
  *stream = node;
  return kSuccess;
}

uint32_t WindowsAudioService::write(uint64_t pid, uint32_t stream, ConstByteView pcm, uint32_t* taken) {
  *taken = 0;
  SharedPointer<PlaybackStream> s;
  {
    kernel::ScopedMutex guard(mutex_);
    if (!owns_locked(pid, stream)) return kInvalidHandle;
  }
  s = graph_.playback_of(stream);
  if (!s) return kInvalidHandle;
  *taken = s->write(pcm);
  return kSuccess;
}

uint32_t WindowsAudioService::read(uint64_t pid, uint32_t stream, ByteView out, uint32_t* got) {
  *got = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    if (!owns_locked(pid, stream)) return kInvalidHandle;
  }
  SharedPointer<RecordStream> s = graph_.record_of(stream);
  if (!s) return kInvalidHandle;
  *got = s->read(out);
  return kSuccess;
}

uint32_t WindowsAudioService::close(uint64_t pid, uint32_t stream, bool drain) {
  {
    kernel::ScopedMutex guard(mutex_);
    Owner* o = owner_locked(pid, false);
    if (!o) return kInvalidHandle;
    bool found = false;
    for (uint64_t i = 0; i < o->nodes.count(); ++i)
      if (o->nodes[i] == stream) {
        o->nodes.remove_at(i);
        found = true;
        break;
      }
    if (!found) return kInvalidHandle;
  }
  graph_.destroy_node(stream, drain);
  return kSuccess;
}

uint32_t WindowsAudioService::volume_node(uint64_t pid, uint32_t stream, uint32_t* node) {
  if (stream == kSourceStream) {
    *node = graph_.source_id();
    return *node ? kSuccess : kDeviceNotConnected;
  }
  *node = stream ? stream : graph_.sink_id();
  if (!stream) return kSuccess;
  kernel::ScopedMutex guard(mutex_);
  return owns_locked(pid, stream) ? kSuccess : kInvalidHandle;
}

uint32_t WindowsAudioService::set_volume(uint64_t pid, uint32_t stream, uint32_t left, uint32_t right) {
  uint32_t node = 0;
  const uint32_t st = volume_node(pid, stream, &node);
  if (st != kSuccess) return st;
  if (left > 0xFFFF) left = 0xFFFF;
  if (right > 0xFFFF) right = 0xFFFF;
  float volume = 1.0f;
  bool mute = false;
  if (!graph_.get_volume(node, &volume, &mute)) return kInvalidHandle;
  // (a playing stream's louder side is its volume, the other one a pan; the master volumes and record streams have no balance)
  return graph_.set_balance(node, static_cast<float>(left) / 65535.0f, static_cast<float>(right) / 65535.0f, mute) ? kSuccess : kInvalidHandle;
}

uint32_t WindowsAudioService::get_volume(uint64_t pid, uint32_t stream, uint32_t* left, uint32_t* right, bool* mute) {
  *left = *right = 0;
  *mute = false;
  uint32_t node = 0;
  const uint32_t st = volume_node(pid, stream, &node);
  if (st != kSuccess) return st;
  float l = 1.0f, r = 1.0f;
  if (!graph_.get_balance(node, &l, &r, mute)) return kInvalidHandle;
  if (l > 1.0f) l = 1.0f;
  if (r > 1.0f) r = 1.0f;
  *left = static_cast<uint32_t>(l * 65535.0f + 0.5f);
  *right = static_cast<uint32_t>(r * 65535.0f + 0.5f);
  return kSuccess;
}

uint32_t WindowsAudioService::set_mute(uint64_t pid, uint32_t stream, bool mute) {
  uint32_t node = 0;
  const uint32_t st = volume_node(pid, stream, &node);
  if (st != kSuccess) return st;
  float volume = 1.0f;
  bool was = false;
  if (!graph_.get_volume(node, &volume, &was)) return kInvalidHandle;
  return graph_.set_volume(node, volume, mute) ? kSuccess : kInvalidHandle;
}

uint32_t WindowsAudioService::call(process::Process& p, const process::SyscallRequest& r) {
  const uint64_t at = r.args[0];
  uint64_t a[8] = {};
  uint8_t raw[64];
  if (!p.read_user(at, ByteView(raw, sizeof raw))) return kAccessViolation;
  for (int i = 0; i < 8; ++i)
    for (int k = 0; k < 8; ++k) a[i] |= uint64_t{raw[i * 8 + k]} << (8 * k);
  const auto put = [&](int index, uint64_t value) {
    uint8_t b[8];
    for (int k = 0; k < 8; ++k) b[k] = static_cast<uint8_t>(value >> (8 * k));
    return p.write_user(at + uint64_t(index) * 8, ConstByteView(b, 8));
  };
  const uint64_t pid = p.id();
  const uint32_t stream = static_cast<uint32_t>(a[1]);
  switch (a[0]) {
    case kOpen: case kOpenCapture: {
      PcmFormat format;
      if (!to_format(a[2], a[3], a[4], &format)) return kInvalidParameter;
      uint8_t name[256];
      uint64_t name_length = a[6] > 255 ? 255 : a[6];
      if (a[5] == 0) name_length = 0;
      if (name_length && !p.read_user(a[5], ByteView(name, name_length))) return kAccessViolation;
      uint32_t node = 0;
      // (OPEN_CAPTURE with [1] = 1: the monitor, what the mixer plays)
      const uint32_t st = open_node(pid, format, StringView(ConstByteView(name, name_length)), a[0] == kOpenCapture, a[0] == kOpenCapture && a[1] == 1, &node);
      if (st != kSuccess) return st;
      if (!put(7, node)) {
        close(pid, node, false);
        return kAccessViolation;
      }
      return kSuccess;
    }
    case kClose: return close(pid, stream, a[2] != 0);
    case kWrite: {
      if (a[3] > kMaxWrite) return kInvalidParameter;
      {
        kernel::ScopedMutex guard(mutex_);
        if (!owns_locked(pid, stream)) return kInvalidHandle;
      }
      SharedPointer<PlaybackStream> s = graph_.playback_of(stream);
      if (!s) return kInvalidHandle;
      // in pieces, so that a large write does not need a large kernel buffer; it stops at the first piece the stream does not take whole
      ByteBuffer piece(allocator_);
      const uint64_t chunk = a[3] < 16384 ? a[3] : 16384;
      if (chunk && !piece.put_zeros(chunk)) return kNoMemory;
      uint64_t taken = 0;
      while (taken < a[3]) {
        const uint64_t n = a[3] - taken < chunk ? a[3] - taken : chunk;
        ByteView part = piece.writable_view().first(n);
        if (!p.read_user(a[2] + taken, part)) return kAccessViolation;
        const uint32_t got = s->write(part);
        taken += got;
        if (got < n) break;
      }
      return put(7, taken) ? kSuccess : kAccessViolation;
    }
    case kRead: {
      if (a[3] > kMaxRead) return kInvalidParameter;
      {
        kernel::ScopedMutex guard(mutex_);
        if (!owns_locked(pid, stream)) return kInvalidHandle;
      }
      SharedPointer<RecordStream> s = graph_.record_of(stream);
      if (!s) return kInvalidHandle;
      // in pieces, like a write: whole frames of the stream's format, until the program's buffer is full or nothing more is waiting
      const uint32_t frame = s->format().frame_size();
      uint64_t want = a[3] - a[3] % (frame ? frame : 1);
      ByteBuffer piece(allocator_);
      uint64_t chunk = want < 16384 ? want : 16384;
      chunk -= chunk % (frame ? frame : 1);
      if (chunk && !piece.put_zeros(chunk)) return kNoMemory;
      uint64_t done = 0;
      while (chunk && done < want) {
        const uint64_t n = want - done < chunk ? want - done : chunk;
        const uint32_t got = s->read(piece.writable_view().first(n));
        if (got && !p.write_user(a[2] + done, piece.view().first(got))) return kAccessViolation;
        done += got;
        if (got < n) break;
      }
      return put(7, done) ? kSuccess : kAccessViolation;
    }
    case kStatus: {
      {
        kernel::ScopedMutex guard(mutex_);
        if (!owns_locked(pid, stream)) return kInvalidHandle;
      }
      if (SharedPointer<RecordStream> r = graph_.record_of(stream)) {
        // a record stream: [4] bytes waiting to be read, [5] bytes lost, [6] bytes recorded (read and waiting), [7] bytes read
        const uint64_t waiting = r->available_bytes(), read = r->read_bytes();
        return put(4, waiting) && put(5, r->lost_bytes()) && put(6, read + waiting) && put(7, read) ? kSuccess : kAccessViolation;
      }
      SharedPointer<PlaybackStream> s = graph_.playback_of(stream);
      if (!s) return kInvalidHandle;
      // (once it has played out, all of it counts as played: the mixer counts device frames, which do not convert back to the program's exactly)
      const bool drained = s->drained();
      const uint64_t written = s->written_bytes(), played = drained ? written : s->played_bytes();
      return put(4, drained ? 0 : s->waiting_bytes()) && put(5, drained ? 0 : s->buffered_bytes()) && put(6, written) && put(7, played < written ? played : written)
                 ? kSuccess
                 : kAccessViolation;
    }
    case kFlush: case kPause: {
      {
        kernel::ScopedMutex guard(mutex_);
        if (!owns_locked(pid, stream)) return kInvalidHandle;
      }
      if (a[0] == kPause) return graph_.set_paused(stream, a[2] != 0) ? kSuccess : kInvalidHandle;
      if (SharedPointer<RecordStream> r = graph_.record_of(stream)) {
        r->flush();
        return kSuccess;
      }
      SharedPointer<PlaybackStream> s = graph_.playback_of(stream);
      if (!s) return kInvalidHandle;
      s->flush();
      return kSuccess;
    }
    case kSetVolume: return set_volume(pid, stream, static_cast<uint32_t>(a[2] & 0xFFFF), static_cast<uint32_t>(a[3] & 0xFFFF));
    case kGetVolume: {
      uint32_t left = 0, right = 0;
      bool mute = false;
      const uint32_t st = get_volume(pid, stream, &left, &right, &mute);
      if (st != kSuccess) return st;
      return put(6, mute ? 1 : 0) && put(7, left | uint64_t{right} << 16) ? kSuccess : kAccessViolation;
    }
    case kSetMute: return set_mute(pid, stream, a[2] != 0);
    case kDevices: return put(6, graph_.has_input() ? 1 : 0) && put(7, graph_.mixer() ? 1 : 0) ? kSuccess : kAccessViolation;
    default: return kInvalidParameter;
  }
}

}  // namespace pipewire
