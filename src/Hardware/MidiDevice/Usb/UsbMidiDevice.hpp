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

// One USB MIDI device (a class-compliant keyboard, controller, interface or synthesizer): its MIDIStreaming interface's bulk endpoints.
//
//   Output: send() collects the messages as USB-MIDI event packets and flush() sends the batch to the bulk OUT endpoint (the batch also goes
//     out when it is full). The device is an IMidiOutput that is available() only when it is a sound maker - it described an element, see
//     UsbMidiModel::has_synth - because the notes sent to a keyboard's output go nowhere and playing a song on it would be silent.
//   Input: the bulk IN endpoint is kept armed on the host controller (xHCI and EHCI can do that, see IUsbHostController::start_bulk_in_stream)
//     and poll_input() hands out the channel messages that have arrived, without waiting. Controllers that cannot leave the device output only.
//
// Not thread safe by itself: the service that owns the device serializes the calls.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/MidiDevice/Usb/UsbMidiDescriptors.hpp"
#include "Midi/MidiTypes.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class UsbMidiDevice final : public midi::IMidiOutput {
 public:
  static constexpr uint32_t kBatchBytes = 64;   // 16 event packets

  // Takes over the device on a root port if it is a USB MIDI one and comes up; null otherwise.
  static UniquePointer<UsbMidiDevice> open(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator);

  UsbMidiDevice(IUsbHostController& host, uint32_t port, const UsbMidiModel& model, const UsbDeviceDescriptor& descriptor, allocators::IAllocator& allocator);
  ~UsbMidiDevice();

  bool initialize();
  const char* name() const { return name_; }
  // False once the device was unplugged (the port's generation moved on).
  bool present() const { return host_.port_generation(port_) == generation_; }
  bool has_output() const { return out_endpoint_ != 0; }
  bool has_input() const { return input_running_; }
  bool is_synthesizer() const { return model_.has_synth; }
  uint32_t port() const { return port_; }
  uint32_t generation() const { return generation_; }
  const IUsbHostController* host() const { return &host_; }

  // IMidiOutput
  bool available() const override { return model_.has_synth && has_output() && present(); }
  void send(uint8_t status, uint8_t data1, uint8_t data2) override;
  void reset() override;
  void flush() override;

  // Hands out the messages that have arrived from the device (all its cables): calls receiver for each; returns how many. Never waits.
  uint32_t poll_input(UsbMidiReceiver receiver, void* context);

 private:
  void read_name(const UsbDeviceDescriptor& descriptor);

  IUsbHostController& host_;
  uint32_t port_;
  allocators::IAllocator& allocator_;
  UsbMidiModel model_;
  uint32_t generation_ = 0;
  uint8_t out_endpoint_ = 0;
  bool input_running_ = false;
  ByteView batch_;             // memory the controller can reach
  uint32_t batch_length_ = 0;
  char name_[64] = "USB MIDI";
};

}  // namespace hardware
