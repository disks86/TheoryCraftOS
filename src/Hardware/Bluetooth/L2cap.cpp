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

#include "Hardware/Bluetooth/L2cap.hpp"

namespace hardware {

namespace {
constexpr uint8_t kCommandReject = 0x01, kConnectionRequest = 0x02, kConnectionResponse = 0x03, kConfigRequest = 0x04, kConfigResponse = 0x05, kDisconnectionRequest = 0x06,
                  kDisconnectionResponse = 0x07, kEchoRequest = 0x08, kEchoResponse = 0x09, kInformationRequest = 0x0A, kInformationResponse = 0x0B,
                  kParameterUpdateRequest = 0x12, kParameterUpdateResponse = 0x13, kLeCreditConnectionRequest = 0x14, kLeCreditConnectionResponse = 0x15;
constexpr uint16_t kResultSuccess = 0, kResultPending = 1, kResultPsmNotSupported = 2, kResultNoResources = 4, kResultInvalidCid = 6;
constexpr uint8_t kOptionMtu = 1;
constexpr uint16_t kDefaultMtu = 672, kMinMtu = 48;
}

L2cap::L2cap(HciController& controller) : controller_(controller) {
  controller_.set_acl_handler(&L2cap::on_acl, this);
  controller_.set_link_handler(&L2cap::on_link, this);
}

L2cap::~L2cap() {
  controller_.set_acl_handler(nullptr, nullptr);
  controller_.set_link_handler(nullptr, nullptr);
}

bool L2cap::register_fixed(uint16_t cid, IL2capFixedHandler* handler) {
  for (Fixed& f : fixed_)
    if (f.handler && f.cid == cid) return false;
  for (Fixed& f : fixed_) {
    if (f.handler) continue;
    f.cid = cid;
    f.handler = handler;
    return true;
  }
  return false;
}

IL2capFixedHandler* L2cap::fixed_for(uint16_t cid) const {
  for (const Fixed& f : fixed_)
    if (f.handler && f.cid == cid) return f.handler;
  return nullptr;
}

bool L2cap::listen(uint16_t psm, IL2capChannelHandler* handler) {
  for (Listener& l : listeners_)
    if (l.handler && l.psm == psm) return false;
  for (Listener& l : listeners_) {
    if (l.handler) continue;
    l.psm = psm;
    l.handler = handler;
    return true;
  }
  return false;
}

HciResult L2cap::send_fixed(uint16_t handle, uint16_t cid, ConstByteView sdu) {
  if (sdu.size() > kMaxFrame - 4) return HciResult::NoRoom;
  uint8_t packet[kMaxFrame];
  ByteWriter out{ByteView(packet)};
  out.le16(static_cast<uint16_t>(sdu.size()));
  out.le16(cid);
  out.bytes(sdu);
  return controller_.send_acl(handle, out.written());
}

// ------------------------------------------------------------------ receiving

L2cap::Assembly* L2cap::assembly_for(uint16_t handle, bool create) {
  Assembly* free_slot = nullptr;
  for (Assembly& a : assemblies_) {
    if (a.used && a.handle == handle) return &a;
    if (!a.used && !free_slot) free_slot = &a;
  }
  if (!create || !free_slot) return nullptr;
  free_slot->used = true;
  free_slot->handle = handle;
  free_slot->have = free_slot->needed = 0;
  free_slot->discard = false;
  return free_slot;
}

void L2cap::on_acl(void* self, const HciLink& link, uint8_t boundary, ConstByteView data) {
  L2cap& l2 = *static_cast<L2cap*>(self);
  Assembly* a = l2.assembly_for(link.handle, true);
  if (!a) { ++l2.dropped_; return; }
  const bool start = boundary == 0x00 || boundary == 0x02;
  if (start) {
    if (a->have != 0 || a->discard) ++l2.dropped_;                      // (the previous frame never finished)
    a->have = 0;
    a->discard = false;
    if (data.size() < 4) { ++l2.dropped_; a->discard = true; return; }
    a->needed = static_cast<uint16_t>(4 + (data[0] | (data[1] << 8)));
    if (a->needed > kMaxFrame) { ++l2.dropped_; a->discard = true; a->have = 0; return; }       // (too big for us: its fragments are skipped)
  } else if (a->discard || a->have == 0) {
    return;                                                              // (a continuation of nothing)
  }
  if (data.size() > static_cast<uint64_t>(a->needed) - a->have) {        // more than the frame holds: damaged
    ++l2.dropped_;
    a->have = 0;
    a->discard = true;
    return;
  }
  for (uint64_t i = 0; i < data.size(); ++i) a->data[a->have + i] = data[i];
  a->have = static_cast<uint16_t>(a->have + data.size());
  if (a->have < a->needed) return;
  const uint16_t cid = static_cast<uint16_t>(a->data[2] | (a->data[3] << 8));
  const uint16_t length = static_cast<uint16_t>(a->needed - 4);
  a->have = 0;
  ++l2.received_;
  uint8_t sdu[kMaxFrame];                                               // (a copy: the handler may send, and the assembly can be reused by what arrives meanwhile)
  for (uint32_t i = 0; i < length; ++i) sdu[i] = a->data[4 + i];
  l2.frame(link, cid, ConstByteView(sdu, length));
}

void L2cap::on_link(void* self, const HciLink& link, bool up) {
  L2cap& l2 = *static_cast<L2cap*>(self);
  if (!up) {
    for (Assembly& a : l2.assemblies_)
      if (a.used && a.handle == link.handle) a.used = false;
    for (L2capChannel& c : l2.channels_)
      if (c.state != L2capState::Closed && c.handle == link.handle) l2.drop_channel(c, true);
  }
  for (Fixed& f : l2.fixed_)
    if (f.handler) f.handler->on_link(link, up);
}

void L2cap::frame(const HciLink& link, uint16_t cid, ConstByteView sdu) {
  if (cid == kCidSignalling && !link.le) { signalling(link, sdu); return; }
  if (cid == kCidLeSignalling && link.le) { signalling(link, sdu); return; }
  if (IL2capFixedHandler* h = fixed_for(cid)) { h->on_data(link, sdu); return; }
  if (cid >= kFirstDynamic) {
    L2capChannel* c = find_by_local(link.handle, cid);
    if (c && c->state == L2capState::Open && c->handler) {
      if (sdu.size() > c->mtu_in) { ++dropped_; return; }
      c->handler->on_data(*c, sdu);
      return;
    }
  }
  ++dropped_;
}

// ------------------------------------------------------------------ signalling

void L2cap::send_signal(const HciLink& link, uint8_t code, uint8_t id, ConstByteView payload) {
  uint8_t pdu[4 + 64];
  if (payload.size() > 64) return;
  ByteWriter out{ByteView(pdu)};
  out.u8(code);
  out.u8(id);
  out.le16(static_cast<uint16_t>(payload.size()));
  out.bytes(payload);
  send_fixed(link.handle, link.le ? kCidLeSignalling : kCidSignalling, out.written());
}

void L2cap::signalling(const HciLink& link, ConstByteView sdu) {
  ByteReader in(sdu);
  while (in.remaining() >= 4) {                                          // (several commands can share a frame on the classic channel)
    const uint8_t code = in.u8();
    const uint8_t id = in.u8();
    const uint16_t length = in.le16();
    const ConstByteView body = in.take(length);
    if (!in.ok()) { ++dropped_; return; }
    command(link, code, id, body);
    if (link.le) return;                                                 // (one command per frame there)
  }
}

L2capChannel* L2cap::find_by_local(uint16_t handle, uint16_t cid) {
  for (L2capChannel& c : channels_)
    if (c.state != L2capState::Closed && c.handle == handle && c.local_cid == cid) return &c;
  return nullptr;
}

L2capChannel* L2cap::allocate(uint16_t handle) {
  for (uint32_t i = 0; i < kMaxChannels; ++i) {
    if (channels_[i].state != L2capState::Closed) continue;
    channels_[i] = L2capChannel();
    channels_[i].handle = handle;
    channels_[i].local_cid = static_cast<uint16_t>(kFirstDynamic + i);
    return &channels_[i];
  }
  return nullptr;
}

void L2cap::drop_channel(L2capChannel& channel, bool notify) {
  const bool was_open = channel.state == L2capState::Open;
  IL2capChannelHandler* handler = channel.handler;
  L2capChannel copy = channel;
  channel = L2capChannel();
  if (notify && handler && (was_open || copy.state != L2capState::Closed)) handler->on_close(copy);
}

void L2cap::finish_open(L2capChannel& channel) {
  if (channel.state == L2capState::Open || !channel.local_configured || !channel.remote_configured) return;
  channel.state = L2capState::Open;
  if (channel.handler) channel.handler->on_open(channel);
}

void L2cap::send_config_request(L2capChannel& channel) {
  uint8_t body[8];
  ByteWriter out{ByteView(body)};
  out.le16(channel.remote_cid);
  out.le16(0);                                                           // no continuation
  out.u8(kOptionMtu);
  out.u8(2);
  out.le16(channel.mtu_in);
  const HciLink* link = controller_.link(channel.handle);
  if (link) send_signal(*link, kConfigRequest, new_id(), out.written());
}

void L2cap::command(const HciLink& link, uint8_t code, uint8_t id, ConstByteView body) {
  ByteReader in(body);
  if (link.le && code != kCommandReject && code != kParameterUpdateRequest && code != kParameterUpdateResponse && code != kLeCreditConnectionRequest && code != kLeCreditConnectionResponse) {
    const uint8_t reject[2] = {0, 0};                                  // (the LE signalling channel knows only these)
    send_signal(link, kCommandReject, id, ConstByteView(reject, 2));
    return;
  }
  switch (code) {
    case kCommandReject:
    case kEchoResponse:
    case kInformationResponse:
    case kParameterUpdateResponse:
      return;
    case kEchoRequest:
      send_signal(link, kEchoResponse, id, body.size() <= 32 ? body : body.first(32));
      return;
    case kInformationRequest: {
      const uint16_t type = in.le16();
      if (!in.ok()) return;
      uint8_t reply[12];
      ByteWriter out{ByteView(reply)};
      out.le16(type);
      if (type == 2) { out.le16(kResultSuccess); out.le32(0); }                          // extended features: none
      else if (type == 3) { out.le16(kResultSuccess); out.le32(0x00000002); out.le32(0); }  // fixed channels: the signalling channel
      else out.le16(1);                                                                  // not supported
      send_signal(link, kInformationResponse, id, out.written());
      return;
    }
    case kParameterUpdateRequest: {
      const uint8_t rejected[2] = {1, 0};                                                // (the link keeps the parameters it has)
      send_signal(link, kParameterUpdateResponse, id, ConstByteView(rejected, 2));
      return;
    }
    case kLeCreditConnectionRequest: {
      const uint8_t refused[10] = {0, 0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(kResultPsmNotSupported), 0};     // (no credit based channels)
      send_signal(link, kLeCreditConnectionResponse, id, ConstByteView(refused, 10));
      return;
    }
    case kConnectionRequest: {
      const uint16_t psm = in.le16();
      const uint16_t source = in.le16();
      if (!in.ok() || link.le) return;
      IL2capChannelHandler* handler = nullptr;
      for (Listener& l : listeners_)
        if (l.handler && l.psm == psm) handler = l.handler;
      uint8_t reply[8];
      ByteWriter out{ByteView(reply)};
      L2capChannel* channel = nullptr;
      uint16_t result = kResultSuccess;
      if (!handler) result = kResultPsmNotSupported;
      else if (source < kFirstDynamic) result = kResultInvalidCid;
      else if (!(channel = allocate(link.handle))) result = kResultNoResources;
      out.le16(channel ? channel->local_cid : 0);
      out.le16(source);
      out.le16(result);
      out.le16(0);
      send_signal(link, kConnectionResponse, id, out.written());
      if (channel) {
        channel->psm = psm;
        channel->remote_cid = source;
        channel->handler = handler;
        channel->state = L2capState::Config;
        send_config_request(*channel);
      }
      return;
    }
    case kConnectionResponse: {
      const uint16_t dcid = in.le16();
      const uint16_t scid = in.le16();
      const uint16_t result = in.le16();
      if (!in.ok()) return;
      L2capChannel* channel = find_by_local(link.handle, scid);
      if (!channel || channel->state != L2capState::Connecting || channel->connect_id != id) return;
      if (result == kResultPending) return;
      if (result != kResultSuccess || dcid < kFirstDynamic) { drop_channel(*channel, true); return; }
      channel->remote_cid = dcid;
      channel->state = L2capState::Config;
      send_config_request(*channel);
      return;
    }
    case kConfigRequest: {
      const uint16_t dcid = in.le16();
      const uint16_t flags = in.le16();
      L2capChannel* channel = in.ok() ? find_by_local(link.handle, dcid) : nullptr;
      uint8_t reply[8];
      ByteWriter out{ByteView(reply)};
      if (!channel || channel->state == L2capState::Connecting) {
        out.le16(0x0002);                                                 // reason: invalid CID
        out.le16(dcid);
        out.le16(0);
        send_signal(link, kCommandReject, id, out.written());
        return;
      }
      uint16_t result = kResultSuccess;
      while (in.remaining() >= 2) {
        const uint8_t type = in.u8();
        const uint8_t length = in.u8();
        const ConstByteView value = in.take(length);
        if (!in.ok()) break;
        if ((type & 0x7F) == kOptionMtu && length == 2) {
          const uint16_t mtu = static_cast<uint16_t>(value[0] | (value[1] << 8));
          channel->mtu_out = mtu < kMinMtu ? kMinMtu : mtu;
        } else if (!(type & 0x80)) {
          result = 3;                                                     // unknown option that is not a hint
        }
      }
      out.le16(channel->remote_cid);
      out.le16(0);
      out.le16(result);
      send_signal(link, kConfigResponse, id, out.written());
      if (result == kResultSuccess && !(flags & 1)) {
        channel->remote_configured = true;
        finish_open(*channel);
      }
      return;
    }
    case kConfigResponse: {
      const uint16_t scid = in.le16();
      in.skip(2);
      const uint16_t result = in.le16();
      L2capChannel* channel = in.ok() ? find_by_local(link.handle, scid) : nullptr;
      if (!channel) return;
      if (result != kResultSuccess) { drop_channel(*channel, true); return; }
      channel->local_configured = true;
      finish_open(*channel);
      return;
    }
    case kDisconnectionRequest: {
      const uint16_t dcid = in.le16();
      const uint16_t scid = in.le16();
      if (!in.ok()) return;
      L2capChannel* channel = find_by_local(link.handle, dcid);
      if (!channel || channel->remote_cid != scid) {
        const uint8_t reject[6] = {0x02, 0, static_cast<uint8_t>(dcid), static_cast<uint8_t>(dcid >> 8), static_cast<uint8_t>(scid), static_cast<uint8_t>(scid >> 8)};
        send_signal(link, kCommandReject, id, ConstByteView(reject, 6));
        return;
      }
      uint8_t reply[4];
      ByteWriter out{ByteView(reply)};
      out.le16(dcid);
      out.le16(scid);
      send_signal(link, kDisconnectionResponse, id, out.written());
      drop_channel(*channel, true);
      return;
    }
    case kDisconnectionResponse: {
      const uint16_t dcid = in.le16();
      const uint16_t scid = in.le16();
      if (!in.ok()) return;
      L2capChannel* channel = find_by_local(link.handle, scid);
      if (channel && channel->remote_cid == dcid) drop_channel(*channel, true);
      return;
    }
    default: {
      const uint8_t reject[2] = {0, 0};                                  // command not understood
      send_signal(link, kCommandReject, id, ConstByteView(reject, 2));
      return;
    }
  }
}

// ------------------------------------------------------------------ dynamic channels

L2capChannel* L2cap::connect(uint16_t handle, uint16_t psm, IL2capChannelHandler* handler) {
  const HciLink* link = controller_.link(handle);
  if (!link || link->le || !handler) return nullptr;
  L2capChannel* channel = allocate(handle);
  if (!channel) return nullptr;
  channel->psm = psm;
  channel->handler = handler;
  channel->we_connected = true;
  channel->state = L2capState::Connecting;
  channel->connect_id = new_id();
  uint8_t body[4];
  ByteWriter out{ByteView(body)};
  out.le16(psm);
  out.le16(channel->local_cid);
  send_signal(*link, kConnectionRequest, channel->connect_id, out.written());
  return channel;
}

HciResult L2cap::send(L2capChannel& channel, ConstByteView sdu) {
  if (channel.state != L2capState::Open) return HciResult::Malformed;
  if (sdu.size() > channel.mtu_out) return HciResult::NoRoom;
  return send_fixed(channel.handle, channel.remote_cid, sdu);
}

void L2cap::close(L2capChannel& channel) {
  if (channel.state == L2capState::Closed) return;
  const HciLink* link = controller_.link(channel.handle);
  if (link && channel.remote_cid != 0) {
    uint8_t body[4];
    ByteWriter out{ByteView(body)};
    out.le16(channel.remote_cid);
    out.le16(channel.local_cid);
    send_signal(*link, kDisconnectionRequest, new_id(), out.written());
  }
  drop_channel(channel, true);
}

}  // namespace hardware
