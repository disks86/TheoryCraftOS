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

#include "Audio/AudioRouter.hpp"
#include "Utility/FixedString.hpp"

namespace audio {

using namespace hardware;

void AudioRouter::notify(const char* what, const char* name) {
  if (!notifier_) return;
  FixedString<128> text;
  text.assign(what);
  text.append(": ");
  text.append(name);
  notifier_(notifier_context_, text.view());
}

void AudioRouter::use_output(SharedPointer<IAudioDevice> device) {
  if (mixer_ && mixer_->set_device(device)) notify("Audio output", device->name());
}

void AudioRouter::on_added(const AudioEvent& event) {
  if (mixer_ && event.device->has_output()) use_output(event.device);
  if (hub_) {
    IAudioCapture* input = event.device->capture();
    if (input) {
      hub_->set_source(input, event.device);
      notify("Audio input", input->name());
    }
  }
}

void AudioRouter::on_removed(const AudioEvent& event) {
  if (mixer_ && mixer_->current_device() == event.device.get()) {
    // The first other hardware output, else whatever else plays (the software fallback).
    SharedPointer<IAudioDevice> best;
    for (int pass = 0; pass < 2 && !best; ++pass) {
      for (uint32_t i = 0; i < devices_.count() && !best; ++i) {
        SharedPointer<IAudioDevice> candidate = devices_.share(i);
        if (candidate && candidate->has_output() && candidate->present() && (pass == 1 || devices_.is_hardware(i))) best = candidate;
      }
    }
    if (best) use_output(best);
  }
  if (hub_ && hub_->source_device() == event.device.get()) {
    // The first other hardware input, else whatever else records (the software fallback's silence).
    SharedPointer<IAudioDevice> best;
    IAudioCapture* input = nullptr;
    for (int pass = 0; pass < 2 && !input; ++pass) {
      for (uint32_t i = 0; i < devices_.count() && !input; ++i) {
        SharedPointer<IAudioDevice> candidate = devices_.share(i);
        if (candidate && candidate->present() && candidate->capture() && (pass == 1 || devices_.is_hardware(i))) {
          best = candidate;
          input = candidate->capture();
        }
      }
    }
    hub_->set_source(input, best);
    notify("Audio input", input ? input->name() : "none");
  }
}

uint32_t AudioRouter::poll() {
  uint32_t handled = 0;
  AudioEvent event;
  while (devices_.poll(&event)) {
    ++handled;
    if (event.kind == AudioEvent::Kind::Added) on_added(event);
    else on_removed(event);
  }
  return handled;
}

bool AudioRouter::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "audio-hotplug");
  return service_ != nullptr;
}

void AudioRouter::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void AudioRouter::service_main(void* self) {
  AudioRouter* router = static_cast<AudioRouter*>(self);
  while (!__atomic_load_n(&router->stop_, __ATOMIC_ACQUIRE)) {
    router->poll();
    kernel::sleep_us(kPollIntervalUs);
  }
}

}  // namespace audio
