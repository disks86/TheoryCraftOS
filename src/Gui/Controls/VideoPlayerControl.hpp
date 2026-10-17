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

#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/ImageView.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Slider.hpp"
#include "Utility/AllocatedView.hpp"
#include "Video/VideoPlayer.hpp"

namespace gui {

// A video player's window onto a video::VideoPlayer: the picture, scaled to fill the space (an ImageView that
// also enlarges, so clicks and keys of the viewer - zoom, pan - work too), and under it a Play/Pause and a Stop button,
// the elapsed and total time, a seek slider, a Loop check box and, when the file has sound, a volume slider. The
// control only operates the player; it does not own it.
//
// The display follows the player when refresh() is called: from a thread of your own, or through start_service(),
// which also runs the player's update() (so a program using the control needs no other thread). The buttons and
// sliders act on the player at once. The picture is copied from the player under its lock, so it is never torn.
class VideoPlayerControl : public Control {
 public:
  static constexpr int32_t kBarHeight = 58;
  static constexpr uint32_t kRefreshIntervalUs = 10000;

  VideoPlayerControl(allocators::IAllocator& allocator, video::VideoPlayer& player);
  ~VideoPlayerControl();

  video::VideoPlayer& player() { return player_; }
  // Brings the picture, buttons, slider and time up to date with the player. Thread safe.
  void refresh();
  bool start_service();
  void stop_service();
  bool service_running() const { return service_ != nullptr; }

  // The parts, for placement and tests.
  ImageView& view() { return view_; }
  Button& play_button() { return play_; }
  Button& stop_button() { return stop_; }
  Slider& seek_slider() { return seek_; }
  Slider& volume_slider() { return volume_; }
  CheckBox& loop_box() { return loop_; }
  StringView time_text() const { return time_.text(); }

  void layout() override;
  void paint(Painter& painter) override;

 protected:
  Size default_size() const override { return {400, 300}; }

 private:
  static void play_clicked(void* self, Control&);
  static void stop_clicked(void* self, Control&);
  static void seek_changed(void* self, Control&);
  static void volume_changed(void* self, Control&);
  static void loop_changed(void* self, Control&);
  static void service_main(void* self);
  bool update_picture();

  video::VideoPlayer& player_;
  ImageView view_;
  Button play_, stop_;
  Label time_, volume_label_;
  Slider seek_, volume_;
  CheckBox loop_;
  ArrayView<uint32_t> pixels_;  // the copy of the picture the view shows
  graphics::Surface surface_;
  uint32_t shown_serial_ = 0;
  bool updating_ = false;  // refresh() is moving the controls: their handlers must not act on the player
  kernel::Thread* service_ = nullptr;
  bool stop_service_ = false;
};

}  // namespace gui
