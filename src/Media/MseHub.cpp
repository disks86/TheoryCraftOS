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

// The MediaSources of a page (see Mse.hpp).
#include "Media/Mse.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {
constexpr double kInfinity = 1e300;   // (a duration this long is +infinity: nothing can be buffered past it)
}

uint32_t MseHub::create_source() {
  if (sources_.count() >= kMaxSources) {
    // a source that is closed and was never attached can go
    for (uint64_t i = 0; i < sources_.count(); ++i) {
      if (sources_[i]->state == MseSource::State::Closed && sources_[i]->element == 0 && sources_[i]->buffers.count() == 0) {
        drop_source(sources_[i]->id);
        break;
      }
    }
    if (sources_.count() >= kMaxSources) return 0;
  }
  auto source = UniquePointer<MseSource>::make(allocator_, allocator_, next_source_);
  if (!source || !sources_.push_back(util::move(source))) return 0;
  return next_source_++;
}

MseSource* MseHub::find(uint32_t id) {
  for (uint64_t i = 0; i < sources_.count(); ++i)
    if (sources_[i]->id == id) return sources_[i].get();
  return nullptr;
}

void MseHub::drop_source(uint32_t id) {
  for (uint64_t i = 0; i < sources_.count(); ++i) {
    if (sources_[i]->id != id) continue;
    if (sources_[i]->element != 0) return;
    for (uint64_t u = urls_.count(); u > 0; --u)
      if (urls_[u - 1]->source == id) urls_.remove_at(u - 1);
    sources_.remove_at(i);
    return;
  }
}

MseHub::AddResult MseHub::add_buffer(uint32_t source, StringView type, uint32_t* buffer) {
  MseSource* s = find(source);
  if (!s || s->state != MseSource::State::Open) return AddResult::InvalidState;
  MseFormat format;
  if (!mse_type_supported(type, &format, nullptr)) return AddResult::NotSupported;
  if (s->buffers.count() >= kMaxBuffersPerSource) return AddResult::Quota;
  auto b = UniquePointer<MseBuffer>::make(allocator_, allocator_, format, next_buffer_);
  if (!b || !s->buffers.push_back(util::move(b))) return AddResult::Memory;
  *buffer = next_buffer_++;
  return AddResult::Ok;
}

bool MseHub::remove_buffer(uint32_t source, uint32_t buffer) {
  MseSource* s = find(source);
  if (!s) return false;
  for (uint64_t i = 0; i < s->buffers.count(); ++i) {
    if (s->buffers[i]->id() != buffer) continue;
    s->buffers.remove_at(i);
    return true;
  }
  return false;
}

MseBuffer* MseHub::find_buffer(uint32_t buffer, MseSource** source) {
  for (uint64_t i = 0; i < sources_.count(); ++i) {
    MseSource& s = *sources_[i];
    for (uint64_t b = 0; b < s.buffers.count(); ++b) {
      if (s.buffers[b]->id() == buffer) {
        if (source) *source = &s;
        return s.buffers[b].get();
      }
    }
  }
  return nullptr;
}

bool MseHub::register_url(StringView url, uint32_t source) {
  auto u = UniquePointer<Url>::make(allocator_, allocator_);
  if (!u) return false;
  u->address.assign(url);
  u->source = source;
  return urls_.push_back(util::move(u));
}

void MseHub::revoke_url(StringView url) {
  for (uint64_t i = 0; i < urls_.count(); ++i) {
    if (urls_[i]->address.view().equals(url)) {
      urls_.remove_at(i);
      return;
    }
  }
}

uint32_t MseHub::source_for_url(StringView url) const {
  for (uint64_t i = 0; i < urls_.count(); ++i)
    if (urls_[i]->address.view().equals(url)) return urls_[i]->source;
  return 0;
}

bool MseHub::attach(uint32_t source, uint32_t element) {
  MseSource* s = find(source);
  if (!s) return false;
  if (s->element == element && s->state != MseSource::State::Closed) return true;
  if (s->state != MseSource::State::Closed) return false;
  s->state = MseSource::State::Open;
  s->element = element;
  s->error = 0;
  queue(source, "sourceopen");
  return true;
}

void MseHub::detach(uint32_t source) {
  MseSource* s = find(source);
  if (!s || s->state == MseSource::State::Closed) return;
  s->buffers.clear();
  s->state = MseSource::State::Closed;
  s->element = 0;
  s->duration_set = false;
  s->live_range = false;
  queue(source, "sourceclose");
}

double MseHub::highest_end(uint32_t source) {
  MseSource* s = find(source);
  if (!s) return -1;
  int64_t best = -1;
  for (uint64_t i = 0; i < s->buffers.count(); ++i) {
    const int64_t e = s->buffers[i]->highest_end();
    if (e > best) best = e;
  }
  return best < 0 ? -1 : static_cast<double>(best) / 1e6;
}

bool MseHub::set_duration(uint32_t source, double seconds) {
  MseSource* s = find(source);
  if (!s || s->state == MseSource::State::Closed) return false;
  if (!(seconds >= 0)) return false;
  const double highest = highest_end(source);
  if (highest >= 0 && seconds < highest) return false;
  s->duration = seconds;
  s->duration_set = true;
  return true;
}

void MseHub::end_of_stream(uint32_t source, uint8_t error) {
  MseSource* s = find(source);
  if (!s || s->state != MseSource::State::Open) return;
  s->state = MseSource::State::Ended;
  s->error = error;
  if (error == 0) {
    const double highest = highest_end(source);
    if (highest >= 0) {
      s->duration = highest;
      s->duration_set = true;
    }
  }
}

void MseHub::reopen(uint32_t source) {
  MseSource* s = find(source);
  if (s && s->state == MseSource::State::Ended) {
    s->state = MseSource::State::Open;
    s->error = 0;
  }
}

void MseHub::buffered(uint32_t source, List<double>* out) {
  out->clear();
  MseSource* s = find(source);
  if (!s || s->buffers.count() == 0) return;
  const int64_t reach = s->state == MseSource::State::Ended ? static_cast<int64_t>(highest_end(source) * 1e6 + 0.5) : -1;
  List<MseBuffer::Range> acc(allocator_), mine(allocator_), merged(allocator_);
  for (uint64_t b = 0; b < s->buffers.count(); ++b) {
    s->buffers[b]->buffered(&mine, reach);
    if (b == 0) {
      for (const MseBuffer::Range& r : mine.view()) acc.push_back(r);
      continue;
    }
    merged.clear();
    uint64_t x = 0, y = 0;
    while (x < acc.count() && y < mine.count()) {
      const int64_t lo = acc[x].start > mine[y].start ? acc[x].start : mine[y].start;
      const int64_t hi = acc[x].end < mine[y].end ? acc[x].end : mine[y].end;
      if (lo < hi) merged.push_back(MseBuffer::Range{lo, hi});
      if (acc[x].end < mine[y].end) ++x;
      else ++y;
    }
    acc.clear();
    for (const MseBuffer::Range& r : merged.view()) acc.push_back(r);
  }
  for (const MseBuffer::Range& r : acc.view()) {
    out->push_back(static_cast<double>(r.start) / 1e6);
    out->push_back(static_cast<double>(r.end) / 1e6);
  }
}

uint64_t MseHub::total_bytes() {
  uint64_t total = 0;
  for (uint64_t i = 0; i < sources_.count(); ++i)
    for (uint64_t b = 0; b < sources_[i]->buffers.count(); ++b) total += sources_[i]->buffers[b]->bytes();
  return total;
}

void MseHub::appended(MseSource* source) {
  if (!source) return;
  if (!source->duration_set) {
    // (the first init segment that knows how long the stream is says so)
    for (uint64_t b = 0; b < source->buffers.count(); ++b) {
      const int64_t d = source->buffers[b]->init_duration_us();
      if (d >= 0) {
        source->duration = static_cast<double>(d) / 1e6;
        source->duration_set = true;
        break;
      }
    }
  }
  const double highest = highest_end(source->id);
  if (highest >= 0 && (!source->duration_set || (source->duration < kInfinity && highest > source->duration))) {
    source->duration = highest;
    source->duration_set = true;
  }
}

void MseHub::queue(uint32_t source, const char* type) {
  if (events_.count() < 256) events_.push_back(Event{source, type});
}

bool MseHub::take_event(Event* out) {
  if (events_.count() == 0) return false;
  *out = events_[0];
  events_.remove_at(0);
  return true;
}

}  // namespace media
