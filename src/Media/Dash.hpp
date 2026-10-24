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
#pragma once
// MPEG-DASH (ISO/IEC 23009-1): the manifest (MPD) and how it names segments, and DashClient, which plays one by feeding the SourceBuffers of a MediaSource.
//
//   parse_xml()        a small XML reader (elements, attributes, text; namespaces dropped; entities, CDATA, comments).
//   DashManifest       static and dynamic presentations; Periods, AdaptationSets, Representations; BaseURL chains; SegmentTemplate ($RepresentationID$,
//                      $Number$, $Bandwidth$, $Time$ with printf widths, $$), SegmentTimeline (S@t d r), SegmentList, SegmentBase with a 'sidx' index;
//                      attributes inherited from Period and AdaptationSet; ISO 8601 durations and dates; ContentProtection descriptors.
//   DashClient         chooses a Representation for each kind of media (adaptive: by the throughput of the last segments and the buffer), fetches
//                      initialization and media segments, appends them (with the Period's timestamp offset), follows seeks, refreshes a dynamic manifest,
//                      and ends the stream when the presentation does.
#include "Allocators/IAllocator.hpp"
#include "Media/Mse.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

// --- XML ----------------------------------------------------------------------------------------------------------------------------------------

struct XmlAttribute {
  explicit XmlAttribute(allocators::IAllocator& a) : name(a), value(a) {}
  String name, value;
};

struct XmlNode {
  explicit XmlNode(allocators::IAllocator& a) : name(a), text(a), attributes(a), children(a) {}
  String name;   // the local name (no prefix)
  String text;   // the character data directly inside, joined
  List<UniquePointer<XmlAttribute>> attributes;
  List<UniquePointer<XmlNode>> children;
  StringView attribute(StringView attribute_name, StringView fallback = StringView()) const;
  bool has_attribute(StringView attribute_name) const;
  const XmlNode* child(StringView child_name) const;   // the first element child with that name
};

// Reads an XML document; the root element. Null if it is not well formed (or too big or too deep).
UniquePointer<XmlNode> parse_xml(allocators::IAllocator& allocator, StringView text);

// --- The manifest ---------------------------------------------------------------------------------------------------------------------------------

struct DashTimelineEntry {
  uint64_t time = 0;      // in timescale units
  uint64_t duration = 0;
  uint32_t repeat = 0;    // extra repetitions (r >= 0; r = -1 is resolved against the end of the period)
  bool open_ended = false;
};

struct DashRange {
  bool present = false;
  uint64_t first = 0, last = 0;   // inclusive byte positions
};

struct DashTemplate {
  explicit DashTemplate(allocators::IAllocator& a) : media(a), initialization(a), timeline(a) {}
  bool present = false;
  String media, initialization;
  uint64_t timescale = 1, duration = 0, start_number = 1;
  int64_t presentation_time_offset = 0;
  bool has_timeline = false;
  List<DashTimelineEntry> timeline;
};

struct DashListItem {
  explicit DashListItem(allocators::IAllocator& a) : media(a) {}
  String media;
  DashRange range;
};

struct DashList {
  explicit DashList(allocators::IAllocator& a) : initialization(a), timeline(a), items(a) {}
  bool present = false;
  String initialization;
  DashRange initialization_range;
  uint64_t timescale = 1, duration = 0, start_number = 1;
  int64_t presentation_time_offset = 0;
  bool has_timeline = false;
  List<DashTimelineEntry> timeline;
  List<UniquePointer<DashListItem>> items;
};

struct DashBase {
  bool present = false;
  uint64_t timescale = 1;
  int64_t presentation_time_offset = 0;
  DashRange index_range, initialization_range;
};

struct DashProtection {
  explicit DashProtection(allocators::IAllocator& a) : scheme(a), default_kid(a) {}
  String scheme;        // schemeIdUri, lower case
  String default_kid;   // cenc:default_KID
};

struct DashRepresentation {
  explicit DashRepresentation(allocators::IAllocator& a)
      : id(a), mime_type(a), codecs(a), base_url(a), segment_template(a), segment_list(a) {}
  String id, mime_type, codecs;
  String base_url;   // the absolute address everything of the representation is relative to
  uint64_t bandwidth = 0;
  uint32_t width = 0, height = 0;
  DashTemplate segment_template;
  DashList segment_list;
  DashBase segment_base;
};

struct DashAdaptationSet {
  explicit DashAdaptationSet(allocators::IAllocator& a) : content_type(a), language(a), representations(a), protection(a) {}
  String content_type;   // "video", "audio", "text" (from contentType, else the mimeType)
  String language;
  List<UniquePointer<DashRepresentation>> representations;   // in order of bandwidth, lowest first
  List<UniquePointer<DashProtection>> protection;
};

struct DashPeriod {
  explicit DashPeriod(allocators::IAllocator& a) : id(a), sets(a) {}
  String id;
  int64_t start_us = 0;
  int64_t duration_us = -1;   // -1: until the next Period / the end of the presentation
  List<UniquePointer<DashAdaptationSet>> sets;
};

struct DashManifest {
  explicit DashManifest(allocators::IAllocator& a) : location(a), periods(a) {}
  bool dynamic = false;
  int64_t duration_us = -1;                  // mediaPresentationDuration, -1 if absent
  int64_t min_buffer_us = 2000000;
  int64_t min_update_us = -1;                // minimumUpdatePeriod, -1 if the manifest does not change
  int64_t time_shift_depth_us = -1;
  int64_t suggested_delay_us = -1;
  int64_t availability_start_s = -1;         // availabilityStartTime, seconds since 1970, -1 if absent
  String location;                           // <Location>: where the next manifest is
  List<UniquePointer<DashPeriod>> periods;
};

// Reads an MPD. `manifest_url` is what relative addresses are relative to. False if it is not a valid MPD.
bool parse_mpd(allocators::IAllocator& allocator, StringView mpd, StringView manifest_url, DashManifest* out);

bool parse_iso_duration(StringView text, int64_t* microseconds);
bool parse_iso_datetime(StringView text, int64_t* seconds);   // 1970 epoch, whole seconds (a fraction is dropped)

// One media segment of a representation.
struct DashSegment {
  int64_t start_us = 0;      // presentation time (Period start included)
  int64_t duration_us = 0;
  uint64_t number = 0;
  uint64_t time = 0;         // in the timescale of the representation, as the template says ($Time$)
  DashRange range;           // bytes of the file, for SegmentList / SegmentBase
  int32_t item = -1;         // SegmentList: which SegmentURL
};

struct DashSegments {
  explicit DashSegments(allocators::IAllocator& a) : list(a), init_url(a) {}
  List<DashSegment> list;
  String init_url;          // empty: the media segments are self-initializing
  DashRange init_range;
  int64_t timestamp_offset_us = 0;   // add to the media's time to get the presentation time
};

// The segments of a representation in a period. `live_edge_us` is the newest presentation time that is available (dynamic manifests; ignored otherwise).
// For SegmentBase the index ('sidx') is read through `fetch_index` (a callback that returns the bytes of a range of the representation's file).
class IDashRangeSource {
 public:
  virtual bool fetch(StringView url, DashRange range, ByteBuffer* out) = 0;

 protected:
  IDashRangeSource() = default;
  ~IDashRangeSource() = default;
};
bool dash_segments(allocators::IAllocator& allocator, const DashManifest& manifest, const DashPeriod& period, int64_t period_end_us, const DashRepresentation& rep,
                   int64_t live_edge_us, IDashRangeSource* source, DashSegments* out);

// The address of a media segment, and of the initialization segment (empty if there is none).
bool dash_media_url(allocators::IAllocator& allocator, const DashRepresentation& rep, const DashSegment& segment, String* out);
// Expands the identifiers of a template ($Number$ ...). False for a malformed template.
bool dash_expand(StringView pattern, StringView representation_id, uint64_t bandwidth, uint64_t number, uint64_t time, String* out);

// The 'sidx' box at the front of `bytes`: the references as (offset from the end of the box + first_offset, size, duration in timescale units).
struct DashSidxEntry {
  uint64_t offset, size, duration;
  bool sap;
};
bool parse_sidx(allocators::IAllocator& allocator, ConstByteView bytes, uint64_t anchor, uint64_t* timescale, uint64_t* earliest, List<DashSidxEntry>* out);

// --- The player -----------------------------------------------------------------------------------------------------------------------------------

class IDashFetcher : public IDashRangeSource {
 public:
  // The host clock in milliseconds (to measure how fast segments come, and to know the live edge).
  virtual uint64_t now_ms() = 0;
  // Seconds since 1970 on the wall clock (for dynamic manifests); 0 if unknown.
  virtual int64_t wall_clock_s() { return 0; }

 protected:
  IDashFetcher() = default;
  ~IDashFetcher() = default;
};

class DashClient {
 public:
  // The MediaSource `source` must be open (attached to an element).
  DashClient(allocators::IAllocator& allocator, MseHub& hub, uint32_t source, IDashFetcher& fetcher);
  ~DashClient();
  DashClient(const DashClient&) = delete;
  DashClient& operator=(const DashClient&) = delete;

  // Reads the manifest and makes the SourceBuffers. False if it cannot be played (error() says why).
  bool start(StringView manifest_url, ConstByteView mpd);
  // Fetches what the buffers are short of. `playhead_us`: where playback is. At most one segment of each kind per call.
  void tick(int64_t playhead_us);
  // 0 none, 2 network, 3 decode, 4 not supported (MediaError codes).
  uint8_t error() const { return error_; }
  const DashManifest* manifest() const { return manifest_.get(); }
  void set_fetcher(IDashFetcher& fetcher) { fetcher_ = &fetcher; }

  // For tests and for the page's information.
  uint32_t stream_count() const;
  uint64_t chosen_bandwidth(uint32_t stream) const;
  uint32_t segments_fetched() const { return fetched_; }
  uint32_t switches() const { return switches_; }
  bool ended() const { return ended_; }
  uint64_t throughput_bps() const { return throughput_; }

 private:
  struct Stream;
  struct CachedSegments;
  bool fail(uint8_t code);
  bool refresh_manifest();
  int64_t period_end(const DashPeriod& p, uint64_t index) const;
  const DashPeriod* period_at(int64_t time_us, uint64_t* index) const;
  const DashAdaptationSet* set_of(const DashPeriod& p, const Stream& s) const;
  bool pick_representation(Stream& s, const DashPeriod& p, uint64_t period_index, int64_t ahead_us, const DashRepresentation** rep);
  DashSegments* segments_for(Stream& s, uint64_t period_index, const DashRepresentation& rep);
  void tick_stream(Stream& s, int64_t playhead_us);
  int64_t live_edge_us() const;
  bool fetch_all(StringView url, DashRange range, ByteBuffer* out, uint32_t attempts);
  void finish_if_done();

  allocators::IAllocator& allocator_;
  MseHub& hub_;
  uint32_t source_;
  IDashFetcher* fetcher_;
  UniquePointer<DashManifest> manifest_;
  String manifest_url_;
  List<UniquePointer<Stream>> streams_;
  uint8_t error_ = 0;
  bool ended_ = false;
  uint64_t throughput_ = 0;
  uint32_t fetched_ = 0, switches_ = 0;
  uint64_t started_ms_ = 0, last_refresh_ms_ = 0;
  int64_t live_shift_us_ = 0;
  bool live_shift_set_ = false;
};

}  // namespace media
