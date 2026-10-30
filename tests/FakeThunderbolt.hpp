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
#include <functional>
#include <map>
#include <vector>
#include "Crypto/Crypto.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltCio.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltIcl.hpp"
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

static const uint8_t service_uuid[16] = {0x79, 0x8F, 0x58, 0x9E, 0x36, 0x16, 0x8A, 0x47, 0x97, 0xC6, 0x56, 0x64, 0xA9, 0x20, 0xC8, 0xDD};
static const uint8_t protocol_uuid[16] = {0xB6, 0x38, 0xD7, 0x0E, 0x42, 0xFF, 0x40, 0xBB, 0x97, 0xC2, 0x90, 0xE2, 0xC0, 0xB2, 0xFF, 0x07};

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
  // the other host on an XDomain connection
  struct Remote {
    uint8_t uuid[16] = {};
    std::vector<uint32_t> block;                     // its property block, as the values of the words
    uint32_t generation = 5;
    bool answer = true, wrong_uuid = false, not_ready = false, wrong_offset = false, shrink_length = false;
    uint32_t drop_first = 0;                        // requests not answered
  } remote;
  // the network service of the other host
  struct Peer {
    bool answer_login = true, approve_error = false;
    uint32_t path = 9;                               // the HopID the peer sends on
    uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint32_t host_path = 0;                          // what the host's login said it sends on
    std::vector<uint32_t> logins_seen, command_ids;  // the types of the service messages the host sent
    bool logout_seen = false, status_seen = false;
    bool approved = false;
    uint32_t tx_path = 0, tx_ring = 0, rx_path = 0, rx_ring = 0;     // from ICM_APPROVE_XDOMAIN: the host sends on tx_path from its ring tx_ring, we send on rx_path into its ring rx_ring
    std::vector<std::vector<uint32_t>> service_messages;
  } peer;
  uint32_t xdomain_approvals = 0, xdomain_disconnects = 0;
  std::vector<std::vector<uint32_t>> xdp_requests;   // what the host sent, as words
  std::vector<std::vector<uint32_t>> xdp_responses;  // what the host answered to the remote's requests
  bool titan_ridge = false;                  // the Titan Ridge forms of the messages
  bool rtd3 = false;                         // DRIVER_READY says the firmware supports runtime power management
  bool answer_get_route = true;
  // secure connect: the keys the devices hold (by the first word of the UUID), and how they misbehave
  std::map<uint32_t, std::vector<uint8_t>> device_keys;
  bool add_key_error = false, challenge_error = false, challenge_wrong = false;
  std::vector<std::vector<uint8_t>> challenges;        // what the host sent
  std::vector<uint8_t> requests_seen;                  // the ICM codes of the requests, in order
  // GET_TOPOLOGY: one entry per router, answered in as many packets
  struct Router { uint64_t route = 0; bool used = true; uint8_t upstream = 0; uint32_t ports[16] = {}; };
  std::vector<Router> topology;
  bool answer_topology = true, topology_reversed = false;
  int topology_drop = -1;                    // a packet that is not sent
  uint32_t topology_total_override = 0;      // what the packets say the total is (0: the real number)

  // what the host did
  std::vector<uint32_t> mailbox_commands;
  std::vector<SentFrame> sent;
  std::vector<Device> devices;
  std::map<uint32_t, uint32_t> config_words;   // word offset of the switch space -> value
  std::map<uint32_t, uint32_t> writes;         // configuration words the host wrote
  std::vector<uint32_t> uuid_approved;
  std::vector<uint64_t> approved_routes;     // Titan Ridge: the route strings of the approvals
  std::vector<uint32_t> fw_sts_writes;       // every write to the firmware status register, in order
  uint32_t fw_sts_written = 0;
  bool firmware_comes_up_on_reset = true;    // a CIO reset after the ARC was told to wait for it starts the firmware
  uint32_t cio_resets = 0;
  uint32_t interrupt_mask_writes = 0, interrupt_clear_writes = 0;
  uint32_t boot_acl_entries = 0;                   // the firmware keeps a preboot ACL of this many entries (0: none)
  uint32_t acl[32] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                      0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  bool acl_error = false;                          // the ACL requests end with the error flag
  uint32_t acl_writes = 0;
  std::function<void()> raise_interrupt;           // the MSI: called when a ring that is unmasked completes something
  uint32_t notify_reads = 0, interrupts_raised = 0;
  uint32_t unmasked[64] = {};                      // the interrupt enable registers as the host left them
  uint32_t notify[96] = {};

  uint32_t read32(uint32_t offset) override {
    using namespace hardware::tb_reg;
    if (!present) return 0xFFFFFFFFu;
    if (offset >= kRingNotifyBase && offset < kRingNotifyBase + 96 * 4 && (offset - kRingNotifyBase) % 4 == 0 && offset != kRingIntClear && offset != kRingIntClear + 4) {
      uint32_t& n = notify[(offset - kRingNotifyBase) / 4];
      const uint32_t v = n;
      n = 0;
      ++notify_reads;
      return v;
    }
    if (offset == kCaps) return (version << kCapsVersionShift) | hops;
    if (offset == kFirmwareStatus) return (icm_enabled ? kFwStsIcmEnable : 0) | (auth_done ? kFwStsNvmAuthDone : 0) | (fw_sts_written & (kFwStsCioResetRequest | kFwStsIcmEnableCpu | kFwStsIcmEnableInvert));
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
    if (offset == kFirmwareStatus) {
      fw_sts_writes.push_back(value);
      fw_sts_written = value;
      return;
    }
    if (offset == kInmailCmd) {
      if ((value & kInmailRequest) && !mailbox_hang) {
        mailbox_commands.push_back(value & kInmailCmdMask);
        value &= ~kInmailRequest;
        if (mailbox_error) value |= kInmailError;
      }
      regs_[offset] = value;
      return;
    }
    if (offset >= kRingInterruptBase && offset < kRingInterruptBase + 64 * 4 && (offset - kRingInterruptBase) % 4 == 0) {
      ++interrupt_mask_writes;
      if (offset != kRingInterruptMaskClearBase) unmasked[(offset - kRingInterruptBase) / 4] = value;
    }
    if (offset >= kRingInterruptMaskClearBase && offset < kRingInterruptMaskClearBase + 0x100) {
      ++interrupt_mask_writes;
      if ((offset - kRingInterruptMaskClearBase) % 4 == 0 && (offset - kRingInterruptMaskClearBase) / 4 < 64) unmasked[(offset - kRingInterruptMaskClearBase) / 4] &= ~value;
    }
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

  // A descriptor of a ring completed: the chip raises the ring's bit and the MSI if the host unmasked it.
  void completed(bool tx_ring, uint32_t hop) {
    const uint32_t index = tx_ring ? hop : hop + hops;
    const uint32_t reg = index / 32, bit = 1u << (index % 32);
    if (reg >= 64 || !(unmasked[reg] & bit)) return;
    notify[reg] |= bit;
    ++interrupts_raised;
    if (raise_interrupt) raise_interrupt();
  }

  // The root router is reset through the PCIe-to-CIO registers.
  void cio_reset() {
    ++cio_resets;
    using namespace hardware::tb_reg;
    const uint32_t need = kFwStsCioResetRequest | kFwStsIcmEnableCpu | kFwStsIcmEnableInvert;
    if (firmware_comes_up_on_reset && (fw_sts_written & need) == need) { icm_enabled = true; auth_done = true; }
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
  // Titan Ridge: the device named by its route string; `packet_id` other than 0 is a quality of service packet
  void send_device_connected_tr(const Device& d, uint64_t route, bool approved = false, bool rejected = false, uint32_t packet_id = 0) {
    std::vector<uint32_t> w(61, 0);
    w[0] = 3u | (packet_id << 16) | (1u << 24);
    memcpy(&w[1], d.uuid, 16);
    w[5] = static_cast<uint32_t>(route >> 32);
    w[6] = static_cast<uint32_t>(route);
    w[7] = d.id | ((uint32_t(approved ? 1u << 8 : 0) | (rejected ? 1u << 9 : 0)) << 16);
    send_icm_event(w);
  }
  void send_device_disconnected_tr(uint64_t route) { send_icm_event({4u | (1u << 24), static_cast<uint32_t>(route >> 32), static_cast<uint32_t>(route)}); }
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
      completed(true, hop);
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
      completed(false, hop);
    }
  }

  void reply(uint8_t eof, const std::vector<uint32_t>& words) { push_to_host(0, 0, eof, wire_frame(words)); }

  // The firmware's side of hop 0.
  void control_frame(uint8_t eof, const std::vector<uint8_t>& frame) {
    const bool icm = eof == kIcmCommand;
    std::vector<uint32_t> w = words_of(frame, !icm);
    if (w.empty()) return;
    if (eof == kIcmCommand) { icm_command(w); return; }
    if (eof == 6) { xdp_request(w); return; }
    if (eof == 7) {
      if (w.size() >= 7 && memcmp(&w[3], fake_tb::service_uuid, 16) == 0) service_message(w);
      else xdp_responses.push_back(w);
      return;
    }
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
  // the host's ThunderboltIP messages (login, logout) to the other host
  void service_message(const std::vector<uint32_t>& w) {
    peer.service_messages.push_back(w);
    if (w.size() < 17) return;
    const uint32_t type = w[15], sn = (w[2] >> 27) & 3;
    std::vector<uint32_t> out(w.begin(), w.begin() + 17);
    out[0] = w[0] | 0x80000000u;
    // the answer is from the other host: initiator and target swap places
    for (int i = 0; i < 4; ++i) { out[7 + i] = w[11 + i]; out[11 + i] = w[7 + i]; }
    if (type == 0) {                                       // login
      peer.host_path = w.size() > 18 ? w[18] : 0;
      peer.logins_seen.push_back(type);
      peer.command_ids.push_back(w[16]);
      if (!peer.answer_login) return;
      out[15] = 1;
      out.resize(25, 0);                                   // status, mac (2 words), mac length, 4 reserved
      out[17] = 0;
      memcpy(&out[18], peer.mac, 6);
      out[20] = 6;
      out[2] = (static_cast<uint32_t>(out.size() * 4 - 12) / 4) | (sn << 27);
      reply(7, out);
    } else if (type == 2) {                                // logout
      peer.logout_seen = true;
      out[15] = 3;
      out.resize(18, 0);
      out[2] = (static_cast<uint32_t>(out.size() * 4 - 12) / 4) | (sn << 27);
      reply(7, out);
    }
  }

public:
  // The other host logs in to ours (a login request frame on the control channel); `host`/`remote` are the two UUIDs.
  void peer_login(const uint8_t remote_uuid[16], const uint8_t host_uuid[16], uint64_t route, uint32_t command_id, uint32_t path, uint32_t sn = 0) {
    std::vector<uint32_t> w(23, 0);
    w[0] = static_cast<uint32_t>(route >> 32);
    w[1] = static_cast<uint32_t>(route);
    memcpy(&w[3], fake_tb::service_uuid, 16);
    memcpy(&w[7], remote_uuid, 16);
    memcpy(&w[11], host_uuid, 16);
    w[15] = 0;
    w[16] = command_id;
    w[17] = 1;
    w[18] = path;
    w[2] = static_cast<uint32_t>(w.size() * 4 - 12) / 4 | (sn << 27);
    push_to_host(0, 7, 7, wire_frame(w));
  }
  void peer_logout(const uint8_t remote_uuid[16], const uint8_t host_uuid[16], uint64_t route, uint32_t command_id) {
    std::vector<uint32_t> w(17, 0);
    w[0] = static_cast<uint32_t>(route >> 32);
    w[1] = static_cast<uint32_t>(route);
    memcpy(&w[3], fake_tb::service_uuid, 16);
    memcpy(&w[7], remote_uuid, 16);
    memcpy(&w[11], host_uuid, 16);
    w[15] = 2;
    w[16] = command_id;
    w[2] = static_cast<uint32_t>(w.size() * 4 - 12) / 4;
    push_to_host(0, 7, 7, wire_frame(w));
  }
  // The other host sends an Ethernet frame through the data path: one or more frames of at most `chunk` bytes of data each.
  void peer_send(const std::vector<uint8_t>& packet, uint32_t id, uint32_t chunk = 4084) {
    const uint32_t count = static_cast<uint32_t>((packet.size() + chunk - 1) / chunk);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t size = i + 1 == count ? static_cast<uint32_t>(packet.size()) - i * chunk : chunk;
      std::vector<uint8_t> frame(12 + size, 0);
      const uint32_t header[3] = {size, i | (id << 16), count};
      memcpy(frame.data(), header, 12);
      memcpy(frame.data() + 12, packet.data() + static_cast<size_t>(i) * chunk, size);
      push_to_host(peer.rx_ring, 1, 2, frame);
    }
  }

private:
  // an XDomain request from the host: answered the way another host would
  void xdp_request(const std::vector<uint32_t>& w) {
    xdp_requests.push_back(w);
    if (w.size() < 8 || !remote.answer) return;
    if (remote.drop_first > 0) { --remote.drop_first; return; }
    const uint32_t type = w[7];
    const uint32_t sn = (w[2] >> 27) & 3;
    auto header = [&](std::vector<uint32_t>& out, uint32_t t) {
      out.assign({w[0] | 0x80000000u, w[1], 0, 0, 0, 0, 0, t});
      memcpy(&out[3], fake_tb::protocol_uuid, 16);
    };
    auto finish = [&](std::vector<uint32_t>& out) {
      out[2] = static_cast<uint32_t>(out.size() * 4 - 12) / 4 | (sn << 27);
      reply(7, out);
    };
    std::vector<uint32_t> out;
    if (type == 12) {                                                   // UUID_REQUEST
      if (remote.not_ready) { header(out, 7); out.push_back(4); finish(out); return; }
      header(out, 2);
      uint8_t u[16];
      memcpy(u, remote.uuid, 16);
      if (remote.wrong_uuid) u[0] ^= 0xFF;
      out.resize(8 + 4);
      memcpy(&out[8], u, 16);
      out.push_back(w[0] & 0x3FFFFF);
      out.push_back(w[1]);
      finish(out);
    } else if (type == 3 && w.size() >= 17) {                           // PROPERTIES_REQUEST: src, dst, offset
      const uint32_t offset = w[16] & 0xFFFF;
      header(out, 4);
      out.resize(8 + 8);
      memcpy(&out[8], &w[12], 16);                                      // (the answer's source is the request's destination)
      memcpy(&out[12], &w[8], 16);
      uint32_t length = static_cast<uint32_t>(remote.block.size());
      if (offset >= length) return;
      uint32_t count = length - offset > 45 ? 45 : length - offset;
      out.push_back((remote.wrong_offset ? offset + 1 : offset) | ((remote.shrink_length && offset > 0 ? length - 1 : length) << 16));
      out.push_back(remote.generation);
      for (uint32_t i = 0; i < count; ++i) out.push_back(remote.block[offset + i]);
      finish(out);
    }
  }

public:
  // The other host asks us something (a request frame on the control channel).
  void remote_request(uint32_t type, const std::vector<uint32_t>& extra, uint64_t route = 0x01, uint32_t sn = 0) {
    std::vector<uint32_t> w = {static_cast<uint32_t>(route >> 32), static_cast<uint32_t>(route), 0, 0, 0, 0, 0, type};
    memcpy(&w[3], fake_tb::protocol_uuid, 16);
    for (uint32_t x : extra) w.push_back(x);
    w[2] = static_cast<uint32_t>(w.size() * 4 - 12) / 4 | (sn << 27);
    push_to_host(0, 6, 6, wire_frame(w));
  }
  void send_xdomain_connected(const uint8_t remote_uuid[16], const uint8_t local_uuid[16], uint8_t link, uint8_t depth, uint64_t local_route) {
    std::vector<uint32_t> w(14, 0);
    w[0] = 6u | (1u << 24);
    w[1] = (uint32_t(link) | (uint32_t(depth) << 4)) << 16;
    memcpy(&w[2], remote_uuid, 16);
    memcpy(&w[6], local_uuid, 16);
    w[10] = static_cast<uint32_t>(local_route >> 32);
    w[11] = static_cast<uint32_t>(local_route);
    send_icm_event(w);
  }
  void send_xdomain_disconnected(const uint8_t remote_uuid[16], uint8_t link, uint8_t depth) {
    std::vector<uint32_t> w(6, 0);
    w[0] = 7u | (1u << 24);
    w[1] = (uint32_t(link) | (uint32_t(depth) << 4)) << 16;
    memcpy(&w[2], remote_uuid, 16);
    send_icm_event(w);
  }
  void send_xdomain_disconnected_tr(const uint8_t remote_uuid[16], uint64_t route) {
    std::vector<uint32_t> w(7, 0);
    w[0] = 7u | (1u << 24);
    w[1] = static_cast<uint32_t>(route >> 32);
    w[2] = static_cast<uint32_t>(route);
    memcpy(&w[3], remote_uuid, 16);
    send_icm_event(w);
  }

private:
  void icm_command(const std::vector<uint32_t>& w) {
    const uint8_t code = static_cast<uint8_t>(w[0] & 0xFF);
    requests_seen.push_back(code);
    if (code == 3) {
      if (!answer_driver_ready) return;
      const uint32_t hdr = 3u | ((rtd3 ? 0x40u : 0u) << 8) | (1u << 24);
      const uint32_t acl_info = boot_acl_entries ? ((boot_acl_entries << 7) | (titan_ridge ? 0u : (1u << 13))) : 0u;
      if (titan_ridge) reply(kIcmResponse, {hdr, ((security & 0x7u) | acl_info) << 16, 0x00100002u, 0x15EBu});
      else reply(kIcmResponse, {hdr, 5u | (7u << 8) | (((security & 0xFu) | acl_info) << 16)});
    } else if (code == 4 && w.size() >= (titan_ridge ? 8u : 6u)) {
      Device d;
      memcpy(d.uuid, &w[1], 16);
      uuid_approved.push_back(w[1]);
      if (titan_ridge) approved_routes.push_back((uint64_t(w[5]) << 32) | w[6]);
      for (Device& known : devices)
        if (memcmp(known.uuid, d.uuid, 16) == 0) known.approved = !approve_error;
      std::vector<uint32_t> out(w.begin(), w.begin() + (titan_ridge ? 8 : 6));
      out[0] = 4u | ((approve_error ? 1u : 0u) << 8) | (1u << 24);
      reply(kIcmResponse, out);
    } else if (code == 6 && w.size() >= (titan_ridge ? 16u : 14u)) {
      // ADD_DEVICE_KEY: hdr, uuid, key id / route, connection id, then 32 bytes of key
      const size_t head = titan_ridge ? 8 : 6;
      std::vector<uint8_t> key(32);
      memcpy(key.data(), &w[head], 32);
      if (!add_key_error) device_keys[w[1]] = key;
      std::vector<uint32_t> out(w.begin(), w.begin() + head);
      out[0] = 6u | ((add_key_error ? 1u : 0u) << 8) | (1u << 24);
      reply(kIcmResponse, out);
    } else if (code == 5 && w.size() >= (titan_ridge ? 16u : 14u)) {
      // CHALLENGE_DEVICE: the answer is the request's header, the challenge and HMAC-SHA256 of it under the device's key
      const size_t head = titan_ridge ? 8 : 6;
      std::vector<uint32_t> out(head + 16, 0);
      for (size_t i = 0; i < head + 8; ++i) out[i] = w[i];
      std::vector<uint8_t> challenge(32);
      memcpy(challenge.data(), &w[head], 32);
      challenges.push_back(challenge);
      uint32_t flags = 0;
      auto it = device_keys.find(w[1]);
      if (it == device_keys.end()) flags |= 1u << 1;                     // no key
      else if (challenge_error) flags |= 1u;
      else {
        uint8_t mac[32];
        crypto::hmac(crypto::HashKind::Sha256, ConstByteView(it->second.data(), 32), ConstByteView(challenge.data(), 32), mac);
        if (challenge_wrong) mac[5] ^= 1;
        memcpy(&out[head + 8], mac, 32);
      }
      out[0] = 5u | (flags << 8) | (1u << 24);
      reply(kIcmResponse, out);
    } else if (code == 0x10 && w.size() >= (titan_ridge ? 9u : 8u)) {
      // ICM_APPROVE_XDOMAIN: the paths (AR: link info, uuid, two words of paths; TR: the route first)
      const size_t at = titan_ridge ? 7 : 6;
      if (!peer.approve_error) {
        peer.approved = true;
        peer.tx_path = w[at] & 0xFFFF;
        peer.tx_ring = w[at] >> 16;
        peer.rx_path = w[at + 1] & 0xFFFF;
        peer.rx_ring = w[at + 1] >> 16;
      }
      ++xdomain_approvals;
      std::vector<uint32_t> out(w.begin(), w.end());
      out[0] = 0x10u | ((peer.approve_error ? 1u : 0u) << 8) | (1u << 24);
      reply(kIcmResponse, out);
    } else if (code == 0x11 && titan_ridge) {
      ++xdomain_disconnects;
      std::vector<uint32_t> out(w.begin(), w.end());
      out[0] = 0x11u | (1u << 24);
      reply(kIcmResponse, out);
    } else if (code == 1) {
      if (!answer_topology) return;
      const uint32_t total = topology_total_override ? topology_total_override : static_cast<uint32_t>(topology.size());
      for (uint32_t k = 0; k < topology.size(); ++k) {
        const uint32_t i = topology_reversed ? static_cast<uint32_t>(topology.size()) - 1 - k : k;
        if (static_cast<int>(i) == topology_drop) continue;
        std::vector<uint32_t> out(38, 0);
        out[0] = 1u | (i << 16) | (total << 24);
        out[1] = static_cast<uint32_t>(topology[i].route);
        out[2] = static_cast<uint32_t>(topology[i].route >> 32);
        out[3] = (topology[i].used ? 1u : 0u) | (uint32_t(topology[i].upstream) << 1) | (i << 24);
        for (int port = 0; port < 16; ++port) out[6 + port] = topology[i].ports[port];
        reply(kIcmResponse, out);
      }
    } else if (code == 0x18 && w.size() >= 33) {
      acl_writes += (w[0] >> 8) & 0x80 ? 1 : 0;
      if (acl_error) {
        reply(kIcmResponse, {0x18u | (1u << 8) | (1u << 24)});
        return;
      }
      if ((w[0] >> 8) & 0x80)
        for (int i = 0; i < 32; ++i) acl[i] = w[1 + i];
      std::vector<uint32_t> out(33);
      out[0] = 0x18u | (1u << 24);
      for (int i = 0; i < 32; ++i) out[1 + i] = acl[i];
      reply(kIcmResponse, out);
    } else if (code == 0xA && w.size() >= 2) {
      if (!answer_get_route) return;
      reply(kIcmResponse, {0xAu | (1u << 24), w[1], static_cast<uint32_t>(route_answer >> 32), static_cast<uint32_t>(route_answer)});
    }
  }

  std::map<uint32_t, uint32_t> regs_;
  Ring tx_[64], rx_[64];
  std::map<uint32_t, std::vector<SentFrame>> queue_;
};


// The upstream port's configuration space as far as the driver uses it: an extended capability list with the vendor specific capability, and the PCIe-to-CIO registers behind it.
class FakeCio final : public hardware::ITbConfigSpace {
 public:
  explicit FakeCio(FakeNhi& chip) : chip_(chip) {}
  uint16_t capability = 0x100;           // where the vendor capability is (0: none)
  bool fail_commands = false;            // every command ends with the timeout bit
  bool hang = false;                     // the start bit never clears
  bool root_resets = true;
  bool titan = false;                    // the reset is a different register on Titan Ridge
  std::map<uint32_t, uint32_t> cs1;      // port -> PHY_PORT_CS1
  struct Command { bool write; uint32_t space, port, index, value; };
  std::vector<Command> commands;

  uint32_t read32(uint16_t offset) override {
    if (capability != 0 && offset == capability) return 0x000B | (1u << 16);
    if (capability != 0 && offset == capability + hardware::tb_cio::kCmd) return cmd_;
    if (capability != 0 && offset == capability + hardware::tb_cio::kReadData) return rddata_;
    return 0;
  }
  void write32(uint16_t offset, uint32_t value) override {
    using namespace hardware::tb_cio;
    if (capability == 0) return;
    if (offset == capability + kWriteData) { wrdata_ = value; return; }
    if (offset != capability + kCmd) return;
    cmd_ = value;
    if (!(value & kCmdStart)) return;
    const uint32_t index = value & kCmdIndexMask, port = (value & kCmdPortMask) >> kCmdPortShift, space = (value & kCmdSpaceMask) >> kCmdSpaceShift;
    const bool write = (value & kCmdWrite) != 0;
    commands.push_back({write, space, port, index, write ? wrdata_ : 0});
    if (hang) return;
    cmd_ &= ~kCmdStart;
    if (fail_commands) { cmd_ |= kCmdTimeout; return; }
    if (write) {
      if (space == 2 && port == 0 && root_resets && ((index == kSwitchResetIndex && (wrdata_ & kSwitchResetBit) && !titan) || (index == kTitanSwitchResetIndex && (wrdata_ & kTitanSwitchResetBit) && titan))) chip_.cio_reset();
      if (space == 1 && index == kPhyPortCs1) cs1[port] = wrdata_;
    } else if (space == 1 && index == kPhyPortCs1) {
      rddata_ = cs1.count(port) ? cs1[port] : 0;
    } else {
      rddata_ = 0;
    }
  }

 private:
  FakeNhi& chip_;
  uint32_t cmd_ = 0, wrdata_ = 0, rddata_ = 0;
};

// A tiny PCI hierarchy for finding the switch's upstream port.
class FakePci final : public hardware::IPciBus {
 public:
  struct Function {
    hardware::PciDeviceInfo info;
    uint8_t header_type = 1, secondary = 0, subordinate = 0;
    int pcie_type = -1;                  // -1: no PCI Express capability
    bool pm = false, pme_from_d3hot = true;     // a power management capability at 0x70
    mutable uint32_t pmcsr = 0;
    uint32_t msix_size = 0;                         // a MSI-X capability at 0x90 with this many entries (0: none)
    mutable uint32_t msix_control = 0;
    mutable std::vector<uint32_t> msix_table;
    bool msix_bar_assigned = true;
  };
  std::vector<Function> functions;
  const char* name() const override { return "fake pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return static_cast<uint32_t>(functions.size()); }
  bool pci_device(uint32_t index, hardware::PciDeviceInfo* out) const override {
    if (index >= functions.size()) return false;
    *out = functions[index].info;
    return true;
  }
  const Function* find(hardware::PciAddress a) const {
    for (const Function& f : functions) if (f.info.address.bus == a.bus && f.info.address.device == a.device && f.info.address.function == a.function) return &f;
    return nullptr;
  }
  uint32_t read32(hardware::PciAddress a, uint16_t offset) const override {
    const Function* f = find(a);
    if (!f) return 0xFFFFFFFFu;
    if (offset == 0x0C) return uint32_t(f->header_type) << 16;
    if (offset == 0x18) return uint32_t(f->secondary) << 8 | uint32_t(f->subordinate) << 16;
    if (offset == 0x80 && f->pcie_type >= 0) return 0x10 | (uint32_t(f->pcie_type) << 20);
    if (f->pm && offset == 0x70) return 0x01 | (3u << 16) | (f->pme_from_d3hot ? (1u << 30) : 0);
    if (f->pm && offset == 0x74) return f->pmcsr;
    if (f->msix_size && offset == 0x90) return 0x11 | ((f->msix_control | (f->msix_size - 1)) << 16);
    if (f->msix_size && offset == 0x94) return 0;                // the table is at offset 0 of BAR 0
    return 0;
  }
  void write32(hardware::PciAddress a, uint16_t offset, uint32_t value) const override {
    const Function* f = find(a);
    if (f && f->msix_size && offset == 0x90) { f->msix_control = (value >> 16) & 0xC000; return; }
    if (!f || !f->pm || offset != 0x74) return;
    // the state and the PME enable are written; the status bit clears when a one is written
    f->pmcsr = (value & 0x103) | (f->pmcsr & 0x8000 & ~value);
  }
  void set_wake_pending(hardware::PciAddress a) const { const Function* f = find(a); if (f) f->pmcsr |= 0x8000; }
  uint64_t memory_bar(hardware::PciAddress a, uint8_t index) const override {
    const Function* f = find(a);
    if (!f || !f->msix_size || index != 0 || !f->msix_bar_assigned) return 0;
    if (f->msix_table.size() != f->msix_size * 4) f->msix_table.assign(f->msix_size * 4, 0xFFFFFFFFu);
    return reinterpret_cast<uint64_t>(f->msix_table.data());
  }
  uint16_t find_capability(hardware::PciAddress a, uint8_t id) const override {
    const Function* f = find(a);
    if (f && id == 0x10 && f->pcie_type >= 0) return 0x80;
    if (f && id == 0x01 && f->pm) return 0x70;
    if (f && id == 0x11 && f->msix_size) return 0x90;
    return 0;
  }
};


// Registers that are plain memory.
class FakeMapper final : public hardware::IRegisterMapper {
 public:
  bool fail = false;
  uint32_t mapped = 0;
  bool map_registers(uint64_t physical, uint64_t length, hardware::MmioRegion* region) override {
    if (fail) return false;
    ++mapped;
    *region = hardware::MmioRegion(physical, length);
    return true;
  }
};


// The NHI's PCI configuration space as far as an Ice Lake controller's power handling uses it.
class FakeIclConfig final : public hardware::ITbConfigSpace {
 public:
  bool firmware_comes_up = true;           // FW_READY appears once the controller is force powered
  bool lc_done = true;
  uint32_t force_power_register = 0, ltr = 0, max_ltr = 0x3FFF, lc_command = 0;
  std::vector<uint32_t> lc_commands;       // the commands sent (without the valid bit)
  uint32_t power_writes = 0;
  uint32_t read32(uint16_t offset) override {
    using namespace hardware::tb_icl;
    if (offset == kFwReadyRegister) return (firmware_comes_up && (force_power_register & kForcePower)) ? kFwReady : 0;
    if (offset == kMaxLtrRegister) return max_ltr;
    if (offset == kLtrRegister) return ltr;
    if (offset == kLcDoneRegister) return lc_done ? kLcDone : 0;
    if (offset == kLcCommandRegister) return lc_command;
    if (offset == kForcePowerRegister) return force_power_register;
    if (offset == kUuidLowRegister) return 0x11223344u;
    if (offset == kUuidHighRegister) return 0x55667788u;
    return 0;
  }
  void write32(uint16_t offset, uint32_t value) override {
    using namespace hardware::tb_icl;
    if (offset == kForcePowerRegister) { force_power_register = value; ++power_writes; }
    else if (offset == kLtrRegister) ltr = value;
    else if (offset == kLcCommandRegister) {
      lc_command = value;
      if (value & kLcValid) lc_commands.push_back((value & kLcCommandMask) >> kLcCommandShift);
    }
  }
};

}  // namespace fake_tb
