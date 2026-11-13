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
// (a virtual sink looped into each of its slaves). 16-bit stereo or mono at 48 kHz on both sides (the mixer's own). Not thread safe: pumped by its
// owner's thread.
#pragma once
#include <stdint.h>
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

}  // namespace pipewire
