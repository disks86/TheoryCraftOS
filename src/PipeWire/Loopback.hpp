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

// Audio from one place of the graph to another, as PulseAudio's module-loopback does it: a record stream of the kernel's on a source (the microphone,
// or a sink's monitor: what plays into the speakers or a virtual sink) and a playback stream into a sink (the speakers or a virtual sink), with what
// was recorded moved across each time it is pumped, about `latency` of it kept queued in the playback. The modules built on it (PulseServer):
// module-loopback itself, module-remap-sink (a virtual sink looped into its master, its channels swapped if the map says so) and module-combine-sink
// (a virtual sink looped into each of its slaves), module-remap-source and module-virtual-source (a source looped into a virtual source). 16-bit
// stereo or mono at 48 kHz on both sides (the mixer's own). Not thread safe: pumped by its owner's thread.
// EchoCancelLoop below is module-echo-cancel's: four streams around an EchoCanceller.
#pragma once
#include <stdint.h>
#include "PipeWire/EchoCanceller.hpp"
#include "PipeWire/Graph.hpp"
#include "Utility/ByteBuffer.hpp"

namespace pipewire {

class Loopback {
 public:
  // Where the audio comes from: the microphone, or the monitor of a sink (0: the speakers', else a virtual sink's node).
  struct From {
    bool monitor = false;
    uint32_t sink = 0;
  };
  static constexpr uint32_t kDefaultLatencyMs = 200;      // (module-loopback's latency_msec)
  static constexpr uint32_t kMaxLatencyMs = 2000;

  Loopback(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), scratch_(allocator) {}
  ~Loopback() { close(); }
  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;

  // Its two streams, the kernel's (`client`), named by `name` (media.name of both nodes). `to`: the sink to play into (0: the speakers, else a
  // virtual sink's node). False when either cannot be made, or when it would play into what it records (a sink's monitor into that sink).
  bool open(uint32_t client, From from, uint32_t to, uint32_t channels, uint32_t latency_ms, bool swap, StringView name);
  void close();
  // Moves what was recorded into the playback, keeping about the latency queued there. Returns the bytes moved.
  uint64_t pump();
  uint32_t record_node() const { return record_node_; }
  uint32_t play_node() const { return play_node_; }
  uint64_t moved() const { return moved_; }

 private:
  Graph& graph_;
  ByteBuffer scratch_;
  SharedPointer<RecordStream> record_;
  SharedPointer<PlaybackStream> play_;
  uint32_t record_node_ = 0, play_node_ = 0;
  uint32_t channels_ = 2;
  uint64_t latency_bytes_ = 0;
  bool swap_ = false;
  uint64_t moved_ = 0;
};

// module-echo-cancel's plumbing: what plays into the echo-cancel sink goes on to its master sink (the speakers) and is the canceller's far end;
// what the master source (the microphone) hears goes through the canceller into the echo-cancel source. The far and near ends are paired sample for
// sample as they arrive (both run at the mixer's rate), the canceller finding the delay between them; while nothing plays into the sink (its monitor
// delivers nothing then) the far end is silence. With `passthrough` (aec_method=null) the microphone goes through unchanged.
class EchoCancelLoop {
 public:
  static constexpr uint32_t kLatencyMs = 50;            // (queued at most into the sink master and the echo-cancel source)
  static constexpr uint32_t kIdleMs = 100;              // microphone heard with no far end before the far end is taken as silent

  EchoCancelLoop(Graph& graph, allocators::IAllocator& allocator);
  ~EchoCancelLoop() { close(); }
  EchoCancelLoop(const EchoCancelLoop&) = delete;
  EchoCancelLoop& operator=(const EchoCancelLoop&) = delete;

  // `mic`: the source master; `master`: the sink master (0 the speakers); `sink` and `source`: the module's virtual sink and source; `channels`
  // of the source (1 or 2: the cancelled mono in each). `options`: what follows the canceller; `partitions`: its filter's length in blocks.
  [[nodiscard]] bool open(uint32_t client, Loopback::From mic, uint32_t master, uint32_t sink, uint32_t source, uint32_t channels, bool passthrough,
                          const EchoCanceller::Options& options = EchoCanceller::Options(), uint32_t partitions = EchoCanceller::kDefaultPartitions);
  void close();
  // Moves what arrived on; returns the microphone samples that went into the source.
  uint64_t pump();
  const EchoCanceller& canceller() const { return aec_; }
  uint64_t far_samples() const { return far_samples_; }
  uint64_t near_samples() const { return near_samples_; }

 private:
  Graph& graph_;
  allocators::IAllocator& allocator_;
  EchoCanceller aec_;
  ByteBuffer scratch_;
  SharedPointer<RecordStream> far_record_, mic_record_;
  SharedPointer<PlaybackStream> far_play_, out_play_;
  uint32_t far_record_node_ = 0, mic_record_node_ = 0, far_play_node_ = 0, out_play_node_ = 0;
  uint32_t channels_ = 1;
  bool passthrough_ = false;
  // mono samples waiting to be paired, oldest first (a full ring drops its oldest)
  struct Ring {
    explicit Ring(allocators::IAllocator& a) : data(a) {}
    List<float> data;
    uint64_t head = 0, count = 0;
    bool size(uint64_t capacity);
    void push(float v);
    float pop();
    void clear() { head = count = 0; }
  };
  Ring far_ring_, mic_ring_;
  uint64_t idle_samples_ = 0;            // microphone samples heard since the far end last delivered
  uint64_t far_samples_ = 0, near_samples_ = 0;
};

}  // namespace pipewire
