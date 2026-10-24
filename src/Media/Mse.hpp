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

// The engine behind the Media Source Extensions: what a page's MediaSource and SourceBuffer objects do with the bytes they are given. No script, no
// element in here, so it can be tested on its own (tests/MseTest.cpp).
//
//   byte stream formats    fragmented MP4 ("video/mp4", "audio/mp4": an initialization segment with the movie box, then moof + mdat pairs; any
//                          number of tracks), MPEG audio ("audio/mpeg", "audio/mp3": layers I-III of MPEG-1, 2 and 2.5, ID3 tags skipped) and AAC in
//                          ADTS ("audio/aac"). A MseParser cuts the bytes into coded frames - in pieces of any size, a segment can be split anywhere.
//   MseBuffer              a SourceBuffer's frames, kept as the standard's coded frame processing says: timestampOffset and the append window,
//                          "segments" and "sequence" mode, discontinuities, waiting for a random access point, replacing what a new frame overlaps
//                          (and the frames that depended on it), remove(), eviction before the playhead, and the buffered ranges. Times are in
//                          microseconds.
//   snapshot               what is buffered around a time, written out as an ordinary file (a flat MP4 with the samples of the range, or the
//                          concatenated MPEG / ADTS frames) for the video and audio players of the machine to open. The range is cut where the
//                          initialization segment changed (a different quality), since one file has one sample description per track.
//   MseHub                 the MediaSources of a page, their buffers, the blob: addresses they were given, and the events they have for script.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

enum class MseFormat : uint8_t { Mp4, Mpeg, Adts };
enum class MseTrackKind : uint8_t { Video, Audio };

// Which byte stream format a MIME type (with its codecs parameter) names; false if this machine cannot play it. `has_video` is true if the type says a
// video codec is in it.
bool mse_type_supported(StringView type, MseFormat* format, bool* has_video);

// Common Encryption (ISO/IEC 23001-7): how the samples of a track are protected.
enum class MseScheme : uint8_t { None, Cenc, Cens, Cbc1, Cbcs };

// What the track's 'tenc' box says (the defaults; a sample group can say otherwise for some samples).
struct MseProtection {
  MseScheme scheme = MseScheme::None;
  bool is_protected = false;
  uint8_t iv_size = 0;                 // bytes of per-sample initialization vector; 0: the constant IV
  uint8_t constant_iv_size = 0;
  uint8_t crypt_blocks = 0, skip_blocks = 0;   // the pattern ('cens' and 'cbcs'; 0, 0: every block)
  uint8_t kid[16] = {};
  uint8_t constant_iv[16] = {};
};

// The keys a player has: the standard of EME asks the key system for a key by its identifier.
class IMseKeys {
 public:
  // The 16-byte key of a key identifier; false if there is none (yet).
  virtual bool key_for(const uint8_t kid[16], uint8_t key[16]) const = 0;
  // Changes whenever keys came or went.
  virtual uint64_t key_epoch() const = 0;

 protected:
  IMseKeys() = default;
  ~IMseKeys() = default;
};

// How one sample is protected, as a media segment says (the subsamples are the 6-byte entries of 'senc': 16 bit clear, 32 bit protected bytes).
struct MseSampleCrypto {
  MseScheme scheme = MseScheme::None;
  uint8_t kid[16] = {};
  uint8_t iv[16] = {};
  uint8_t crypt_blocks = 0, skip_blocks = 0;
  uint32_t subsample_count = 0;
  ConstByteView subsamples;
};

struct MseTrackInfo {
  uint32_t id = 0;
  MseTrackKind kind = MseTrackKind::Audio;
  uint32_t codec = 0;                       // the sample entry's four-character code
  uint32_t timescale = 1000;
  uint32_t width = 0, height = 0, channels = 0, sample_rate = 0;
  uint32_t default_duration = 0, default_size = 0, default_flags = 0;   // trex
  uint64_t entry_offset = 0, entry_size = 0;                           // the sample description box inside MseInit::entries (without its protection)
  MseProtection protection;
};

// An initialization segment: the tracks and the boxes the player needs to know how to decode their samples.
class MseInit {
 public:
  explicit MseInit(allocators::IAllocator& allocator) : tracks(allocator), entries(allocator), pssh(allocator) {}
  List<MseTrackInfo> tracks;
  ByteBuffer entries;
  ByteBuffer pssh;            // the 'pssh' boxes of the movie box, one after the other (what the "cenc" init data of an "encrypted" event is)
  int64_t duration_us = -1;   // from the movie header, -1 if it does not say
  ConstByteView entry(const MseTrackInfo& track) const;
  bool same_as(const MseInit& other) const;   // the same tracks and sample descriptions
  const MseTrackInfo* first_of(MseTrackKind kind) const;
};

// What a parser hands over.
class IMseSink {
 public:
  // False if the stream is not acceptable (the append fails with a decode error).
  virtual bool init_segment(UniquePointer<MseInit> init) = 0;
  // One coded frame of track `track` (an index into the last init segment's tracks), times in microseconds.
  // `crypto` says how the frame is protected (null: it is in the clear).
  virtual bool frame(uint32_t track, int64_t pts_us, int64_t dts_us, int64_t duration_us, bool sync, ConstByteView data, const MseSampleCrypto* crypto) = 0;
  // 'pssh' boxes found in a media segment (a key rotation brings the key identifiers of the next keys this way).
  virtual void protection_data(ConstByteView pssh_boxes) = 0;

 protected:
  IMseSink() = default;
  ~IMseSink() = default;
};

// Cuts a byte stream into frames; bytes may arrive in pieces of any size.
class IMseParser {
 public:
  // False if the bytes cannot be part of a stream of this format.
  virtual bool append(ConstByteView bytes, IMseSink& sink) = 0;
  // Forgets a half-received segment (abort()); what the init segment said stays.
  virtual void reset() = 0;
  // How many bytes of a half-received segment are waiting for the rest.
  virtual uint64_t pending() const = 0;

 protected:
  IMseParser() = default;
  ~IMseParser() = default;
};

UniquePointer<IMseParser> make_mse_parser(allocators::IAllocator& allocator, MseFormat format);

enum class MseAppend : uint8_t { Ok, Decode, Quota, Memory };
enum class MsePlan : uint8_t { Ok, NoData, NeedKey };
enum class MseDecrypt : uint8_t { Ok, NeedKey, Failed };

// What to write out: the range around a time and the pieces to take it from.
struct MseSnapshotPlan {
  int64_t start_us = 0, end_us = 0;   // presentation times; start is a random access point
  uint32_t generation = 0;            // the init segment the lead track's frames belong to
  int64_t base_us = 0;                // the time of time 0 in the written file: element time = file time + base_us (set by snapshot())
  bool has_video = false;
  uint32_t width = 0, height = 0;
};

class MseBuffer final : private IMseSink {
 public:
  struct Range {
    int64_t start, end;
  };
  enum class Mode : uint8_t { Segments, Sequence };

  MseBuffer(allocators::IAllocator& allocator, MseFormat format, uint32_t id);
  ~MseBuffer();
  MseBuffer(const MseBuffer&) = delete;
  MseBuffer& operator=(const MseBuffer&) = delete;

  uint32_t id() const { return id_; }
  MseFormat format() const { return format_; }
  bool init_received() const { return have_init_; }
  // The first video track's size (0 without one).
  uint32_t width() const;
  uint32_t height() const;
  bool has_video() const;
  int64_t init_duration_us() const { return init_duration_us_; }
  uint32_t track_count() const { return slot_count_; }
  // What track `index` is (false if there is no such track).
  bool track_info(uint32_t index, MseTrackKind* kind, uint32_t* id) const;

  // --- Settings
  Mode mode() const { return mode_; }
  bool set_mode(Mode mode);   // false if that is not allowed (the format forces "sequence" for MPEG audio)
  int64_t timestamp_offset() const { return timestamp_offset_; }
  void set_timestamp_offset(int64_t offset_us);
  int64_t window_start() const { return window_start_; }
  int64_t window_end() const { return window_end_; }
  void set_window(int64_t start_us, int64_t end_us) {
    window_start_ = start_us;
    window_end_ = end_us;
  }
  // changeType(): carry on with another format (the frames stay).
  bool change_format(MseFormat format);

  // --- Bytes in
  // Runs the segment parser and the coded frame processing over `bytes`. `playhead_us` is where playback is, for making room (frames before it are
  // evicted when the buffer is over its limit). Quota: nothing was taken, and nothing more can be made room for.
  MseAppend append(ConstByteView bytes, int64_t playhead_us);
  // abort(): a half-received segment is dropped, the next frames start a new coded frame group.
  void abort();
  // remove(): drops the frames presented in [start, end) and the ones that cannot be shown without them.
  void remove(int64_t start_us, int64_t end_us);

  // --- What is held
  // The time ranges that can be played (every track has its frames there). With `reach_us` >= 0 (the source has ended), the tracks' last ranges are
  // extended to that time: the end of the longest track of the whole source.
  void buffered(List<Range>* out, int64_t reach_us = -1) const;
  int64_t highest_end() const;   // the end of the last frame of any track, -1 if there is none
  uint64_t bytes() const { return arena_used_ - garbage_; }
  static constexpr uint64_t kMaxBytes = 64u << 20;
  // A counter that goes up whenever frames were removed or replaced, and was it in [from, to) since `epoch`?
  uint64_t epoch() const { return epoch_; }
  bool changed_since(uint64_t epoch, int64_t from_us, int64_t to_us) const;
  // Bumped by every append that added frames.
  uint64_t appended() const { return appended_; }

  // --- Snapshots
  bool plan(int64_t time_us, MseSnapshotPlan* out) const;
  // The same, for content that may be encrypted: the plan stops before the first frame whose key `keys` does not have; if that is the first frame the plan
  // would start with, there is no plan but NeedKey (and the key identifier it asks for in `missing_kid`, 16 bytes). Null keys: no key at all.
  MsePlan plan_for(int64_t time_us, MseSnapshotPlan* out, const IMseKeys* keys, uint8_t* missing_kid) const;
  // Decrypts, in place, the frames a plan covers (what snapshot() then writes is in the clear).
  MseDecrypt decrypt(const IMseKeys& keys, const MseSnapshotPlan& plan, uint8_t* missing_kid);
  // Init data for the "encrypted" event: false when there is none waiting. `keyids` says the data is a {"kids":[...]} JSON text instead of 'pssh' boxes.
  struct EncryptedEvent {
    explicit EncryptedEvent(allocators::IAllocator& a) : data(a) {}
    bool keyids = false;
    ByteBuffer data;
  };
  bool take_encrypted(UniquePointer<EncryptedEvent>* out);
  bool has_protected_frames() const { return protected_frames_ != 0; }
  // Writes the file for a plan; fills in plan->base_us and the video size. False if memory ran out.
  bool snapshot(MseSnapshotPlan* plan, ByteBuffer* out) const;

 private:
  struct Frame {
    int64_t pts, dts, duration;
    uint64_t offset;
    uint32_t size, generation;
    uint32_t aux;            // 0: in the clear; else the position of its protection record in aux_, plus 1
    uint8_t slot;
    bool sync;
  };
  class FrameList;
  struct Slot;

  // IMseSink
  bool init_segment(UniquePointer<MseInit> init) override;
  bool frame(uint32_t track, int64_t pts_us, int64_t dts_us, int64_t duration_us, bool sync, ConstByteView data, const MseSampleCrypto* crypto) override;
  void protection_data(ConstByteView pssh_boxes) override;

  bool add_frame(uint32_t slot, int64_t pts, int64_t dts, int64_t duration, bool sync, ConstByteView data, const MseSampleCrypto* crypto);
  void queue_encrypted(bool keyids, ConstByteView data);
  void forget_aux(const Frame& f);
  void remove_overlap(uint32_t slot, int64_t pts, int64_t duration);
  void erase_frames(uint32_t slot, uint64_t first, uint64_t last);   // [first, last)
  void note_removed(int64_t from, int64_t to);
  void reset_group();
  void compact();
  bool make_room(uint64_t needed, int64_t playhead_us);
  void slot_ranges(uint32_t slot, List<Range>* out) const;
  bool write_mp4(const MseSnapshotPlan& plan, ByteBuffer* out, int64_t* base_us) const;

  allocators::IAllocator& allocator_;
  MseFormat format_;
  uint32_t id_;
  UniquePointer<IMseParser> parser_;
  List<UniquePointer<MseInit>> inits_;
  bool have_init_ = false;
  int64_t init_duration_us_ = -1;
  uint32_t current_generation_ = 0;
  int8_t slot_map_[8];       // track index of the current init segment -> slot, -1 for a track that is not kept
  uint32_t slot_map_count_ = 0;

  static constexpr uint32_t kMaxSlots = 4;
  UniquePointer<Slot> slots_[kMaxSlots];
  uint32_t slot_count_ = 0;

  UniquePointer<ByteBuffer> arena_;
  UniquePointer<ByteBuffer> aux_;   // the protection records of the encrypted frames
  uint64_t aux_garbage_ = 0, protected_frames_ = 0;
  List<UniquePointer<EncryptedEvent>> encrypted_;
  List<UniquePointer<ByteBuffer>> sent_;   // (init data announced lately, so that the same is not announced again)
  uint64_t arena_used_ = 0, garbage_ = 0;

  Mode mode_ = Mode::Segments;
  int64_t timestamp_offset_ = 0;
  int64_t window_start_ = 0, window_end_ = INT64_MAX;
  int64_t group_end_ = 0;
  bool group_start_pending_ = false;
  int64_t highest_added_ = -1;
  bool memory_error_ = false;
  mutable List<Range> range_cache_;
  mutable bool range_cache_valid_ = false;

  uint64_t epoch_ = 0, appended_ = 0;
  struct Change {
    uint64_t epoch;
    int64_t from, to;
  };
  Change changes_[32];
  uint32_t change_count_ = 0;   // how many are valid (the ring is full beyond 32)
  uint32_t change_next_ = 0;
  uint64_t dropped_epoch_ = 0;   // the newest change the ring has forgotten
};

// --- The MediaSources of a page ---------------------------------------------------------------------------------------------------------------

class MseSource {
 public:
  enum class State : uint8_t { Closed, Open, Ended };
  explicit MseSource(allocators::IAllocator& allocator, uint32_t source_id) : id(source_id), buffers(allocator) {}
  uint32_t id;
  State state = State::Closed;
  bool duration_set = false;
  double duration = 0;              // seconds (+infinity allowed) while duration_set
  uint32_t element = 0;             // the serial of the element it is attached to, 0 for none
  uint8_t error = 0;                // endOfStream("network") is 2, ("decode") is 3
  bool live_range = false;
  double live_start = 0, live_end = 0;
  List<UniquePointer<MseBuffer>> buffers;
};

class MseHub {
 public:
  explicit MseHub(allocators::IAllocator& allocator) : allocator_(allocator), sources_(allocator), urls_(allocator), events_(allocator) {}
  static constexpr uint32_t kMaxSources = 16, kMaxBuffersPerSource = 4;
  static constexpr uint64_t kMaxTotalBytes = 192u << 20;

  uint32_t create_source();   // 0 if there are too many
  MseSource* find(uint32_t source);
  void drop_source(uint32_t source);   // the object is gone from script (only a closed, unattached one can go)

  enum class AddResult : uint8_t { Ok, NotSupported, InvalidState, Quota, Memory };
  // addSourceBuffer(): `buffer` gets the new buffer's id.
  AddResult add_buffer(uint32_t source, StringView type, uint32_t* buffer);
  bool remove_buffer(uint32_t source, uint32_t buffer);
  MseBuffer* find_buffer(uint32_t buffer, MseSource** source = nullptr);

  // --- blob: addresses
  bool register_url(StringView url, uint32_t source);
  void revoke_url(StringView url);
  uint32_t source_for_url(StringView url) const;

  // --- Attaching to an element
  // The element starts using the source: it opens ("sourceopen" follows). False if it is attached to another element already.
  bool attach(uint32_t source, uint32_t element);
  // The element lets go: the source closes, its buffers are removed ("sourceclose" follows).
  void detach(uint32_t source);

  // --- MediaSource operations
  // False if the duration may not be changed that way.
  bool set_duration(uint32_t source, double seconds);
  void end_of_stream(uint32_t source, uint8_t error);
  // Gets the source back to "open" (an append to an ended source does that).
  void reopen(uint32_t source);
  // The ranges every buffer has in common (HTMLMediaElement.buffered), in seconds, as start, end pairs.
  void buffered(uint32_t source, List<double>* out);
  // The end of the longest buffer in seconds, -1 if none.
  double highest_end(uint32_t source);
  uint64_t total_bytes();
  // Called after bytes were added: the duration grows to hold them.
  void appended(MseSource* source);

  struct Event {
    uint32_t source = 0;
    const char* type = "";
  };
  void queue(uint32_t source, const char* type);
  bool take_event(Event* out);

 private:
  struct Url {
    explicit Url(allocators::IAllocator& a) : address(a) {}
    String address;
    uint32_t source = 0;
  };
  allocators::IAllocator& allocator_;
  List<UniquePointer<MseSource>> sources_;
  List<UniquePointer<Url>> urls_;
  List<Event> events_;
  uint32_t next_source_ = 1, next_buffer_ = 1;
};

}  // namespace media
