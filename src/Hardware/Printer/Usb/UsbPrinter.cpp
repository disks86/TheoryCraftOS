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

#include "Hardware/Printer/Usb/UsbPrinter.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Hardware/Printer/Brother/BrotherHl2070n.hpp"

namespace hardware {

namespace {
constexpr uint8_t kClassPrinter = 7, kSubclassPrinter = 1, kProtocolUnidirectional = 1, kProtocolBidirectional = 2;
constexpr uint8_t kClassInterfaceIn = 0xA1, kClassInterfaceOut = 0x21;
constexpr uint8_t kGetDeviceId = 0, kGetPortStatus = 1, kSoftReset = 2;
constexpr uint8_t kStandardInterfaceOut = 0x01, kSetInterface = 0x0B;
constexpr uint8_t kEndpointOut = 0x02, kClearFeature = 1, kFeatureEndpointHalt = 0;
constexpr uint32_t kMaxFailures = 3;
}  // namespace

UsbPrinter::~UsbPrinter() { shutdown(); }

void UsbPrinter::discover(IUsbBus& usb, allocators::IAllocator& allocator, List<UniquePointer<IPrinter>>& out) {
  for (uint32_t c = 0; c < usb.controller_count(); ++c) {
    IUsbHostController* host = usb.host_controller(c);
    if (!host) continue;
    for (uint32_t port = 0; port < host->port_count(); ++port) {
      uint8_t blob[512];
      uint16_t length = 0;
      if (!host->configuration_descriptor(port, ByteView(blob), &length)) continue;
      const ConstByteView config = ConstByteView(blob).at_most(length);
      UsbDescriptorWalker walker(config);
      ConstByteView descriptor;
      UsbConfigurationInfo configuration;
      if (!walker.next(&descriptor) || !parse_usb_configuration(descriptor, &configuration)) continue;

      // Every printer-class interface setting with its bulk endpoints.
      struct Candidate {
        UsbPrinterEndpoints ep;
        uint8_t protocol;
      };
      Candidate found[8];
      uint32_t n = 0;
      bool in_printer = false;
      while (walker.next(&descriptor)) {
        UsbInterfaceInfo interface;
        UsbEndpointInfo endpoint;
        if (parse_usb_interface(descriptor, &interface)) {
          in_printer = interface.device_class == kClassPrinter && interface.subclass == kSubclassPrinter &&
                       (interface.protocol == kProtocolUnidirectional || interface.protocol == kProtocolBidirectional) && n < 8;
          if (in_printer) {
            found[n].ep = UsbPrinterEndpoints();
            found[n].ep.configuration = configuration.value;
            found[n].ep.interface_number = interface.number;
            found[n].ep.alternate = interface.alternate_setting;
            found[n].protocol = interface.protocol;
            ++n;
          }
        } else if (in_printer && parse_usb_endpoint(descriptor, &endpoint) && endpoint.transfer_type() == 2) {
          UsbPrinterEndpoints& e = found[n - 1].ep;
          if (endpoint.is_in()) {
            if (!e.in_endpoint) e.in_endpoint = endpoint.address, e.in_max_packet = endpoint.max_packet;
          } else if (!e.out_endpoint) {
            e.out_endpoint = endpoint.address, e.out_max_packet = endpoint.max_packet;
          }
        }
      }
      // One printer per interface number: the bidirectional setting if it has both endpoints, else any with an OUT endpoint.
      for (uint32_t i = 0; i < n; ++i) {
        bool first_of_number = true;
        for (uint32_t k = 0; k < i; ++k)
          if (found[k].ep.interface_number == found[i].ep.interface_number) first_of_number = false;
        if (!first_of_number) continue;
        const Candidate* best = nullptr;
        for (uint32_t k = i; k < n; ++k) {
          if (found[k].ep.interface_number != found[i].ep.interface_number || !found[k].ep.out_endpoint) continue;
          const bool bidi = found[k].protocol == kProtocolBidirectional && found[k].ep.in_endpoint;
          if (!best || (bidi && !(best->protocol == kProtocolBidirectional && best->ep.in_endpoint))) best = &found[k];
        }
        if (!best) continue;
        UniquePointer<UsbPrinter> printer = UniquePointer<UsbPrinter>::make(allocator, *host, port, best->ep, allocator);
        if (printer && printer->initialize()) out.push_back(UniquePointer<IPrinter>(util::move(printer)));
      }
    }
  }
}

bool UsbPrinter::initialize() {
  if (started_) return true;
  wire_ = allocate_view<uint8_t>(allocator_, kChunk, 65536);
  if (wire_.empty()) return false;
  if (!host_.set_configuration(port_, ep_.configuration) || !host_.start_bulk_endpoint(port_, ep_.out_endpoint, ep_.out_max_packet) ||
      (ep_.in_endpoint && !host_.start_bulk_endpoint(port_, ep_.in_endpoint, ep_.in_max_packet))) {
    release_view(allocator_, wire_);
    return false;
  }
  if (ep_.alternate != 0 && !host_.control_out(port_, kStandardInterfaceOut, kSetInterface, ep_.alternate, ep_.interface_number)) {
    release_view(allocator_, wire_);
    return false;
  }
  started_ = true;
  read_identity();  // a printer that will not say who it is still prints
  return true;
}

// Fills name_, identity_ and info_ from the device ID.
bool UsbPrinter::read_identity() {
  ByteView buffer = wire_.first(512);
  buffer.fill(0);
  uint16_t got = 0;
  PrinterInfo generic;
  generic.manufacturer = "";
  generic.model = name_;
  generic.languages = static_cast<uint8_t>(1u << static_cast<uint8_t>(PrinterLanguage::Pcl));
  generic.paper_sizes = static_cast<uint16_t>((1u << static_cast<uint8_t>(PaperSize::A4)) | (1u << static_cast<uint8_t>(PaperSize::Letter)) |
                                             (1u << static_cast<uint8_t>(PaperSize::Legal)) | (1u << static_cast<uint8_t>(PaperSize::A5)));
  info_ = generic;
  const uint16_t index = static_cast<uint16_t>((ep_.interface_number << 8) | ep_.alternate);
  if (!host_.control_in(port_, kClassInterfaceIn, kGetDeviceId, 0, index, buffer)) return false;
  got = static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);  // big-endian length, counting these two bytes
  if (got < 2) return false;
  if (got > buffer.size()) got = static_cast<uint16_t>(buffer.size());
  identity_ = parse_device_id(ConstByteView(buffer).subview(2, got - 2));
  // Names: "Manufacturer Model".
  uint32_t n = 0;
  for (const char* p = identity_.manufacturer; *p && n + 1 < sizeof name_; ++p) name_[n++] = *p;
  if (n && identity_.model[0] && n + 1 < sizeof name_) name_[n++] = ' ';
  for (const char* p = identity_.model; *p && n + 1 < sizeof name_; ++p) name_[n++] = *p;
  name_[n] = 0;
  if (n == 0) {
    const char fallback[] = "USB printer";
    for (uint32_t i = 0; i < sizeof fallback; ++i) name_[i] = fallback[i];
  }
  // A printer we know gets its real capabilities; any other gets what its command set promises.
  bool brother = false;
  for (const char* p = identity_.model; *p; ++p)
    if (p[0] == 'H' && p[1] == 'L' && p[2] == '-' && p[3] == '2' && p[4] == '0' && p[5] == '7' && p[6] == '0') brother = true;
  if (brother) {
    info_ = BrotherHl2070n::model_info();
  } else {
    if (identity_.languages) info_.languages = identity_.languages;
    info_.manufacturer = identity_.manufacturer;
  }
  info_.model = name_;
  return true;
}

void UsbPrinter::shutdown() {
  if (!started_) return;
  cancel_all();
  started_ = false;
  release_view(allocator_, wire_);
}

bool UsbPrinter::submit(ConstByteView job, const char*) {
  if (!started_ || count_ >= kQueueDepth || job.empty()) return false;
  UniquePointer<Job> entry = UniquePointer<Job>::make(allocator_, allocator_);
  if (!entry || !entry->data.append(job)) return false;
  queue_[(head_ + count_) % kQueueDepth] = util::move(entry);
  ++count_;
  return true;
}

void UsbPrinter::recover() {
  host_.reset_bulk_endpoint(port_, ep_.out_endpoint);
  host_.control_out(port_, kEndpointOut, kClearFeature, kFeatureEndpointHalt, ep_.out_endpoint);
}

void UsbPrinter::fail_front() {
  soft_reset();
  ++stats_.jobs_failed;
  last_failed_ = true;
  queue_[head_].reset();
  head_ = (head_ + 1) % kQueueDepth;
  --count_;
}

void UsbPrinter::poll() {
  if (!started_ || count_ == 0) return;
  Job& job = *queue_[head_];
  const uint64_t left = job.data.size() - job.sent;
  const uint32_t n = left < kChunk ? static_cast<uint32_t>(left) : kChunk;
  ConstByteView source = job.data.view().subview(job.sent, n);
  ByteView chunk = wire_.first(n);
  chunk.copy_from(source);  // the controller wants the bytes in its DMA-safe block
  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, ep_.out_endpoint, chunk, &moved)) {
    recover();
    if (++job.failures >= kMaxFailures) fail_front();
    return;
  }
  job.failures = 0;
  job.sent += moved;
  stats_.bytes_sent += moved;
  if (job.sent >= job.data.size()) {
    ++stats_.jobs_sent;
    last_failed_ = false;
    queue_[head_].reset();
    head_ = (head_ + 1) % kQueueDepth;
    --count_;
  }
}

void UsbPrinter::cancel_all() {
  for (UniquePointer<Job>& j : queue_) j.reset();
  head_ = count_ = 0;
}

bool UsbPrinter::read_status(UsbPrinterStatus* status) {
  if (!started_) return false;
  ByteView b = wire_.first(8);
  b.fill(0);
  if (!host_.control_in(port_, kClassInterfaceIn, kGetPortStatus, 0, ep_.interface_number, b.first(1))) return false;
  status->paper_empty = (b[0] & 0x20) != 0;
  status->selected = (b[0] & 0x10) != 0;
  status->no_error = (b[0] & 0x08) != 0;
  return true;
}

bool UsbPrinter::read_back(ByteView out, uint32_t* length) {
  if (!started_ || !ep_.in_endpoint) return false;
  const uint32_t want = out.size() < kChunk ? static_cast<uint32_t>(out.size()) : kChunk;
  uint32_t moved = 0;
  if (!host_.bulk_transfer(port_, ep_.in_endpoint, wire_.first(want), &moved)) return false;
  if (moved > want) moved = want;
  out.first(moved).copy_from(ConstByteView(wire_).first(moved));
  *length = moved;
  return true;
}

bool UsbPrinter::soft_reset() {
  if (!started_) return false;
  return host_.control_out(port_, kClassInterfaceOut, kSoftReset, 0, ep_.interface_number);
}

}  // namespace hardware
