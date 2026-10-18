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

#include "Gui/Controls/AudioPlayerControl.hpp"
#include "Gui/TimeText.hpp"

namespace gui {

namespace {
constexpr int32_t kPad = 4, kRow1 = 4, kRow2 = 32, kRow3 = 60, kRowHeight = 24, kLabelWidth = 64;
constexpr int32_t kMeterFall = 4;  // how far the meter drops per refresh

}  // namespace

AudioPlayerControl::AudioPlayerControl(allocators::IAllocator& allocator, audio::AudioPlayer& player)
    : Control(allocator),
      player_(player),
      rewind_(allocator),
      play_(allocator),
      pause_(allocator),
      stop_(allocator),
      forward_(allocator),
      position_(allocator),
      length_(allocator),
      volume_label_(allocator),
      combined_(allocator),
      seek_(allocator),
      volume_(allocator),
      meter_(allocator) {
  rewind_.set_text("<<");
  play_.set_text("Play");
  pause_.set_text("Pause");
  stop_.set_text("Stop");
  forward_.set_text(">>");
  position_.set_text("00:00");
  length_.set_text("00:00");
  combined_.assign("00:00 / 00:00");
  volume_label_.set_text("Vol");
  seek_.set_range(0, 1000);
  seek_.set_step(10, 100);
  volume_.set_range(0, 100);
  volume_.set_step(5, 20);
  volume_.set_value(player.volume());
  meter_.set_peak_hold(false);
  rewind_.on_click(Handler::immediate(&AudioPlayerControl::rewind_clicked, this));
  play_.on_click(Handler::immediate(&AudioPlayerControl::play_clicked, this));
  pause_.on_click(Handler::immediate(&AudioPlayerControl::pause_clicked, this));
  stop_.on_click(Handler::immediate(&AudioPlayerControl::stop_clicked, this));
  forward_.on_click(Handler::immediate(&AudioPlayerControl::forward_clicked, this));
  seek_.on_value_changed(Handler::immediate(&AudioPlayerControl::seek_changed, this));
  volume_.on_value_changed(Handler::immediate(&AudioPlayerControl::volume_changed, this));
  Control* parts[11] = {&rewind_, &play_, &pause_, &stop_, &forward_, &position_, &seek_, &length_, &volume_label_, &volume_, &meter_};
  for (Control* c : parts) add(c);
  refresh();
}

void AudioPlayerControl::play_clicked(void* self, Control&) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  c->player_.play();
  c->refresh();
}

void AudioPlayerControl::pause_clicked(void* self, Control&) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  c->player_.pause();
  c->refresh();
}

void AudioPlayerControl::rewind_clicked(void* self, Control&) { static_cast<AudioPlayerControl*>(self)->rewind(); }
void AudioPlayerControl::forward_clicked(void* self, Control&) { static_cast<AudioPlayerControl*>(self)->fast_forward(); }

void AudioPlayerControl::rewind() {
  const uint64_t skip = uint64_t{kSkipSeconds} * player_.sample_rate(), position = player_.position();
  player_.seek(position > skip ? position - skip : 0);
  refresh();
}

void AudioPlayerControl::fast_forward() {
  player_.seek(player_.position() + uint64_t{kSkipSeconds} * player_.sample_rate());   // (clamped to the length)
  refresh();
}

void AudioPlayerControl::stop_clicked(void* self, Control&) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  c->player_.stop();
  c->refresh();
}

void AudioPlayerControl::seek_changed(void* self, Control&) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  if (c->updating_) return;
  c->player_.seek(c->player_.duration() * static_cast<uint64_t>(c->seek_.value()) / 1000);
  c->refresh();
}

void AudioPlayerControl::volume_changed(void* self, Control&) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  if (!c->updating_) c->player_.set_volume(static_cast<uint8_t>(c->volume_.value()));
}

void AudioPlayerControl::refresh() {
  UiGuard lock(this);
  const audio::PlayerState state = player_.state();
  const bool loaded = state != audio::PlayerState::Empty;
  const uint64_t duration = player_.duration(), position = loaded ? player_.position() : 0;
  const uint32_t rate = player_.sample_rate();
  updating_ = true;
  play_.set_enabled(loaded && state != audio::PlayerState::Playing);
  pause_.set_enabled(state == audio::PlayerState::Playing);
  stop_.set_enabled(loaded);
  rewind_.set_enabled(loaded && duration > 0);
  forward_.set_enabled(loaded && duration > 0);
  seek_.set_enabled(loaded && duration > 0);
  if (!seek_.dragging()) seek_.set_value(duration > 0 ? static_cast<int32_t>(position * 1000 / duration) : 0);
  if (volume_.value() != player_.volume() && !volume_.dragging()) volume_.set_value(player_.volume());
  updating_ = false;

  char now[16], total[16];
  const uint32_t a = format_clock(rate ? position / rate : 0, now);
  const uint32_t b = format_clock(rate ? duration / rate : 0, total);
  if (!position_.text().equals(StringView(now, a))) position_.set_text(StringView(now, a));
  if (!length_.text().equals(StringView(total, b))) length_.set_text(StringView(total, b));
  combined_.assign(StringView(now, a));
  combined_.append(" / ");
  combined_.append(StringView(total, b));

  // The meter jumps up with the sound and falls back at its own pace.
  const int32_t level = audio::peak_to_percent(player_.take_peak());
  meter_level_ = level >= meter_level_ ? level : (meter_level_ > kMeterFall ? meter_level_ - kMeterFall : 0);
  meter_.set_level(meter_level_);
}

void AudioPlayerControl::layout() {
  const int32_t w = static_cast<int32_t>(bounds_.width);
  const int32_t seek_w = w - 2 * kPad - 2 * kLabelWidth;
  position_.set_bounds({kPad, kRow1, kLabelWidth, kRowHeight});
  seek_.set_bounds({kPad + kLabelWidth, kRow1, static_cast<uint32_t>(seek_w > 0 ? seek_w : 0), kRowHeight});
  length_.set_bounds({w - kPad - kLabelWidth, kRow1, kLabelWidth, kRowHeight});
  int32_t x = kPad;
  Button* buttons[5] = {&rewind_, &play_, &pause_, &stop_, &forward_};
  const int32_t widths[5] = {36, 52, 56, 52, 36};
  for (int i = 0; i < 5; ++i) {
    buttons[i]->set_bounds({x, kRow2, static_cast<uint32_t>(widths[i]), kRowHeight});
    x += widths[i] + 4;
  }
  volume_label_.set_bounds({kPad, kRow3, 32, kRowHeight});
  volume_.set_bounds({kPad + 32, kRow3, 100, kRowHeight});
  const int32_t meter_x = kPad + 140;
  meter_.set_bounds({meter_x, kRow3 + 3, static_cast<uint32_t>(w - meter_x - kPad > 0 ? w - meter_x - kPad : 0), 18});
  Control::layout();
}

void AudioPlayerControl::paint(Painter& painter) {
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, painter.theme().face);
  painter.bevel(all, false);
}

bool AudioPlayerControl::start_service() {
  if (service_) return true;
  stop_service_ = false;
  service_ = kernel::spawn(service_main, this, "player-ui");
  return service_ != nullptr;
}

void AudioPlayerControl::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_service_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void AudioPlayerControl::service_main(void* self) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  while (!__atomic_load_n(&c->stop_service_, __ATOMIC_ACQUIRE)) {
    c->player_.update();
    c->refresh();
    kernel::sleep_us(kRefreshIntervalUs);
  }
}

}  // namespace gui
