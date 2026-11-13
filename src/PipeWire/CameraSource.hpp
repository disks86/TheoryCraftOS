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

// The machine's cameras in the kernel's PipeWire graph (Graph.hpp), as PipeWire's libcamera and V4L2 monitors put them there: each camera is a
// Video/Source node of the kernel's ("tcos-camera-<id>", its description the camera's name) that programs watch with a pw_stream video input
// (target.object its node.name, or the first video source there is: OBS, a video call, pw-cat). A camera streams only while someone watches: it
// is started (640 x 480 at 30 frames a second, or the nearest mode it has) when its first watcher comes and stopped two seconds after the last one
// went, and its newest picture is published as I420 on the node's video channel.
#pragma once
#include <stdint.h>
#include "Hardware/Camera/ICamera.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace pipewire {

class CameraSources {
 public:
  static constexpr uint32_t kMaxCameras = 8;
  static constexpr uint32_t kWidth = 640, kHeight = 480, kFps = 30;
  static constexpr uint64_t kIdleStopUs = 2000000;      // (a camera nobody watches stops after this)

  // A camera as the caller's list has it: an id that stays while the camera does, and the camera.
  struct Camera {
    uint32_t id = 0;
    SharedPointer<hardware::ICamera> camera;
  };

  CameraSources(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), allocator_(allocator), sources_(allocator) {}
  ~CameraSources();
  CameraSources(const CameraSources&) = delete;
  CameraSources& operator=(const CameraSources&) = delete;

  // One round, every few tens of milliseconds from one thread: the cameras there are now (a node for each new one, the nodes of the ones that went
  // or are no longer present removed), cameras started and stopped as watchers come and go, and each streaming camera's newest picture published.
  void update(ArrayView<const Camera> cameras, uint64_t now_us);
  uint32_t count() const { return static_cast<uint32_t>(sources_.count()); }
  // The node of the camera with this id (0: none).
  uint32_t node_of(uint32_t id) const;
  bool streaming(uint32_t id) const;
  uint64_t published(uint32_t id) const;

 private:
  struct Source {
    explicit Source(allocators::IAllocator& a) : frame(a) {}
    uint32_t id = 0, node = 0;
    SharedPointer<hardware::ICamera> camera;
    SharedPointer<VideoChannel> channel;
    hardware::VideoFrameBuffer frame;
    bool started = false;
    uint64_t watched_us = 0;      // when it last had a watcher
    uint64_t published = 0;
  };
  void remove_at(uint64_t index);
  void start(Source& s);

  Graph& graph_;
  allocators::IAllocator& allocator_;
  List<UniquePointer<Source>> sources_;
};

}  // namespace pipewire
