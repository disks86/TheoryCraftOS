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

#include "Kernel/Delay.hpp"
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
// Returns the frame (42 bytes; the NIC pads to 60).
ConstByteView build_arp_request(ByteView storage, const hardware::MacAddress& mac, const uint8_t (&sender)[4],
                                const uint8_t (&target)[4]) {
  ByteWriter out(storage);
  out.zeros(0);
  for (int i = 0; i < 6; ++i) out.u8(0xFF);  // destination: broadcast
  mac.write(out);                            // source
  out.be16(0x0806);                          // EtherType: ARP
  out.be16(1);                               // hardware type: Ethernet
  out.be16(0x0800);                          // protocol type: IPv4
  out.u8(6);                                 // address sizes
  out.u8(4);
  out.be16(1);                               // operation: request
  mac.write(out);                            // sender hardware address
  out.bytes(ConstByteView(sender));          // sender protocol address
  out.zeros(6);                              // target hardware address: unknown
  out.bytes(ConstByteView(target));          // target protocol address
  return out.ok() ? ConstByteView(out.written()) : ConstByteView();
}

// Is this frame an ARP reply from `from_ip`? Fills in the sender's hardware address.
bool is_arp_reply_from(ConstByteView frame, const uint8_t (&from_ip)[4], hardware::MacAddress* sender_mac) {
  ByteReader in(frame);
  in.skip(12);  // destination and source
  if (in.be16() != 0x0806) return false;
  in.skip(6);  // hardware type, protocol type, sizes
  if (in.be16() != 2) return false;  // not a reply
  const hardware::MacAddress mac = hardware::MacAddress::read(in);
  uint8_t ip[4];
  in.read_into(ByteView(ip));
  if (!in.ok() || !ConstByteView(ip).equals(ConstByteView(from_ip))) return false;
  *sender_mac = mac;
  return true;
}
}  // namespace

bool run_network_probe(hardware::INetworkInterface& nic, efi::SystemTable* st) {
  console::puts("  ");
  console::puts(nic.name());
  console::puts(", MAC ");
  put_mac(nic.mac_address());
  console::puts(nic.link_up() ? ", link up\n" : ", link DOWN\n");

  uint8_t frame_storage[hardware::kEthernetMaxFrame];
  const ConstByteView frame = build_arp_request(ByteView(frame_storage), nic.mac_address(), kOurIp, kGatewayIp);
  bool answered = false;
  if (!frame.empty() && nic.transmit(frame)) {
    for (int ms = 0; ms < 2000 && !answered; ++ms) {
      uint8_t reply[hardware::kEthernetMaxFrame];
      uint32_t reply_length = 0;
      while (nic.receive(ByteView(reply), &reply_length)) {
        if (reply_length > sizeof(reply)) continue;
        // ARP reply from the gateway.
        hardware::MacAddress gateway;
        if (!is_arp_reply_from(ConstByteView(reply).first(reply_length), kGatewayIp, &gateway))
          continue;  // other traffic (e.g. IPv6 chatter): keep looking
        console::puts("    ARP reply: 10.0.2.2 is at ");
        put_mac(gateway);
        console::puts("\n");
        answered = true;
        break;
      }
      if (!answered) hardware::delay_us(1000);
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
    for (uint32_t batch = 0; batch < 16 && sent < kBurst && nic.transmit(frame); ++batch) ++sent;
    uint8_t reply[hardware::kEthernetMaxFrame];
    uint32_t reply_length = 0;
    hardware::MacAddress ignored;
    while (nic.receive(ByteView(reply), &reply_length))
      if (reply_length <= sizeof(reply) && is_arp_reply_from(ConstByteView(reply).first(reply_length), kGatewayIp, &ignored))
        ++replies;
    hardware::delay_us(1000);
  }
  console::puts("    burst: sent ");
  console::put_dec(sent);
  console::puts(", ARP replies ");
  console::put_dec(replies);
  console::puts("\n");
  return replies == kBurst;
}
