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
#include "Audio/AudioMixer.hpp"
#include "Audio/Level.hpp"
#include "Audio/PcmSource.hpp"
#include "Kernel/Sync.hpp"

namespace audio {

enum class PlayerState : uint8_t {
  Empty,    // nothing loaded
  Stopped,  // loaded, at the start (or at the end, once it has played through)
  Playing,
  Paused,
};

// Plays one IPcmSource through the sound server: a transport (play, pause, stop, seek, loop) on top
// of an AudioStream. update() feeds the stream from the source and notices the end; call it every
// 10-20 ms while playing, or start_service() to have a thread do it. Each player has its own volume,
// pan and mute (those of its stream), so several players mix. Thread safe.
class AudioPlayer {
 public:
  using Notify = void (*)(void* context);

  AudioPlayer(AudioMixer& mixer, allocators::IAllocator& allocator) : mixer_(mixer), allocator_(allocator) {}
  ~AudioPlayer();
  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  // Loads `source` (borrowed: it must outlive the player or the next open()/close()), replacing and
  // stopping the previous one. False if the mixer has no stream to give or the format cannot be played.
  bool open(IPcmSource* source);
  void close();

  // Plays from the position (or the start, if the end was reached). False if nothing is loaded.
  bool play();
  void pause();
  // Stops and goes back to the start.
  void stop();
  // Moves the position (clamped to the length), playing or not.
  void seek(uint64_t frame);
  void set_looping(bool looping);
  bool looping() const { return looping_; }

  PlayerState state() const { return state_; }
  bool playing() const { return state_ == PlayerState::Playing; }
  // In frames of the source (divide by sample_rate() for seconds). The position is what is being
  // played, give or take the mixer's short delay: the frames fed minus those still queued.
  uint64_t position();
  uint64_t duration() const { return duration_; }
  uint32_t sample_rate() const { return rate_; }

  void set_volume(uint8_t volume);
  uint8_t volume() const { return volume_; }
  void set_pan(int pan);
  int pan() const { return pan_; }
  void set_muted(bool muted);
  bool muted() const { return muted_; }
  // The loudest sample played since the last call, 0 .. 32767 (see peak_to_percent()).
  uint32_t take_peak();

  // Feeds the stream; moves to Stopped when the end is reached and played out.
  void update();
  bool start_service();
  void stop_service();
  // Called (from whichever thread changed the state) after the state changes.
  void on_state_changed(Notify notify, void* context) { notify_ = notify; notify_context_ = context; }

 private:
  static constexpr uint32_t kChunkBytes = 4096;
  static void service_main(void* self);
  void apply_stream_settings();
  void change_state(PlayerState state, bool* notify);

  AudioMixer& mixer_;
  allocators::IAllocator& allocator_;
  kernel::Mutex mutex_;
  IPcmSource* source_ = nullptr;
  SharedPointer<AudioStream> stream_;
  PlayerState state_ = PlayerState::Empty;
  uint64_t duration_ = 0;
  uint64_t next_frame_ = 0;  // the next source frame to feed
  uint32_t rate_ = 0;
  uint32_t frame_size_ = 0;
  bool looping_ = false;
  uint8_t volume_ = 100;
  int pan_ = 0;
  bool muted_ = false;
  Notify notify_ = nullptr;
  void* notify_context_ = nullptr;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace audio
