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
  {  // UNMAP and the provisioning pages, exact values
    uint8_t cdb[10], parameters[24];
    if (!scsi::build_unmap(ByteView(cdb), ByteView(parameters), 0x123456789Aull, 77) || cdb[0] != 0x42 || cdb[8] != 24 || parameters[1] != 22 ||
        parameters[3] != 16 || parameters[11] != 0x12 || parameters[15] != 0x9A || parameters[19] != 77) { puts("FAILED: UNMAP layout"); return 1; }
    uint8_t lbp[8] = {0, 0xB2, 0, 4, 0, 0x80, 0, 0}, limits[64] = {0, 0xB0};
    limits[23] = 0x40;
    limits[27] = 1;
    uint32_t blocks = 0, descriptors = 0;
    if (!scsi::parse_lbp_vpd_unmap(ConstByteView(lbp)) || !scsi::parse_block_limits(ConstByteView(limits), &blocks, &descriptors) || blocks != 0x40 ||
        descriptors != 1 || scsi::parse_lbp_vpd_unmap(ConstByteView(lbp).first(5))) { puts("FAILED: provisioning pages"); return 1; }
  }
  {  // probe_unmap / unmap_range against a scripted unit: thin provisioned, LBPRZ, UNMAP of at most 1000 blocks
    std::vector<std::vector<uint8_t>> unmaps;
    bool thin = true, lbpu = true, has_limits = true;
    auto unit = [&](ConstByteView cdb, ByteView data, bool in) {
      if (cdb[0] == scsi::kServiceActionIn16 && in) {
        data.fill(0);
        if (data.size() > 14) data[14] = thin ? 0xC0 : 0;
        return true;
      }
      if (cdb[0] == scsi::kInquiry && (cdb[1] & 1) && in) {
        data.fill(0);
        if (cdb[2] == scsi::kVpdLogicalBlockProvisioning) { data[1] = 0xB2; data[5] = lbpu ? 0x80 : 0; return true; }
        if (cdb[2] == scsi::kVpdBlockLimits && has_limits) { data[1] = 0xB0; data[22] = 0x03; data[23] = 0xE8; data[27] = 1; return true; }
        return false;   // (no such page)
      }
      if (cdb[0] == scsi::kUnmap && !in) { unmaps.emplace_back(data.data(), data.data() + data.size()); return true; }
      return false;
    };
    uint8_t scratch[64];
    scsi::UnmapSupport u = scsi::probe_unmap(unit, ByteView(scratch));
    if (!u.unmap || !u.reads_zeros || u.max_blocks != 1000) { puts("FAILED: probe_unmap"); return 1; }
    if (!scsi::unmap_range(unit, u, ByteView(scratch), 5000, 2500) || unmaps.size() != 3 || unmaps[2][19] != (500 & 0xFF) || unmaps[1][15] != ((6000) & 0xFF)) {
      puts("FAILED: unmap_range");
      return 1;
    }
    has_limits = false;
    u = scsi::probe_unmap(unit, ByteView(scratch));
    if (!u.unmap || u.max_blocks != 0x400000) { puts("FAILED: probe_unmap without limits"); return 1; }
    lbpu = false;
    if (scsi::probe_unmap(unit, ByteView(scratch)).unmap) { puts("FAILED: probe_unmap without LBPU"); return 1; }
    lbpu = true;
    thin = false;
    if (scsi::probe_unmap(unit, ByteView(scratch)).unmap || scsi::unmap_range(unit, scsi::UnmapSupport{}, ByteView(scratch), 0, 1)) { puts("FAILED: not thin"); return 1; }
    if (scsi::probe_unmap(unit, ByteView(scratch).first(10)).unmap) { puts("FAILED: small scratch"); return 1; }
  }
  {  // WRITE SAME as write zeroes: the limit from the Block Limits page, commands split at it, the block sent is zeros
    std::vector<std::vector<uint8_t>> commands;
    uint64_t limit = 3000;
    bool page = true, refuse = false;
    auto unit = [&](ConstByteView cdb, ByteView data, bool in) {
      if (cdb[0] == scsi::kInquiry && (cdb[1] & 1) && in && cdb[2] == scsi::kVpdBlockLimits && page) {
        data.fill(0);
        data[1] = 0xB0;
        for (int i = 0; i < 8; ++i) data[36 + i] = static_cast<uint8_t>(limit >> (56 - 8 * i));
        return true;
      }
      if (cdb[0] == scsi::kWriteSame16 && !in && !refuse) {
        for (uint8_t b : data) if (b != 0) return false;
        commands.emplace_back(cdb.data(), cdb.data() + 16);
        return true;
      }
      return false;
    };
    uint8_t scratch[64], block[512];
    for (uint8_t& b : block) b = 0xEE;
    const uint64_t max = scsi::probe_write_same(unit, ByteView(scratch));
    auto lba = [&](size_t i) { uint64_t v = 0; for (int k = 2; k < 10; ++k) v = (v << 8) | commands[i][k]; return v; };
    auto count = [&](size_t i) { uint32_t v = 0; for (int k = 10; k < 14; ++k) v = (v << 8) | commands[i][k]; return v; };
    if (max != 3000 || !scsi::write_same_zeroes(unit, ByteView(block), max, 0x100000000ull, 7000) || commands.size() != 3 || commands[0][0] != 0x93 ||
        commands[0][1] != 0 || lba(0) != 0x100000000ull || count(0) != 3000 || lba(1) != 0x100000000ull + 3000 || count(1) != 3000 ||
        lba(2) != 0x100000000ull + 6000 || count(2) != 1000) { puts("FAILED: write same"); return 1; }
    limit = 0;
    if (scsi::probe_write_same(unit, ByteView(scratch)) != 0xFFFF) { puts("FAILED: write same without a limit"); return 1; }
    page = false;
    if (scsi::probe_write_same(unit, ByteView(scratch)) != 0) { puts("FAILED: write same without the page"); return 1; }
    refuse = true;
    if (scsi::write_same_zeroes(unit, ByteView(block), 100, 0, 10)) { puts("FAILED: write same refused"); return 1; }
  }

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
    uint32_t unmap_blocks, unmap_descriptors;
    (void)scsi::capacity16_thin_provisioned(ConstByteView(s.data(), s.size()));
    (void)scsi::parse_lbp_vpd_unmap(ConstByteView(s.data(), s.size()));
    (void)scsi::parse_block_limits(ConstByteView(s.data(), s.size()), &unmap_blocks, &unmap_descriptors);
    char text[1 + rnd() % 45];
    copy_device_string(ArrayView<char>(text, sizeof(text)), ConstByteView(s.data(), s.size()), rnd() & 1);
    uint8_t cdb[16]; (void)scsi::build_read_write(ByteView(cdb), rnd() & 1, ((uint64_t)rnd() << 32) | rnd(), rnd() % 70000);
    uint8_t parameters[24]; (void)scsi::build_unmap(ByteView(cdb), ByteView(parameters), ((uint64_t)rnd() << 32) | rnd(), rnd());
    ++cases;
  }
  printf("parser fuzz: %llu hostile inputs survived\n", (unsigned long long)cases);
  return 0;
}
