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

// Feeds hostile data to the parsers of untrusted device data - USB descriptors, HID
// report descriptors and reports, SCSI data, device strings - with exactly sized heap
// buffers, so any read or write past the end is a bounds violation or an ASAN report.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "Hardware/Bus/Usb/Hid/HidReportParser.hpp"
#include "Hardware/Bus/Usb/Hid/UsbHidInterface.hpp"
#include "Hardware/Bus/Usb/UsbDescriptors.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"

using namespace hardware;

static uint32_t g_state = 99;
static uint32_t rnd() { g_state = g_state * 1664525u + 1013904223u; return g_state >> 8; }

// A believable configuration descriptor (HID keyboard + mass storage) to mutate.
static std::vector<uint8_t> sample_configuration() {
  return {9, 2, 0, 0, 2, 1, 0, 0x80, 50,
          9, 4, 0, 0, 1, 3, 1, 1, 0,  9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,  7, 5, 0x81, 3, 8, 0, 10,
          9, 4, 1, 0, 2, 8, 6, 0x50, 0,  7, 5, 0x81, 2, 0, 2, 0,  7, 5, 0x02, 2, 0, 2, 0};
}

static std::vector<uint8_t> sample_report_descriptor() {
  return {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
          0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
          0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0};
}

static void exercise_usb(const std::vector<uint8_t>& blob) {
  UsbDescriptorWalker walker{ConstByteView(blob.data(), blob.size())};
  ConstByteView d;
  while (walker.next(&d)) {
    UsbConfigurationInfo c; UsbInterfaceInfo i; UsbEndpointInfo e;
    (void)parse_usb_configuration(d, &c); (void)parse_usb_interface(d, &i); (void)parse_usb_endpoint(d, &e);
  }
  UsbHidInterface interfaces[3];
  (void)find_hid_interfaces(ConstByteView(blob.data(), blob.size()), ArrayView<UsbHidInterface>(interfaces));
}

static void exercise_hid(const std::vector<uint8_t>& descriptor, const std::vector<uint8_t>& report) {
  HidPointerLayout layout;
  if (parse_hid_pointer(ConstByteView(descriptor.data(), descriptor.size()), &layout)) {
    const ConstByteView r(report.data(), report.size());
    for (uint64_t b = 0; b < layout.buttons().size(); ++b) (void)hid_extract(r, layout.buttons()[b], layout.report_id);
    (void)hid_extract(r, layout.x, layout.report_id); (void)hid_extract(r, layout.y, layout.report_id);
    (void)hid_extract(r, layout.wheel, layout.report_id);
  }
  // fields with arbitrary offsets and sizes against arbitrary reports
  HidField f; f.valid = true; f.bit_offset = rnd() % 700; f.bit_size = (uint8_t)(rnd() % 40); f.is_signed = rnd() & 1;
  (void)hid_extract(ConstByteView(report.data(), report.size()), f, (uint8_t)(rnd() % 3));
}

int main() {
  const std::vector<uint8_t> config = sample_configuration(), hid = sample_report_descriptor();
  uint64_t cases = 0;

  exercise_usb(config);
  { UsbHidInterface out[2]; const uint32_t found = find_hid_interfaces(ConstByteView(config.data(), config.size()), ArrayView<UsbHidInterface>(out));
    if (found != 1 || out[0].endpoint_address != 0x81 || out[0].report_descriptor_length != 63) { puts("FAILED: sample configuration not understood"); return 1; } }
  HidPointerLayout sample;
  if (!parse_hid_pointer(ConstByteView(hid.data(), hid.size()), &sample) || !sample.x.relative) { puts("FAILED: sample report descriptor not understood"); return 1; }

  for (int round = 0; round < 40000; ++round) {
    // truncations at every length, byte flips, junk, and lengths that lie
    std::vector<uint8_t> c(config.begin(), config.begin() + (rnd() % (config.size() + 1)));
    for (int i = 0, n = rnd() % 5; i < n && !c.empty(); ++i) c[rnd() % c.size()] = (uint8_t)rnd();
    for (int i = 0, n = rnd() % 3; i < n; ++i) c.push_back((uint8_t)rnd());
    exercise_usb(c); ++cases;

    std::vector<uint8_t> h(hid.begin(), hid.begin() + (rnd() % (hid.size() + 1)));
    for (int i = 0, n = rnd() % 6; i < n && !h.empty(); ++i) h[rnd() % h.size()] = (uint8_t)rnd();
    std::vector<uint8_t> report(rnd() % 12); for (auto& b : report) b = (uint8_t)rnd();
    exercise_hid(h, report); ++cases;

    std::vector<uint8_t> noise(rnd() % 80); for (auto& b : noise) b = (uint8_t)rnd();
    exercise_usb(noise); exercise_hid(noise, report); ++cases;

    // SCSI and device strings
    std::vector<uint8_t> s(rnd() % 50); for (auto& b : s) b = (uint8_t)rnd();
    scsi::InquiryInfo inquiry; uint64_t last; uint32_t block;
    (void)scsi::parse_inquiry(ConstByteView(s.data(), s.size()), &inquiry);
    (void)scsi::parse_capacity10(ConstByteView(s.data(), s.size()), &last, &block);
    (void)scsi::parse_capacity16(ConstByteView(s.data(), s.size()), &last, &block);
    char text[1 + rnd() % 45];
    copy_device_string(ArrayView<char>(text, sizeof(text)), ConstByteView(s.data(), s.size()), rnd() & 1);
    uint8_t cdb[16]; (void)scsi::build_read_write(ByteView(cdb), rnd() & 1, ((uint64_t)rnd() << 32) | rnd(), rnd() % 70000);
    ++cases;
  }
  printf("parser fuzz: %llu hostile inputs survived\n", (unsigned long long)cases);
  return 0;
}
