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

// A DisplayPort sink as the AUX channel sees it, written from the AUX wire format (address, command and length bytes; native and I2C-over-AUX requests with the
// middle-of-transaction flag; replies as a status byte and payload), for the tests of AmdAux and of the card: a DPCD, a monitor's EDID with its segment pointer, a log of the
// requests, a check that they are well formed, and a script of replies that go wrong.
#pragma once
#include <stdint.h>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

struct FakeAuxSink {
  struct Request {
    std::vector<uint8_t> bytes;     // the request as the sink saw it: 3 or 4 bytes of header, the payload
    uint32_t pointer = 0, out_pointer = 0, channel = 0, delay = 0, hpd = 0;
    uint8_t length_byte = 0;        // the fourth byte of the header, which the table is given whatever the wire carries
  };
  struct Answer {
    uint8_t status = 0;             // the table's reply status: 1 timeout, 2 busy, 3 error, else the reply command in the high nibble
    std::vector<uint8_t> data;
    uint32_t length = 0;            // what the table reports as the data length
  };

  std::vector<Request> requests;
  // the sink
  std::vector<uint8_t> edid, dpcd;
  bool monitor_present = true;
  uint32_t segment = 0, offset = 0;
  bool in_transaction = false;
  uint32_t malformed = 0, outside_transaction = 0;
  // the script
  uint32_t native_defers = 0, i2c_defers = 0, busy = 0, timeouts = 0, errors = 0, native_nacks = 0;
  uint32_t max_read = 0;               // 0: all that was asked
  uint32_t claim_extra = 0;            // the length reported beyond the data
  bool random_replies = false;
  uint8_t native_reply_extra = 0;      // bits ORed into the status of every answer to a native request (the I2C reply bits mean nothing there)
  bool native_odd_reply = false;       // native requests are answered with a reply command that does not exist
  bool zero_reads = false;             // reads are acknowledged with no data
  uint32_t rng = 12345;

  FakeAuxSink() { dpcd.resize(0x400); for (size_t i = 0; i < dpcd.size(); ++i) dpcd[i] = static_cast<uint8_t>(i ^ 0x5A); }

  uint32_t next() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

  // `request` is the 20 bytes the table was given at its request pointer, `params1` the second parameter word (channel, delay, reply status, data length, hot plug pin)
  Answer handle(const uint8_t* request, uint32_t params1, uint32_t pointer, uint32_t out_pointer) {
    Request req;
    req.pointer = pointer;
    req.out_pointer = out_pointer;
    req.channel = params1 & 0xFF;
    req.delay = params1 >> 8 & 0xFF;
    req.hpd = params1 >> 24;
    const uint8_t* head = request;
    req.length_byte = head[3];
    const uint32_t raw = head[3] >> 4;
    const uint32_t wire = raw ? raw : 16;     // the number of bytes of the request on the wire (the four bits hold 16 as 0)
    for (uint32_t i = 0; i < wire && i < 20; ++i) req.bytes.push_back(request[i]);
    requests.push_back(req);
    return answer(req, head, raw);
  }

 private:
  Answer finish(uint8_t status, std::vector<uint8_t> data = {}) {
    Answer a;
    a.status = status;
    a.length = static_cast<uint32_t>(data.size()) + claim_extra;
    a.data = data;
    return a;
  }

  Answer answer(const Request& req, const uint8_t* head, uint32_t raw) {
    if (timeouts) { --timeouts; return finish(1); }
    if (busy) { --busy; return finish(2); }
    if (errors) { --errors; return finish(3); }
    if (random_replies) {
      std::vector<uint8_t> junk(next() % 40);
      for (uint8_t& b : junk) b = static_cast<uint8_t>(next());
      claim_extra = next() % 8 == 0 ? next() % 300 : 0;
      return finish(next() % 4 == 0 ? static_cast<uint8_t>(next()) : static_cast<uint8_t>(next() % 3 ? 0 : (next() % 16) << 4), junk);
    }
    const uint32_t address = head[0] | head[1] << 8 | (head[2] & 0xF) << 16;
    const bool bare = raw == 3;
    const uint32_t command = head[2] >> 4;
    const bool is_read = command == 9 || (command < 8 && (command & 3) == 1);
    const uint32_t count = bare ? 0 : (head[3] & 0xF) + 1;
    if (bare ? (head[3] & 0xF) != 0 : raw != (is_read ? 4 : (4 + count) & 15)) ++malformed;
    std::vector<uint8_t> data;
    if (native_nacks) { --native_nacks; return finish(0x10); }
    if (native_defers) { --native_defers; return finish(0x20); }
    if (command == 9) {
      if (address + count > dpcd.size()) return finish(0x10);
      for (uint32_t i = 0; i < count && !zero_reads; ++i) data.push_back(dpcd[address + i]);
      return finish(native_odd_reply ? 0x30 : native_reply_extra, data);
    } else if (command == 8) {
      if (address + count > dpcd.size()) return finish(0x10);
      for (uint32_t i = 0; i < count; ++i) dpcd[address + i] = req.bytes[4 + i];
      return finish(native_odd_reply ? 0x30 : native_reply_extra);
    } else {
      const bool mot = command & 4, read = is_read;
      if (i2c_defers) { --i2c_defers; return finish(0x80); }
      if (!monitor_present || (address != 0x50 && address != 0x30)) return finish(0x40);
      if (bare) {
        in_transaction = mot;
        if (!mot) segment = 0;
      } else {
        if (!in_transaction) ++outside_transaction;
        if (!read) {
          if (address == 0x30) segment = req.bytes[4];
          else offset = req.bytes[4];
        } else if (address == 0x50) {
          const uint32_t n = zero_reads ? 0 : max_read && count > max_read ? max_read : count;
          for (uint32_t i = 0; i < n; ++i) {
            const size_t at = segment * 256 + offset + i;
            data.push_back(at < edid.size() ? edid[at] : 0xFF);
          }
          offset += n;
        }
      }
    }
    return finish(0, data);
  }
};

// The sink on the card's registers, for a video BIOS whose ProcessAuxChannelTransaction table passes what it was given to the card: the parameter words to 0x7011, the five request words
// (scratch words 1 to 5) to 0x7021..0x7025, then 0x7010 as the doorbell; the answer is read back from 0x7012 (reply status), 0x7013 (length) and 0x7030..0x7033 (the data, 16 bytes).
struct FakeAuxCard final : IVcnRegisters {
  static constexpr uint32_t kDoorbell = 0x7010, kParams1 = 0x7011, kStatus = 0x7012, kLength = 0x7013, kRequest = 0x7020, kData = 0x7030;
  FakeAuxSink sink;
  uint32_t params1 = 0, status = 0, length = 0, request[6] = {}, data[4] = {};
  uint32_t doorbells = 0;
  uint32_t read(uint32_t r) override {
    if (r == kStatus) return status;
    if (r == kLength) return length;
    if (r >= kData && r < kData + 4) return data[r - kData];
    return 0;
  }
  void write(uint32_t r, uint32_t v) override {
    if (r == kParams1) params1 = v;
    if (r > kRequest && r < kRequest + 6) request[r - kRequest] = v;
    if (r != kDoorbell) return;
    ++doorbells;
    uint8_t bytes[20];
    for (uint32_t i = 0; i < 20; ++i) bytes[i] = static_cast<uint8_t>(request[1 + i / 4] >> (i % 4 * 8));
    const FakeAuxSink::Answer a = sink.handle(bytes, params1, v & 0xFFFF, v >> 16);
    status = a.status;
    length = a.length;
    uint8_t out[16] = {};
    for (size_t i = 0; i < a.data.size() && i < 16; ++i) out[i] = a.data[i];
    for (uint32_t i = 0; i < 4; ++i) data[i] = out[i * 4] | out[i * 4 + 1] << 8 | out[i * 4 + 2] << 16 | uint32_t{out[i * 4 + 3]} << 24;
  }
};

}  // namespace hardware
