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

#include "PipeWire/PulseServer.hpp"
#include "Utility/ByteStream.hpp"

namespace pipewire {

using pulse::Cvolume;
using pulse::ChannelMap;
using pulse::SampleSpec;
using pulse::TagReader;
using pulse::TagWriter;

namespace {

// pulsecore/native-common.h
enum : uint32_t {
  kError = 0, kReply = 2, kCreatePlaybackStream = 3, kDeletePlaybackStream = 4, kCreateRecordStream = 5, kDeleteRecordStream = 6, kExit = 7, kAuth = 8,
  kSetClientName = 9, kLookupSink = 10, kLookupSource = 11, kDrainPlaybackStream = 12, kStat = 13, kGetPlaybackLatency = 14, kCreateUploadStream = 15,
  kDeleteUploadStream = 16, kFinishUploadStream = 17, kPlaySample = 18, kRemoveSample = 19, kGetServerInfo = 20, kGetSinkInfo = 21,
  kGetSinkInfoList = 22, kGetSourceInfo = 23, kGetSourceInfoList = 24, kGetModuleInfo = 25, kGetModuleInfoList = 26, kGetClientInfo = 27,
  kGetClientInfoList = 28, kGetSinkInputInfo = 29, kGetSinkInputInfoList = 30, kGetSourceOutputInfo = 31, kGetSourceOutputInfoList = 32,
  kGetSampleInfo = 33, kGetSampleInfoList = 34, kSubscribe = 35, kSetSinkVolume = 36, kSetSinkInputVolume = 37, kSetSourceVolume = 38,
  kSetSinkMute = 39, kSetSourceMute = 40, kCorkPlaybackStream = 41, kFlushPlaybackStream = 42, kTriggerPlaybackStream = 43, kSetDefaultSink = 44,
  kSetDefaultSource = 45, kSetPlaybackStreamName = 46, kSetRecordStreamName = 47, kKillClient = 48, kKillSinkInput = 49, kKillSourceOutput = 50,
  kLoadModule = 51, kUnloadModule = 52, kGetRecordLatency = 57, kCorkRecordStream = 58, kFlushRecordStream = 59, kPrebufPlaybackStream = 60,
  kRequest = 61, kOverflow = 62, kUnderflow = 63, kPlaybackStreamKilled = 64, kRecordStreamKilled = 65, kSubscribeEvent = 66, kMoveSinkInput = 67,
  kMoveSourceOutput = 68, kSetSinkInputMute = 69, kSuspendSink = 70, kSuspendSource = 71, kSetPlaybackStreamBufferAttr = 72,
  kSetRecordStreamBufferAttr = 73, kUpdatePlaybackStreamSampleRate = 74, kUpdateRecordStreamSampleRate = 75, kUpdateRecordStreamProplist = 80,
  kUpdatePlaybackStreamProplist = 81, kUpdateClientProplist = 82, kRemoveRecordStreamProplist = 83, kRemovePlaybackStreamProplist = 84,
  kRemoveClientProplist = 85, kStarted = 86, kExtension = 87, kGetCardInfo = 88, kGetCardInfoList = 89, kSetCardProfile = 90, kSetSinkPort = 96,
  kSetSourcePort = 97, kSetPortLatencyOffset = 98, kEnableSrbchannel = 99, kDisableSrbchannel = 100, kRegisterMemfdShmid = 101, kSendObjectMessage = 102,
};

// pulse/def.h errors
enum : uint32_t {
  kErrAccess = 1, kErrCommand = 2, kErrInvalid = 3, kErrExist = 4, kErrNoEntity = 5, kErrProtocol = 7, kErrVersion = 17, kErrTooLarge = 18,
  kErrNotSupported = 19, kErrNoExtension = 21,
};

constexpr uint32_t kTagNone = 0xFFFFFFFFu;
constexpr uint32_t kVersionMask = 0x0000FFFFu;
constexpr const char* kSinkName = "tcos_output.stereo";
constexpr uint8_t kPositionMono = 0, kPositionLeft = 1, kPositionRight = 2;
constexpr uint32_t kSinkFlags = 0x1 | 0x2 | 0x4;      // hardware volume, latency, hardware
constexpr uint32_t kSinkStateRunning = 0, kSinkStateIdle = 1;
constexpr uint64_t kSinkLatencyUs = 50000;

uint32_t ms_bytes(const SampleSpec& spec, uint32_t ms) {
  const uint64_t frames = uint64_t{spec.rate} * ms / 1000;
  return static_cast<uint32_t>(frames * spec.frame_size());
}

uint32_t round_frames(uint32_t bytes, uint32_t frame) {
  if (frame == 0) return bytes;
  bytes -= bytes % frame;
  return bytes < frame ? frame : bytes;
}

bool to_pcm_format(const SampleSpec& spec, PcmFormat* out) {
  SampleFormat f;
  switch (spec.format) {
    case pulse::kU8: f = SampleFormat::U8; break;
    case pulse::kAlaw: f = SampleFormat::Alaw; break;
    case pulse::kUlaw: f = SampleFormat::Ulaw; break;
    case pulse::kS16Le: f = SampleFormat::S16Le; break;
    case pulse::kS16Be: f = SampleFormat::S16Be; break;
    case pulse::kFloat32Le: f = SampleFormat::F32Le; break;
    case pulse::kFloat32Be: f = SampleFormat::F32Be; break;
    case pulse::kS32Le: f = SampleFormat::S32Le; break;
    case pulse::kS32Be: f = SampleFormat::S32Be; break;
    case pulse::kS24Le: f = SampleFormat::S24Le; break;
    case pulse::kS24Be: f = SampleFormat::S24Be; break;
    case pulse::kS24_32Le: f = SampleFormat::S24_32Le; break;
    default: return false;
  }
  out->format = f;
  out->rate = spec.rate;
  out->channels = spec.channels;
  return out->valid();
}

SampleSpec to_spec(const PcmFormat& f) {
  SampleSpec s;
  switch (f.format) {
    case SampleFormat::U8: s.format = pulse::kU8; break;
    case SampleFormat::Alaw: s.format = pulse::kAlaw; break;
    case SampleFormat::Ulaw: s.format = pulse::kUlaw; break;
    case SampleFormat::S16Be: s.format = pulse::kS16Be; break;
    case SampleFormat::S24Le: s.format = pulse::kS24Le; break;
    case SampleFormat::S24Be: s.format = pulse::kS24Be; break;
    case SampleFormat::S24_32Le: s.format = pulse::kS24_32Le; break;
    case SampleFormat::S32Le: s.format = pulse::kS32Le; break;
    case SampleFormat::S32Be: s.format = pulse::kS32Be; break;
    case SampleFormat::F32Le: case SampleFormat::F64Le: s.format = pulse::kFloat32Le; break;
    case SampleFormat::F32Be: s.format = pulse::kFloat32Be; break;
    default: s.format = pulse::kS16Le; break;
  }
  s.channels = static_cast<uint8_t>(f.channels > pulse::kChannelsMax ? pulse::kChannelsMax : f.channels);
  s.rate = f.rate;
  return s;
}

ChannelMap default_map(uint8_t channels) {
  ChannelMap m;
  m.channels = channels;
  if (channels == 1) m.positions[0] = kPositionMono;
  else
    for (uint8_t i = 0; i < channels && i < pulse::kChannelsMax; ++i) m.positions[i] = static_cast<uint8_t>(i < 2 ? kPositionLeft + i : i + 1);
  return m;
}

Cvolume flat_volume(uint8_t channels, uint32_t v) {
  Cvolume c;
  c.channels = channels;
  for (uint8_t i = 0; i < channels && i < pulse::kChannelsMax; ++i) c.values[i] = v;
  return c;
}

void put_be32(ByteView v, uint64_t at, uint32_t x) {
  v[at] = uint8_t(x >> 24);
  v[at + 1] = uint8_t(x >> 16);
  v[at + 2] = uint8_t(x >> 8);
  v[at + 3] = uint8_t(x);
}

void drop_front(ByteBuffer& b, uint64_t n) {
  if (n >= b.size()) { b.clear(); return; }
  ByteView all = b.writable_view();
  all.copy_from(ConstByteView(all).subview(n));
  b.truncate(b.size() - n);
}

float cube_root(float x) {
  if (!(x > 0.0f)) return 0.0f;
  float r = x < 1.0f ? 1.0f : x;
  for (int i = 0; i < 24; ++i) r = r - (r * r * r - x) / (3.0f * r * r);
  return r;
}

}  // namespace

float pulse_volume_to_linear(uint32_t volume) {
  const float v = static_cast<float>(volume) / static_cast<float>(pulse::kVolumeNorm);
  return v * v * v;
}

uint32_t linear_to_pulse_volume(float linear) {
  if (!(linear > 0.0f)) return 0;
  const float v = cube_root(linear) * static_cast<float>(pulse::kVolumeNorm) + 0.5f;
  return v >= static_cast<float>(pulse::kVolumeMax) ? pulse::kVolumeMax : static_cast<uint32_t>(v);
}

PulseServer::PulseServer(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), allocator_(allocator), clients_(allocator), snapshot_(allocator) {}

PulseServer::~PulseServer() {
  for (UniquePointer<Client>& c : clients_.view()) graph_.remove_client(c->graph_client);
}

PulseServer::Client* PulseServer::find(uint32_t handle) {
  for (UniquePointer<Client>& c : clients_.view())
    if (c->handle == handle) return c.get();
  return nullptr;
}

PulseServer::Stream* PulseServer::stream(Client& c, uint32_t channel) {
  for (UniquePointer<Stream>& s : c.streams.view())
    if (s->channel == channel) return s.get();
  return nullptr;
}

uint32_t PulseServer::open_client(uint64_t pid) {
  if (clients_.count() >= kMaxClients) return 0;
  UniquePointer<Client> c = UniquePointer<Client>::make(allocator_, allocator_);
  if (!c) return 0;
  c->handle = next_handle_++;
  if (next_handle_ == 0) next_handle_ = 1;
  c->pid = pid;
  c->graph_client = graph_.add_client(Origin::Linux, pid, "protocol-pulse", "pipewire-pulse");
  if (!c->graph_client) return 0;
  const uint32_t handle = c->handle;
  if (!clients_.push_back(util::move(c))) return 0;
  return handle;
}

void PulseServer::close_client(uint32_t handle) {
  for (uint64_t i = 0; i < clients_.count(); ++i)
    if (clients_[i]->handle == handle) {
      graph_.remove_client(clients_[i]->graph_client);      // (its streams stop with it)
      clients_.remove_at(i);
      return;
    }
}

bool PulseServer::take_output(uint32_t handle, ByteBuffer* out) {
  Client* c = find(handle);
  if (!c) return true;
  if (c->outbox.size()) {
    if (!out->append(c->outbox.view())) return true;
    c->outbox.clear();
  }
  return c->closing;
}

void PulseServer::receive(uint32_t handle, ConstByteView bytes) {
  Client* c = find(handle);
  if (!c || c->closing) return;
  if (bytes.size() > uint64_t{kMaxPacket} + kDescriptorSize - c->inbox.size() || !c->inbox.append(bytes)) {
    c->closing = true;
    return;
  }
  uint64_t at = 0;
  const ConstByteView in = c->inbox.view();
  while (!c->closing && in.size() - at >= kDescriptorSize) {
    ByteReader d(in.subview(at, kDescriptorSize));
    const uint32_t length = d.be32(), channel = d.be32(), offset_hi = d.be32(), offset_lo = d.be32(), flags = d.be32();
    if (length > kMaxPacket) {
      c->closing = true;
      break;
    }
    if (length > in.size() - at - kDescriptorSize) break;
    const int64_t offset = static_cast<int64_t>(uint64_t{offset_hi} << 32 | offset_lo);
    packet(*c, channel, in.subview(at + kDescriptorSize, length), offset, flags);
    at += kDescriptorSize + uint64_t{length};
  }
  drop_front(c->inbox, at);
  if (c->outbox.size() > kMaxOutbox) c->closing = true;
}

void PulseServer::packet(Client& c, uint32_t channel, ConstByteView payload, int64_t offset, uint32_t flags) {
  (void)offset;
  (void)flags;      // (seek modes: audio is appended as it comes)
  if (channel == kCommandChannel) command(c, payload);
  else if (c.authorized) audio(c, channel, payload);
  else c.closing = true;
}

// ---- packets out -----------------------------------------------------------------------------------------------------------------------------------

uint64_t PulseServer::begin_command(Client& c, uint32_t command, uint32_t tag) {
  const uint64_t start = c.outbox.size();
  c.outbox.put_zeros(kDescriptorSize);
  ByteView d = c.outbox.writable_view();
  if (d.size() >= start + kDescriptorSize) put_be32(d, start + 4, kCommandChannel);
  TagWriter w(c.outbox);
  w.u32(command);
  w.u32(tag);
  return start;
}

void PulseServer::end_command(Client& c, uint64_t start) {
  ByteView d = c.outbox.writable_view();
  if (d.size() < start + kDescriptorSize) return;
  put_be32(d, start, static_cast<uint32_t>(d.size() - start - kDescriptorSize));
}

void PulseServer::send_error(Client& c, uint32_t tag, uint32_t error) {
  const uint64_t m = begin_command(c, kError, tag);
  TagWriter(c.outbox).u32(error);
  end_command(c, m);
}

void PulseServer::send_simple_reply(Client& c, uint32_t tag) { end_command(c, begin_command(c, kReply, tag)); }

// ---- commands --------------------------------------------------------------------------------------------------------------------------------------

bool PulseServer::is_sink_name(StringView name) const { return name == StringView(kSinkName) || name == StringView("@DEFAULT_SINK@"); }

void PulseServer::command(Client& c, ConstByteView payload) {
  TagReader t(payload);
  uint32_t cmd = 0, tag = 0;
  if (!t.u32(&cmd) || !t.u32(&tag)) {
    c.closing = true;
    return;
  }
  if (!c.authorized && cmd != kAuth) {
    send_error(c, tag, kErrAccess);
    return;
  }
  const uint32_t sink = graph_.sink_id();
  auto invalid = [&] { send_error(c, tag, kErrProtocol); };
  switch (cmd) {
    case kAuth: {
      uint32_t version = 0;
      ConstByteView cookie;
      if (!t.u32(&version) || !t.arbitrary(&cookie)) return invalid();
      version &= kVersionMask;
      if (version < 8) {
        send_error(c, tag, kErrVersion);
        return;
      }
      c.version = version < kProtocolVersion ? version : kProtocolVersion;
      c.authorized = true;
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter(c.outbox).u32(c.version);      // (no SHM / memfd flag: the audio comes in the packets)
      end_command(c, m);
      return;
    }
    case kSetClientName: {
      Properties props(allocator_);
      if (c.version >= 13) {
        if (!t.proplist(&props)) return invalid();
      } else {
        StringView name;
        if (!t.string(&name)) return invalid();
        props.set("application.name", name);
      }
      c.props.update(props, Properties::Mode::kReplace);
      graph_.update_client(c.graph_client, props, Properties::Mode::kReplace);
      const uint64_t m = begin_command(c, kReply, tag);
      if (c.version >= 13) TagWriter(c.outbox).u32(c.graph_client);
      end_command(c, m);
      return;
    }
    case kCreatePlaybackStream: create_playback(c, tag, t); return;
    case kDeletePlaybackStream: {
      uint32_t channel = 0;
      if (!t.u32(&channel)) return invalid();
      if (!stream(c, channel)) return send_error(c, tag, kErrNoEntity);
      delete_stream(c, channel, false);
      return send_simple_reply(c, tag);
    }
    case kDrainPlaybackStream: {
      uint32_t channel = 0;
      if (!t.u32(&channel)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      s->drains.push_back(tag);      // (answered by flow() once it has played out)
      flow(c, *s);
      return;
    }
    case kCorkPlaybackStream: {
      uint32_t channel = 0;
      bool cork = false;
      if (!t.u32(&channel) || !t.boolean(&cork)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      s->corked = cork;
      graph_.set_paused(s->node, cork);
      send_simple_reply(c, tag);
      if (!cork) flow(c, *s);
      return;
    }
    case kFlushPlaybackStream: {
      uint32_t channel = 0;
      if (!t.u32(&channel)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      s->pending.clear();
      s->pending_offset = 0;
      s->playback->flush();
      send_simple_reply(c, tag);
      flow(c, *s);
      return;
    }
    case kTriggerPlaybackStream: case kPrebufPlaybackStream: {
      uint32_t channel = 0;
      if (!t.u32(&channel)) return invalid();
      if (!stream(c, channel)) return send_error(c, tag, kErrNoEntity);
      return send_simple_reply(c, tag);
    }
    case kGetPlaybackLatency: {
      uint32_t channel = 0, sec = 0, usec = 0;
      if (!t.u32(&channel) || !t.timeval(&sec, &usec)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      const uint64_t played = s->playback->played_bytes();
      const uint64_t frame = s->spec.frame_size();
      const uint64_t queued = (s->pending.size() - s->pending_offset) + s->playback->buffered_bytes();
      const uint64_t queued_us = frame && s->spec.rate ? queued / frame * 1000000 / s->spec.rate : 0;
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      w.usec(kSinkLatencyUs + queued_us);
      w.usec(0);
      w.boolean(!s->corked && s->started);
      w.timeval(sec, usec);
      w.timeval(sec, usec);      // (the same clock: client and server are one machine)
      w.s64(static_cast<int64_t>(s->write_index));
      w.s64(static_cast<int64_t>(played < s->write_index ? played : s->write_index));
      if (c.version >= 13) {
        w.u64(0);
        w.u64(played);
      }
      end_command(c, m);
      return;
    }
    case kSetPlaybackStreamBufferAttr: {
      uint32_t channel = 0, maxlength = 0, tlength = 0, prebuf = 0, minreq = 0;
      bool adjust = false, early = false;
      if (!t.u32(&channel) || !t.u32(&maxlength) || !t.u32(&tlength) || !t.u32(&prebuf) || !t.u32(&minreq)) return invalid();
      if (c.version >= 13 && !t.boolean(&adjust)) return invalid();
      if (c.version >= 14 && !t.boolean(&early)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      const uint32_t frame = s->spec.frame_size();
      if (tlength != 0xFFFFFFFFu && tlength) {
        const uint32_t lo = ms_bytes(s->spec, 20), hi = ms_bytes(s->spec, 2000);
        s->tlength = round_frames(tlength < lo ? lo : tlength > hi ? hi : tlength, frame);
      }
      if (minreq != 0xFFFFFFFFu && minreq) s->minreq = round_frames(minreq > s->tlength / 2 ? s->tlength / 2 : minreq, frame);
      if (s->maxlength < s->tlength) s->maxlength = s->tlength;
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      w.u32(s->maxlength);
      w.u32(s->tlength);
      w.u32(s->prebuf);
      w.u32(s->minreq);
      if (c.version >= 13) w.usec(frame && s->spec.rate ? uint64_t{s->tlength} / frame * 1000000 / s->spec.rate : 0);
      end_command(c, m);
      return;
    }
    case kUpdatePlaybackStreamProplist: {
      uint32_t channel = 0, mode = 0;
      Properties props(allocator_);
      if (!t.u32(&channel) || !t.u32(&mode) || !t.proplist(&props)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      if (mode > 2) return send_error(c, tag, kErrInvalid);
      graph_.update_node(s->node, props, mode == 0 ? Properties::Mode::kReplace : static_cast<Properties::Mode>(mode));
      return send_simple_reply(c, tag);
    }
    case kRemovePlaybackStreamProplist: case kRemoveClientProplist: return send_simple_reply(c, tag);
    case kSetPlaybackStreamName: {
      uint32_t channel = 0;
      StringView name;
      if (!t.u32(&channel) || !t.string(&name)) return invalid();
      Stream* s = stream(c, channel);
      if (!s) return send_error(c, tag, kErrNoEntity);
      Properties props(allocator_);
      props.set("media.name", name);
      graph_.update_node(s->node, props, Properties::Mode::kReplace);
      return send_simple_reply(c, tag);
    }
    case kUpdateClientProplist: {
      uint32_t mode = 0;
      Properties props(allocator_);
      if (!t.u32(&mode) || !t.proplist(&props)) return invalid();
      if (mode > 2) return send_error(c, tag, kErrInvalid);
      c.props.update(props, static_cast<Properties::Mode>(mode));
      graph_.update_client(c.graph_client, props, mode == 0 ? Properties::Mode::kReplace : static_cast<Properties::Mode>(mode));
      return send_simple_reply(c, tag);
    }
    case kGetServerInfo: {
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      w.string("PulseAudio (on PipeWire 1.4.0, TheoryCraftOS)");
      w.string("15.0.0");
      w.string("root");
      w.string("theorycraft");
      w.sample_spec(SampleSpec{pulse::kS16Le, 2, hardware::kAudioSampleRate});
      w.string(kSinkName);
      w.string_null();      // (no source)
      w.u32(0x7c05ae11);
      if (c.version >= 15) w.channel_map(default_map(2));
      end_command(c, m);
      return;
    }
    case kLookupSink: {
      StringView name;
      if (!t.string(&name)) return invalid();
      if (!is_sink_name(name)) return send_error(c, tag, kErrNoEntity);
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter(c.outbox).u32(sink);
      end_command(c, m);
      return;
    }
    case kGetSinkInfo: {
      uint32_t index = 0;
      StringView name;
      bool null_name = true;
      if (!t.u32(&index) || !t.string(&name, &null_name)) return invalid();
      if (!(index == sink || (!null_name && is_sink_name(name)))) return send_error(c, tag, kErrNoEntity);
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      fill_sink(c, w);
      end_command(c, m);
      return;
    }
    case kGetSinkInfoList: {
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      fill_sink(c, w);
      end_command(c, m);
      return;
    }
    case kGetSinkInputInfo: case kGetClientInfo: {
      uint32_t index = 0;
      if (!t.u32(&index)) return invalid();
      bool found = false;
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      graph_.visit(index, [&](const GraphObject& o) {
        if (cmd == kGetSinkInputInfo && o.type == ObjectType::Node && o.role == NodeRole::Playback) {
          fill_sink_input(c, w, o);
          found = true;
        } else if (cmd == kGetClientInfo && o.type == ObjectType::Client) {
          fill_client(c, w, o);
          found = true;
        }
      });
      if (!found) {
        c.outbox.truncate(m);
        return send_error(c, tag, kErrNoEntity);
      }
      end_command(c, m);
      return;
    }
    case kGetSinkInputInfoList: case kGetClientInfoList: {
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      graph_.for_each([&](const GraphObject& o) {
        if (cmd == kGetSinkInputInfoList && o.type == ObjectType::Node && o.role == NodeRole::Playback) fill_sink_input(c, w, o);
        else if (cmd == kGetClientInfoList && o.type == ObjectType::Client) fill_client(c, w, o);
      });
      end_command(c, m);
      return;
    }
    case kGetSourceInfoList: case kGetModuleInfoList: case kGetSourceOutputInfoList: case kGetSampleInfoList: case kGetCardInfoList:
      return send_simple_reply(c, tag);
    case kGetSourceInfo: case kGetModuleInfo: case kGetSourceOutputInfo: case kGetSampleInfo: case kGetCardInfo: case kLookupSource: case kPlaySample:
    case kRemoveSample:
      return send_error(c, tag, kErrNoEntity);
    case kSubscribe: {
      uint32_t mask = 0;
      if (!t.u32(&mask)) return invalid();
      c.subscriptions = mask;
      graph_.snapshot(&c.known);
      return send_simple_reply(c, tag);
    }
    case kSetSinkVolume: case kSetSinkMute: {
      uint32_t index = 0;
      StringView name;
      bool null_name = true;
      if (!t.u32(&index) || !t.string(&name, &null_name)) return invalid();
      if (!(index == sink || (!null_name && is_sink_name(name)))) return send_error(c, tag, kErrNoEntity);
      float volume = 1.0f;
      bool mute = false;
      graph_.get_volume(sink, &volume, &mute);
      if (cmd == kSetSinkVolume) {
        Cvolume v;
        if (!t.cvolume(&v) || v.channels == 0) return invalid();
        volume = pulse_volume_to_linear(v.max());
      } else if (!t.boolean(&mute)) {
        return invalid();
      }
      graph_.set_volume(sink, volume, mute);
      return send_simple_reply(c, tag);
    }
    case kSetSinkInputVolume: case kSetSinkInputMute: {
      uint32_t index = 0;
      if (!t.u32(&index)) return invalid();
      float volume = 1.0f;
      bool mute = false;
      if (index == sink || !graph_.get_volume(index, &volume, &mute)) return send_error(c, tag, kErrNoEntity);
      if (cmd == kSetSinkInputVolume) {
        Cvolume v;
        if (!t.cvolume(&v) || v.channels == 0) return invalid();
        volume = pulse_volume_to_linear(v.max());
      } else if (!t.boolean(&mute)) {
        return invalid();
      }
      graph_.set_volume(index, volume, mute);
      return send_simple_reply(c, tag);
    }
    case kKillSinkInput: {
      uint32_t index = 0;
      if (!t.u32(&index)) return invalid();
      if (index == sink || !graph_.playback_of(index)) return send_error(c, tag, kErrNoEntity);
      graph_.destroy_node(index, false);
      return send_simple_reply(c, tag);
    }
    case kSetDefaultSink: case kSuspendSink: case kSetDefaultSource: case kSuspendSource: return send_simple_reply(c, tag);
    case kStat: {
      const uint64_t m = begin_command(c, kReply, tag);
      TagWriter w(c.outbox);
      for (int i = 0; i < 5; ++i) w.u32(0);
      end_command(c, m);
      return;
    }
    case kExtension: return send_error(c, tag, kErrNoExtension);
    case kCreateRecordStream: case kCreateUploadStream: case kLoadModule: case kUpdatePlaybackStreamSampleRate: case kMoveSinkInput:
    case kSetSinkPort: case kSetCardProfile: case kSendObjectMessage:
      return send_error(c, tag, kErrNotSupported);
    case kExit: case kKillClient: case kUnloadModule: return send_error(c, tag, kErrAccess);
    default: return send_error(c, tag, kErrCommand);
  }
}

void PulseServer::create_playback(Client& c, uint32_t tag, TagReader& t) {
  SampleSpec spec;
  ChannelMap map;
  Cvolume volume;
  uint32_t sink_index = 0, maxlength = 0, tlength = 0, prebuf = 0, minreq = 0, syncid = 0;
  StringView sink_name, legacy_name;
  bool corked = false, b = false, muted = false, volume_set = false, muted_set = false;
  Properties props(allocator_);
  auto invalid = [&] { send_error(c, tag, kErrProtocol); };
  if (c.version < 13 && !t.string(&legacy_name)) return invalid();
  if (!t.sample_spec(&spec) || !t.channel_map(&map) || !t.u32(&sink_index) || !t.string(&sink_name) || !t.u32(&maxlength) || !t.boolean(&corked) ||
      !t.u32(&tlength) || !t.u32(&prebuf) || !t.u32(&minreq) || !t.u32(&syncid) || !t.cvolume(&volume))
    return invalid();
  if (c.version >= 12)
    for (int i = 0; i < 7; ++i)      // no_remap, no_remix, fix_format, fix_rate, fix_channels, no_move, variable_rate
      if (!t.boolean(&b)) return invalid();
  if (c.version >= 13 && (!t.boolean(&muted) || !t.boolean(&b) || !t.proplist(&props))) return invalid();      // muted, adjust_latency, proplist
  if (c.version >= 14 && (!t.boolean(&volume_set) || !t.boolean(&b))) return invalid();      // volume_set, early_requests
  if (c.version >= 15 && (!t.boolean(&muted_set) || !t.boolean(&b) || !t.boolean(&b))) return invalid();      // muted_set, dont_inhibit_auto_suspend, fail_on_suspend
  if (c.version >= 17 && !t.boolean(&b)) return invalid();      // relative_volume
  if (c.version >= 18 && !t.boolean(&b)) return invalid();      // passthrough
  if (c.version >= 21) {
    uint8_t formats = 0;
    if (!t.u8(&formats)) return invalid();
    for (uint8_t i = 0; i < formats; ++i) {
      uint8_t encoding = 0;
      Properties fp(allocator_);
      if (!t.format_info(&encoding, &fp)) return invalid();
    }
  }
  if (c.version < 13 && !legacy_name.empty()) props.set("media.name", legacy_name);
  if (!spec.valid()) return send_error(c, tag, kErrInvalid);
  if (map.channels == 0) map = default_map(spec.channels);
  if (map.channels != spec.channels) return send_error(c, tag, kErrInvalid);
  if (sink_index != pulse::kInvalidIndex && sink_index != graph_.sink_id()) return send_error(c, tag, kErrNoEntity);
  if (!sink_name.empty() && !is_sink_name(sink_name)) return send_error(c, tag, kErrNoEntity);
  PcmFormat format;
  if (!to_pcm_format(spec, &format)) return send_error(c, tag, kErrNotSupported);
  if (c.streams.count() >= kMaxStreamsPerClient) return send_error(c, tag, kErrTooLarge);

  // the client's own properties go to its streams too (application.name, .icon_name, ...)
  props.update(c.props, Properties::Mode::kMerge);
  props.set("client.api", "pipewire-pulse");
  SharedPointer<PlaybackStream> playback;
  const uint32_t node = graph_.create_playback(c.graph_client, format, props, &playback);
  if (!node) return send_error(c, tag, kErrTooLarge);

  UniquePointer<Stream> s = UniquePointer<Stream>::make(allocator_, allocator_);
  if (!s) {
    graph_.destroy_node(node);
    return send_error(c, tag, kErrTooLarge);
  }
  const uint32_t frame = spec.frame_size();
  s->channel = c.next_channel++;
  s->node = node;
  s->playback = playback;
  s->spec = spec;
  s->map = map;
  const uint32_t lo = ms_bytes(spec, 20), hi = ms_bytes(spec, 2000);
  s->tlength = round_frames(tlength == 0xFFFFFFFFu || tlength == 0 ? ms_bytes(spec, 200) : (tlength < lo ? lo : tlength > hi ? hi : tlength), frame);
  s->minreq = round_frames(minreq == 0xFFFFFFFFu || minreq == 0 ? s->tlength / 4 : (minreq > s->tlength / 2 ? s->tlength / 2 : minreq), frame);
  s->prebuf = prebuf == 0xFFFFFFFFu ? s->minreq : (prebuf > s->tlength ? s->tlength : prebuf);
  s->maxlength = maxlength == 0xFFFFFFFFu || maxlength < s->tlength ? s->tlength * 4 : maxlength;
  if (s->maxlength > kMaxPacket) s->maxlength = kMaxPacket < s->tlength ? s->tlength : kMaxPacket;
  s->requested = s->tlength;
  s->corked = corked;
  if (corked) graph_.set_paused(node, true);
  float linear = 1.0f;
  bool mute_now = false;
  graph_.get_volume(node, &linear, &mute_now);
  if ((c.version < 14 || volume_set) && volume.channels > 0) linear = pulse_volume_to_linear(volume.max());
  if (c.version < 15 || muted_set) mute_now = muted;
  graph_.set_volume(node, linear, mute_now);

  const uint64_t m = begin_command(c, kReply, tag);
  TagWriter w(c.outbox);
  w.u32(s->channel);
  w.u32(node);
  w.u32(s->requested);
  if (c.version >= 9) {
    w.u32(s->maxlength);
    w.u32(s->tlength);
    w.u32(s->prebuf);
    w.u32(s->minreq);
  }
  if (c.version >= 12) {
    w.sample_spec(spec);
    w.channel_map(map);
    w.u32(graph_.sink_id());
    w.string(kSinkName);
    w.boolean(false);
  }
  if (c.version >= 13) w.usec(uint64_t{s->tlength} / frame * 1000000 / spec.rate);
  if (c.version >= 21) {
    Properties none(allocator_);
    w.format_info(1, none);      // PCM
  }
  end_command(c, m);
  if (!c.streams.push_back(util::move(s))) graph_.destroy_node(node);
}

void PulseServer::delete_stream(Client& c, uint32_t channel, bool killed) {
  for (uint64_t i = 0; i < c.streams.count(); ++i) {
    Stream& s = *c.streams[i];
    if (s.channel != channel) continue;
    if (killed) {
      const uint64_t m = begin_command(c, kPlaybackStreamKilled, kTagNone);
      TagWriter(c.outbox).u32(channel);
      end_command(c, m);
    } else {
      graph_.destroy_node(s.node, false);
    }
    c.streams.remove_at(i);
    return;
  }
}

// ---- audio and flow control ------------------------------------------------------------------------------------------------------------------------

void PulseServer::audio(Client& c, uint32_t channel, ConstByteView data) {
  Stream* s = stream(c, channel);
  if (!s) return;      // (a stream that was just deleted: its last packets are dropped)
  const uint64_t frame = s->spec.frame_size();
  const uint64_t held = s->pending.size() - s->pending_offset;
  if (held + data.size() > uint64_t{s->maxlength} + s->tlength) {
    // more than it was allowed to send: what does not fit is dropped (PulseAudio does the same)
    const uint64_t room = uint64_t{s->maxlength} + s->tlength > held ? uint64_t{s->maxlength} + s->tlength - held : 0;
    data = data.first(room - room % (frame ? frame : 1));
  }
  // keep the pending buffer short: what was taken goes first
  if (s->pending_offset > 0 && s->pending_offset * 2 >= s->pending.size()) {
    drop_front(s->pending, s->pending_offset);
    s->pending_offset = 0;
  }
  if (!s->pending.append(data)) return;
  s->write_index += data.size();
  s->requested = s->requested > data.size() ? s->requested - data.size() : 0;
  feed(*s);
}

void PulseServer::feed(Stream& s) {
  while (s.pending_offset < s.pending.size()) {
    const uint32_t taken = s.playback->write(s.pending.view().subview(s.pending_offset));
    if (taken == 0) break;
    s.pending_offset += taken;
  }
  if (s.pending_offset == s.pending.size()) {
    s.pending.clear();
    s.pending_offset = 0;
  }
}

void PulseServer::flow(Client& c, Stream& s) {
  feed(s);
  if (!s.started && s.playback->played_bytes() > 0) {
    s.started = true;
    if (c.version >= 13) {
      const uint64_t m = begin_command(c, kStarted, kTagNone);
      TagWriter(c.outbox).u32(s.channel);
      end_command(c, m);
    }
  }
  const uint64_t held = s.pending.size() - s.pending_offset;
  const uint64_t buffered = held + s.playback->buffered_bytes();
  if (!s.corked && s.tlength > buffered + s.requested) {
    uint64_t missing = s.tlength - buffered - s.requested;
    const uint64_t frame = s.spec.frame_size();
    if (frame) missing -= missing % frame;
    if (missing >= s.minreq) {
      const uint64_t m = begin_command(c, kRequest, kTagNone);
      TagWriter w(c.outbox);
      w.u32(s.channel);
      w.u32(static_cast<uint32_t>(missing));
      end_command(c, m);
      s.requested += missing;
    }
  }
  if (s.drains.count() && held == 0 && s.playback->drained()) {
    for (uint32_t tag : s.drains) send_simple_reply(c, tag);
    s.drains.clear();
  }
}

// ---- introspection ---------------------------------------------------------------------------------------------------------------------------------

void PulseServer::fill_sink(Client& c, TagWriter& w) {
  graph_.visit(graph_.sink_id(), [&](const GraphObject& o) {
    w.u32(o.id);
    w.string(kSinkName);
    w.string(o.props.get("node.description"));
    w.sample_spec(SampleSpec{pulse::kS16Le, 2, hardware::kAudioSampleRate});
    w.channel_map(default_map(2));
    w.u32(pulse::kInvalidIndex);      // module
    w.cvolume(flat_volume(2, linear_to_pulse_volume(o.volume)));
    w.boolean(o.mute);
    w.u32(pulse::kInvalidIndex);      // monitor source
    w.string_null();
    w.usec(kSinkLatencyUs);
    w.string("tcos-mixer");
    w.u32(kSinkFlags);
    if (c.version >= 13) {
      w.proplist(o.props);
      w.usec(kSinkLatencyUs);
    }
    if (c.version >= 15) {
      w.volume(pulse::kVolumeNorm);      // base volume
      w.u32(o.state == kNodeStateRunning ? kSinkStateRunning : kSinkStateIdle);
      w.u32(pulse::kVolumeNorm + 1);     // volume steps
      w.u32(pulse::kInvalidIndex);       // card
    }
    if (c.version >= 16) {
      w.u32(0);      // ports
      w.string_null();
    }
    if (c.version >= 21) {
      w.u8(1);
      Properties none(allocator_);
      w.format_info(1, none);
    }
  });
}

void PulseServer::fill_sink_input(Client& c, TagWriter& w, const GraphObject& o) {
  const SampleSpec spec = to_spec(o.format);
  w.u32(o.id);
  w.string(o.props.get("media.name"));
  w.u32(pulse::kInvalidIndex);      // module
  w.u32(o.client ? o.client : pulse::kInvalidIndex);
  w.u32(graph_.sink_id());
  w.sample_spec(spec);
  w.channel_map(default_map(spec.channels));
  w.cvolume(flat_volume(spec.channels, linear_to_pulse_volume(o.volume)));
  w.usec(kSinkLatencyUs);
  w.usec(kSinkLatencyUs);
  w.string("speex-float-1");
  w.string(o.origin == Origin::Windows ? "tcos-winmm" : o.origin == Origin::Native ? "tcos-native" : "protocol-pulse");
  if (c.version >= 11) w.boolean(o.mute);
  if (c.version >= 13) w.proplist(o.props);
  if (c.version >= 19) w.boolean(o.state != kNodeStateRunning);
  if (c.version >= 20) {
    w.boolean(true);
    w.boolean(true);
  }
  if (c.version >= 21) {
    Properties none(allocator_);
    w.format_info(1, none);
  }
}

void PulseServer::fill_client(Client& c, TagWriter& w, const GraphObject& o) {
  w.u32(o.id);
  w.string(o.props.get("application.name"));
  w.u32(pulse::kInvalidIndex);
  w.string(o.props.get("pipewire.protocol"));
  if (c.version >= 13) w.proplist(o.props);
}

void PulseServer::send_subscription_events(Client& c) {
  // facility (pulse/def.h: sink 0, sink input 2, client 5) and the masks that enable them
  constexpr uint32_t kSink = 0, kSinkInput = 2, kClientFacility = 5, kNew = 0x00, kChange = 0x10, kRemove = 0x20;
  const uint32_t sink = graph_.sink_id();
  auto facility_of = [&](const GraphEntry& e, uint32_t* out) {
    if (e.type == ObjectType::Client) *out = kClientFacility;
    else if (e.type == ObjectType::Node) *out = e.id == sink ? kSink : kSinkInput;
    else return false;
    return (c.subscriptions & (1u << *out)) != 0;
  };
  auto event = [&](uint32_t facility, uint32_t type, uint32_t index) {
    const uint64_t m = begin_command(c, kSubscribeEvent, kTagNone);
    TagWriter w(c.outbox);
    w.u32(facility | type);
    w.u32(index);
    end_command(c, m);
  };
  const ArrayView<const GraphEntry> was = c.known.view(), now = snapshot_.view();
  uint64_t i = 0, j = 0;
  uint32_t facility = 0;
  while (i < was.size() || j < now.size()) {
    if (j >= now.size() || (i < was.size() && was[i].id < now[j].id)) {
      if (facility_of(was[i], &facility)) event(facility, kRemove, was[i].id);
      ++i;
    } else if (i >= was.size() || now[j].id < was[i].id) {
      if (facility_of(now[j], &facility)) event(facility, kNew, now[j].id);
      ++j;
    } else {
      if (now[j].changed != was[i].changed && facility_of(now[j], &facility)) event(facility, kChange, now[j].id);
      ++i;
      ++j;
    }
  }
  c.known.clear();
  for (const GraphEntry& e : now) c.known.push_back(e);
}

void PulseServer::tick() {
  graph_.refresh();
  const bool changed = graph_.serial() != last_serial_;
  if (changed) {
    last_serial_ = graph_.serial();
    graph_.snapshot(&snapshot_);
  }
  for (UniquePointer<Client>& p : clients_.view()) {
    Client& c = *p;
    if (c.closing) continue;
    // streams whose node is gone (killed from another client, pactl kill-sink-input)
    for (uint64_t i = c.streams.count(); i > 0; --i) {
      Stream& s = *c.streams[i - 1];
      if (!graph_.playback_of(s.node)) delete_stream(c, s.channel, true);
    }
    for (UniquePointer<Stream>& s : c.streams.view()) flow(c, *s);
    if (changed && c.subscriptions) send_subscription_events(c);
    if (c.outbox.size() > kMaxOutbox) c.closing = true;
  }
}

}  // namespace pipewire
