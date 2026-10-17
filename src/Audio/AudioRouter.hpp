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
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Hardware/AudioDevice/SystemAudioDevices.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/StringView.hpp"

namespace audio {

// Keeps the sound server pointed at the right devices while USB audio devices come and go. It takes the changes SystemAudioDevices reports and
//  - when a device that plays sound is plugged in, the mixer switches to it (what you plugged in is what you want to hear);
//  - when the device the mixer plays to is pulled out, the mixer switches to the first other hardware output, else to the software fallback
//    (SystemAudioDevices brings that up when the last output goes);
//  - the capture hub follows the same rules for microphones (a new input is taken up; when the one in use leaves, the first other hardware
//    input, else the software fallback's silence).
// Programs keep their streams throughout. Either of the mixer and the hub may be null.
class AudioRouter {
 public:
  static constexpr uint64_t kPollIntervalUs = 250000;
  // Told about each switch ("Audio output: ..."), from the thread that polls. May be null.
  using Notifier = void (*)(void* context, StringView text);

  AudioRouter(hardware::SystemAudioDevices& devices, AudioMixer* mixer, AudioCaptureHub* hub) : devices_(devices), mixer_(mixer), hub_(hub) {}
  ~AudioRouter() { stop_service(); }
  AudioRouter(const AudioRouter&) = delete;
  AudioRouter& operator=(const AudioRouter&) = delete;

  void set_notifier(Notifier notifier, void* context) { notifier_ = notifier; notifier_context_ = context; }

  // Looks for devices that came or went and routes accordingly; returns how many changes it handled. Call from one thread.
  uint32_t poll();
  // Runs poll() every 250 ms on a thread of its own ("audio-hotplug").
  bool start_service();
  void stop_service();

 private:
  void on_added(const hardware::AudioEvent& event);
  void on_removed(const hardware::AudioEvent& event);
  void use_output(SharedPointer<hardware::IAudioDevice> device);
  void notify(const char* what, const char* name);
  static void service_main(void* self);

  hardware::SystemAudioDevices& devices_;
  AudioMixer* mixer_;
  AudioCaptureHub* hub_;
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace audio
