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

#include "NetworkDemo.hpp"
#include "console.hpp"

namespace {
constexpr uint8_t kOurIp[4] = {10, 0, 2, 15};
constexpr uint8_t kGatewayIp[4] = {10, 0, 2, 2};

void put_mac(const hardware::MacAddress& mac) {
  const char* digits = "0123456789abcdef";
  for (int i = 0; i < 6; ++i) {
    if (i) console::puts(":");
    const char text[3] = {digits[mac.bytes[i] >> 4], digits[mac.bytes[i] & 15], 0};
    console::puts(text);
  }
}

// An Ethernet frame carrying an ARP request: who has `target`? tell `sender`.
uint32_t build_arp_request(uint8_t* frame, const hardware::MacAddress& mac, const uint8_t* sender,
                           const uint8_t* target) {
  uint8_t* p = frame;
  for (int i = 0; i < 6; ++i) *p++ = 0xFF;                 // destination: broadcast
  for (int i = 0; i < 6; ++i) *p++ = mac.bytes[i];         // source
  *p++ = 0x08; *p++ = 0x06;                                // EtherType: ARP
  *p++ = 0x00; *p++ = 0x01;                                // hardware type: Ethernet
  *p++ = 0x08; *p++ = 0x00;                                // protocol type: IPv4
  *p++ = 6; *p++ = 4;                                      // address sizes
  *p++ = 0x00; *p++ = 0x01;                                // operation: request
  for (int i = 0; i < 6; ++i) *p++ = mac.bytes[i];         // sender hardware address
  for (int i = 0; i < 4; ++i) *p++ = sender[i];            // sender protocol address
  for (int i = 0; i < 6; ++i) *p++ = 0;                    // target hardware address: unknown
  for (int i = 0; i < 4; ++i) *p++ = target[i];            // target protocol address
  return static_cast<uint32_t>(p - frame);                 // 42 bytes; the NIC pads to 60
}
}  // namespace

bool run_network_probe(hardware::INetworkInterface& nic, efi::SystemTable* st) {
  console::puts("  ");
  console::puts(nic.name());
  console::puts(", MAC ");
  put_mac(nic.mac_address());
  console::puts(nic.link_up() ? ", link up\n" : ", link DOWN\n");

  uint8_t frame[hardware::kEthernetMaxFrame];
  const uint32_t length = build_arp_request(frame, nic.mac_address(), kOurIp, kGatewayIp);
  bool answered = false;
  if (nic.transmit(frame, length)) {
    for (int ms = 0; ms < 2000 && !answered; ++ms) {
      uint8_t reply[hardware::kEthernetMaxFrame];
      uint32_t reply_length = 0;
      while (nic.receive(reply, sizeof(reply), &reply_length)) {
        // ARP reply from the gateway: EtherType 0x0806, operation 2, sender IP 10.0.2.2.
        const bool arp_reply = reply_length >= 42 && reply[12] == 0x08 && reply[13] == 0x06 && reply[20] == 0x00 &&
                               reply[21] == 0x02 && reply[28] == kGatewayIp[0] && reply[29] == kGatewayIp[1] &&
                               reply[30] == kGatewayIp[2] && reply[31] == kGatewayIp[3];
        if (!arp_reply) continue;  // other traffic (e.g. IPv6 chatter): keep looking
        hardware::MacAddress gateway;
        for (int i = 0; i < 6; ++i) gateway.bytes[i] = reply[22 + i];
        console::puts("    ARP reply: 10.0.2.2 is at ");
        put_mac(gateway);
        console::puts("\n");
        answered = true;
        break;
      }
      if (!answered) st->boot_services->stall(1000);
    }
  } else {
    console::puts("    could not send\n");
  }
  if (!answered) {
    console::puts("    no ARP reply\n");
    return false;
  }

  // Burst: many requests wrap every descriptor ring several times. Replies come
  // back immediately, and a NIC drops frames it has no receive buffer for, so
  // send in small batches and drain between them.
  constexpr uint32_t kBurst = 300;
  uint32_t sent = 0, replies = 0;
  for (int ms = 0; ms < 3000 && replies < kBurst; ++ms) {
    for (uint32_t batch = 0; batch < 16 && sent < kBurst && nic.transmit(frame, length); ++batch) ++sent;
    uint8_t reply[hardware::kEthernetMaxFrame];
    uint32_t reply_length = 0;
    while (nic.receive(reply, sizeof(reply), &reply_length))
      if (reply_length >= 42 && reply[12] == 0x08 && reply[13] == 0x06 && reply[21] == 0x02) ++replies;
    st->boot_services->stall(1000);
  }
  console::puts("    burst: sent ");
  console::put_dec(sent);
  console::puts(", ARP replies ");
  console::put_dec(replies);
  console::puts("\n");
  return replies == kBurst;
}
