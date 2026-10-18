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

#include "Hardware/MidiDevice/Usb/UsbMidiService.hpp"
#include "Kernel/Thread.hpp"

namespace hardware {

UsbMidiService::UsbMidiService(IUsbBus* usb, audio::AudioMixer* mixer, allocators::IAllocator& allocator)
    : usb_(usb), mixer_(mixer), allocator_(allocator), devices_(allocator), watcher_(usb) {
  if (mixer_) live_ = UniquePointer<midi::LiveSynth>::make(allocator, *mixer_, allocator);
}

UsbMidiService::~UsbMidiService() { stop(); }

bool UsbMidiService::start() {
  if (started_) return true;
  started_ = true;
  {
    kernel::ScopedMutex lock(mutex_);
    scan_devices();
  }
  watcher_.snapshot();   // (what is there now has been looked at)
  if (kernel::running()) {
    __atomic_store_n(&stop_, false, __ATOMIC_RELEASE);
    thread_ = kernel::spawn(&UsbMidiService::thread_main, this, "usb-midi");
    return thread_ != nullptr;
  }
  return true;
}

void UsbMidiService::stop() {
  if (thread_) {
    __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
    kernel::join(thread_);
    thread_ = nullptr;
  }
  kernel::ScopedMutex lock(mutex_);
  if (live_) live_->stop();
  devices_.clear();
  started_ = false;
}

void UsbMidiService::thread_main(void* self) {
  auto* s = static_cast<UsbMidiService*>(self);
  uint32_t round = 0;
  while (!__atomic_load_n(&s->stop_, __ATOMIC_ACQUIRE)) {
    s->step(++round % kRoundsPerScan == 0);
    kernel::sleep_us(2000);
  }
}

// --- Devices --------------------------------------------------------------------------------------

// Takes over the MIDI devices on every port (at start-up; later UsbPortWatcher says which ports have something new).
void UsbMidiService::scan_devices() {
  if (!usb_) return;
  for (uint32_t c = 0; c < usb_->controller_count(); ++c) {
    IUsbHostController* host = usb_->host_controller(c);
    for (uint32_t p = 0; host && p < host->port_count(); ++p) {
      UniquePointer<UsbMidiDevice> device = UsbMidiDevice::open(*host, p, allocator_);
      if (device && devices_.push_back(util::move(device))) ++added_;
    }
  }
  update_live_synth();
}

void UsbMidiService::drop_gone_devices() {
  for (uint64_t i = devices_.count(); i > 0; --i)
    if (!devices_[i - 1]->present()) {
      devices_.remove_at(i - 1);
      ++removed_;
    }
}

// The live synthesizer plays while there is a keyboard to play it.
void UsbMidiService::update_live_synth() {
  if (!live_) return;
  bool input = false;
  for (const auto& d : devices_) input = input || d->has_input();
  if (input && !live_->running()) live_->start();
  else if (!input && live_->running()) live_->stop();
}

uint32_t UsbMidiService::device_count() const {
  kernel::ScopedMutex lock(mutex_);
  return static_cast<uint32_t>(devices_.count());
}

bool UsbMidiService::device_info(uint32_t index, DeviceInfo* out) const {
  kernel::ScopedMutex lock(mutex_);
  if (index >= devices_.count()) return false;
  const UsbMidiDevice& d = *devices_[index];
  uint32_t n = 0;
  for (; d.name()[n] && n + 1 < sizeof out->name; ++n) out->name[n] = d.name()[n];
  out->name[n] = 0;
  out->input = d.has_input();
  out->output = d.has_output();
  out->synthesizer = d.is_synthesizer();
  return true;
}

// --- Routing ----------------------------------------------------------------------------------------

midi::IMidiOutput* UsbMidiService::sound_maker() {
  for (const auto& d : devices_)
    if (d->available()) return d.get();
  if (fallback_ && fallback_->available()) return fallback_;
  return nullptr;
}

bool UsbMidiService::available() const {
  kernel::ScopedMutex lock(mutex_);
  for (const auto& d : devices_)
    if (d->available()) return true;
  return fallback_ && fallback_->available();
}

void UsbMidiService::send(uint8_t status, uint8_t data1, uint8_t data2) {
  kernel::ScopedMutex lock(mutex_);
  if (midi::IMidiOutput* out = sound_maker()) out->send(status, data1, data2);
}

void UsbMidiService::reset() {
  kernel::ScopedMutex lock(mutex_);
  if (midi::IMidiOutput* out = sound_maker()) out->reset();
}

void UsbMidiService::flush() {
  kernel::ScopedMutex lock(mutex_);
  if (midi::IMidiOutput* out = sound_maker()) out->flush();
}

// A key was pressed on a keyboard (called with the lock held): to a sound maker, else to the live synthesizer.
void UsbMidiService::route_input(uint8_t status, uint8_t data1, uint8_t data2) {
  ++routed_;
  if (midi::IMidiOutput* out = sound_maker()) {
    out->send(status, data1, data2);
    return;
  }
  if (live_ && live_->running()) live_->send(status, data1, data2);
}

void UsbMidiService::on_message(void* self, uint8_t status, uint8_t data1, uint8_t data2) {
  static_cast<UsbMidiService*>(self)->route_input(status, data1, data2);
}

void UsbMidiService::step(bool scan) {
  kernel::ScopedMutex lock(mutex_);
  if (scan && usb_) {
    drop_gone_devices();
    watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) {
      UniquePointer<UsbMidiDevice> device = UsbMidiDevice::open(host, port, allocator_);
      if (device && devices_.push_back(util::move(device))) ++added_;
    });
    update_live_synth();
  }
  for (const auto& d : devices_)
    if (d->has_input()) d->poll_input(&UsbMidiService::on_message, this);
  if (midi::IMidiOutput* out = sound_maker()) out->flush();
  if (live_ && live_->running() && !kernel::running()) live_->update();   // (without threads the player is fed here)
}

}  // namespace hardware
