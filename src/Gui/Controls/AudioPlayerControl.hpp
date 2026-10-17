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
#include "Audio/AudioPlayer.hpp"
#include "Gui/Control.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/LevelMeter.hpp"
#include "Gui/Controls/Slider.hpp"

namespace gui {

// A player's front panel for an audio::AudioPlayer: a Play/Pause button, a Stop button, the elapsed and total
// time, a seek slider, a volume slider and a level meter showing what is being played. The control only
// operates the player; it does not own it, and the sound it plays is whatever the player has open.
//
// The display follows the player when refresh() is called: from a thread of your own, or through
// start_service(), which also feeds the player (so a program using the control needs no other thread). The
// buttons and sliders act on the player at once.
class AudioPlayerControl : public Control {
 public:
  static constexpr int32_t kHeight = 88;
  static constexpr uint32_t kRefreshIntervalUs = 40000;

  AudioPlayerControl(allocators::IAllocator& allocator, audio::AudioPlayer& player);
  ~AudioPlayerControl() { stop_service(); }

  audio::AudioPlayer& player() { return player_; }
  // Brings the buttons, slider, time and meter up to date with the player. Thread safe.
  void refresh();
  bool start_service();
  void stop_service();
  bool service_running() const { return service_ != nullptr; }

  // The parts, for placement and tests.
  Button& play_button() { return play_; }
  Button& stop_button() { return stop_; }
  Slider& seek_slider() { return seek_; }
  Slider& volume_slider() { return volume_; }
  LevelMeter& meter() { return meter_; }
  StringView time_text() const { return time_.text(); }

  void layout() override;
  void paint(Painter& painter) override;

 protected:
  Size default_size() const override { return {320, kHeight}; }

 private:
  static void play_clicked(void* self, Control&);
  static void stop_clicked(void* self, Control&);
  static void seek_changed(void* self, Control&);
  static void volume_changed(void* self, Control&);
  static void service_main(void* self);

  audio::AudioPlayer& player_;
  Button play_, stop_;
  Label time_, volume_label_;
  Slider seek_, volume_;
  LevelMeter meter_;
  bool updating_ = false;  // refresh() is moving the sliders: their handlers must not act on the player
  int32_t meter_level_ = 0;
  kernel::Thread* service_ = nullptr;
  bool stop_service_ = false;
};

}  // namespace gui
