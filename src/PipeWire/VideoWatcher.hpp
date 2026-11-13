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

// The kernel watching the graph's video for itself (the desktop's Video Viewer): the producers there are, and one of them watched as a watcher
// node of the kernel's ("tcos-video-viewer", linked to it as a program's video input would be, so a camera starts streaming), its newest frame
// handed out as 0xAARRGGBB pixels (converted from the producer's format; encoded video decoded: Motion JPEG always, the other codecs when the graph has
// decoders, Graph::set_video_codecs).
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "Utility/String.hpp"

namespace pipewire {

class VideoWatcher {
 public:
  VideoWatcher(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), allocator_(allocator), converted_(allocator), decode_(allocator) {}
  ~VideoWatcher() { watch(0); }
  VideoWatcher(const VideoWatcher&) = delete;
  VideoWatcher& operator=(const VideoWatcher&) = delete;

  bool producers(List<uint32_t>* out) { return graph_.video_producers(out); }
  // A producer's name to show (its node.description, else its node.name).
  bool name_of(uint32_t producer, String* out);
  // Watches `producer` (0: nothing). False when it is not a video producer.
  bool watch(uint32_t producer);
  uint32_t watching() const { return producer_; }
  // Its newest frame if newer than *sequence (which becomes it): `pixels` holds width * height 0xAARRGGBB words. False otherwise, *why saying why
  // when it is not merely that nothing new came.
  bool latest(uint64_t* sequence, List<uint32_t>* pixels, uint32_t* width, uint32_t* height, StringView* why);

 private:
  Graph& graph_;
  allocators::IAllocator& allocator_;
  uint32_t producer_ = 0, node_ = 0;
  SharedPointer<VideoChannel> channel_;
  ByteBuffer converted_;
  VideoDecodeSession decode_;
};

}  // namespace pipewire
