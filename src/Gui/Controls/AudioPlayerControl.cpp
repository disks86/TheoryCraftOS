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
constexpr int32_t kPad = 4, kRow1 = 4, kRow2 = 32, kRow3 = 60, kRowHeight = 24;
constexpr int32_t kMeterFall = 4;  // how far the meter drops per refresh

}  // namespace

AudioPlayerControl::AudioPlayerControl(allocators::IAllocator& allocator, audio::AudioPlayer& player)
    : Control(allocator),
      player_(player),
      play_(allocator),
      stop_(allocator),
      time_(allocator),
      volume_label_(allocator),
      seek_(allocator),
      volume_(allocator),
      meter_(allocator) {
  play_.set_text("Play");
  stop_.set_text("Stop");
  time_.set_text("00:00 / 00:00");
  volume_label_.set_text("Vol");
  seek_.set_range(0, 1000);
  seek_.set_step(10, 100);
  volume_.set_range(0, 100);
  volume_.set_step(5, 20);
  volume_.set_value(player.volume());
  meter_.set_peak_hold(false);
  play_.on_click(Handler::immediate(&AudioPlayerControl::play_clicked, this));
  stop_.on_click(Handler::immediate(&AudioPlayerControl::stop_clicked, this));
  seek_.on_value_changed(Handler::immediate(&AudioPlayerControl::seek_changed, this));
  volume_.on_value_changed(Handler::immediate(&AudioPlayerControl::volume_changed, this));
  Control* parts[7] = {&play_, &stop_, &time_, &seek_, &volume_label_, &volume_, &meter_};
  for (Control* c : parts) add(c);
  refresh();
}

void AudioPlayerControl::play_clicked(void* self, Control&) {
  AudioPlayerControl* c = static_cast<AudioPlayerControl*>(self);
  if (c->player_.playing()) c->player_.pause();
  else c->player_.play();
  c->refresh();
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
  const StringView play_text = state == audio::PlayerState::Playing ? "Pause" : "Play";
  if (!play_.text().equals(play_text)) play_.set_text(play_text);
  play_.set_enabled(loaded);
  stop_.set_enabled(loaded);
  seek_.set_enabled(loaded && duration > 0);
  if (!seek_.dragging()) seek_.set_value(duration > 0 ? static_cast<int32_t>(position * 1000 / duration) : 0);
  if (volume_.value() != player_.volume() && !volume_.dragging()) volume_.set_value(player_.volume());
  updating_ = false;

  char now[16], total[16], text[40];
  const uint32_t a = format_clock(rate ? position / rate : 0, now);
  const uint32_t b = format_clock(rate ? duration / rate : 0, total);
  uint32_t n = 0;
  for (uint32_t i = 0; i < a; ++i) text[n++] = now[i];
  text[n++] = ' ';
  text[n++] = '/';
  text[n++] = ' ';
  for (uint32_t i = 0; i < b; ++i) text[n++] = total[i];
  if (!time_.text().equals(StringView(text, n))) time_.set_text(StringView(text, n));

  // The meter jumps up with the sound and falls back at its own pace.
  const int32_t level = audio::peak_to_percent(player_.take_peak());
  meter_level_ = level >= meter_level_ ? level : (meter_level_ > kMeterFall ? meter_level_ - kMeterFall : 0);
  meter_.set_level(meter_level_);
}

void AudioPlayerControl::layout() {
  const int32_t w = static_cast<int32_t>(bounds_.width);
  const int32_t inner = w - 2 * kPad > 0 ? w - 2 * kPad : 0;
  play_.set_bounds({kPad, kRow1, 64, kRowHeight});
  stop_.set_bounds({kPad + 68, kRow1, 56, kRowHeight});
  const int32_t time_x = kPad + 132;
  time_.set_bounds({time_x, kRow1, static_cast<uint32_t>(w - time_x - kPad > 0 ? w - time_x - kPad : 0), kRowHeight});
  seek_.set_bounds({kPad, kRow2, static_cast<uint32_t>(inner), kRowHeight});
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
