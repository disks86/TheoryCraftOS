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

// A model of a Thunderbolt 3 host interface (Alpine Ridge NHI) with its connection manager firmware, for ThunderboltTest: the ring registers of every hop (the descriptors are read and
// written in host memory the way the chip does), the capability word, the firmware status and the mailbox, and on hop 0 a firmware that answers the control channel: router
// configuration space reads and writes, DRIVER_READY, APPROVE_DEVICE and GET_ROUTE, and that sends device events. The frames are built and checked here with an independent CRC-32C.
#pragma once
#include <stdint.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/Bus/Thunderbolt/ThunderboltRegisters.hpp"

namespace fake_tb {

inline uint32_t crc32c_bitwise(const uint8_t* data, size_t n) {
  uint32_t crc = ~0u;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
  }
  return ~crc;
}

// A wire frame from the words of a body (each sent most significant byte first) with its CRC.
inline std::vector<uint8_t> wire_frame(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> out;
  for (uint32_t w : words) for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(w >> s));
  const uint32_t crc = crc32c_bitwise(out.data(), out.size());
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(crc >> s));
  return out;
}

// The words of a wire frame; empty if it is malformed or the CRC is wrong.
inline std::vector<uint32_t> words_of(const std::vector<uint8_t>& frame, bool check_crc = true) {
  std::vector<uint32_t> words;
  if (frame.size() < 8 || frame.size() % 4) return words;
  const size_t n = frame.size() - 4;
  if (check_crc) {
    const uint32_t crc = crc32c_bitwise(frame.data(), n);
    const uint32_t stored = (uint32_t(frame[n]) << 24) | (uint32_t(frame[n + 1]) << 16) | (uint32_t(frame[n + 2]) << 8) | frame[n + 3];
    if (crc != stored) return words;
  }
  for (size_t i = 0; i < n; i += 4) words.push_back((uint32_t(frame[i]) << 24) | (uint32_t(frame[i + 1]) << 16) | (uint32_t(frame[i + 2]) << 8) | frame[i + 3]);
  return words;
}

enum Pdf : uint8_t { kRead = 1, kWrite = 2, kError = 3, kEvent = 5, kIcmEvent = 10, kIcmCommand = 11, kIcmResponse = 12 };

struct Device {
  uint8_t uuid[16] = {};
  uint8_t key = 0, id = 0, link = 1, depth = 1;
  bool approved = false;
};

class FakeNhi final : public hardware::ITbRegisters {
 public:
  struct Ring {
    uint64_t base = 0;
    uint32_t size = 0, head = 0, tail = 0, frame_size = 0, options = 0;
  };
  struct SentFrame {
    uint32_t hop;
    uint8_t sof, eof;
    std::vector<uint8_t> bytes;
  };

  // configuration of the model
  uint32_t hops = 12, version = 1;
  bool present = true, icm_enabled = true, auth_done = true;
  uint32_t mode = 3;                         // REG_OUTMAIL_CMD operation mode: 3 = connection manager
  uint32_t security = 1;                     // user approval
  bool answer_driver_ready = true, approve_error = false, mailbox_error = false, mailbox_hang = false;
  uint32_t drop_config_replies = 0;          // replies to configuration reads that are not sent
  uint32_t unreadable_root_reads = 0;        // root router reads that fail with an error packet first
  uint32_t error_code = 0;                   // set: answer configuration requests with this error code
  uint64_t route_answer = 0x0000000000000001ull;

  // what the host did
  std::vector<uint32_t> mailbox_commands;
  std::vector<SentFrame> sent;
  std::vector<Device> devices;
  std::map<uint32_t, uint32_t> config_words;   // word offset of the switch space -> value
  std::map<uint32_t, uint32_t> writes;         // configuration words the host wrote
  std::vector<uint32_t> uuid_approved;
  uint32_t interrupt_mask_writes = 0, interrupt_clear_writes = 0;

  uint32_t read32(uint32_t offset) override {
    using namespace hardware::tb_reg;
    if (!present) return 0xFFFFFFFFu;
    if (offset == kCaps) return (version << kCapsVersionShift) | hops;
    if (offset == kFirmwareStatus) return (icm_enabled ? kFwStsIcmEnable : 0) | (auth_done ? kFwStsNvmAuthDone : 0);
    if (offset == kOutmailCmd) return mode << kOutmailOpModeShift;
    if (is_ring_register(offset)) {
      Ring& r = ring_of(offset);
      const uint32_t field = field_of(offset);
      if (field == 8) return is_tx(offset) ? (r.head << 16) | r.tail : (r.tail << 16) | r.head;
    }
    auto it = regs_.find(offset);
    return it == regs_.end() ? 0 : it->second;
  }

  void write32(uint32_t offset, uint32_t value) override {
    using namespace hardware::tb_reg;
    if (offset == kInmailCmd) {
      if ((value & kInmailRequest) && !mailbox_hang) {
        mailbox_commands.push_back(value & kInmailCmdMask);
        value &= ~kInmailRequest;
        if (mailbox_error) value |= kInmailError;
      }
      regs_[offset] = value;
      return;
    }
    if (offset >= kRingInterruptMaskClearBase && offset < kRingInterruptMaskClearBase + 0x100) ++interrupt_mask_writes;
    if (offset >= kRingIntClear && offset < kRingIntClear + 0x100) ++interrupt_clear_writes;
    if (is_ring_register(offset)) {
      Ring& r = ring_of(offset);
      const uint32_t field = field_of(offset);
      const bool tx = is_tx(offset);
      if (field == 0) r.base = (r.base & ~0xFFFFFFFFull) | value;
      else if (field == 4) r.base = (r.base & 0xFFFFFFFFull) | (uint64_t(value) << 32);
      else if (field == 12) { r.size = value & 0xFFFF; r.frame_size = value >> 16; }
      else if (field == 8) {
        // (the host disables a ring before it clears its indices; a doorbell of zero on a running ring is the index zero)
        if (!(r.options & hardware::tb_reg::kRingFlagEnable)) { r.head = r.tail = 0; }
        else if (tx) { r.head = value >> 16; run_tx(hop_of(offset)); }
        else { r.head = value & 0xFFFF; deliver(hop_of(offset)); }
      }
      return;
    }
    if (offset >= kTxOptionsBase && offset < kTxOptionsBase + 32 * 64) { if ((offset - kTxOptionsBase) % 32 == 0) tx_[(offset - kTxOptionsBase) / 32].options = value; return; }
    if (offset >= kRxOptionsBase && offset < kRxOptionsBase + 32 * 64) { if ((offset - kRxOptionsBase) % 32 == 0) rx_[(offset - kRxOptionsBase) / 32].options = value; return; }
    regs_[offset] = value;
  }

  // ---- the model's side of the rings
  Ring& tx(uint32_t hop) { return tx_[hop]; }
  Ring& rx(uint32_t hop) { return rx_[hop]; }
  bool tx_enabled(uint32_t hop) { return (tx_[hop].options & hardware::tb_reg::kRingFlagEnable) != 0; }

  // A frame for the host on a hop: delivered into the next posted receive buffer.
  void push_to_host(uint32_t hop, uint8_t sof, uint8_t eof, std::vector<uint8_t> bytes) {
    queue_[hop].push_back({hop, sof, eof, std::move(bytes)});
    deliver(hop);
  }
  void send_icm_event(const std::vector<uint32_t>& words) { push_to_host(0, kIcmEvent, kIcmEvent, wire_frame(words)); }

  // ICM_EVENT_DEVICE_CONNECTED
  void send_device_connected(const Device& d, bool approved = false, bool rejected = false, uint32_t flags = 0) {
    std::vector<uint32_t> w(61, 0);
    w[0] = 3 | (flags << 8) | (0u << 16) | (1u << 24);
    memcpy(&w[1], d.uuid, 16);
    w[5] = d.key | (uint32_t(d.id) << 8) | ((uint32_t(d.link) | (uint32_t(d.depth) << 4) | (approved ? 1u << 8 : 0) | (rejected ? 1u << 9 : 0)) << 16);
    send_icm_event(w);
  }
  void send_device_disconnected(uint8_t link, uint8_t depth) { send_icm_event({4u | (1u << 24), (uint32_t(link) | (uint32_t(depth) << 4)) << 16}); }

 private:
  static bool is_ring_register(uint32_t o) { return (o < 0x10000) && ((o & 0x7FFF) < 16 * 64); }
  static bool is_tx(uint32_t o) { return o < 0x8000; }
  static uint32_t hop_of(uint32_t o) { return (o & 0x7FFF) / 16; }
  static uint32_t field_of(uint32_t o) { return o % 16; }
  Ring& ring_of(uint32_t o) { return is_tx(o) ? tx_[hop_of(o)] : rx_[hop_of(o)]; }

  uint32_t* descriptor(const Ring& r, uint32_t index) { return reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(r.base) + static_cast<uintptr_t>(index) * 16); }

  // The host posted transmit descriptors: send what they point at.
  void run_tx(uint32_t hop) {
    Ring& r = tx_[hop];
    if (!(r.options & hardware::tb_reg::kRingFlagEnable) || r.size == 0 || r.base == 0) return;
    while (r.tail != r.head) {
      uint32_t* d = descriptor(r, r.tail);
      const uint32_t word = d[2];
      const uint32_t length = word & 0xFFF;
      const uint8_t eof = (word >> 12) & 0xF, sof = (word >> 16) & 0xF;
      const uint8_t* buffer = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(uint64_t(d[0]) | (uint64_t(d[1]) << 32)));
      std::vector<uint8_t> bytes(buffer, buffer + length);
      d[2] = word | (0x2u << 20);                    // COMPLETED
      r.tail = (r.tail + 1) % r.size;
      sent.push_back({hop, sof, eof, bytes});
      if (hop == 0) control_frame(eof, bytes);
    }
  }

  // Frames queued for the host go into the posted receive buffers.
  void deliver(uint32_t hop) {
    Ring& r = rx_[hop];
    if (!(r.options & hardware::tb_reg::kRingFlagEnable) || r.size == 0 || r.base == 0) return;
    auto& q = queue_[hop];
    while (!q.empty() && r.tail != r.head) {
      uint32_t* d = descriptor(r, r.tail);
      const SentFrame& f = q.front();
      uint8_t* buffer = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(uint64_t(d[0]) | (uint64_t(d[1]) << 32)));
      const uint32_t cap = r.frame_size ? r.frame_size : 4096;
      uint32_t flags = 0x2;                          // COMPLETED
      uint32_t length = static_cast<uint32_t>(f.bytes.size());
      if (length > cap) { length = cap; flags |= 0x4; }   // overrun
      memcpy(buffer, f.bytes.data(), length);
      d[2] = (length & 0xFFF) | (uint32_t(f.eof & 0xF) << 12) | (uint32_t(f.sof & 0xF) << 16) | (flags << 20);
      r.tail = (r.tail + 1) % r.size;
      q.erase(q.begin());
    }
  }

  void reply(uint8_t eof, const std::vector<uint32_t>& words) { push_to_host(0, 0, eof, wire_frame(words)); }

  // The firmware's side of hop 0.
  void control_frame(uint8_t eof, const std::vector<uint8_t>& frame) {
    const bool icm = eof == kIcmCommand;
    std::vector<uint32_t> w = words_of(frame, !icm);
    if (w.empty()) return;
    if (eof == kIcmCommand) { icm_command(w); return; }
    if (eof != kRead && eof != kWrite) return;
    // configuration space request: route (22 + 32 bits), then offset:13 length:6 port:6 space:2 seq:2
    const uint32_t route_hi = w[0] & 0x3FFFFF, route_lo = w[1], addr = w[2];
    const uint32_t offset = addr & 0x1FFF, length = (addr >> 13) & 0x3F, seq = (addr >> 27) & 3, space = (addr >> 25) & 3;
    // (the model keeps what it was told to as the host's byte order of little endian words: the wire words are the values themselves)
    if (route_hi == 0 && route_lo == 0 && space == 2 && offset == 0 && unreadable_root_reads > 0) {
      --unreadable_root_reads;
      reply(kError, {route_hi | (0x200u << 22), route_lo, 2u /* invalid config space */ | (0u << 8)});
      return;
    }
    if (error_code != 0) {
      reply(kError, {route_hi | (0x200u << 22), route_lo, error_code | (3u << 8)});
      return;
    }
    if (drop_config_replies > 0) { --drop_config_replies; return; }
    std::vector<uint32_t> out = {route_hi | (0x200u << 22), route_lo, addr};
    if (eof == kRead) {
      for (uint32_t i = 0; i < length; ++i) {
        auto it = config_words.find(offset + i);
        out.push_back(it == config_words.end() ? 0xDEAD0000u + offset + i : it->second);
      }
    } else {
      for (uint32_t i = 0; i < length && 3 + i < w.size(); ++i) writes[offset + i] = w[3 + i];
    }
    reply(static_cast<uint8_t>(eof), out);
  }

  // (the words of a body are the little endian values of the bytes the driver wrote: an ICM header is code | flags << 8 | packet id << 16 | total packets << 24)
  void icm_command(const std::vector<uint32_t>& w) {
    const uint8_t code = static_cast<uint8_t>(w[0] & 0xFF);
    if (code == 3) {
      if (!answer_driver_ready) return;
      reply(kIcmResponse, {3u | (1u << 24), 5u | (7u << 8) | ((security & 0xFu) << 16)});
    } else if (code == 4 && w.size() >= 6) {
      Device d;
      memcpy(d.uuid, &w[1], 16);
      uuid_approved.push_back(w[1]);
      for (Device& known : devices)
        if (memcmp(known.uuid, d.uuid, 16) == 0) known.approved = !approve_error;
      std::vector<uint32_t> out(w.begin(), w.begin() + 6);
      out[0] = 4u | ((approve_error ? 1u : 0u) << 8) | (1u << 24);
      reply(kIcmResponse, out);
    } else if (code == 0xA && w.size() >= 2) {
      reply(kIcmResponse, {0xAu | (1u << 24), w[1], static_cast<uint32_t>(route_answer >> 32), static_cast<uint32_t>(route_answer)});
    }
  }

  std::map<uint32_t, uint32_t> regs_;
  Ring tx_[64], rx_[64];
  std::map<uint32_t, std::vector<SentFrame>> queue_;
};

}  // namespace fake_tb
