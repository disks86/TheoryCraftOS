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

#include "Gui/Controls/VideoPlayerControl.hpp"
#include "Gui/TimeText.hpp"

namespace gui {

namespace {
constexpr int32_t kPad = 4, kRow1 = 2, kRow2 = 30, kRowHeight = 24, kLabelWidth = 64;
}

VideoPlayerControl::VideoPlayerControl(allocators::IAllocator& allocator, video::VideoPlayer& player)
    : Control(allocator),
      player_(player),
      view_(allocator),
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
      loop_(allocator) {
  view_.set_fit_enlarges(true);
  rewind_.set_text("<<");
  play_.set_text("Play");
  pause_.set_text("Pause");
  stop_.set_text("Stop");
  forward_.set_text(">>");
  position_.set_text("00:00");
  length_.set_text("00:00");
  combined_.assign("00:00 / 00:00");
  volume_label_.set_text("Vol");
  loop_.set_text("Loop");
  seek_.set_range(0, 1000);
  seek_.set_step(10, 100);
  volume_.set_range(0, 100);
  volume_.set_step(5, 20);
  rewind_.on_click(Handler::immediate(&VideoPlayerControl::rewind_clicked, this));
  play_.on_click(Handler::immediate(&VideoPlayerControl::play_clicked, this));
  pause_.on_click(Handler::immediate(&VideoPlayerControl::pause_clicked, this));
  stop_.on_click(Handler::immediate(&VideoPlayerControl::stop_clicked, this));
  forward_.on_click(Handler::immediate(&VideoPlayerControl::forward_clicked, this));
  seek_.on_value_changed(Handler::immediate(&VideoPlayerControl::seek_changed, this));
  volume_.on_value_changed(Handler::immediate(&VideoPlayerControl::volume_changed, this));
  loop_.on_changed(Handler::immediate(&VideoPlayerControl::loop_changed, this));
  Control* parts[12] = {&view_, &rewind_, &play_, &pause_, &stop_, &forward_, &position_, &seek_, &length_, &loop_, &volume_label_, &volume_};
  for (Control* c : parts) add(c);
  refresh();
}

VideoPlayerControl::~VideoPlayerControl() {
  stop_service();
  release_view(allocator(), pixels_);
}

void VideoPlayerControl::play_clicked(void* self, Control&) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  c->player_.play();
  c->refresh();
}

void VideoPlayerControl::pause_clicked(void* self, Control&) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  c->player_.pause();
  c->refresh();
}

void VideoPlayerControl::rewind_clicked(void* self, Control&) { static_cast<VideoPlayerControl*>(self)->rewind(); }
void VideoPlayerControl::forward_clicked(void* self, Control&) { static_cast<VideoPlayerControl*>(self)->fast_forward(); }

void VideoPlayerControl::rewind() {
  const int64_t skip = int64_t{kSkipSeconds} * 1000000, position = player_.position_us();
  player_.seek(position > skip ? position - skip : 0);
  refresh();
}

void VideoPlayerControl::fast_forward() {
  player_.seek(player_.position_us() + int64_t{kSkipSeconds} * 1000000);   // (clamped to the length)
  refresh();
}

void VideoPlayerControl::stop_clicked(void* self, Control&) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  c->player_.stop();
  c->refresh();
}

void VideoPlayerControl::seek_changed(void* self, Control&) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  if (c->updating_) return;
  c->player_.seek(c->player_.duration_us() * c->seek_.value() / 1000);
  c->refresh();
}

void VideoPlayerControl::volume_changed(void* self, Control&) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  if (!c->updating_ && c->player_.audio()) c->player_.audio()->set_volume(static_cast<uint8_t>(c->volume_.value()));
}

void VideoPlayerControl::loop_changed(void* self, Control&) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  if (!c->updating_) c->player_.set_looping(c->loop_.checked());
}

// Copies the player's picture into ours and shows it. True if there is a picture.
bool VideoPlayerControl::update_picture() {
  const graphics::Surface* current = player_.picture();
  if (!current) {
    if (surface_.valid()) {  // the player was closed
      surface_ = graphics::Surface();
      view_.set_image(nullptr);
      shown_serial_ = 0;
    }
    return false;
  }
  const uint32_t w = player_.frame_width(), h = player_.frame_height();
  bool new_size = false;
  if (!surface_.valid() || surface_.width != w || surface_.height != h) {
    release_view(allocator(), pixels_);
    pixels_ = allocate_view<uint32_t>(allocator(), uint64_t{w} * h);
    if (pixels_.empty()) return false;
    surface_.pixels = pixels_;
    surface_.width = w;
    surface_.height = h;
    surface_.pitch = w;
    new_size = true;
  }
  uint32_t serial = 0;
  if (!player_.copy_picture(pixels_.data(), w, &serial)) return false;
  if (new_size) view_.set_image(&surface_);
  else if (serial != shown_serial_) view_.image_changed();
  shown_serial_ = serial;
  return true;
}

void VideoPlayerControl::refresh() {
  UiGuard lock(this);
  const video::VideoState state = player_.state();
  const bool loaded = state != video::VideoState::Empty;
  update_picture();
  const int64_t duration = player_.duration_us(), position = loaded ? player_.position_us() : 0;
  updating_ = true;
  play_.set_enabled(loaded && state != video::VideoState::Playing);
  pause_.set_enabled(state == video::VideoState::Playing);
  stop_.set_enabled(loaded);
  rewind_.set_enabled(loaded && duration > 0);
  forward_.set_enabled(loaded && duration > 0);
  seek_.set_enabled(loaded && duration > 0);
  if (!seek_.dragging()) seek_.set_value(duration > 0 ? static_cast<int32_t>(position * 1000 / duration) : 0);
  const bool has_audio = player_.has_audio();
  volume_.set_enabled(has_audio);
  if (has_audio && !volume_.dragging() && volume_.value() != player_.audio()->volume()) volume_.set_value(player_.audio()->volume());
  if (loop_.checked() != player_.looping()) loop_.set_checked(player_.looping());
  updating_ = false;

  char now[16], total[16];
  const uint32_t a = format_clock(static_cast<uint64_t>(position / 1000000), now);
  const uint32_t b = format_clock(static_cast<uint64_t>(duration / 1000000), total);
  if (!position_.text().equals(StringView(now, a))) position_.set_text(StringView(now, a));
  if (!length_.text().equals(StringView(total, b))) length_.set_text(StringView(total, b));
  combined_.assign(StringView(now, a));
  combined_.append(" / ");
  combined_.append(StringView(total, b));
}

void VideoPlayerControl::layout() {
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const int32_t bar_top = h > kBarHeight ? h - kBarHeight : 0;
  view_.set_bounds({0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(bar_top)});
  const int32_t row1 = bar_top + kRow1, row2 = bar_top + kRow2;
  const int32_t seek_w = w - 2 * kPad - 2 * kLabelWidth;
  position_.set_bounds({kPad, row1, kLabelWidth, kRowHeight});
  seek_.set_bounds({kPad + kLabelWidth, row1, static_cast<uint32_t>(seek_w > 0 ? seek_w : 0), kRowHeight});
  length_.set_bounds({w - kPad - kLabelWidth, row1, kLabelWidth, kRowHeight});
  int32_t x = kPad;
  Button* buttons[5] = {&rewind_, &play_, &pause_, &stop_, &forward_};
  const int32_t widths[5] = {36, 52, 56, 52, 36};
  for (int i = 0; i < 5; ++i) {
    buttons[i]->set_bounds({x, row2, static_cast<uint32_t>(widths[i]), kRowHeight});
    x += widths[i] + 4;
  }
  loop_.set_bounds({x + 8, row2, 70, kRowHeight});
  const int32_t vol_x = x + 84;
  volume_label_.set_bounds({vol_x, row2, 32, kRowHeight});
  volume_.set_bounds({vol_x + 32, row2, static_cast<uint32_t>(w - vol_x - 32 - kPad > 40 ? w - vol_x - 32 - kPad : 40), kRowHeight});
  Control::layout();
}

void VideoPlayerControl::paint(Painter& painter) {
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, painter.theme().face);
}

bool VideoPlayerControl::start_service() {
  if (service_) return true;
  stop_service_ = false;
  service_ = kernel::spawn(service_main, this, "video-ui");
  return service_ != nullptr;
}

void VideoPlayerControl::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_service_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void VideoPlayerControl::service_main(void* self) {
  VideoPlayerControl* c = static_cast<VideoPlayerControl*>(self);
  while (!__atomic_load_n(&c->stop_service_, __ATOMIC_ACQUIRE)) {
    c->player_.update();
    c->refresh();
    kernel::sleep_us(kRefreshIntervalUs);
  }
}

}  // namespace gui
