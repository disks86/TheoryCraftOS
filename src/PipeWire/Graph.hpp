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
// MIDI and video are in it too: the MIDI bus (MidiBus.hpp) as the node tcos_midi.bridge with every program's MIDI node linked to it, and programs' video
// producers and watchers (each producer's frames in a VideoChannel its watchers share); and the virtual sinks the native protocol's adapter factory makes.
//
// Thread safe. The graph's lock is never held while the mixer's is taken (the mixer is called before or after it).
#pragma once
#include <stdint.h>
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/MidiBus.hpp"
#include "PipeWire/PlaybackStream.hpp"
#include "PipeWire/Properties.hpp"
#include "PipeWire/RecordStream.hpp"
#include "PipeWire/VideoChannel.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

enum class ObjectType : uint8_t { Core, Client, Device, Node, Port, Link, Metadata, Factory, Module };
// Which kind of program an object belongs to.
enum class Origin : uint8_t { Kernel, Native, Linux, Windows };
// (MidiBridge: the bus every MIDI port is linked to; MidiStream, VideoStream: a program's MIDI or video node; Virtual: a sink or source made with the
// adapter factory, support.null-audio-sink; Linked: a program's audio stream that plays into or records from a Virtual node instead of the devices)
enum class NodeRole : uint8_t { None, Sink, Playback, Source, Capture, MidiBridge, MidiStream, VideoStream, Virtual, Linked };

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
  float balance_left = 1.0f, balance_right = 1.0f;   // the sides' share of the volume (the louder one is 1): a playing stream's pan
  SharedPointer<PlaybackStream> playback;            // a stream a protocol opened
  SharedPointer<RecordStream> record;                // a record stream a protocol opened
  SharedPointer<audio::AudioStream> native;          // a stream a native program opened on the mixer
  SharedPointer<audio::CaptureStream> native_capture;      // a stream a native program opened on the capture hub
  uint32_t input_ports = 0, output_ports = 0;
  bool monitor = false;                              // a record node of the sink's monitor (what plays), not of the source
  SharedPointer<VideoChannel> video;                 // a video node: what its producer shows (a watcher's is its producer's)
  SharedPointer<MidiQueue> midi_in;                  // a MIDI node that listens: what it hears
  uint32_t direction_of_node = kDirectionOutput;     // a MIDI, video or linked node: which way it goes (output: it plays or shows)
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
  // A record stream of `client` in `format`, its node and ports linked from the source (or, with `monitor`, from the sink's monitor ports: what the
  // mixer plays, before the master volume). 0 when there is no capture hub (no mixer for a monitor), the format is not valid or the hub (the mixer's
  // taps) is full. *out is the stream the client reads from.
  uint32_t create_record(uint32_t client, const PcmFormat& format, const Properties& props, SharedPointer<RecordStream>* out, bool monitor = false);
  // A filter (a pw_filter with float ports both ways: an effect): one node that plays `out_format` to the sink (its output ports) and records
  // `in_format` from the source (its input ports linked from it); either may have 0 channels. Its media class is kept.
  uint32_t create_filter(uint32_t client, const PcmFormat& out_format, const PcmFormat& in_format, const Properties& props,
                         SharedPointer<PlaybackStream>* playback, SharedPointer<RecordStream>* record);
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
  // A playing stream's two sides (linear, 0 .. 1.5): the louder is its volume, the balance becomes its pan (audio::pan_for_gains). Other nodes have no
  // balance: the louder side is their volume.
  bool set_balance(uint32_t node, float left, float right, bool mute);
  bool get_balance(uint32_t node, float* left, float* right, bool* mute);
  bool set_paused(uint32_t node, bool paused);
  uint32_t sink_id() const { return sink_; }
  // 0 when there is no capture hub.
  uint32_t source_id() const { return source_; }
  uint32_t native_client_id() const { return native_client_; }
  uint32_t device_id() const { return device_; }
  uint32_t metadata_id() const { return metadata_; }
  uint32_t midi_bridge_id() const { return midi_bridge_; }

  // ---- MIDI: the bus (MidiBus.hpp) and the programs' MIDI nodes, each linked to the bridge (an output port to its input, its output to an input port).
  MidiBus& midi_bus() { return midi_; }
  // A MIDI node of `client`; with `direction` input its queue (*in) hears the bus. 0 when the graph is full.
  uint32_t create_midi_node(uint32_t client, const Properties& props, uint32_t direction, SharedPointer<MidiQueue>* in);

  // ---- video: a producer's node makes a channel (*channel) that its watchers share; a watcher's node is linked to the producer `target` names (its
  // node.name, object.serial or id), else to the first producer, and is given that producer's channel (null while there is no producer: link_video()
  // tries again).
  uint32_t create_video_node(uint32_t client, const Properties& props, uint32_t direction, SharedPointer<VideoChannel>* channel);
  bool link_video(uint32_t node, SharedPointer<VideoChannel>* channel);
  SharedPointer<VideoChannel> video_of(uint32_t node);

  // ---- the adapter factory's virtual sinks and sources (support.null-audio-sink): a node with `channels` input ports and as many monitor (output)
  // ports. A `linger`ing one stays when its client goes (it is the kernel's then).
  uint32_t create_virtual_node(uint32_t client, const Properties& props, uint32_t channels, uint32_t rate);
  void keep_node(uint32_t node);
  // A program's audio stream that plays into (`direction` output) or records from a Virtual node, linked to its ports.
  uint32_t create_linked_stream(uint32_t client, const PcmFormat& format, const Properties& props, uint32_t direction, uint32_t peer);
  // The node `target` names (target.object, node.target: a node.name, an object.serial or an id); 0 if none.
  uint32_t find_node(StringView target);

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
  uint32_t add_record_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props, bool monitor);
  void add_capture_ports_locked(GraphObject& node, uint32_t client, const PcmFormat& format, bool monitor);
  SharedPointer<RecordStream> open_record_stream(const PcmFormat& format, bool monitor);
  void name_stream_node_locked(GraphObject& node, uint32_t client, const PcmFormat& format);
  void sync_source_locked(StringView device_name, uint8_t gain, bool muted);
  void sync_native_capture_locked(const List<SharedPointer<audio::CaptureStream>>& live);
  uint32_t add_stream_node_locked(uint32_t client, Origin origin, const PcmFormat& format, const Properties& props);
  void set_default_metadata_locked();
  void sync_sink_locked(const char* device_name, uint8_t master, bool muted);
  bool add_midi_bridge_locked();
  GraphObject* add_media_node_locked(uint32_t client, NodeRole role, const Properties& props, const char* media_class, uint32_t direction);
  bool add_port_locked(uint32_t client, uint32_t node, uint32_t index, uint32_t direction, StringView name, StringView dsp, uint32_t* id);
  bool add_link_locked(uint32_t client, uint32_t out_node, uint32_t out_port, uint32_t in_node, uint32_t in_port);
  uint32_t port_of_locked(uint32_t node, uint32_t direction, uint32_t index);

  allocators::IAllocator& allocator_;
  audio::AudioMixer* mixer_;
  audio::AudioCaptureHub* capture_;
  kernel::Mutex mutex_;
  List<UniquePointer<GraphObject>> objects_;      // in id order
  List<SharedPointer<audio::AudioStream>> retired_;      // streams the graph closed that the mixer may still play out
  uint32_t next_id_ = 0;
  uint64_t next_object_serial_ = 1;
  uint64_t serial_ = 1;
  uint32_t native_client_ = 0, device_ = 0, sink_ = 0, metadata_ = 0, midi_bridge_ = 0;
  uint32_t midi_bridge_ports_[2] = {};      // its input (playback) and output (capture) ports
  MidiBus midi_;
  uint32_t sink_ports_[2] = {};
  uint32_t monitor_ports_[2] = {};      // the sink's monitor_FL / monitor_FR
  uint32_t source_device_ = 0, source_ = 0;
  uint32_t source_ports_[2] = {};
  uint8_t source_gain_ = 0;
  bool source_muted_ = false;
  uint8_t master_ = 0;
  bool master_muted_ = false;
};

}  // namespace pipewire
