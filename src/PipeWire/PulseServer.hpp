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

// The PulseAudio native protocol over the PipeWire graph, as pipewire-pulse serves it: what libpulse programs (paplay, pactl, browsers, SDL, games, ...)
// connect to at $XDG_RUNTIME_DIR/pulse/native.
//
// A packet is a 20-byte descriptor - length, channel, offset (high, low), flags, all big endian - and a payload. Channel 0xFFFFFFFF carries commands, a
// tagstruct (Tagstruct.hpp) of u32 command, u32 tag and arguments; any other channel is audio for that playback stream. Shared memory is not offered
// (the AUTH reply has no SHM flag), so libpulse sends the audio inside the packets.
//
// Served: AUTH, SET_CLIENT_NAME, playback streams (create, delete, drain, cork, flush, trigger, prebuf, latency, buffer attributes, name and property
// updates) with PulseAudio's flow control (REQUEST, STARTED, PLAYBACK_STREAM_KILLED), the server/sink/sink-input/client queries and lists, sink and
// sink-input volume and mute, SUBSCRIBE with its events, STAT, LOOKUP_SINK. Every stream is a node of the graph, so "pactl list sink-inputs" also shows
// the native and Windows programs that play. Not served (an error answer): record and upload streams, samples, modules, cards, extensions.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/ProtocolServer.hpp"
#include "PipeWire/Tagstruct.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

class PulseServer final : public IProtocolServer {
 public:
  static constexpr uint32_t kProtocolVersion = 35;
  static constexpr uint32_t kDescriptorSize = 20;
  static constexpr uint32_t kMaxPacket = 4u << 20;
  static constexpr uint64_t kMaxOutbox = 8u << 20;
  static constexpr uint32_t kMaxClients = 128;
  static constexpr uint32_t kMaxStreamsPerClient = 32;
  static constexpr uint32_t kCommandChannel = 0xFFFFFFFFu;

  PulseServer(Graph& graph, allocators::IAllocator& allocator);
  ~PulseServer();

  uint32_t open_client(uint64_t pid) override;
  void receive(uint32_t client, ConstByteView bytes) override;
  bool take_output(uint32_t client, ByteBuffer* out) override;
  void close_client(uint32_t client) override;
  void tick() override;
  uint64_t tick_interval_us() const override { return 10000; }

  uint32_t client_count() const { return static_cast<uint32_t>(clients_.count()); }

 private:
  struct Stream {
    explicit Stream(allocators::IAllocator& a) : pending(a), drains(a) {}
    uint32_t channel = 0;
    uint32_t node = 0;
    SharedPointer<PlaybackStream> playback;
    pulse::SampleSpec spec;
    pulse::ChannelMap map;
    uint32_t maxlength = 0, tlength = 0, prebuf = 0, minreq = 0;
    uint64_t requested = 0;      // bytes the client may still send without being asked
    ByteBuffer pending;          // came, not yet taken by the playback stream
    uint64_t pending_offset = 0;
    List<uint32_t> drains;       // tags of DRAIN requests to answer once it has played out
    uint64_t write_index = 0;
    bool corked = false;
    bool started = false;
  };
  struct Client {
    explicit Client(allocators::IAllocator& a) : inbox(a), outbox(a), streams(a), known(a), props(a) {}
    uint32_t handle = 0;
    uint64_t pid = 0;
    uint32_t graph_client = 0;
    uint32_t version = 0;
    bool authorized = false;
    bool closing = false;
    uint32_t subscriptions = 0;
    uint32_t next_channel = 0;
    ByteBuffer inbox;
    ByteBuffer outbox;
    List<UniquePointer<Stream>> streams;
    List<GraphEntry> known;      // for subscription events
    Properties props;
  };

  Client* find(uint32_t handle);
  Stream* stream(Client& c, uint32_t channel);
  void packet(Client& c, uint32_t channel, ConstByteView payload, int64_t offset, uint32_t flags);
  void command(Client& c, ConstByteView payload);
  void audio(Client& c, uint32_t channel, ConstByteView data);

  // Command packets out: begin_command() writes the descriptor and the command/tag, end_command() patches the length.
  uint64_t begin_command(Client& c, uint32_t command, uint32_t tag);
  void end_command(Client& c, uint64_t start);
  void send_error(Client& c, uint32_t tag, uint32_t error);
  void send_simple_reply(Client& c, uint32_t tag);

  void create_playback(Client& c, uint32_t tag, pulse::TagReader& t);
  void delete_stream(Client& c, uint32_t channel, bool killed);
  void feed(Stream& s);
  void flow(Client& c, Stream& s);

  void fill_sink(Client& c, pulse::TagWriter& w);
  void fill_sink_input(Client& c, pulse::TagWriter& w, const GraphObject& o);
  void fill_client(Client& c, pulse::TagWriter& w, const GraphObject& o);
  bool is_sink_name(StringView name) const;
  void send_subscription_events(Client& c);

  Graph& graph_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Client>> clients_;
  List<GraphEntry> snapshot_;
  uint64_t last_serial_ = 0;
  uint32_t next_handle_ = 1;
};

// PulseAudio's volumes are cubic: (volume / PA_VOLUME_NORM)^3 is the linear factor PipeWire and the mixer use.
float pulse_volume_to_linear(uint32_t volume);
uint32_t linear_to_pulse_volume(float linear);

}  // namespace pipewire
