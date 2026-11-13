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

ArrayView<uint32_t> ScreenSource::frame_wanted(uint32_t width, uint32_t height, uint64_t now_us) {
  if (!channel_ || width == 0 || height == 0 || width > VideoFormat::kMaxSide || height > VideoFormat::kMaxSide || !watched()) return ArrayView<uint32_t>();
  if (last_us_ && now_us - last_us_ < 1000000ull / kFps) return ArrayView<uint32_t>();
  const uint64_t pixels = uint64_t{width} * height;
  if (frame_.count() < pixels) {
    if (!frame_.reserve(pixels)) return ArrayView<uint32_t>();
    while (frame_.count() < pixels)
      if (!frame_.push_back(0)) return ArrayView<uint32_t>();
  }
  last_us_ = now_us;
  return frame_.view().first(pixels);
}

void ScreenSource::frame_read(uint32_t width, uint32_t height, int64_t pts_ns) {
  if (!channel_ || uint64_t{width} * height > frame_.count()) return;
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

}  // namespace pipewire
