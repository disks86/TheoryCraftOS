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

// The Video Viewer: what a video source of the system shows (the cameras, the screen, a program's camera or screen share: every PipeWire video
// producer), picked from a list, the picture fitted to the window. It watches only the source it shows and only while it is open (so a camera
// streams only then); encoded video is decoded (Motion JPEG, and H.264, H.265, MPEG-2, VC-1, VP8 and VP9 through the machine's video codecs). The sources come from an IVideoSources the kernel gives the desktop (src/PipeWire over the graph).
#pragma once
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/ImageView.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Windows/Frame.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

// The system's video sources, for the viewer. Called from the desktop's thread.
class IVideoSources {
 public:
  // The sources there are now (0, 1, ...): how many, and each one's id (stays while the source does) and name.
  virtual uint32_t source_count() = 0;
  virtual bool source(uint32_t index, uint32_t* id, String* name) = 0;
  // Watches the source of this id (and no other); 0 stops watching. False when it is not there.
  virtual bool watch(uint32_t id) = 0;
  // The newest picture of the source watched, as 0xAARRGGBB pixels (`pixels` made width * height), if it is newer than *sequence (which it
  // then becomes). False when there is none newer, or it cannot be shown (*shown_why says why: "encoded video", "the source went").
  virtual bool latest(uint64_t* sequence, List<uint32_t>* pixels, uint32_t* width, uint32_t* height, StringView* shown_why) = 0;

 protected:
  ~IVideoSources() = default;
};

class VideoViewer : public gui::Frame {
 public:
  explicit VideoViewer(allocators::IAllocator& allocator);
  ~VideoViewer();

  void set_sources(IVideoSources* sources);
  // Each round of the desktop while it is open: the list follows the sources, the newest picture is shown.
  void update(uint64_t now_us);
  // It was closed: it watches nothing more.
  void stop_watching();
  // (for tests) what it shows
  uint32_t picture_width() const { return width_; }
  uint32_t picture_height() const { return height_; }
  uint64_t pictures() const { return pictures_; }
  StringView status() const { return status_text_.view(); }
  gui::DropDown& source_list() { return list_; }

 private:
  static void on_pick(void* self, gui::Control& sender);
  void refresh_list();
  void set_status(StringView text);

  IVideoSources* sources_ = nullptr;
  gui::DropDown list_;
  gui::Label status_;
  gui::ImageView view_;
  List<uint32_t> ids_;              // the list's sources, in its order
  uint32_t watching_ = 0;           // the id watched (0: none)
  uint64_t sequence_ = 0;
  List<uint32_t> pixels_;
  graphics::Surface picture_;
  uint32_t width_ = 0, height_ = 0;
  uint64_t pictures_ = 0;
  uint64_t listed_us_ = 0;
  String status_text_;
};

}  // namespace desktop
