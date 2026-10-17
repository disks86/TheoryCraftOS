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
#include "Audio/AudioStream.hpp"
#include "Hardware/AudioDevice/IAudioDevice.hpp"
#include "Kernel/Thread.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace audio {

struct MixerStatistics {
  uint64_t frames_mixed = 0;      // frames written to the device
  uint64_t clipped_samples = 0;   // samples the sum of the streams pushed past full scale
  uint64_t streams_opened = 0;
  uint32_t active_streams = 0;
};

// A sound server for one audio device. Hardware plays a single stream, so the
// mixer is the one program that owns the device: any number of programs each
// open_stream() their own AudioStream, write to it whenever they like, and the
// mixer adds all the streams together in real time into the device.
//
// Each stream can use its own format (see StreamFormat), volume and pan; the
// stream converts on write.
//
// pump() does the work: it tops the device up to a short target delay (about
// 50 ms), sums the queued audio of every stream (each at its own volume) with
// clipping, and drops streams that are closed and drained. start_service() runs
// pump() from a thread every few milliseconds; without it, call pump() often.
// When nothing is queued nothing is written, and the device plays silence.
//
// Master volume and mute go to the device. The device can be exchanged while the mixer runs (set_device(): a USB headset was plugged in
// or the one in use was pulled out): the streams carry on into the new device and the master volume and mute are applied to it. Thread safe.
class AudioMixer {
 public:
  static constexpr uint32_t kMaxStreams = 32;
  static constexpr uint32_t kTargetDelayFrames = hardware::kAudioSampleRate / 20;  // 50 ms
  static constexpr uint32_t kChunkFrames = 1024;
  static constexpr uint32_t kPumpIntervalUs = 5000;

  // The mixer plays into `device`, which must outlive it (and must not be one that is removed while the mixer runs: use the next constructor).
  AudioMixer(hardware::IAudioDevice& device, allocators::IAllocator& allocator)
      : device_(&device), allocator_(allocator), streams_(allocator), volume_(device.volume()), muted_(device.muted()) {}
  // The mixer shares the device, so it stays valid for as long as the mixer uses it.
  AudioMixer(SharedPointer<hardware::IAudioDevice> device, allocators::IAllocator& allocator)
      : device_(device.get()), owner_(device), allocator_(allocator), streams_(allocator), volume_(device->volume()), muted_(device->muted()) {}
  ~AudioMixer() {
    stop_service();
    release_view(allocator_, accumulator_);
    release_view(allocator_, output_);
  }
  AudioMixer(const AudioMixer&) = delete;
  AudioMixer& operator=(const AudioMixer&) = delete;

  // Allocates the mixing buffers. False if out of memory.
  bool initialize();

  // A new stream, or an empty pointer at kMaxStreams / out of memory / an invalid format. The program writes
  // in `format` (default: the device's own, 48 kHz 16-bit stereo, which needs no conversion).
  SharedPointer<AudioStream> open_stream(const StreamFormat& format, uint32_t ring_bytes = AudioStream::kDefaultRingBytes);
  SharedPointer<AudioStream> open_stream(uint32_t ring_bytes = AudioStream::kDefaultRingBytes) {
    return open_stream(StreamFormat(), ring_bytes);
  }

  void pump();
  bool start_service();
  void stop_service();
  bool service_running() const { return service_ != nullptr; }

  // Switches to another device: what is queued for the old one is dropped (a few milliseconds), the streams are untouched, and the master
  // volume and mute are set on the new device. False for a null device.
  bool set_device(SharedPointer<hardware::IAudioDevice> device);
  // The device now in use (null never; the pointer is only good until the next set_device()).
  hardware::IAudioDevice* current_device();
  // The same, kept alive for as long as the caller holds it (null if the mixer was bound to a device by reference). Use this to look at the device
  // from another thread while devices may be exchanged.
  SharedPointer<hardware::IAudioDevice> shared_device();

  void set_master_volume(uint8_t volume);
  uint8_t master_volume() const;
  void set_master_muted(bool muted);
  bool master_muted() const;

  uint32_t stream_count();
  MixerStatistics statistics();
  // (the device in use; see current_device() when it can change)
  hardware::IAudioDevice& device() { return *device_; }

 private:
  static void service_main(void* self);
  void flush_pending();

  hardware::IAudioDevice* device_;
  SharedPointer<hardware::IAudioDevice> owner_;   // keeps the device alive when the mixer was given a shared one
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;  // guards the device in use, the stream list, the buffers and the statistics
  List<SharedPointer<AudioStream>> streams_;
  ArrayView<int32_t> accumulator_;  // kChunkFrames stereo frames
  ByteView output_;                 // the same, as 16-bit samples
  uint32_t pending_offset_ = 0;     // part of output_ the device has not taken yet
  uint32_t pending_bytes_ = 0;
  MixerStatistics stats_;
  uint8_t volume_;
  bool muted_;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace audio
