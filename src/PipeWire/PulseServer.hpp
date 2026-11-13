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
// the native and Windows programs that play.
//
// Recording (parecord, browsers' getUserMedia through libpulse, voice chat): record streams (create, delete, cork, flush, latency, buffer attributes,
// name and property updates) read the graph's source - the capture hub's microphone - and the server sends what they record in packets on the stream's
// channel, a fragment (fragsize) at a time; source and source-output queries, lists, volume and mute, LOOKUP_SOURCE, and their subscription events.
// "pactl list source-outputs" shows the Windows programs that record as well. Modules: module-null-sink (LOAD_MODULE makes a virtual sink with
// sink_name, sink_properties, rate, channels and channel_map, as pipewire-pulse does; it stays until UNLOAD_MODULE, whoever loaded it), listed with
// GET_MODULE_INFO(_LIST); and module-loopback (a source or a sink's monitor into a sink, latency_msec), module-remap-sink (a sink looped into its
// master, left and right swapped when the channel maps say so) and module-combine-sink (a sink looped into each of its slaves), built on
// Loopback.hpp and pumped from tick(); module-virtual-sink (a remap sink in the same order), module-remap-source and module-virtual-source (a virtual
// source hearing its master; virtual-source's uplink_sink a sink it hears too), module-echo-cancel (EchoCancelLoop: a sink into its master and a source
// with that sink's audio cancelled out of the microphone), and module-always-sink and module-switch-on-connect (loaded once, nothing to do: the one sink
// is always there and follows plugged-in devices); other modules are refused. Not served (an error answer): upload streams, samples, cards, extensions. The sink's monitor (tcos_output.stereo.monitor, index sink | 1 << 24 as pipewire-pulse numbers it) is a source too: recording it
// records what the mixer plays (OBS, "record what you hear").
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/Loopback.hpp"
#include "PipeWire/ProtocolServer.hpp"
#include "PipeWire/Pipe.hpp"
#include "PipeWire/Rtp.hpp"
#include "PipeWire/StreamRestore.hpp"
#include "PipeWire/Tagstruct.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

// PulseAudio module arguments ("sink_name=x sink_properties='device.description=\"My sink\"'"): key=value pairs separated by white space, a value
// in single or double quotes when it holds spaces, a backslash taking the next character as it is. Each pair is set in *out (PulseAudio key names
// for properties - device.description, media.role, ... - become PipeWire's). False when the text is not such a list (a key without a value, a
// quote not closed) or there are too many.
bool parse_module_arguments(StringView text, Properties* out);

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
    SharedPointer<PlaybackStream> playback;      // a playback stream; else
    SharedPointer<RecordStream> record;          // a record stream
    pulse::SampleSpec spec;
    pulse::ChannelMap map;
    uint32_t maxlength = 0, tlength = 0, prebuf = 0, minreq = 0, fragsize = 0;
    uint64_t requested = 0;      // bytes the client may still send without being asked
    ByteBuffer pending;          // came, not yet taken by the playback stream
    uint64_t pending_offset = 0;
    List<uint32_t> drains;       // tags of DRAIN requests to answer once it has played out
    uint64_t write_index = 0;    // playback: bytes the client sent; record: bytes sent to the client
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
    bool restore_events = false;      // (module-stream-restore's SUBSCRIBE)
    uint32_t next_channel = 0;
    ByteBuffer inbox;
    ByteBuffer outbox;
    List<UniquePointer<Stream>> streams;
    List<GraphEntry> known;      // for subscription events
    Properties props;
  };

  Client* find(uint32_t handle);
  // The playback (or with `record`, the record) stream on `channel`.
  Stream* stream(Client& c, uint32_t channel, bool record = false);
  void packet(Client& c, uint32_t channel, ConstByteView payload, int64_t offset, uint32_t flags);
  void command(Client& c, ConstByteView payload);
  void audio(Client& c, uint32_t channel, ConstByteView data);

  // Command packets out: begin_command() writes the descriptor and the command/tag, end_command() patches the length.
  uint64_t begin_command(Client& c, uint32_t command, uint32_t tag);
  void end_command(Client& c, uint64_t start);
  void send_error(Client& c, uint32_t tag, uint32_t error);
  void send_simple_reply(Client& c, uint32_t tag);

  void create_playback(Client& c, uint32_t tag, pulse::TagReader& t);
  void create_record(Client& c, uint32_t tag, pulse::TagReader& t);
  // Sends what a record stream has recorded, whole fragments.
  void send_recorded(Client& c, Stream& s);
  void delete_stream(Client& c, uint32_t channel, bool killed);
  void feed(Stream& s);
  void flow(Client& c, Stream& s);

  void fill_sink(Client& c, pulse::TagWriter& w, uint32_t sink);
  // The microphone's source, or (`monitor`) the sink's monitor source.
  // `of_sink`: (a monitor) the sink whose monitor it is (0: the speakers')
  void fill_source(Client& c, pulse::TagWriter& w, bool monitor, uint32_t of_sink = 0);
  void fill_source_output(Client& c, pulse::TagWriter& w, const GraphObject& o);
  void fill_sink_input(Client& c, pulse::TagWriter& w, const GraphObject& o);
  void fill_client(Client& c, pulse::TagWriter& w, const GraphObject& o);
  bool is_sink_name(StringView name) const;
  bool is_source_name(StringView name) const;
  // The source a request names (by name unless `null_name`, else by index): true if there is one, *monitor for the sink's monitor.
  // `*of_sink`: (a monitor) the sink it is the monitor of (the speakers' or a virtual sink's node). A virtual source is recorded as a virtual
  // sink's monitor is: *monitor set, *of_sink its node (Graph::is_virtual_source tells them apart).
  bool find_source(uint32_t index, StringView name, bool null_name, bool* monitor, uint32_t* of_sink = nullptr) const;
  // The sink an index or a name says: the speakers' node, or a virtual sink's (the adapter factory's null sinks, named by their node.name); 0 for none.
  uint32_t find_sink(uint32_t index, StringView name, bool null_name) const;
  // A virtual sink (source) of this node.name (0: none).
  uint32_t virtual_sink_named(StringView name) const;
  uint32_t virtual_source_named(StringView name) const;
  // The index a source has to clients: a virtual source's node, a monitor's sink with kMonitorFlag, else the microphone's node.
  uint32_t index_of_source(bool monitor, uint32_t of_sink) const;
  void send_subscription_events(Client& c);

  Graph& graph_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Client>> clients_;
  List<GraphEntry> snapshot_;
  uint64_t last_serial_ = 0;
  uint32_t next_handle_ = 1;

  // ---- loaded modules (module-null-sink: a virtual sink each, the kernel's)
 public:
  static constexpr uint32_t kModuleFlag = 1u << 29;      // (module indexes, as pipewire-pulse numbers them)
  static constexpr uint32_t kMaxModules = 16;
  // Where module-stream-restore keeps its entries (null: only while it is loaded). Set before it is loaded.
  void set_stream_restore_storage(IStreamRestoreStorage* storage) { restore_storage_ = storage; }
  // The network module-rtp-send and -recv use (null: they are refused). Set before they are loaded.
  void set_rtp_network(IRtpNetwork* network) { rtp_network_ = network; }
  // The named pipes module-pipe-sink and -source use (null: they are refused). Set before they are loaded.
  void set_pipe_files(IPipeFiles* files) { pipe_files_ = files; }
  StreamRestore* stream_restore() { return restore_.get(); }
  // What LOAD_MODULE does: the module's index, or 0 with *error a PulseAudio error.
  uint32_t load_module(StringView name, StringView arguments, uint32_t* error);
  bool unload_module(uint32_t index);
  // The module that made `node` (kInvalidIndex: none).
  uint32_t module_of(uint32_t node);

 private:
  struct Module {
    explicit Module(allocators::IAllocator& a) : name(a), arguments(a), loops(a) {}
    uint32_t index = 0;
    // (module-null-sink, -remap-sink, -virtual-sink, -combine-sink, -pipe-sink: its sink; -pipe-source too: its source; -remap-source, -virtual-source: its source; -echo-cancel: its
    // source; module-loopback and the policy ones: none)
    uint32_t node = 0;
    uint32_t extra = 0;      // (module-echo-cancel: its sink; -virtual-source with uplink_sink: that sink)
    String name, arguments;
    List<UniquePointer<Loopback>> loops;      // (module-loopback: one; -remap-sink: into its master; -combine-sink: one a slave; -*-source: from its master)
    UniquePointer<EchoCancelLoop> echo;       // (module-echo-cancel)
    UniquePointer<RtpSender> rtp_send;        // (module-rtp-send)
    UniquePointer<RtpReceiver> rtp_recv;      // (module-rtp-recv)
    UniquePointer<PipeSink> pipe_sink;        // (module-pipe-sink)
    UniquePointer<PipeSource> pipe_source;    // (module-pipe-source)
  };
  // A virtual sink (`source`: source) of a module's arguments, the kernel's.
  uint32_t module_node(const Properties& args, bool source, const char* default_name, uint32_t* error);
  void destroy_module_nodes(Module& m);
  void prune_modules();      // (forgets the ones whose sink went another way: pw-cli destroy, ...)
  void fill_module(pulse::TagWriter& w, uint32_t version, const Module& m);
  List<UniquePointer<Module>> modules_;
  uint32_t next_module_ = 0;
  // module-stream-restore (while it is loaded): the remembered volumes, its module index, and its extension
  IStreamRestoreStorage* restore_storage_ = nullptr;
  IRtpNetwork* rtp_network_ = nullptr;
  IPipeFiles* pipe_files_ = nullptr;
  uint32_t rtp_seed_ = 0x9E3779B9u;
  UniquePointer<StreamRestore> restore_;
  uint32_t restore_module_ = 0;
  void extension(Client& c, uint32_t tag, pulse::TagReader& t);
  void send_restore_events();
};

// PulseAudio's volumes are cubic: (volume / PA_VOLUME_NORM)^3 is the linear factor PipeWire and the mixer use.
float pulse_volume_to_linear(uint32_t volume);
uint32_t linear_to_pulse_volume(float linear);

}  // namespace pipewire
