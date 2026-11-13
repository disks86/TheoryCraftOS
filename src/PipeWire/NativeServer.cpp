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

#include "PipeWire/NativeServer.hpp"
#include "Utility/ByteStream.hpp"

namespace pipewire {

namespace {

// opcodes (pipewire/core.h and friends: a method's number counts add_listener as 0)
enum : uint32_t { kCoreHello = 1, kCoreSync = 2, kCorePong = 3, kCoreError = 4, kCoreGetRegistry = 5, kCoreCreateObject = 6, kCoreDestroy = 7 };
enum : uint32_t { kCoreEvInfo = 0, kCoreEvDone = 1, kCoreEvPing = 2, kCoreEvError = 3, kCoreEvRemoveId = 4, kCoreEvBoundId = 5, kCoreEvBoundProps = 8 };
enum : uint32_t { kRegistryBind = 1, kRegistryDestroy = 2, kRegistryEvGlobal = 0, kRegistryEvGlobalRemove = 1 };
enum : uint32_t { kClientError = 1, kClientUpdateProperties = 2, kClientGetPermissions = 3, kClientUpdatePermissions = 4, kClientEvInfo = 0, kClientEvPermissions = 1 };
enum : uint32_t { kNodeSubscribeParams = 1, kNodeEnumParams = 2, kNodeSetParam = 3, kNodeSendCommand = 4, kEvInfo = 0, kEvParam = 1 };
enum : uint32_t { kMetadataSetProperty = 1, kMetadataClear = 2, kMetadataEvProperty = 0 };

// spa/param/param.h, spa/param/props.h, spa/param/format.h, spa/utils/type.h
constexpr uint32_t kParamProps = 2, kParamEnumFormat = 3, kParamFormat = 4;
constexpr uint32_t kParamInfoRead = 1u << 1, kParamInfoWrite = 1u << 2;
constexpr uint32_t kObjectProps = 0x40002, kObjectFormat = 0x40003;
constexpr uint32_t kPropVolume = 0x10003, kPropMute = 0x10004, kPropChannelVolumes = 0x10008, kPropVolumeBase = 0x10009, kPropVolumeStep = 0x1000a,
                   kPropChannelMap = 0x1000b, kPropSoftMute = 0x1000f, kPropSoftVolumes = 0x10010;
constexpr uint32_t kFormatMediaType = 1, kFormatMediaSubtype = 2, kFormatAudioFormat = 0x10001, kFormatAudioRate = 0x10003, kFormatAudioChannels = 0x10004,
                   kFormatAudioPosition = 0x10005;
constexpr uint32_t kMediaTypeAudio = 1, kMediaSubtypeRaw = 1;
constexpr uint32_t kFL = 3, kFR = 4, kMono = 2;

constexpr uint32_t kPermAll = 0400 | 0200 | 0100 | 0010;      // R W X M
constexpr int32_t kENOENT = 2, kEINVAL = 22, kENOTSUP = 95, kEPERM = 1, kEPROTO = 71;

constexpr uint32_t kIdAny = 0xFFFFFFFFu;

void push_dict(pod::Builder& b, const Properties& props) {
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(props.count()));
  for (uint64_t i = 0; i < props.count(); ++i) {
    b.string(props.at(i).key.view());
    b.string(props.at(i).value.view());
  }
  b.end(f);
}

struct ParamInfo {
  uint32_t id, flags;
};

void push_params(pod::Builder& b, ArrayView<const ParamInfo> params) {
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(params.size()));
  for (const ParamInfo& p : params) {
    b.id(p.id);
    b.int32(static_cast<int32_t>(p.flags));
  }
  b.end(f);
}

constexpr ParamInfo kNodeParams[] = {{kParamProps, kParamInfoRead | kParamInfoWrite}, {kParamEnumFormat, kParamInfoRead}, {kParamFormat, kParamInfoRead}};
constexpr ParamInfo kPortParams[] = {{kParamEnumFormat, kParamInfoRead}, {kParamFormat, kParamInfoRead}};

// Reads a dict (Struct: Int n, then n key/value strings) into `out`.
bool read_dict(pod::Parser& args, Properties* out) {
  pod::Parser d;
  int32_t n = 0;
  if (!args.structure(&d) || !d.int32(&n) || n < 0 || n > static_cast<int32_t>(Properties::kMaxItems)) return false;
  for (int32_t i = 0; i < n; ++i) {
    StringView k, v;
    if (!d.string(&k) || !d.string(&v)) return false;
    if (!k.empty() && !out->set(k, v)) return false;
  }
  return true;
}

int subscribed_index(uint32_t param) { return param == kParamProps ? 0 : param == kParamEnumFormat ? 1 : param == kParamFormat ? 2 : -1; }

void drop_front(ByteBuffer& b, uint64_t n) {
  if (n >= b.size()) { b.clear(); return; }
  ByteView all = b.writable_view();
  all.copy_from(ConstByteView(all).subview(n));
  b.truncate(b.size() - n);
}

}  // namespace

void build_props_param(pod::Builder& b, float volume, bool mute, uint32_t channels, float left, float right) {
  const uint32_t n = channels == 1 ? 1 : 2;
  const float volumes[2] = {volume * (n == 1 ? 1.0f : left), volume * (n == 1 ? 1.0f : right)};
  const uint32_t map_stereo[2] = {kFL, kFR}, map_mono[1] = {kMono};
  const uint64_t f = b.begin_object(kObjectProps, kParamProps);
  b.property(kPropVolume);
  b.float32(volume);
  b.property(kPropMute);
  b.boolean(mute);
  b.property(kPropChannelVolumes);
  b.array_floats(ArrayView<const float>(volumes, n));
  b.property(kPropVolumeBase);
  b.float32(1.0f);
  b.property(kPropVolumeStep);
  b.float32(1.0f / 65536.0f);
  b.property(kPropChannelMap);
  b.array_ids(n == 1 ? ArrayView<const uint32_t>(map_mono, 1) : ArrayView<const uint32_t>(map_stereo, 2));
  b.property(kPropSoftMute);
  b.boolean(mute);
  b.property(kPropSoftVolumes);
  b.array_floats(ArrayView<const float>(volumes, n));
  b.end(f);
}

void build_format_param(pod::Builder& b, uint32_t param_id, const PcmFormat& format) {
  const uint32_t channels = format.channels == 1 ? 1 : 2;
  const uint32_t map_stereo[2] = {kFL, kFR}, map_mono[1] = {kMono};
  const uint64_t f = b.begin_object(kObjectFormat, param_id);
  b.property(kFormatMediaType);
  b.id(kMediaTypeAudio);
  b.property(kFormatMediaSubtype);
  b.id(kMediaSubtypeRaw);
  b.property(kFormatAudioFormat);
  b.id(format.spa_format());
  b.property(kFormatAudioRate);
  b.int32(static_cast<int32_t>(format.rate));
  b.property(kFormatAudioChannels);
  b.int32(static_cast<int32_t>(channels));
  b.property(kFormatAudioPosition);
  b.array_ids(channels == 1 ? ArrayView<const uint32_t>(map_mono, 1) : ArrayView<const uint32_t>(map_stereo, 2));
  b.end(f);
}

NativeServer::NativeServer(Graph& graph, allocators::IAllocator& allocator, uint64_t (*clock)())
    : graph_(graph), allocator_(allocator), clients_(allocator), snapshot_(allocator), clock_(clock), virtuals_(allocator) {
  if (clock) {
    graph_.midi_bus().set_clock(clock);
    graph_.set_clock(clock);      // (virtual sinks play on it)
  }
}

NativeServer::~NativeServer() {
  for (UniquePointer<Client>& c : clients_.view()) graph_.remove_client(c->graph_client);
  for (UniquePointer<VirtualSink>& v : virtuals_.view()) graph_.destroy_node(v->node);
}

NativeServer::Client* NativeServer::find(uint32_t handle) {
  for (UniquePointer<Client>& c : clients_.view())
    if (c->handle == handle) return c.get();
  return nullptr;
}

NativeServer::Resource* NativeServer::resource(Client& c, uint32_t id) {
  for (Resource& r : c.resources.view())
    if (r.id == id) return &r;
  return nullptr;
}

bool NativeServer::add_resource(Client& c, const Resource& r) {
  if (c.resources.count() >= kMaxResources || resource(c, r.id)) return false;
  return c.resources.push_back(r);
}

void NativeServer::remove_resource(Client& c, uint32_t id) {
  for (uint64_t i = 0; i < c.resources.count(); ++i)
    if (c.resources[i].id == id) {
      c.resources.remove_at(i);
      return;
    }
}

uint32_t NativeServer::open_client(uint64_t pid) {
  if (clients_.count() >= kMaxClients) return 0;
  UniquePointer<Client> c = UniquePointer<Client>::make(allocator_, allocator_);
  if (!c) return 0;
  c->handle = next_handle_++;
  if (next_handle_ == 0) next_handle_ = 1;
  c->pid = pid;
  c->graph_client = graph_.add_client(Origin::Linux, pid, "protocol-native", "");
  if (!c->graph_client) return 0;
  Resource core{0, ObjectType::Core, false, Graph::kCoreId, 4, 0, {}};
  Resource client{1, ObjectType::Client, false, c->graph_client, 3, 0, {}};
  if (!add_resource(*c, core) || !add_resource(*c, client)) {
    graph_.remove_client(c->graph_client);
    return 0;
  }
  const uint32_t handle = c->handle;
  if (!clients_.push_back(util::move(c))) return 0;
  return handle;
}

void NativeServer::close_client(uint32_t handle) {
  for (uint64_t i = 0; i < clients_.count(); ++i)
    if (clients_[i]->handle == handle) {
      while (clients_[i]->nodes.count()) destroy_client_node(*clients_[i], clients_[i]->nodes[0]->resource);
      // its virtual sinks go with it, unless they linger (object.linger): they are the kernel's then
      for (uint64_t k = virtuals_.count(); k > 0; --k) {
        VirtualSink& v = *virtuals_[k - 1];
        if (v.owner != handle) continue;
        if (v.linger) {
          graph_.keep_node(v.node);
          v.owner = 0;
          v.resource = 0;
        } else {
          destroy_virtual(v.node);
        }
      }
      graph_.remove_client(clients_[i]->graph_client);
      clients_.remove_at(i);
      return;
    }
}

bool NativeServer::take_output(uint32_t handle, ByteBuffer* out) {
  Client* c = find(handle);
  if (!c) return true;
  if (c->outbox.size()) {
    if (!out->append(c->outbox.view())) return true;
    c->outbox.clear();
  }
  c->out_fds.clear();      // (a transport that cannot pass them: the messages that needed them are of no use without, nothing better to do)
  return c->closing;
}

bool NativeServer::take_output_with_fds(uint32_t handle, ByteBuffer* out, List<Fd>* fds) {
  Client* c = find(handle);
  if (!c) return true;
  if (c->outbox.size()) {
    if (!out->append(c->outbox.view())) return true;
    c->outbox.clear();
  }
  for (Fd& fd : c->out_fds)
    if (!fds->push_back(fd)) c->closing = true;
  c->out_fds.clear();
  return c->closing;
}

void NativeServer::receive_with_fds(uint32_t handle, ConstByteView bytes, List<Fd>& fds) {
  Client* c = find(handle);
  if (!c || c->closing) {
    fds.clear();
    return;
  }
  for (Fd& fd : fds)
    if (c->in_fds.count() >= kMaxInFds || !c->in_fds.push_back(util::move(fd))) c->closing = true;
  fds.clear();
  receive(handle, bytes);
}

void NativeServer::receive(uint32_t handle, ConstByteView bytes) {
  Client* c = find(handle);
  if (!c || c->closing) return;
  if (bytes.size() > kMaxInbox - c->inbox.size() || !c->inbox.append(bytes)) {
    c->closing = true;
    return;
  }
  uint64_t at = 0;
  const ConstByteView in = c->inbox.view();
  while (!c->closing && in.size() - at >= kHeaderSize) {
    ByteReader h(in.subview(at, kHeaderSize));
    const uint32_t id = h.le32(), word = h.le32(), seq = h.le32(), n_fds = h.le32();
    const uint32_t size = word & 0xFFFFFF, opcode = word >> 24;
    if (size > in.size() - at - kHeaderSize) break;      // the rest has not come yet
    // the message's descriptors: the next n_fds that came (an fd argument is an index into them; one that did not come refers to nothing)
    c->message_in_fds.clear();
    for (uint32_t k = 0; k < n_fds && c->in_fds.count(); ++k) {
      c->message_in_fds.push_back(util::move(c->in_fds[0]));
      c->in_fds.remove_at(0);
    }
    c->message_seq = seq;
    dispatch(*c, id, opcode, in.subview(at + kHeaderSize, size));
    c->message_in_fds.clear();
    at += kHeaderSize + size;
  }
  drop_front(c->inbox, at);
  if (c->outbox.size() > kMaxOutbox) c->closing = true;
}

void NativeServer::protocol_error(Client& c, uint32_t id, int32_t res, const char* message) {
  send_error(c, id, c.message_seq, -res, message);
}

void NativeServer::dispatch(Client& c, uint32_t id, uint32_t opcode, ConstByteView payload) {
  pod::Parser outer(payload);
  pod::Parser args;
  if (!outer.structure(&args)) {
    protocol_error(c, id, kEPROTO, "invalid message");
    return;
  }
  Resource* r = resource(c, id);
  if (!r) return;      // (a proxy the server has let go of already: PipeWire ignores those too)
  if (r->registry) {
    registry_method(c, *r, opcode, args);
    return;
  }
  switch (r->type) {
    case ObjectType::Core: core_method(c, opcode, args); break;
    case ObjectType::Client: client_method(c, *r, opcode, args); break;
    case ObjectType::Node: case ObjectType::Port: case ObjectType::Device:
      if (r->client_node) client_node_method(c, *r, opcode, args);
      else node_method(c, *r, opcode, args);
      break;
    case ObjectType::Metadata:
      // set_property / clear: the one sink stays the default; accepted and answered with the properties as they are
      if (opcode == kMetadataSetProperty || opcode == kMetadataClear) send_metadata(c, *r);
      break;
    default: break;
  }
}

// ---- messages out ----------------------------------------------------------------------------------------------------------------------------------

uint64_t NativeServer::begin(Client& c, uint32_t id, uint32_t opcode) {
  const uint64_t start = c.outbox.size();
  c.message_start = start;
  c.message_fds = 0;
  c.outbox.put32(id);
  c.outbox.put32(opcode << 24);
  c.outbox.put32(++c.send_seq);
  c.outbox.put32(0);
  return start;
}

void NativeServer::end(Client& c, uint64_t start) {
  const uint64_t size = c.outbox.size() - start - kHeaderSize;
  if (size > kMaxPayload) {
    c.outbox.truncate(start);
    return;
  }
  ByteReader h(c.outbox.view().subview(start + 4, 4));
  const uint32_t opcode = h.le32() >> 24;
  c.outbox.patch32(start + 4, opcode << 24 | static_cast<uint32_t>(size));
  if (start == c.message_start) c.outbox.patch32(start + 12, c.message_fds);
}

void NativeServer::send_done(Client& c, uint32_t id, uint32_t seq) {
  const uint64_t m = begin(c, 0, kCoreEvDone);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(id));
  b.int32(static_cast<int32_t>(seq));
  b.end(f);
  end(c, m);
}

void NativeServer::send_error(Client& c, uint32_t id, uint32_t seq, int32_t res, const char* message) {
  const uint64_t m = begin(c, 0, kCoreEvError);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(id));
  b.int32(static_cast<int32_t>(seq));
  b.int32(res);
  b.string(message);
  b.end(f);
  end(c, m);
}

void NativeServer::send_remove_id(Client& c, uint32_t id) {
  const uint64_t m = begin(c, 0, kCoreEvRemoveId);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(id));
  b.end(f);
  end(c, m);
}

void NativeServer::send_core_info(Client& c) {
  graph_.visit(Graph::kCoreId, [&](const GraphObject& o) {
    const uint64_t m = begin(c, 0, kCoreEvInfo);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(0);
    b.int32(0x7c05ae11);      // cookie
    b.string("root");
    b.string("theorycraft");
    b.string("1.4.0");
    b.string("pipewire-0");
    b.int64(1);               // PW_CORE_CHANGE_MASK_PROPS
    push_dict(b, o.props);
    b.end(f);
    end(c, m);
  });
}

void NativeServer::send_info(Client& c, Resource& r) {
  graph_.visit(r.global, [&](const GraphObject& o) {
    r.seen = o.changed;
    pod::Builder b(c.outbox);
    uint64_t m = 0, f = 0;
    switch (o.type) {
      case ObjectType::Core:
        return;      // (sent by send_core_info)
      case ObjectType::Client:
        m = begin(c, r.id, kClientEvInfo);
        f = b.begin_struct();
        b.int32(static_cast<int32_t>(o.id));
        b.int64(1);
        push_dict(b, o.props);
        break;
      case ObjectType::Device:
        m = begin(c, r.id, kEvInfo);
        f = b.begin_struct();
        b.int32(static_cast<int32_t>(o.id));
        b.int64(3);
        push_dict(b, o.props);
        push_params(b, ArrayView<const ParamInfo>());
        break;
      case ObjectType::Node:
        m = begin(c, r.id, kEvInfo);
        f = b.begin_struct();
        b.int32(static_cast<int32_t>(o.id));
        {
          // (the sink has input ports and its monitor's output ports; a record node input ports; the others output ports)
          b.int32(o.role == NodeRole::Sink || o.role == NodeRole::Capture ? 64 : 0);      // max input ports
          b.int32(o.role == NodeRole::Capture ? 0 : 64);                                 // max output ports
        }
        b.int64(0x1f);      // input ports, output ports, state, props, params
        b.int32(static_cast<int32_t>(o.input_ports));
        b.int32(static_cast<int32_t>(o.output_ports));
        b.id(o.state);
        b.none();           // no error
        push_dict(b, o.props);
        push_params(b, ArrayView<const ParamInfo>(kNodeParams, 3));
        break;
      case ObjectType::Port:
        m = begin(c, r.id, kEvInfo);
        f = b.begin_struct();
        b.int32(static_cast<int32_t>(o.id));
        b.int32(static_cast<int32_t>(o.direction));
        b.int64(3);
        push_dict(b, o.props);
        push_params(b, ArrayView<const ParamInfo>(kPortParams, 2));
        break;
      case ObjectType::Link:
        m = begin(c, r.id, kEvInfo);
        f = b.begin_struct();
        b.int32(static_cast<int32_t>(o.id));
        b.int32(static_cast<int32_t>(o.output_node));
        b.int32(static_cast<int32_t>(o.output_port));
        b.int32(static_cast<int32_t>(o.input_node));
        b.int32(static_cast<int32_t>(o.input_port));
        b.int64(7);
        b.int32(kLinkStateActive);
        b.none();
        b.none();           // (no format negotiated: the mixer converts)
        push_dict(b, o.props);
        break;
      case ObjectType::Metadata:
        return;      // (metadata has properties, sent by send_metadata)
      case ObjectType::Factory: case ObjectType::Module:
        return;
    }
    b.end(f);
    end(c, m);
  });
}

void NativeServer::send_metadata(Client& c, Resource& r) {
  graph_.visit(r.global, [&](const GraphObject& o) {
    r.seen = o.changed;
    for (uint64_t i = 0; i < o.props.count(); ++i) {
      const Property& p = o.props.at(i);
      if (!p.key.view().starts_with("default.")) continue;
      const uint64_t m = begin(c, r.id, kMetadataEvProperty);
      pod::Builder b(c.outbox);
      const uint64_t f = b.begin_struct();
      b.int32(0);      // subject: the core
      b.string(p.key.view());
      b.string("Spa:String:JSON");
      b.string(p.value.view());
      b.end(f);
      end(c, m);
    }
  });
}

void NativeServer::send_global(Client& c, uint32_t registry, const GraphObject& o) {
  const uint64_t m = begin(c, registry, kRegistryEvGlobal);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(o.id));
  b.int32(static_cast<int32_t>(kPermAll));
  b.string(interface_name(o.type));
  b.int32(static_cast<int32_t>(interface_version(o.type)));
  push_dict(b, o.props);
  b.end(f);
  end(c, m);
}

void NativeServer::send_params(Client& c, Resource& r, uint32_t seq, uint32_t param_id, uint32_t start, uint32_t num) {
  if (start > 0 || num == 0) return;      // every param has a single value
  graph_.visit(r.global, [&](const GraphObject& o) {
    PcmFormat format = o.format;
    float volume = o.volume;
    bool mute = o.mute;
    if (o.type == ObjectType::Port) {
      format.channels = 1;
      if (param_id == kParamProps) return;
    } else if (o.type != ObjectType::Node) {
      return;
    }
    if (param_id != kParamProps && param_id != kParamEnumFormat && param_id != kParamFormat) return;
    // (MIDI and video have no volume; a video node or port says its producer's format once it has one)
    const bool media = o.role == NodeRole::MidiStream || o.role == NodeRole::MidiBridge || o.role == NodeRole::VideoStream;
    if (param_id == kParamProps && media) return;
    const VideoFormat video = o.role == NodeRole::VideoStream && o.video ? o.video->format() : VideoFormat();
    if (o.role == NodeRole::VideoStream && !video.valid()) return;
    const uint64_t m = begin(c, r.id, kEvParam);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(seq));
    b.id(param_id);
    b.int32(0);
    b.int32(1);
    if (param_id == kParamProps) build_props_param(b, volume, mute, format.channels, o.balance_left, o.balance_right);
    else if (o.role == NodeRole::MidiStream || o.role == NodeRole::MidiBridge) build_control_format_param(b, param_id);
    else if (o.role == NodeRole::VideoStream) build_video_format_param(b, param_id, video);
    else build_format_param(b, param_id, format);
    b.end(f);
    end(c, m);
  });
}

// ---- methods ---------------------------------------------------------------------------------------------------------------------------------------

void NativeServer::core_method(Client& c, uint32_t opcode, pod::Parser& args) {
  switch (opcode) {
    case kCoreHello: {
      int32_t version = 3;
      args.int32(&version);
      c.core_version = version > 0 ? static_cast<uint32_t>(version) : 3;
      send_core_info(c);
      if (Resource* r = resource(c, 1)) send_info(c, *r);
      break;
    }
    case kCoreSync: {
      int32_t id = 0, seq = 0;
      if (!args.int32(&id) || !args.int32(&seq)) { protocol_error(c, 0, kEINVAL, "invalid sync"); return; }
      update(c);      // (everything that happened before the sync arrives before its done)
      send_done(c, static_cast<uint32_t>(id), static_cast<uint32_t>(seq));
      break;
    }
    case kCorePong: case kCoreError: break;
    case kCoreGetRegistry: {
      int32_t version = 0, new_id = 0;
      if (!args.int32(&version) || !args.int32(&new_id)) { protocol_error(c, 0, kEINVAL, "invalid get_registry"); return; }
      Resource reg{static_cast<uint32_t>(new_id), ObjectType::Core, true, 0, static_cast<uint32_t>(version), 0, {}};
      if (!add_resource(c, reg)) { protocol_error(c, static_cast<uint32_t>(new_id), kEINVAL, "id in use"); return; }
      // a new registry is told of every global
      graph_.for_each([&](const GraphObject& o) { send_global(c, reg.id, o); });
      if (!c.has_registry) {
        c.has_registry = true;
        graph_.snapshot(&c.known);
      }
      break;
    }
    case kCoreCreateObject: {
      StringView factory, type;
      int32_t version = 0, new_id = 0;
      Properties props(allocator_);
      if (!args.string(&factory) || !args.string(&type) || !args.int32(&version) || !read_dict(args, &props) || !args.int32(&new_id)) {
        protocol_error(c, 0, kEINVAL, "invalid create_object");
        return;
      }
      if (factory == StringView("client-node") && type == StringView("PipeWire:Interface:ClientNode") && maker_) {
        create_client_node(c, static_cast<uint32_t>(new_id), props);
        break;
      }
      if (factory == StringView("adapter") && type == StringView("PipeWire:Interface:Node")) {
        create_adapter(c, static_cast<uint32_t>(new_id), static_cast<uint32_t>(version), props);
        break;
      }
      send_error(c, static_cast<uint32_t>(new_id), c.message_seq, -kENOTSUP,
                 "no such factory (client-node and adapter are served: play and record with pw_stream or the PulseAudio socket)");
      send_remove_id(c, static_cast<uint32_t>(new_id));
      break;
    }
    case kCoreDestroy: {
      int32_t id = 0;
      if (!args.int32(&id)) return;
      if (id <= 1) { protocol_error(c, static_cast<uint32_t>(id), kEINVAL, "cannot destroy the core or the client"); return; }
      if (Resource* r = resource(c, static_cast<uint32_t>(id))) {
        if (r->client_node) destroy_client_node(c, static_cast<uint32_t>(id));
        for (UniquePointer<VirtualSink>& v : virtuals_.view())
          if (v->owner == c.handle && v->resource == static_cast<uint32_t>(id)) {
            destroy_virtual(v->node);
            break;
          }
        remove_resource(c, static_cast<uint32_t>(id));
        send_remove_id(c, static_cast<uint32_t>(id));
      }
      break;
    }
    default: protocol_error(c, 0, kENOTSUP, "unknown core method"); break;
  }
}

void NativeServer::client_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args) {
  switch (opcode) {
    case kClientUpdateProperties: {
      Properties props(allocator_);
      if (!read_dict(args, &props)) { protocol_error(c, r.id, kEINVAL, "invalid properties"); return; }
      if (r.global == c.graph_client) graph_.update_client(c.graph_client, props, Properties::Mode::kReplace);
      break;
    }
    case kClientGetPermissions: {
      int32_t index = 0, num = 0;
      if (!args.int32(&index) || !args.int32(&num)) return;
      const uint64_t m = begin(c, r.id, kClientEvPermissions);
      pod::Builder b(c.outbox);
      const uint64_t f = b.begin_struct();
      b.int32(index);
      const uint64_t g = b.begin_struct();
      b.int32(index == 0 ? 1 : 0);
      if (index == 0) {
        b.int32(static_cast<int32_t>(kIdAny));
        b.int32(static_cast<int32_t>(kPermAll));
      }
      b.end(g);
      b.end(f);
      end(c, m);
      break;
    }
    case kClientError: case kClientUpdatePermissions: break;
    default: protocol_error(c, r.id, kENOTSUP, "unknown client method"); break;
  }
}

void NativeServer::registry_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args) {
  if (opcode == kRegistryDestroy) {
    protocol_error(c, r.id, kEPERM, "objects of the kernel's graph cannot be destroyed this way");
    return;
  }
  if (opcode != kRegistryBind) { protocol_error(c, r.id, kENOTSUP, "unknown registry method"); return; }
  int32_t global = 0, version = 0, new_id = 0;
  StringView type;
  if (!args.int32(&global) || !args.string(&type) || !args.int32(&version) || !args.int32(&new_id)) {
    protocol_error(c, r.id, kEINVAL, "invalid bind");
    return;
  }
  ObjectType wanted;
  ObjectType actual = ObjectType::Core;
  Properties props(allocator_);
  const bool exists = graph_.visit(static_cast<uint32_t>(global), [&](const GraphObject& o) {
    actual = o.type;
    props.copy_from(o.props);
  });
  if (!exists) {
    send_error(c, static_cast<uint32_t>(new_id), c.message_seq, -kENOENT, "unknown global");
    send_remove_id(c, static_cast<uint32_t>(new_id));
    return;
  }
  if (!interface_from_name(type, &wanted) || wanted != actual) {
    send_error(c, static_cast<uint32_t>(new_id), c.message_seq, -kEINVAL, "wrong interface for the global");
    send_remove_id(c, static_cast<uint32_t>(new_id));
    return;
  }
  Resource bound{static_cast<uint32_t>(new_id), actual, false, static_cast<uint32_t>(global), static_cast<uint32_t>(version), 0, {}};
  if (!add_resource(c, bound)) { protocol_error(c, static_cast<uint32_t>(new_id), kEINVAL, "id in use"); return; }
  // bound_props (core 4) / bound_id, then the info
  const uint64_t m = begin(c, 0, c.core_version >= 4 ? kCoreEvBoundProps : kCoreEvBoundId);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(new_id);
  b.int32(global);
  if (c.core_version >= 4) push_dict(b, props);
  b.end(f);
  end(c, m);
  Resource* res = resource(c, static_cast<uint32_t>(new_id));
  if (actual == ObjectType::Core) send_core_info(c);
  else if (actual == ObjectType::Metadata) send_metadata(c, *res);
  else send_info(c, *res);
}

void NativeServer::node_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args) {
  switch (opcode) {
    case kNodeSubscribeParams: {
      pod::Value ids;
      pod::ArrayReader a;
      if (!args.pod(&ids) || !a.open(ids)) { protocol_error(c, r.id, kEINVAL, "invalid subscribe_params"); return; }
      for (bool& s : r.subscribed) s = false;
      for (uint64_t i = 0; i < a.count() && i < 32; ++i) {
        uint32_t id = 0;
        if (!a.get32(i, &id)) continue;
        const int k = subscribed_index(id);
        if (k >= 0) r.subscribed[k] = true;
        send_params(c, r, 0, id, 0, 1);
      }
      break;
    }
    case kNodeEnumParams: {
      int32_t seq = 0, index = 0, num = 0;
      uint32_t id = 0;
      pod::Value filter;
      if (!args.int32(&seq) || !args.id(&id) || !args.int32(&index) || !args.int32(&num)) { protocol_error(c, r.id, kEINVAL, "invalid enum_params"); return; }
      args.pod(&filter);      // (the filter is not applied: there is one value)
      send_params(c, r, static_cast<uint32_t>(seq), id, static_cast<uint32_t>(index < 0 ? 0 : index), static_cast<uint32_t>(num));      // (num is unsigned: -1 is "all")
      break;
    }
    case kNodeSetParam: {
      if (r.type != ObjectType::Node) break;
      uint32_t id = 0;
      int32_t flags = 0;
      pod::Value param;
      if (!args.id(&id) || !args.int32(&flags) || !args.pod(&param)) { protocol_error(c, r.id, kEINVAL, "invalid set_param"); return; }
      if (id != kParamProps) { protocol_error(c, r.id, kENOTSUP, "only Props can be set"); return; }
      float volume = 1.0f;
      bool mute = false;
      if (!graph_.get_volume(r.global, &volume, &mute)) return;
      float sides[2] = {volume, volume};
      bool have_sides = false;
      pod::ObjectReader o;
      if (!o.open(param)) { protocol_error(c, r.id, kEINVAL, "Props must be an object"); return; }
      uint32_t key = 0, pflags = 0;
      pod::Value v;
      while (o.next(&key, &pflags, &v)) {
        if (key == kPropVolume) v.get_float(&volume);
        else if (key == kPropMute || key == kPropSoftMute) v.get_bool(&mute);
        else if (key == kPropChannelVolumes || key == kPropSoftVolumes) {
          pod::ArrayReader a;
          if (a.open(v) && a.child_type() == pod::kFloat && a.count() > 0) {
            float loudest = 0.0f;
            for (uint64_t i = 0; i < a.count(); ++i) {
              uint32_t bits = 0;
              a.get32(i, &bits);
              const float f = __builtin_bit_cast(float, bits);
              if (f > loudest) loudest = f;
              if (i < 2) sides[i] = f;
            }
            volume = loudest;
            have_sides = a.count() >= 2;
          }
        }
      }
      // two channels or more: the first two are front left and right (the channel map the node reports), a balance
      if (have_sides) graph_.set_balance(r.global, sides[0], sides[1], mute);
      else graph_.set_volume(r.global, volume, mute);
      break;
    }
    case kNodeSendCommand: break;
    default: protocol_error(c, r.id, kENOTSUP, "unknown method"); break;
  }
}

// ---- changes ---------------------------------------------------------------------------------------------------------------------------------------

void NativeServer::send_globals(Client& c) {
  // both lists are in id order
  uint64_t i = 0, j = 0;
  const ArrayView<const GraphEntry> was = c.known.view(), now = snapshot_.view();
  auto registries = [&](auto&& f) {
    for (uint64_t k = 0; k < c.resources.count(); ++k)
      if (c.resources[k].registry) f(c.resources[k].id);
  };
  List<uint32_t> gone(allocator_);
  while (i < was.size() || j < now.size()) {
    if (j >= now.size() || (i < was.size() && was[i].id < now[j].id)) {
      const uint32_t id = was[i].id;
      registries([&](uint32_t reg) {
        const uint64_t m = begin(c, reg, kRegistryEvGlobalRemove);
        pod::Builder b(c.outbox);
        const uint64_t f = b.begin_struct();
        b.int32(static_cast<int32_t>(id));
        b.end(f);
        end(c, m);
      });
      gone.push_back(id);
      ++i;
    } else if (i >= was.size() || now[j].id < was[i].id) {
      const uint32_t id = now[j].id;
      graph_.visit(id, [&](const GraphObject& o) { registries([&](uint32_t reg) { send_global(c, reg, o); }); });
      ++j;
    } else {
      ++i;
      ++j;
    }
  }
  // the proxies bound to globals that went
  for (uint32_t id : gone)
    for (uint64_t k = c.resources.count(); k > 0; --k) {
      const Resource& r = c.resources[k - 1];
      if (!r.registry && !r.client_node && r.global == id && r.id > 1) {
        const uint32_t proxy = r.id;
        c.resources.remove_at(k - 1);
        send_remove_id(c, proxy);
      }
    }
  c.known.clear();
  for (const GraphEntry& e : now) c.known.push_back(e);
}

void NativeServer::update(Client& c) {
  if (graph_.serial() != last_serial_) {
    last_serial_ = graph_.serial();
    graph_.snapshot(&snapshot_);
  }
  if (c.has_registry) send_globals(c);
  for (uint64_t k = 0; k < c.resources.count(); ++k) {
    Resource& r = c.resources[k];
    if (r.registry || r.type == ObjectType::Core || r.client_node) continue;
    uint64_t changed = 0;
    for (const GraphEntry& e : snapshot_)
      if (e.id == r.global) changed = e.changed;
    if (changed == 0 || changed <= r.seen) continue;
    if (r.type == ObjectType::Metadata) {
      send_metadata(c, r);
      continue;
    }
    send_info(c, r);
    // subscribed params are sent again with the info
    static constexpr uint32_t kIds[3] = {kParamProps, kParamEnumFormat, kParamFormat};
    for (int s = 0; s < 3; ++s)
      if (r.subscribed[s]) send_params(c, r, 0, kIds[s], 0, 1);
  }
}

void NativeServer::tick() {
  ++ticks_;
  graph_.refresh();
  const uint64_t now = now_ns();
  // the client nodes' cycles first: they are the time-critical part (and the ones still waiting for something, a video watcher for its producer)
  uint32_t running = 0;
  for (UniquePointer<Client>& c : clients_.view()) {
    if (c->closing) continue;
    for (UniquePointer<ClientNode>& n : c->nodes.view()) {
      if (!n->started) {
        if (n->active && n->node) negotiate(*c, *n);
        continue;
      }
      ++running;
      // a watcher whose producer changed its format is negotiated again (as is a producer that said a new one)
      if (n->video)
        for (UniquePointer<ClientPort>& p : n->ports.view())
          if (p->media == Media::Video && p->direction == 0 && p->format_sent && !p->renegotiate) {
            const VideoFormat now_format = n->video->format();
            if (now_format.valid() && !(now_format == p->source_video)) p->renegotiate = n->renegotiate = true;
            // (shared buffers: let go of by the producer, or newly shared and of a kind this watcher takes as it is: given the producer's own)
            if (p->buffers_sent && !p->renegotiate && watcher_share_changed(*p, *n->video)) p->renegotiate = n->renegotiate = true;
          }
      if (n->renegotiate) negotiate(*c, *n);
      run_client_node(*n, now);
    }
  }
  running_nodes_ = running;
  midi_waiting_ = graph_.midi_bus().pump(now) != 0;      // (programs' MIDI whose time has come, to the synthesizer)
  for (UniquePointer<Client>& c : clients_.view()) {
    if (c->closing) continue;
    update(*c);
    if (c->outbox.size() > kMaxOutbox) c->closing = true;
  }
}

}  // namespace pipewire
