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

// Everything MIDI that is plugged into the USB ports: finds the devices (those present at start-up, and keyboards plugged in or pulled out later),
// reads what they play, and lets songs be played on the ones that make sound.
//
//   - Input. What a keyboard or controller sends is passed on to the first of these that exists: a USB device that is a synthesizer, the fallback
//     output (the machine's MPU-401 port, say), and the software synthesizer, played live through the sound mixer. So a plugged-in keyboard can
//     be played at once, with or without MIDI hardware.
//   - Output. The service is an IMidiOutput for the media player that is available() when a synthesizer is plugged in or the fallback output is,
//     and sends the notes of a song there.
//
// A thread polls the devices every couple of milliseconds and looks for new ones a few times a second (step() does one round, for machines and
// tests without threads). USB devices are served only on host controllers that can keep a bulk IN endpoint armed (xHCI, EHCI); on the others
// a MIDI device can still be played to, but its keys are not heard.
#pragma once
#include "Audio/AudioMixer.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiDevice.hpp"
#include "Kernel/Sync.hpp"
#include "Midi/LiveSynth.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

class UsbMidiService final : public midi::IMidiOutput {
 public:
  static constexpr uint32_t kRoundsPerScan = 100;   // (a round is about 2 ms)

  // `usb` and `mixer` may be null: without a bus there are no devices, without a mixer no live synthesizer.
  UsbMidiService(IUsbBus* usb, audio::AudioMixer* mixer, allocators::IAllocator& allocator);
  ~UsbMidiService();

  // Another output that keyboards are routed to, and songs are played on, when no USB synthesizer is there (the MPU-401). Must outlive the service.
  void set_fallback_output(midi::IMidiOutput* output) { fallback_ = output; }

  // Takes over the MIDI devices present now and starts the thread (if the kernel runs threads). False only if the thread cannot be started.
  bool start();
  void stop();
  // One round of the thread's work: reads the devices and passes on the keys that were pressed; with `scan` also looks for devices plugged in
  // or pulled out. Only for those who run no thread.
  void step(bool scan);

  struct DeviceInfo {
    char name[64] = {};
    bool input = false, output = false, synthesizer = false;
  };
  uint32_t device_count() const;
  bool device_info(uint32_t index, DeviceInfo* out) const;
  // Counts that grow as things happen, for logs and tests: devices taken over, devices lost, messages routed to somewhere.
  uint32_t devices_added() const { return added_; }
  uint32_t devices_removed() const { return removed_; }
  uint32_t messages_routed() const { return routed_; }
  bool live_synth_running() const { return live_ && live_->running(); }
  midi::LiveSynth* live_synth() { return live_.get(); }

  // IMidiOutput - the notes of a song.
  bool available() const override;
  void send(uint8_t status, uint8_t data1, uint8_t data2) override;
  void reset() override;
  void flush() override;

 private:
  static void thread_main(void* self);
  static void on_message(void* self, uint8_t status, uint8_t data1, uint8_t data2);
  void scan_devices();
  void drop_gone_devices();
  void update_live_synth();
  midi::IMidiOutput* sound_maker();   // (locked) the output songs and keys go to, or null
  void route_input(uint8_t status, uint8_t data1, uint8_t data2);

  IUsbBus* usb_;
  audio::AudioMixer* mixer_;
  allocators::IAllocator& allocator_;
  midi::IMidiOutput* fallback_ = nullptr;
  mutable kernel::Mutex mutex_;
  List<UniquePointer<UsbMidiDevice>> devices_;
  UsbPortWatcher watcher_;
  UniquePointer<midi::LiveSynth> live_;
  kernel::Thread* thread_ = nullptr;
  bool stop_ = false;
  bool started_ = false;
  uint32_t rounds_ = 0;
  uint32_t added_ = 0, removed_ = 0, routed_ = 0;
};

}  // namespace hardware
