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

#include "PipeWire/ScreenSource.hpp"

namespace pipewire {

namespace {
constexpr uint32_t kSpaBGRx = 8;      // (0xAARRGGBB words are B, G, R, A in memory)
}  // namespace

ScreenSource::~ScreenSource() {
  if (node_) graph_.destroy_node(node_);
}

bool ScreenSource::start() {
  if (node_) return true;
  Properties props(allocator_);
  props.set("node.name", "tcos-screen");
  props.set("node.description", "Screen");
  props.set("media.class", "Video/Source");
  props.set("media.role", "Screen");
  props.set("node.driver", "true");
  node_ = graph_.create_video_node(graph_.native_client_id(), props, kDirectionOutput, &channel_);
  if (node_ && !channel_) {
    graph_.destroy_node(node_);
    node_ = 0;
  }
  return node_ != 0;
}

bool ScreenSource::watched() { return node_ && graph_.video_watchers(node_) > 0; }

bool ScreenSource::watcher_came() {
  const bool now = watched();
  const bool came = now && !had_watchers_;
  had_watchers_ = now;
  return came;
}

uint64_t ScreenSource::published() {
  kernel::ScopedMutex guard(mutex_);
  return published_;
}

ArrayView<uint32_t> ScreenSource::frame_wanted(uint32_t width, uint32_t height, uint64_t now_us, ScreenOrigin origin) {
  if (!channel_ || width == 0 || height == 0 || width > VideoFormat::kMaxSide || height > VideoFormat::kMaxSide) return ArrayView<uint32_t>();
  const bool seen = watched();
  kernel::ScopedMutex guard(mutex_);
  if (origin == ScreenOrigin::Program) {      // (the screen shows the program's image now, watched or not)
    program_us_ = now_us;
    program_showing_ = true;
  } else if (program_showing_ && now_us - program_us_ < kProgramHoldUs) {
    return ArrayView<uint32_t>();
  }
  if (!seen || (filling_ && now_us - now_us_ < kKeepAliveUs)) return ArrayView<uint32_t>();      // (a fill whose read never came is given up)
  if (last_us_ && now_us - last_us_ < 1000000ull / kFps) return ArrayView<uint32_t>();
  const uint64_t pixels = uint64_t{width} * height;
  if (frame_.count() < pixels) {
    if (!frame_.reserve(pixels)) return ArrayView<uint32_t>();
    while (frame_.count() < pixels)
      if (!frame_.push_back(0)) return ArrayView<uint32_t>();
  }
  last_us_ = now_us;
  now_us_ = now_us;
  filling_ = true;
  return frame_.view().first(pixels);
}

void ScreenSource::frame_read(uint32_t width, uint32_t height, int64_t pts_ns) {
  kernel::ScopedMutex guard(mutex_);
  filling_ = false;
  if (!channel_ || uint64_t{width} * height > frame_.count()) return;
  sent_us_ = now_us_;
  if (width != width_ || height != height_) {
    VideoFormat f;
    f.format = kSpaBGRx;
    f.width = width;
    f.height = height;
    f.rate_num = kFps;
    f.rate_den = 1;
    channel_->set_format(f);      // (watchers are negotiated again at the new size)
    width_ = width;
    height_ = height;
  }
  const ArrayView<const uint32_t> words = ArrayView<const uint32_t>(frame_.view()).first(uint64_t{width} * height);
  if (channel_->publish(words_as_bytes(words), width * 4, pts_ns)) ++published_;
}

bool ScreenSource::desktop_returned(uint64_t now_us) {
  kernel::ScopedMutex guard(mutex_);
  if (!program_showing_ || now_us - program_us_ < kProgramHoldUs) return false;
  program_showing_ = false;
  return true;
}

bool ScreenSource::keep_alive(uint64_t now_us) {
  if (!channel_ || !watched()) return false;
  kernel::ScopedMutex guard(mutex_);
  if ((filling_ && now_us - now_us_ < kKeepAliveUs) || !width_ || !height_ || now_us - sent_us_ < kKeepAliveUs) return false;
  sent_us_ = now_us;
  const ArrayView<const uint32_t> words = ArrayView<const uint32_t>(frame_.view()).first(uint64_t{width_} * height_);
  if (!channel_->publish(words_as_bytes(words), width_ * 4, static_cast<int64_t>(now_us * 1000))) return false;
  ++published_;
  return true;
}

}  // namespace pipewire
