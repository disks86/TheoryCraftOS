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

#include "PipeWire/CameraSource.hpp"
#include "Utility/String.hpp"

namespace pipewire {

namespace {
constexpr uint32_t kSpaI420 = 2;      // SPA_VIDEO_FORMAT_I420
}  // namespace

CameraSources::~CameraSources() {
  while (sources_.count() > 0) remove_at(sources_.count() - 1);
}

uint32_t CameraSources::node_of(uint32_t id) const {
  for (const UniquePointer<Source>& s : sources_.view())
    if (s->id == id) return s->node;
  return 0;
}

bool CameraSources::streaming(uint32_t id) const {
  for (const UniquePointer<Source>& s : sources_.view())
    if (s->id == id) return s->started;
  return false;
}

uint64_t CameraSources::published(uint32_t id) const {
  for (const UniquePointer<Source>& s : sources_.view())
    if (s->id == id) return s->published;
  return 0;
}

void CameraSources::remove_at(uint64_t index) {
  Source& s = *sources_[index];
  if (s.started && s.camera) s.camera->stop();
  if (s.node) graph_.destroy_node(s.node);      // (its watchers see the channel closed)
  sources_.remove_at(index);
}

void CameraSources::start(Source& s) {
  if (!s.camera->start(kWidth, kHeight, kFps)) return;
  const hardware::VideoFormat got = s.camera->frame_format();
  hardware::VideoFormat want = got;
  want.pixel_format = hardware::PixelFormat::I420;
  VideoFormat format;
  format.format = kSpaI420;
  format.width = got.width;
  format.height = got.height;
  format.rate_num = kFps;
  format.rate_den = 1;
  hardware::CameraMode mode;
  for (uint32_t i = 0; i < s.camera->mode_count(); ++i)      // (its rate: the mode that streams, as near as the camera says it)
    if (s.camera->mode(i, &mode) && mode.width == got.width && mode.height == got.height && mode.fps) {
      format.rate_num = mode.fps;
      break;
    }
  if (!got.valid() || !format.valid() || !s.frame.allocate(want)) {
    s.camera->stop();
    return;
  }
  s.channel->set_format(format);
  s.started = true;
}

void CameraSources::update(ArrayView<const Camera> cameras, uint64_t now_us) {
  // the ones that went (or are no longer present)
  for (uint64_t i = sources_.count(); i > 0; --i) {
    const Source& s = *sources_[i - 1];
    bool there = false;
    for (const Camera& c : cameras) there |= c.id == s.id && c.camera.get() == s.camera.get() && c.camera->present();
    if (!there) remove_at(i - 1);
  }
  // the new ones: a node each
  for (const Camera& c : cameras) {
    if (!c.camera || !c.camera->present() || node_of(c.id) || sources_.count() >= kMaxCameras) continue;
    UniquePointer<Source> s = UniquePointer<Source>::make(allocator_, allocator_);
    if (!s) return;
    s->id = c.id;
    s->camera = c.camera;
    Properties props(allocator_);
    String name(allocator_);
    name.append("tcos-camera-");
    name.append_uint(c.id);
    props.set("node.name", name.view());
    props.set("node.description", StringView(c.camera->name()));
    props.set("media.class", "Video/Source");
    props.set("media.role", "Camera");
    props.set("device.api", "uvc");
    props.set("node.driver", "true");
    s->node = graph_.create_video_node(graph_.native_client_id(), props, kDirectionOutput, &s->channel);
    if (!s->node || !s->channel) {
      if (s->node) graph_.destroy_node(s->node);
      continue;
    }
    if (!sources_.push_back(util::move(s))) return;
  }
  // started while watched, stopped once nobody has watched for a while; the newest picture to the channel
  for (UniquePointer<Source>& p : sources_.view()) {
    Source& s = *p;
    if (graph_.video_watchers(s.node) > 0) {
      s.watched_us = now_us;
      if (!s.started) start(s);
    } else if (s.started && now_us - s.watched_us > kIdleStopUs) {
      s.camera->stop();
      s.started = false;
    }
    if (!s.started || !s.camera->read_frame(s.frame)) continue;
    const hardware::VideoFormat& f = s.frame.format();
    VideoPlaneData planes[3];
    for (uint32_t k = 0; k < 3; ++k) planes[k] = VideoPlaneData{s.frame.plane(k), f.plane_stride(k)};
    if (s.channel->publish(ArrayView<const VideoPlaneData>(planes, 3), static_cast<int64_t>(now_us * 1000))) ++s.published;
  }
}

}  // namespace pipewire
