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
#include "Allocators/IAllocator.hpp"
#include "Audio/AudioPlayer.hpp"
#include "Graphics/GraphicsTypes.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Hardware/VideoCodec/VideoFrameBuffer.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Media/AudioStreaming.hpp"
#include "Media/ByteSource.hpp"
#include "Media/Image.hpp"
#include "Media/Mp4.hpp"
#include "Media/Webm.hpp"
#include "Utility/UniquePointer.hpp"
#include "Video/AviFile.hpp"

namespace video {

enum class VideoState : uint8_t { Empty, Stopped, Playing, Paused };

enum class OpenResult : uint8_t {
  Ok,
  NotAvi,       // not an AVI file, or no video in it
  Unsupported,  // open_media(): not a video format this OS reads (see media::VideoFileFormat)
  NoDecoder,    // no video codec device can decode this stream (H.265 needs a hardware engine driver)
  OutOfMemory,
};

// Plays an AVI file (or, with open_media(), an MP4 / MOV file with H.264, VP9 or AV1 video, a WebM / Matroska file with VP8, VP9 or AV1 video and Opus or Vorbis sound, or a GIF, APNG, animated WebP or Y4M file -
// decoded up front into frames that carry their own durations, and silent): the picture through a video decoder session from the machine's codec devices,
// the sound (if the file has PCM audio and a mixer is given) through an audio::AudioPlayer. The sound is the clock
// while it plays - the picture follows it, skipping frames when decoding falls behind - and with no sound the picture
// follows the system clock. update() does the work and must be called often (every 10 ms or so) while playing, or
// start_service() has a thread do it. The current picture is a BGRA surface owned by the player; picture_serial()
// changes whenever it does. Thread safe.
class VideoPlayer {
 public:
  VideoPlayer(const hardware::IClock& clock, hardware::SystemVideoCodecs& codecs, allocators::IAllocator& allocator,
              audio::AudioMixer* mixer = nullptr)
      : clock_(clock), codecs_(codecs), allocator_(allocator), mixer_(mixer), file_(allocator), images_(allocator), frame_starts_(allocator),
        packet_(allocator), y4m_head_(allocator), y4m_frame_(allocator), mp4_a_(allocator), mp4_b_(allocator), codec_config_(allocator), webm_(allocator), seq_dec_of_rank_(allocator), seq_rank_of_dec_(allocator),
        seq_times_(allocator), seq_sync_(allocator), display_(allocator) {}
  ~VideoPlayer();
  VideoPlayer(const VideoPlayer&) = delete;
  VideoPlayer& operator=(const VideoPlayer&) = delete;

  // Opens `avi` (borrowed: it must outlive the player or the next open()/close()), replacing what was open. The first
  // picture is decoded at once, so a stopped or paused player shows something.
  OpenResult open(ConstByteView avi);
  // Opens a video file from a source - an AVI or MP4 / MOV file is read from it a frame at a time while it plays (and so is its sound), so the
  // file is never held in memory; the formats that are decoded up front (GIF, APNG, WebP, Y4M, a still picture) are read in whole (at most
  // kMaxInMemoryBytes) and then forgotten. The source must outlive the player or the next open()/close().
  OpenResult open_media(media::IByteSource& source);
  static constexpr uint64_t kMaxInMemoryBytes = 256ull << 20;
  // Opens any video file this OS reads, by looking at it: AVI as open(), GIF/APNG/WebP/Y4M as frames in memory. `data` is
  // borrowed for AVI only (the other formats are decoded into the player). A still picture opens as a one-frame video.
  OpenResult open_media(ConstByteView data);
  void close();
  // Carries on with a longer copy of the MP4 file that is playing (a stream being received: what open_media() was given, cut off at the front at a
  // sync sample and with more samples at the end), without decoding anything again: the decoder, the picture and the playing state stay as they are.
  // Time 0 of the new file is `shift_us` after time 0 of the old one. False, with nothing changed, if `source` is not such a file or the player is not
  // at a place the new file has (the caller then opens it afresh). On success `source` replaces the old one and must outlive the player like it.
  bool continue_media(media::IByteSource& source, int64_t shift_us);

  bool play();
  void pause();
  // Stops and goes back to the start.
  void stop();
  // Moves to `time_us` (clamped), playing or not.
  void seek(int64_t time_us);
  void set_looping(bool looping);
  bool looping() const { return looping_; }

  // (These, the sizes and the counts below are answered from what the service thread last published while one runs: it changes them as it decodes.)
  VideoState state() const { return service_running() ? published().state : state_; }
  bool playing() const { return state() == VideoState::Playing; }
  int64_t position_us();
  int64_t duration_us() const { return service_running() ? published().duration_us : duration_us_; }
  uint32_t frame_width() const { return service_running() ? published().width : width_; }
  uint32_t frame_height() const { return service_running() ? published().height : height_; }
  uint32_t current_frame() const { return current_frame_; }
  uint32_t frame_count() const {
    return y4m_mode_ ? static_cast<uint32_t>(y4m_frames_) : image_mode_ ? images_.frame_count() : seq_mode() ? static_cast<uint32_t>(seq_dec_of_rank_.count()) : file_.frame_count();
  }
  // The picture now to be shown (nullptr when nothing is open). The pixels may change at the next update(): copy or
  // draw under no concurrent update (use copy_picture() when a service thread runs), or re-read it when picture_serial() changes.
  const graphics::Surface* picture() const { return state_ == VideoState::Empty ? nullptr : &surface_; }
  uint32_t picture_serial() const { return serial_; }
  // Copies the current picture into `dest` (rows of `pitch` pixels) under the picture's lock, so it is whole even while a
  // service thread decodes (and it does not wait for a frame being decoded); sets *serial to the picture's serial. False
  // if there is no picture, or `pitch` or `dest` is too small for it.
  bool copy_picture(ArrayView<uint32_t> dest, uint32_t pitch, uint32_t* serial);

  bool has_audio() const { return audio_player_ != nullptr; }
  audio::AudioPlayer* audio() { return audio_player_.get(); }
  const AviFile& file() const { return file_; }
  uint64_t frames_decoded() const { return service_running() ? published().decoded : decoded_; }
  uint64_t frames_skipped() const { return service_running() ? published().skipped : skipped_; }
  // Pictures put on show (a decoded frame that was skipped because playing fell behind counts in frames_skipped() instead).
  uint64_t frames_presented() const { return service_running() ? published().presented : presented_; }

  void update();
  bool start_service();
  void stop_service();
  bool service_running() const { return __atomic_load_n(&service_running_, __ATOMIC_ACQUIRE); }

 private:
  static constexpr uint32_t kMaxCatchUpFrames = 12;
  static constexpr uint32_t kHiddenFrameReach = 3;   // how many frames before the one wanted are shown anyway (frames that show nothing may follow them)
  static void service_main(void* self);
  void release();
  bool decode_frame(uint32_t index);
  bool all_keyframes() const { return image_mode_ || (!seq_mode() && file_.all_keyframes()); }
  bool seq_mode() const { return seq_source_ != SeqSource::None; }
  uint32_t frame_at_time(int64_t time_us) const;
  OpenResult open_images();
  OpenResult open_mp4(media::IByteSource& source);
  OpenResult open_webm(media::IByteSource& source);
  OpenResult finish_avi();
  bool open_y4m_stream(media::IByteSource& source, OpenResult* result);
  int64_t y4m_start_us(uint64_t frame) const { return static_cast<int64_t>((frame * 1000 * y4m_rate_den_ / y4m_rate_num_) * 1000); }
  bool show_decoded(const hardware::VideoFrame& frame);
  bool seq_show(uint32_t target);
  bool seq_packet(uint32_t decode_index, hardware::VideoPacket* packet) const;
  bool seq_build_avi();
  bool seq_build_mp4(const media::Mp4Track& track);
  bool seq_build_webm(const media::WebmTrack& track);
  void seq_clear();
  void open_audio();
  void start_audio(audio::IPcmSource* source);
  void show_frame(uint32_t target);
  int64_t position_locked();
  void rebase(int64_t position_us);
  void publish_clock();

  const hardware::IClock& clock_;
  hardware::SystemVideoCodecs& codecs_;
  allocators::IAllocator& allocator_;
  audio::AudioMixer* mixer_;
  kernel::Mutex mutex_;           // everything but what the two below guard; held while a frame is decoded
  kernel::Mutex picture_mutex_;   // the pixels of display_ (taken inside mutex_ by whoever writes them; alone by copy_picture(), which so never waits for a decode)
  mutable kernel::Mutex clock_mutex_;   // published_ (taken inside mutex_ by publish_clock(); alone by whoever reads it)
  // Where playing is and what the player's state is, as of the last change or update(): what the other threads are answered from while a service
  // thread decodes, without waiting for it.
  struct Published {
    VideoState state = VideoState::Empty;
    int64_t position_us = 0, duration_us = 0;
    uint64_t time_us = 0;   // (the system time position_us was true at)
    uint32_t width = 0, height = 0;
    uint64_t decoded = 0, skipped = 0, presented = 0;
  } published_;
  Published published() const {
    kernel::ScopedMutex guard(clock_mutex_);
    return published_;
  }

  AviFile file_;
  media::Image images_;            // the frames of a GIF/APNG/Y4M file
  List<int64_t> frame_starts_;     // when each frame of images_ starts (microseconds), and the end after the last
  media::MemoryByteSource memory_;   // a file given as memory
  media::IByteSource* source_ = nullptr;   // where the open AVI / MP4 file is read from
  mutable ByteBuffer packet_;        // the packet being decoded, when it had to be read from the file
  // A Y4M file read frame by frame: every frame is the same size, so frame i is found by arithmetic.
  bool y4m_mode_ = false;
  ByteBuffer y4m_head_;              // the stream header line
  media::Image y4m_frame_;           // the frame being shown, decoded
  uint64_t y4m_first_ = 0, y4m_frame_bytes_ = 0, y4m_step_ = 0, y4m_frames_ = 0, y4m_rate_num_ = 1, y4m_rate_den_ = 1;
  bool image_mode_ = false;
  // Streams whose pictures come out of the decoder later than they go in (H.264): packets are fed in decode order, and
  // the pictures come back in display order, each display position being a "frame" for the player.
  enum class SeqSource : uint8_t { None, Avi, Mp4, Webm };
  SeqSource seq_source_ = SeqSource::None;
  media::Mp4File mp4_a_, mp4_b_;     // (two, so that continue_media() can read the new file before it lets go of the old one)
  media::Mp4File* mp4_ = &mp4_a_;    // the one of the file that is open
  ByteBuffer codec_config_;          // the MP4 track's decoder configuration: the decoder's own copy (the file it came from may be replaced, see continue_media())
  const media::Mp4Track* mp4_track_ = nullptr;
  media::WebmFile webm_;
  const media::WebmTrack* webm_track_ = nullptr;
  int64_t webm_origin_us_ = 0;       // the time of the first picture in the file: the video starts there
  bool seq_one_to_one_ = false;      // a decoder that gives its picture (if the packet shows one) at once: a packet without a picture is a hidden frame
  List<uint32_t> seq_dec_of_rank_;  // decode index of the frame at each display position
  List<uint32_t> seq_rank_of_dec_;
  List<int64_t> seq_times_;         // when each display position starts, and the end after the last
  List<uint8_t> seq_sync_;          // per decode index: can start decoding here
  uint32_t seq_fed_ = 0;            // next packet to feed (decode index)
  uint32_t seq_next_rank_ = 0;      // display position of the next picture the decoder gives
  bool seq_started_ = false;
  bool seq_flushed_ = false;        // the decoder was told the stream has ended (it gave the pictures it held back)
  UniquePointer<hardware::IVideoDecoder> decoder_;
  hardware::VideoFrameBuffer display_;
  graphics::Surface surface_;
  UniquePointer<audio::IPcmSource> audio_stream_;   // the sound, decoded as it plays (reads from source_)
  UniquePointer<audio::AudioPlayer> audio_player_;

  VideoState state_ = VideoState::Empty;
  uint32_t width_ = 0, height_ = 0;
  int64_t duration_us_ = 0;
  uint32_t current_frame_ = 0;
  uint32_t serial_ = 0;
  bool have_picture_ = false;
  bool looping_ = false;
  // Without sound: the position was base_position_us_ at system time base_time_us_.
  int64_t base_position_us_ = 0;
  uint64_t base_time_us_ = 0;
  int64_t paused_position_us_ = 0;
  uint64_t decoded_ = 0, skipped_ = 0, presented_ = 0;
  kernel::Thread* service_ = nullptr;
  bool service_running_ = false;   // (atomic: position_us() asks from any thread)
  bool stop_ = false;
  bool picture_ready_ = false;     // display_ holds a picture of what is open (under picture_mutex_)
};

}  // namespace video
