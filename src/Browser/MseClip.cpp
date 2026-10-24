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

#include "Browser/MseClip.hpp"
#include "Utility/Move.hpp"

namespace browser {

namespace {
constexpr int64_t kStartSlack = 100000;   // a playhead this close before the data counts as being at its start
constexpr int64_t kRefillAhead = 3000000; // swap in a longer snapshot when the playhead is less than this from the end of the one it plays
}  // namespace

MseClip::MseClip(allocators::IAllocator& allocator, media::MseHub& hub, uint32_t source, IObjectEnvironment* env) : allocator_(allocator), hub_(hub), source_(source), env_(env) {}

MseClip::~MseClip() = default;

MseClip::Child* MseClip::child_of(uint32_t buffer) {
  for (uint32_t i = 0; i < kMaxChildren; ++i)
    if (used_[i] && children_[i].buffer == buffer) return &children_[i];
  return nullptr;
}

void MseClip::drop(Child* c) {
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    if (&children_[i] != c) continue;
    children_[i].clip.reset();
    used_[i] = false;
  }
}

void MseClip::drop_all() {
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    children_[i].clip.reset();
    used_[i] = false;
  }
}

MseClip::Child* MseClip::master() {
  Child* first = nullptr;
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    if (!used_[i] || !children_[i].clip) continue;
    if (children_[i].video) return &children_[i];
    if (!first) first = &children_[i];
  }
  return first;
}

void MseClip::fail(uint8_t code) {
  if (!error_) error_ = code;
  playing_ = false;
  stalled_ = false;
  drop_all();
}

bool MseClip::play() {
  if (error_) return false;
  playing_ = true;
  stalled_ = false;
  return true;
}

void MseClip::pause() {
  playing_ = false;
  stalled_ = false;
  for (uint32_t i = 0; i < kMaxChildren; ++i)
    if (used_[i] && children_[i].clip && children_[i].clip->playing()) children_[i].clip->pause();
}

void MseClip::seek(int64_t microseconds) {
  if (microseconds < 0) microseconds = 0;
  const int64_t duration = duration_us();
  if (duration >= 0 && duration != INT64_MAX && microseconds > duration) microseconds = duration;
  position_ = microseconds;
  stalled_ = false;
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    if (!used_[i] || !children_[i].clip) continue;
    Child& c = children_[i];
    if (microseconds >= c.start - kStartSlack && microseconds < c.end) {
      const int64_t at = microseconds - c.base;
      c.clip->seek(at < 0 ? 0 : at);
    } else {
      drop(&c);   // (update() opens the file for the new place, if there is data there)
    }
  }
}

void MseClip::set_volume(double volume, bool muted) {
  volume_ = volume;
  muted_ = muted;
  for (uint32_t i = 0; i < kMaxChildren; ++i)
    if (used_[i] && children_[i].clip) children_[i].clip->set_volume(volume, muted);
}

bool MseClip::set_rate(double rate) {
  rate_ = rate;
  bool all = true, any = false;
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    if (!used_[i] || !children_[i].clip) continue;
    any = true;
    all = children_[i].clip->set_rate(rate) && all;
  }
  return any ? all : rate == 1;
}

int64_t MseClip::position_us() {
  Child* m = master();
  if (m && !stalled_) {
    int64_t p = m->base + m->clip->position_us();
    if (p > m->end) p = m->end;
    if (p < 0) p = 0;
    return p;
  }
  return position_;
}

int64_t MseClip::duration_us() {
  media::MseSource* s = hub_.find(source_);
  if (!s || !s->duration_set) return -1;
  if (!(s->duration < 9e12)) return INT64_MAX;
  return static_cast<int64_t>(s->duration * 1e6);
}

uint32_t MseClip::width() {
  Child* m = master();
  if (m && m->video) return m->clip->width();
  media::MseSource* s = hub_.find(source_);
  if (!s) return 0;
  for (uint64_t i = 0; i < s->buffers.count(); ++i)
    if (s->buffers[i]->has_video()) return s->buffers[i]->width();
  return 0;
}

uint32_t MseClip::height() {
  Child* m = master();
  if (m && m->video) return m->clip->height();
  media::MseSource* s = hub_.find(source_);
  if (!s) return 0;
  for (uint64_t i = 0; i < s->buffers.count(); ++i)
    if (s->buffers[i]->has_video()) return s->buffers[i]->height();
  return 0;
}

bool MseClip::copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* serial) {
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    if (!used_[i] || !children_[i].clip || !children_[i].video) continue;
    uint32_t inner = 0;
    if (!children_[i].clip->copy_picture(dest, pitch, &inner)) return false;
    *serial = inner ^ (picture_serial_ << 24);
    return true;
  }
  return false;
}

bool MseClip::open_child(media::MseBuffer& buffer, const media::MseSnapshotPlan& plan, Child* into) {
  if (!env_) {
    fail(4);
    return false;
  }
  auto bytes = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  media::MseSnapshotPlan p = plan;
  if (!bytes || !buffer.snapshot(&p, bytes.get())) {
    fail(3);
    return false;
  }
  UniquePointer<IVideoClip> clip = p.has_video ? env_->open_video(util::move(bytes)) : env_->open_audio(util::move(bytes));
  if (!clip) {   // (this machine cannot decode what the page appended)
    fail(3);
    hub_.end_of_stream(source_, 3);
    return false;
  }
  into->clip = util::move(clip);
  into->buffer = buffer.id();
  into->base = p.base_us;
  into->start = p.start_us;
  into->end = p.end_us;
  into->generation = p.generation;
  into->epoch = buffer.epoch();
  into->appended = buffer.appended();
  into->key_epoch = keys_ ? keys_->key_epoch() : 0;
  into->video = p.has_video;
  into->started = false;
  ++picture_serial_;
  into->clip->set_volume(volume_, muted_);
  into->clip->set_rate(rate_);
  const int64_t at = position_ - p.base_us;
  into->clip->seek(at < 0 ? 0 : at);
  return true;
}

void MseClip::sync_children() {
  Child* m = master();
  if (!m) return;
  for (uint32_t i = 0; i < kMaxChildren; ++i) {
    if (!used_[i] || !children_[i].clip || &children_[i] == m) continue;
    Child& c = children_[i];
    const int64_t at = c.base + c.clip->position_us();
    const int64_t diff = at - position_;
    if (diff > 250000 || diff < -250000) {
      const int64_t to = position_ - c.base;
      c.clip->seek(to < 0 ? 0 : to);
    }
  }
}

// Is there data at the playhead that only a key we do not have would open? (Looked at again only when something that could change the answer did.)
bool MseClip::blocked_on_key(media::MseSource* source) {
  uint64_t signature = reinterpret_cast<uintptr_t>(keys_) * 1000003u + (keys_ ? keys_->key_epoch() : 0) * 7919u + static_cast<uint64_t>(position_ / 100000);
  for (uint64_t b = 0; b < source->buffers.count(); ++b) signature = signature * 31u + source->buffers[b]->appended() + source->buffers[b]->epoch() * 17u;
  if (probed_ && signature == probe_signature_) return probe_result_;
  bool blocked = false;
  for (uint64_t b = 0; b < source->buffers.count() && !blocked; ++b) {
    media::MseBuffer& buffer = *source->buffers[b];
    if (!buffer.has_protected_frames()) continue;
    media::MseSnapshotPlan plan;
    uint8_t missing[16];
    blocked = buffer.plan_for(position_, &plan, keys_, missing) == media::MsePlan::NeedKey;
  }
  probed_ = true;
  probe_signature_ = signature;
  probe_result_ = blocked;
  return blocked;
}

void MseClip::update() {
  media::MseSource* s = hub_.find(source_);
  if (!s || s->state == media::MseSource::State::Closed) {
    drop_all();
    return;
  }
  if (s->error && !error_) fail(s->error);
  if (error_) return;
  for (uint32_t i = 0; i < kMaxChildren; ++i)
    if (used_[i] && children_[i].clip) children_[i].clip->update();
  // where the playhead is
  if (Child* m = master()) {
    if (!stalled_) {
      int64_t p = m->base + m->clip->position_us();
      if (p > m->end) p = m->end;
      if (p >= 0) position_ = p;
    }
  }
  // the files: one for every buffer that has data at the playhead
  for (uint32_t i = 0; i < kMaxChildren; ++i) {   // (buffers that were removed)
    if (!used_[i]) continue;
    bool found = false;
    for (uint64_t b = 0; b < s->buffers.count(); ++b)
      if (s->buffers[b]->id() == children_[i].buffer) found = true;
    if (!found) drop(&children_[i]);
  }
  bool all = s->buffers.count() > 0;
  const bool master_stopped = [&]() {
    Child* m = master();
    return m && m->started && !stalled_ && playing_ && !m->clip->playing();
  }();
  for (uint64_t b = 0; b < s->buffers.count(); ++b) {
    media::MseBuffer& buffer = *s->buffers[b];
    if (!buffer.init_received()) {
      all = false;
      continue;
    }
    Child* c = child_of(buffer.id());
    media::MseSnapshotPlan plan;
    bool have_plan = false, rebuild = false;
    if (c && c->clip) {
      if (position_ < c->start - kStartSlack || position_ > c->end) {
        rebuild = true;   // the playhead has left the file
      } else if (buffer.appended() != c->appended || buffer.epoch() != c->epoch || (keys_ && keys_->key_epoch() != c->key_epoch)) {
        uint8_t missing[16];
        have_plan = buffer.plan_for(position_, &plan, keys_, missing) == media::MsePlan::Ok;
        if (!have_plan || buffer.changed_since(c->epoch, position_, c->end)) rebuild = true;
        else if (plan.generation == c->generation && plan.end_us > c->end + kStartSlack && (c->end - position_ < kRefillAhead || master_stopped)) rebuild = true;
        else {
          c->appended = buffer.appended();
          c->epoch = buffer.epoch();
          c->key_epoch = keys_ ? keys_->key_epoch() : 0;
        }
      }
    } else {
      rebuild = true;
    }
    if (rebuild) {
      uint8_t missing[16];
      if (!have_plan) have_plan = buffer.plan_for(position_, &plan, keys_, missing) == media::MsePlan::Ok;
      if (!have_plan) {
        if (c) drop(c);
        all = false;
        continue;
      }
      if (buffer.has_protected_frames()) {   // (what the playhead is going to see is decrypted before the file is written)
        const media::MseDecrypt done = keys_ ? buffer.decrypt(*keys_, plan, missing) : media::MseDecrypt::NeedKey;
        if (done == media::MseDecrypt::Failed) {
          fail(3);
          hub_.end_of_stream(source_, 3);
          return;
        }
        if (done == media::MseDecrypt::NeedKey) {
          if (c) drop(c);
          all = false;
          continue;
        }
      }
      if (!c) {
        for (uint32_t i = 0; i < kMaxChildren && !c; ++i)
          if (!used_[i]) {
            c = &children_[i];
            used_[i] = true;
          }
      }
      if (!c) {
        all = false;
        continue;
      }
      if (!open_child(buffer, plan, c)) {
        drop(c);
        return;
      }
    }
    if (!c || !c->clip || position_ < c->start - kStartSlack || position_ > c->end) all = false;
  }
  // Is the clip blocked for want of a key? (No file for the playhead because the key is missing; or the end of the file it plays, and what comes next is under a key
  // that is not there.)
  need_key_ = false;
  if (!all || (playing_ && [&]() { int64_t limit = INT64_MAX; for (uint32_t i = 0; i < kMaxChildren; ++i) if (used_[i] && children_[i].clip && children_[i].end < limit) limit = children_[i].end; return limit != INT64_MAX && position_ >= limit - 30000; }()))
    need_key_ = blocked_on_key(s);
  // playing, or waiting for the data to go on
  if (playing_) {
    bool wait = !all;
    if (all) {
      int64_t limit = INT64_MAX;
      for (uint32_t i = 0; i < kMaxChildren; ++i)
        if (used_[i] && children_[i].clip && children_[i].end < limit) limit = children_[i].end;
      const bool at_end = position_ >= limit - 30000 || master_stopped;
      if (at_end) {
        const double highest = hub_.highest_end(source_);
        if (s->state == media::MseSource::State::Ended && (highest < 0 || static_cast<double>(position_) / 1e6 >= highest - 0.1)) {
          // the end of the stream: playing is over
          playing_ = false;
          stalled_ = true;   // (the position is held at the end)
          position_ = limit;
          for (uint32_t i = 0; i < kMaxChildren; ++i)
            if (used_[i] && children_[i].clip && children_[i].clip->playing()) children_[i].clip->pause();
          return;
        }
        wait = true;
        if (limit != INT64_MAX && position_ >= limit - 30000) position_ = limit;
      }
    }
    stalled_ = wait;
    for (uint32_t i = 0; i < kMaxChildren; ++i) {
      if (!used_[i] || !children_[i].clip) continue;
      if (stalled_) {
        if (children_[i].clip->playing()) children_[i].clip->pause();
        children_[i].started = false;
      } else if (!children_[i].clip->playing()) {
        children_[i].clip->play();
        children_[i].started = true;
      }
    }
    if (!stalled_) sync_children();
  }
}

uint8_t MseClip::ready_state() {
  media::MseSource* s = hub_.find(source_);
  if (!s || s->state == media::MseSource::State::Closed) return 0;
  bool any_init = false;
  for (uint64_t b = 0; b < s->buffers.count(); ++b) any_init = any_init || s->buffers[b]->init_received();
  if (!any_init) return 0;
  if (need_key_) return 1;   // (blocked: the data is there, but it cannot be shown)
  List<double> ranges(allocator_);
  hub_.buffered(source_, &ranges);
  const double t = static_cast<double>(position_us()) / 1e6;
  for (uint64_t i = 0; i + 1 < ranges.count(); i += 2) {
    if (t >= ranges[i] - 0.1 && t <= ranges[i + 1] + 0.001) {
      if (s->state == media::MseSource::State::Ended) return 4;
      const double ahead = ranges[i + 1] - t;
      return ahead < 0.04 ? 2 : ahead < 3.0 ? 3 : 4;
    }
  }
  return 1;
}

uint32_t MseClip::buffered(ArrayView<double> pairs) {
  List<double> ranges(allocator_);
  hub_.buffered(source_, &ranges);
  uint64_t n = ranges.count();
  if (n > pairs.size()) n = pairs.size() & ~uint64_t{1};
  for (uint64_t i = 0; i < n; ++i) pairs[i] = ranges[i];
  return static_cast<uint32_t>(n / 2);
}

uint32_t MseClip::seekable(ArrayView<double> pairs) {
  if (pairs.size() < 2) return 0;
  media::MseSource* s = hub_.find(source_);
  if (!s) return 0;
  if (s->live_range) {
    pairs[0] = s->live_start;
    pairs[1] = s->live_end;
    return 1;
  }
  if (s->duration_set && s->duration < 9e12) {
    pairs[0] = 0;
    pairs[1] = s->duration;
    return 1;
  }
  List<double> ranges(allocator_);
  hub_.buffered(source_, &ranges);
  if (ranges.count() < 2) return 0;
  pairs[0] = ranges[0];
  pairs[1] = ranges[ranges.count() - 1];
  return 1;
}

}  // namespace browser
