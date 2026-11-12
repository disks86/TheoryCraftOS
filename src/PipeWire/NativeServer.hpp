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

// PipeWire's native protocol (version 3, what libpipewire 0.3 and 1.x speak) over the graph: what pw-dump, pw-cli, pw-top and the session tools see.
//
// A message is a 16-byte header - u32 object id, u32 (opcode << 24 | payload size), u32 sequence, u32 number of fds - and a payload that is one SPA
// Struct of arguments (Pod.hpp), possibly followed by a footer Struct that is ignored here. Object ids are the connection's own: 0 is the core, 1 the
// client; the program picks the ids of the proxies it makes (get_registry, bind). Events go back on those ids.
//
// What is served: Core hello / sync / pong / get_registry / destroy, Registry bind (Core, Client, Device, Node, Port, Link, Metadata) with the global and
// global_remove events kept up to date from graph snapshots, the info event of every bound object (sent again when it changes), Client update_properties
// and get_permissions, Node and Port enum_params / subscribe_params (EnumFormat, Format, Props; MIDI ports say application/control, video ones their
// producer's format), Node set_param(Props) for volume and mute (the stream's volume, the sink's is the master volume), and the "default" metadata's
// properties.
//
// Core create_object of the "client-node" factory (what libpipewire's pw_stream and pw_filter export their nodes with: pw-play, pw-record, pw-cat,
// pw-midiplay, camera and screen-sharing programs, native PipeWire programs): see NativeClientNode.cpp. The server plays the session manager and the
// driver itself: it configures the stream's adapter (PortConfig: audio converted to F32 at the stream's rate, video passed through), sets each port's
// Format, gives it buffers with the metadata it asks for in a memfd (or takes the memfds a port allocates itself: port_buffers), the ports' io areas
// and the node's activation record in others, starts it, and then runs its cycles: it fills in the position, marks the activation TRIGGERED and
// signals the client's eventfd; once the client has marked it FINISHED what its outputs produced goes to the graph (a PlaybackStream, the MIDI bus, a
// video channel, a virtual sink) and its inputs were filled before (a RecordStream, the bus, a producer's frames). A node that drives itself
// (node.driver: video, MIDI) is never triggered; its buffers are taken as its io areas say. Core create_object of the "adapter" factory with
// factory.name support.null-audio-sink makes a virtual sink streams can play into and record from. Other factories answer -ENOTSUP. Without a
// descriptor maker (a transport that cannot pass descriptors) client-node is refused too.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/MidiBus.hpp"
#include "PipeWire/Pod.hpp"
#include "PipeWire/VideoChannel.hpp"
#include "PipeWire/ProtocolServer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

class NativeServer final : public IProtocolServer {
 public:
  static constexpr uint32_t kHeaderSize = 16;
  static constexpr uint32_t kMaxPayload = 0xFFFFFF;      // what 24 bits can say
  static constexpr uint64_t kMaxInbox = 4u << 20;
  static constexpr uint64_t kMaxOutbox = 8u << 20;
  static constexpr uint32_t kMaxResources = 8192;
  static constexpr uint32_t kMaxClients = 128;

  // `clock` (may be null) is the time in nanoseconds (monotonic): the clocked client nodes (MIDI, video, virtual sinks) run on it. Without one the server
  // counts its ticks (5 ms each) instead.
  NativeServer(Graph& graph, allocators::IAllocator& allocator, uint64_t (*clock)() = nullptr);
  ~NativeServer();

  uint32_t open_client(uint64_t pid) override;
  void receive(uint32_t client, ConstByteView bytes) override;
  void receive_with_fds(uint32_t client, ConstByteView bytes, List<Fd>& fds) override;
  bool take_output(uint32_t client, ByteBuffer* out) override;
  void close_client(uint32_t client) override;
  void tick() override;
  // (1 ms while programs' MIDI waits for its time to sound)
  uint64_t tick_interval_us() const override { return midi_waiting_ ? 1000 : running_nodes_ ? 5000 : 50000; }
  bool take_output_with_fds(uint32_t client, ByteBuffer* out, List<Fd>* fds) override;
  void set_descriptor_maker(IDescriptorMaker* maker) override { maker_ = maker; }

  // The client nodes now running (for tests and statistics).
  uint32_t running_nodes() const { return running_nodes_; }
  uint32_t virtual_sinks() const { return static_cast<uint32_t>(virtuals_.count()); }
  static constexpr uint32_t kMaxPorts = 16;           // of one client node
  static constexpr uint32_t kMaxInFds = 256;          // descriptors a client may have sent ahead of their messages

  uint32_t client_count() const { return static_cast<uint32_t>(clients_.count()); }

 // (the client nodes' types, public for the helpers of NativeClientNode.cpp)
  // What a port of a client node carries (from its EnumFormat).
  enum class Media : uint8_t { Unknown, Audio, Midi, Video };
  // A port of a client node (NativeClientNode.cpp): its formats, the metadata and buffers the server gave it, and where its data goes.
  struct ClientPort {
    static constexpr uint32_t kMaxMetas = 8;
    explicit ClientPort(allocators::IAllocator& a) : enum_format(a), client_data(a), ump_in(a) {}
    uint32_t direction = 0, id = 0;
    uint32_t index = 0;            // its place in the node's io area (8 bytes each)
    uint64_t flags = 0;            // SPA_PORT_FLAG_* from its info
    Media media = Media::Unknown;
    bool ump = false;              // a MIDI port of UMP (control.ump): what it is given is UMP too
    bool dsp = false;              // an audio port of one float channel (a pw_filter's "32 bit float mono audio")
    ByteBuffer enum_format;        // its first EnumFormat as it came (fixated into its Format)
    bool format_sent = false, buffers_sent = false, data_ready = false, wants_alloc = false;
    // what its Buffers and Meta params ask for (0: not said)
    uint32_t wanted_buffers = 0, wanted_blocks = 0, wanted_size = 0, wanted_stride = 0, wanted_types = 0;
    struct Meta {
      uint32_t type = 0, size = 0, offset = 0;      // offset: in each buffer's part
    };
    Meta metas[kMaxMetas];
    uint32_t n_metas = 0;
    // the format set
    PcmFormat pcm{SampleFormat::F32Le, 48000, 2};
    VideoFormat video;
    // its buffers: in one memfd of the server's, n parts of `buffer_stride`: the metas, the chunk, then the data (unless the client gives the data)
    Fd buffers;
    uint32_t buffers_mem = 0, n_buffers = 0, buffer_stride = 0, chunk_offset = 0, data_offset = 0, data_size = 0, data_type = 0;
    uint32_t blocks = 1;           // data blocks of each buffer (video: one a plane, as the port asks; else 1), each with its chunk (chunk_offset + 16 k) and
                                   // its data (data_offset + data_size k)
    bool renegotiate = false;      // its format is to change (the program's new EnumFormat, or a watcher's producer's new format): Format and buffers again
    bool alloc = false;            // the client allocates the data (port_use_buffers ALLOC, answered with port_buffers)
    struct ClientData {
      Fd fd;
      uint32_t mapoffset = 0, maxsize = 0;
    };
    List<ClientData> client_data;  // (alloc) each buffer's data blocks, as port_buffers gave them (buffer i's block k at i * blocks + k)
    uint32_t next_buffer = 0;
    uint64_t seq = 0;              // the SPA_META_Header seq the server writes next
    uint64_t video_seen = 0;       // (a watcher) the producer's last frame given
    UmpDecoder ump_in;             // (a UMP output) its system exclusive pieces put together
  };
 private:
  struct Resource {
    uint32_t id;           // the program's id for it
    ObjectType type;
    bool registry;         // a registry (type unused)
    uint32_t global;       // the graph object it is bound to
    uint32_t version;
    uint64_t seen;         // the object's graph serial when its info was last sent
    bool subscribed[3];    // node/port param subscriptions: Props, EnumFormat, Format
    bool client_node = false;      // a ClientNode (create_object): its state is in Client::nodes
  };
  // A node a program exported with the client-node factory (NativeClientNode.cpp).
  struct ClientNode {
    explicit ClientNode(allocators::IAllocator& a) : props(a), ports(a), scratch(a) {}
    uint32_t resource = 0;         // the program's id of the ClientNode proxy
    uint32_t node = 0;             // the graph's node (0 until what it is is known)
    uint32_t direction = 1;        // of the node (its media class): 1 output (it plays or shows), 0 input (it records or watches)
    Media media = Media::Unknown;  // of the node (its media class)
    bool active = false, started = false, config_sent = false;
    bool raw = false;              // ports came before it was active: not an adapter (a pw_filter, a MIDI stream); no PortConfig
    bool driving = false;          // the program runs its own cycles (node.driver: a video or MIDI driver); the server only takes and gives buffers
    bool follower = false;         // node.driver but audio: made to follow the server's clock (its position is the server's driver activation)
    bool pending = false;          // a cycle was triggered and has not finished
    Fd activation, io, wake, done, driver;
    uint32_t activation_mem = 0, io_mem = 0, driver_mem = 0;
    uint64_t position = 0, cycles = 0, triggered_at = 0;
    uint64_t next_cycle_ns = 0;    // clocked nodes (MIDI, video, virtual sinks): when the next cycle is due
    uint64_t cycle_ns = 0;
    bool renegotiate = false;      // a port's format is to change (ClientPort::renegotiate): the next tick sees to it         // the time of the round being run (run_client_node): MIDI offsets are counted from it
    uint32_t seen_cycle = 0;       // (driving) the program's cycle counter when last looked at
    // The adapter's port format (F32 at the stream's own rate and channels when its EnumFormat names them) and the frames of a cycle (its node.latency).
    PcmFormat format{SampleFormat::F32Le, 48000, 2};
    uint32_t quantum = 1024;
    uint32_t frame() const { return 4u * format.channels; }
    uint32_t out_channels = 0, in_channels = 0;      // (a raw node: a pw_filter) its float output and input ports: the channels it plays and records
    SharedPointer<PlaybackStream> playback;
    SharedPointer<RecordStream> record;
    SharedPointer<MidiQueue> midi_in;
    SharedPointer<VideoChannel> video;
    uint32_t virtual_sink = 0;     // a virtual sink it plays into or records from (its graph node), 0: the devices
    uint64_t virtual_cursor = 0;   // where in that sink's frames it plays or records next
    Properties props;
    List<UniquePointer<ClientPort>> ports;
    ByteBuffer scratch;            // a filter's float ports' frames, one port after another (played) or interleaved (recorded)
  };
  // A sink or source the adapter factory made (support.null-audio-sink, NativeClientNode.cpp): what the streams that play into it mix to, on the server's
  // clock, for the streams that record from it.
  struct VirtualSink {
    static constexpr uint32_t kRingFrames = 1u << 16;      // (about 1.4 s at 48 kHz)
    explicit VirtualSink(allocators::IAllocator& a) : ring(a) {}
    uint32_t node = 0, owner = 0, resource = 0;            // its graph node; the client that made it and its proxy (0 once it lingers alone)
    uint32_t rate = 48000, channels = 2;
    uint64_t start_ns = 0;
    uint64_t now = 0;                                      // frames since it was made (the clock)
    ByteBuffer ring;                                       // kRingFrames frames of F32
    uint64_t cleared = 0;                                  // the ring holds frames from here - kRingFrames / 2 up to here + kRingFrames / 2
    bool linger = false;
  };
  struct Client {
    explicit Client(allocators::IAllocator& a) : inbox(a), outbox(a), resources(a), known(a), nodes(a), out_fds(a), in_fds(a), message_in_fds(a), free_mem_ids(a) {}
    uint32_t handle = 0;
    uint64_t pid = 0;
    uint32_t graph_client = 0;
    uint32_t core_version = 3;
    uint32_t send_seq = 0;
    uint32_t message_seq = 0;      // the sequence of the message being handled
    ByteBuffer inbox;
    ByteBuffer outbox;
    List<Resource> resources;
    List<GraphEntry> known;        // the globals the registries were told of
    bool has_registry = false;
    bool closing = false;
    List<UniquePointer<ClientNode>> nodes;
    List<Fd> out_fds;              // what the outbox's messages carry
    uint64_t message_start = 0;    // the message being written, and the descriptors it has so far
    uint32_t message_fds = 0;
    uint32_t next_mem_id = 0;
    List<uint32_t> free_mem_ids;   // memory ids removed (remove_mem): the program's pool gives them out again, the last removed first (as pw_map does)
    List<Fd> in_fds;               // descriptors that came and no message has taken yet
    List<Fd> message_in_fds;       // the message being handled's descriptors (a POD Fd is an index into these)
  };

  Client* find(uint32_t handle);
  Resource* resource(Client& c, uint32_t id);
  bool add_resource(Client& c, const Resource& r);
  void remove_resource(Client& c, uint32_t id);

  void dispatch(Client& c, uint32_t id, uint32_t opcode, ConstByteView payload);
  void core_method(Client& c, uint32_t opcode, pod::Parser& args);
  void client_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args);
  void registry_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args);
  void node_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args);
  void protocol_error(Client& c, uint32_t id, int32_t res, const char* message);

  // Messages out: begin() writes the header, end() patches the payload size.
  uint64_t begin(Client& c, uint32_t id, uint32_t opcode);
  void end(Client& c, uint64_t start);

  void send_done(Client& c, uint32_t id, uint32_t seq);
  void send_error(Client& c, uint32_t id, uint32_t seq, int32_t res, const char* message);
  void send_remove_id(Client& c, uint32_t id);
  void send_core_info(Client& c);
  void send_info(Client& c, Resource& r);
  void send_globals(Client& c);      // registry events for what changed since the last snapshot
  void send_global(Client& c, uint32_t registry, const GraphObject& o);
  void send_params(Client& c, Resource& r, uint32_t seq, uint32_t param_id, uint32_t start, uint32_t num);
  void send_metadata(Client& c, Resource& r);
  void update(Client& c);            // globals and the info of bound objects that changed

  // ---- client-node (NativeClientNode.cpp)
  // A descriptor for the message being written: its index in that message.
  int64_t add_fd(Client& c, const Fd& fd);
  void create_client_node(Client& c, uint32_t new_id, const Properties& props);
  void create_adapter(Client& c, uint32_t new_id, uint32_t version, const Properties& props);
  VirtualSink* virtual_sink(uint32_t node);
  void destroy_virtual(uint32_t node);
  void advance_virtual(VirtualSink& v, uint64_t now_ns);
  ClientPort* client_port(ClientNode& n, uint32_t direction, uint32_t port_id);
  void port_update(Client& c, ClientNode& n, uint32_t direction, uint32_t port_id, int32_t change_mask, pod::Parser& args, uint32_t n_params);
  void port_buffers(Client& c, ClientNode& n, pod::Parser& args);
  void negotiate(Client& c, ClientNode& n);
  bool fixate_format(ClientNode& n, ClientPort& p, ByteBuffer* out);
  void send_port_param(Client& c, ClientNode& n, ClientPort& p, ConstByteView format);
  bool send_port_buffers(Client& c, ClientNode& n, ClientPort& p);
  void send_driver_position(Client& c, ClientNode& n);
  void maybe_start(Client& c, ClientNode& n);
  bool consume_output(ClientNode& n, ClientPort& p);
  void publish_video(ClientNode& n, ClientPort& p, uint32_t id, ConstByteView first, int32_t stride0, int64_t pts);
  bool fill_input(ClientNode& n, ClientPort& p);
  ByteView buffer_data(ClientPort& p, uint32_t id, uint32_t* maxsize, uint32_t block = 0);
  ByteView buffer_chunk(ClientPort& p, uint32_t id, uint32_t block = 0);
  void drop_port_buffers(Client& c, ClientPort& p);
  uint32_t new_mem_id(Client& c);
  void renegotiate(Client& c, ClientNode& n);
  void write_header_meta(ClientPort& p, uint32_t id, int64_t pts);
  bool buffer_busy(ClientPort& p, uint32_t id);
  bool cycle_due(ClientNode& n, uint64_t now_ns);
  uint64_t now_ns();
  ClientNode* client_node(Client& c, uint32_t resource);
  void client_node_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args);
  void destroy_client_node(Client& c, uint32_t resource);
  bool make_graph_node(Client& c, ClientNode& n);
  void send_add_mem(Client& c, uint32_t mem_id, const Fd& fd);
  void send_port_config(Client& c, ClientNode& n);
  void set_quantum(ClientNode& n);
  void send_command(Client& c, ClientNode& n, uint32_t command);
  void run_client_node(ClientNode& n, uint64_t now_ns);
  void trigger(ClientNode& n, uint64_t now_ns);

  Graph& graph_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Client>> clients_;
  List<GraphEntry> snapshot_;
  uint32_t next_handle_ = 1;
  uint64_t last_serial_ = 0;
  IDescriptorMaker* maker_ = nullptr;
  uint32_t running_nodes_ = 0;
  bool midi_waiting_ = false;
  uint64_t (*clock_)() = nullptr;
  uint64_t ticks_ = 0;
  List<UniquePointer<VirtualSink>> virtuals_;
};

// Builders shared with the tests: the param objects of a node (Props, EnumFormat/Format) as PipeWire writes them.
// (left, right: the sides' share of the volume, 1 for the louder)
void build_props_param(pod::Builder& b, float volume, bool mute, uint32_t channels, float left = 1.0f, float right = 1.0f);
void build_format_param(pod::Builder& b, uint32_t param_id, const PcmFormat& format);
// A raw video format (video/raw: format, size, frame rate) and MIDI's (application/control).
void build_video_format_param(pod::Builder& b, uint32_t param_id, const VideoFormat& format);
void build_control_format_param(pod::Builder& b, uint32_t param_id);

}  // namespace pipewire
