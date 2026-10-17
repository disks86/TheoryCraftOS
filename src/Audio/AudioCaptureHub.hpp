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
#include "Audio/CaptureStream.hpp"
#include "Hardware/AudioDevice/IAudioCapture.hpp"
#include "Hardware/AudioDevice/IAudioDevice.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace audio {

struct CaptureHubStatistics {
  uint64_t frames_distributed = 0;  // frames taken from the device
  uint64_t streams_opened = 0;
  uint32_t active_streams = 0;
};

// The recording counterpart of AudioMixer: one microphone, many listeners. The
// hub owns the device's capture side; each program calls open_stream() and gets
// every frame recorded from then on, independently of the others.
//
// pump() moves recorded audio from the device into every open stream (and drops
// streams that were closed). With no stream open the device's recording is just
// discarded, so a stream opened later starts with sound from that moment, not
// old audio. start_service() runs pump() from a thread every few milliseconds.
//
// Master gain and mute go to the device. The source can be exchanged, or taken away, while the hub runs (set_source(): a USB microphone was
// plugged in or the one in use was pulled out): open streams stay open (they receive nothing while there is no source) and the master gain and
// mute are applied to the new source. Thread safe.
class AudioCaptureHub {
 public:
  static constexpr uint32_t kMaxStreams = 32;
  static constexpr uint32_t kChunkFrames = 1024;
  static constexpr uint32_t kPumpIntervalUs = 5000;

  // A hub on `device`, which must outlive it (and must not be one that is removed while the hub runs: use set_source()).
  AudioCaptureHub(hardware::IAudioCapture& device, allocators::IAllocator& allocator)
      : device_(&device), allocator_(allocator), streams_(allocator), gain_(device.gain()), muted_(device.muted()) {}
  // A hub with no source yet (set_source() gives it one when a microphone turns up).
  explicit AudioCaptureHub(allocators::IAllocator& allocator) : allocator_(allocator), streams_(allocator) {}
  ~AudioCaptureHub() {
    stop_service();
    release_view(allocator_, chunk_);
  }
  AudioCaptureHub(const AudioCaptureHub&) = delete;
  AudioCaptureHub& operator=(const AudioCaptureHub&) = delete;

  // Allocates the transfer buffer. False if out of memory.
  bool initialize();

  // A new stream, or an empty pointer at kMaxStreams / out of memory.
  // The program reads in `format` (default: the device's own, 48 kHz 16-bit stereo, which needs no conversion).
  SharedPointer<CaptureStream> open_stream(const StreamFormat& format, uint32_t ring_bytes = CaptureStream::kDefaultRingBytes);
  SharedPointer<CaptureStream> open_stream(uint32_t ring_bytes = CaptureStream::kDefaultRingBytes) {
    return open_stream(StreamFormat(), ring_bytes);
  }

  void pump();
  bool start_service();
  void stop_service();
  bool service_running() const { return service_ != nullptr; }

  // Records from `source` from now on, or from nothing (null). `owner` is the audio device it belongs to; the hub shares it, so it stays valid
  // while in use. The master gain and mute are set on the new source.
  void set_source(hardware::IAudioCapture* source, SharedPointer<hardware::IAudioDevice> owner);
  // The source in use (may be null; only good until the next set_source()).
  hardware::IAudioCapture* source();
  // The audio device the source belongs to (null when it was bound by reference or there is none).
  hardware::IAudioDevice* source_device();

  void set_master_gain(uint8_t gain);
  uint8_t master_gain() const;
  void set_master_muted(bool muted);
  bool master_muted() const;

  uint32_t stream_count();
  CaptureHubStatistics statistics();
  // (the source in use, which must exist; see source() when it can change)
  hardware::IAudioCapture& device() { return *device_; }

 private:
  static void service_main(void* self);

  hardware::IAudioCapture* device_ = nullptr;
  SharedPointer<hardware::IAudioDevice> owner_;   // keeps the source's device alive
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;  // guards the source, the stream list, the buffer and the statistics
  List<SharedPointer<CaptureStream>> streams_;
  ByteView chunk_;
  CaptureHubStatistics stats_;
  uint8_t gain_ = hardware::kAudioMaxVolume;
  bool muted_ = false;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace audio
