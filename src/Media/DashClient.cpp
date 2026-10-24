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
#include "Media/Dash.hpp"
#include "Utility/Move.hpp"

namespace media {

namespace {

constexpr int64_t kTargetAheadUs = 20000000;      // buffer this much beyond the playhead
constexpr int64_t kUpSwitchAheadUs = 6000000;     // switch to a better quality only with this much buffered
constexpr uint32_t kFetchAttempts = 3;
constexpr int64_t kGapToleranceUs = 150000;
constexpr int64_t kDefaultLiveDelayUs = 6000000;

bool type_string(const DashRepresentation& rep, String* out) {
  if (rep.mime_type.empty()) return false;
  if (!out->assign(rep.mime_type.view())) return false;
  if (!rep.codecs.empty() && (!out->append("; codecs=\"") || !out->append(rep.codecs.view()) || !out->push_back('"'))) return false;
  return true;
}

bool playable(const DashAdaptationSet& set, const DashRepresentation& rep) {
  if (set.protection.count() != 0) {   // only a protection that the player can ignore (the 'mp4protection' descriptor without a key system) is allowed
    for (const auto& p : set.protection.view())
      if (!p->scheme.view().equals("urn:mpeg:dash:mp4protection:2011")) return false;
  }
  String type(set.content_type.allocator());
  return type_string(rep, &type) && mse_type_supported(type.view(), nullptr, nullptr);
}

}  // namespace

struct DashClient::CachedSegments {
  explicit CachedSegments(allocators::IAllocator& a) : segments(a) {}
  uint64_t period = 0;
  const DashRepresentation* rep = nullptr;
  int64_t built_edge = 0;
  DashSegments segments;
};

struct DashClient::Stream {
  explicit Stream(allocators::IAllocator& a) : kind(a), init_key(a), cache(a) {}
  String kind;               // "video" or "audio"
  uint32_t buffer = 0;
  int64_t next_us = 0;       // presentation time (before the live shift) the next segment must cover
  uint64_t chosen_bandwidth = 0;
  String init_key;           // the initialization segment (and offset) the buffer was last given
  int64_t offset_us = INT64_MIN;
  List<UniquePointer<CachedSegments>> cache;
  uint32_t failures = 0;
  bool done = false;
  bool positioned = false;
  bool automatic = true;
  uint32_t forced = 0;   // the quality index held when !automatic
};

DashClient::DashClient(allocators::IAllocator& allocator, MseHub& hub, uint32_t source, IDashFetcher& fetcher)
    : allocator_(allocator), hub_(hub), source_(source), fetcher_(&fetcher), manifest_url_(allocator), streams_(allocator) {}

DashClient::~DashClient() = default;

bool DashClient::fail(uint8_t code) {
  if (error_ == 0) error_ = code;
  hub_.end_of_stream(source_, code == 2 ? 2 : 3);
  return false;
}

uint32_t DashClient::stream_count() const { return static_cast<uint32_t>(streams_.count()); }
uint64_t DashClient::chosen_bandwidth(uint32_t s) const { return s < streams_.count() ? streams_[s]->chosen_bandwidth : 0; }

int64_t DashClient::live_edge_us() const {
  const DashManifest& m = *manifest_;
  const int64_t wall = fetcher_->wall_clock_s();
  if (m.availability_start_s >= 0 && wall > 0) return (wall - m.availability_start_s) * 1000000 + static_cast<int64_t>((fetcher_->now_ms() % 1000)) * 1000;
  return static_cast<int64_t>(fetcher_->now_ms() - started_ms_) * 1000 + 12000000;
}

int64_t DashClient::period_end(const DashPeriod& p, uint64_t index) const {
  if (p.duration_us >= 0) return p.start_us + p.duration_us;
  if (index + 1 < manifest_->periods.count()) return manifest_->periods[index + 1]->start_us;
  if (manifest_->duration_us >= 0) return manifest_->duration_us;
  return -1;
}

const DashPeriod* DashClient::period_at(int64_t t, uint64_t* index) const {
  const DashManifest& m = *manifest_;
  for (uint64_t i = m.periods.count(); i > 0; --i) {
    if (m.periods[i - 1]->start_us <= t || i == 1) {
      *index = i - 1;
      return m.periods[i - 1].get();
    }
  }
  return nullptr;
}

const DashAdaptationSet* DashClient::set_of(const DashPeriod& p, const Stream& s) const {
  for (const auto& set : p.sets.view()) {
    if (!set->content_type.view().equals(s.kind.view())) continue;
    for (const auto& r : set->representations.view())
      if (playable(*set, *r)) return set.get();
  }
  return nullptr;
}

bool DashClient::fetch_all(StringView url, DashRange range, ByteBuffer* out, uint32_t attempts) {
  for (uint32_t i = 0; i < attempts; ++i) {
    out->clear();
    if (fetcher_->fetch(url, range, out)) return true;
  }
  return false;
}

bool DashClient::start(StringView manifest_url, ConstByteView mpd) {
  manifest_url_.assign(manifest_url);
  manifest_ = UniquePointer<DashManifest>::make(allocator_, allocator_);
  started_ms_ = last_refresh_ms_ = fetcher_->now_ms();
  if (!manifest_ || !parse_mpd(allocator_, StringView(mpd), manifest_url, manifest_.get())) return fail(4);
  const DashManifest& m = *manifest_;
  const DashPeriod& first = *m.periods[0];
  uint32_t made = 0;
  static const char* const kKinds[2] = {"video", "audio"};
  for (const char* kind : kKinds) {
    auto stream = UniquePointer<Stream>::make(allocator_, allocator_);
    if (!stream) return fail(3);
    stream->kind.assign(kind);
    const DashAdaptationSet* set = set_of(first, *stream);
    if (!set) continue;
    const DashRepresentation* rep = nullptr;
    for (const auto& r : set->representations.view())
      if (playable(*set, *r)) {
        rep = r.get();
        break;
      }
    String type(allocator_);
    uint32_t buffer = 0;
    if (!rep || !type_string(*rep, &type) || hub_.add_buffer(source_, type.view(), &buffer) != MseHub::AddResult::Ok) continue;
    stream->buffer = buffer;
    if (!streams_.push_back(util::move(stream))) return fail(3);
    ++made;
  }
  if (made == 0) return fail(4);
  if (!m.dynamic && m.duration_us >= 0) hub_.set_duration(source_, static_cast<double>(m.duration_us) / 1e6);
  if (m.dynamic) {
    // playback starts a little behind the live edge
    const int64_t delay = m.suggested_delay_us >= 0 ? m.suggested_delay_us : (3 * m.min_buffer_us > kDefaultLiveDelayUs ? 3 * m.min_buffer_us : kDefaultLiveDelayUs);
    int64_t start = live_edge_us() - delay;
    if (start < first.start_us) start = first.start_us;
    live_shift_us_ = start;
    live_shift_set_ = true;
    for (auto& s : streams_.view()) s->next_us = start;
  } else {
    for (auto& s : streams_.view()) s->next_us = first.start_us;
  }
  return true;
}

bool DashClient::refresh_manifest() {
  const DashManifest& old = *manifest_;
  const uint64_t now = fetcher_->now_ms();
  const int64_t every = old.min_update_us > 1000000 ? old.min_update_us : 1000000;
  if (!old.dynamic || old.min_update_us < 0 || static_cast<int64_t>(now - last_refresh_ms_) * 1000 < every) return true;
  last_refresh_ms_ = now;
  String url(allocator_);
  url.assign(!old.location.empty() ? old.location.view() : manifest_url_.view());
  ByteBuffer bytes(allocator_);
  if (!fetcher_->fetch(url.view(), DashRange(), &bytes)) return true;   // (try again next time)
  auto fresh = UniquePointer<DashManifest>::make(allocator_, allocator_);
  if (!fresh || !parse_mpd(allocator_, StringView(bytes.view()), url.view(), fresh.get())) return true;
  for (auto& s : streams_.view()) s->cache.clear();
  manifest_ = util::move(fresh);
  return true;
}

DashSegments* DashClient::segments_for(Stream& s, uint64_t period_index, const DashRepresentation& rep) {
  const DashManifest& m = *manifest_;
  const DashPeriod& p = *m.periods[period_index];
  const int64_t edge = m.dynamic ? live_edge_us() : -1;
  for (uint64_t i = 0; i < s.cache.count(); ++i) {
    CachedSegments& c = *s.cache[i];
    if (c.period != period_index || c.rep != &rep) continue;
    const bool stale = m.dynamic && edge - c.built_edge > 1000000;
    if (!stale) return &c.segments;
    s.cache.remove_at(i);
    break;
  }
  auto fresh = UniquePointer<CachedSegments>::make(allocator_, allocator_);
  if (!fresh) return nullptr;
  fresh->period = period_index;
  fresh->rep = &rep;
  fresh->built_edge = edge;
  if (!dash_segments(allocator_, m, p, period_end(p, period_index), rep, edge, fetcher_, &fresh->segments)) return nullptr;
  CachedSegments* raw = fresh.get();
  if (s.cache.count() >= 12) s.cache.remove_at(0);
  if (!s.cache.push_back(util::move(fresh))) return nullptr;
  return &raw->segments;
}

bool DashClient::pick_representation(Stream& s, const DashPeriod& p, uint64_t period_index, int64_t ahead_us, const DashRepresentation** out) {
  (void)period_index;
  const DashAdaptationSet* set = set_of(p, s);
  if (!set) return false;
  const DashRepresentation* lowest = nullptr;
  const DashRepresentation* best = nullptr;
  const DashRepresentation* current = nullptr;
  const uint64_t allowed = throughput_ == 0 ? 0 : throughput_ / 100 * 85;
  for (const auto& r : set->representations.view()) {
    if (!playable(*set, *r)) continue;
    if (!lowest) lowest = r.get();
    if (allowed != 0 && r->bandwidth <= allowed) best = r.get();
    if (s.chosen_bandwidth != 0 && r->bandwidth == s.chosen_bandwidth) current = r.get();
  }
  const DashRepresentation* pick = best ? best : lowest;
  if (!s.automatic) {   // a quality that is held
    uint32_t index = 0;
    const DashRepresentation* held = nullptr;
    for (const auto& r : set->representations.view()) {
      if (!playable(*set, *r)) continue;
      held = r.get();
      if (index++ >= s.forced) break;
    }
    if (held) {
      *out = held;
      return true;
    }
  }
  if (current && pick && pick->bandwidth > current->bandwidth && ahead_us < kUpSwitchAheadUs && allowed < pick->bandwidth * 2) pick = current;   // not yet: the buffer is too thin to risk it
  if (!pick) return false;
  *out = pick;
  return true;
}

void DashClient::tick_stream(Stream& s, int64_t playhead_us) {
  if (s.done || error_ != 0) return;
  MseBuffer* buffer = hub_.find_buffer(s.buffer);
  if (!buffer) return;
  const DashManifest& m = *manifest_;
  const int64_t shift = live_shift_set_ ? live_shift_us_ : 0;
  // where the buffer is, and so where the next segment must come from
  List<MseBuffer::Range> ranges(allocator_);
  buffer->buffered(&ranges);
  int64_t range_end = -1;
  for (const auto& r : ranges.view())
    if (r.start - kGapToleranceUs <= playhead_us && playhead_us <= r.end + kGapToleranceUs) range_end = r.end;
  bool containing = false;
  if (range_end < 0) {   // the playhead is somewhere nothing is buffered: a seek (or the start)
    if (!s.positioned || s.next_us < playhead_us + shift - 1000000 || s.next_us > playhead_us + shift + kTargetAheadUs) {
      s.next_us = playhead_us + shift;
      containing = true;
    }
  } else if (s.next_us > range_end + shift + kGapToleranceUs) {
    s.next_us = range_end + shift;   // a hole behind the cursor
  }
  s.positioned = true;
  const int64_t ahead = (range_end >= 0 ? range_end : playhead_us) - playhead_us;
  if (s.next_us - shift - playhead_us >= kTargetAheadUs) return;

  uint64_t period_index;
  const DashPeriod* period = period_at(s.next_us, &period_index);
  if (!period) return;
  const int64_t end = period_end(*period, period_index);
  const bool last_period = period_index + 1 == m.periods.count();
  if (end >= 0 && s.next_us >= end - 1000) {   // the period is over
    if (!last_period) {
      s.next_us = m.periods[period_index + 1]->start_us;
      return;
    }
    if (!m.dynamic) {
      s.done = true;
      return;
    }
  }
  const DashRepresentation* rep = nullptr;
  if (!pick_representation(s, *period, period_index, ahead, &rep)) {
    if (!last_period) s.next_us = m.periods[period_index + 1]->start_us;
    else s.done = !m.dynamic;
    return;
  }
  DashSegments* segs = segments_for(s, period_index, *rep);
  if (!segs) {
    if (++s.failures >= kFetchAttempts) fail(2);
    return;
  }
  const DashSegment* seg = nullptr;
  for (const DashSegment& candidate : segs->list.view()) {
    if (containing ? candidate.start_us + candidate.duration_us > s.next_us : candidate.start_us >= s.next_us - 50000) {
      seg = &candidate;
      break;
    }
  }
  if (!seg) {
    if (!last_period) s.next_us = m.periods[period_index + 1]->start_us;
    else if (!m.dynamic) s.done = true;
    return;   // (a dynamic presentation waits for the next segment to be published)
  }
  if (m.dynamic && seg->start_us + seg->duration_us > live_edge_us()) return;

  // initialization segment and timestamp offset
  const int64_t offset = segs->timestamp_offset_us - shift;
  String key(allocator_);
  key.append(segs->init_url.view());
  key.push_back('#');
  key.append(rep->id.view());
  key.push_back('#');
  key.append_uint(period_index);
  if (!s.init_key.view().equals(key.view())) {
    if (!segs->init_url.empty()) {
      ByteBuffer init(allocator_);
      if (!fetch_all(segs->init_url.view(), segs->init_range, &init, kFetchAttempts)) {
        fail(2);
        return;
      }
      buffer->set_timestamp_offset(offset);
      s.offset_us = offset;
      const MseAppend r = buffer->append(init.view(), playhead_us);
      if (r != MseAppend::Ok) {
        if (r != MseAppend::Quota) fail(3);
        return;
      }
    }
    s.init_key.assign(key.view());
    if (s.chosen_bandwidth != 0 && s.chosen_bandwidth != rep->bandwidth) ++switches_;
  }
  if (s.chosen_bandwidth != rep->bandwidth) ++quality_epoch_;
  if (s.offset_us != offset) {
    buffer->set_timestamp_offset(offset);
    s.offset_us = offset;
  }

  String url(allocator_);
  if (!dash_media_url(allocator_, *rep, *seg, &url)) {
    fail(4);
    return;
  }
  ByteBuffer bytes(allocator_);
  const uint64_t t0 = fetcher_->now_ms();
  if (!fetch_all(url.view(), seg->range, &bytes, kFetchAttempts)) {
    fail(2);
    return;
  }
  const uint64_t t1 = fetcher_->now_ms();
  const uint64_t ms = t1 > t0 ? t1 - t0 : 1;
  const uint64_t measured = bytes.size() * 8000 / ms;
  throughput_ = throughput_ == 0 ? measured : (throughput_ * 6 + measured * 4) / 10;
  s.chosen_bandwidth = rep->bandwidth;
  ++fetched_;
  const MseAppend r = buffer->append(bytes.view(), playhead_us);
  if (r == MseAppend::Quota) return;   // full: retry once playback has made room
  if (r != MseAppend::Ok) {
    fail(3);
    return;
  }
  if (MseSource* source = hub_.find(source_)) hub_.appended(source);
  s.next_us = seg->start_us + seg->duration_us;
  s.failures = 0;
}

StringView DashClient::stream_kind(uint32_t s) const { return s < streams_.count() ? streams_[s]->kind.view() : StringView(); }

uint32_t DashClient::quality_count(uint32_t s) const {
  if (s >= streams_.count() || !manifest_) return 0;
  const DashAdaptationSet* set = set_of(*manifest_->periods[0], *streams_[s]);
  uint32_t n = 0;
  if (set)
    for (const auto& r : set->representations.view()) n += playable(*set, *r) ? 1 : 0;
  return n;
}

bool DashClient::quality_info(uint32_t s, uint32_t index, QualityInfo* out) const {
  if (s >= streams_.count() || !manifest_) return false;
  const DashAdaptationSet* set = set_of(*manifest_->periods[0], *streams_[s]);
  if (!set) return false;
  uint32_t n = 0;
  for (const auto& r : set->representations.view()) {
    if (!playable(*set, *r)) continue;
    if (n++ != index) continue;
    out->bandwidth = r->bandwidth;
    out->width = r->width;
    out->height = r->height;
    out->id.assign(r->id.view());
    out->codecs.assign(r->codecs.view());
    return true;
  }
  return false;
}

int32_t DashClient::current_quality(uint32_t s) const {
  if (s >= streams_.count() || streams_[s]->chosen_bandwidth == 0 || !manifest_) return -1;
  const DashAdaptationSet* set = set_of(*manifest_->periods[0], *streams_[s]);
  if (!set) return -1;
  int32_t n = 0;
  for (const auto& r : set->representations.view()) {
    if (!playable(*set, *r)) continue;
    if (r->bandwidth == streams_[s]->chosen_bandwidth) return n;
    ++n;
  }
  return -1;
}

bool DashClient::auto_switch(uint32_t s) const { return s < streams_.count() && streams_[s]->automatic; }
void DashClient::set_auto_switch(uint32_t s, bool on) {
  if (s < streams_.count()) streams_[s]->automatic = on;
}
void DashClient::set_quality(uint32_t s, uint32_t index) {
  if (s >= streams_.count()) return;
  streams_[s]->automatic = false;
  const uint32_t n = quality_count(s);
  streams_[s]->forced = index >= n && n > 0 ? n - 1 : index;
}

double DashClient::buffer_level(uint32_t s, int64_t playhead_us) const {
  if (s >= streams_.count()) return 0;
  MseBuffer* buffer = hub_.find_buffer(streams_[s]->buffer);
  if (!buffer) return 0;
  List<MseBuffer::Range> ranges(allocator_);
  buffer->buffered(&ranges);
  for (const auto& r : ranges.view())
    if (r.start - kGapToleranceUs <= playhead_us && playhead_us <= r.end) return static_cast<double>(r.end - playhead_us) / 1e6;
  return 0;
}

void DashClient::finish_if_done() {
  if (ended_ || error_ != 0 || manifest_->dynamic) return;
  for (const auto& s : streams_.view())
    if (!s->done) return;
  ended_ = true;
  hub_.end_of_stream(source_, 0);
}

void DashClient::tick(int64_t playhead_us) {
  if (!manifest_ || error_ != 0 || ended_) return;
  if (hub_.find(source_) == nullptr) return;
  refresh_manifest();
  for (auto& s : streams_.view()) tick_stream(*s, playhead_us);
  finish_if_done();
}

}  // namespace media
