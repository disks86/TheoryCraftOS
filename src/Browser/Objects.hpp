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
//             controls. play() / pause() / currentTime / volume and the media events are the script side of the same thing.
//   <audio>   the same, without a picture: the file (MP3, AAC / M4A, Ogg Vorbis, FLAC, WAV, AIFF, AU, MIDI ...) is handed to the environment to play
//             (open_audio()); only an element with controls takes room on the page - a bar with play / pause, a position and the time. `new Audio(src)` too.
//   <canvas>  a CanvasBitmap (see Canvas.hpp), drawn by the renderer the page is shown on - the GPU's when the machine has one.
//
// All of it is shared between the thread the page runs on and the one that draws, so every call takes the object lock (a recursive one: scripts hold it
// across a sequence of calls through Lock).
#pragma once
#include "Browser/Canvas.hpp"
#include "Browser/ObjectTypes.hpp"
#include "Browser/Media.hpp"
#include "Dom/Dom.hpp"
#include "Graphics/Graphics2D/IRenderer2D.hpp"
#include "Kernel/Sync.hpp"
#include "Media/Image.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

// A video the environment is playing for a page.
class IVideoClip {
 public:
  virtual bool play() = 0;
  virtual void pause() = 0;
  virtual void seek(int64_t microseconds) = 0;
  virtual void set_looping(bool looping) = 0;
  virtual void set_volume(double volume, bool muted) = 0;
  virtual bool playing() = 0;
  virtual int64_t position_us() = 0;
  virtual int64_t duration_us() = 0;
  virtual uint32_t width() = 0;
  virtual uint32_t height() = 0;
  // Does what playing takes (decodes, follows the clock); called every few milliseconds.
  virtual void update() = 0;
  // Copies the current picture (width() x height(), 0xAARRGGBB, `pitch` pixels per row, so `dest` needs pitch * height() pixels) and says which picture it
  // is; false if there is none or `dest` is too small.
  virtual bool copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* serial) = 0;

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
  };
  // The next event to fire at an element (load, error, loadedmetadata, play, pause, ended, timeupdate ...); false when there is none.
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
  struct VideoState {
    bool known = false, ready = false, paused = true, ended = false, loop = false, muted = false;
    double current_time = 0, duration = 0, volume = 1;
    uint32_t width = 0, height = 0;
  };
  void video_state(uint32_t serial, VideoState* out);
  bool video_play(uint32_t serial);
  void video_pause(uint32_t serial);
  void video_seek(uint32_t serial, double seconds);
  void video_set_volume(uint32_t serial, double volume);
  void video_set_muted(uint32_t serial, bool muted);
  void video_set_loop(uint32_t serial, bool loop);
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
    double volume = 1;
    uint64_t last_timeupdate_ms = 0;
    ArrayView<uint32_t> picture;
    uint32_t picture_serial = 0xFFFFFFFFu;
    bool have_picture = false;
    Scaled scaled, poster_scaled;
    int64_t position_us = 0;
  };
  struct CanvasSlot {
    CanvasSlot() = default;
    uint32_t serial = 0;
    UniquePointer<CanvasBitmap> bitmap;
    uint32_t width = 0, height = 0;
  };

  ImageSlot* find_image(uint32_t serial);
  VideoSlot* find_video(uint32_t serial);
  CanvasSlot* find_canvas(uint32_t serial);
  CanvasSlot* find_canvas_slot(uint32_t serial);
  int32_t load_image_url(IObjectEnvironment& env, StringView url, bool* failed);
  void sync_image(dom::Node* n, IObjectEnvironment& env, StringView base, const MediaEnvironment& media, uint32_t* fetched);
  void sync_video(dom::Node* n, IObjectEnvironment& env, StringView base, const MediaEnvironment& media, uint32_t* fetched);
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
  List<Event> events_;
  uint64_t canvas_pixels_ = 0, now_ms_ = 0;
  uint32_t fetched_images_ = 0;
  bool dirty_ = false, accelerated_ = false;
};

}  // namespace browser
