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

#include "Desktop/VideoViewer.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;

namespace {
constexpr uint64_t kListEveryUs = 1000000;      // (the list of sources is looked at again once a second)
}  // namespace

VideoViewer::VideoViewer(allocators::IAllocator& allocator)
    : Frame(allocator), list_(allocator), status_(allocator), view_(allocator), ids_(allocator), pixels_(allocator), status_text_(allocator) {
  set_title("Video Viewer");
  set_bounds({200, 120, 520, 420});
  set_padding(Insets::all(6));
  set_minimize_enabled(true);
  list_.set_dock(DockStyle::Top);
  list_.set_size(0, 24);
  list_.on_selection_changed({&VideoViewer::on_pick, this});
  add(&list_);
  status_.set_dock(DockStyle::Bottom);
  status_.set_size(0, 20);
  add(&status_);
  view_.set_dock(DockStyle::Fill);
  view_.set_fit_enlarges(true);
  add(&view_);
  set_status("No video sources");
}

VideoViewer::~VideoViewer() { stop_watching(); }

void VideoViewer::set_sources(IVideoSources* sources) {
  UiGuard lock(this);
  stop_watching();
  sources_ = sources;
  listed_us_ = 0;
}

void VideoViewer::set_status(StringView text) {
  if (status_text_.view() == text) return;
  status_text_.assign(text);
  status_.set_text(text);
}

void VideoViewer::stop_watching() {
  if (sources_ && watching_) sources_->watch(0);
  watching_ = 0;
  sequence_ = 0;
}

// The list again: the sources there are now (the one watched stays picked; one that went is no longer watched).
void VideoViewer::refresh_list() {
  const uint32_t n = sources_->source_count();
  List<uint32_t> ids(allocator());
  List<String> names(allocator());
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t id = 0;
    String name(allocator());
    if (sources_->source(i, &id, &name) && ids.push_back(id)) names.push_back(util::move(name));
  }
  bool same = ids.count() == ids_.count();
  for (uint64_t i = 0; same && i < ids.count(); ++i) same = ids[i] == ids_[i];
  if (same) return;
  int32_t keep = -1;
  list_.clear_items();
  ids_.clear();
  for (uint64_t i = 0; i < ids.count(); ++i) {
    list_.add_item(names[i].view());
    ids_.push_back(ids[i]);
    if (ids[i] == watching_) keep = static_cast<int32_t>(i);
  }
  bool went = false;
  if (keep < 0 && watching_) {      // (the source went: said, and nothing else picked for the user)
    stop_watching();
    view_.set_image(nullptr);
    set_status("The source went");
    went = true;
  }
  if (keep < 0 && !ids_.count() && !went) set_status("No video sources");
  if (keep < 0 && ids_.count() && !went && !pictures_) list_.set_selected_index(0);      // (the first one, when nothing was shown yet)
  else if (keep >= 0) list_.set_selected_index(keep);
}

void VideoViewer::on_pick(void* self, Control&) {
  VideoViewer& v = *static_cast<VideoViewer*>(self);
  const int32_t index = v.list_.selected_index();
  if (!v.sources_ || index < 0 || static_cast<uint64_t>(index) >= v.ids_.count()) return;
  const uint32_t id = v.ids_[static_cast<uint64_t>(index)];
  if (id == v.watching_) return;
  v.stop_watching();
  if (v.sources_->watch(id)) {
    v.watching_ = id;
    v.set_status("Waiting for a picture...");
  } else {
    v.set_status("The source cannot be watched");
  }
}

void VideoViewer::update(uint64_t now_us) {
  UiGuard lock(this);
  if (!sources_) return;
  if (!listed_us_ || now_us - listed_us_ >= kListEveryUs) {
    listed_us_ = now_us;
    refresh_list();
  }
  if (!watching_) return;
  uint32_t w = 0, h = 0;
  StringView why;
  if (!sources_->latest(&sequence_, &pixels_, &w, &h, &why)) {
    if (!why.empty()) set_status(why);
    return;
  }
  picture_.pixels = pixels_.view();
  picture_.width = picture_.pitch = w;
  picture_.height = h;
  const bool resized = w != width_ || h != height_;
  width_ = w;
  height_ = h;
  ++pictures_;
  if (resized || view_.image() != &picture_) view_.set_image(&picture_);
  else view_.image_changed();
  String text(allocator());
  text.append_uint(w);
  text.append(" x ");
  text.append_uint(h);
  set_status(text.view());
}

}  // namespace desktop
