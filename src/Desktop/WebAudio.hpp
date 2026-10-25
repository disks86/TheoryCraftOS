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

// What a web page's WebRTC uses of the machine's sound: the microphone through the capture hub, the speaker through the mixer, both at 8 kHz mono (the streams convert), and the
// user's yes or no for the microphone (asked through the dialog of WebNotifications).
#pragma once
#include "Audio/AudioCaptureHub.hpp"
#include "Audio/AudioMixer.hpp"
#include "Desktop/WebNotifications.hpp"
#include "Rtc/RtcAudio.hpp"

namespace desktop {

class DesktopRtcAudio final : public rtc::IRtcAudioDevices {
 public:
  DesktopRtcAudio(allocators::IAllocator& allocator, WebNotifications& prompts) : allocator_(allocator), prompts_(prompts) {}
  void set_mixer(audio::AudioMixer* mixer) { mixer_ = mixer; }
  void set_capture_hub(audio::AudioCaptureHub* hub) { capture_ = hub; }

  bool microphone_present() override { return capture_ && capture_->source() != nullptr; }
  int microphone_permission(StringView origin) override { return prompts_.microphone_state(origin); }
  int microphone_request(StringView origin) override { return prompts_.ask_microphone(origin); }
  UniquePointer<rtc::IRtcAudioSource> open_microphone() override;
  UniquePointer<rtc::IRtcAudioSink> open_speaker() override;

 private:
  allocators::IAllocator& allocator_;
  WebNotifications& prompts_;
  audio::AudioMixer* mixer_ = nullptr;
  audio::AudioCaptureHub* capture_ = nullptr;
};

}  // namespace desktop
