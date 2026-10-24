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

// The pictures, videos and canvases of a page. An <img>, a <video> and a <canvas> are *objects*: they take a rectangle of the page, in pixels, that
// PageLayout reserves and PageView draws into. PageObjects keeps what is behind them:
//
//   <img>     the picture is fetched (an address, a data: URL, a srcset / <picture> choice), decoded by the image codecs (PNG, JPEG, GIF - animated too -
//             BMP, WebP lossless, QOI, TGA, TIFF, PNM) and shown scaled to its place; one that cannot be had is shown as its alternative text. load and
//             error events are queued for the page to fire.
//   <video>   the file is fetched and handed to the environment to play (IVideoClip: AVI, MP4 / MOV with H.264, GIF, APNG, WebP, Y4M - on the video
//             hardware where the machine has some); the current picture is drawn, a poster before that, and a control bar when the element asks for
//             controls. play() / pause() / currentTime / volume / muted / playbackRate and the media events are the script side of the same thing: the
//             element's readyState, networkState, error, seeking, buffered / seekable / played ranges follow what the file and the clip are doing. The
//             file is fetched whole (fetch_bytes), so it is buffered from the moment it opens; a clip that cannot play at another speed is made to by
//             seeking (with its sound muted), see update().
//   <audio>   the same, without a picture: the file (MP3, AAC / M4A, Ogg Vorbis, FLAC, WAV, AIFF, AU, MIDI ...) is handed to the environment to play
//             (open_audio()); only an element with controls takes room on the page - a bar with play / pause, a position and the time. `new Audio(src)` too.
//   <canvas>  a CanvasBitmap (see Canvas.hpp), drawn by the renderer the page is shown on - the GPU's when the machine has one.
//
// All of it is shared between the thread the page runs on and the one that draws, so every call takes the object lock (a recursive one: scripts hold it
// across a sequence of calls through Lock).
#pragma once
#include "Browser/Canvas.hpp"
#include "Browser/Svg.hpp"
#include "Browser/ObjectTypes.hpp"
#include "Browser/Media.hpp"
#include "Dom/Dom.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Kernel/Sync.hpp"
#include "Media/Image.hpp"
#include "Media/Eme.hpp"
#include "Media/Mse.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

class MseClip;

// A video the environment is playing for a page.
class IVideoClip {
 public:
  virtual bool play() = 0;
  virtual void pause() = 0;
  virtual void seek(int64_t microseconds) = 0;
  virtual void set_looping(bool looping) = 0;
  virtual void set_volume(double volume, bool muted) = 0;
  // Plays at `rate` times the normal speed; false if this clip only plays at normal speed (it does for 1). The page then follows the clock itself by seeking.
  virtual bool set_rate(double rate) { return rate == 1; }
  virtual bool playing() = 0;
  virtual int64_t position_us() = 0;
  // The length in microseconds; -1 if it is not known (NaN to script), INT64_MAX for a stream that does not end (Infinity).
  virtual int64_t duration_us() = 0;
  virtual uint32_t width() = 0;
  virtual uint32_t height() = 0;
  // Does what playing takes (decodes, follows the clock); called every few milliseconds.
  virtual void update() = 0;
  // Copies the current picture (width() x height(), 0xAARRGGBB, `pitch` pixels per row, so `dest` needs pitch * height() pixels) and says which picture it
  // is; false if there is none or `dest` is too small.
  virtual bool copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* serial) = 0;

  // --- For a clip that is fed while it plays (a media source); a file that is all there needs none of this.
  // Do its readiness and ranges change by themselves? The page then watches them and fires the events (loadedmetadata, canplay, waiting, progress ...).
  virtual bool dynamic() { return false; }
  // HAVE_NOTHING 0 .. HAVE_ENOUGH_DATA 4.
  virtual uint8_t ready_state() { return 4; }
  // The buffered and the seekable time ranges, as start and end seconds one after the other (at most pairs.size() / 2); how many ranges there are.
  virtual uint32_t buffered(ArrayView<double> pairs) { (void)pairs; return 0; }
  virtual uint32_t seekable(ArrayView<double> pairs) { (void)pairs; return 0; }
  // A MediaError code (2 network, 3 decode, 4 not supported) if the clip cannot go on, else 0.
  virtual uint8_t error() { return 0; }
  // Is it stopped for want of a key to decrypt what comes next (an encrypted stream, Encrypted Media Extensions)? "waitingforkey" is fired when it is.
  virtual bool waiting_for_key() { return false; }

 protected:
  IVideoClip() = default;
  ~IVideoClip() = default;
};

// What the objects need from the browser.
class IObjectEnvironment {
 public:
  // The bytes at an address (an image, a video), at most `limit`. False if they cannot be had.
  virtual bool fetch_bytes(StringView address, uint64_t limit, ByteBuffer* out) = 0;
  // Opens a video file for playing; the clip keeps `data` as long as it needs it. Null if this machine cannot play it.
  virtual UniquePointer<IVideoClip> open_video(UniquePointer<ByteBuffer> data) { (void)data; return UniquePointer<IVideoClip>(); }
  // The same for a sound file: a clip whose width() and height() are 0 and that has no picture. Null if this machine cannot play it.
  virtual UniquePointer<IVideoClip> open_audio(UniquePointer<ByteBuffer> data) { (void)data; return UniquePointer<IVideoClip>(); }

 protected:
  IObjectEnvironment() = default;
  ~IObjectEnvironment() = default;
};

// Decodes a data: URL ("data:image/png;base64,....") into its bytes and media type. False if it is not one.
bool decode_data_url(allocators::IAllocator& allocator, StringView url, ByteBuffer* out, String* media_type);

class PageObjects final : public IObjectSource, public IObjectPainter {
 public:
  static constexpr uint32_t kMaxImages = 64, kMaxVideos = 8;
  static constexpr uint64_t kMaxImageBytes = 24u << 20, kMaxVideoBytes = 96u << 20, kMaxImagePixels = 16u << 20, kMaxCanvasPixels = 96u << 20;

  explicit PageObjects(allocators::IAllocator& allocator);
  ~PageObjects();
  PageObjects(const PageObjects&) = delete;
  PageObjects& operator=(const PageObjects&) = delete;

  // Holds the object lock for as long as it lives.
  class Lock {
   public:
    explicit Lock(PageObjects& objects) : mutex_(objects.mutex_) { mutex_.lock(); }
    ~Lock() { mutex_.unlock(); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    kernel::RecursiveMutex& mutex_;
  };

  // --- The page's side
  // Looks through the document for pictures, videos and canvases: fetches what has not been fetched (relative to `base`), opens videos, forgets what is gone.
  void sync(dom::Document& doc, IObjectEnvironment& env, StringView base, const MediaEnvironment& media);
  struct Event {
    uint32_t serial = 0;
    const char* type = "";
    uint32_t source = 0;   // not 0: the event is for the MediaSource with that number (sourceopen, sourceclose), `serial` is not used
  };
  // The next event to fire at an element (load, error, loadedmetadata, play, pause, ended, timeupdate ...) or a MediaSource; false when there is none.
  bool take_event(Event* out);
  // Moves the videos along (the host clock, in ms); queues the events that follow.
  void update(uint64_t now_ms);
  // Is something moving (a video playing, a picture animated) so that the page needs drawing often?
  bool animating();
  // Did something visible change since the last call (a canvas drawn, a video frame, an animation frame)?
  bool take_dirty();
  void mark_dirty() {
    Lock lock(*this);
    dirty_ = true;
  }

  // --- IObjectSource, IObjectPainter
  bool object_info(const dom::Node* element, ObjectInfo* out) override;
  void paint_object(graphics::IRenderer2D& r, uint32_t node, ObjectKind kind, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip) override;
  // A click inside a video's rectangle w x h at (lx, ly): the control bar or the picture act on the player. True if something was done.
  bool object_clicked(uint32_t serial, int32_t lx, int32_t ly, uint32_t w, uint32_t h);
  void set_accelerated(bool accelerated) { accelerated_ = accelerated; }
  bool accelerated() const { return accelerated_; }
  void set_now(uint64_t now_ms) { now_ms_ = now_ms; }

  // --- Pictures (script side)
  struct ImageState {
    bool known = false, complete = false, broken = false;
    uint32_t width = 0, height = 0;
    String current_source;
    explicit ImageState(allocators::IAllocator& a) : current_source(a) {}
  };
  void image_state(uint32_t serial, ImageState* out);
  // The picture to draw from (the frame showing now for an animation). False if the element has none. Valid until the next sync().
  bool image_pixels(uint32_t serial, ImageSource* out);
  // The current picture of a video, for drawImage() (valid until the next call). False if there is none.
  bool video_pixels(uint32_t serial, ImageSource* out);

  // --- Canvases (script side)
  // The canvas of an element with that size; made if it was not (null if memory or the total size limit ran out). A different size starts a new one.
  CanvasBitmap* canvas(uint32_t serial, uint32_t width, uint32_t height);
  CanvasBitmap* existing_canvas(uint32_t serial);

  // --- Videos (script side)
  // --- Media sources (the MediaSource and SourceBuffer objects of script work on this; the element plays what they hold)
  media::MseHub& mse() { return mse_; }
  // --- Blob URLs: URL.createObjectURL(blob) hands the bytes over, so that <img>, <video> and <audio> can show them (bounded; a blob too big is only
  // left out here, fetch() still reaches it). Revoking the address forgets them; what an element has loaded stays.
  bool register_blob(StringView url, ConstByteView bytes);
  void revoke_blob(StringView url);

  // --- Encrypted media: the keys of the MediaKeys objects, and which of them an element decrypts with (setMediaKeys(); 0 for none).
  media::EmeHub& eme() { return eme_; }
  void video_set_keys(uint32_t serial, uint32_t keys);
  // Where playback is for the element a media source is attached to (microseconds; 0 if it is not attached to one).
  int64_t mse_playhead(uint32_t source);

  struct VideoState {
    bool known = false, ready = false, paused = true, ended = false, loop = false, muted = false, seeking = false;
    double current_time = 0, duration = 0, volume = 1, rate = 1, default_rate = 1;
    double buffered_end = 0, played_end = 0;   // seconds; the buffered and seekable range is 0 to buffered_end, the played one 0 to played_end
    static constexpr uint32_t kMaxRanges = 16;
    double buffered_ranges[kMaxRanges * 2], seekable_ranges[kMaxRanges * 2];   // start, end seconds; a media source says which (else: 0 to buffered_end)
    uint32_t buffered_count = 0, seekable_count = 0;
    bool dynamic = false;
    uint32_t width = 0, height = 0;
    uint8_t ready_state = 0;     // HAVE_NOTHING 0 .. HAVE_ENOUGH_DATA 4
    uint8_t network_state = 0;   // NETWORK_EMPTY 0, IDLE 1, LOADING 2, NO_SOURCE 3
    uint8_t error_code = 0;      // MediaError.code, 0 for none
  };
  void video_state(uint32_t serial, VideoState* out);
  // False if there is nothing that could play (no source, or one that failed): play() then rejects with NotSupportedError.
  // (`audio` and `has_source` say what the element is and whether it names a file: they are known to the script before the page has been through sync().)
  bool video_play(uint32_t serial, bool audio, bool has_source);
  void video_pause(uint32_t serial);
  void video_seek(uint32_t serial, double seconds);   // clamped to 0 .. duration; the position is kept for a file that has not opened yet
  void video_set_volume(uint32_t serial, double volume);
  void video_set_muted(uint32_t serial, bool muted);
  void video_set_loop(uint32_t serial, bool loop);
  void video_set_rate(uint32_t serial, double rate);           // playbackRate, 0 .. 16 (the caller checks)
  void video_set_default_rate(uint32_t serial, double rate);   // defaultPlaybackRate: what playbackRate is after load()
  void video_load(uint32_t serial);   // fetch again

 private:
  struct Scaled {
    ArrayView<uint32_t> pixels;
    uint32_t width = 0, height = 0, frame = 0xFFFFFFFFu;
  };
  struct CachedImage {
    explicit CachedImage(allocators::IAllocator& a) : url(a), image(a) {}
    String url;
    media::Image image;
    bool ok = false;
    uint64_t started_ms = 0;
  };
  struct ImageSlot {
    explicit ImageSlot(allocators::IAllocator& a) : url(a) {}
    uint32_t serial = 0;
    String url;
    int32_t cache = -1;
    uint8_t state = 0;           // 0 nothing to show yet, 1 loaded, 2 failed
    bool announced = false, seen = false;
    Scaled scaled;
  };
  struct VideoSlot {
    explicit VideoSlot(allocators::IAllocator& a) : url(a), poster(a) {}
    uint32_t serial = 0;
    String url, poster;
    int32_t poster_cache = -1;
    UniquePointer<IVideoClip> clip;
    uint8_t state = 0;           // 0 nothing yet, 1 ready, 2 failed
    bool audio = false, seen = false, controls = false, loop = false, muted = false, autoplay = false, ended = false, was_playing = false, announced = false, wants_play = false;
    bool has_source = false;     // the element names a file (it may not be fetched yet)
    uint32_t mse_source = 0;     // the MediaSource it plays (its address is a blob: one made from one)
    MseClip* mse_clip = nullptr;   // (the clip, when it is that kind)
    uint32_t keys = 0;           // the MediaKeys it decrypts with (EmeHub number), 0 for none
    bool waiting_key_sent = false;
    uint8_t last_ready = 0;      // (a clip that is fed while it plays) what readyState was at the last update, and what has been announced
    bool loaded_data = false, waiting_sent = false;
    int64_t last_duration_us = -2;
    double last_progress = -1;
    bool seeking = false;        // a seek was made; "seeked" follows at the next update()
    bool play_pending = false;   // "play" was fired while the file was still on its way; "playing" is owed
    bool emulate_rate = false;   // the clip cannot play at `rate`: update() keeps it on time by seeking
    bool throttled = false;      // (emulating a rate below 1) the clip is paused for a moment to let the clock catch up
    uint8_t error_code = 0;
    double volume = 1, rate = 1, default_rate = 1;
    int64_t pending_seek_us = -1, played_end_us = 0, anchor_us = 0;
    uint64_t anchor_ms = 0;      // the host clock and the clip's position when it last started playing, or changed speed or place
    uint64_t last_timeupdate_ms = 0;
    ArrayView<uint32_t> picture;
    uint32_t picture_serial = 0xFFFFFFFFu;
    bool have_picture = false;
    Scaled scaled, poster_scaled;
    int64_t position_us = 0;
  };
  struct SvgSlot {
    SvgSlot() = default;
    uint32_t serial = 0, width = 0, height = 0;
    uint64_t version = 0;       // the version of the document it was drawn for
    bool seen = false;
    UniquePointer<CanvasBitmap> bitmap;
  };
  struct CanvasSlot {
    CanvasSlot() = default;
    uint32_t serial = 0;
    UniquePointer<CanvasBitmap> bitmap;
    uint32_t width = 0, height = 0;
  };

  ImageSlot* find_image(uint32_t serial);
  VideoSlot* find_video(uint32_t serial);
  VideoSlot* ensure_video(uint32_t serial);   // (a script can set the volume or the rate of an element before the page has been drawn: the slot is made then)
  CanvasSlot* find_canvas(uint32_t serial);
  SvgSlot* find_svg(uint32_t serial);
  void sync_svg(dom::Document& doc, dom::Node* n);
  CanvasSlot* find_canvas_slot(uint32_t serial);
  int32_t load_image_url(IObjectEnvironment& env, StringView url, bool* failed);
  void sync_image(dom::Node* n, IObjectEnvironment& env, StringView base, const MediaEnvironment& media, uint32_t* fetched);
  void sync_video(dom::Node* n, IObjectEnvironment& env, StringView base, const MediaEnvironment& media, uint32_t* fetched);
  // The media element algorithms; the script side and the control bar both go through them, so the events are the same.
  bool play_slot(VideoSlot& v);
  void pause_slot(VideoSlot& v);
  void seek_slot(VideoSlot& v, int64_t microseconds);
  void apply_rate(VideoSlot& v);
  void apply_volume(VideoSlot& v);
  void reanchor(VideoSlot& v);
  void attach_mse(VideoSlot& v, dom::Node* n, uint32_t source, StringView address, IObjectEnvironment& env);
  void release_mse(VideoSlot& v);
  void watch_dynamic(VideoSlot& v);
  void ensure_picture(VideoSlot& v);
  void follow_clock(VideoSlot& v);
  static bool slot_playing(const VideoSlot& v) { return v.clip && (v.clip->playing() || v.throttled); }
  bool choose_source(const dom::Node* img, const MediaEnvironment& media, String* out);
  void queue(uint32_t serial, const char* type);
  void draw_scaled(graphics::IRenderer2D& r, Scaled& cache, const ImageSource& src, uint32_t frame, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip,
                   bool fit);
  void draw_video(graphics::IRenderer2D& r, VideoSlot& v, int32_t x, int32_t y, uint32_t w, uint32_t h, const Rect2D& clip);
  uint32_t image_frame(const CachedImage& c) const;
  void release_scaled(Scaled* s);

  allocators::IAllocator& allocator_;
  kernel::RecursiveMutex mutex_;
  List<UniquePointer<CachedImage>> cache_;
  List<UniquePointer<ImageSlot>> images_;
  List<UniquePointer<VideoSlot>> videos_;
  List<UniquePointer<CanvasSlot>> canvases_;
  List<UniquePointer<SvgSlot>> svgs_;
  List<Event> events_;
  media::MseHub mse_;
  media::EmeHub eme_;
  struct BlobEntry {
    explicit BlobEntry(allocators::IAllocator& a) : url(a), data(a) {}
    String url;
    ByteBuffer data;
  };
  List<UniquePointer<BlobEntry>> blobs_;
  uint64_t blob_bytes_ = 0;
  static constexpr uint64_t kMaxBlobBytes = 128u << 20;
  bool fetch_blob(StringView url, ByteBuffer* out);   // (a blob: address)
  uint64_t canvas_pixels_ = 0, now_ms_ = 0;
  uint32_t fetched_images_ = 0;
  bool dirty_ = false, accelerated_ = false;
};

}  // namespace browser
