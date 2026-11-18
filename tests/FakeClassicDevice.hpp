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

// A fake classic (BR/EDR) HID device behind the fake controller (FakeBluetooth.hpp; its link manager is the controller's ClassicLmp): the L2CAP signalling of basic mode channels,
// in both directions, an SDP server with a HID record and a PnP Information record (answers split by the continuation state, or damaged on purpose), and the HIDP control and
// interrupt channels (input reports, output reports, GET_REPORT and SET_REPORT, HID_CONTROL). Written from the specifications, not from the host's code.
#pragma once
#include <string.h>
#include <vector>
#include "FakeBluetooth.hpp"

namespace fake_bt {

struct ClassicDevice {
  enum class SdpMode { Good, Truncated, Deep, BadContinuation, BadLength, Error, Silent, BadAttributeId };
  struct Channel { uint16_t psm = 0, local = 0, remote = 0; bool configured_in = false, configured_out = false, open = false; };

  Controller& ctl;
  uint16_t handle = 1;
  uint32_t acl_mtu = 27;
  // the SDP records
  Bytes report_map;
  uint16_t vendor = 0x054C, product = 0x09CC;
  bool pnp_record = true;
  uint32_t sdp_chunk = 40;                  // bytes of AttributeLists an answer carries at most (fewer than asked: the continuation state is exercised)
  SdpMode sdp_mode = SdpMode::Good;
  // HIDP behaviour
  bool answer_feature = true, refuse_hid = false;
  Bytes feature = {0x02, 0xAA, 0xBB, 0xCC};   // the feature report GET_REPORT returns (id first)
  // what it saw
  std::vector<Channel> channels;
  std::vector<Bytes> outputs, set_reports, get_reports, sdp_requests;
  std::vector<uint8_t> signals;              // signalling codes received
  uint32_t disconnect_requests = 0;
  uint8_t next_id = 0x60;
  Bytes frame;
  uint32_t needed = 0;

  ClassicDevice(Controller& c, uint16_t h) : ctl(c), handle(h) {}

  // ---- sending
  void send_frame(uint16_t cid, const Bytes& payload) {
    Bytes f = {static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(cid), static_cast<uint8_t>(cid >> 8)};
    f.insert(f.end(), payload.begin(), payload.end());
    for (size_t at = 0; at < f.size(); at += acl_mtu) {
      const size_t n = f.size() - at < acl_mtu ? f.size() - at : acl_mtu;
      ctl.inject_acl(handle, at == 0 ? 2 : 1, Bytes(f.begin() + at, f.begin() + at + n));
    }
  }
  void signal(uint8_t code, uint8_t id, const Bytes& body) {
    Bytes p = {code, id, static_cast<uint8_t>(body.size()), static_cast<uint8_t>(body.size() >> 8)};
    p.insert(p.end(), body.begin(), body.end());
    send_frame(1, p);
  }
  static Bytes le16(uint16_t v) { return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)}; }
  Channel* by_psm(uint16_t psm) { for (Channel& c : channels) if (c.psm == psm && c.open) return &c; return nullptr; }
  Channel* by_local(uint16_t cid) { for (Channel& c : channels) if (c.local == cid) return &c; return nullptr; }
  bool open(uint16_t psm) { return by_psm(psm) != nullptr; }
  void send_on(uint16_t psm, const Bytes& payload) { if (Channel* c = by_psm(psm)) send_frame(c->remote, payload); }

  // The device opens a channel to the host (a reconnecting pad opens control, then interrupt).
  void connect_channel(uint16_t psm) {
    Channel c;
    c.psm = psm;
    c.local = static_cast<uint16_t>(0x80 + channels.size());
    channels.push_back(c);
    Bytes body = le16(psm);
    const Bytes s = le16(c.local);
    body.insert(body.end(), s.begin(), s.end());
    signal(0x02, next_id++, body);
  }
  // The device closes a channel (it is switching off).
  void close_channel(uint16_t psm) {
    Channel* c = by_psm(psm);
    if (!c) return;
    Bytes body = le16(c->remote);
    const Bytes s = le16(c->local);
    body.insert(body.end(), s.begin(), s.end());
    signal(0x06, next_id++, body);
  }
  void input(const Bytes& report) {
    Bytes p = {0xA1};
    p.insert(p.end(), report.begin(), report.end());
    send_on(0x13, p);
  }
  void unplug() { send_on(0x11, {0x15}); }

  // ---- receiving from the host
  void on_acl(const Bytes& packet) {
    if (packet.size() < 4) return;
    const uint16_t first = static_cast<uint16_t>(packet[0] | (packet[1] << 8));
    if ((first & 0x0FFF) != handle) return;
    const uint8_t pb = (first >> 12) & 3;
    const Bytes data(packet.begin() + 4, packet.end());
    if (pb != 1) {
      frame = data;
      needed = data.size() >= 2 ? 4 + (data[0] | (data[1] << 8)) : 4;
    } else if (frame.empty()) {
      return;
    } else {
      frame.insert(frame.end(), data.begin(), data.end());
    }
    if (frame.size() < needed) return;
    const uint16_t cid = static_cast<uint16_t>(frame[2] | (frame[3] << 8));
    const Bytes payload(frame.begin() + 4, frame.end());
    frame.clear();
    if (cid == 1) { signalling(payload); return; }
    Channel* c = by_local(cid);
    if (!c || !c->open) return;
    if (c->psm == 1) sdp(*c, payload);
    else if (c->psm == 0x11) control(*c, payload);
    else if (c->psm == 0x13) interrupt(payload);
  }

  void configure(Channel& c) {
    Bytes body = le16(c.remote);
    body.insert(body.end(), {0, 0, 1, 2, 0xA0, 0x02});                  // flags, MTU 672
    signal(0x04, next_id++, body);
  }
  void signalling(const Bytes& payload) {
    size_t at = 0;
    while (at + 4 <= payload.size()) {
      const uint8_t code = payload[at], id = payload[at + 1];
      const size_t length = payload[at + 2] | (payload[at + 3] << 8);
      if (at + 4 + length > payload.size()) return;
      const Bytes body(payload.begin() + at + 4, payload.begin() + at + 4 + length);
      at += 4 + length;
      signals.push_back(code);
      auto u16 = [&](size_t i) { return static_cast<uint16_t>(body.size() >= i + 2 ? body[i] | (body[i + 1] << 8) : 0); };
      switch (code) {
        case 0x02: {                                                     // the host opens a channel
          const uint16_t psm = u16(0), source = u16(2);
          const bool ok = psm == 1 || ((psm == 0x11 || psm == 0x13) && !refuse_hid);
          Channel c;
          c.psm = psm;
          c.local = static_cast<uint16_t>(0x80 + channels.size());
          c.remote = source;
          Bytes rsp = le16(ok ? c.local : 0);
          const Bytes s = le16(source);
          rsp.insert(rsp.end(), s.begin(), s.end());
          rsp.insert(rsp.end(), {static_cast<uint8_t>(ok ? 0 : 2), 0, 0, 0});
          signal(0x03, id, rsp);
          if (ok) { channels.push_back(c); configure(channels.back()); }
          break;
        }
        case 0x03: {                                                     // the host's answer to ours
          Channel* c = by_local(u16(2));
          if (c && u16(4) == 0) { c->remote = u16(0); configure(*c); }
          break;
        }
        case 0x04: {                                                     // the host configures its side
          Channel* c = by_local(u16(0));
          if (!c) break;
          Bytes rsp = le16(c->remote);
          rsp.insert(rsp.end(), {0, 0, 0, 0});
          signal(0x05, id, rsp);
          c->configured_in = true;
          c->open = c->configured_in && c->configured_out;
          break;
        }
        case 0x05: {
          Channel* c = by_local(u16(0));
          if (!c) break;
          c->configured_out = true;
          c->open = c->configured_in && c->configured_out;
          break;
        }
        case 0x06: {                                                     // the host closes one
          ++disconnect_requests;
          Channel* c = by_local(u16(0));
          if (!c) break;
          Bytes rsp = le16(c->local);
          const Bytes r = le16(c->remote);
          rsp.insert(rsp.end(), r.begin(), r.end());
          signal(0x07, id, rsp);
          c->open = false;
          c->psm = 0;
          break;
        }
        case 0x07: {
          Channel* c = by_local(u16(2));
          if (c) { c->open = false; c->psm = 0; }
          break;
        }
        default:
          break;
      }
    }
  }

  // ---- SDP
  static void element_header(Bytes& out, uint8_t type, size_t length) {
    if (length < 256) { out.push_back(static_cast<uint8_t>(type << 3 | 5)); out.push_back(static_cast<uint8_t>(length)); }
    else { out.push_back(static_cast<uint8_t>(type << 3 | 6)); out.push_back(static_cast<uint8_t>(length >> 8)); out.push_back(static_cast<uint8_t>(length)); }
  }
  static Bytes seq(const Bytes& inner) { Bytes out; element_header(out, 6, inner.size()); out.insert(out.end(), inner.begin(), inner.end()); return out; }
  static Bytes u16e(uint16_t v) { return {0x09, static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)}; }
  static Bytes u8e(uint8_t v) { return {0x08, v}; }
  static Bytes uuid16(uint16_t v) { return {0x19, static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)}; }
  static void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

  Bytes hid_record() const {
    Bytes r;
    append(r, u16e(0x0000)); append(r, {0x0A, 0, 1, 0, 1});                 // ServiceRecordHandle
    append(r, u16e(0x0001)); append(r, seq(uuid16(0x1124)));                // ServiceClassIDList
    append(r, u16e(0x0202)); append(r, u8e(0x08));                          // HIDDeviceSubclass (gamepad)
    Bytes text;
    element_header(text, 4, report_map.size());
    append(text, report_map);
    Bytes entry = u8e(0x22);
    append(entry, text);
    append(r, u16e(0x0206)); append(r, seq(seq(entry)));                    // HIDDescriptorList
    return seq(r);
  }
  Bytes pnp_record_bytes() const {
    Bytes r;
    append(r, u16e(0x0001)); append(r, seq(uuid16(0x1200)));
    append(r, u16e(0x0201)); append(r, u16e(vendor));
    append(r, u16e(0x0202)); append(r, u16e(product));
    return seq(r);
  }
  void sdp(Channel& c, const Bytes& pdu) {
    sdp_requests.push_back(pdu);
    if (pdu.size() < 5 || pdu[0] != 0x06 || sdp_mode == SdpMode::Silent) return;
    const uint8_t t0 = pdu[1], t1 = pdu[2];
    // the pattern's UUID: 35 03 19 hi lo at 5
    const uint16_t uuid = pdu.size() >= 10 ? static_cast<uint16_t>(pdu[8] << 8 | pdu[9]) : 0;
    Bytes lists;
    if (uuid == 0x1124) lists = seq(hid_record());
    else if (uuid == 0x1200 && pnp_record) lists = seq(pnp_record_bytes());
    else lists = seq(Bytes());
    if (sdp_mode == SdpMode::Truncated && uuid == 0x1124) lists[1] = static_cast<uint8_t>(lists[1] + 9);     // (a sequence that claims more than there is)
    if (sdp_mode == SdpMode::Deep && uuid == 0x1124) { Bytes deep = u8e(1); for (int i = 0; i < 20; ++i) deep = seq(deep); lists = seq(deep); }
    if (sdp_mode == SdpMode::BadAttributeId && uuid == 0x1124) { Bytes r = {0x08, 0x06}; append(r, u8e(1)); lists = seq(seq(r)); }
    // the continuation state: the offset reached, two bytes
    size_t offset = 0;
    const size_t cont_at = pdu.size() >= 20 ? 19 : pdu.size();
    if (cont_at < pdu.size() && pdu[cont_at] == 2 && pdu.size() >= cont_at + 3) offset = static_cast<size_t>(pdu[cont_at + 1] << 8 | pdu[cont_at + 2]);
    if (offset > lists.size()) offset = lists.size();
    const size_t n = lists.size() - offset < sdp_chunk ? lists.size() - offset : sdp_chunk;
    Bytes params = {static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
    params.insert(params.end(), lists.begin() + offset, lists.begin() + offset + n);
    const size_t next = offset + n;
    if (next < lists.size()) {
      if (sdp_mode == SdpMode::BadContinuation) { params.push_back(17); params.insert(params.end(), 17, 0); }
      else params.insert(params.end(), {2, static_cast<uint8_t>(next >> 8), static_cast<uint8_t>(next)});
    } else {
      params.push_back(0);
    }
    Bytes rsp = {0x07, t0, t1, static_cast<uint8_t>(params.size() >> 8), static_cast<uint8_t>(params.size())};
    if (sdp_mode == SdpMode::BadLength) rsp[4] = static_cast<uint8_t>(rsp[4] + 3);
    if (sdp_mode == SdpMode::Error) rsp = {0x01, t0, t1, 0, 2, 0, 3};
    else rsp.insert(rsp.end(), params.begin(), params.end());
    send_frame(c.remote, rsp);
  }

  // ---- HIDP
  void control(Channel& c, const Bytes& p) {
    if (p.empty()) return;
    const uint8_t type = p[0] & 0xF0, param = p[0] & 0x0F;
    if (type == 0x40) {                                                  // GET_REPORT
      get_reports.push_back(p);
      if (!answer_feature) return;
      if ((param & 3) != 3 || p.size() < 2 || p[1] != feature[0]) { send_frame(c.remote, {0x02}); return; }    // HANDSHAKE: invalid report id
      Bytes rsp = {0xA3};
      rsp.insert(rsp.end(), feature.begin(), feature.end());
      send_frame(c.remote, rsp);
    } else if (type == 0x50) {                                           // SET_REPORT
      set_reports.push_back(p);
      send_frame(c.remote, {0x00});
    }
  }
  void interrupt(const Bytes& p) {
    if (!p.empty() && p[0] == 0xA2) outputs.push_back(Bytes(p.begin() + 1, p.end()));
  }
};

}  // namespace fake_bt
