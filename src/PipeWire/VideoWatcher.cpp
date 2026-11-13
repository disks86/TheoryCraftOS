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

#include "PipeWire/VideoWatcher.hpp"
#include "PipeWire/VideoConvert.hpp"

namespace pipewire {

namespace {
constexpr uint32_t kSpaBGRA = 12, kSpaBGRx = 8;      // (0xAARRGGBB words are B, G, R, A in memory)
}  // namespace

bool VideoWatcher::name_of(uint32_t producer, String* out) {
  bool found = false;
  graph_.visit(producer, [&](const GraphObject& o) {
    StringView name = o.props.get("node.description");
    if (name.empty()) name = o.props.get("node.name");
    if (name.empty()) name = o.props.get("application.name");
    out->assign(name.empty() ? StringView("Video") : name);
    found = true;
  });
  return found;
}

bool VideoWatcher::watch(uint32_t producer) {
  if (node_) graph_.destroy_node(node_);
  node_ = producer_ = 0;
  channel_ = SharedPointer<VideoChannel>();
  if (!producer) return true;
  // its object.serial names it for the link (a node.name may be shared by several programs' nodes)
  String target(allocator_);
  bool video = false;
  graph_.visit(producer, [&](const GraphObject& o) {
    video = o.type == ObjectType::Node && o.role == NodeRole::VideoStream && o.direction_of_node == kDirectionOutput;
    target.assign(o.props.get("object.serial"));
  });
  if (!video) return false;
  Properties props(allocator_);
  props.set("node.name", "tcos-video-viewer");
  props.set("media.class", "Stream/Input/Video");
  props.set("target.object", target.view());
  node_ = graph_.create_video_node(graph_.native_client_id(), props, kDirectionInput, &channel_);
  if (!node_) return false;
  if (!channel_ || graph_.video_of(producer).get() != channel_.get()) {      // (linked to another one: it is not there any more)
    graph_.destroy_node(node_);
    node_ = 0;
    channel_ = SharedPointer<VideoChannel>();
    return false;
  }
  producer_ = producer;
  return true;
}

bool VideoWatcher::latest(uint64_t* sequence, List<uint32_t>* pixels, uint32_t* width, uint32_t* height, StringView* why) {
  *why = StringView();
  if (!channel_) return false;
  if (channel_->closed()) {
    *why = "The source went";
    return false;
  }
  SharedPointer<VideoFrame> frame = channel_->latest();
  if (!frame || frame->sequence <= *sequence) return false;
  *sequence = frame->sequence;
  const VideoFormat& f = frame->format;
  if (f.subtype == kSubtypeMjpg) {      // (Motion JPEG: decoded, its size the picture's)
    if (!jpeg_) jpeg_ = UniquePointer<hardware::JpegDecoder>::make(allocator_, allocator_);
    if (!jpeg_ || !jpeg_->decode(frame->pixels.view())) {
      *why = "A Motion JPEG picture that cannot be decoded";
      return false;
    }
    const uint32_t w = jpeg_->width(), h = jpeg_->height();
    const uint64_t n = uint64_t{w} * h;
    if (!pixels->reserve(n)) return false;
    while (pixels->count() < n)
      if (!pixels->push_back(0)) return false;
    while (pixels->count() > n) pixels->remove_at(pixels->count() - 1);
    if (!jpeg_->output_bgra(pixels->view().data(), w)) return false;
    *width = w;
    *height = h;
    return true;
  }
  if (f.encoded()) {
    *why = "Encoded video (not shown)";
    return false;
  }
  ConstByteView bgra = frame->pixels.view();
  if (f.format != kSpaBGRA && f.format != kSpaBGRx) {
    VideoFormat to = f;
    to.format = kSpaBGRA;
    if (!convert_video(f, frame->pixels.view(), to, &converted_)) {
      *why = "A video format the viewer cannot show";
      return false;
    }
    bgra = converted_.view();
  }
  const uint64_t n = uint64_t{f.width} * f.height;
  if (bgra.size() < n * 4 || !pixels->reserve(n)) return false;
  pixels->clear();
  for (uint64_t i = 0; i < n; ++i) {
    const uint32_t a = f.format == kSpaBGRx ? 0xFFu : bgra[4 * i + 3];
    if (!pixels->push_back(a << 24 | uint32_t(bgra[4 * i + 2]) << 16 | uint32_t(bgra[4 * i + 1]) << 8 | bgra[4 * i])) return false;
  }
  *width = f.width;
  *height = f.height;
  return true;
}

}  // namespace pipewire
