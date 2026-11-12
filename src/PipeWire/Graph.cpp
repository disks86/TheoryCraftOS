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

#include "PipeWire/Graph.hpp"

namespace pipewire {

namespace {

constexpr const char* kSinkName = "tcos_output.stereo";
constexpr const char* kSourceName = "tcos_input.stereo";
constexpr uint32_t kFL = 3, kFR = 4, kMono = 2;

const char* channel_name(uint32_t position) {
  switch (position) {
    case kFL: return "FL";
    case kFR: return "FR";
    default: return "MONO";
  }
}

uint8_t to_device_volume(float volume) {
  if (!(volume > 0.0f)) return 0;
  if (volume >= 1.0f) return hardware::kAudioMaxVolume;
  return static_cast<uint8_t>(volume * hardware::kAudioMaxVolume + 0.5f);
}

void set_port_props(GraphObject& port, uint32_t node, uint32_t index, uint32_t direction, const char* prefix, allocators::IAllocator& allocator) {
  String name(allocator);
  name.append(prefix);
  name.append(channel_name(port.channel));
  port.node = node;
  port.direction = direction;
  port.props.set("port.name", name.view());
  port.props.set("port.direction", direction == kDirectionInput ? "in" : "out");
  port.props.set_uint("port.id", index);
  port.props.set_uint("node.id", node);
  port.props.set("audio.channel", channel_name(port.channel));
  port.props.set("format.dsp", "32 bit float mono audio");
}

}  // namespace

const char* interface_name(ObjectType type) {
  switch (type) {
    case ObjectType::Core: return "PipeWire:Interface:Core";
    case ObjectType::Client: return "PipeWire:Interface:Client";
    case ObjectType::Device: return "PipeWire:Interface:Device";
    case ObjectType::Node: return "PipeWire:Interface:Node";
    case ObjectType::Port: return "PipeWire:Interface:Port";
    case ObjectType::Link: return "PipeWire:Interface:Link";
    case ObjectType::Metadata: return "PipeWire:Interface:Metadata";
    case ObjectType::Factory: return "PipeWire:Interface:Factory";
    case ObjectType::Module: return "PipeWire:Interface:Module";
  }
  return "";
}

uint32_t interface_version(ObjectType type) {
  switch (type) {
    case ObjectType::Core: return 4;
    case ObjectType::Client: return 3;
    case ObjectType::Device: return 3;
    case ObjectType::Node: return 3;
    case ObjectType::Port: return 3;
    case ObjectType::Link: return 3;
    case ObjectType::Metadata: return 3;
    case ObjectType::Factory: return 3;
    case ObjectType::Module: return 3;
  }
  return 3;
}

bool interface_from_name(StringView name, ObjectType* out) {
  static constexpr ObjectType kAll[] = {ObjectType::Core, ObjectType::Client, ObjectType::Device, ObjectType::Node, ObjectType::Port,
                                        ObjectType::Link, ObjectType::Metadata, ObjectType::Factory, ObjectType::Module};
  for (ObjectType t : kAll)
    if (name == StringView(interface_name(t))) {
      *out = t;
      return true;
    }
  return false;
}

Graph::Graph(allocators::IAllocator& allocator, audio::AudioMixer* mixer, audio::AudioCaptureHub* capture)
    : allocator_(allocator), mixer_(mixer), capture_(capture), objects_(allocator), retired_(allocator) {}

Graph::~Graph() {
  // the streams that are still playing stop with the graph
  for (UniquePointer<GraphObject>& o : objects_.view()) {
    if (o->playback) {
      o->playback->flush();
      o->playback->close();
    }
    if (o->record) o->record->close();
  }
}

bool Graph::has_input() { return capture_ && capture_->source() != nullptr; }

GraphObject* Graph::find_locked(uint32_t id) {
  // objects_ is in id order
  uint64_t lo = 0, hi = objects_.count();
  while (lo < hi) {
    const uint64_t mid = (lo + hi) / 2;
    const uint32_t at = objects_[mid]->id;
    if (at == id) return objects_[mid].get();
    if (at < id) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

GraphObject* Graph::add_locked(ObjectType type, uint32_t client) {
  if (objects_.count() >= kMaxObjects || next_id_ == UINT32_MAX) return nullptr;
  UniquePointer<GraphObject> o = UniquePointer<GraphObject>::make(allocator_, allocator_);
  if (!o) return nullptr;
  o->id = next_id_;
  o->object_serial = next_object_serial_;
  o->type = type;
  o->client = client;
  o->changed = ++serial_;
  GraphObject* raw = o.get();
  if (!objects_.push_back(util::move(o))) return nullptr;
  ++next_id_;
  ++next_object_serial_;
  raw->props.set_uint("object.id", raw->id);
  raw->props.set_uint("object.serial", raw->object_serial);
  if (client) raw->props.set_uint("client.id", client);
  return raw;
}

void Graph::remove_locked(uint32_t id) {
  for (uint64_t i = 0; i < objects_.count(); ++i)
    if (objects_[i]->id == id) {
      objects_.remove_at(i);
      ++serial_;
      return;
    }
}

bool Graph::initialize() {
  // (the hub's lock is not taken under the graph's)
  const uint8_t gain = capture_ ? capture_->master_gain() : 0;
  const bool gain_muted = capture_ ? capture_->master_muted() : false;
  kernel::ScopedMutex guard(mutex_);
  if (objects_.count()) return true;
  GraphObject* core = add_locked(ObjectType::Core, 0);
  if (!core) return false;
  core->props.set("core.name", "pipewire-0");
  core->props.set("core.daemon", "true");
  core->props.set("default.clock.rate", "48000");
  core->props.set("default.clock.quantum", "1024");
  core->props.set("cpu.max-align", "32");
  core->props.set("link.max-buffers", "16");
  core->props.set("mem.allow-mlock", "false");
  core->props.set("object.id", "0");

  GraphObject* client = add_locked(ObjectType::Client, 0);
  if (!client) return false;
  native_client_ = client->id;
  client->origin = Origin::Native;
  client->props.set("pipewire.protocol", "kernel");
  client->props.set("client.api", "tcos-native");
  client->props.set("application.name", "TheoryCraftOS");
  client->props.set("application.process.id", "0");

  GraphObject* metadata = add_locked(ObjectType::Metadata, 0);
  if (!metadata) return false;
  metadata_ = metadata->id;
  metadata->props.set("metadata.name", "default");

  GraphObject* device = add_locked(ObjectType::Device, 0);
  if (!device) return false;
  device_ = device->id;
  device->props.set("device.api", "tcos");
  device->props.set("device.name", "tcos_card.output");
  device->props.set("device.description", "Audio output");
  device->props.set("media.class", "Audio/Device");
  device->props.set("device.bus", "pci");

  GraphObject* sink = add_locked(ObjectType::Node, 0);
  if (!sink) return false;
  sink_ = sink->id;
  sink->role = NodeRole::Sink;
  sink->format = PcmFormat{SampleFormat::S16Le, hardware::kAudioSampleRate, 2};
  sink->input_ports = 2;
  sink->props.set("node.name", kSinkName);
  sink->props.set("node.description", "Audio output");
  sink->props.set("node.nick", "Audio output");
  sink->props.set("media.class", "Audio/Sink");
  sink->props.set_uint("device.id", device_);
  sink->props.set("audio.channels", "2");
  sink->props.set("audio.rate", "48000");
  sink->props.set("audio.format", "S16LE");
  sink->props.set("audio.position", "FL,FR");
  sink->props.set("priority.session", "1000");
  sink->props.set("factory.name", "api.tcos.mixer");
  static constexpr uint32_t kPositions[2] = {kFL, kFR};
  for (uint32_t i = 0; i < 2; ++i) {
    GraphObject* port = add_locked(ObjectType::Port, 0);
    if (!port) return false;
    port->channel = kPositions[i];
    set_port_props(*port, sink_, i, kDirectionInput, "playback_", allocator_);
    sink_ports_[i] = port->id;
  }
  // the monitor: what the mixer plays, for record streams of the monitor (PulseAudio's tcos_output.stereo.monitor, WASAPI loopback)
  sink->output_ports = 2;
  for (uint32_t i = 0; i < 2; ++i) {
    GraphObject* port = add_locked(ObjectType::Port, 0);
    if (!port) return false;
    port->channel = kPositions[i];
    set_port_props(*port, sink_, i, kDirectionOutput, "monitor_", allocator_);
    port->props.set("port.monitor", "true");
    monitor_ports_[i] = port->id;
  }
  if (capture_) {
    if (!add_source_locked()) return false;
    source_gain_ = gain;
    source_muted_ = gain_muted;
    if (GraphObject* source = find_locked(source_)) {
      source->volume = gain / float(hardware::kAudioMaxVolume);
      source->mute = gain_muted;
    }
  }
  set_default_metadata_locked();
  if (mixer_) {
    master_ = mixer_->master_volume();
    master_muted_ = mixer_->master_muted();
    sink->volume = master_ / float(hardware::kAudioMaxVolume);
    sink->mute = master_muted_;
  }
  return true;
}

bool Graph::add_source_locked() {
  GraphObject* device = add_locked(ObjectType::Device, 0);
  if (!device) return false;
  source_device_ = device->id;
  device->props.set("device.api", "tcos");
  device->props.set("device.name", "tcos_card.input");
  device->props.set("device.description", "Audio input");
  device->props.set("media.class", "Audio/Device");
  device->props.set("device.bus", "pci");

  GraphObject* source = add_locked(ObjectType::Node, 0);
  if (!source) return false;
  source_ = source->id;
  source->role = NodeRole::Source;
  source->format = PcmFormat{SampleFormat::S16Le, hardware::kAudioSampleRate, 2};
  source->output_ports = 2;
  source->state = kNodeStateSuspended;      // (refresh() tells it idle once there is a microphone)
  source->props.set("node.name", kSourceName);
  source->props.set("node.description", "Audio input");
  source->props.set("node.nick", "Audio input");
  source->props.set("media.class", "Audio/Source");
  source->props.set_uint("device.id", source_device_);
  source->props.set("audio.channels", "2");
  source->props.set("audio.rate", "48000");
  source->props.set("audio.format", "S16LE");
  source->props.set("audio.position", "FL,FR");
  source->props.set("priority.session", "2000");
  source->props.set("factory.name", "api.tcos.capture");
  static constexpr uint32_t kPositions[2] = {kFL, kFR};
  for (uint32_t i = 0; i < 2; ++i) {
    GraphObject* port = add_locked(ObjectType::Port, 0);
    if (!port) return false;
    port->channel = kPositions[i];
    set_port_props(*port, source_, i, kDirectionOutput, "capture_", allocator_);
    source_ports_[i] = port->id;
  }
  return true;
}

void Graph::set_default_metadata_locked() {
  if (GraphObject* m = find_locked(metadata_)) {
    m->props.set("default.audio.sink", "{\"name\":\"tcos_output.stereo\"}");
    m->props.set("default.configured.audio.sink", "{\"name\":\"tcos_output.stereo\"}");
    if (source_) {
      m->props.set("default.audio.source", "{\"name\":\"tcos_input.stereo\"}");
      m->props.set("default.configured.audio.source", "{\"name\":\"tcos_input.stereo\"}");
    }
    touch_locked(*m);
  }
}

uint64_t Graph::serial() {
  kernel::ScopedMutex guard(mutex_);
  return serial_;
}

bool Graph::snapshot(List<GraphEntry>* out) {
  out->clear();
  kernel::ScopedMutex guard(mutex_);
  if (!out->reserve(objects_.count())) return false;
  for (const UniquePointer<GraphObject>& o : objects_.view())
    if (!out->push_back(GraphEntry{o->id, o->type, o->changed, o->role})) return false;
  return true;
}

uint32_t Graph::record_count() {
  kernel::ScopedMutex guard(mutex_);
  uint32_t n = 0;
  for (const UniquePointer<GraphObject>& o : objects_.view())
    if (o->type == ObjectType::Node && o->role == NodeRole::Capture) ++n;
  return n;
}

uint32_t Graph::playback_count() {
  kernel::ScopedMutex guard(mutex_);
  uint32_t n = 0;
  for (const UniquePointer<GraphObject>& o : objects_.view())
    if (o->type == ObjectType::Node && o->role == NodeRole::Playback) ++n;
  return n;
}

// ---- clients ---------------------------------------------------------------------------------------------------------------------------------------

uint32_t Graph::add_client(Origin origin, uint64_t pid, StringView protocol, StringView api) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* c = add_locked(ObjectType::Client, 0);
  if (!c) return 0;
  c->origin = origin;
  c->pid = pid;
  c->props.set("pipewire.protocol", protocol);
  if (!api.empty()) c->props.set("client.api", api);
  c->props.set_uint("application.process.id", pid);
  c->props.set("pipewire.access", "unrestricted");
  return c->id;
}

bool Graph::update_client(uint32_t client, const Properties& props, Properties::Mode mode) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* c = find_locked(client);
  if (!c || c->type != ObjectType::Client) return false;
  // the graph's own keys stay as the graph has them
  Properties kept(allocator_);
  static constexpr const char* kOwn[] = {"object.id", "object.serial", "pipewire.protocol", "client.api", "application.process.id", "pipewire.access"};
  for (const char* k : kOwn)
    if (c->props.has(k)) kept.set(k, c->props.get(k));
  if (!c->props.update(props, mode)) return false;
  c->props.update(kept, Properties::Mode::kReplace);
  touch_locked(*c);
  return true;
}

void Graph::remove_client(uint32_t client) {
  List<SharedPointer<PlaybackStream>> stopped(allocator_);
  {
    kernel::ScopedMutex guard(mutex_);
    for (uint64_t i = objects_.count(); i > 0; --i) {
      GraphObject& o = *objects_[i - 1];
      if (o.type == ObjectType::Node && o.client == client) remove_node_locked(o.id, &stopped, false);
    }
    remove_locked(client);
  }
  for (SharedPointer<PlaybackStream>& s : stopped) {
    s->flush();
    s->close();
  }
}

// ---- streams ---------------------------------------------------------------------------------------------------------------------------------------

void Graph::name_stream_node_locked(GraphObject& node, uint32_t client, const PcmFormat& format) {
  // what the graph owns
  node.props.set_uint("object.id", node.id);
  node.props.set_uint("object.serial", node.object_serial);
  node.props.set_uint("client.id", client);
  if (!node.props.has("node.name") || node.props.get("node.name").empty()) {
    String name(allocator_);
    name.append(node.props.has("application.name") ? node.props.get("application.name") : StringView("stream"));
    name.push_back('.');
    name.append_uint(node.id);
    node.props.set("node.name", name.view());
  }
  if (!node.props.has("media.name")) node.props.set("media.name", node.props.get("node.name"));
  if (!node.props.has("media.type")) node.props.set("media.type", "Audio");
  node.props.set_uint("audio.channels", format.channels);
  node.props.set_uint("audio.rate", format.rate);
  node.props.set("audio.format", format.name());
  node.props.set("node.autoconnect", "true");
  if (GraphObject* c = find_locked(client)) {
    if (!node.props.has("application.name") && c->props.has("application.name")) node.props.set("application.name", c->props.get("application.name"));
    node.props.set_uint("application.process.id", c->pid);
  }
}

uint32_t Graph::add_stream_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props) {
  GraphObject* node = add_locked(ObjectType::Node, client);
  if (!node) return 0;
  node->role = NodeRole::Playback;
  node->origin = origin;
  node->format = format;
  node->state = kNodeStateIdle;      // (refresh() tells it running once something is queued)
  node->props.update(props, Properties::Mode::kReplace);
  node->props.set("media.class", "Stream/Output/Audio");
  node->props.set("target.object", kSinkName);
  name_stream_node_locked(*node, client, format);
  const uint32_t node_id = node->id;
  // ports: mono, or front left and right (more channels are folded into two)
  const uint32_t ports = format.channels == 1 ? 1 : 2;
  node->output_ports = ports;
  for (uint32_t i = 0; i < ports; ++i) {
    const uint32_t position = ports == 1 ? kMono : (i == 0 ? kFL : kFR);
    GraphObject* port = add_locked(ObjectType::Port, client);
    if (!port) return node_id;
    port->channel = position;
    set_port_props(*port, node_id, i, kDirectionOutput, "output_", allocator_);
    const uint32_t port_id = port->id;
    // links to the sink: mono to both sides
    for (uint32_t s = 0; s < 2; ++s) {
      if (ports == 2 && s != i) continue;
      GraphObject* link = add_locked(ObjectType::Link, client);
      if (!link) return node_id;
      link->output_node = node_id;
      link->output_port = port_id;
      link->input_node = sink_;
      link->input_port = sink_ports_[s];
      link->props.set_uint("link.output.node", node_id);
      link->props.set_uint("link.output.port", port_id);
      link->props.set_uint("link.input.node", sink_);
      link->props.set_uint("link.input.port", sink_ports_[s]);
      link->props.set("factory.name", "link-factory");
    }
  }
  return node_id;
}

uint32_t Graph::create_playback(uint32_t client, const PcmFormat& format, const Properties& props, SharedPointer<PlaybackStream>* out) {
  *out = SharedPointer<PlaybackStream>();
  if (!mixer_ || !format.valid()) return 0;
  {
    kernel::ScopedMutex guard(mutex_);
    GraphObject* c = find_locked(client);
    if (!c || c->type != ObjectType::Client) return 0;
  }
  audio::StreamFormat sf;
  sf.sample_rate = format.rate;
  sf.channels = format.channels == 1 ? 1 : 2;
  sf.type = audio::SampleType::S16;
  SharedPointer<audio::AudioStream> stream = mixer_->open_stream(sf, kStreamRingBytes);
  if (!stream) return 0;
  StringView label = props.get("application.name");
  if (label.empty()) label = props.get("media.name");
  stream->set_label(label);
  SharedPointer<PlaybackStream> playback = SharedPointer<PlaybackStream>::make(allocator_, allocator_, format, stream, mixer_);
  if (!playback) {
    stream->close();
    return 0;
  }
  uint32_t id = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    Origin origin = Origin::Kernel;
    if (GraphObject* c = find_locked(client)) origin = c->origin;
    id = add_stream_node_locked(client, origin, format, props);
    if (GraphObject* node = id ? find_locked(id) : nullptr) node->playback = playback;
  }
  if (!id) {
    stream->close();
    return 0;
  }
  *out = playback;
  return id;
}

uint32_t Graph::add_record_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props, bool monitor) {
  GraphObject* node = add_locked(ObjectType::Node, client);
  if (!node) return 0;
  node->role = NodeRole::Capture;
  node->monitor = monitor;
  node->origin = origin;
  node->format = format;
  node->state = kNodeStateRunning;
  node->props.update(props, Properties::Mode::kReplace);
  node->props.set("media.class", "Stream/Input/Audio");
  node->props.set("target.object", monitor ? kSinkName : kSourceName);
  if (monitor) node->props.set("stream.capture.sink", "true");
  name_stream_node_locked(*node, client, format);
  const uint32_t node_id = node->id;
  const uint32_t from_node = monitor ? sink_ : source_;
  const uint32_t* from_ports = monitor ? monitor_ports_ : source_ports_;
  // ports: mono, or front left and right (more channels are made from those two)
  const uint32_t ports = format.channels == 1 ? 1 : 2;
  node->input_ports = ports;
  for (uint32_t i = 0; i < ports; ++i) {
    GraphObject* port = add_locked(ObjectType::Port, client);
    if (!port) return node_id;
    port->channel = ports == 1 ? kMono : (i == 0 ? kFL : kFR);
    set_port_props(*port, node_id, i, kDirectionInput, "input_", allocator_);
    const uint32_t port_id = port->id;
    // links from the source: both sides into a mono port
    for (uint32_t s = 0; s < 2; ++s) {
      if (ports == 2 && s != i) continue;
      GraphObject* link = add_locked(ObjectType::Link, client);
      if (!link) return node_id;
      link->output_node = from_node;
      link->output_port = from_ports[s];
      link->input_node = node_id;
      link->input_port = port_id;
      link->props.set_uint("link.output.node", from_node);
      link->props.set_uint("link.output.port", from_ports[s]);
      link->props.set_uint("link.input.node", node_id);
      link->props.set_uint("link.input.port", port_id);
      link->props.set("factory.name", "link-factory");
    }
  }
  return node_id;
}

uint32_t Graph::create_record(uint32_t client, const PcmFormat& format, const Properties& props, SharedPointer<RecordStream>* out, bool monitor) {
  *out = SharedPointer<RecordStream>();
  if (monitor ? !mixer_ : (!capture_ || !source_)) return 0;
  if (!format.valid()) return 0;
  {
    kernel::ScopedMutex guard(mutex_);
    GraphObject* c = find_locked(client);
    if (!c || c->type != ObjectType::Client) return 0;
  }
  audio::StreamFormat sf;
  sf.sample_rate = format.rate;
  sf.channels = format.channels == 1 ? 1 : 2;
  sf.type = audio::SampleType::S16;
  // half a second at the program's rate (a program that reads less often than that loses audio)
  uint32_t ring = sf.frame_size() * (format.rate / 2);
  if (ring < 16384) ring = 16384;
  SharedPointer<audio::CaptureStream> capture;
  if (monitor) {
    // (a stream of its own that the mixer feeds what it mixes)
    capture = SharedPointer<audio::CaptureStream>::make(allocator_, allocator_, sf);
    if (capture && (!capture->allocate(ring) || !mixer_->add_tap(capture))) capture = SharedPointer<audio::CaptureStream>();
  } else {
    capture = capture_->open_stream(sf, ring, true);      // (marked: refresh() must not take it for a native program's)
  }
  if (!capture) return 0;
  SharedPointer<RecordStream> record = SharedPointer<RecordStream>::make(allocator_, allocator_, format, capture);
  if (!record) {
    capture->close();
    return 0;
  }
  uint32_t id = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    Origin origin = Origin::Kernel;
    if (GraphObject* c = find_locked(client)) origin = c->origin;
    id = add_record_node_locked(client, origin, format, props, monitor);
    if (GraphObject* node = id ? find_locked(id) : nullptr) node->record = record;
  }
  if (!id) {
    capture->close();
    return 0;
  }
  *out = record;
  return id;
}

void Graph::remove_node_locked(uint32_t node_id, List<SharedPointer<PlaybackStream>>* stopped, bool drain) {
  GraphObject* node = find_locked(node_id);
  if (!node || node->type != ObjectType::Node || (node->role != NodeRole::Playback && node->role != NodeRole::Capture)) return;
  if (node->record) node->record->close();
  if (node->playback) {
    retired_.push_back(node->playback->shared_stream());
    if (drain) node->playback->close();
    else stopped->push_back(node->playback);
  }
  // its ports and links go with it
  for (uint64_t i = objects_.count(); i > 0; --i) {
    GraphObject& o = *objects_[i - 1];
    const bool mine = (o.type == ObjectType::Port && o.node == node_id) ||
                      (o.type == ObjectType::Link && (o.output_node == node_id || o.input_node == node_id));
    if (mine) {
      objects_.remove_at(i - 1);
      ++serial_;
    }
  }
  remove_locked(node_id);
}

void Graph::destroy_node(uint32_t node, bool drain) {
  List<SharedPointer<PlaybackStream>> stopped(allocator_);
  {
    kernel::ScopedMutex guard(mutex_);
    remove_node_locked(node, &stopped, drain);
  }
  for (SharedPointer<PlaybackStream>& s : stopped) {
    s->flush();
    s->close();
  }
}

bool Graph::update_node(uint32_t node_id, const Properties& props, Properties::Mode mode) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* node = find_locked(node_id);
  if (!node || node->type != ObjectType::Node) return false;
  Properties kept(allocator_);
  static constexpr const char* kOwn[] = {"object.id", "object.serial", "client.id", "media.class", "audio.channels", "audio.rate", "audio.format", "node.name"};
  for (const char* k : kOwn)
    if (node->props.has(k)) kept.set(k, node->props.get(k));
  if (!node->props.update(props, mode)) return false;
  node->props.update(kept, Properties::Mode::kReplace);
  touch_locked(*node);
  return true;
}

SharedPointer<RecordStream> Graph::record_of(uint32_t node_id) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* node = find_locked(node_id);
  return node && node->type == ObjectType::Node ? node->record : SharedPointer<RecordStream>();
}

SharedPointer<PlaybackStream> Graph::playback_of(uint32_t node_id) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* node = find_locked(node_id);
  return node && node->type == ObjectType::Node ? node->playback : SharedPointer<PlaybackStream>();
}

// ---- controls --------------------------------------------------------------------------------------------------------------------------------------

bool Graph::set_volume(uint32_t node_id, float volume, bool mute) {
  if (!(volume >= 0.0f)) volume = 0.0f;
  if (volume > 1.5f) volume = 1.5f;
  bool sink = false, source = false;
  {
    kernel::ScopedMutex guard(mutex_);
    GraphObject* node = find_locked(node_id);
    if (!node || node->type != ObjectType::Node) return false;
    if ((node->role == NodeRole::Source || node->role == NodeRole::Capture) && volume > 1.0f) volume = 1.0f;      // (the hub only attenuates)
    node->volume = volume;
    node->mute = mute;
    touch_locked(*node);
    if (node->role == NodeRole::Sink) {
      sink = true;
      master_ = to_device_volume(volume);
      master_muted_ = mute;
    } else if (node->role == NodeRole::Source) {
      source = true;
      source_gain_ = to_device_volume(volume);
      source_muted_ = mute;
    } else if (node->record) {
      node->record->capture().set_gain(to_device_volume(volume));
      node->record->capture().set_muted(mute);
    } else if (node->native_capture) {
      node->native_capture->set_gain(to_device_volume(volume));
      node->native_capture->set_muted(mute);
    } else if (node->playback) {
      node->playback->stream().set_volume(to_device_volume(volume));
      node->playback->stream().set_muted(mute);
    } else if (node->native) {
      node->native->set_volume(to_device_volume(volume));
      node->native->set_muted(mute);
    }
  }
  if (sink && mixer_) {
    // (outside the graph's lock: the mixer takes its own)
    mixer_->set_master_volume(to_device_volume(volume));
    mixer_->set_master_muted(mute);
  }
  if (source && capture_) {
    capture_->set_master_gain(to_device_volume(volume));
    capture_->set_master_muted(mute);
  }
  return true;
}

bool Graph::get_volume(uint32_t node_id, float* volume, bool* mute) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* node = find_locked(node_id);
  if (!node || node->type != ObjectType::Node) return false;
  *volume = node->volume;
  *mute = node->mute;
  return true;
}

bool Graph::set_paused(uint32_t node_id, bool paused) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* node = find_locked(node_id);
  if (!node || node->type != ObjectType::Node || (!node->playback && !node->record)) return false;
  if (node->playback) node->playback->set_paused(paused);
  else node->record->set_paused(paused);
  const uint32_t state = paused ? kNodeStateIdle : kNodeStateRunning;
  if (node->state != state) {
    node->state = state;
    touch_locked(*node);
  }
  return true;
}

// ---- refresh ---------------------------------------------------------------------------------------------------------------------------------------

void Graph::sync_sink_locked(const char* device_name, uint8_t master, bool muted) {
  GraphObject* sink = find_locked(sink_);
  if (!sink) return;
  if (device_name && sink->props.get("node.description") != StringView(device_name)) {
    sink->props.set("node.description", device_name);
    sink->props.set("node.nick", device_name);
    touch_locked(*sink);
    if (GraphObject* device = find_locked(device_)) {
      device->props.set("device.description", device_name);
      touch_locked(*device);
    }
  }
  if (master != master_ || muted != master_muted_) {
    master_ = master;
    master_muted_ = muted;
    sink->volume = master / float(hardware::kAudioMaxVolume);
    sink->mute = muted;
    touch_locked(*sink);
  }
}

void Graph::sync_native_capture_locked(const List<SharedPointer<audio::CaptureStream>>& live) {
  auto is_live = [&](const audio::CaptureStream* s) {
    for (const SharedPointer<audio::CaptureStream>& l : live)
      if (l.get() == s) return true;
    return false;
  };
  // native record streams that ended
  List<uint32_t> ended(allocator_);
  for (const UniquePointer<GraphObject>& o : objects_.view())
    if (o->native_capture && (o->native_capture->closed() || !is_live(o->native_capture.get()))) ended.push_back(o->id);
  List<SharedPointer<PlaybackStream>> none(allocator_);
  for (uint32_t id : ended) remove_node_locked(id, &none, false);
  // and new ones (the hub's streams that are not the graph's own record streams)
  for (const SharedPointer<audio::CaptureStream>& l : live) {
    bool known = false;
    for (const UniquePointer<GraphObject>& o : objects_.view())
      if (o->native_capture.get() == l.get() || (o->record && &o->record->capture() == l.get())) {
        known = true;
        break;
      }
    if (known || l->server_owned()) continue;
    const audio::StreamFormat& sf = l->format();
    PcmFormat format{sf.type == audio::SampleType::S16 ? SampleFormat::S16Le : SampleFormat::U8, sf.sample_rate, sf.channels};
    Properties props(allocator_);
    const FixedString<48> label = l->label();
    props.set("application.name", label.empty() ? StringView("TheoryCraftOS") : label.view());
    props.set("media.name", label.empty() ? StringView("Recording") : label.view());
    props.set("client.api", "tcos-native");
    const uint32_t id = add_record_node_locked(native_client_, Origin::Native, format, props, false);
    if (GraphObject* node = id ? find_locked(id) : nullptr) {
      node->native_capture = l;
      node->volume = l->gain() / float(hardware::kAudioMaxVolume);
      node->mute = l->muted();
    }
  }
}

void Graph::sync_source_locked(StringView device_name, uint8_t gain, bool muted) {
  GraphObject* source = find_locked(source_);
  if (!source) return;
  const StringView description = device_name.empty() ? StringView("Audio input") : device_name;
  if (source->props.get("node.description") != description) {
    source->props.set("node.description", description);
    source->props.set("node.nick", description);
    touch_locked(*source);
    if (GraphObject* device = find_locked(source_device_)) {
      device->props.set("device.description", description);
      touch_locked(*device);
    }
  }
  if (gain != source_gain_ || muted != source_muted_) {
    source_gain_ = gain;
    source_muted_ = muted;
    source->volume = gain / float(hardware::kAudioMaxVolume);
    source->mute = muted;
    touch_locked(*source);
  }
  // the record streams' state; the source runs while one of them listens, and is suspended with no microphone
  bool listening = false;
  for (UniquePointer<GraphObject>& p : objects_.view()) {
    GraphObject& o = *p;
    if (o.type != ObjectType::Node || o.role != NodeRole::Capture || !o.record) continue;
    const uint32_t state = o.record->paused() ? kNodeStateIdle : kNodeStateRunning;
    if (state == kNodeStateRunning && !o.monitor) listening = true;
    if (state != o.state) {
      o.state = state;
      touch_locked(o);
    }
  }
  const uint32_t state = device_name.empty() ? kNodeStateSuspended : listening ? kNodeStateRunning : kNodeStateIdle;
  if (state != source->state) {
    source->state = state;
    touch_locked(*source);
  }
}

void Graph::refresh() {
  if (capture_ && source_) {
    const FixedString<64> name = capture_->source_name();
    const uint8_t gain = capture_->master_gain();
    const bool muted = capture_->master_muted();
    List<SharedPointer<audio::CaptureStream>> live(allocator_);
    const bool listed = capture_->snapshot_streams(&live);
    kernel::ScopedMutex guard(mutex_);
    sync_source_locked(name.view(), gain, muted);
    if (listed) sync_native_capture_locked(live);
  }
  if (!mixer_) return;
  List<SharedPointer<audio::AudioStream>> live(allocator_);
  if (!mixer_->snapshot_streams(&live)) return;
  const uint8_t master = mixer_->master_volume();
  const bool muted = mixer_->master_muted();
  SharedPointer<hardware::IAudioDevice> shared = mixer_->shared_device();
  // (a device the mixer holds by reference does not change)
  const char* device_name = shared ? shared->name() : mixer_->device().name();
  kernel::ScopedMutex guard(mutex_);
  sync_sink_locked(device_name, master, muted);
  auto is_live = [&](const audio::AudioStream* s) {
    for (const SharedPointer<audio::AudioStream>& l : live)
      if (l.get() == s) return true;
    return false;
  };
  // the graph's closed streams that the mixer is done with
  for (uint64_t i = retired_.count(); i > 0; --i)
    if (!is_live(retired_[i - 1].get())) retired_.remove_at(i - 1);
  // native streams that ended; the state of the others
  List<uint32_t> ended(allocator_);
  for (UniquePointer<GraphObject>& p : objects_.view()) {
    GraphObject& o = *p;
    if (o.type == ObjectType::Node && o.role == NodeRole::Capture && o.monitor && o.record && !capture_) {
      // (the monitor's record nodes, when there is no capture hub to look after them)
      const uint32_t state = o.record->paused() ? kNodeStateIdle : kNodeStateRunning;
      if (state != o.state) {
        o.state = state;
        touch_locked(o);
      }
    }
    if (o.type != ObjectType::Node || o.role != NodeRole::Playback) continue;
    if (o.native && !is_live(o.native.get())) {
      ended.push_back(o.id);
      continue;
    }
    audio::AudioStream* s = o.native ? o.native.get() : (o.playback ? &o.playback->stream() : nullptr);
    if (!s) continue;
    const uint32_t state = (s->paused() || s->drained()) ? kNodeStateIdle : kNodeStateRunning;
    if (state != o.state) {
      o.state = state;
      touch_locked(o);
    }
  }
  List<SharedPointer<PlaybackStream>> none(allocator_);
  for (uint32_t id : ended) remove_node_locked(id, &none, true);
  // native streams that are new
  for (const SharedPointer<audio::AudioStream>& l : live) {
    bool known = false;
    for (const SharedPointer<audio::AudioStream>& r : retired_)
      if (r.get() == l.get()) known = true;
    for (const UniquePointer<GraphObject>& o : objects_.view()) {
      if (known) break;
      if (o->native.get() == l.get() || (o->playback && &o->playback->stream() == l.get())) known = true;
    }
    if (known || l->finished()) continue;
    const audio::StreamFormat& sf = l->format();
    PcmFormat format{sf.type == audio::SampleType::S16 ? SampleFormat::S16Le : SampleFormat::U8, sf.sample_rate, sf.channels};
    Properties props(allocator_);
    const FixedString<48> label = l->label();
    props.set("application.name", label.empty() ? StringView("TheoryCraftOS") : label.view());
    props.set("media.name", label.empty() ? StringView("System sound") : label.view());
    props.set("client.api", "tcos-native");
    const uint32_t id = add_stream_node_locked(native_client_, Origin::Native, format, props);
    if (GraphObject* node = id ? find_locked(id) : nullptr) {
      node->native = l;
      node->state = (l->paused() || l->drained()) ? kNodeStateIdle : kNodeStateRunning;
      node->volume = l->volume() / float(hardware::kAudioMaxVolume);
      node->mute = l->muted();
    }
  }
}

}  // namespace pipewire
