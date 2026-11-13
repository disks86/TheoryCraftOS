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

// The screen in the kernel's PipeWire graph (Graph.hpp), for screen sharing and recording (what xdg-desktop-portal's ScreenCast hands programs on
// Linux: OBS, a video call's "share screen"): a Video/Source node of the kernel's ("tcos-screen") whose frames are what the desktop draws, BGRx at
// the screen's size, at most kFps a second and only while someone watches. The desktop's window gives it frames (gui::IFrameTap in main.cpp: the
// renderer reads its frame back between drawing and presenting). Frames come when the desktop draws (the caller makes the window draw once when a
// watcher comes); while the screen is still, the last one is sent again once a second (keep_alive), so a watcher always has a recent frame.
// A program that presents straight to the display (Direct3D's present, dxgk::IPresentTap in main.cpp: the 3D test's direct presents, a full-screen
// Direct3D program) gives frames too: while it presents, the screen shows its image and the desktop's frames are passed over; once it has stopped for
// kProgramHoldUs, desktop_returned() says so (the caller has the desktop drawn again).
// The render thread (frame_wanted, frame_read), the presenting thread and the kernel's loop (keep_alive) may call it at once.
#pragma once
#include <stdint.h>
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "PipeWire/Graph.hpp"

namespace pipewire {

// Who draws a frame: the desktop's window, or a program presenting to the display itself.
enum class ScreenOrigin : uint8_t { Desktop, Program };

class ScreenSource {
 public:
  static constexpr uint32_t kFps = 10;
  static constexpr uint64_t kProgramHoldUs = 250000;
  ScreenSource(Graph& graph, allocators::IAllocator& allocator) : graph_(graph), allocator_(allocator), frame_(allocator) {}
  ~ScreenSource();
  ScreenSource(const ScreenSource&) = delete;
  ScreenSource& operator=(const ScreenSource&) = delete;

  // Its node. False when the graph has no room.
  bool start();
  uint32_t node() const { return node_; }
  // Whether a program watches it now; true once per watcher coming (the caller then has a frame drawn).
  bool watched();
  bool watcher_came();
  // A frame of width x height is being drawn at `now_us`: a buffer to read it into when one is due (someone watches, and the last was 1/kFps ago), A desktop frame is never wanted while a program presents (it is not what the screen shows).
  ArrayView<uint32_t> frame_wanted(uint32_t width, uint32_t height, uint64_t now_us, ScreenOrigin origin = ScreenOrigin::Desktop);
  // True once when a program that presented to the display has stopped (kProgramHoldUs without a present): the desktop shows again.
  bool desktop_returned(uint64_t now_us);
  // The frame is in that buffer: published (the channel's format follows the screen's size).
  void frame_read(uint32_t width, uint32_t height, int64_t pts_ns);
  // While watched: the last frame again when none was sent for kKeepAliveUs. Returns whether it sent one.
  bool keep_alive(uint64_t now_us);
  static constexpr uint64_t kKeepAliveUs = 1000000;
  uint64_t published();

 private:
  Graph& graph_;
  allocators::IAllocator& allocator_;
  List<uint32_t> frame_;
  SharedPointer<VideoChannel> channel_;
  uint32_t node_ = 0;
  uint32_t width_ = 0, height_ = 0;
  uint64_t last_us_ = 0;
  bool had_watchers_ = false;
  uint64_t published_ = 0;
  kernel::Mutex mutex_;      // (the frame, its size and times: the render thread's and the loop's)
  bool filling_ = false;     // the render thread is reading a frame into frame_ (between frame_wanted and frame_read)
  uint64_t sent_us_ = 0;     // when a frame last went out
  uint64_t now_us_ = 0;      // (the time of the frame being filled)
  uint64_t program_us_ = 0;  // when a program last presented to the display
  bool program_showing_ = false;
};

}  // namespace pipewire
