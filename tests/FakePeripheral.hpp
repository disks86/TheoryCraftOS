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

// A fake Bluetooth peripheral behind the fake controller: it reassembles the L2CAP frames the host sends, serves a small GATT database over the attribute protocol, and is the responding side
// of the security manager (legacy and Secure Connections pairing, key distribution, encryption with the keys it handed out). On the classic side it answers the L2CAP signalling that
// opens channels. Written from the specification with the same crypto functions as the host, so it checks the host's protocol, not the primitives (those are checked on their own).
#pragma once
#include <string.h>
#include <functional>
#include <vector>
#include "FakeBluetooth.hpp"
#include "Hardware/Bluetooth/BtCrypto.hpp"
#include "Hardware/Bluetooth/Gatt.hpp"

namespace fake_bt {

struct Attr {
  uint16_t handle = 0;
  Uuid uuid;
  Bytes value;
  bool encrypted_only = false;      // reads and writes need an encrypted link
  bool writable = false;
  uint16_t group_end = 0;           // service declarations: the last handle of the service
};

struct Peripheral {
  Controller& ctl;
  uint16_t handle;
  bool le;
  uint8_t address_type = 1;
  uint8_t address[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
  uint32_t acl_mtu = 27;             // fragments the peripheral sends in
  std::vector<Attr> db;
  uint16_t server_mtu = 185, mtu = 23;
  bool encrypted = false;
  std::vector<Bytes> log;            // every ATT PDU received
  uint32_t indication_confirms = 0;
  bool malformed_discovery = false;  // answers discovery with damaged PDUs
  bool silent_att = false;

  // reassembly of what the host sends
  Bytes frame;
  uint32_t needed = 0;
  uint32_t dropped_fragments = 0;
  std::vector<std::pair<uint16_t, Bytes>> frames;     // complete L2CAP frames received (cid, payload) - the non-ATT, non-SMP ones too

  Peripheral(Controller& c, uint16_t h, bool is_le) : ctl(c), handle(h), le(is_le) {}

  // ---- sending to the host in ACL fragments
  void send_frame(uint16_t cid, const Bytes& payload) {
    Bytes frame_bytes = {static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(cid), static_cast<uint8_t>(cid >> 8)};
    frame_bytes.insert(frame_bytes.end(), payload.begin(), payload.end());
    for (size_t at = 0; at < frame_bytes.size(); at += acl_mtu) {
      const size_t n = frame_bytes.size() - at < acl_mtu ? frame_bytes.size() - at : acl_mtu;
      ctl.inject_acl(handle, at == 0 ? 2 : 1, Bytes(frame_bytes.begin() + at, frame_bytes.begin() + at + n));
    }
  }
  void att_send(const Bytes& pdu) { if (!silent_att) send_frame(4, pdu); }
  void att_error(uint8_t request, uint16_t at, uint8_t code) { att_send({0x01, request, static_cast<uint8_t>(at), static_cast<uint8_t>(at >> 8), code}); }
  void notify(uint16_t value_handle, const Bytes& value, bool indicate = false) {
    Bytes p = {static_cast<uint8_t>(indicate ? 0x1D : 0x1B), static_cast<uint8_t>(value_handle), static_cast<uint8_t>(value_handle >> 8)};
    p.insert(p.end(), value.begin(), value.end());
    att_send(p);
  }

  // ---- receiving: ACL packets from the host
  void on_acl(const Bytes& packet) {
    if (packet.size() < 4) return;
    const uint16_t first = static_cast<uint16_t>(packet[0] | (packet[1] << 8));
    if ((first & 0x0FFF) != handle) return;
    const uint8_t pb = (first >> 12) & 3;
    Bytes data(packet.begin() + 4, packet.end());
    if (pb != 1) {
      frame = data;
      needed = 4 + (data[0] | (data[1] << 8));
    } else if (frame.empty()) {
      ++dropped_fragments;
      return;
    } else {
      frame.insert(frame.end(), data.begin(), data.end());
    }
    if (frame.size() < needed) return;
    const uint16_t cid = static_cast<uint16_t>(frame[2] | (frame[3] << 8));
    const Bytes payload(frame.begin() + 4, frame.end());
    frame.clear();
    if (cid == 4) att_receive(payload);
    else if (cid == 6) smp_receive(payload);
    else if (cid == 1 || cid == 5) signalling_receive(cid, payload);
    else frames.push_back({cid, payload});
  }

  // ---- GATT
  const Attr* attr(uint16_t h) const { for (const Attr& a : db) if (a.handle == h) return &a; return nullptr; }
  Attr* attr(uint16_t h) { for (Attr& a : db) if (a.handle == h) return &a; return nullptr; }
  static Bytes uuid_bytes(const Uuid& u) { return u.is16() ? Bytes{u.b[12], u.b[13]} : Bytes(u.b, u.b + 16); }

  void add_service(uint16_t first, uint16_t last, const Uuid& uuid) {
    Attr a;
    a.handle = first;
    a.uuid = Uuid::from16(0x2800);
    a.value = uuid_bytes(uuid);
    a.group_end = last;
    db.push_back(a);
  }
  // A characteristic: declaration at `declaration`, the value on the next handle (so `declaration + 1`); returns the value attribute.
  Attr& add_characteristic(uint16_t declaration, uint8_t props, const Uuid& uuid, const Bytes& value, bool encrypted_only = false, bool writable = false) {
    Attr d;
    d.handle = declaration;
    d.uuid = Uuid::from16(0x2803);
    d.value = {props, static_cast<uint8_t>(declaration + 1), static_cast<uint8_t>((declaration + 1) >> 8)};
    const Bytes u = uuid_bytes(uuid);
    d.value.insert(d.value.end(), u.begin(), u.end());
    db.push_back(d);
    Attr v;
    v.handle = static_cast<uint16_t>(declaration + 1);
    v.uuid = uuid;
    v.value = value;
    v.encrypted_only = encrypted_only;
    v.writable = writable;
    db.push_back(v);
    return db.back();
  }
  void add_descriptor(uint16_t h, uint16_t uuid16, const Bytes& value, bool encrypted_only = false) {
    Attr a;
    a.handle = h;
    a.uuid = Uuid::from16(uuid16);
    a.value = value;
    a.encrypted_only = encrypted_only;
    db.push_back(a);
  }
  void add_cccd(uint16_t h) {
    Attr a;
    a.handle = h;
    a.uuid = Uuid::from16(0x2902);
    a.value = {0, 0};
    a.writable = true;
    db.push_back(a);
  }
  static bool in_range(uint16_t h, uint16_t from, uint16_t to) { return h >= from && h <= to; }

  void att_receive(const Bytes& pdu) {
    if (pdu.empty()) return;
    log.push_back(pdu);
    if (silent_att) return;
    const uint8_t op = pdu[0];
    auto u16 = [&](size_t at) { return static_cast<uint16_t>(pdu[at] | (pdu[at + 1] << 8)); };
    const size_t limit = mtu - 1;       // the largest answer, less the opcode byte
    switch (op) {
      case 0x02: {
        if (pdu.size() < 3) return att_error(op, 0, 4);
        mtu = u16(1) < server_mtu ? u16(1) : server_mtu;
        if (mtu < 23) mtu = 23;
        att_send({0x03, static_cast<uint8_t>(server_mtu), static_cast<uint8_t>(server_mtu >> 8)});
        // the MTU in force is the smaller of the two for both sides: as it is the answer that fixes it, the next requests are sized by it
        return;
      }
      case 0x04: {
        if (pdu.size() != 5) return att_error(op, 0, 4);
        const uint16_t from = u16(1), to = u16(3);
        Bytes out = {0x05, 0};
        size_t item = 0;
        for (const Attr& a : db) {
          if (!in_range(a.handle, from, to)) continue;
          const size_t size = a.uuid.is16() ? 2 : 16;
          const uint8_t format = a.uuid.is16() ? 1 : 2;
          if (out[1] == 0) { out[1] = format; item = 2 + size; }
          if (format != out[1] || out.size() + item > mtu) break;
          out.push_back(static_cast<uint8_t>(a.handle));
          out.push_back(static_cast<uint8_t>(a.handle >> 8));
          const Bytes u = uuid_bytes(a.uuid);
          out.insert(out.end(), u.begin(), u.end());
        }
        if (malformed_discovery) { out.resize(out.size() > 4 ? out.size() - 1 : out.size()); out[1] = 7; }
        if (out[1] == 0) return att_error(op, from, 0x0A);
        return att_send(out);
      }
      case 0x08:
      case 0x10: {
        if (pdu.size() != 7 && pdu.size() != 21) return att_error(op, 0, 4);
        const uint16_t from = u16(1), to = u16(3);
        const Uuid want = Uuid::from_bytes(ConstByteView(pdu.data() + 5, pdu.size() - 5));
        Bytes out = {static_cast<uint8_t>(op + 1), 0};
        size_t item = 0;
        for (const Attr& a : db) {
          if (!in_range(a.handle, from, to) || a.uuid != want) continue;
          if (a.encrypted_only && !encrypted) return att_error(op, a.handle, 0x0F);
          Bytes entry = {static_cast<uint8_t>(a.handle), static_cast<uint8_t>(a.handle >> 8)};
          if (op == 0x10) { entry.push_back(static_cast<uint8_t>(a.group_end)); entry.push_back(static_cast<uint8_t>(a.group_end >> 8)); }
          entry.insert(entry.end(), a.value.begin(), a.value.end());
          if (out[1] == 0) { out[1] = static_cast<uint8_t>(entry.size()); item = entry.size(); }
          if (entry.size() != item || out.size() + item > mtu) break;
          out.insert(out.end(), entry.begin(), entry.end());
        }
        if (malformed_discovery && out[1] != 0) out[1] = 3;
        if (out[1] == 0) return att_error(op, from, 0x0A);
        return att_send(out);
      }
      case 0x0A:
      case 0x0C: {
        const size_t want = op == 0x0A ? 3 : 5;
        if (pdu.size() != want) return att_error(op, 0, 4);
        const Attr* a = attr(u16(1));
        if (!a) return att_error(op, u16(1), 1);
        if (a->encrypted_only && !encrypted) return att_error(op, a->handle, 0x0F);
        const size_t offset = op == 0x0C ? u16(3) : 0;
        if (offset > a->value.size()) return att_error(op, a->handle, 7);
        Bytes out = {static_cast<uint8_t>(op + 1)};
        const size_t n = a->value.size() - offset < limit ? a->value.size() - offset : limit;
        out.insert(out.end(), a->value.begin() + offset, a->value.begin() + offset + n);
        return att_send(out);
      }
      case 0x12:
      case 0x52: {
        if (pdu.size() < 3) return att_error(op, 0, 4);
        Attr* a = attr(u16(1));
        if (!a) return op == 0x12 ? att_error(op, u16(1), 1) : void();
        if (!a->writable) return op == 0x12 ? att_error(op, a->handle, 3) : void();
        if (a->encrypted_only && !encrypted) return op == 0x12 ? att_error(op, a->handle, 0x0F) : void();
        a->value = Bytes(pdu.begin() + 3, pdu.end());
        if (op == 0x12) att_send({0x13});
        return;
      }
      case 0x1E:
        ++indication_confirms;
        return;
      default:
        return att_error(op, 0, 6);
    }
  }

  // ---- L2CAP signalling (classic and LE)
  struct ClassicChannel { uint16_t local = 0, remote = 0, psm = 0; bool open = false; };
  std::vector<ClassicChannel> channels;
  std::vector<Bytes> signal_responses;       // what the host answered to our requests
  bool accept_psm_0x11 = true;
  Bytes echoed;
  void signal(uint8_t code, uint8_t id, const Bytes& payload) {
    Bytes p = {code, id, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
    p.insert(p.end(), payload.begin(), payload.end());
    send_frame(le ? 5 : 1, p);
  }
  // The peripheral opens a channel to the host (its own channel identifier is `local`).
  void open_to_host(uint16_t psm, uint16_t local) {
    ClassicChannel c;
    c.local = local;
    c.psm = psm;
    channels.push_back(c);
    signal(0x02, 0x41, Bytes{static_cast<uint8_t>(psm), static_cast<uint8_t>(psm >> 8), static_cast<uint8_t>(local), static_cast<uint8_t>(local >> 8)});
  }
  void signalling_receive(uint16_t, const Bytes& payload) {
    size_t at = 0;
    while (at + 4 <= payload.size()) {
      const uint8_t code = payload[at], id = payload[at + 1];
      const size_t len = payload[at + 2] | (payload[at + 3] << 8);
      if (at + 4 + len > payload.size()) return;
      const Bytes body(payload.begin() + at + 4, payload.begin() + at + 4 + len);
      at += 4 + len;
      signal_command(code, id, body);
    }
  }
  void signal_command(uint8_t code, uint8_t id, const Bytes& body) {
    auto u16 = [&](size_t i) { return static_cast<uint16_t>(body[i] | (body[i + 1] << 8)); };
    switch (code) {
      case 0x03: {                     // connection response to our request
        signal_responses.push_back(Bytes{code, id});
        if (body.size() >= 8 && u16(4) == 0) {
          for (ClassicChannel& c : channels) if (c.local == u16(2)) c.remote = u16(0);
          for (ClassicChannel& c : channels) if (c.local == u16(2)) {
            Bytes cfg = {static_cast<uint8_t>(c.remote), static_cast<uint8_t>(c.remote >> 8), 0, 0, 1, 2, 0xA0, 0x02};
            signal(0x04, 0x21, cfg);
          }
        }
        return;
      }
      case 0x02: {                     // the host opens a channel to us
        const uint16_t psm = u16(0), source = u16(2);
        ClassicChannel c;
        c.local = static_cast<uint16_t>(0x70 + channels.size());
        c.remote = source;
        c.psm = psm;
        const bool ok = psm == 0x11 && accept_psm_0x11;
        Bytes rsp = {static_cast<uint8_t>(ok ? c.local : 0), static_cast<uint8_t>(ok ? c.local >> 8 : 0), static_cast<uint8_t>(source), static_cast<uint8_t>(source >> 8), static_cast<uint8_t>(ok ? 0 : 2), 0, 0, 0};
        signal(0x03, id, rsp);
        if (ok) {
          channels.push_back(c);
          Bytes cfg = {static_cast<uint8_t>(source), static_cast<uint8_t>(source >> 8), 0, 0, 1, 2, 0xA0, 0x02};      // MTU 672
          signal(0x04, 0x31, cfg);
        }
        return;
      }
      case 0x04: {                     // configuration request: accept
        for (ClassicChannel& c : channels) {
          if (c.local != u16(0)) continue;
          Bytes rsp = {static_cast<uint8_t>(c.remote), static_cast<uint8_t>(c.remote >> 8), 0, 0, 0, 0};
          signal(0x05, id, rsp);
          c.open = true;
        }
        return;
      }
      case 0x05:
        signal_responses.push_back(Bytes{code, id});
        return;
      case 0x06: {                     // disconnection request
        Bytes rsp(body.begin(), body.end());
        signal(0x07, id, rsp);
        for (size_t i = 0; i < channels.size(); ++i) if (channels[i].local == u16(0)) { channels.erase(channels.begin() + i); break; }
        return;
      }
      case 0x07:
        signal_responses.push_back(Bytes{code, id});
        return;
      case 0x09:
        echoed = body;
        signal_responses.push_back(Bytes{code, id});
        return;
      default:
        signal_responses.push_back(Bytes{code, id});
        return;
    }
  }

  // ---- the security manager, responding
  uint8_t io_capability = 3;          // 0 display only, 1 display and yes/no, 2 keyboard only, 3 none, 4 keyboard and display
  bool want_sc = true, want_mitm = false;
  uint8_t max_key = 16, oob = 0;
  uint8_t resp_dist = 0x03;
  uint32_t passkey = 0;               // the number shown to the user (set by the test from what the host displays) or to type (set by the test for the host to ask for)
  int method = 0;                     // 0 just works, 1 numeric comparison, 2 the peripheral displays, 3 the peripheral inputs (the test says which it expects)
  uint32_t numeric_value = 0;         // what the peripheral computed for numeric comparison
  bool reject_pairing = false;
  // state
  uint8_t preq[7] = {}, pres[7] = {};
  bool sc = false;
  uint8_t tk[16] = {}, srand_[16] = {}, mrand[16] = {}, mconfirm[16] = {};
  uint8_t ltk[16] = {}, stk[16] = {}, mackey[16] = {}, dhkey[32] = {}, nb[16] = {}, na[16] = {};
  uint8_t central_addr[7] = {}, own_addr[7] = {};
  bt::SmKeyPair keys;
  uint8_t central_public[64] = {};
  uint32_t round = 0;
  uint8_t keysize = 16;
  uint8_t stored_ltk[16] = {};
  uint8_t stored_rand[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint16_t stored_ediv = 0x1234;
  uint8_t irk[16] = {0x9b, 0x7d, 0x39, 0x0a, 0xa6, 0x10, 0x10, 0x34, 0x05, 0xad, 0xc8, 0x57, 0xa3, 0x34, 0x02, 0xec};
  bool have_bond = false, legacy_encryption_pending = false;
  uint32_t pairings_completed = 0;
  uint8_t failed_with = 0;
  crypto::IRandom* random = nullptr;
  uint32_t distribution_sent = 0;
  std::vector<Bytes> smp_log;

  void smp_send(uint8_t code, const Bytes& body) { Bytes p = {code}; p.insert(p.end(), body.begin(), body.end()); send_frame(6, p); }
  void smp_fail(uint8_t reason) { failed_with = reason; smp_send(0x05, {reason}); }
  void rnd(uint8_t out[16]) { random->fill(ByteView(out, 16)); }
  static void passkey16(uint8_t out[16], uint32_t v) { memset(out, 0, 16); for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(v >> (8 * i)); }
  // the peripheral is the responder; the central is the initiator
  void smp_receive(const Bytes& pdu) {
    smp_log.push_back(pdu);
    if (pdu.empty()) return;
    const uint8_t code = pdu[0];
    const Bytes body(pdu.begin() + 1, pdu.end());
    using namespace bt;
    switch (code) {
      case 0x01: {                       // pairing request
        if (pdu.size() != 7 || reject_pairing) return smp_fail(5);
        memcpy(preq, pdu.data(), 7);
        sc = want_sc && (pdu[3] & 0x08);
        const uint8_t auth = static_cast<uint8_t>(0x01 | (want_mitm ? 4 : 0) | (want_sc ? 8 : 0));
        keysize = pdu[4] < max_key ? pdu[4] : max_key;
        const uint8_t dist = static_cast<uint8_t>(pdu[6] & resp_dist);
        const uint8_t response[7] = {0x02, io_capability, oob, auth, max_key, 0, dist};
        memcpy(pres, response, 7);
        smp_send(0x02, Bytes(response + 1, response + 7));
        memcpy(central_addr, ctl.address, 6);   // (the host's own address, which the fake controller reports)
        central_addr[6] = 0;
        memcpy(own_addr, address, 6);
        own_addr[6] = address_type;
        round = 0;
        return;
      }
      case 0x03: {                       // confirm (legacy: Mconfirm; secure connections passkey: Cai)
        if (body.size() != 16) return smp_fail(10);
        if (!sc) {
          memcpy(mconfirm, body.data(), 16);
          memset(tk, 0, 16);
          if (method >= 2) passkey16(tk, passkey);                  // (known by now: shown to the user, or typed)
          rnd(srand_);
          uint8_t c[16];
          sm_c1(tk, srand_, preq, pres, 0, central_addr, address_type, address, c);
          return smp_send(0x03, Bytes(c, c + 16));
        }
        memcpy(mconfirm, body.data(), 16);
        rnd(nb);
        uint8_t z = 0;
        if (method >= 2) z = static_cast<uint8_t>(0x80 | ((passkey >> round) & 1));
        uint8_t c[16];
        sm_f4(keys.public_key, central_public, nb, z, c);
        return smp_send(0x03, Bytes(c, c + 16));
      }
      case 0x04: {                       // random
        if (body.size() != 16) return smp_fail(10);
        if (!sc) {
          memcpy(mrand, body.data(), 16);
          uint8_t c[16];
          sm_c1(tk, mrand, preq, pres, 0, central_addr, address_type, address, c);
          if (memcmp(c, mconfirm, 16) != 0) return smp_fail(4);
          smp_send(0x04, Bytes(srand_, srand_ + 16));
          sm_s1(tk, srand_, mrand, stk);
          for (uint32_t i = keysize; i < 16; ++i) stk[i] = 0;
          memcpy(ltk, stk, 16);
          legacy_encryption_pending = true;
          return;
        }
        memcpy(na, body.data(), 16);
        uint8_t z = 0;
        if (method >= 2) z = static_cast<uint8_t>(0x80 | ((passkey >> round) & 1));
        if (method >= 2) {                // (just works and numeric comparison: the initiator made no commitment)
          uint8_t c[16];
          sm_f4(central_public, keys.public_key, na, z, c);
          if (memcmp(c, mconfirm, 16) != 0) return smp_fail(1);
        }
        smp_send(0x04, Bytes(nb, nb + 16));
        if (method >= 2) ++round;
        if (method == 1) sm_g2(central_public, keys.public_key, na, nb, &numeric_value);
        return;
      }
      case 0x0C: {                       // the central's public key
        if (body.size() != 64 || !sc) return smp_fail(10);
        memcpy(central_public, body.data(), 64);
        sm_generate_key_pair(*random, &keys);
        smp_send(0x0C, Bytes(keys.public_key, keys.public_key + 64));
        if (!sm_dhkey(keys.private_key, central_public, dhkey)) return smp_fail(10);
        if (method < 2) {                // just works / numeric: the confirm comes at once
          rnd(nb);
          uint8_t c[16];
          sm_f4(keys.public_key, central_public, nb, 0, c);
          smp_send(0x03, Bytes(c, c + 16));
        }
        return;
      }
      case 0x0D: {                       // the central's DHKey check
        if (body.size() != 16) return smp_fail(10);
        sm_f5(dhkey, na, nb, central_addr, own_addr, mackey, ltk);
        uint8_t cap_a[3] = {preq[1], preq[2], preq[3]}, cap_b[3] = {pres[1], pres[2], pres[3]}, r[16] = {}, ea[16], eb[16];
        if (method >= 2) passkey16(r, passkey);
        sm_f6(mackey, na, nb, r, cap_a, central_addr, own_addr, ea);
        if (memcmp(ea, body.data(), 16) != 0) return smp_fail(0x0B);
        sm_f6(mackey, nb, na, r, cap_b, own_addr, central_addr, eb);
        smp_send(0x0D, Bytes(eb, eb + 16));
        legacy_encryption_pending = true;
        return;
      }
      case 0x05:
        failed_with = body.empty() ? 8 : body[0];
        return;
      default:
        return;
    }
  }
  // the host asked the controller to encrypt: is it the key we hold?
  bool encryption_request(const uint8_t rand_in[8], uint16_t ediv_in, const uint8_t key[16]) {
    bool ok = false;
    if (legacy_encryption_pending) {
      uint8_t zero[8] = {};
      ok = ediv_in == 0 && memcmp(rand_in, zero, 8) == 0 && memcmp(key, ltk, 16) == 0;
    } else if (have_bond) {
      ok = ediv_in == stored_ediv && memcmp(rand_in, stored_rand, 8) == 0 && memcmp(key, stored_ltk, 16) == 0;
    }
    if (ok) encrypted = true;
    return ok;
  }
  // called after the encryption change event went out: hand over the keys
  void after_encryption() {
    if (!legacy_encryption_pending) return;
    legacy_encryption_pending = false;
    const uint8_t dist = static_cast<uint8_t>(pres[6]);
    if (!sc) {
      random->fill(ByteView(stored_ltk, 16));
      for (uint32_t i = keysize; i < 16; ++i) stored_ltk[i] = 0;
      if (dist & 1) {
        smp_send(0x06, Bytes(stored_ltk, stored_ltk + 16));
        Bytes ci = {static_cast<uint8_t>(stored_ediv), static_cast<uint8_t>(stored_ediv >> 8)};
        ci.insert(ci.end(), stored_rand, stored_rand + 8);
        smp_send(0x07, ci);
      }
    } else {
      memcpy(stored_ltk, ltk, 16);
      stored_ediv = 0;
      memset(stored_rand, 0, 8);
    }
    if (dist & 2) {
      smp_send(0x08, Bytes(irk, irk + 16));
      Bytes ia = {address_type};
      ia.insert(ia.end(), address, address + 6);
      smp_send(0x09, ia);
    }
    have_bond = true;
    ++pairings_completed;
  }
};

}  // namespace fake_bt
