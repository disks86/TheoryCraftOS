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

// A UAS disk behind a host controller with bulk streams and asynchronous transfers, at the level of IUsbHostController: the configuration
// (alternate setting 0 Bulk-Only, 1 UAS with its four pipes, companion and Pipe Usage descriptors), SET_INTERFACE, and a SCSI disk that
// takes Command IUs and answers each tag on its streams when its status (and data) transfers are queued - in a random order and not all
// at once, so commands overlap and finish out of order. Failures as a real device has them: CHECK CONDITION before the data stage
// (that data transfer stays queued until the host takes it back), UNIT ATTENTION, a medium error at one LBA, unplugging. Without
// streams (streams_offered 0) it speaks the USB 2 way: Read Ready / Write Ready, then the data, then the Sense IU.
#pragma once
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <vector>
#include "Hardware/Bus/Usb/IUsbHostController.hpp"

using namespace hardware;

struct FakeUasHost final : IUsbHostController {
  static constexpr uint8_t kCommandEp = 0x01, kStatusEp = 0x82, kDataInEp = 0x83, kDataOutEp = 0x04;
  mutable std::mutex m;
  uint32_t streams_offered = 16;
  uint32_t block_size = 512;
  uint64_t blocks = 8192;
  std::vector<uint8_t> disk;
  uint32_t generation = 1;
  uint8_t alternate = 0;
  uint32_t set_interfaces = 0;
  bool fail_inquiry = false;
  uint64_t medium_error_lba = ~0ull;
  int unit_attentions = 0;
  uint64_t cancels = 0, commands_seen = 0;
  uint32_t in_flight = 0, peak = 0;
  std::mt19937 rng{1234};

  struct Transfer {
    bool used = false, done = false, ok = false;
    uint8_t ep = 0;
    uint16_t stream = 0;
    ByteView data;
    uint32_t moved = 0;
  };
  std::vector<Transfer> transfers;
  // What the disk does with a command (decided once, when it arrives): the data it moves (in: filled; out: taken) and the status.
  struct Outcome {
    bool data_in = false;
    uint64_t bytes = 0;
    uint8_t status = 0, sense_key = 0;
  };
  struct Command {
    uint8_t cdb[16] = {};
    Outcome outcome;
    bool data_done = false;   // (USB 2: the data stage has been moved)
  };
  std::map<uint16_t, Command> commands;   // by tag: received, not answered
  uint16_t serial_tag = 0;                // USB 2: the command being exchanged

  FakeUasHost() { disk.assign(blocks * block_size, 0); }
  static uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
  Outcome plan(const uint8_t* cdb) {
    Outcome o;
    const uint8_t op = cdb[0];
    auto fail = [&](uint8_t key) { o.status = 2; o.sense_key = key; o.bytes = 0; return o; };
    if (op == 0x12) {   // INQUIRY
      if (fail_inquiry) return fail(5);
      o.data_in = true;
      o.bytes = cdb[1] & 1 ? 0 : cdb[4];
      if (cdb[1] & 1) return fail(5);   // no VPD pages
      return o;
    }
    if (unit_attentions > 0) {
      --unit_attentions;
      return fail(6);
    }
    switch (op) {
      case 0x00: case 0x35: return o;   // TEST UNIT READY, SYNCHRONIZE CACHE
      case 0x25: o.data_in = true; o.bytes = 8; return o;
      case 0x28: case 0x2A: case 0x88: case 0x8A: {
        const bool sixteen = op == 0x88 || op == 0x8A;
        const uint64_t lba = sixteen ? (uint64_t{be32(cdb + 2)} << 32) | be32(cdb + 6) : be32(cdb + 2);
        const uint64_t count = sixteen ? be32(cdb + 10) : (uint32_t{cdb[7]} << 8) | cdb[8];
        if (lba + count > blocks) return fail(5);
        if ((op == 0x28 || op == 0x88) && medium_error_lba >= lba && medium_error_lba < lba + count) return fail(3);
        o.data_in = op == 0x28 || op == 0x88;
        o.bytes = count * block_size;
        return o;
      }
      default: return fail(5);   // (READ CAPACITY(16), WRITE SAME, ... : ILLEGAL REQUEST)
    }
  }
  void move_data(const uint8_t* cdb, ByteView data) {
    const uint8_t op = cdb[0];
    if (op == 0x12) {
      uint8_t inquiry[36] = {0, 0, 5, 2, 31};
      memcpy(inquiry + 8, "FAKE    UAS DISK        ", 24);
      for (uint64_t i = 0; i < data.size() && i < 36; ++i) data[i] = inquiry[i];
      return;
    }
    if (op == 0x25) {
      const uint32_t last = static_cast<uint32_t>(blocks - 1);
      const uint8_t c[8] = {uint8_t(last >> 24), uint8_t(last >> 16), uint8_t(last >> 8), uint8_t(last), uint8_t(block_size >> 24), uint8_t(block_size >> 16),
                            uint8_t(block_size >> 8), uint8_t(block_size)};
      for (uint64_t i = 0; i < data.size() && i < 8; ++i) data[i] = c[i];
      return;
    }
    const bool sixteen = op == 0x88 || op == 0x8A;
    const uint64_t lba = sixteen ? (uint64_t{be32(cdb + 2)} << 32) | be32(cdb + 6) : be32(cdb + 2);
    if (op == 0x28 || op == 0x88) memcpy(data.data(), disk.data() + lba * block_size, data.size());
    else memcpy(disk.data() + lba * block_size, data.data(), data.size());
  }
  static void sense_iu(ByteView out, uint16_t tag, uint8_t status, uint8_t key, uint32_t* moved) {
    memset(out.data(), 0, out.size());
    out[0] = 0x03;
    out[2] = uint8_t(tag >> 8);
    out[3] = uint8_t(tag);
    out[6] = status;
    const uint32_t length = status ? 18 : 0;
    out[15] = uint8_t(length);
    if (length) {
      out[16] = 0x70;
      out[18] = key;
      out[23] = 10;
    }
    *moved = 16 + length;
  }

  // --- streams: answer some of the commands whose transfers are all queued.
  int find(uint8_t ep, uint16_t stream) {
    for (size_t i = 0; i < transfers.size(); ++i)
      if (transfers[i].used && !transfers[i].done && transfers[i].ep == ep && transfers[i].stream == stream) return static_cast<int>(i);
    return -1;
  }
  void process() {
    std::vector<uint16_t> tags;
    for (auto& [tag, c] : commands) tags.push_back(tag);
    std::shuffle(tags.begin(), tags.end(), rng);
    for (uint16_t tag : tags) {
      if (rng() % 3 == 0) continue;   // (not every command at once: they overlap)
      Command& c = commands[tag];
      const int status = find(kStatusEp, tag);
      if (status < 0) continue;
      const Outcome o = c.outcome;
      if (o.status == 0 && o.bytes) {
        const int data = find(o.data_in ? kDataInEp : kDataOutEp, tag);
        if (data < 0) continue;
        Transfer& d = transfers[data];
        if (d.data.size() < o.bytes) continue;
        move_data(c.cdb, d.data.first(o.bytes));
        d.done = d.ok = true;
        d.moved = static_cast<uint32_t>(o.bytes);
      }
      Transfer& s = transfers[status];
      sense_iu(s.data, tag, o.status, o.sense_key, &s.moved);
      s.done = s.ok = true;
      commands.erase(tag);
      --in_flight;
    }
  }

  // --- IUsbHostController
  const char* name() const override { return "fake UAS host"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  UsbControllerKind kind() const override { return UsbControllerKind::Xhci; }
  uint32_t port_count() const override { return 1; }
  bool port_info(uint32_t, UsbPortInfo* out) const override { out->connected = true; out->speed = UsbSpeed::Super; return true; }
  bool device_descriptor(uint32_t, UsbDeviceDescriptor*) const override { return false; }
  bool configuration_descriptor(uint32_t port, ByteView buffer, uint16_t* length) const override {
    if (port != 0) return false;
    std::vector<uint8_t> c = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
    auto endpoint = [&](uint8_t address, uint8_t stream_exponent, int pipe) {
      const uint8_t e[] = {7, 5, address, 2, 0x00, 0x04, 0};
      c.insert(c.end(), e, e + 7);
      const uint8_t companion[] = {6, 0x30, 0, stream_exponent, 0, 0};
      c.insert(c.end(), companion, companion + 6);
      if (pipe) {
        const uint8_t usage[] = {4, 0x24, uint8_t(pipe), 0};
        c.insert(c.end(), usage, usage + 4);
      }
    };
    const uint8_t bot[] = {9, 4, 0, 0, 2, 8, 6, 0x50, 0};
    c.insert(c.end(), bot, bot + 9);
    endpoint(0x81, 0, 0);
    endpoint(0x02, 0, 0);
    const uint8_t uas[] = {9, 4, 0, 1, 4, 8, 6, 0x62, 0};
    c.insert(c.end(), uas, uas + 9);
    endpoint(kCommandEp, 0, 1);
    endpoint(kStatusEp, 5, 2);
    endpoint(kDataInEp, 5, 3);
    endpoint(kDataOutEp, 5, 4);
    c[2] = uint8_t(c.size());
    c[3] = uint8_t(c.size() >> 8);
    *length = static_cast<uint16_t>(c.size());
    for (size_t i = 0; i < c.size() && i < buffer.size(); ++i) buffer[i] = c[i];
    return true;
  }
  bool set_configuration(uint32_t, uint8_t) override { return true; }
  bool control_out(uint32_t, uint8_t request_type, uint8_t request, uint16_t value, uint16_t) override {
    std::lock_guard<std::mutex> g(m);
    if (request_type == 0x01 && request == 11) {
      alternate = static_cast<uint8_t>(value);
      ++set_interfaces;
    }
    return true;
  }
  bool control_in(uint32_t, uint8_t, uint8_t, uint16_t, uint16_t, ByteView) override { return false; }
  bool start_interrupt_in(uint32_t, uint8_t, uint16_t, uint8_t) override { return false; }
  bool poll_interrupt_in(uint32_t, uint8_t, ByteView, uint16_t*) override { return false; }
  bool start_bulk_endpoint(uint32_t, uint8_t, uint16_t) override { return true; }
  bool reset_bulk_endpoint(uint32_t, uint8_t) override { return true; }
  uint32_t port_generation(uint32_t) const override { std::lock_guard<std::mutex> g(m); return generation; }
  uint32_t max_bulk_streams(uint32_t) const override { return streams_offered; }
  bool start_bulk_endpoint_streams(uint32_t, uint8_t, uint16_t, uint32_t streams) override { return streams && streams <= streams_offered; }
  bool supports_async_bulk() const override { return true; }

  bool submit_bulk(uint32_t, uint8_t ep, uint16_t stream, ByteView data, uint32_t* token) override {
    std::lock_guard<std::mutex> g(m);
    if (alternate != 1) return false;
    size_t i = 0;
    while (i < transfers.size() && transfers[i].used) ++i;
    if (i == transfers.size()) transfers.emplace_back();
    Transfer& t = transfers[i];
    t = Transfer{};
    t.used = true;
    t.ep = ep;
    t.stream = stream;
    t.data = data;
    *token = static_cast<uint32_t>(i);
    if (ep == kCommandEp) {   // the Command IU: taken at once
      if (data.size() < 32 || data[0] != 0x01) { t.done = true; return true; }
      const uint16_t tag = static_cast<uint16_t>((data[2] << 8) | data[3]);
      if (commands.count(tag)) { t.done = true; return true; }   // (an overlapped tag: refused)
      Command c;
      memcpy(c.cdb, data.data() + 16, 16);
      c.outcome = plan(c.cdb);
      commands[tag] = c;
      ++commands_seen;
      if (++in_flight > peak) peak = in_flight;
      t.done = t.ok = true;
      t.moved = 32;
    }
    return true;
  }
  bool poll_bulk(uint32_t, uint32_t token, bool* ok, uint32_t* transferred) override {
    std::lock_guard<std::mutex> g(m);
    *ok = false;
    *transferred = 0;
    if (token >= transfers.size() || !transfers[token].used) return true;
    process();
    Transfer& t = transfers[token];
    if (generation != 1 && !t.done) t.done = true;   // unplugged
    if (!t.done) return false;
    *ok = t.ok && generation == 1;
    *transferred = t.moved;
    t = Transfer{};
    return true;
  }
  bool cancel_bulk(uint32_t, uint32_t token) override {
    std::lock_guard<std::mutex> g(m);
    if (token >= transfers.size() || !transfers[token].used) return false;
    transfers[token] = Transfer{};
    ++cancels;
    return true;
  }

  // USB 2 (no streams): blocking exchanges on the pipes.
  bool bulk_transfer(uint32_t, uint8_t ep, ByteView data, uint32_t* transferred) override {
    std::lock_guard<std::mutex> g(m);
    *transferred = 0;
    if (alternate != 1 || generation != 1) return false;
    if (ep == kCommandEp) {
      if (data.size() < 32 || data[0] != 0x01) return false;
      serial_tag = static_cast<uint16_t>((data[2] << 8) | data[3]);
      Command c;
      memcpy(c.cdb, data.data() + 16, 16);
      c.outcome = plan(c.cdb);
      commands[serial_tag] = c;
      ++commands_seen;
      *transferred = 32;
      return true;
    }
    auto it = commands.find(serial_tag);
    if (it == commands.end()) return false;
    Command& c = it->second;
    const Outcome o = c.outcome;
    if (ep == kStatusEp) {
      if (o.status == 0 && o.bytes && !c.data_done) {   // the data stage is next: say which way
        memset(data.data(), 0, 4);
        data[0] = o.data_in ? 0x06 : 0x07;
        data[2] = uint8_t(serial_tag >> 8);
        data[3] = uint8_t(serial_tag);
        *transferred = 4;
        return true;
      }
      sense_iu(data, serial_tag, o.status, o.sense_key, transferred);
      commands.erase(it);
      return true;
    }
    if ((ep == kDataInEp) != o.data_in || data.size() < o.bytes) return false;
    move_data(c.cdb, data.first(o.bytes));
    c.data_done = true;
    *transferred = static_cast<uint32_t>(o.bytes);
    return true;
  }
};
