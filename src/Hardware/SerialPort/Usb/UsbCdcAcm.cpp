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

#include "Hardware/SerialPort/Usb/UsbCdcAcm.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassCommunication = 0x02, kSubclassAcm = 0x02, kClassData = 0x0A;
constexpr uint8_t kCsInterface = 0x24, kSubtypeAcm = 0x02, kSubtypeUnion = 0x06;
constexpr uint8_t kTransferBulk = 2, kTransferInterrupt = 3;
constexpr uint8_t kClassInterfaceOut = 0x21;
constexpr uint8_t kSetLineCoding = 0x20, kSetControlLineState = 0x22, kSendBreak = 0x23;
constexpr uint8_t kSerialState = 0x20;

void copy_name(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}
}  // namespace

bool find_usb_acm_function(ConstByteView configuration, UsbAcmFunction* out) {
  *out = UsbAcmFunction{};
  UsbDescriptorWalker walker(configuration);
  ConstByteView d;
  UsbConfigurationInfo config;
  if (!walker.next(&d) || !parse_usb_configuration(d, &config)) return false;
  UsbAcmFunction f;
  f.configuration = config.value;
  enum class In : uint8_t { Other, Control, Data } in = In::Other;
  bool have_control = false, have_data = false, have_union = false;
  int8_t union_data = -1;
  while (walker.next(&d)) {
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
    const uint8_t type = usb_descriptor_type(d);
    if (type == kUsbDescriptorInterface && parse_usb_interface(d, &interface)) {
      in = In::Other;
      if (interface.alternate_setting != 0) continue;
      if (!have_control && interface.device_class == kClassCommunication && interface.subclass == kSubclassAcm) {
        in = In::Control;
        have_control = true;
        f.control_interface = interface.number;
      } else if (have_control && !have_data && interface.device_class == kClassData &&
                 (union_data < 0 || union_data == static_cast<int8_t>(interface.number))) {
        in = In::Data;
        f.data_interface = interface.number;
      }
    } else if (in == In::Control && type == kCsInterface && d.size() >= 3) {
      if (d[2] == kSubtypeAcm && d.size() >= 4) f.capabilities = d[3];
      if (d[2] == kSubtypeUnion && d.size() >= 5 && !have_union) {
        have_union = true;
        union_data = static_cast<int8_t>(d[4]);   // (the first subordinate interface: the data one)
      }
    } else if (type == kUsbDescriptorEndpoint && parse_usb_endpoint(d, &endpoint)) {
      if (in == In::Control && endpoint.is_in() && endpoint.transfer_type() == kTransferInterrupt && !f.notify_endpoint) {
        f.notify_endpoint = endpoint.address;
        f.notify_max_packet = endpoint.max_packet;
        f.notify_interval = endpoint.interval;
      } else if (in == In::Data && endpoint.transfer_type() == kTransferBulk) {
        if (endpoint.is_in() && !f.in_endpoint) { f.in_endpoint = endpoint.address; f.in_max_packet = endpoint.max_packet; }
        if (!endpoint.is_in() && !f.out_endpoint) { f.out_endpoint = endpoint.address; f.out_max_packet = endpoint.max_packet; }
        if (f.in_endpoint && f.out_endpoint) have_data = true;
      }
    }
  }
  if (!have_control || !have_data || !f.in_max_packet || !f.out_max_packet) return false;
  *out = f;
  return true;
}

bool encode_line_coding(const SerialConfig& config, ByteView out) {
  if (!config.valid() || out.size() < 7) return false;
  ByteWriter w(out);
  w.le32(config.baud);
  w.u8(config.stop_bits == SerialStopBits::Two ? 2 : 0);
  w.u8(static_cast<uint8_t>(config.parity));          // (None 0, Odd 1, Even 2, Mark 3, Space 4: the same numbers)
  w.u8(config.data_bits);
  return w.ok();
}

bool decode_serial_state(ConstByteView n, uint8_t* lines, SerialStatistics* statistics) {
  if (n.size() < 10 || n[0] != 0xA1 || n[1] != kSerialState) return false;
  const uint16_t length = static_cast<uint16_t>(n[6] | (n[7] << 8));
  if (length < 2) return false;
  const uint8_t bits = n[8];
  uint8_t l = SerialLines::kCts;                       // (ACM has no CTS: the USB flow control stands in for it)
  if (bits & 0x01) l |= SerialLines::kCarrier;
  if (bits & 0x02) l |= SerialLines::kDsr;
  if (bits & 0x08) l |= SerialLines::kRing;
  *lines = l;
  if (bits & 0x04) ++statistics->breaks;
  if (bits & 0x10) ++statistics->framing_errors;
  if (bits & 0x20) ++statistics->parity_errors;
  if (bits & 0x40) ++statistics->overruns;
  return true;
}

UniquePointer<UsbCdcAcm> UsbCdcAcm::open_device(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator) {
  UsbDeviceDescriptor device;
  if (!host.device_descriptor(port, &device)) return UniquePointer<UsbCdcAcm>();
  uint8_t config[512];
  uint16_t length = 0;
  if (!host.configuration_descriptor(port, ByteView(config), &length)) return UniquePointer<UsbCdcAcm>();
  UsbAcmFunction f;
  if (!find_usb_acm_function(ConstByteView(config).at_most(length), &f)) return UniquePointer<UsbCdcAcm>();
  const uint16_t vendor = device.vendor_id, product = device.product_id;   // (copies: the descriptor is packed)
  UniquePointer<UsbCdcAcm> acm = UniquePointer<UsbCdcAcm>::make(allocator, host, port, f, allocator, vendor, product);
  if (!acm || !acm->initialize()) return UniquePointer<UsbCdcAcm>();
  return acm;
}

UsbCdcAcm::UsbCdcAcm(IUsbHostController& host, uint32_t port, const UsbAcmFunction& function, allocators::IAllocator& allocator, uint16_t vendor, uint16_t product)
    : host_(host), port_(port), function_(function), allocator_(allocator), vendor_(vendor), product_(product), generation_(host.port_generation(port)) {
  copy_name(name_, sizeof name_, "USB serial port");
  lines_ = SerialLines::kCts;
}

UsbCdcAcm::~UsbCdcAcm() {
  rx_.release(allocator_);
  tx_.release(allocator_);
  release_view(allocator_, staging_);
}

void UsbCdcAcm::set_name(const char* name) {
  kernel::ScopedMutex guard(mutex_);
  copy_name(name_, sizeof name_, name);
}

bool UsbCdcAcm::initialize() {
  if (ready_) return true;
  if (!rx_.allocate(allocator_, kQueue) || !tx_.allocate(allocator_, kQueue)) return false;
  staging_ = allocate_dma_view<uint8_t>(allocator_, kStaging, 64);
  if (staging_.empty()) return false;
  if (!host_.set_configuration(port_, function_.configuration)) return false;
  if (!host_.start_bulk_endpoint(port_, function_.out_endpoint, function_.out_max_packet)) return false;
  if (!host_.start_bulk_in_stream(port_, function_.in_endpoint, function_.in_max_packet)) return false;   // (receiving without waiting)
  if (function_.notify_endpoint)
    (void)host_.start_interrupt_in(port_, function_.notify_endpoint, function_.notify_max_packet, function_.notify_interval);   // (no lines without it: fine)
  ready_ = true;
  return true;
}

void UsbCdcAcm::shutdown() {
  close();
}

bool UsbCdcAcm::control(uint8_t request, uint16_t value) {
  return host_.control_out(port_, kClassInterfaceOut, request, value, function_.control_interface);
}

bool UsbCdcAcm::send_line_coding(const SerialConfig& config) {
  uint8_t coding[7];
  if (!encode_line_coding(config, ByteView(coding))) return false;
  // (A device without line coding support - bit 1 of its capabilities clear - ignores it or stalls; neither stops the port working.)
  const bool ok = host_.control_out_data(port_, kClassInterfaceOut, kSetLineCoding, 0, function_.control_interface, ConstByteView(coding));
  return ok || !(function_.capabilities & 0x02);
}

bool UsbCdcAcm::open(const SerialConfig& config) {
  if (!ready_ || !present() || !config.valid()) return false;
  if (!send_line_coding(config)) return false;
  (void)control(kSetControlLineState, 0x0003);          // DTR and RTS on: "a terminal is there" (many boards start talking only then)
  kernel::ScopedMutex guard(mutex_);
  config_ = config;
  rx_.clear();
  tx_.clear();
  in_flight_ = 0;
  lines_ = static_cast<uint8_t>((lines_ & ~(SerialLines::kRts | SerialLines::kDtr)) | SerialLines::kRts | SerialLines::kDtr);
  state_ = SerialState::Open;
  return true;
}

void UsbCdcAcm::close() {
  {
    kernel::ScopedMutex guard(mutex_);
    if (state_ == SerialState::Closed) return;
    state_ = SerialState::Closed;
    lines_ = static_cast<uint8_t>(lines_ & ~(SerialLines::kRts | SerialLines::kDtr));
  }
  if (present()) (void)control(kSetControlLineState, 0x0000);
}

SerialState UsbCdcAcm::state() const {
  kernel::ScopedMutex guard(mutex_);
  return present() ? state_ : SerialState::Error;
}

uint32_t UsbCdcAcm::write(ConstByteView data) {
  kernel::ScopedMutex guard(mutex_);
  if (state_ != SerialState::Open || !present()) return 0;
  return tx_.push(data);
}

uint32_t UsbCdcAcm::read(ByteView out) {
  kernel::ScopedMutex guard(mutex_);
  return rx_.pop(out);
}

uint32_t UsbCdcAcm::bytes_readable() const {
  kernel::ScopedMutex guard(mutex_);
  return rx_.size();
}

uint32_t UsbCdcAcm::bytes_writable() const {
  kernel::ScopedMutex guard(mutex_);
  return state_ == SerialState::Open ? tx_.space() : 0;
}

bool UsbCdcAcm::transmit_pending() const {
  kernel::ScopedMutex guard(mutex_);
  return tx_.size() > 0 || in_flight_ > 0;
}

void UsbCdcAcm::discard_input() {
  kernel::ScopedMutex guard(mutex_);
  rx_.clear();
}

uint8_t UsbCdcAcm::lines() const {
  kernel::ScopedMutex guard(mutex_);
  return lines_;
}

void UsbCdcAcm::set_lines(uint8_t lines) {
  uint16_t value = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    lines_ = static_cast<uint8_t>((lines_ & ~(SerialLines::kRts | SerialLines::kDtr)) | (lines & (SerialLines::kRts | SerialLines::kDtr)));
    value = static_cast<uint16_t>(((lines_ & SerialLines::kDtr) ? 1 : 0) | ((lines_ & SerialLines::kRts) ? 2 : 0));
  }
  (void)control(kSetControlLineState, value);
}

void UsbCdcAcm::set_break(bool on) { (void)control(kSendBreak, on ? 0xFFFF : 0x0000); }

SerialStatistics UsbCdcAcm::statistics() const {
  kernel::ScopedMutex guard(mutex_);
  return statistics_;
}

void UsbCdcAcm::poll() {
  if (!ready_ || !present()) return;
  // In: everything the endpoint has, into the receive queue (what does not fit is an overrun). The modem lines from the notifications.
  uint8_t packet[512];
  uint16_t length = 0;
  for (uint32_t budget = 0; budget < 64 && host_.poll_interrupt_in(port_, function_.in_endpoint, ByteView(packet), &length); ++budget) {
    const ConstByteView got = ConstByteView(packet).at_most(length);
    kernel::ScopedMutex guard(mutex_);
    if (state_ != SerialState::Open) continue;            // (a closed port drops what arrives)
    const uint32_t kept = rx_.push(got);
    statistics_.bytes_received += kept;
    statistics_.overruns += got.size() - kept;
  }
  if (function_.notify_endpoint) {
    for (uint32_t budget = 0; budget < 8 && host_.poll_interrupt_in(port_, function_.notify_endpoint, ByteView(packet), &length); ++budget) {
      kernel::ScopedMutex guard(mutex_);
      uint8_t inputs = 0;
      if (decode_serial_state(ConstByteView(packet).at_most(length), &inputs, &statistics_))
        lines_ = static_cast<uint8_t>((lines_ & (SerialLines::kRts | SerialLines::kDtr)) | inputs);
    }
  }
  // Out: one staging buffer's worth per poll, sent outside the lock.
  uint32_t taken = 0;
  {
    kernel::ScopedMutex guard(mutex_);
    if (state_ != SerialState::Open || tx_.size() == 0) return;
    taken = tx_.pop(staging_);
    in_flight_ = taken;
  }
  uint32_t sent = 0;
  const bool ok = host_.bulk_transfer(port_, function_.out_endpoint, staging_.first(taken), &sent);
  kernel::ScopedMutex guard(mutex_);
  in_flight_ = 0;
  if (!ok) {
    state_ = present() ? state_ : SerialState::Error;
    return;
  }
  statistics_.bytes_sent += sent;
}

}  // namespace hardware
