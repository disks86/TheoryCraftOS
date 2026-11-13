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

// The client-node factory of the native protocol (NativeServer.hpp): how libpipewire's pw_stream and pw_filter programs (pw-play, pw-record, pw-cat,
// pw-midiplay, a camera or screen-sharing program, native PipeWire programs) play, record, show and watch through the graph; and the adapter factory's
// virtual sinks (support.null-audio-sink).
//
// A program exports a node with Core::create_object("client-node"). The server answers as PipeWire's module-client-node does, and plays the session
// manager and the driver as well:
//   1. the activation record (Core add_mem of a memfd) and the ClientNode transport (two eventfds); once the graph node is made, bound_props and the
//      node's clock and position (set_io Clock, Position) in that record;
//   2. once the program says it is active: an adapter (pw_stream of audio or video, which has no ports yet) gets a PortConfig: audio in convert mode at
//      the stream's own rate and channels, F32 (one interleaved port), video in passthrough mode (the stream's own port). A node whose ports came before
//      it was active (a pw_filter, a MIDI stream) is raw: it keeps its ports;
//   3. each port: its Format (the adapter's audio format; the port's own EnumFormat fixated: a video producer's size and rate, a MIDI or float port's one
//      format; a video watcher the producer's format), then its buffers once it says what it wants (Buffers, Meta params): n buffers in a memfd, each
//      with its metadata (SPA_META_Header, Busy, VideoCrop, ...), a chunk and the data (MemPtr, or MemId of page-aligned data when the port takes only
//      memfds), or without data when the port allocates its own (CAN_ALLOC_BUFFERS: port_use_buffers ALLOC, answered by port_buffers with its memfds);
//      its io area (spa_io_buffers) and command Start;
//   4. every cycle: the server triggers the node (position, status TRIGGERED, a signal on its eventfd) and takes what it played once it marked the
//      activation FINISHED; recording, MIDI and video inputs are filled before the trigger. Playback to the speakers runs while the graph's stream holds
//      less than a few quanta, recording while a quantum has been recorded, MIDI, video and virtual sinks on the server's clock. A node that drives itself
//      (node.driver: video-src, a MIDI driver; the program calls pw_stream_trigger_process) is never triggered: the server takes and fills its buffers as
//      its io areas say. An audio driver follows the server's clock instead (set_io Position to an activation of the server's: it is not driving then).
// Where the data goes: audio to a PlaybackStream (the speakers) or from a RecordStream (the microphone, or the sink's monitor), or into and out of a
// virtual sink the stream names (target.object); MIDI to and from the graph's MidiBus (the synthesizer, keyboards, other programs); video to a
// VideoChannel of the producer's, from which watchers are given the newest frame.
// The records the server and the client share are PipeWire 1.x's layouts (pw_node_activation, spa_io_position/clock, spa_io_buffers, spa_chunk,
// spa_meta_*): their offsets are in the constants below, checked against libpipewire 1.6.9's headers.
#include "PipeWire/NativeServer.hpp"
#include "PipeWire/VideoConvert.hpp"
#include "Utility/ByteStream.hpp"

namespace pipewire {

namespace {

// ClientNode methods (client -> server) and events (server -> client), pipewire/extensions/client-node.h
enum : uint32_t { kCnGetNode = 1, kCnUpdate = 2, kCnPortUpdate = 3, kCnSetActive = 4, kCnEvent = 5, kCnPortBuffers = 6 };
enum : uint32_t { kCnEvTransport = 0, kCnEvSetParam = 1, kCnEvSetIo = 2, kCnEvEvent = 3, kCnEvCommand = 4, kCnEvAddPort = 5, kCnEvRemovePort = 6,
                  kCnEvPortSetParam = 7, kCnEvPortUseBuffers = 8, kCnEvPortSetIo = 9, kCnEvSetActivation = 10, kCnEvPortSetMixInfo = 11 };
constexpr uint32_t kCoreEvBoundId = 5, kCoreEvAddMem = 6, kCoreEvRemoveMem = 7, kCoreEvBoundProps = 8;
constexpr uint32_t kPortUpdateParams = 1, kPortUpdateInfo = 2;

// spa types and ids (spa/param/param.h, format.h, port-config.h, buffers.h, node/command.h, node/io.h, buffer/buffer.h, buffer/meta.h, control/control.h)
constexpr uint32_t kParamEnumFormat = 3, kParamFormat = 4, kParamPortConfig = 11;
constexpr uint32_t kObjectFormat = 0x40003, kObjectBuffers = 0x40004, kObjectMeta = 0x40005, kObjectPortConfig = 0x40008;
constexpr uint32_t kFormatMediaType = 1, kFormatMediaSubtype = 2, kFormatAudioRate = 0x10003, kFormatAudioChannels = 0x10004;
constexpr uint32_t kFormatVideoFormat = 0x20001, kFormatVideoModifier = 0x20002, kFormatVideoSize = 0x20003, kFormatVideoFramerate = 0x20004;
constexpr uint32_t kPropMandatory = 1u << 3;      // SPA_POD_PROP_FLAG_MANDATORY
constexpr uint32_t kSpaBGRx = 8, kSpaBGRA = 12;      // (SPA_VIDEO_FORMAT_*: what a GPU image of the server holds)
constexpr uint32_t kMediaAudio = 1, kMediaVideo = 2, kMediaApplication = 6, kSubtypeRaw = 1, kSubtypeDsp = 2, kSubtypeControl = 0x60001;
constexpr uint32_t kPortConfigDirection = 1, kPortConfigMode = 2, kPortConfigMonitor = 3, kPortConfigControl = 4, kPortConfigFormat = 5;
constexpr uint32_t kModePassthrough = 1, kModeConvert = 2;
constexpr uint32_t kCommandNode = 0x30002, kCommandPause = 1, kCommandStart = 2;
constexpr uint32_t kIoBuffers = 1, kIoClock = 3, kIoPosition = 7;
constexpr uint32_t kIoClockSize = 160, kIoPositionSize = 1688;
constexpr uint32_t kDataMemPtr = 1, kDataMemFd = 2, kDataDmaBuf = 3, kDataMemId = 4, kDataReadWrite = 1 | 2;
constexpr uint32_t kMemReadWrite = 1 | 2;
constexpr uint32_t kDataFlagReadable = 1, kDataFlagWritable = 2;      // SPA_DATA_FLAG_*
constexpr uint32_t kBuffersBuffers = 1, kBuffersBlocks = 2, kBuffersSize = 3, kBuffersStride = 4, kBuffersDataType = 6, kBuffersMetaType = 7;
constexpr uint32_t kMetaParamType = 1, kMetaParamSize = 2, kMetaParamFeatures = 3;
constexpr uint32_t kMetaHeader = 1, kMetaBusy = 7, kMetaSyncTimeline = 9;
constexpr uint32_t kSyncTimelineRelease = 1;      // SPA_META_FEATURE_SYNC_TIMELINE_RELEASE
constexpr uint32_t kMetaSyncReleaseFeature = kMetaSyncTimeline << 16 | kSyncTimelineRelease;      // (SPA_META_TYPE_FEATURES: a meta of no size)
constexpr uint32_t kSyncUnscheduledRelease = 1;   // SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE
constexpr uint32_t kDataSyncObj = 5;              // SPA_DATA_SyncObj
constexpr uint32_t kControlMidi = 2, kControlUmp = 4;
constexpr uint64_t kPortCanAlloc = 1u << 2;
constexpr int32_t kStatusNeedData = 1, kStatusHaveData = 2;
constexpr uint32_t kIdInvalid = 0xFFFFFFFFu;
constexpr int32_t kEINVAL = 22, kENOMEM = 12, kENOTSUP = 95;

// pw_node_activation (src/pipewire/private.h, PW_VERSION_NODE_ACTIVATION 1) and the spa_io_position inside it
constexpr uint64_t kActivationSize = 2312;
constexpr uint32_t kActNotTriggered = 0, kActTriggered = 1, kActFinished = 3, kActInactive = 4;
constexpr uint64_t kActStatus = 0, kActRequired = 12, kActSignalTime = 32, kActServerVersion = 544, kActActiveDriver = 548, kActDriver = 552,
                   kActPosition = 560;
constexpr uint64_t kPosClock = 0, kPosOffset = 200, kPosState = 208, kPosSegmentCount = 212, kPosSegments = 216;
constexpr uint64_t kClockId = 4, kClockName = 8, kClockNsec = 72, kClockRate = 80, kClockPosition = 88, kClockDuration = 96, kClockRateDiff = 112,
                   kClockNextNsec = 120, kClockTargetRate = 128, kClockTargetDuration = 136, kClockCycle = 148;
constexpr uint64_t kSegmentRate = 24;
constexpr uint32_t kPositionRunning = 2;

// the buffers
constexpr uint32_t kMinQuantum = 64, kMaxChannels = 2, kMinRate = 8000, kMaxRate = 192000;      // (an audio port is F32 at its stream's rate, 1 or 2 channels)
constexpr uint32_t kBuffers = 3, kMaxBuffers = 16, kMaxFrames = 8192;
constexpr uint32_t kMaxDataSize = 1u << 20;              // the most an audio or MIDI buffer's data may be (what the port asks for, within reason)
constexpr uint32_t kMaxVideoData = 64u << 20;            // a video buffer's
constexpr uint32_t kMidiData = 16384;                    // a MIDI buffer's data when the port does not say
constexpr uint32_t kMaxMetaSize = 1u << 20;              // one meta's (a cursor's bitmap can be big)
constexpr uint32_t kPage = 4096;
constexpr uint32_t kAheadFrames = 3072;      // what playback keeps queued in the graph at least (and two cycles' worth)
constexpr uint32_t kTimeoutTicks = 100;      // a cycle that never finishes is given up after this many ticks
constexpr uint64_t kTickNs = 5000000;        // (without a clock: a tick is 5 ms)

// ---- the shared records ------------------------------------------------------------------------------------------------------------------------------
// The client maps the same memory and changes it while the server reads it, so fields are read and written with atomic loads and stores. Every access
// goes through field_at(): it checks that the field lies inside the block (bounds) and is aligned to its size (the blocks are page-aligned and every
// offset above is a multiple of the field's size, so a misaligned one is a bug, refused), and it is the only place the bytes become a typed field.
template <class T>
T* field_at(ByteView memory, uint64_t offset) {
  if (memory.size() < sizeof(T) || offset > memory.size() - sizeof(T)) return nullptr;
  uint8_t* at = memory.data() + offset;
  if (reinterpret_cast<uintptr_t>(at) % sizeof(T) != 0) return nullptr;
  return reinterpret_cast<T*>(at);
}
template <class T>
T load(ByteView memory, uint64_t offset) {
  T* f = field_at<T>(memory, offset);
  return f ? __atomic_load_n(f, __ATOMIC_ACQUIRE) : T{};
}
template <class T>
void store(ByteView memory, uint64_t offset, T value) {
  if (T* f = field_at<T>(memory, offset)) __atomic_store_n(f, value, __ATOMIC_RELEASE);
}
// The field becomes `desired` if it is `expected`; true if it was.
template <class T>
bool exchange_if(ByteView memory, uint64_t offset, T expected, T desired) {
  T* f = field_at<T>(memory, offset);
  return f && __atomic_compare_exchange_n(f, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
void store_double(ByteView memory, uint64_t offset, double value) { store<uint64_t>(memory, offset, __builtin_bit_cast(uint64_t, value)); }
// A little-endian float sample of a view (the bytes of a stream, the virtual sinks' rings: not shared with a client while being mixed).
float sample_at(ConstByteView v, uint64_t offset) {
  ByteReader r(v.subview(offset, 4));
  return __builtin_bit_cast(float, r.le32());
}
void put_sample(ByteView v, uint64_t offset, float value) {
  ByteWriter w(v.subview(offset, 4));
  w.le32(__builtin_bit_cast(uint32_t, value));
}

// A part of `memory`, or empty when it does not fit (sizes from the client are not trusted).
ByteView part(ByteView memory, uint64_t offset, uint64_t size) {
  ByteView out;
  return memory.try_subview(Slice(offset, size), &out) ? out : ByteView();
}

uint64_t frames_ns(uint64_t frames, uint32_t rate) { return rate ? frames * 1000000000ull / rate : 0; }
uint32_t round_up(uint32_t v, uint32_t to) { return (v + to - 1) / to * to; }      // (callers keep v far below 2^32)

// "N/D" (node.latency, node.force-quantum's "N"): true with both numbers, each at most 2^31.
bool parse_fraction(StringView text, uint32_t* num, uint32_t* den) {
  uint64_t parts[2] = {0, 1};
  uint32_t which = 0;
  bool digits = false;
  for (uint64_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '/' && which == 0 && digits) { which = 1; parts[1] = 0; digits = false; continue; }
    if (ch < '0' || ch > '9') return false;
    parts[which] = parts[which] * 10 + uint64_t(ch - '0');
    if (parts[which] > 0x80000000ull) return false;
    digits = true;
  }
  if (!digits || parts[1] == 0) return false;
  *num = uint32_t(parts[0]);
  *den = uint32_t(parts[1]);
  return true;
}

uint32_t parse_uint(StringView text, uint32_t fallback) {
  uint32_t num = 0, den = 1;
  return parse_fraction(text, &num, &den) && den == 1 ? num : fallback;
}

// Reads the inline dict of an update (Int n, then n key/value strings) into `out`.
bool read_inline_dict(pod::Parser& p, Properties* out) {
  int32_t n = 0;
  if (!p.int32(&n) || n < 0 || n > static_cast<int32_t>(Properties::kMaxItems)) return false;
  for (int32_t i = 0; i < n; ++i) {
    StringView k, v;
    if (!p.string(&k) || !p.string(&v)) return false;
    if (!k.empty() && out) out->set(k, v);
  }
  return true;
}

void push_empty_dict(pod::Builder& b) {
  const uint64_t f = b.begin_struct();
  b.int32(0);
  b.end(f);
}

// An Int property of a param, or a Choice of Ints' default (its first value).
bool int_or_default(const pod::Value& v, uint32_t* out) {
  int32_t i = 0;
  if (v.get_int(&i)) {
    *out = static_cast<uint32_t>(i);
    return true;
  }
  pod::ArrayReader a;
  return v.type == pod::kChoice && a.open(v) && a.child_type() == pod::kInt && a.get32(0, out);
}

// An Id property, or a Choice of Ids' default.
bool id_or_default(const pod::Value& v, uint32_t* out) {
  if (v.get_id(out)) return true;
  pod::ArrayReader a;
  return v.type == pod::kChoice && a.open(v) && a.child_type() == pod::kId && a.get32(0, out);
}

// A Long property, or a Choice of Longs' default.
bool long_or_default(const pod::Value& v, uint64_t* out) {
  int64_t l = 0;
  if (v.get_long(&l)) {
    *out = static_cast<uint64_t>(l);
    return true;
  }
  if (v.type != pod::kChoice || v.body.size() < 24) return false;
  ByteReader h(v.body.first(24));
  h.le32();
  h.le32();
  const uint32_t child_size = h.le32(), child_type = h.le32();
  if (child_size != 8 || child_type != pod::kLong) return false;
  *out = h.le64();
  return true;
}

// A Rectangle or Fraction (two u32), or a Choice of them's default. (A Choice's body: u32 kind, u32 flags, the child's header, the values.)
bool pair_or_default(const pod::Value& v, uint32_t type, uint32_t* a, uint32_t* b) {
  ConstByteView body = v.body;
  if (v.type == pod::kChoice) {
    if (body.size() < 16) return false;
    ByteReader h(body.first(16));
    h.le32();
    h.le32();
    const uint32_t child_size = h.le32(), child_type = h.le32();
    if (child_type != type || child_size != 8 || body.size() < 24) return false;
    body = body.subview(16, 8);
  } else if (v.type != type || body.size() < 8) {
    return false;
  }
  ByteReader r(body.first(8));
  *a = r.le32();
  *b = r.le32();
  return true;
}

// A port's EnumFormat as the format it is set to: every Choice replaced by its default value (PipeWire's spa_pod_fixate), the id made Format.
bool fixate_object(const pod::Value& enum_format, ByteBuffer* out) {
  pod::ObjectReader o;
  if (!o.open(enum_format) || o.object_type() != kObjectFormat) return false;
  pod::Builder b(*out);
  const uint64_t f = b.begin_object(kObjectFormat, kParamFormat);
  uint32_t key = 0, flags = 0;
  pod::Value v;
  while (o.next(&key, &flags, &v)) {
    b.property(key, 0);
    if (v.type == pod::kChoice) {
      if (v.body.size() < 16) return false;
      ByteReader h(v.body.first(16));
      h.le32();
      h.le32();
      const uint32_t child_size = h.le32(), child_type = h.le32();
      if (child_size == 0 || child_size > 64 || v.body.size() - 16 < child_size) return false;
      // the default as a POD of its own: the child's header, its first value
      ByteBuffer one(out->allocator());
      if (!one.put32(child_size) || !one.put32(child_type) || !one.append(v.body.subview(16, child_size))) return false;
      b.raw(one.view());
    } else {
      b.raw(v.whole);
    }
  }
  b.end(f);
  return b.ok() && o.ok();
}

// What media a port's EnumFormat is, and whether it is PipeWire's DSP flavour (one float channel / float video).
NativeServer::Media media_of(const pod::Value& enum_format, bool* dsp) {
  pod::ObjectReader o;
  *dsp = false;
  if (!o.open(enum_format) || o.object_type() != kObjectFormat) return NativeServer::Media::Unknown;
  uint32_t key = 0, flags = 0, type = 0, subtype = 0;
  pod::Value v;
  while (o.next(&key, &flags, &v)) {
    if (key == kFormatMediaType) id_or_default(v, &type);
    if (key == kFormatMediaSubtype) id_or_default(v, &subtype);
  }
  *dsp = subtype == kSubtypeDsp;
  if (type == kMediaAudio && (subtype == kSubtypeRaw || subtype == kSubtypeDsp)) return NativeServer::Media::Audio;
  if (type == kMediaVideo && (subtype == kSubtypeRaw || subtype == kSubtypeDsp || is_encoded_video_subtype(subtype))) return NativeServer::Media::Video;
  if (type == kMediaApplication && subtype == kSubtypeControl) return NativeServer::Media::Midi;
  return NativeServer::Media::Unknown;
}

// The video format a (fixated) Format object says (raw: its format, size and rate; encoded: its subtype, size and rate).
VideoFormat video_of(const pod::Value& format) {
  VideoFormat out;
  out.subtype = 0;
  pod::ObjectReader o;
  if (!o.open(format)) return out;
  uint32_t key = 0, flags = 0;
  pod::Value v;
  while (o.next(&key, &flags, &v)) {
    if (key == kFormatMediaSubtype) id_or_default(v, &out.subtype);
    if (key == kFormatVideoFormat) id_or_default(v, &out.format);
    if (key == kFormatVideoSize) pair_or_default(v, pod::kRectangle, &out.width, &out.height);
    if (key == kFormatVideoFramerate) pair_or_default(v, pod::kFraction, &out.rate_num, &out.rate_den);
    if (key == kFormatVideoModifier) out.has_modifier = long_or_default(v, &out.modifier);
  }
  if (out.subtype == VideoFormat::kSubtypeDsp && out.format == 0) out.format = 79;      // (SPA_VIDEO_FORMAT_DSP_F32: what dsp video is)
  return out;
}

// What a media class says of a node: its media and its direction.
NativeServer::Media class_media(StringView media_class, StringView media_type) {
  auto has = [](StringView text, const char* word) { return text.find(word) != StringView::npos; };
  if (has(media_class, "Audio") || media_type == StringView("Audio")) return NativeServer::Media::Audio;
  if (has(media_class, "Midi") || media_type == StringView("Midi")) return NativeServer::Media::Midi;
  if (has(media_class, "Video") || media_type == StringView("Video")) return NativeServer::Media::Video;
  return NativeServer::Media::Unknown;
}

uint32_t class_direction(StringView media_class, uint32_t fallback) {
  if (media_class.find("Output") != StringView::npos || media_class.find("Source") != StringView::npos) return kDirectionOutput;
  if (media_class.find("Input") != StringView::npos || media_class.find("Sink") != StringView::npos) return kDirectionInput;
  return fallback;
}

// A plane's stride in a block that holds the planes one after another, the first with stride `stride0` (at least each plane's row).
uint32_t plane_stride(const VideoFormat& v, uint32_t index, uint32_t stride0) {
  const VideoPlane pl = v.plane(index);
  if (index == 0) return stride0 > pl.row_bytes ? stride0 : pl.row_bytes;
  const uint64_t scaled = (uint64_t{stride0} * pl.stride_num + pl.stride_den - 1) / pl.stride_den;
  return scaled > pl.row_bytes ? static_cast<uint32_t>(scaled > 0xFFFFFFFFu ? 0xFFFFFFFFu : scaled) : pl.row_bytes;
}
// The bytes of a raw frame's planes at these strides (the first's `stride0`), one after another.
uint64_t planes_bytes(const VideoFormat& v, uint32_t stride0, uint32_t first, uint32_t count) {
  uint64_t total = 0;
  for (uint32_t i = first; i < first + count && i < v.planes(); ++i) total += uint64_t{plane_stride(v, i, stride0)} * v.plane(i).rows;
  return total;
}

// Whether an EnumFormat names a DRM modifier (the watcher can take DMA-BUFs).
bool enum_has_modifier(ConstByteView enum_format) {
  pod::Parser parser(enum_format);
  pod::Value v;
  pod::ObjectReader o;
  if (!parser.next(&v) || !o.open(v)) return false;
  uint32_t key = 0, flags = 0;
  pod::Value prop;
  while (o.next(&key, &flags, &prop))
    if (key == kFormatVideoModifier) return true;
  return false;
}

// The video formats an EnumFormat accepts (its format property: an Id, or a Choice of Ids, the default first); how many (0: it does not say).
uint32_t enum_video_formats(ConstByteView enum_format, uint32_t* out, uint32_t most) {
  pod::Parser parser(enum_format);
  pod::Value v;
  pod::ObjectReader o;
  if (!parser.next(&v) || !o.open(v)) return 0;
  uint32_t key = 0, flags = 0, n = 0;
  pod::Value prop;
  while (o.next(&key, &flags, &prop)) {
    if (key != kFormatVideoFormat) continue;
    if (prop.type == pod::kId) {
      uint32_t id = 0;
      if (prop.get_id(&id) && most) out[n++] = id;
      return n;
    }
    if (prop.type != pod::kChoice || prop.body.size() < 16) return 0;
    ByteReader h(prop.body.first(16));
    h.le32();
    h.le32();
    const uint32_t child_size = h.le32(), child_type = h.le32();
    if (child_size != 4 || child_type != pod::kId) return 0;
    for (uint64_t at = 16; prop.body.size() - at >= 4 && n < most; at += 4) {
      ByteReader r(prop.body.subview(at, 4));
      const uint32_t id = r.le32();
      bool seen = false;
      for (uint32_t k = 0; k < n; ++k) seen |= out[k] == id;
      if (!seen) out[n++] = id;
    }
    return n;
  }
  return 0;
}

// The size a watcher takes, given its EnumFormat and its producer's size: the producer's when the EnumFormat allows it (it says no size, a range
// that holds it, a list with it), else the EnumFormat's own (a fixed size, a list's first, the producer's clamped into a range).
void watcher_size(ConstByteView enum_format, uint32_t pw, uint32_t ph, uint32_t* w, uint32_t* h) {
  *w = pw;
  *h = ph;
  pod::Parser parser(enum_format);
  pod::Value v;
  pod::ObjectReader o;
  if (!parser.next(&v) || !o.open(v)) return;
  uint32_t key = 0, flags = 0;
  pod::Value prop;
  while (o.next(&key, &flags, &prop)) {
    if (key != kFormatVideoSize) continue;
    auto rect = [](ConstByteView body, uint32_t* rw, uint32_t* rh) {
      if (body.size() < 8) return false;
      ByteReader r(body.first(8));
      *rw = r.le32();
      *rh = r.le32();
      return *rw >= 1 && *rh >= 1 && *rw <= VideoFormat::kMaxSide && *rh <= VideoFormat::kMaxSide;
    };
    uint32_t a = 0, b = 0;
    if (prop.type == pod::kRectangle) {
      if (rect(prop.body, &a, &b)) *w = a, *h = b;
      return;
    }
    if (prop.type != pod::kChoice || prop.body.size() < 24) return;
    ByteReader hdr(prop.body.first(16));
    const uint32_t kind = hdr.le32();
    hdr.le32();
    const uint32_t child_size = hdr.le32(), child_type = hdr.le32();
    if (child_size != 8 || child_type != pod::kRectangle) return;
    const uint64_t count = (prop.body.size() - 16) / 8;
    uint32_t dw = 0, dh = 0;
    if (!rect(prop.body.subview(16, 8), &dw, &dh)) return;
    if (kind == pod::kChoiceNone) {
      *w = dw, *h = dh;
    } else if (kind == pod::kChoiceRange && count >= 3) {
      uint32_t lw = 0, lh = 0, hw = 0, hh = 0;
      if (!rect(prop.body.subview(24, 8), &lw, &lh) || !rect(prop.body.subview(32, 8), &hw, &hh)) return;
      *w = pw < lw ? lw : pw > hw ? hw : pw;
      *h = ph < lh ? lh : ph > hh ? hh : ph;
    } else if (kind == pod::kChoiceEnum) {
      for (uint64_t k = 1; k < count; ++k)
        if (rect(prop.body.subview(16 + 8 * k, 8), &a, &b) && a == pw && b == ph) return;      // (the producer's size is one it lists)
      *w = dw, *h = dh;
    }
    return;
  }
}

// The media subtype an EnumFormat says (its first, for a Choice); 0 when it does not say.
uint32_t enum_subtype(ConstByteView enum_format) {
  pod::Parser parser(enum_format);
  pod::Value v;
  pod::ObjectReader o;
  if (!parser.next(&v) || !o.open(v)) return 0;
  uint32_t key = 0, flags = 0, subtype = 0;
  pod::Value prop;
  while (o.next(&key, &flags, &prop))
    if (key == kFormatMediaSubtype) {
      id_or_default(prop, &subtype);
      return subtype;
    }
  return 0;
}

// The period of a video port's frames (its frame rate), 1 ms .. 1 s; 40 ms when it says none.
uint64_t frame_period_ns(const VideoFormat& v) {
  if (v.rate_num == 0 || v.rate_den == 0) return 40000000ull;
  uint64_t ns = 1000000000ull * v.rate_den / v.rate_num;
  if (ns < 1000000ull) ns = 1000000ull;
  if (ns > 1000000000ull) ns = 1000000000ull;
  return ns;
}

// A MIDI Sequence POD (spa_pod_sequence: unit, pad, then controls of offset, type and a POD) into messages, f(const MidiMessage&, uint32_t offset):
// SPA_CONTROL_Midi's raw bytes (system exclusive whole, made with `allocator`) and SPA_CONTROL_UMP's packets (put together by `ump`, which keeps a
// system exclusive message's pieces from one cycle to the next); other controls are skipped. `offset`: the control's frame in the cycle.
template <class F>
void parse_sequence(ConstByteView pod_bytes, UmpDecoder& ump, allocators::IAllocator& allocator, F&& f) {
  if (pod_bytes.size() < 16) return;
  ByteReader h(pod_bytes.first(8));
  const uint32_t size = h.le32(), type = h.le32();
  if (type != pod::kSequence || size < 8 || size > pod_bytes.size() - 8) return;
  ConstByteView body = pod_bytes.subview(16, size - 8);      // (after unit and pad)
  uint64_t at = 0;
  while (body.size() - at >= 16) {
    ByteReader c(body.subview(at, 16));
    const uint32_t offset = c.le32();
    const uint32_t control = c.le32(), value_size = c.le32();
    c.le32();      // (the value's type: Bytes)
    if (value_size > body.size() - at - 16) return;
    ConstByteView value = body.subview(at + 16, value_size);
    auto at_offset = [&](const MidiMessage& m) { f(m, offset); };
    if (control == kControlMidi) {
      parse_midi_bytes(value, at_offset, &allocator);
    } else if (control == kControlUmp) {
      uint32_t words[4];
      uint64_t i = 0;
      while (value.size() - i >= 4) {
        ByteReader w(value.subview(i, 4));
        words[0] = w.le32();
        const uint32_t n = ump_packet_words(words[0]);
        if (uint64_t{n} * 4 > value.size() - i) break;
        for (uint32_t k = 1; k < n; ++k) {
          ByteReader more(value.subview(i + 4ull * k, 4));
          words[k] = more.le32();
        }
        MidiMessage m;
        if (ump.decode(ArrayView<const uint32_t>(words, n), &m)) f(m, offset);
        i += uint64_t{n} * 4;
      }
    }
    const uint64_t step = 16 + ((uint64_t{value_size} + 7) & ~uint64_t{7});
    if (step > body.size() - at) return;      // (the last control's padding may run past a body whose size is not a multiple of 8)
    at += step;
  }
}

}  // namespace

void build_video_format_param(pod::Builder& b, uint32_t param_id, const VideoFormat& v) {
  const uint64_t f = b.begin_object(kObjectFormat, param_id);
  b.property(kFormatMediaType);
  b.id(kMediaVideo);
  b.property(kFormatMediaSubtype);
  b.id(v.subtype);
  if (!v.encoded()) {
    b.property(kFormatVideoFormat);
    b.id(v.format);
    if (v.has_modifier) {      // (DMA-BUF frames: their layout)
      b.property(kFormatVideoModifier, kPropMandatory);
      b.int64(static_cast<int64_t>(v.modifier));
    }
  }
  if (v.width && v.height) {
    b.property(kFormatVideoSize);
    b.rectangle(v.width, v.height);
  }
  b.property(kFormatVideoFramerate);
  b.fraction(v.rate_num, v.rate_den ? v.rate_den : 1);
  b.end(f);
}

void build_control_format_param(pod::Builder& b, uint32_t param_id) {
  const uint64_t f = b.begin_object(kObjectFormat, param_id);
  b.property(kFormatMediaType);
  b.id(kMediaApplication);
  b.property(kFormatMediaSubtype);
  b.id(kSubtypeControl);
  b.end(f);
}

// ---- setting up ---------------------------------------------------------------------------------------------------------------------------------

uint64_t NativeServer::now_ns() { return clock_ ? clock_() : ticks_ * kTickNs; }

int64_t NativeServer::add_fd(Client& c, const Fd& fd) {
  if (!fd || !c.out_fds.push_back(fd)) return -1;
  return c.message_fds++;
}

NativeServer::ClientNode* NativeServer::client_node(Client& c, uint32_t resource) {
  for (UniquePointer<ClientNode>& n : c.nodes.view())
    if (n->resource == resource) return n.get();
  return nullptr;
}

NativeServer::ClientPort* NativeServer::client_port(ClientNode& n, uint32_t direction, uint32_t port_id) {
  for (UniquePointer<ClientPort>& p : n.ports.view())
    if (p->direction == direction && p->id == port_id) return p.get();
  return nullptr;
}

void NativeServer::send_add_mem(Client& c, uint32_t mem_id, const Fd& fd, uint32_t type, uint32_t flags) {
  const uint64_t m = begin(c, 0, kCoreEvAddMem);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(mem_id));
  b.id(type);
  b.fd(add_fd(c, fd));
  b.int32(static_cast<int32_t>(flags));
  b.end(f);
  end(c, m);
}

// The node's ClientNode set_io: area `id` is `size` bytes at `offset` of memory `mem`.
static void send_node_io(pod::Builder& b, uint32_t id, uint32_t mem, uint64_t offset, uint32_t size) {
  const uint64_t f = b.begin_struct();
  b.id(id);
  b.int32(static_cast<int32_t>(mem));
  b.int32(static_cast<int32_t>(offset));
  b.int32(static_cast<int32_t>(size));
  b.end(f);
}

bool NativeServer::make_graph_node(Client& c, ClientNode& n) {
  if (n.node) return true;
  const StringView media_class = n.props.get("media.class");
  // what it is: its media class says, else its first port with a format
  Media media = class_media(media_class, n.props.get("media.type"));
  if (media == Media::Unknown)
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->media != Media::Unknown) { media = p->media; break; }
  if (media == Media::Unknown) return false;      // (not known yet: the update may say)
  n.media = media;
  n.direction = class_direction(media_class, n.direction);
  // a raw node's audio (a pw_filter): its float output ports are the channels it plays, its float input ports those it records, at the graph's rate
  if (media == Media::Audio && n.raw) {
    n.out_channels = n.in_channels = 0;
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->media == Media::Audio) ++(p->direction == kDirectionOutput ? n.out_channels : n.in_channels);
    if (n.out_channels && !n.in_channels) n.direction = kDirectionOutput;
    if (n.in_channels && !n.out_channels) n.direction = kDirectionInput;
    const uint32_t channels = n.direction == kDirectionOutput ? n.out_channels : n.in_channels;
    n.format = PcmFormat{SampleFormat::F32Le, 48000, channels ? channels : 1u};
  }
  Properties props(allocator_);
  props.copy_from(n.props);
  props.set("client.api", "pipewire");
  if (media == Media::Audio) {
    // into or out of a virtual sink the stream names, else the speakers or the microphone (or the sink's monitor)
    StringView target = n.props.get("target.object");
    if (target.empty()) target = n.props.get("node.target");
    const uint32_t named = graph_.find_node(target);
    if (n.raw && n.out_channels && n.in_channels) {
      // both ways (an effect): one node that records from the microphone and plays to the speakers
      n.node = graph_.create_filter(c.graph_client, PcmFormat{SampleFormat::F32Le, 48000, n.out_channels}, PcmFormat{SampleFormat::F32Le, 48000, n.in_channels},
                                    props, &n.playback, &n.record);
    } else {
      // (a virtual sink: played into its mixer, or recorded from its monitor; the graph's streams convert the format)
      n.virtual_sink = named && graph_.is_virtual_sink(named) ? named : 0;
      if (n.direction == kDirectionOutput)
        n.node = graph_.create_playback(c.graph_client, n.format, props, &n.playback, n.virtual_sink);
      else
        n.node = graph_.create_record(c.graph_client, n.format, props, &n.record, props.get("stream.capture.sink") == StringView("true"), n.virtual_sink);
    }
  } else if (media == Media::Midi) {
    n.node = graph_.create_midi_node(c.graph_client, props, n.direction, n.direction == kDirectionInput ? &n.midi_in : nullptr);
  } else {
    n.node = graph_.create_video_node(c.graph_client, props, n.direction, &n.video);
  }
  if (!n.node) return false;
  // a driver: a video or MIDI one runs its own cycles; an audio one follows the server's clock
  const bool driver = n.props.get("node.driver") == StringView("true");
  n.driving = driver && media != Media::Audio;
  n.follower = driver && media == Media::Audio;
  // the proxy is bound to the node (bound_props, core 4; bound_id before)
  const uint64_t m = begin(c, 0, c.core_version >= 4 ? kCoreEvBoundProps : kCoreEvBoundId);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(n.resource));
  b.int32(static_cast<int32_t>(n.node));
  if (c.core_version >= 4) {
    Properties bound(allocator_);
    graph_.visit(n.node, [&](const GraphObject& o) { bound.copy_from(o.props); });
    const uint64_t d = b.begin_struct();
    b.int32(static_cast<int32_t>(bound.count()));
    for (uint64_t i = 0; i < bound.count(); ++i) {
      b.string(bound.at(i).key.view());
      b.string(bound.at(i).value.view());
    }
    b.end(d);
  }
  b.end(f);
  end(c, m);
  // the activation's clock names the node; the client is told its clock and position again now that the id is in them (a driver is driving when they
  // name the same clock: its own); a driving node's own trigger is required once a cycle
  ByteView a = n.activation->memory();
  store<uint32_t>(a, kActActiveDriver, n.node);
  store<uint32_t>(a, kActDriver, n.node);
  store<uint32_t>(a, kActPosition + kPosClock + kClockId, n.node);
  if (n.driving) store<int32_t>(a, kActRequired, 1);
  {
    const uint64_t io = begin(c, n.resource, kCnEvSetIo);
    pod::Builder iob(c.outbox);
    send_node_io(iob, kIoClock, n.activation_mem, kActPosition + kPosClock, kIoClockSize);
    end(c, io);
  }
  if (n.follower) send_driver_position(c, n);
  else {
    const uint64_t io = begin(c, n.resource, kCnEvSetIo);
    pod::Builder iob(c.outbox);
    send_node_io(iob, kIoPosition, n.activation_mem, kActPosition, kIoPositionSize);
    end(c, io);
  }
  return true;
}

// An audio node.driver stream: its position is an activation of the server's (the server's clock), so it is not driving and is triggered as any other.
void NativeServer::send_driver_position(Client& c, ClientNode& n) {
  if (!n.driver) {
    n.driver = maker_->make_memory(4096, "pipewire-driver");
    if (!n.driver || n.driver->memory().size() < kActivationSize) {
      n.driver = Fd();
      n.follower = false;
      return;
    }
    n.driver_mem = new_mem_id(c);
    send_add_mem(c, n.driver_mem, n.driver);
    ByteView d = n.driver->memory();
    store<uint32_t>(d, kActStatus, kActFinished);
    store<uint32_t>(d, kActServerVersion, 1);
    store<uint32_t>(d, kActPosition + kPosClock + kClockId, graph_.sink_id());      // (the clock is the speakers')
    store<uint32_t>(d, kActPosition + kPosClock + kClockRate, 1);
    store<uint32_t>(d, kActPosition + kPosClock + kClockRate + 4, n.format.rate);
    store<uint64_t>(d, kActPosition + kPosClock + kClockDuration, n.quantum);
    store<uint32_t>(d, kActPosition + kPosClock + kClockTargetRate, 1);
    store<uint32_t>(d, kActPosition + kPosClock + kClockTargetRate + 4, n.format.rate);
    store<uint64_t>(d, kActPosition + kPosClock + kClockTargetDuration, n.quantum);
    store_double(d, kActPosition + kPosClock + kClockRateDiff, 1.0);
    store<uint32_t>(d, kActPosition + kPosState, kPositionRunning);
    store<uint32_t>(d, kActPosition + kPosSegmentCount, 1);
    store_double(d, kActPosition + kPosSegments + kSegmentRate, 1.0);
  }
  const uint64_t io = begin(c, n.resource, kCnEvSetIo);
  pod::Builder b(c.outbox);
  send_node_io(b, kIoPosition, n.driver_mem, kActPosition, kIoPositionSize);
  end(c, io);
}

void NativeServer::create_client_node(Client& c, uint32_t new_id, const Properties& props) {
  auto refuse = [&](int32_t res, const char* why) {
    send_error(c, new_id, c.message_seq, -res, why);
    send_remove_id(c, new_id);
  };
  if (client_node(c, new_id) || resource(c, new_id)) return protocol_error(c, new_id, kEINVAL, "id in use");
  UniquePointer<ClientNode> n = UniquePointer<ClientNode>::make(allocator_, allocator_);
  if (!n) return refuse(kENOMEM, "out of memory");
  n->resource = new_id;
  n->props.copy_from(props);
  n->activation = maker_->make_memory(4096, "pipewire-activation");
  n->io = maker_->make_memory(4096, "pipewire-io");
  n->wake = maker_->make_event();
  n->done = maker_->make_event();
  if (!n->activation || !n->io || !n->wake || !n->done || n->activation->memory().size() < kActivationSize ||
      n->io->memory().size() < 8u * kMaxPorts)
    return refuse(kENOMEM, "no shared memory or eventfds");
  // the activation: not schedulable until the client marks it FINISHED; the server's protocol version; the position of a stopped clock
  ByteView a = n->activation->memory();
  store<uint32_t>(a, kActStatus, kActInactive);
  store<uint32_t>(a, kActServerVersion, 1);
  const uint64_t clock = kActPosition + kPosClock;
  ByteView name = a.subview(clock + kClockName, 64);
  name.fill(0);
  const StringView clock_name("tcos.driver");
  name.first(clock_name.size()).copy_from(clock_name.bytes());
  store<uint32_t>(a, clock + kClockRate, 1);
  store<uint32_t>(a, clock + kClockRate + 4, n->format.rate);
  store<uint64_t>(a, clock + kClockDuration, n->quantum);
  store_double(a, clock + kClockRateDiff, 1.0);
  store<uint32_t>(a, clock + kClockTargetRate, 1);
  store<uint32_t>(a, clock + kClockTargetRate + 4, n->format.rate);
  store<uint64_t>(a, clock + kClockTargetDuration, n->quantum);
  store<uint32_t>(a, kActPosition + kPosState, kPositionRunning);
  store<uint32_t>(a, kActPosition + kPosSegmentCount, 1);
  store_double(a, kActPosition + kPosSegments + kSegmentRate, 1.0);
  store<int64_t>(a, kActPosition + kPosOffset, 0);
  Resource r{new_id, ObjectType::Node, false, 0, 6, 0, {}, true};
  if (!add_resource(c, r)) return refuse(kENOMEM, "too many objects");
  ClientNode& node = *n;
  if (!c.nodes.push_back(util::move(n))) {
    remove_resource(c, new_id);
    return refuse(kENOMEM, "out of memory");
  }
  // (the graph's node is made at set_active or the first port_update after it: the update before them may still say the stream's format, which the
  // node's stream is made in)
  // the activation memory and the transport (the client waits on `wake`; `done` is the other way)
  node.activation_mem = new_mem_id(c);
  send_add_mem(c, node.activation_mem, node.activation);
  node.io_mem = new_mem_id(c);
  send_add_mem(c, node.io_mem, node.io);
  const uint64_t m = begin(c, new_id, kCnEvTransport);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.fd(add_fd(c, node.wake));
  b.fd(add_fd(c, node.done));
  b.int32(static_cast<int32_t>(node.activation_mem));
  b.int32(0);
  b.int32(static_cast<int32_t>(kActivationSize));
  b.end(f);
  end(c, m);
}

void NativeServer::destroy_client_node(Client& c, uint32_t resource_id) {
  for (uint64_t i = 0; i < c.nodes.count(); ++i) {
    ClientNode& n = *c.nodes[i];
    if (n.resource != resource_id) continue;
    if (n.node) graph_.destroy_node(n.node, n.playback ? true : false);      // (what it played still plays out)
    c.nodes.remove_at(i);
    return;
  }
}

// ---- the server's messages -----------------------------------------------------------------------------------------------------------------------

void NativeServer::send_port_config(Client& c, ClientNode& n) {
  if (n.activation) {      // (the clock at the stream's rate and cycle, now that its update said them)
    ByteView a = n.activation->memory();
    const uint64_t clock = kActPosition + kPosClock;
    store<uint32_t>(a, clock + kClockRate + 4, n.format.rate);
    store<uint64_t>(a, clock + kClockDuration, n.quantum);
    store<uint32_t>(a, clock + kClockTargetRate + 4, n.format.rate);
    store<uint64_t>(a, clock + kClockTargetDuration, n.quantum);
  }
  const uint64_t m = begin(c, n.resource, kCnEvSetParam);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.id(kParamPortConfig);
  b.int32(0);
  const uint64_t o = b.begin_object(kObjectPortConfig, kParamPortConfig);
  b.property(kPortConfigDirection);
  b.id(n.direction);
  b.property(kPortConfigMode);
  // audio: converted to the port's format by the adapter; video: the stream's own port, in the format it and the server agree on
  b.id(n.media == Media::Video ? kModePassthrough : kModeConvert);
  b.property(kPortConfigMonitor);
  b.boolean(false);
  b.property(kPortConfigControl);
  b.boolean(false);
  if (n.media != Media::Video) {
    b.property(kPortConfigFormat);
    build_format_param(b, kParamFormat, n.format);
  }
  b.end(o);
  b.end(f);
  end(c, m);
  n.config_sent = true;
}

void NativeServer::send_port_param(Client& c, ClientNode& n, ClientPort& p, ConstByteView format) {
  const uint64_t m = begin(c, n.resource, kCnEvPortSetParam);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(p.direction));
  b.int32(static_cast<int32_t>(p.id));
  b.id(kParamFormat);
  b.int32(0);
  b.raw(format);
  b.end(f);
  end(c, m);
  p.format_sent = true;
}

// The Format a port is set to: the adapter's audio format, a watcher's producer's, else its own EnumFormat fixated. False while it cannot be said yet
// (a watcher with no producer).
bool NativeServer::fixate_format(ClientNode& n, ClientPort& p, ByteBuffer* out) {
  pod::Builder b(*out);
  if (p.media == Media::Audio && !p.dsp && !n.raw) {
    build_format_param(b, kParamFormat, n.format);
    p.pcm = n.format;
    return b.ok();
  }
  if (p.media == Media::Video && p.direction == kDirectionInput) {
    if (!n.video && n.node) graph_.link_video(n.node, &n.video);
    if (!n.video) return false;
    const VideoFormat v = n.video->format();
    if (!v.valid()) return false;
    // the producer's format, or (when the watcher cannot take it) the first one it lists that the producer's frames can be converted to
    p.source_video = v;
    p.video = v;
    uint32_t wanted[32];
    const uint32_t listed = v.encoded() ? 0 : enum_video_formats(p.enum_format.view(), wanted, 32);
    // (a raw watcher of an encoded producer of a known size whose frames can be decoded: offered raw, the frames decoded; the first format it lists
    // that can be made, else BGRx)
    IVideoDecoders* decoders = graph_.video_decoders();
    const bool decoded = v.encoded() && v.width && v.height && can_decode_subtype(v.subtype, decoders) &&
                         enum_subtype(p.enum_format.view()) == VideoFormat::kSubtypeRaw;
    if (decoded) {
      const uint32_t raw_listed = enum_video_formats(p.enum_format.view(), wanted, 32);
      p.video.subtype = VideoFormat::kSubtypeRaw;
      p.video.format = 8;
      for (uint32_t k = 0; k < raw_listed; ++k)
        if (can_decode_video(v, wanted[k], decoders)) {
          p.video.format = wanted[k];
          break;
        }
    }
    // the size it takes: scaled to it when that is not the producer's (and the frames can be converted at all)
    if (v.width && v.height && (!v.encoded() ? can_convert_video(v.format, v.format) : decoded)) {
      uint32_t ww = 0, wh = 0;
      watcher_size(p.enum_format.view(), v.width, v.height, &ww, &wh);
      p.video.width = ww;
      p.video.height = wh;
    }
    bool takes = listed == 0;
    for (uint32_t k = 0; k < listed; ++k) takes |= wanted[k] == v.format;
    for (uint32_t k = 0; k < listed && !takes; ++k)
      if (can_convert_video(v.format, wanted[k])) {
        p.video.format = wanted[k];
        takes = true;
      }
    // DMA-BUFs, for a watcher whose EnumFormat names a modifier (it can take them): the producer's own when they can be shared with it (DMA-BUFs in its
    // very format and size), else GPU images of the server's when it can make them - 32-bit RGB (BGRx, BGRA), chosen when the watcher lists one. The
    // Format then says the modifier (linear for the server's images). Otherwise the frames go in memory and the Format says none.
    p.video.has_modifier = false;
    p.video.modifier = 0;
    if (enum_has_modifier(p.enum_format.view())) {
      SharedPointer<VideoShare> share = n.video->share();
      if (share && !share->closed && share->data_type == kDataDmaBuf && p.video == v) {
        p.video.has_modifier = true;
        p.video.modifier = v.has_modifier ? v.modifier : 0;
      } else if (maker_ && maker_->gpu_images() && (!v.encoded() || decoded)) {
        uint32_t all[32];
        const uint32_t count = enum_video_formats(p.enum_format.view(), all, 32);
        bool rgb = p.video.format == kSpaBGRx || p.video.format == kSpaBGRA;
        for (uint32_t k = 0; k < count && !rgb; ++k)
          if ((all[k] == kSpaBGRx || all[k] == kSpaBGRA) && (decoded ? can_decode_video(v, all[k], decoders) : can_convert_video(v.format, all[k]))) {
            p.video.format = all[k];
            rgb = true;
          }
        p.video.has_modifier = rgb;
      }
    }
    build_video_format_param(b, kParamFormat, p.video);
    return b.ok();
  }
  if (p.enum_format.size() == 0) return false;
  pod::Parser parser(p.enum_format.view());
  pod::Value v;
  if (!parser.next(&v) || !fixate_object(v, out)) return false;
  if (p.media == Media::Audio) p.pcm = PcmFormat{SampleFormat::F32Le, n.format.rate, 1};
  if (p.media == Media::Video) {
    pod::Parser fixed(out->view());
    pod::Value fv;
    if (fixed.next(&fv)) p.video = video_of(fv);
    if (n.video && p.direction == kDirectionOutput) n.video->set_format(p.video);
  }
  return true;
}

// A port's buffers: as many as it asks for (2 .. 16), each in its own part of one memfd: its metadata, the chunk, then the data (as big as it asks, at
// least a quantum of the largest for audio; at most 1 MiB, 64 MiB for video), unless the client allocates the data itself.
bool NativeServer::send_port_buffers(Client& c, ClientNode& n, ClientPort& p) {
  // explicit sync: a video producer that allocates, offers a layout for it and asks for SyncTimeline metadata gets that layout - its data blocks and two
  // SyncObj blocks a buffer (its acquire and release timelines) - and the RELEASE feature when it asks (the server signals each release point)
  p.sync = false;
  p.timelines.clear();
  bool sync_meta = false;
  for (uint32_t k = 0; k < p.n_metas; ++k) sync_meta |= p.metas[k].type == kMetaSyncTimeline && p.metas[k].size >= 24;
  if (p.media == Media::Video && p.direction == kDirectionOutput && (p.flags & kPortCanAlloc) && p.sync_layout.offered && p.sync_layout.blocks >= 3 &&
      sync_meta) {
    p.sync = true;
    p.wanted_buffers = p.sync_layout.buffers;
    p.wanted_blocks = p.sync_layout.blocks - 2;
    p.wanted_size = p.sync_layout.size;
    p.wanted_stride = p.sync_layout.stride;
    p.wanted_types = p.sync_layout.types;
    bool has_feature = false;
    for (uint32_t k = 0; k < p.n_metas; ++k) has_feature |= p.metas[k].type == kMetaSyncReleaseFeature;
    if (p.wants_sync_release && !has_feature && p.n_metas < ClientPort::kMaxMetas) p.metas[p.n_metas++] = ClientPort::Meta{kMetaSyncReleaseFeature, 0, 0};
  }
  p.blocks = 1;
  p.n_buffers = p.wanted_buffers >= 1 && p.wanted_buffers <= kMaxBuffers ? (p.wanted_buffers < 2 ? 2 : p.wanted_buffers) : kBuffers;
  uint32_t data = 0;
  switch (p.media) {
    case Media::Audio: {
      const uint32_t frame = p.dsp || n.raw ? 4u : n.frame();
      data = p.wanted_size > kMaxFrames * frame ? p.wanted_size : kMaxFrames * frame;
      if (data > kMaxDataSize) data = kMaxDataSize;
      break;
    }
    case Media::Midi:
      data = p.wanted_size > kMidiData ? p.wanted_size : kMidiData;
      if (data > kMaxDataSize) data = kMaxDataSize;
      break;
    default: {
      // raw: one block a plane when the port asks for as many (else the planes one after another in one); encoded: a frame's bytes as asked, 4 MiB
      uint64_t frame = 0;
      if (p.video.encoded()) {
        frame = p.wanted_size ? p.wanted_size : 4u << 20;
      } else {
        const uint32_t planes = p.video.planes();
        if (planes > 1 && p.wanted_blocks >= planes && p.wanted_blocks <= VideoFormat::kMaxPlanes) p.blocks = p.wanted_blocks;
        const uint32_t stride0 = p.wanted_stride;
        if (p.blocks > 1)
          for (uint32_t i = 0; i < planes; ++i) {
            const uint64_t b = planes_bytes(p.video, stride0, i, 1);
            if (b > frame) frame = b;
          }
        else
          frame = planes_bytes(p.video, stride0, 0, planes);
        if (p.wanted_size > frame) frame = p.wanted_size;
      }
      if (frame == 0) frame = kPage;
      if (frame > kMaxVideoData) return false;
      data = static_cast<uint32_t>(frame);
      break;
    }
  }
  // memory: shared with the client by pointer when it takes that, else by memfd (page-aligned data); its own when it can allocate (an output port)
  const uint32_t types = p.wanted_types;
  p.alloc = (p.flags & kPortCanAlloc) && p.direction == kDirectionOutput;
  p.data_type = types == 0 || (types & (1u << kDataMemPtr)) ? kDataMemPtr : kDataMemId;
  p.leave_share();
  p.share_mems.clear();
  p.gpu_images.clear();
  p.gpu_stride = 0;
  // A watcher of video may be given other memory than the server's: its producer's own buffers when they are shared and it takes them as they are (the
  // producer's format and size; their memory a type it takes: zero-copy), else the server's GPU images when it takes DMA-BUFs only or was offered them.
  SharedPointer<VideoShare> share;
  bool gpu = false;
  if (p.media == Media::Video && p.direction == kDirectionInput && n.video) {
    auto takes = [&](uint32_t type) { return types ? (types & (1u << type)) != 0 : type == kDataDmaBuf && p.video.has_modifier; };
    SharedPointer<VideoShare> s = n.video->share();
    if (s && takes_share(p, *s)) share = s;
    const bool dmabuf_only = types != 0 && !(types & ((1u << kDataMemPtr) | (1u << kDataMemFd))) && (types & (1u << kDataDmaBuf));
    if (!share && (dmabuf_only || (p.video.has_modifier && takes(kDataDmaBuf)))) {
      // (one image a buffer, 32-bit RGB at the watcher's size)
      if ((p.video.format != kSpaBGRx && p.video.format != kSpaBGRA) || p.video.encoded() || !maker_) return false;
      for (uint32_t i = 0; i < p.n_buffers; ++i) {
        uint32_t stride = 0;
        Fd image = maker_->make_gpu_image(p.video.width, p.video.height, &stride);
        if (!image || (p.gpu_stride && stride != p.gpu_stride) || !p.gpu_images.push_back(util::move(image))) {
          p.gpu_images.clear();
          return false;
        }
        p.gpu_stride = stride;
      }
      p.wanted_stride = p.gpu_stride;
      gpu = true;
    }
  }
  if (share) {
    p.n_buffers = share->n_buffers;
    p.blocks = share->blocks;
  }
  if (gpu) p.blocks = 1;
  if (!p.alloc && !share && !gpu && types != 0 && !(types & ((1u << kDataMemPtr) | (1u << kDataMemFd)))) return false;      // (only DMA-BUFs: none here)
  uint32_t at = 0;
  for (uint32_t i = 0; i < p.n_metas; ++i) {
    p.metas[i].offset = at;
    at += round_up(p.metas[i].size, 8);
  }
  p.chunk_offset = at;
  at += 16 * p.blocks;
  if (p.alloc || share || gpu) {      // (no data of the server's: the client's, the producer's, or the GPU images)
    p.data_offset = 0;
    p.data_size = 0;
    p.buffer_stride = round_up(at, 64);
    if (!p.alloc) p.data_type = kDataMemId;
  } else {
    // (each block's data page-aligned when it goes by memfd)
    p.data_offset = p.data_type == kDataMemId ? round_up(at, kPage) : round_up(at, 64);
    p.data_size = round_up(data, p.data_type == kDataMemId ? kPage : 64u);
    const uint64_t stride = uint64_t{p.data_offset} + uint64_t{p.data_size} * p.blocks;
    if (stride > kMaxVideoData + uint64_t{kPage} * 8) return false;
    p.buffer_stride = round_up(static_cast<uint32_t>(stride), p.data_type == kDataMemId ? kPage : 64u);
  }
  const uint64_t total = uint64_t{p.n_buffers} * p.buffer_stride;
  p.buffers = maker_->make_memory(total, "pipewire-buffers");
  if (!p.buffers || p.buffers->memory().size() < total) return false;
  p.buffers_mem = new_mem_id(c);
  send_add_mem(c, p.buffers_mem, p.buffers);
  // (the producer's descriptors, or the GPU images: each block's memory of its own, readable)
  const uint64_t extra = share ? share->fds.count() : p.gpu_images.count();
  for (uint64_t i = 0; i < extra; ++i) {
    const uint32_t mem = new_mem_id(c);
    send_add_mem(c, mem, share ? share->fds[i] : p.gpu_images[i], share ? share->data_type : kDataDmaBuf, 1);
    if (!p.share_mems.push_back(mem)) return false;
  }
  // the port's one link: mix 0 (its peer: the speakers, the microphone, a virtual sink, the MIDI bridge or the node itself)
  uint32_t peer = n.node;
  if (n.media == Media::Audio) peer = n.virtual_sink ? n.virtual_sink : (n.direction == kDirectionOutput ? graph_.sink_id() : graph_.source_id());
  else if (n.media == Media::Midi) peer = graph_.midi_bridge_id();
  {
    const uint64_t m = begin(c, n.resource, kCnEvPortSetMixInfo);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(p.direction));
    b.int32(static_cast<int32_t>(p.id));
    b.int32(0);
    b.int32(static_cast<int32_t>(peer));
    push_empty_dict(b);
    b.end(f);
    end(c, m);
  }
  // the buffers: an output port takes them on the port itself (mix SPA_ID_INVALID), an input port on its mix
  {
    const uint64_t m = begin(c, n.resource, kCnEvPortUseBuffers);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(p.direction));
    b.int32(static_cast<int32_t>(p.id));
    b.int32(static_cast<int32_t>(p.direction == kDirectionOutput ? kIdInvalid : 0));
    b.int32(p.alloc ? 1 : 0);      // (SPA_NODE_BUFFERS_FLAG_ALLOC)
    b.int32(static_cast<int32_t>(p.n_buffers));
    for (uint32_t i = 0; i < p.n_buffers; ++i) {
      b.int32(static_cast<int32_t>(p.buffers_mem));
      b.int32(static_cast<int32_t>(i * p.buffer_stride));
      b.int32(static_cast<int32_t>(p.buffer_stride));
      b.int32(static_cast<int32_t>(p.n_metas));
      for (uint32_t k = 0; k < p.n_metas; ++k) {
        b.id(p.metas[k].type);
        b.int32(static_cast<int32_t>(p.metas[k].size));
      }
      b.int32(static_cast<int32_t>(p.blocks + (p.sync ? 2 : 0)));      // datas (and, explicit sync, the two timelines)
      for (uint32_t k = 0; k < p.blocks + (p.sync ? 2u : 0u); ++k) {
        if (k >= p.blocks) {      // (a SyncObj the client puts its timeline in)
          b.id(1u << kDataSyncObj);
          b.int32(0);
          b.int32(0);
          b.int32(0);
          b.int32(0);
          continue;
        }
        const uint32_t data_at = p.data_offset + k * p.data_size;
        if (p.alloc) {
          // (the data is the client's: the types it may use, as PipeWire says them for an allocating port)
          b.id(types ? types : 1u << kDataMemFd);
          b.int32(0);
          b.int32(0);
          b.int32(0);
          b.int32(0);
        } else if (share) {
          const uint64_t index = uint64_t{i} * p.blocks + k;
          b.id(kDataMemId);
          b.int32(static_cast<int32_t>(p.share_mems[index]));      // (the producer's memory, as it gave it)
          b.int32(static_cast<int32_t>((share->flags[index] | kDataFlagReadable) & ~kDataFlagWritable));      // (the watcher only reads it)
          b.int32(static_cast<int32_t>(share->mapoffsets[index]));
          b.int32(static_cast<int32_t>(share->maxsizes[index]));
        } else if (gpu) {
          b.id(kDataMemId);
          b.int32(static_cast<int32_t>(p.share_mems[i]));
          b.int32(static_cast<int32_t>(kDataFlagReadable));
          b.int32(0);
          b.int32(static_cast<int32_t>(uint64_t{p.gpu_stride} * p.video.height));
        } else if (p.data_type == kDataMemPtr) {
          b.id(kDataMemPtr);
          b.int32(static_cast<int32_t>(data_at));      // (MemPtr: the offset in the buffer's part)
          b.int32(static_cast<int32_t>(kDataReadWrite));
          b.int32(0);
          b.int32(static_cast<int32_t>(p.data_size));
        } else {
          b.id(kDataMemId);
          b.int32(static_cast<int32_t>(p.buffers_mem));      // (MemId: the memory, mapped at mapoffset)
          b.int32(static_cast<int32_t>(kDataReadWrite));
          b.int32(static_cast<int32_t>(i * p.buffer_stride + data_at));
          b.int32(static_cast<int32_t>(p.data_size));
        }
      }
    }
    b.end(f);
    end(c, m);
  }
  // the metadata's starting values: nothing busy, no header yet
  ByteView bufs = p.buffers->memory();
  for (uint32_t i = 0; i < p.n_buffers; ++i)
    for (uint32_t k = 0; k < p.n_metas; ++k) part(bufs, uint64_t{i} * p.buffer_stride + p.metas[k].offset, p.metas[k].size).fill(0);
  // the port's io area (spa_io_buffers) on mix 0, 8 bytes of the node's io memory
  {
    ByteView io = n.io->memory();
    store<int32_t>(io, 8ull * p.index, p.direction == kDirectionOutput ? kStatusNeedData : 0);
    store<uint32_t>(io, 8ull * p.index + 4, kIdInvalid);
    const uint64_t m = begin(c, n.resource, kCnEvPortSetIo);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(p.direction));
    b.int32(static_cast<int32_t>(p.id));
    b.int32(0);
    b.id(kIoBuffers);
    b.int32(static_cast<int32_t>(n.io_mem));
    b.int32(static_cast<int32_t>(8 * p.index));
    b.int32(8);
    b.end(f);
    end(c, m);
  }
  p.buffers_sent = true;
  p.data_ready = !p.alloc;
  p.client_data.clear();
  if (share) {      // (the watcher holds nothing of them yet)
    p.share = share;
    ++share->attached;
  }
  return true;
}

void NativeServer::send_command(Client& c, ClientNode& n, uint32_t command) {
  const uint64_t m = begin(c, n.resource, kCnEvCommand);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  const uint64_t o = b.begin_object(kCommandNode, command);
  b.end(o);
  b.end(f);
  end(c, m);
}

// What can be done for the node's ports now: their formats, then their buffers once they said what they want; Start once every port with media has
// its buffers (and an allocating port its data).
void NativeServer::negotiate(Client& c, ClientNode& n) {
  if (!n.node || !n.active) return;
  if (n.renegotiate) renegotiate(c, n);
  for (UniquePointer<ClientPort>& pp : n.ports.view()) {
    ClientPort& p = *pp;
    if (p.media == Media::Unknown) continue;
    if (!p.format_sent) {
      ByteBuffer format(allocator_);
      if (fixate_format(n, p, &format)) send_port_param(c, n, p, format.view());
      continue;
    }
    if (!p.buffers_sent && p.wants_alloc) {
      if (!send_port_buffers(c, n, p)) {
        protocol_error(c, n.resource, kENOTSUP, "no buffers for this port (out of memory, too big, or only DMA-BUFs)");
        p.wants_alloc = false;
      }
    }
  }
  maybe_start(c, n);
}

void NativeServer::maybe_start(Client& c, ClientNode& n) {
  if (n.started || !n.active || !n.node) return;
  bool any = false;
  for (UniquePointer<ClientPort>& p : n.ports.view()) {
    if (p->media == Media::Unknown) continue;
    if (!p->buffers_sent || !p->data_ready) return;
    any = true;
  }
  if (!any) return;
  send_command(c, n, kCommandStart);
  n.started = true;
  n.next_cycle_ns = 0;
}

// ---- the client's methods ------------------------------------------------------------------------------------------------------------------------

void NativeServer::port_update(Client& c, ClientNode& n, uint32_t direction, uint32_t port_id, int32_t change_mask, pod::Parser& args, uint32_t n_params) {
  ClientPort* p = client_port(n, direction, port_id);
  // the params (each kind replaces what the port said of it before)
  bool has_enum = false, has_buffers = false, has_meta = false, wants_release = false;
  ByteBuffer enum_format(allocator_);
  uint32_t wanted_buffers = 0, wanted_blocks = 0, wanted_size = 0, wanted_stride = 0, wanted_types = 0;
  ClientPort::SyncLayout sync_layout;
  ClientPort::Meta metas[ClientPort::kMaxMetas];
  uint32_t n_metas = 0;
  for (uint32_t i = 0; i < n_params; ++i) {
    pod::Value v;
    if (!args.pod(&v)) { protocol_error(c, n.resource, kEINVAL, "invalid port_update"); return; }
    pod::ObjectReader o;
    if (!o.open(v)) continue;
    if (o.object_type() == kObjectFormat && o.object_id() == kParamEnumFormat && !has_enum) {
      has_enum = enum_format.append(v.whole);
    } else if (o.object_type() == kObjectBuffers) {
      // how many buffers, how big their data and its stride, which memory (each a choice: its default; the data types a mask). A layout that
      // depends on SyncTimeline metadata (explicit sync) is kept apart: the first other one is the port's usual layout.
      uint32_t key = 0, flags = 0, b_buffers = 0, b_blocks = 0, b_size = 0, b_stride = 0, b_types = 0, b_meta = 0;
      pod::Value prop;
      while (o.next(&key, &flags, &prop)) {
        uint32_t value = 0;
        if (key == kBuffersBuffers && int_or_default(prop, &value)) b_buffers = value;
        if (key == kBuffersBlocks && int_or_default(prop, &value)) b_blocks = value;
        if (key == kBuffersSize && int_or_default(prop, &value)) b_size = value;
        if (key == kBuffersStride && int_or_default(prop, &value)) b_stride = value;
        if (key == kBuffersDataType && int_or_default(prop, &value)) b_types = value;
        if (key == kBuffersMetaType && int_or_default(prop, &value)) b_meta = value;
      }
      if (b_meta & (1u << kMetaSyncTimeline)) {
        if (!sync_layout.offered) sync_layout = ClientPort::SyncLayout{true, b_buffers, b_blocks, b_size, b_stride, b_types};
      } else if (!has_buffers) {
        has_buffers = true;
        wanted_buffers = b_buffers;
        wanted_blocks = b_blocks;
        wanted_size = b_size;
        wanted_stride = b_stride;
        wanted_types = b_types;
      }
    } else if (o.object_type() == kObjectMeta) {
      has_meta = true;
      uint32_t key = 0, flags = 0, type = 0, size = 0;
      pod::Value prop;
      uint32_t features = 0;
      while (o.next(&key, &flags, &prop)) {
        if (key == kMetaParamType) id_or_default(prop, &type);
        if (key == kMetaParamSize) int_or_default(prop, &size);
        if (key == kMetaParamFeatures) int_or_default(prop, &features);
      }
      if (type == kMetaSyncTimeline && (features & kSyncTimelineRelease)) wants_release = true;
      bool known = false;
      for (uint32_t k = 0; k < n_metas; ++k) known |= metas[k].type == type;
      if (type != 0 && size != 0 && size <= kMaxMetaSize && !known && n_metas < ClientPort::kMaxMetas) metas[n_metas++] = ClientPort::Meta{type, size, 0};
    }
  }
  pod::Value info;
  if (!args.pod(&info)) { protocol_error(c, n.resource, kEINVAL, "invalid port_update"); return; }
  if (change_mask == 0 && n_params == 0 && info.is_none()) {      // the port went
    for (uint64_t i = 0; i < n.ports.count(); ++i)
      if (n.ports[i].get() == p) {
        n.ports.remove_at(i);
        break;
      }
    return;
  }
  // the port's info: its flags and properties
  uint64_t port_flags = 0;
  bool has_flags = false, ump = false;
  if (info.type == pod::kStruct) {
    pod::Parser q(info.body, args.depth() + 1);
    int64_t mask = 0, flags = 0;
    int32_t num = 0, den = 0;
    Properties props(allocator_);
    if (!q.int64(&mask) || !q.int64(&flags) || !q.int32(&num) || !q.int32(&den) || !read_inline_dict(q, &props)) {
      protocol_error(c, n.resource, kEINVAL, "invalid port info");
      return;
    }
    port_flags = static_cast<uint64_t>(flags);
    has_flags = true;
    ump = props.get("control.ump") == StringView("true");
  }
  if (!p) {
    if (n.ports.count() >= kMaxPorts) { protocol_error(c, n.resource, kENOMEM, "too many ports"); return; }
    UniquePointer<ClientPort> made = UniquePointer<ClientPort>::make(allocator_, allocator_);
    if (!made) { protocol_error(c, n.resource, kENOMEM, "out of memory"); return; }
    made->direction = direction;
    made->id = port_id;
    uint32_t index = 0;      // (the lowest io slot free)
    for (bool taken = true; taken && index < kMaxPorts;) {
      taken = false;
      for (UniquePointer<ClientPort>& q : n.ports.view()) taken |= q->index == index;
      if (taken) ++index;
    }
    made->index = index;
    p = made.get();
    if (!n.ports.push_back(util::move(made))) { protocol_error(c, n.resource, kENOMEM, "out of memory"); return; }
  }
  if (has_flags) p->flags = port_flags;
  if (has_flags) p->ump = ump;
  if (has_enum && !p->format_sent) {
    p->enum_format.clear();
    p->enum_format.append(enum_format.view());
    pod::Parser q(p->enum_format.view());
    pod::Value v;
    if (q.next(&v)) p->media = media_of(v, &p->dsp);
  }
  if (has_enum && p->format_sent && p->media == Media::Video && p->direction == kDirectionOutput && !p->renegotiate) {
    // a producer that says a new EnumFormat (pw_stream_update_params): when it fixates to another format, its port is negotiated again
    ByteBuffer fixed(allocator_);
    pod::Parser q(enum_format.view());
    pod::Value v, fv;
    if (q.next(&v) && media_of(v, &p->dsp) == Media::Video && fixate_object(v, &fixed)) {
      pod::Parser fq(fixed.view());
      if (fq.next(&fv)) {
        const VideoFormat changed = video_of(fv);
        if (changed.valid() && !(changed == p->video)) {
          p->enum_format.clear();
          p->enum_format.append(enum_format.view());
          p->renegotiate = n.renegotiate = true;
        }
      }
    }
  }
  // (what it wants of its buffers is taken only before they are made: the server's layout must stay the one the program was sent)
  if ((change_mask & kPortUpdateParams) && p->format_sent && !p->renegotiate && !p->buffers_sent) {
    if (has_buffers) {
      p->wanted_buffers = wanted_buffers;
      p->wanted_blocks = wanted_blocks;
      p->wanted_size = wanted_size;
      p->wanted_stride = wanted_stride;
      p->wanted_types = wanted_types;
    }
    if (has_buffers || sync_layout.offered) p->sync_layout = sync_layout;
    if (!has_buffers && sync_layout.offered) {      // (only a sync layout: its data blocks are the usual layout too)
      p->wanted_buffers = sync_layout.buffers;
      p->wanted_blocks = sync_layout.blocks > 2 ? sync_layout.blocks - 2 : 1;
      p->wanted_size = sync_layout.size;
      p->wanted_stride = sync_layout.stride;
      p->wanted_types = sync_layout.types;
    }
    if (has_meta) p->wants_sync_release = wants_release;
    if (has_meta || has_buffers) {
      p->n_metas = n_metas;
      for (uint32_t k = 0; k < n_metas; ++k) p->metas[k] = metas[k];
    }
    // the port took its format and says what it wants: its buffers can be made (a port with no Buffers param gets the usual ones)
    if (!p->buffers_sent && (has_buffers || n_params > 0)) p->wants_alloc = true;
  }
  if (!n.config_sent && !n.active) n.raw = true;      // (ports before the node is active: not an adapter)
  // a configured adapter's port in its direction carries the adapter's media, whether or not it said an EnumFormat yet
  if (p->media == Media::Unknown && !n.raw && n.config_sent && direction == n.direction) p->media = n.media;
  if (n.active && !n.node) make_graph_node(c, n);
  negotiate(c, n);
}

void NativeServer::port_buffers(Client& c, ClientNode& n, pod::Parser& args) {
  int32_t direction = 0, port_id = 0, mix = 0, count = 0;
  if (!args.int32(&direction) || !args.int32(&port_id) || !args.int32(&mix) || !args.int32(&count) || count < 0 || count > int32_t(kMaxBuffers)) {
    protocol_error(c, n.resource, kEINVAL, "invalid port_buffers");
    return;
  }
  ClientPort* p = client_port(n, static_cast<uint32_t>(direction), static_cast<uint32_t>(port_id));
  if (!p || !p->alloc || !p->buffers_sent || uint32_t(count) != p->n_buffers) {
    protocol_error(c, n.resource, kEINVAL, "port_buffers for buffers the server did not ask the port to allocate");
    return;
  }
  List<ClientPort::ClientData> datas(allocator_);
  List<Fd> timelines(allocator_);
  for (int32_t i = 0; i < count; ++i) {
    int32_t n_datas = 0;
    if (!args.int32(&n_datas) || n_datas < int32_t(p->blocks) || n_datas > int32_t(VideoFormat::kMaxPlanes + 2)) {
      protocol_error(c, n.resource, kEINVAL, "invalid port_buffers");
      return;
    }
    for (int32_t k = 0; k < n_datas; ++k) {
      uint32_t type = 0;
      int32_t flags = 0, mapoffset = 0, maxsize = 0;
      pod::Value fdv;
      int64_t index = -1;
      if (!args.id(&type) || !args.pod(&fdv) || !fdv.get_fd(&index) || !args.int32(&flags) || !args.int32(&mapoffset) || !args.int32(&maxsize)) {
        protocol_error(c, n.resource, kEINVAL, "invalid port_buffers");
        return;
      }
      if (uint32_t(k) >= p->blocks) {      // (the blocks the server asked for are used; explicit sync: the two after them are its timelines)
        if (p->sync && uint32_t(k) < p->blocks + 2 && type == kDataSyncObj && index >= 0 && uint64_t(index) < c.message_in_fds.count())
          timelines.push_back(c.message_in_fds[uint64_t(index)]);
        continue;
      }
      if (type != kDataMemFd && type != kDataDmaBuf) { protocol_error(c, n.resource, kENOTSUP, "only memfd and DMA-BUF buffers can be shared with the server"); return; }
      if (index < 0 || uint64_t(index) >= c.message_in_fds.count()) { protocol_error(c, n.resource, kEINVAL, "port_buffers without its descriptor"); return; }
      ClientPort::ClientData d;
      d.fd = c.message_in_fds[uint64_t(index)];
      // (a DMA-BUF must be a GPU image the server can read: on this system an export of /dev/dxg)
      if (type == kDataDmaBuf && (!d.fd || !d.fd->gpu())) { protocol_error(c, n.resource, kENOTSUP, "a DMA-BUF the server cannot read (not an export of /dev/dxg)"); return; }
      d.gpu = type == kDataDmaBuf;
      d.mapoffset = static_cast<uint32_t>(mapoffset);
      d.maxsize = static_cast<uint32_t>(maxsize);
      d.flags = static_cast<uint32_t>(flags);
      const uint64_t size = d.fd ? d.fd->memory().size() : 0;
      if (mapoffset < 0 || maxsize <= 0 || uint64_t(uint32_t(maxsize)) > size || uint64_t(uint32_t(mapoffset)) > size - uint32_t(maxsize)) {
        protocol_error(c, n.resource, kEINVAL, "a buffer outside its memfd");
        return;
      }
      if (!datas.push_back(util::move(d))) { protocol_error(c, n.resource, kENOMEM, "out of memory"); return; }
    }
  }
  p->client_data.clear();
  for (ClientPort::ClientData& d : datas) p->client_data.push_back(util::move(d));
  // (explicit sync only with both timelines of every buffer; without them the producer is not promised releases)
  p->timelines.clear();
  if (p->sync && timelines.count() == 2ull * uint32_t(count))
    for (Fd& t : timelines) p->timelines.push_back(util::move(t));
  else
    p->sync = false;
  p->data_size = p->client_data.count() ? p->client_data[0].maxsize : 0;
  p->data_ready = p->client_data.count() == uint64_t{p->n_buffers} * p->blocks;
  if (p->media == Media::Video && p->direction == kDirectionOutput) share_buffers(n, *p);
  maybe_start(c, n);
}

bool NativeServer::takes_share(const ClientPort& p, const VideoShare& share) {
  if (share.closed || !(p.video == p.source_video) || share.n_buffers < 1 || share.n_buffers > VideoShare::kMaxBuffers || share.blocks < 1 ||
      share.blocks > VideoFormat::kMaxPlanes)
    return false;
  return p.wanted_types ? (p.wanted_types & (1u << share.data_type)) != 0 : share.data_type == kDataDmaBuf && p.video.has_modifier;
}

bool NativeServer::watcher_share_changed(const ClientPort& p, VideoChannel& channel) {
  SharedPointer<VideoShare> now = channel.share();
  if (p.share) return p.share->closed || now.get() != p.share.get();
  return now && takes_share(p, *now);
}

// A video producer's own buffers (DMA-BUFs or memfds it allocated, all of one kind): shared through its channel with the watchers that can take them as
// they are (they are negotiated again to be given them).
void NativeServer::share_buffers(ClientNode& n, ClientPort& p) {
  p.leave_share();
  if (!n.video) return;
  n.video->set_share(SharedPointer<VideoShare>());
  if (!p.alloc || !p.data_ready || p.n_buffers == 0 || p.n_buffers > VideoShare::kMaxBuffers || p.blocks > VideoFormat::kMaxPlanes) return;
  uint32_t gpu = 0;
  for (const ClientPort::ClientData& d : p.client_data.view()) gpu += d.gpu;
  if (gpu != 0 && gpu != p.client_data.count()) return;      // (DMA-BUFs and memfds mixed: not shared)
  SharedPointer<VideoShare> share = SharedPointer<VideoShare>::make(allocator_, allocator_);
  if (!share) return;
  share->n_buffers = p.n_buffers;
  share->blocks = p.blocks;
  share->data_type = gpu ? kDataDmaBuf : kDataMemFd;
  for (const ClientPort::ClientData& d : p.client_data.view())
    if (!share->fds.push_back(d.fd) || !share->mapoffsets.push_back(d.mapoffset) || !share->maxsizes.push_back(d.maxsize) || !share->flags.push_back(d.flags))
      return;
  p.share = share;
  n.video->set_share(util::move(share));
}

void NativeServer::client_node_method(Client& c, Resource& r, uint32_t opcode, pod::Parser& args) {
  ClientNode* n = client_node(c, r.id);
  if (!n) return;
  switch (opcode) {
    case kCnUpdate: {
      int32_t change_mask = 0, n_params = 0;
      if (!args.int32(&change_mask) || !args.int32(&n_params) || n_params < 0 || n_params > 256) {
        protocol_error(c, r.id, kEINVAL, "invalid update");
        return;
      }
      for (int32_t i = 0; i < n_params; ++i) {
        pod::Value v;
        if (!args.pod(&v)) { protocol_error(c, r.id, kEINVAL, "invalid update"); return; }
        // the stream's EnumFormat or Format: its rate and channels become the port's (until the graph node is made: a stream's format is fixed then)
        pod::ObjectReader o;
        if (n->node || !o.open(v) || o.object_type() != kObjectFormat || (o.object_id() != kParamEnumFormat && o.object_id() != kParamFormat)) continue;
        uint32_t key = 0, flags = 0, rate = 0, channels = 0;
        pod::Value prop;
        while (o.next(&key, &flags, &prop)) {
          if (key == kFormatAudioRate) int_or_default(prop, &rate);
          if (key == kFormatAudioChannels) int_or_default(prop, &channels);
        }
        if (rate >= kMinRate && rate <= kMaxRate) n->format.rate = rate;
        if (channels >= 1 && channels <= kMaxChannels) n->format.channels = channels;
        set_quantum(*n);      // (node.latency's frames are at its own rate)
      }
      pod::Value info;
      if (!args.pod(&info)) { protocol_error(c, r.id, kEINVAL, "invalid update"); return; }
      if (info.type == pod::kStruct) {
        pod::Parser p(info.body, args.depth() + 1);
        int32_t max_in = 0, max_out = 0;
        int64_t mask = 0, flags = 0;
        Properties props(allocator_);
        if (!p.int32(&max_in) || !p.int32(&max_out) || !p.int64(&mask) || !p.int64(&flags) || !read_inline_dict(p, &props)) {
          protocol_error(c, r.id, kEINVAL, "invalid node info");
          return;
        }
        if (props.count()) {
          n->props.update(props, Properties::Mode::kReplace);
          set_quantum(*n);
          if (n->node) graph_.update_node(n->node, props, Properties::Mode::kReplace);
        }
      }
      return;
    }
    case kCnPortUpdate: {
      int32_t direction = 0, port_id = 0, change_mask = 0, n_params = 0;
      if (!args.int32(&direction) || !args.int32(&port_id) || !args.int32(&change_mask) || !args.int32(&n_params) || n_params < 0 || n_params > 256 ||
          (direction != 0 && direction != 1) || port_id < 0 || port_id > 0xFFFF) {
        protocol_error(c, r.id, kEINVAL, "invalid port_update");
        return;
      }
      port_update(c, *n, uint32_t(direction), uint32_t(port_id), change_mask, args, uint32_t(n_params));
      return;
    }
    case kCnSetActive: {
      bool active = false;
      if (!args.boolean(&active)) { protocol_error(c, r.id, kEINVAL, "invalid set_active"); return; }
      n->active = active;
      if (active && n->ports.count() && !n->config_sent) n->raw = true;      // (it has its ports already: a pw_filter, a MIDI stream)
      if (!n->node && !make_graph_node(c, *n)) return;
      if (active && !n->config_sent && !n->raw && (n->media == Media::Audio || n->media == Media::Video)) send_port_config(c, *n);
      if (active) {
        negotiate(c, *n);
      } else if (n->started) {
        send_command(c, *n, kCommandPause);
        n->started = false;
        n->pending = false;
      }
      return;
    }
    case kCnPortBuffers: port_buffers(c, *n, args); return;
    case kCnGetNode: case kCnEvent: return;      // (no server-side node proxy; RequestProcess and other events: the server keeps its own pace)
    default: protocol_error(c, r.id, kENOTSUP, "unknown client-node method"); return;
  }
}

// A cycle's frames: node.force-quantum (frames), else node.latency ("N/D": N frames at rate D, so N * rate / D at the port's), else 1024; 64 to 8192.
void NativeServer::set_quantum(ClientNode& n) {
  uint32_t num = 0, den = 1;
  uint64_t frames = 1024;
  if (parse_fraction(n.props.get("node.force-quantum"), &num, &den) && den == 1 && num) frames = num;
  else if (parse_fraction(n.props.get("node.latency"), &num, &den) && num) frames = uint64_t{num} * n.format.rate / den;
  if (frames < kMinQuantum) frames = kMinQuantum;
  if (frames > kMaxFrames) frames = kMaxFrames;
  n.quantum = static_cast<uint32_t>(frames);
}

// ---- the buffers ---------------------------------------------------------------------------------------------------------------------------------

// Buffer `id`'s data (the server's, or the client's memfd for an allocating port); empty when there is none.
ByteView NativeServer::buffer_data(ClientPort& p, uint32_t id, uint32_t* maxsize, uint32_t block) {
  *maxsize = 0;
  if (id >= p.n_buffers || block >= p.blocks || !p.buffers) return ByteView();
  if (p.alloc) {
    const uint64_t index = uint64_t{id} * p.blocks + block;
    if (index >= p.client_data.count() || !p.client_data[index].fd) return ByteView();
    ClientPort::ClientData& d = p.client_data[index];
    ByteView data = part(d.fd->memory(), d.mapoffset, d.maxsize);
    *maxsize = static_cast<uint32_t>(data.size());
    return data;
  }
  if (!p.gpu_images.empty()) {      // (the GPU image's CPU copy: written, then synced to the image)
    if (block != 0 || id >= p.gpu_images.count() || !p.gpu_images[id]) return ByteView();
    ByteView data = part(p.gpu_images[id]->memory(), 0, uint64_t{p.gpu_stride} * p.video.height);
    *maxsize = static_cast<uint32_t>(data.size());
    return data;
  }
  ByteView data = part(p.buffers->memory(), uint64_t{id} * p.buffer_stride + p.data_offset + uint64_t{block} * p.data_size, p.data_size);
  *maxsize = static_cast<uint32_t>(data.size());
  return data;
}

ByteView NativeServer::buffer_chunk(ClientPort& p, uint32_t id, uint32_t block) {
  if (id >= p.n_buffers || block >= p.blocks || !p.buffers) return ByteView();
  return part(p.buffers->memory(), uint64_t{id} * p.buffer_stride + p.chunk_offset + 16ull * block, 16);
}

// The id the program's memory pool gives the next memory added: a removed one (the last removed first), else the next new one.
uint32_t NativeServer::new_mem_id(Client& c) {
  if (c.free_mem_ids.count() > 0) {
    const uint32_t id = c.free_mem_ids[c.free_mem_ids.count() - 1];
    c.free_mem_ids.remove_at(c.free_mem_ids.count() - 1);
    return id;
  }
  return c.next_mem_id++;
}

// A port's buffers go (its format is to change): the program is told its memory is gone (it dropped the buffers when it was given the new Format).
void NativeServer::drop_port_buffers(Client& c, ClientPort& p) {
  if (p.buffers) {
    const uint64_t m = begin(c, 0, kCoreEvRemoveMem);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(p.buffers_mem));
    b.end(f);
    end(c, m);
    c.free_mem_ids.push_back(p.buffers_mem);
  }
  for (uint32_t mem : p.share_mems.view()) {      // (the producer's descriptors or the GPU images it was given)
    const uint64_t m = begin(c, 0, kCoreEvRemoveMem);
    pod::Builder b(c.outbox);
    const uint64_t f = b.begin_struct();
    b.int32(static_cast<int32_t>(mem));
    b.end(f);
    end(c, m);
    c.free_mem_ids.push_back(mem);
  }
  p.share_mems.clear();
  p.gpu_images.clear();
  p.leave_share();      // (a producer's buffers are not shared any more; a watcher holds none of them)
  p.buffers = Fd();
  p.client_data.clear();
  p.n_buffers = 0;
  p.blocks = 1;
  p.next_buffer = 0;
}

// Ports whose format is to change: they are given the new Format (the program drops its buffers), their old memory is removed, and new buffers follow
// when the program says what it wants of them (negotiate). The node keeps running; its other ports go on.
void NativeServer::renegotiate(Client& c, ClientNode& n) {
  n.renegotiate = false;
  for (UniquePointer<ClientPort>& pp : n.ports.view()) {
    ClientPort& p = *pp;
    if (!p.renegotiate) continue;
    p.renegotiate = false;
    if (n.io) {
      store<int32_t>(n.io->memory(), 8ull * p.index, p.direction == kDirectionOutput ? kStatusNeedData : 0);
      store<uint32_t>(n.io->memory(), 8ull * p.index + 4, kIdInvalid);
    }
    p.format_sent = false;
    p.buffers_sent = false;
    p.data_ready = false;
    p.wants_alloc = false;
    p.decode = UniquePointer<VideoDecodeSession>();      // (a new stream: its decoder starts again)
    ByteBuffer format(allocator_);
    if (fixate_format(n, p, &format)) send_port_param(c, n, p, format.view());
    drop_port_buffers(c, p);
  }
}

// The buffer's SPA_META_Header (if the port has one): what the server filled in, its time stamp and its number.
void NativeServer::write_header_meta(ClientPort& p, uint32_t id, int64_t pts) {
  for (uint32_t k = 0; k < p.n_metas; ++k) {
    if (p.metas[k].type != kMetaHeader || p.metas[k].size < 32) continue;
    ByteView h = part(p.buffers->memory(), uint64_t{id} * p.buffer_stride + p.metas[k].offset, 32);
    store<uint32_t>(h, 0, 0);
    store<uint32_t>(h, 4, 0);
    store<int64_t>(h, 8, pts);
    store<int64_t>(h, 16, 0);
    store<uint64_t>(h, 24, p.seq++);
  }
}

// SPA_META_Busy: a buffer whose count is not 0 is still being read by someone and must not be written.
bool NativeServer::buffer_busy(ClientPort& p, uint32_t id) {
  for (uint32_t k = 0; k < p.n_metas; ++k)
    if (p.metas[k].type == kMetaBusy && p.metas[k].size >= 8)
      return load<uint32_t>(part(p.buffers->memory(), uint64_t{id} * p.buffer_stride + p.metas[k].offset, 8), 4) != 0;
  return false;
}

static int64_t header_pts(ByteView buffers, uint64_t base, const NativeServer::ClientPort& p) {
  for (uint32_t k = 0; k < p.n_metas; ++k)
    if (p.metas[k].type == kMetaHeader && p.metas[k].size >= 32) return load<int64_t>(part(buffers, base + p.metas[k].offset, 32), 8);
  return 0;
}

// A producer's frame to its video channel: encoded, the first block's bytes; raw, a plane in each block (the port has one a plane), else the planes one
// after another in the first (`first`: its chunk's bytes, `stride0`: its chunk's stride).
void NativeServer::publish_video(ClientNode& n, ClientPort& p, uint32_t id, ConstByteView first, int32_t stride0, int64_t pts, const VideoFrame* shared) {
  const VideoFormat& v = p.video;
  if (v.encoded()) {
    const VideoPlaneData one{first, 0};
    n.video->publish(ArrayView<const VideoPlaneData>(&one, 1), pts, shared);
    return;
  }
  const uint32_t planes = v.planes();
  VideoPlaneData data[VideoFormat::kMaxPlanes];
  const uint32_t s0 = stride0 > 0 ? uint32_t(stride0) : v.plane(0).row_bytes;
  if (p.blocks >= planes) {
    for (uint32_t i = 0; i < planes; ++i) {
      if (i == 0) {
        data[0] = VideoPlaneData{first, s0};
        continue;
      }
      uint32_t maxsize = 0;
      ByteView block = buffer_data(p, id, &maxsize, i);
      ByteView chunk = buffer_chunk(p, id, i);
      const uint32_t offset = load<uint32_t>(chunk, 0), size = load<uint32_t>(chunk, 4);
      const int32_t stride = load<int32_t>(chunk, 8);
      ConstByteView bytes;
      if (!ConstByteView(block).try_subview(offset, size, &bytes)) return;      // (a chunk outside its block: the frame is dropped)
      data[i] = VideoPlaneData{bytes, stride > 0 ? uint32_t(stride) : v.plane(i).row_bytes};
    }
  } else {
    uint64_t at = 0;
    for (uint32_t i = 0; i < planes; ++i) {
      const uint32_t st = plane_stride(v, i, s0);
      if (at > first.size()) return;
      data[i] = VideoPlaneData{first.subview(at), st};
      at += uint64_t{st} * v.plane(i).rows;
    }
  }
  n.video->publish(ArrayView<const VideoPlaneData>(data, planes), pts, shared);
}

// What an output port produced this cycle (the buffer its io names with HAVE_DATA): to where the node's data goes; the buffer is handed back. False
// when nothing came.
bool NativeServer::consume_output(ClientNode& n, ClientPort& p) {
  ByteView io = part(n.io->memory(), 8ull * p.index, 8);
  const int32_t status = load<int32_t>(io, 0);
  const uint32_t id = load<uint32_t>(io, 4);
  const bool shared = p.media == Media::Video && p.alloc && p.share && !p.share->closed;
  if (!(status & kStatusHaveData) || id >= p.n_buffers) {
    if (!n.driving) {
      // (nothing came: nothing to hand back, or the producer would get a buffer twice - unless buffers it shared are held still and one of them is
      // free now: the io has room for it this cycle)
      const uint32_t back = shared ? pick_back(p) : kIdInvalid;
      store<uint32_t>(io, 4, back);
      store<int32_t>(io, 0, kStatusNeedData);
      if (back != kIdInvalid) {
        p.share->taken[back] = false;
        signal_release(p, back);
      }
    }
    return false;
  }
  if (shared) return consume_shared(n, p, status, id);
  // (a DMA-BUF's pixels: brought into the server's copy first; a frame the GPU is still drawing is not taken)
  if (p.alloc)
    for (uint32_t k = 0; k < p.blocks; ++k) {
      const uint64_t index = uint64_t{id} * p.blocks + k;
      if (index < p.client_data.count() && p.client_data[index].gpu && !p.client_data[index].fd->sync_from_gpu()) {
        if (!n.driving) {
          store<uint32_t>(io, 4, id);      // (handed back unread)
          store<int32_t>(io, 0, kStatusNeedData);
        } else {
          exchange_if<int32_t>(io, 0, status, kStatusNeedData);
        }
        return false;
      }
    }
  uint32_t maxsize = 0;
  ByteView data = buffer_data(p, id, &maxsize);
  const uint64_t base = uint64_t{id} * p.buffer_stride;
  ByteView chunk = buffer_chunk(p, id);
  const uint32_t offset = load<uint32_t>(chunk, 0), size = load<uint32_t>(chunk, 4);
  const int32_t stride = load<int32_t>(chunk, 8);
  if (maxsize > 0) {
    const uint32_t start = offset % maxsize, length = size < maxsize ? size : maxsize;
    const uint32_t first = length < maxsize - start ? length : maxsize - start;
    ConstByteView a = ConstByteView(data).subview(start, first), b = ConstByteView(data).first(length - first);
    switch (p.media) {
      case Media::Audio: {
        // (one interleaved port: straight on; a filter's float ports are put together by the caller, through the scratch)
        if (!n.raw) {
          const uint32_t frame = n.frame();
          if (n.playback) {
            n.playback->write(a.first(a.size() - a.size() % frame));
            if (b.size()) n.playback->write(b.first(b.size() - b.size() % frame));
          }
        } else {
          n.scratch.append(a);
          n.scratch.append(b);
        }
        break;
      }
      case Media::Midi: {
        // (a Sequence POD in the data; it cannot wrap: the chunk's offset is where it starts.) Each message sounds its offset's time after now: the
        // cycle's messages keep their distances, a cycle late.
        const uint64_t now = n.cycle_ns ? n.cycle_ns : now_ns();
        const uint32_t rate = n.format.rate ? n.format.rate : 48000;
        parse_sequence(a, p.ump_in, allocator_, [&](const MidiMessage& m0, uint32_t offset) {
          MidiMessage m = m0;
          m.source = n.node;
          const uint32_t in_cycle = offset < n.quantum ? offset : n.quantum;      // (an offset past the cycle: at its end)
          m.time_ns = now + uint64_t{in_cycle} * 1000000000ull / rate;
          graph_.midi_bus().publish(m, now);
        });
        break;
      }
      case Media::Video: {
        if (n.video && p.video.valid() && b.size() == 0) publish_video(n, p, id, a, stride, header_pts(p.buffers->memory(), base, p));
        break;
      }
      default: break;
    }
  }
  if (n.driving) {
    // (its own cycle may be running: the buffer is handed back only if the io still says it)
    exchange_if<int32_t>(io, 0, status, kStatusNeedData);
  } else {
    store<uint32_t>(io, 4, id);      // (handed back)
    store<int32_t>(io, 0, kStatusNeedData);
  }
  return true;
}

// ---- a producer whose buffers are shared ------------------------------------------------------------------------------------------------------------

// Whether the frame in buffer `id` is drawn: its acquire point reached (explicit sync, when its timeline can be read), else the GPU done with each of its
// DMA-BUFs.
bool NativeServer::frame_ready(ClientPort& p, uint32_t id) {
  VideoShare& s = *p.share;
  if (p.sync && p.timelines.count() > 2ull * id + 1 && id < VideoShare::kMaxBuffers) {
    uint64_t value = 0;
    if (p.timelines[2ull * id]->timeline_value(&value)) return value >= s.acquire_point[id];
  }
  for (uint32_t k = 0; k < p.blocks; ++k) {
    const uint64_t index = uint64_t{id} * p.blocks + k;
    if (index < p.client_data.count() && p.client_data[index].gpu && !p.client_data[index].fd->gpu_ready()) return false;
  }
  return true;
}

uint32_t NativeServer::pick_back(ClientPort& p) {
  VideoShare& s = *p.share;
  uint32_t back = kIdInvalid;
  for (uint32_t i = 0; i < s.n_buffers && i < VideoShare::kMaxBuffers; ++i)
    if (s.taken[i] && (i != s.latest || !s.attached) && i != s.pending && s.users[i] == 0 && (back == kIdInvalid || s.taken_at[i] < s.taken_at[back]))
      back = i;      // (the newest frame's is kept only while some watcher may still want it)
  return back;
}

void NativeServer::signal_release(ClientPort& p, uint32_t id) {
  VideoShare& s = *p.share;
  if (id >= VideoShare::kMaxBuffers || !s.promised[id]) return;
  s.promised[id] = false;
  if (p.timelines.count() > 2ull * id + 1) p.timelines[2ull * id + 1]->timeline_signal(s.release_point[id]);
}

// A drawn frame to the channel: as it is, in the shared buffer, for the watchers given that (and with a copy when some watcher needs one: the kernel's,
// one converting, one in its own memory); its buffer held for the watchers.
void NativeServer::take_shared_frame(ClientNode& n, ClientPort& p, uint32_t id) {
  VideoShare& s = *p.share;
  if (!n.video || !p.video.valid()) return;
  const bool copy = !s.attached || graph_.video_watchers(n.node) > s.attached;
  if (copy)
    for (uint32_t k = 0; k < p.blocks; ++k) {
      const uint64_t index = uint64_t{id} * p.blocks + k;
      if (index < p.client_data.count() && p.client_data[index].gpu && !p.client_data[index].fd->sync_from_gpu()) return;
    }
  uint32_t maxsize = 0;
  ByteView data = buffer_data(p, id, &maxsize);
  ByteView chunk = buffer_chunk(p, id);
  if (chunk.empty()) return;
  const uint32_t offset = load<uint32_t>(chunk, 0), size = load<uint32_t>(chunk, 4);
  const int32_t stride = load<int32_t>(chunk, 8);
  SharedPointer<VideoFrame> frame = SharedPointer<VideoFrame>::make(allocator_, allocator_);
  if (!frame) return;
  frame->share = p.share;
  frame->share_buffer = id;
  frame->pts = header_pts(p.buffers->memory(), uint64_t{id} * p.buffer_stride, p);
  for (uint32_t k = 0; k < p.blocks && k < VideoFormat::kMaxPlanes; ++k) {
    ByteView ck = buffer_chunk(p, id, k);
    if (ck.empty()) break;
    frame->chunks[k] = VideoShareChunk{load<uint32_t>(ck, 0), load<uint32_t>(ck, 4), load<int32_t>(ck, 8)};
  }
  if (copy) {
    if (maxsize == 0 || offset >= maxsize || size > maxsize - offset) return;      // (a frame does not wrap)
    publish_video(n, p, id, ConstByteView(data).subview(offset, size), stride, frame->pts, frame.get());
  } else if (!n.video->publish_shared(util::move(frame))) {
    return;
  }
  s.latest = s.attached ? id : kIdInvalid;
}

bool NativeServer::consume_shared(ClientNode& n, ClientPort& p, int32_t status, uint32_t id) {
  ByteView io = part(n.io->memory(), 8ull * p.index, 8);
  VideoShare& s = *p.share;
  if (id >= VideoShare::kMaxBuffers) return false;
  // (explicit sync: the frame's points; the server promises the release - clears UNSCHEDULED_RELEASE - when it can signal that timeline)
  s.promised[id] = false;
  if (p.sync)
    for (uint32_t k = 0; k < p.n_metas; ++k) {
      if (p.metas[k].type != kMetaSyncTimeline || p.metas[k].size < 24) continue;
      ByteView m = part(p.buffers->memory(), uint64_t{id} * p.buffer_stride + p.metas[k].offset, 24);
      if (m.empty()) break;
      s.acquire_point[id] = load<uint64_t>(m, 8);
      s.release_point[id] = load<uint64_t>(m, 16);
      uint64_t now = 0;
      if (p.timelines.count() > 2ull * id + 1 && p.timelines[2ull * id + 1]->timeline_value(&now)) {
        store<uint32_t>(m, 0, load<uint32_t>(m, 0) & ~kSyncUnscheduledRelease);
        s.promised[id] = true;
      }
      break;
    }
  s.taken[id] = true;
  s.taken_at[id] = ++s.clock;
  if (s.pending == id) s.pending = kIdInvalid;
  if (frame_ready(p, id)) take_shared_frame(n, p, id);
  else s.pending = id;      // (shown once it is drawn: check_pending; a pending one before it is passed over)
  // what goes back: the oldest buffer taken that nobody holds (this one when nothing else is held and it was taken at once)
  const uint32_t back = pick_back(p);
  if (n.driving) {
    // (its own cycle may be running: handed back only if the io still says what it gave)
    const uint64_t was = uint64_t{uint32_t(status)} | uint64_t{id} << 32, now = uint64_t{uint32_t(kStatusNeedData)} | uint64_t{back} << 32;
    if (!exchange_if<uint64_t>(io, 0, was, now)) return true;
  } else {
    store<uint32_t>(io, 4, back);
    store<int32_t>(io, 0, kStatusNeedData);
  }
  if (back != kIdInvalid) {
    s.taken[back] = false;
    signal_release(p, back);
  }
  return true;
}

void NativeServer::check_pending(ClientNode& n, ClientPort& p) {
  if (!p.share || p.share->closed || p.share->pending == kIdInvalid) return;
  const uint32_t id = p.share->pending;
  if (!frame_ready(p, id)) return;
  p.share->pending = kIdInvalid;
  take_shared_frame(n, p, id);
}

// Fills an input port's next free buffer with what it is to hear or see this cycle, and says so in its io (HAVE_DATA). False when there is nothing.
bool NativeServer::fill_input(ClientNode& n, ClientPort& p) {
  ByteView io = part(n.io->memory(), 8ull * p.index, 8);
  if (n.driving && load<int32_t>(io, 0) == kStatusHaveData) return false;      // (its cycle has not taken the last one yet)
  if (p.media == Media::Video && p.share) return fill_shared(n, p);
  uint32_t id = kIdInvalid;
  for (uint32_t tries = 0; tries < p.n_buffers; ++tries) {
    const uint32_t candidate = p.next_buffer;
    p.next_buffer = (p.next_buffer + 1) % p.n_buffers;
    if (!buffer_busy(p, candidate)) {
      id = candidate;
      break;
    }
  }
  if (id == kIdInvalid) return false;
  uint32_t maxsize = 0;
  ByteView data = buffer_data(p, id, &maxsize);
  if (maxsize == 0) return false;
  uint32_t got = 0;
  int32_t stride = 0;
  switch (p.media) {
    case Media::Audio: {
      const uint32_t frame = n.frame();
      const uint64_t want = uint64_t{n.quantum} * frame;
      if (want > maxsize) return false;
      if (n.record) {
        got = n.record->read(data.first(want));
      }
      stride = static_cast<int32_t>(frame);
      break;
    }
    case Media::Midi: {
      // a Sequence of the messages whose time has come (before this cycle), each a control at its place in the cycle: the time it came, a cycle later
      // (SPA_CONTROL_UMP for a UMP port, else SPA_CONTROL_Midi's raw bytes; system exclusive whole, or as UMP Data Messages)
      if (!n.midi_in || maxsize < 16) return false;
      const uint32_t rate = n.format.rate ? n.format.rate : 48000;
      const uint64_t period = frames_ns(n.quantum, rate);
      const uint64_t now = n.cycle_ns;
      const uint64_t window = now > period ? now - period : 0;
      ByteWriter w(data);
      w.le32(0);
      w.le32(pod::kSequence);
      w.le32(0);
      w.le32(0);
      uint64_t used = 16;
      uint32_t last_offset = 0;
      auto value_bytes = [&](const MidiMessage& m) -> uint64_t { return p.ump ? 4ull * ump_words_for(m) : m.wire().size(); };
      MidiMessage m;
      while (n.midi_in->pop_if([&](const MidiMessage& next) {
               if (now && next.time_ns > now) return MidiQueue::Choice::Wait;      // (not yet)
               const uint64_t padded = (value_bytes(next) + 7) & ~uint64_t{7};
               if (padded + 16 > uint64_t{maxsize} - 16) return MidiQueue::Choice::Drop;      // (it can never fit this port's buffers)
               return padded + 16 <= uint64_t{maxsize} - used ? MidiQueue::Choice::Take : MidiQueue::Choice::Wait;
             }, &m)) {
        uint64_t offset = m.time_ns > window ? (m.time_ns - window) * rate / 1000000000ull : 0;
        if (offset >= n.quantum) offset = n.quantum - 1;
        if (offset < last_offset) offset = last_offset;      // (in order: a Sequence's offsets never go back)
        last_offset = static_cast<uint32_t>(offset);
        const uint64_t size = value_bytes(m);
        w.le32(last_offset);
        w.le32(p.ump ? kControlUmp : kControlMidi);
        w.le32(static_cast<uint32_t>(size));
        w.le32(pod::kBytes);
        if (p.ump) write_ump(m, [&](uint32_t word) { w.le32(word); });
        else w.bytes(m.wire());
        const uint64_t pad = ((size + 7) & ~uint64_t{7}) - size;
        const uint8_t zeros[8] = {};
        if (pad) w.bytes(ConstByteView(zeros, pad));
        used += 16 + size + pad;
      }
      store<uint32_t>(data, 0, static_cast<uint32_t>(used - 8));
      got = static_cast<uint32_t>(used);
      break;
    }
    case Media::Video: {
      // the producer's newest frame: encoded, its bytes; raw, a plane in each block (or one after another in the first), rows at the port's stride
      if (!n.video) return false;
      SharedPointer<VideoFrame> frame = n.video->latest();
      if (!frame || frame->sequence <= p.video_seen || !(frame->format == p.source_video)) return false;
      if (frame->pixels.size() == 0) return false;      // (only in its producer's shared buffers: the next one is copied for this watcher too)
      const VideoFormat& v = p.video;
      // (in the watcher's format: converted when that is not the producer's)
      ConstByteView pixels = frame->pixels.view();
      if (!(v == p.source_video)) {
        if (p.source_video.encoded()) {      // (encoded video to a raw watcher: decoded)
          if (!p.decode) p.decode = UniquePointer<VideoDecodeSession>::make(allocator_, allocator_);
          bool failed = false;
          if (!p.decode || !decode_video(pixels, p.source_video, v, *p.decode, graph_.video_decoders(), &p.converted, &failed)) {
            p.video_seen = frame->sequence;      // (a frame that cannot be decoded is passed over; one the decoder holds back comes out later)
            return false;
          }
        } else if (!convert_video(p.source_video, pixels, v, &p.converted)) {
          return false;
        }
        pixels = p.converted.view();
      }
      if (v.encoded()) {
        if (pixels.size() > maxsize) return false;
        data.first(pixels.size()).copy_from(pixels);
        got = static_cast<uint32_t>(pixels.size());
        p.video_seen = frame->sequence;
        break;
      }
      const uint32_t planes = v.planes();
      const uint32_t s0 = plane_stride(v, 0, p.wanted_stride);
      const bool separate = p.blocks >= planes && planes > 1;
      if (!separate && planes_bytes(v, s0, 0, planes) > maxsize) return false;
      uint64_t at = 0;
      for (uint32_t i = 0; i < planes; ++i) {
        const VideoPlane pl = v.plane(i);
        const uint32_t st = plane_stride(v, i, s0);
        const uint64_t bytes = uint64_t{st} * pl.rows;
        uint32_t block_size = maxsize;
        ByteView out = separate ? buffer_data(p, id, &block_size, i) : data;
        if (separate) at = 0;
        if (block_size < bytes || out.size() < at + bytes) return false;
        const ConstByteView src = pixels.subview(v.plane_offset(i), uint64_t{pl.row_bytes} * pl.rows);
        for (uint32_t y = 0; y < pl.rows; ++y)
          out.subview(at + uint64_t{y} * st, pl.row_bytes).copy_from(src.subview(uint64_t{y} * pl.row_bytes, pl.row_bytes));
        if (separate && i > 0) {
          ByteView chunk = buffer_chunk(p, id, i);
          store<uint32_t>(chunk, 0, 0);
          store<uint32_t>(chunk, 4, static_cast<uint32_t>(bytes));
          store<int32_t>(chunk, 8, static_cast<int32_t>(st));
          store<int32_t>(chunk, 12, 0);
        }
        if (i == 0) {
          got = static_cast<uint32_t>(separate ? bytes : planes_bytes(v, s0, 0, planes));
          stride = static_cast<int32_t>(st);
        }
        at += bytes;
      }
      if (!p.gpu_images.empty() && !p.gpu_images[id]->sync_to_gpu()) return false;      // (a GPU image: the frame into it)
      p.video_seen = frame->sequence;
      break;
    }
    default: return false;
  }
  ByteView chunk = buffer_chunk(p, id);
  store<uint32_t>(chunk, 0, 0);
  store<uint32_t>(chunk, 4, got);
  store<int32_t>(chunk, 8, stride);
  store<int32_t>(chunk, 12, 0);
  write_header_meta(p, id, static_cast<int64_t>(frames_ns(n.position, n.format.rate)));
  store<uint32_t>(io, 4, id);
  store<int32_t>(io, 0, kStatusHaveData);
  return got > 0 || p.media == Media::Midi;
}

// A watcher given its producer's own buffers: the buffer it recycled (its io: NEED_DATA with that id) let go, then the newest frame's buffer given - its
// chunks and header written in the watcher's own part of it - with no copy. False when no new frame came.
bool NativeServer::fill_shared(ClientNode& n, ClientPort& p) {
  ByteView io = part(n.io->memory(), 8ull * p.index, 8);
  VideoShare& share = *p.share;
  const int32_t status = load<int32_t>(io, 0);
  const uint32_t back = load<uint32_t>(io, 4);
  auto release = [&](uint32_t id) {
    if (id >= share.n_buffers || id >= VideoShare::kMaxBuffers || !p.holds[id]) return;
    p.holds[id] = false;
    if (share.users[id]) --share.users[id];
  };
  if (status != kStatusHaveData && back != kIdInvalid) {
    release(back);
    store<uint32_t>(io, 4, kIdInvalid);
  }
  if (share.closed || !n.video) return false;      // (it is given other buffers at the next tick)
  SharedPointer<VideoFrame> frame = n.video->latest();
  if (!frame || frame->sequence <= p.video_seen || frame->share.get() != &share || frame->share_buffer >= share.n_buffers ||
      frame->share_buffer >= VideoShare::kMaxBuffers)
    return false;
  if (status == kStatusHaveData) release(back);      // (the last one given was never taken: this one instead)
  const uint32_t id = frame->share_buffer;
  for (uint32_t k = 0; k < p.blocks && k < VideoFormat::kMaxPlanes; ++k) {
    ByteView chunk = buffer_chunk(p, id, k);
    if (chunk.empty()) return false;
    store<uint32_t>(chunk, 0, frame->chunks[k].offset);
    store<uint32_t>(chunk, 4, frame->chunks[k].size);
    store<int32_t>(chunk, 8, frame->chunks[k].stride);
    store<int32_t>(chunk, 12, 0);
  }
  write_header_meta(p, id, frame->pts);
  if (!p.holds[id]) {
    p.holds[id] = true;
    ++share.users[id];
  }
  p.video_seen = frame->sequence;
  store<uint32_t>(io, 4, id);
  store<int32_t>(io, 0, kStatusHaveData);
  return true;
}

// ---- the cycle -----------------------------------------------------------------------------------------------------------------------------------

// Whether a server-driven node's next cycle is due: playback (to the speakers or a virtual sink's mixer) while the graph holds little, recording once a
// quantum is there, MIDI and video on the server's clock (a watcher when there is a new frame).
bool NativeServer::cycle_due(ClientNode& n, uint64_t now) {
  if (n.media == Media::Audio) {
    if (n.playback) {
      const uint64_t ahead = 2ull * n.quantum > kAheadFrames ? 2ull * n.quantum : kAheadFrames;
      // (an effect also records: due once a quantum is recorded too, so that its inputs are never given less than a cycle)
      if (n.record && n.in_channels && n.record->available_bytes() < uint64_t{n.quantum} * 4 * n.in_channels) return false;
      return n.playback->waiting_bytes() < ahead * n.frame();
    }
    if (n.record) return n.record->available_bytes() >= uint64_t{n.quantum} * n.frame();
    return false;
  }
  uint64_t period = frames_ns(n.quantum, n.format.rate);
  if (n.media == Media::Video)
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->media == Media::Video) period = frame_period_ns(p->video);
  if (n.next_cycle_ns == 0) n.next_cycle_ns = now;
  if (now < n.next_cycle_ns) return false;
  if (n.media == Media::Video && n.direction == kDirectionInput) {
    if (!n.video) return false;
    bool fresh = false;
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->media == Media::Video && p->direction == kDirectionInput) fresh |= n.video->frames() > p->video_seen;
    if (!fresh) return false;
  }
  n.next_cycle_ns += period;
  if (n.next_cycle_ns + 4 * period < now) n.next_cycle_ns = now + period;      // (long late: start the beat again)
  return true;
}

void NativeServer::trigger(ClientNode& n, uint64_t) {
  ByteView a = n.activation->memory();
  const uint32_t status = load<uint32_t>(a, kActStatus);
  if (status == kActInactive) return;      // (the client has not added the node to its loop yet)
  const uint64_t nsec = frames_ns(n.position, n.format.rate);
  const ByteView records[2] = {a, n.driver ? n.driver->memory() : ByteView()};      // (a follower's position is the server's driver activation)
  for (const ByteView& act : records) {
    if (act.empty()) continue;
    const uint64_t clock = kActPosition + kPosClock;
    store<uint64_t>(act, clock + kClockNsec, nsec);
    store<uint64_t>(act, clock + kClockPosition, n.position);
    store<uint32_t>(act, clock + kClockRate + 4, n.format.rate);
    store<uint64_t>(act, clock + kClockDuration, n.quantum);
    store<uint64_t>(act, clock + kClockNextNsec, nsec + frames_ns(n.quantum, n.format.rate));
    store<uint32_t>(act, clock + kClockCycle, static_cast<uint32_t>(n.cycles));
  }
  store<uint64_t>(a, kActSignalTime, nsec);
  store<uint32_t>(a, kActStatus, kActNotTriggered);
  store<uint32_t>(a, kActStatus, kActTriggered);
  n.position += n.quantum;
  ++n.cycles;
  n.pending = true;
  n.triggered_at = 0;
  n.wake->signal();
}

void NativeServer::run_client_node(ClientNode& n, uint64_t now) {
  if (!n.activation || !n.io) return;
  n.cycle_ns = now;
  ByteView a = n.activation->memory();
  // a filter's float ports: what its outputs played, put together (a channel each) for the speakers; what its inputs are to hear (the microphone),
  // taken apart (a channel each)
  const bool filter_out = n.raw && n.media == Media::Audio && n.out_channels > 0;
  const bool filter_in = n.raw && n.media == Media::Audio && n.in_channels > 0;
  auto play_ports = [&]() {
    if (!filter_out) return;
    ClientPort* sides[kMaxPorts] = {};
    uint32_t found = 0;
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->media == Media::Audio && p->direction == kDirectionOutput && found < n.out_channels) sides[found++] = p.get();
    n.scratch.clear();
    uint64_t starts[kMaxPorts] = {}, lengths[kMaxPorts] = {};
    uint64_t frames = 0;      // (the longest port's: one that gave less is silent for the rest)
    for (uint32_t s = 0; s < found; ++s) {
      starts[s] = n.scratch.size();
      if (sides[s]->buffers_sent && sides[s]->data_ready) consume_output(n, *sides[s]);
      lengths[s] = n.scratch.size() - starts[s];
      if (lengths[s] / 4 > frames) frames = lengths[s] / 4;
    }
    if (frames == 0 || !n.playback) return;
    const uint64_t frame_bytes = 4ull * n.out_channels;
    ByteBuffer interleaved(allocator_);
    if (!interleaved.put_zeros(frames * frame_bytes)) return;
    ByteView out = interleaved.writable_view();
    const ConstByteView all = n.scratch.view();
    for (uint32_t s = 0; s < found; ++s)
      for (uint64_t f = 0; f < frames && f < lengths[s] / 4; ++f) out.subview(f * frame_bytes + 4ull * s, 4).copy_from(all.subview(starts[s] + f * 4, 4));
    n.playback->write(interleaved.view());
  };
  auto take_outputs = [&]() {
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->direction == kDirectionOutput && p->buffers_sent && p->data_ready && !(filter_out && p->media == Media::Audio)) consume_output(n, *p);
    play_ports();
  };
  auto give_inputs = [&]() {
    if (filter_in) {
      // a quantum recorded in the filter's channels, each to its port
      ClientPort* sides[kMaxPorts] = {};
      uint32_t found = 0;
      for (UniquePointer<ClientPort>& p : n.ports.view())
        if (p->media == Media::Audio && p->direction == kDirectionInput && p->buffers_sent && p->data_ready && found < n.in_channels) sides[found++] = p.get();
      const uint64_t frame_bytes = 4ull * n.in_channels;
      n.scratch.clear();
      if (found > 0 && n.scratch.put_zeros(uint64_t{n.quantum} * frame_bytes)) {
        uint32_t got = 0;
        if (n.record) got = n.record->read(n.scratch.writable_view());
        const uint64_t frames = got / frame_bytes;
        for (uint32_t s = 0; s < found; ++s) {
          ClientPort& p = *sides[s];
          ByteView io = part(n.io->memory(), 8ull * p.index, 8);
          const uint32_t id = p.next_buffer;
          p.next_buffer = (p.next_buffer + 1) % p.n_buffers;
          uint32_t maxsize = 0;
          ByteView data = buffer_data(p, id, &maxsize);
          if (maxsize < frames * 4) continue;
          for (uint64_t f = 0; f < frames; ++f) data.subview(f * 4, 4).copy_from(n.scratch.view().subview(f * frame_bytes + 4ull * s, 4));
          ByteView chunk = buffer_chunk(p, id);
          store<uint32_t>(chunk, 0, 0);
          store<uint32_t>(chunk, 4, static_cast<uint32_t>(frames * 4));
          store<int32_t>(chunk, 8, 4);
          store<int32_t>(chunk, 12, 0);
          write_header_meta(p, id, static_cast<int64_t>(frames_ns(n.position, n.format.rate)));
          store<uint32_t>(io, 4, id);
          store<int32_t>(io, 0, kStatusHaveData);
        }
      }
    }
    for (UniquePointer<ClientPort>& p : n.ports.view())
      if (p->direction == kDirectionInput && p->buffers_sent && p->data_ready && !(filter_in && p->media == Media::Audio)) fill_input(n, *p);
  };
  if (n.driving) {
    // the program runs its cycles: what its outputs produced is taken, its inputs are given what there is
    take_outputs();
    give_inputs();
    return;
  }
  if (n.pending) {
    const uint32_t status = load<uint32_t>(a, kActStatus);
    if (status != kActFinished) {
      if (++n.triggered_at < kTimeoutTicks) return;
      n.pending = false;      // (it never finished: try again)
    } else {
      n.pending = false;
      take_outputs();
    }
  }
  if (n.pending || !cycle_due(n, now)) return;
  give_inputs();
  trigger(n, now);
}

// ---- virtual sinks (the adapter factory) ---------------------------------------------------------------------------------------------------------

NativeServer::VirtualSink* NativeServer::virtual_sink(uint32_t node) {
  for (UniquePointer<VirtualSink>& v : virtuals_.view())
    if (v->node == node) return v.get();
  return nullptr;
}

void NativeServer::create_adapter(Client& c, uint32_t new_id, uint32_t version, const Properties& props) {
  auto refuse = [&](int32_t res, const char* why) {
    send_error(c, new_id, c.message_seq, -res, why);
    send_remove_id(c, new_id);
  };
  if (resource(c, new_id) || client_node(c, new_id)) return protocol_error(c, new_id, kEINVAL, "id in use");
  if (props.get("factory.name") != StringView("support.null-audio-sink"))
    return refuse(kENOTSUP, "the adapter makes support.null-audio-sink nodes only (no SPA plugins run in the kernel)");
  uint32_t channels = parse_uint(props.get("audio.channels"), 0);
  const StringView position = props.get("audio.position");
  if (channels == 0 && !position.empty()) {
    channels = 1;
    for (uint64_t i = 0; i < position.size(); ++i) channels += position[i] == ',';
  }
  if (channels == 0) channels = 2;
  if (channels > kMaxChannels) channels = kMaxChannels;      // (wider ones are made stereo)
  uint32_t rate = parse_uint(props.get("audio.rate"), 48000);
  if (rate < kMinRate || rate > kMaxRate) rate = 48000;
  UniquePointer<VirtualSink> v = UniquePointer<VirtualSink>::make(allocator_);
  if (!v) return refuse(kENOMEM, "out of memory");
  Properties node_props(allocator_);
  node_props.copy_from(props);
  if (!node_props.has("node.name")) node_props.set("node.name", "null-sink");
  const uint32_t node = graph_.create_virtual_node(c.graph_client, node_props, channels, rate);
  if (!node) return refuse(kENOMEM, "no virtual sink could be made (too many, out of memory, or the graph has no clock)");
  v->node = node;
  v->owner = c.handle;
  v->resource = new_id;
  v->linger = props.get("object.linger") == StringView("true") || props.get("object.linger") == StringView("1");
  Resource r{new_id, ObjectType::Node, false, node, version ? version : 3, 0, {}};
  if (!add_resource(c, r) || !virtuals_.push_back(util::move(v))) {
    graph_.destroy_node(node);
    return refuse(kENOMEM, "out of memory");
  }
  // the proxy is bound to the node, and told what it is
  const uint64_t m = begin(c, 0, c.core_version >= 4 ? kCoreEvBoundProps : kCoreEvBoundId);
  pod::Builder b(c.outbox);
  const uint64_t f = b.begin_struct();
  b.int32(static_cast<int32_t>(new_id));
  b.int32(static_cast<int32_t>(node));
  if (c.core_version >= 4) {
    Properties bound(allocator_);
    graph_.visit(node, [&](const GraphObject& o) { bound.copy_from(o.props); });
    const uint64_t d = b.begin_struct();
    b.int32(static_cast<int32_t>(bound.count()));
    for (uint64_t i = 0; i < bound.count(); ++i) {
      b.string(bound.at(i).key.view());
      b.string(bound.at(i).value.view());
    }
    b.end(d);
  }
  b.end(f);
  end(c, m);
  if (Resource* made = resource(c, new_id)) send_info(c, *made);
}

void NativeServer::destroy_virtual(uint32_t node) {
  for (uint64_t i = 0; i < virtuals_.count(); ++i)
    if (virtuals_[i]->node == node) {
      // the streams that played into it or recorded from it lose it (they hear and make silence)
      for (UniquePointer<Client>& c : clients_.view())
        for (UniquePointer<ClientNode>& n : c->nodes.view())
          if (n->virtual_sink == node) n->virtual_sink = 0;
      virtuals_.remove_at(i);
      graph_.destroy_node(node);
      return;
    }
}

}  // namespace pipewire
