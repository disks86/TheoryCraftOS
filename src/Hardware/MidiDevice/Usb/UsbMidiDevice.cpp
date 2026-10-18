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

#include "Hardware/MidiDevice/Usb/UsbMidiDevice.hpp"

namespace hardware {

namespace {
constexpr uint8_t kSetInterface = 11;
constexpr uint8_t kStandardInterfaceOut = 0x01;
}

UniquePointer<UsbMidiDevice> UsbMidiDevice::open(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator) {
  UniquePointer<UsbMidiDevice> none;
  UsbDeviceDescriptor descriptor;
  uint8_t blob[2048];
  uint16_t length = 0;
  if (!host.device_descriptor(port, &descriptor) || !host.configuration_descriptor(port, ByteView(blob), &length)) return none;
  UsbMidiModel model;
  if (!parse_usb_midi_configuration(ConstByteView(blob).at_most(length), &model)) return none;
  UniquePointer<UsbMidiDevice> device = UniquePointer<UsbMidiDevice>::make(allocator, host, port, model, descriptor, allocator);
  if (!device || !device->initialize()) return none;
  return device;
}

UsbMidiDevice::UsbMidiDevice(IUsbHostController& host, uint32_t port, const UsbMidiModel& model, const UsbDeviceDescriptor& descriptor, allocators::IAllocator& allocator)
    : host_(host), port_(port), allocator_(allocator), model_(model), generation_(host.port_generation(port)) {
  read_name(descriptor);
}

UsbMidiDevice::~UsbMidiDevice() {
  if (present() && out_endpoint_) midi::silence(*this);   // nothing may go on sounding on a device that stays plugged in
  release_view(allocator_, batch_);
}

bool UsbMidiDevice::initialize() {
  if (!host_.set_configuration(port_, model_.configuration)) return false;
  if (model_.alternate != 0 && !host_.control_out(port_, kStandardInterfaceOut, kSetInterface, model_.alternate, model_.interface_number)) return false;
  if (model_.out_endpoint) {
    batch_ = allocate_dma_view<uint8_t>(allocator_, kBatchBytes, 64);
    if (batch_.empty()) return false;
    uint16_t max_packet = model_.out_max_packet ? model_.out_max_packet : 64;
    if (host_.start_bulk_endpoint(port_, model_.out_endpoint, max_packet)) out_endpoint_ = model_.out_endpoint;
  }
  if (model_.in_endpoint && model_.in_max_packet) input_running_ = host_.start_bulk_in_stream(port_, model_.in_endpoint, model_.in_max_packet);
  return out_endpoint_ != 0 || input_running_;
}

void UsbMidiDevice::send(uint8_t status, uint8_t data1, uint8_t data2) {
  if (!out_endpoint_) return;
  uint8_t packet[4];
  if (!encode_usb_midi_packet(0, status, data1, data2, packet)) return;
  if (batch_length_ + 4 > kBatchBytes) flush();
  for (int i = 0; i < 4; ++i) batch_[batch_length_ + i] = packet[i];
  batch_length_ += 4;
  if (batch_length_ + 4 > kBatchBytes) flush();
}

void UsbMidiDevice::flush() {
  if (!out_endpoint_ || batch_length_ == 0) return;
  const uint32_t length = batch_length_;
  batch_length_ = 0;
  if (!present()) return;
  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, out_endpoint_, batch_.first(length), &moved)) {   // a stall: bring the endpoint back, the batch is lost
    host_.reset_bulk_endpoint(port_, out_endpoint_);
    host_.control_out(port_, 0x02, 1, 0, out_endpoint_);   // CLEAR_FEATURE(ENDPOINT_HALT)
  }
}

void UsbMidiDevice::reset() {
  if (!out_endpoint_) return;
  midi::silence(*this);
  for (uint8_t channel = 0; channel < 16; ++channel) {
    send(static_cast<uint8_t>(0xB0 | channel), midi::kControllerResetAll, 0);
    send(static_cast<uint8_t>(0xE0 | channel), 0, 0x40);   // pitch bend centre
  }
  flush();
}

uint32_t UsbMidiDevice::poll_input(UsbMidiReceiver receiver, void* context) {
  if (!input_running_) return 0;
  uint32_t total = 0;
  for (int round = 0; round < 8; ++round) {   // (a device that talks without a pause does not keep the caller from other work)
    uint8_t buffer[512];
    uint16_t length = 0;
    if (!host_.poll_interrupt_in(port_, model_.in_endpoint, ByteView(buffer), &length) || length == 0) break;
    total += decode_usb_midi_packets(ConstByteView(buffer, length), receiver, context);
  }
  return total;
}

void UsbMidiDevice::read_name(const UsbDeviceDescriptor& d) {
  const char prefix[] = "USB MIDI: ";
  uint32_t n = 0;
  for (; prefix[n]; ++n) name_[n] = prefix[n];
  uint8_t buffer[64];
  if (d.product_string && host_.control_in(port_, 0x80, 6, static_cast<uint16_t>((3u << 8) | d.product_string), 0x0409, ByteView(buffer)) && buffer[0] >= 4 && buffer[1] == 3) {
    const uint32_t length = buffer[0] < sizeof buffer ? buffer[0] : sizeof buffer;
    uint32_t m = n;
    for (uint32_t i = 2; i + 1 < length && m + 1 < sizeof name_; i += 2) {
      const uint8_t ch = buffer[i];
      if (buffer[i + 1] == 0 && ch >= 32 && ch < 127) name_[m++] = static_cast<char>(ch);
    }
    if (m > n) {
      name_[m] = 0;
      return;
    }
  }
  static const char digits[] = "0123456789abcdef";
  const uint16_t ids[2] = {d.vendor_id, d.product_id};
  n = 0;
  const char fallback[] = "USB MIDI device ";
  for (; fallback[n]; ++n) name_[n] = fallback[n];
  for (int k = 0; k < 2; ++k) {
    for (int shift = 12; shift >= 0; shift -= 4) name_[n++] = digits[(ids[k] >> shift) & 15];
    if (k == 0) name_[n++] = ':';
  }
  name_[n] = 0;
}

}  // namespace hardware
