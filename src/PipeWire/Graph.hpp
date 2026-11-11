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

// The kernel's PipeWire: one media graph for the whole system, laid over the desktop's sound server (audio::AudioMixer). Every program that makes sound
// is in it, whatever it is:
//   - native programs (the media player, web audio, the MIDI synth, ...) open audio::AudioStreams on the mixer as before; refresh() finds them and shows
//     each as a node of the "TheoryCraftOS" client;
//   - Linux programs reach it over AF_UNIX: PipeWire's native protocol at $XDG_RUNTIME_DIR/pipewire-0 (NativeServer.hpp) and PulseAudio's at
//     $XDG_RUNTIME_DIR/pulse/native (PulseServer.hpp, as pipewire-pulse does);
//   - Windows programs reach it through winmm.dll and the NtTcosAudio service (WindowsAudioService.hpp).
// The mixer adds all the streams up into the output device, so the sound of all three kinds of program plays at once, each stream with its own volume.
// Recording goes the other way: the capture hub (audio::AudioCaptureHub) shares the microphone, and every program that records gets a capture node
// (Stream/Input/Audio) linked from the graph's one source node (Audio/Source), each with its own RecordStream, volume and mute.
//
// The graph holds PipeWire's objects with their ids and properties: the core, the "default" metadata, one device and one sink node for the mixer's output,
// clients, and for every playback stream a node (Stream/Output/Audio), its output ports and the links to the sink's input ports; with a capture hub also
// a second device and the source node, and for every record stream a node, its input ports and the links from the source's output ports. The protocols show these
// to their clients (pw-dump, pactl list ...). Each change bumps a serial; the protocols compare snapshots to tell their clients what came, went or changed.
//
// Thread safe. The graph's lock is never held while the mixer's is taken (the mixer is called before or after it).
#pragma once
#include <stdint.h>
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/PlaybackStream.hpp"
#include "PipeWire/Properties.hpp"
#include "PipeWire/RecordStream.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

enum class ObjectType : uint8_t { Core, Client, Device, Node, Port, Link, Metadata, Factory, Module };
// Which kind of program an object belongs to.
enum class Origin : uint8_t { Kernel, Native, Linux, Windows };
enum class NodeRole : uint8_t { None, Sink, Playback, Source, Capture };

// PipeWire's interface names ("PipeWire:Interface:Node") and the versions the graph speaks.
const char* interface_name(ObjectType type);
uint32_t interface_version(ObjectType type);
bool interface_from_name(StringView name, ObjectType* out);

constexpr uint32_t kNodeStateIdle = 2, kNodeStateRunning = 3, kNodeStateSuspended = 1;
constexpr int32_t kLinkStateActive = 4;
constexpr uint32_t kDirectionInput = 0, kDirectionOutput = 1;

struct GraphObject {
  explicit GraphObject(allocators::IAllocator& a) : props(a) {}
  uint32_t id = 0;
  uint64_t object_serial = 0;      // PipeWire's object.serial: never reused
  ObjectType type = ObjectType::Core;
  uint64_t changed = 0;            // the graph serial of its last change
  Properties props;
  uint32_t client = 0;             // the client that owns it (0: the kernel)
  Origin origin = Origin::Kernel;
  // a node
  NodeRole role = NodeRole::None;
  PcmFormat format;
  uint32_t state = kNodeStateIdle;
  float volume = 1.0f;
  bool mute = false;
  SharedPointer<PlaybackStream> playback;            // a stream a protocol opened
  SharedPointer<RecordStream> record;                // a record stream a protocol opened
  SharedPointer<audio::AudioStream> native;          // a stream a native program opened on the mixer
  uint32_t input_ports = 0, output_ports = 0;
  // a port
  uint32_t node = 0, direction = 0, channel = 0;     // channel: an SPA position
  // a link
  uint32_t output_node = 0, output_port = 0, input_node = 0, input_port = 0;
  // a client
  uint64_t pid = 0;
};

// What a snapshot lists of one object.
struct GraphEntry {
  uint32_t id;
  ObjectType type;
  uint64_t changed;
  NodeRole role = NodeRole::None;
};

class Graph {
 public:
  static constexpr uint32_t kCoreId = 0;
  static constexpr uint32_t kMaxObjects = 4096;
  static constexpr uint32_t kStreamRingBytes = 64 * 1024;      // device bytes per stream (~340 ms)

  // `mixer` may be null (no audio device): streams cannot be made then, the rest works. `capture` (may be null: no recording, no source node) is the hub
  // record streams are opened on; it may have no microphone at a time (record streams then hear nothing until one is plugged in).
  Graph(allocators::IAllocator& allocator, audio::AudioMixer* mixer, audio::AudioCaptureHub* capture = nullptr);
  ~Graph();
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  // Makes the fixed objects (core, metadata, the native client, the device and the sink). False if out of memory.
  bool initialize();
  allocators::IAllocator& allocator() const { return allocator_; }
  audio::AudioMixer* mixer() const { return mixer_; }
  audio::AudioCaptureHub* capture_hub() const { return capture_; }
  // A microphone is there to record from now.
  bool has_input();

  // ---- clients
  // A new client (`protocol` as PipeWire names it: "protocol-native", "protocol-pulse", ...); 0 if the graph is full.
  uint32_t add_client(Origin origin, uint64_t pid, StringView protocol, StringView api);
  bool update_client(uint32_t client, const Properties& props, Properties::Mode mode);
  // The client and everything it owns go; its streams stop at once.
  void remove_client(uint32_t client);

  // ---- streams
  // A playback stream of `client` in `format`, its node and ports linked to the sink. 0 when there is no mixer, the format is not valid or the mixer is
  // full. *out is the stream the client writes to.
  uint32_t create_playback(uint32_t client, const PcmFormat& format, const Properties& props, SharedPointer<PlaybackStream>* out);
  // A record stream of `client` in `format`, its node and ports linked from the source. 0 when there is no capture hub, the format is not valid or the
  // hub is full. *out is the stream the client reads from.
  uint32_t create_record(uint32_t client, const PcmFormat& format, const Properties& props, SharedPointer<RecordStream>* out);
  // `drain`: what is queued still plays; else it stops now. (A record node just stops.)
  void destroy_node(uint32_t node, bool drain = false);
  bool update_node(uint32_t node, const Properties& props, Properties::Mode mode);
  // The stream of a node a protocol made (null for other nodes).
  SharedPointer<PlaybackStream> playback_of(uint32_t node);
  // The record stream of a node a protocol made (null for other nodes).
  SharedPointer<RecordStream> record_of(uint32_t node);

  // ---- controls: a stream's own volume (linear, 0 .. 1.5) and mute; on the sink they are the mixer's master volume and mute, on the source the capture
  // hub's master gain and mute. (Record streams and the source go no higher than 1.)
  bool set_volume(uint32_t node, float volume, bool mute);
  bool get_volume(uint32_t node, float* volume, bool* mute);
  bool set_paused(uint32_t node, bool paused);
  uint32_t sink_id() const { return sink_; }
  // 0 when there is no capture hub.
  uint32_t source_id() const { return source_; }
  uint32_t native_client_id() const { return native_client_; }
  uint32_t device_id() const { return device_; }
  uint32_t metadata_id() const { return metadata_; }

  // Finds the mixer's streams that native programs opened (and the ones that ended), and follows the output device and master volume. Cheap when nothing
  // changed; the protocol servers call it every round.
  void refresh();

  // ---- views
  uint64_t serial();
  bool snapshot(List<GraphEntry>* out);
  // Runs f(const GraphObject&) on object `id` with the graph locked; false if there is no such object. `f` must not call the graph.
  template <class F>
  bool visit(uint32_t id, F&& f) {
    kernel::ScopedMutex guard(mutex_);
    const GraphObject* o = find_locked(id);
    if (!o) return false;
    f(*o);
    return true;
  }
  // Runs f(const GraphObject&) on every object (in id order) with the graph locked.
  template <class F>
  void for_each(F&& f) {
    kernel::ScopedMutex guard(mutex_);
    for (const UniquePointer<GraphObject>& o : objects_.view()) f(*o);
  }
  // How many nodes play now (for statistics and tests).
  uint32_t playback_count();
  // How many record nodes there are.
  uint32_t record_count();

 private:
  GraphObject* find_locked(uint32_t id);
  GraphObject* add_locked(ObjectType type, uint32_t client);
  void touch_locked(GraphObject& o) { o.changed = ++serial_; }
  void remove_locked(uint32_t id);
  void remove_node_locked(uint32_t node, List<SharedPointer<PlaybackStream>>* stopped, bool drain);
  bool add_source_locked();
  uint32_t add_record_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props);
  void name_stream_node_locked(GraphObject& node, uint32_t client, const PcmFormat& format);
  void sync_source_locked(StringView device_name, uint8_t gain, bool muted);
  uint32_t add_stream_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props);
  void set_default_metadata_locked();
  void sync_sink_locked(const char* device_name, uint8_t master, bool muted);

  allocators::IAllocator& allocator_;
  audio::AudioMixer* mixer_;
  audio::AudioCaptureHub* capture_;
  kernel::Mutex mutex_;
  List<UniquePointer<GraphObject>> objects_;      // in id order
  List<SharedPointer<audio::AudioStream>> retired_;      // streams the graph closed that the mixer may still play out
  uint32_t next_id_ = 0;
  uint64_t next_object_serial_ = 1;
  uint64_t serial_ = 1;
  uint32_t native_client_ = 0, device_ = 0, sink_ = 0, metadata_ = 0;
  uint32_t sink_ports_[2] = {};
  uint32_t source_device_ = 0, source_ = 0;
  uint32_t source_ports_[2] = {};
  uint8_t source_gain_ = 0;
  bool source_muted_ = false;
  uint8_t master_ = 0;
  bool master_muted_ = false;
};

}  // namespace pipewire
