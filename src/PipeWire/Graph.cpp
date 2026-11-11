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

Graph::Graph(allocators::IAllocator& allocator, audio::AudioMixer* mixer)
    : allocator_(allocator), mixer_(mixer), objects_(allocator), retired_(allocator) {}

Graph::~Graph() {
  // the streams that are still playing stop with the graph
  for (UniquePointer<GraphObject>& o : objects_.view())
    if (o->playback) {
      o->playback->flush();
      o->playback->close();
    }
}

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
    port->node = sink_;
    port->direction = kDirectionInput;
    port->channel = kPositions[i];
    sink_ports_[i] = port->id;
    String name(allocator_);
    name.append("playback_");
    name.append(channel_name(kPositions[i]));
    port->props.set("port.name", name.view());
    port->props.set("port.direction", "in");
    port->props.set_uint("port.id", i);
    port->props.set_uint("node.id", sink_);
    port->props.set("audio.channel", channel_name(kPositions[i]));
    port->props.set("format.dsp", "32 bit float mono audio");
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

void Graph::set_default_metadata_locked() {
  if (GraphObject* m = find_locked(metadata_)) {
    m->props.set("default.audio.sink", "{\"name\":\"tcos_output.stereo\"}");
    m->props.set("default.configured.audio.sink", "{\"name\":\"tcos_output.stereo\"}");
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
    if (!out->push_back(GraphEntry{o->id, o->type, o->changed})) return false;
  return true;
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

uint32_t Graph::add_stream_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props) {
  GraphObject* node = add_locked(ObjectType::Node, client);
  if (!node) return 0;
  node->role = NodeRole::Playback;
  node->origin = origin;
  node->format = format;
  node->state = kNodeStateIdle;      // (refresh() tells it running once something is queued)
  node->props.update(props, Properties::Mode::kReplace);
  // what the graph owns
  node->props.set_uint("object.id", node->id);
  node->props.set_uint("object.serial", node->object_serial);
  node->props.set_uint("client.id", client);
  node->props.set("media.class", "Stream/Output/Audio");
  if (!node->props.has("node.name") || node->props.get("node.name").empty()) {
    String name(allocator_);
    name.append(node->props.has("application.name") ? node->props.get("application.name") : StringView("stream"));
    name.push_back('.');
    name.append_uint(node->id);
    node->props.set("node.name", name.view());
  }
  if (!node->props.has("media.name")) node->props.set("media.name", node->props.get("node.name"));
  if (!node->props.has("media.type")) node->props.set("media.type", "Audio");
  node->props.set_uint("audio.channels", format.channels);
  node->props.set_uint("audio.rate", format.rate);
  node->props.set("audio.format", format.name());
  node->props.set("target.object", kSinkName);
  node->props.set("node.autoconnect", "true");
  if (GraphObject* c = find_locked(client)) {
    if (!node->props.has("application.name") && c->props.has("application.name")) node->props.set("application.name", c->props.get("application.name"));
    node->props.set_uint("application.process.id", c->pid);
  }
  const uint32_t node_id = node->id;
  // ports: mono, or front left and right (more channels are folded into two)
  const uint32_t ports = format.channels == 1 ? 1 : 2;
  node->output_ports = ports;
  for (uint32_t i = 0; i < ports; ++i) {
    const uint32_t position = ports == 1 ? kMono : (i == 0 ? kFL : kFR);
    GraphObject* port = add_locked(ObjectType::Port, client);
    if (!port) return node_id;
    port->node = node_id;
    port->direction = kDirectionOutput;
    port->channel = position;
    String name(allocator_);
    name.append("output_");
    name.append(channel_name(position));
    port->props.set("port.name", name.view());
    port->props.set("port.direction", "out");
    port->props.set_uint("port.id", i);
    port->props.set_uint("node.id", node_id);
    port->props.set("audio.channel", channel_name(position));
    port->props.set("format.dsp", "32 bit float mono audio");
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

void Graph::remove_node_locked(uint32_t node_id, List<SharedPointer<PlaybackStream>>* stopped, bool drain) {
  GraphObject* node = find_locked(node_id);
  if (!node || node->type != ObjectType::Node || node->role != NodeRole::Playback) return;
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

SharedPointer<PlaybackStream> Graph::playback_of(uint32_t node_id) {
  kernel::ScopedMutex guard(mutex_);
  GraphObject* node = find_locked(node_id);
  return node && node->type == ObjectType::Node ? node->playback : SharedPointer<PlaybackStream>();
}

// ---- controls --------------------------------------------------------------------------------------------------------------------------------------

bool Graph::set_volume(uint32_t node_id, float volume, bool mute) {
  if (!(volume >= 0.0f)) volume = 0.0f;
  if (volume > 1.5f) volume = 1.5f;
  bool sink = false;
  {
    kernel::ScopedMutex guard(mutex_);
    GraphObject* node = find_locked(node_id);
    if (!node || node->type != ObjectType::Node) return false;
    node->volume = volume;
    node->mute = mute;
    touch_locked(*node);
    if (node->role == NodeRole::Sink) {
      sink = true;
      master_ = to_device_volume(volume);
      master_muted_ = mute;
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
  if (!node || node->type != ObjectType::Node || !node->playback) return false;
  node->playback->set_paused(paused);
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

void Graph::refresh() {
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
