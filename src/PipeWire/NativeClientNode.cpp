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

// The client-node factory of the native protocol (NativeServer.hpp): how libpipewire's pw_stream programs (pw-play, pw-record, pw-cat, native PipeWire
// programs) play and record through the graph.
//
// A pw_stream exports an adapter node (audioconvert around the stream) with Core::create_object("client-node"). The server answers as PipeWire's
// module-client-node does, and plays the session manager and the driver as well:
//   1. bound_props (the graph node's id), the activation record (Core add_mem of a memfd) and ClientNode transport (its two eventfds);
//   2. once the program says it is active: set_param(PortConfig: convert mode, S16LE stereo at 48 kHz), so the adapter makes one interleaved port in the
//      mixer's own format and converts the stream's format to it;
//   3. when the port appears: port_set_param(Format) with that format; when the program answers: the port's mix (port_set_mix_info), its buffers
//      (add_mem of a memfd, port_use_buffers: n buffers of a chunk and up to 8192 frames each, MemPtr data), its io area (port_set_io Buffers) and
//      command Start;
//   4. every cycle (demand-driven: playback while the graph's stream holds less than three quanta, recording while a quantum has been recorded): the
//      position (a 1024-frame quantum at 48 kHz), status TRIGGERED, a signal on the client's eventfd; at a later tick, once the client marked the
//      activation FINISHED, the buffer it filled (spa_io_buffers HAVE_DATA) goes to the node's PlaybackStream and is handed back (NEED_DATA). Recording
//      fills a buffer from the RecordStream before the trigger instead.
// The records the server and the client share are PipeWire 1.x's layouts (pw_node_activation, spa_io_position/clock, spa_io_buffers, spa_chunk):
// their offsets are in the constants below, checked against libpipewire 1.6.9's headers.
#include "PipeWire/NativeServer.hpp"
#include "Utility/ByteStream.hpp"

namespace pipewire {

namespace {

// ClientNode methods (client -> server) and events (server -> client), pipewire/extensions/client-node.h
enum : uint32_t { kCnGetNode = 1, kCnUpdate = 2, kCnPortUpdate = 3, kCnSetActive = 4, kCnEvent = 5, kCnPortBuffers = 6 };
enum : uint32_t { kCnEvTransport = 0, kCnEvSetParam = 1, kCnEvSetIo = 2, kCnEvEvent = 3, kCnEvCommand = 4, kCnEvAddPort = 5, kCnEvRemovePort = 6,
                  kCnEvPortSetParam = 7, kCnEvPortUseBuffers = 8, kCnEvPortSetIo = 9, kCnEvSetActivation = 10, kCnEvPortSetMixInfo = 11 };
constexpr uint32_t kCoreEvBoundId = 5, kCoreEvAddMem = 6, kCoreEvBoundProps = 8;

// spa types and ids (spa/param/param.h, port-config.h, node/command.h, node/io.h, buffer/buffer.h)
constexpr uint32_t kParamFormat = 4, kParamPortConfig = 11, kObjectPortConfig = 0x40008, kObjectBuffers = 0x40004;
constexpr uint32_t kPortConfigDirection = 1, kPortConfigMode = 2, kPortConfigMonitor = 3, kPortConfigControl = 4, kPortConfigFormat = 5, kModeConvert = 2;
constexpr uint32_t kCommandNode = 0x30002, kCommandPause = 1, kCommandStart = 2;
constexpr uint32_t kIoBuffers = 1;
constexpr uint32_t kDataMemFd = 2, kDataMemPtr = 1, kDataReadWrite = 1 | 2;
constexpr uint32_t kMemReadWrite = 1 | 2;
constexpr int32_t kStatusNeedData = 1, kStatusHaveData = 2;
constexpr uint32_t kIdInvalid = 0xFFFFFFFFu;
constexpr int32_t kEINVAL = 22, kENOMEM = 12, kENOTSUP = 95;

// pw_node_activation (src/pipewire/private.h, PW_VERSION_NODE_ACTIVATION 1) and the spa_io_position inside it
constexpr uint64_t kActivationSize = 2312;
constexpr uint32_t kActNotTriggered = 0, kActTriggered = 1, kActAwake = 2, kActFinished = 3, kActInactive = 4;
constexpr uint64_t kActStatus = 0, kActSignalTime = 32, kActServerVersion = 544, kActActiveDriver = 548, kActDriver = 552, kActPosition = 560;
constexpr uint64_t kPosClock = 0, kPosOffset = 200, kPosState = 208, kPosSegmentCount = 212, kPosSegments = 216;
constexpr uint64_t kClockId = 4, kClockName = 8, kClockNsec = 72, kClockRate = 80, kClockPosition = 88, kClockDuration = 96, kClockRateDiff = 112,
                   kClockNextNsec = 120, kClockTargetRate = 128, kClockTargetDuration = 136, kClockCycle = 148;
constexpr uint64_t kSegmentRate = 24;
constexpr uint32_t kPositionRunning = 2;

// the buffers: a spa_chunk, then the data, in each buffer's own part of one memfd
constexpr uint32_t kQuantum = 1024, kRate = 48000, kFrame = 8;      // F32LE stereo
constexpr uint32_t kBuffers = 3, kMaxBuffers = 16, kMaxFrames = 8192, kDataOffset = 64;
constexpr uint32_t kMaxDataSize = 1u << 20;      // the most a buffer's data may be (what the port asks for, within reason)
constexpr uint32_t kBuffersBuffers = 1, kBuffersSize = 3;      // spa/param/buffers.h
constexpr uint64_t kAheadBytes = 3ull * kQuantum * kFrame;      // what playback keeps queued in the graph
constexpr uint32_t kTimeoutTicks = 100;                         // a cycle that never finishes is given up after this many ticks

// ---- the shared records ------------------------------------------------------------------------------------------------------------------------------
// The client maps the same memory and changes it while the server reads it, so fields are read and written with atomic loads and stores. Every access
// goes through field_at(): it checks that the field lies inside the block (bounds) and is aligned to its size (the blocks are page-aligned and every
// offset above is a multiple of the field's size, so a misaligned one is a bug, refused), and it is the only place the bytes become a typed field.
template <class T>
T* field_at(ByteView memory, uint64_t offset) {
  if (memory.size() < sizeof(T) || offset > memory.size() - sizeof(T)) return nullptr;
  uint8_t* at = memory.data() + offset;
  if (reinterpret_cast<uintptr_t>(at) % sizeof(T) != 0) return nullptr;
  return reinterpret_cast<T*>(at);
}
template <class T>
T load(ByteView memory, uint64_t offset) {
  T* f = field_at<T>(memory, offset);
  return f ? __atomic_load_n(f, __ATOMIC_ACQUIRE) : T{};
}
template <class T>
void store(ByteView memory, uint64_t offset, T value) {
  if (T* f = field_at<T>(memory, offset)) __atomic_store_n(f, value, __ATOMIC_RELEASE);
}
void store_double(ByteView memory, uint64_t offset, double value) { store<uint64_t>(memory, offset, __builtin_bit_cast(uint64_t, value)); }

uint64_t frames_ns(uint64_t frames) { return frames * 1000000000ull / kRate; }

// Reads the inline dict of an update (Int n, then n key/value strings) into `out`.
bool read_inline_dict(pod::Parser& p, Properties* out) {
  int32_t n = 0;
  if (!p.int32(&n) || n < 0 || n > static_cast<int32_t>(Properties::kMaxItems)) return false;
  for (int32_t i = 0; i < n; ++i) {
    StringView k, v;
    if (!p.string(&k) || !p.string(&v)) return false;
    if (!k.empty() && out) out->set(k, v);
  }
  return true;
}

void push_empty_dict(pod::Builder& b) {
  const uint64_t f = b.begin_struct();
  b.int32(0);
  b.end(f);
}

const PcmFormat kMixFormat{SampleFormat::F32Le, kRate, 2};

// An Int property of a param, or a Choice of Ints' default (its first value).
bool int_or_default(const pod::Value& v, uint32_t* out) {
  int32_t i = 0;
  if (v.get_int(&i)) {
    *out = static_cast<uint32_t>(i);
    return true;
  }
  pod::ArrayReader a;
  return v.type == pod::kChoice && a.open(v) && a.child_type() == pod::kInt && a.get32(0, out);
}

}  // namespace

// ---- setting up ---------------------------------------------------------------------------------------------------------------------------------

int64_t NativeServer::add_fd(Client& c, const Fd& fd) {
  if (!fd || !c.out_fds.push_back(fd)) return -1;
  return c.message_fds++;
}

NativeServer::ClientNode* NativeServer::client_node(Client& c, uint32_t resource) {
  for (UniquePointer<ClientNode>& n : c.nodes.view())
    if (n->resource == resource) return n.get();
  return nullptr;
}

void NativeServer::send_add_mem(Client& c, uint32_t mem_id, const Fd& fd) {
  const uint64_t m = begin(c, 0, kCoreEvAddMem);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(mem_id));
  b.id(kDataMemFd);
  b.fd(add_fd(c, fd));
  b.int32(static_cast<int32_t>(kMemReadWrite));
  b.end(f);
  end(c, m);
}

bool NativeServer::make_graph_node(Client& c, ClientNode& n) {
  if (n.node) return true;
  const StringView media_class = n.props.get("media.class");
  const bool plays = media_class.starts_with("Stream/Output/Audio");
  const bool records = media_class.starts_with("Stream/Input/Audio");
  if (!plays && !records) return false;      // (not known yet: the update may say)
  Properties props(allocator_);
  props.copy_from(n.props);
  props.set("client.api", "pipewire");
  if (plays) {
    n.direction = kDirectionOutput;
    n.node = graph_.create_playback(c.graph_client, kMixFormat, props, &n.playback);
  } else {
    n.direction = kDirectionInput;
    n.node = graph_.create_record(c.graph_client, kMixFormat, props, &n.record, props.get("stream.capture.sink") == StringView("true"));
  }
  if (!n.node) return false;
  // the proxy is bound to the node (bound_props, core 4; bound_id before)
  const uint64_t m = begin(c, 0, c.core_version >= 4 ? kCoreEvBoundProps : kCoreEvBoundId);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(n.resource));
  b.int32(static_cast<int32_t>(n.node));
  if (c.core_version >= 4) {
    Properties bound(allocator_);
    graph_.visit(n.node, [&](const GraphObject& o) { bound.copy_from(o.props); });
    const uint64_t d = b.begin_struct();
    b.int32(static_cast<int32_t>(bound.count()));
    for (uint64_t i = 0; i < bound.count(); ++i) {
      b.string(bound.at(i).key.view());
      b.string(bound.at(i).value.view());
    }
    b.end(d);
  }
  b.end(f);
  end(c, m);
  // the activation's clock names the node; the record is ready for the client from here
  ByteView a = n.activation->memory();
  store<uint32_t>(a, kActActiveDriver, n.node);
  store<uint32_t>(a, kActDriver, n.node);
  store<uint32_t>(a, kActPosition + kPosClock + kClockId, n.node);
  return true;
}

void NativeServer::create_client_node(Client& c, uint32_t new_id, const Properties& props) {
  auto refuse = [&](int32_t res, const char* why) {
    send_error(c, new_id, c.message_seq, -res, why);
    send_remove_id(c, new_id);
  };
  if (client_node(c, new_id) || resource(c, new_id)) return protocol_error(c, new_id, kEINVAL, "id in use");
  UniquePointer<ClientNode> n = UniquePointer<ClientNode>::make(allocator_, allocator_);
  if (!n) return refuse(kENOMEM, "out of memory");
  n->resource = new_id;
  n->props.copy_from(props);
  n->activation = maker_->make_memory(4096, "pipewire-activation");
  n->io = maker_->make_memory(4096, "pipewire-io");
  n->wake = maker_->make_event();
  n->done = maker_->make_event();
  if (!n->activation || !n->io || !n->wake || !n->done || n->activation->memory().size() < kActivationSize || n->io->memory().size() < 8)
    return refuse(kENOMEM, "no shared memory or eventfds");
  // the activation: not schedulable until the client marks it FINISHED; the server's protocol version; the position of a stopped clock
  ByteView a = n->activation->memory();
  store<uint32_t>(a, kActStatus, kActInactive);
  store<uint32_t>(a, kActServerVersion, 1);
  const uint64_t clock = kActPosition + kPosClock;
  ByteView name = a.subview(clock + kClockName, 64);
  name.fill(0);
  const StringView clock_name("tcos.driver");
  name.first(clock_name.size()).copy_from(clock_name.bytes());
  store<uint32_t>(a, clock + kClockRate, 1);
  store<uint32_t>(a, clock + kClockRate + 4, kRate);
  store<uint64_t>(a, clock + kClockDuration, kQuantum);
  store_double(a, clock + kClockRateDiff, 1.0);
  store<uint32_t>(a, clock + kClockTargetRate, 1);
  store<uint32_t>(a, clock + kClockTargetRate + 4, kRate);
  store<uint64_t>(a, clock + kClockTargetDuration, kQuantum);
  store<uint32_t>(a, kActPosition + kPosState, kPositionRunning);
  store<uint32_t>(a, kActPosition + kPosSegmentCount, 1);
  store_double(a, kActPosition + kPosSegments + kSegmentRate, 1.0);
  store<int64_t>(a, kActPosition + kPosOffset, 0);
  Resource r{new_id, ObjectType::Node, false, 0, 6, 0, {}, true};
  if (!add_resource(c, r)) return refuse(kENOMEM, "too many objects");
  ClientNode& node = *n;
  if (!c.nodes.push_back(util::move(n))) {
    remove_resource(c, new_id);
    return refuse(kENOMEM, "out of memory");
  }
  make_graph_node(c, node);      // (pw_stream names its media class here; others do in their update)
  // the activation memory and the transport (the client waits on `wake`; `done` is the other way)
  node.activation_mem = c.next_mem_id++;
  send_add_mem(c, node.activation_mem, node.activation);
  const uint64_t m = begin(c, new_id, kCnEvTransport);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.fd(add_fd(c, node.wake));
  b.fd(add_fd(c, node.done));
  b.int32(static_cast<int32_t>(node.activation_mem));
  b.int32(0);
  b.int32(static_cast<int32_t>(kActivationSize));
  b.end(f);
  end(c, m);
}

void NativeServer::destroy_client_node(Client& c, uint32_t resource_id) {
  for (uint64_t i = 0; i < c.nodes.count(); ++i) {
    ClientNode& n = *c.nodes[i];
    if (n.resource != resource_id) continue;
    if (n.node) graph_.destroy_node(n.node, n.playback ? true : false);      // (what it played still plays out)
    c.nodes.remove_at(i);
    return;
  }
}

// ---- the server's messages -----------------------------------------------------------------------------------------------------------------------

void NativeServer::send_port_config(Client& c, ClientNode& n) {
  const uint64_t m = begin(c, n.resource, kCnEvSetParam);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.id(kParamPortConfig);
  b.int32(0);
  const uint64_t o = b.begin_object(kObjectPortConfig, kParamPortConfig);
  b.property(kPortConfigDirection);
  b.id(n.direction);
  b.property(kPortConfigMode);
  b.id(kModeConvert);
  b.property(kPortConfigMonitor);
  b.boolean(false);
  b.property(kPortConfigControl);
  b.boolean(false);
  b.property(kPortConfigFormat);
  build_format_param(b, kParamFormat, kMixFormat);
  b.end(o);
  b.end(f);
  end(c, m);
  n.config_sent = true;
}

void NativeServer::send_port_format(Client& c, ClientNode& n) {
  const uint64_t m = begin(c, n.resource, kCnEvPortSetParam);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(n.direction));
  b.int32(0);
  b.id(kParamFormat);
  b.int32(0);
  build_format_param(b, kParamFormat, kMixFormat);
  b.end(f);
  end(c, m);
  n.format_sent = true;
}

bool NativeServer::send_buffers(Client& c, ClientNode& n) {
  // as many as the port asks for (2 .. 16) and as big (at least a quantum of the largest, 8192 frames; at most 1 MiB), each in its own 64-byte-aligned part
  n.n_buffers = n.wanted_buffers >= 2 && n.wanted_buffers <= kMaxBuffers ? n.wanted_buffers : kBuffers;
  uint32_t data = n.wanted_size > kMaxFrames * kFrame ? n.wanted_size : kMaxFrames * kFrame;
  if (data > kMaxDataSize) data = kMaxDataSize;
  data = (data + 63) & ~63u;
  n.data_size = data;
  n.buffer_stride = kDataOffset + data;
  const uint64_t total = uint64_t{n.n_buffers} * n.buffer_stride;
  n.buffers = maker_->make_memory(total, "pipewire-buffers");
  if (!n.buffers || n.buffers->memory().size() < total) return false;
  n.buffers_mem = c.next_mem_id++;
  n.io_mem = c.next_mem_id++;
  send_add_mem(c, n.buffers_mem, n.buffers);
  send_add_mem(c, n.io_mem, n.io);
  // the port's one link: mix 0 (its peer is the sink's or the source's port)
  {
    const uint64_t m = begin(c, n.resource, kCnEvPortSetMixInfo);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(n.direction));
    b.int32(0);
    b.int32(0);
    b.int32(static_cast<int32_t>(n.direction == kDirectionOutput ? graph_.sink_id() : graph_.source_id()));
    push_empty_dict(b);
    b.end(f);
    end(c, m);
  }
  // the buffers: an output port takes them on the port itself (mix SPA_ID_INVALID), an input port on its mix
  {
    const uint64_t m = begin(c, n.resource, kCnEvPortUseBuffers);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(n.direction));
    b.int32(0);
    b.int32(static_cast<int32_t>(n.direction == kDirectionOutput ? kIdInvalid : 0));
    b.int32(0);
    b.int32(static_cast<int32_t>(n.n_buffers));
    for (uint32_t i = 0; i < n.n_buffers; ++i) {
      b.int32(static_cast<int32_t>(n.buffers_mem));
      b.int32(static_cast<int32_t>(i * n.buffer_stride));
      b.int32(static_cast<int32_t>(n.buffer_stride));
      b.int32(0);      // metas
      b.int32(1);      // datas
      b.id(kDataMemPtr);
      b.int32(static_cast<int32_t>(kDataOffset));      // (MemPtr: the offset in the buffer's part)
      b.int32(static_cast<int32_t>(kDataReadWrite));
      b.int32(0);
      b.int32(static_cast<int32_t>(n.data_size));
    }
    b.end(f);
    end(c, m);
  }
  // the port's io area (spa_io_buffers) on mix 0
  {
    ByteView io = n.io->memory();
    store<int32_t>(io, 0, n.direction == kDirectionOutput ? kStatusNeedData : 0);
    store<uint32_t>(io, 4, kIdInvalid);
    const uint64_t m = begin(c, n.resource, kCnEvPortSetIo);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(n.direction));
    b.int32(0);
    b.int32(0);
    b.id(kIoBuffers);
    b.int32(static_cast<int32_t>(n.io_mem));
    b.int32(0);
    b.int32(8);
    b.end(f);
    end(c, m);
  }
  n.buffers_sent = true;
  return true;
}

void NativeServer::send_command(Client& c, ClientNode& n, uint32_t command) {
  const uint64_t m = begin(c, n.resource, kCnEvCommand);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  const uint64_t o = b.begin_object(kCommandNode, command);
  b.end(o);
  b.end(f);
  end(c, m);
}

// ---- the client's methods ------------------------------------------------------------------------------------------------------------------------

void NativeServer::client_node_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args) {
  ClientNode* n = client_node(c, r.id);
  if (!n) return;
  switch (opcode) {
    case kCnUpdate: {
      int32_t change_mask = 0, n_params = 0;
      if (!args.int32(&change_mask) || !args.int32(&n_params) || n_params < 0 || n_params > 256) {
        protocol_error(c, r.id, kEINVAL, "invalid update");
        return;
      }
      for (int32_t i = 0; i < n_params; ++i) {
        pod::Value v;
        if (!args.pod(&v)) { protocol_error(c, r.id, kEINVAL, "invalid update"); return; }
      }
      pod::Value info;
      if (!args.pod(&info)) { protocol_error(c, r.id, kEINVAL, "invalid update"); return; }
      if (info.type == pod::kStruct) {
        pod::Parser p(info.body, args.depth() + 1);
        int32_t max_in = 0, max_out = 0;
        int64_t mask = 0, flags = 0;
        Properties props(allocator_);
        if (!p.int32(&max_in) || !p.int32(&max_out) || !p.int64(&mask) || !p.int64(&flags) || !read_inline_dict(p, &props)) {
          protocol_error(c, r.id, kEINVAL, "invalid node info");
          return;
        }
        if (props.count()) {
          n->props.update(props, Properties::Mode::kReplace);
          if (n->node) graph_.update_node(n->node, props, Properties::Mode::kReplace);
          else make_graph_node(c, *n);
        }
      }
      return;
    }
    case kCnPortUpdate: {
      int32_t direction = 0, port_id = 0, change_mask = 0, n_params = 0;
      if (!args.int32(&direction) || !args.int32(&port_id) || !args.int32(&change_mask) || !args.int32(&n_params) || n_params < 0 || n_params > 256) {
        protocol_error(c, r.id, kEINVAL, "invalid port_update");
        return;
      }
      bool has_buffers_param = false;
      for (int32_t i = 0; i < n_params; ++i) {
        pod::Value v;
        if (!args.pod(&v)) { protocol_error(c, r.id, kEINVAL, "invalid port_update"); return; }
        pod::ObjectReader o;
        if (!o.open(v) || o.object_type() != kObjectBuffers) continue;
        has_buffers_param = true;
        // how many buffers and how big their data must be (each a choice: its default)
        uint32_t key = 0, flags = 0;
        pod::Value prop;
        while (o.next(&key, &flags, &prop)) {
          uint32_t value = 0;
          if (key == kBuffersBuffers && int_or_default(prop, &value)) n->wanted_buffers = value;
          if (key == kBuffersSize && int_or_default(prop, &value)) n->wanted_size = value;
        }
      }
      pod::Value info;
      if (!args.pod(&info)) { protocol_error(c, r.id, kEINVAL, "invalid port_update"); return; }
      if (port_id != 0 || static_cast<uint32_t>(direction) != n->direction) return;      // (one port, in the node's direction)
      if (change_mask == 0 && n_params == 0 && info.is_none()) {      // the port went
        n->port = false;
        return;
      }
      n->port = true;
      if (!n->node) make_graph_node(c, *n);
      if (!n->format_sent) {
        send_port_format(c, *n);
      } else if (!n->buffers_sent && (has_buffers_param || n_params > 0)) {
        if (!send_buffers(c, *n)) { protocol_error(c, r.id, kENOMEM, "no memory for the buffers"); return; }
      }
      if (n->buffers_sent && n->active && !n->started && n->node) {
        send_command(c, *n, kCommandStart);
        n->started = true;
      }
      return;
    }
    case kCnSetActive: {
      bool active = false;
      if (!args.boolean(&active)) { protocol_error(c, r.id, kEINVAL, "invalid set_active"); return; }
      n->active = active;
      if (!n->node && !make_graph_node(c, *n)) return;
      if (active && !n->config_sent) send_port_config(c, *n);
      if (active && n->buffers_sent && !n->started) {
        send_command(c, *n, kCommandStart);
        n->started = true;
      } else if (!active && n->started) {
        send_command(c, *n, kCommandPause);
        n->started = false;
        n->pending = false;
      }
      return;
    }
    case kCnGetNode: case kCnEvent: case kCnPortBuffers: return;      // (no server-side node proxy; events and client buffers are not used)
    default: protocol_error(c, r.id, kENOTSUP, "unknown client-node method"); return;
  }
}

// ---- the cycle -----------------------------------------------------------------------------------------------------------------------------------

void NativeServer::trigger(ClientNode& n, uint64_t) {
  ByteView a = n.activation->memory();
  const uint32_t status = load<uint32_t>(a, kActStatus);
  if (status == kActInactive) return;      // (the client has not added the node to its loop yet)
  const uint64_t nsec = frames_ns(n.position);
  const uint64_t clock = kActPosition + kPosClock;
  store<uint64_t>(a, clock + kClockNsec, nsec);
  store<uint64_t>(a, clock + kClockPosition, n.position);
  store<uint64_t>(a, clock + kClockDuration, kQuantum);
  store<uint64_t>(a, clock + kClockNextNsec, nsec + frames_ns(kQuantum));
  store<uint32_t>(a, clock + kClockCycle, static_cast<uint32_t>(n.cycles));
  store<uint64_t>(a, kActSignalTime, nsec);
  store<uint32_t>(a, kActStatus, kActNotTriggered);
  store<uint32_t>(a, kActStatus, kActTriggered);
  n.position += kQuantum;
  ++n.cycles;
  n.pending = true;
  n.triggered_at = 0;
  n.wake->signal();
}

void NativeServer::run_client_node(ClientNode& n, uint64_t now) {
  if (!n.activation || !n.buffers || !n.io) return;
  ByteView a = n.activation->memory(), io = n.io->memory(), bufs = n.buffers->memory();
  if (n.pending) {
    const uint32_t status = load<uint32_t>(a, kActStatus);
    if (status != kActFinished) {
      if (++n.triggered_at < kTimeoutTicks) return;
      n.pending = false;      // (it never finished: try again)
    } else {
      n.pending = false;
      if (n.playback) {
        // what it played this cycle: the buffer io names, its chunk's region of the data
        const int32_t io_status = load<int32_t>(io, 0);
        const uint32_t id = load<uint32_t>(io, 4);
        if ((io_status & kStatusHaveData) && id < n.n_buffers) {
          const uint64_t base = uint64_t{id} * n.buffer_stride;
          const uint32_t offset = load<uint32_t>(bufs, base), size = load<uint32_t>(bufs, base + 4);
          const uint32_t maxsize = n.data_size;
          const uint32_t start = offset % maxsize, length = size < maxsize ? size : maxsize;
          ConstByteView data = ConstByteView(bufs).subview(base + kDataOffset, maxsize);
          const uint32_t first = length < maxsize - start ? length : maxsize - start;
          n.playback->write(data.subview(start, first - first % kFrame));
          if (length > first) n.playback->write(data.first(length - first - (length - first) % kFrame));
          store<uint32_t>(io, 4, id);      // (handed back)
        } else {
          store<uint32_t>(io, 4, kIdInvalid);      // (nothing came: nothing to hand back, or the producer would get a buffer twice)
        }
        store<int32_t>(io, 0, kStatusNeedData);
      }
    }
  }
  if (n.pending) return;
  if (n.playback) {
    if (n.playback->waiting_bytes() < kAheadBytes) trigger(n, now);
  } else if (n.record) {
    if (n.record->available_bytes() < uint64_t{kQuantum} * kFrame) return;
    const uint32_t id = n.next_buffer;
    n.next_buffer = (n.next_buffer + 1) % n.n_buffers;
    const uint64_t base = uint64_t{id} * n.buffer_stride;
    const uint32_t got = n.record->read(bufs.subview(base + kDataOffset, uint64_t{kQuantum} * kFrame));
    store<uint32_t>(bufs, base, 0);
    store<uint32_t>(bufs, base + 4, got);
    store<int32_t>(bufs, base + 8, static_cast<int32_t>(kFrame));
    store<int32_t>(bufs, base + 12, 0);
    store<uint32_t>(io, 4, id);
    store<int32_t>(io, 0, kStatusHaveData);
    trigger(n, now);
  }
}

}  // namespace pipewire
