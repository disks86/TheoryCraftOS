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
// and get_permissions, Node and Port enum_params / subscribe_params (EnumFormat, Format, Props), Node set_param(Props) for volume and mute (the stream's
// volume, the sink's is the master volume), and the "default" metadata's properties.
//
// Not served: create_object (Core error -ENOTSUP): programs that play through libpipewire's pw_stream need the client-node factory with shared-memory
// buffers and eventfds, which the Linux layer does not have yet - they play through the PulseAudio socket instead (PulseServer.hpp), as most do.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "PipeWire/Pod.hpp"
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

  NativeServer(Graph& graph, allocators::IAllocator& allocator);
  ~NativeServer();

  uint32_t open_client(uint64_t pid) override;
  void receive(uint32_t client, ConstByteView bytes) override;
  bool take_output(uint32_t client, ByteBuffer* out) override;
  void close_client(uint32_t client) override;
  void tick() override;
  uint64_t tick_interval_us() const override { return 50000; }

  uint32_t client_count() const { return static_cast<uint32_t>(clients_.count()); }

 private:
  struct Resource {
    uint32_t id;           // the program's id for it
    ObjectType type;
    bool registry;         // a registry (type unused)
    uint32_t global;       // the graph object it is bound to
    uint32_t version;
    uint64_t seen;         // the object's graph serial when its info was last sent
    bool subscribed[3];    // node/port param subscriptions: Props, EnumFormat, Format
  };
  struct Client {
    explicit Client(allocators::IAllocator& a) : inbox(a), outbox(a), resources(a), known(a) {}
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

  Graph& graph_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Client>> clients_;
  List<GraphEntry> snapshot_;
  uint32_t next_handle_ = 1;
  uint64_t last_serial_ = 0;
};

// Builders shared with the tests: the param objects of a node (Props, EnumFormat/Format) as PipeWire writes them.
void build_props_param(pod::Builder& b, float volume, bool mute, uint32_t channels);
void build_format_param(pod::Builder& b, uint32_t param_id, const PcmFormat& format);

}  // namespace pipewire
