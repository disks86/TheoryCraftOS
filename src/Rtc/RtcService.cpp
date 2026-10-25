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

#include "Rtc/RtcService.hpp"
#include "Utility/Move.hpp"

namespace rtc {

namespace {

class Locked {
 public:
  explicit Locked(kernel::RecursiveMutex& m) : m_(m) { m_.lock(); }
  ~Locked() { m_.unlock(); }

 private:
  kernel::RecursiveMutex& m_;
};

void jq(String* out, StringView text) {
  out->push_back('"');
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(c);
    } else if (c == '\n') {
      out->append("\\n");
    } else if (c == '\r') {
      out->append("\\r");
    } else if (c == '\t') {
      out->append("\\t");
    } else if (static_cast<uint8_t>(c) < 0x20) {
      static const char kHex[] = "0123456789abcdef";
      out->append("\\u00");
      out->push_back(kHex[(static_cast<uint8_t>(c) >> 4) & 15]);
      out->push_back(kHex[static_cast<uint8_t>(c) & 15]);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

}  // namespace

RtcService::RtcService(allocators::IAllocator& allocator, IRtcNetwork& network, crypto::IRandom& random)
    : allocator_(allocator), net_(network), random_(random), identity_(allocator), slots_(allocator), sources_(allocator) {}

uint32_t RtcService::create(StringView ice_servers, String* error) {
  Locked lock(mutex_);
  if (slots_.count() >= kMaxConnections) {
    error->assign("OperationError: Too many peer connections");
    return 0;
  }
  if (!identity_ready_) identity_ready_ = identity_.generate(random_, "WebRTC");
  if (!identity_ready_) {
    error->assign("OperationError: The certificate could not be made");
    return 0;
  }
  UniquePointer<Slot> s = UniquePointer<Slot>::make(allocator_);
  if (!s) {
    error->assign("OperationError: Out of memory");
    return 0;
  }
  s->pc = UniquePointer<PeerConnection>::make(allocator_, allocator_, net_, random_, identity_);
  if (!s->pc) {
    error->assign("OperationError: Out of memory");
    return 0;
  }
  StringView rest = ice_servers, line;
  while (rest.next_token('\n', &line)) s->pc->add_stun_server(line.trimmed());
  s->pc->set_devices(devices_);
  s->id = next_id_++;
  const uint32_t id = s->id;
  slots_.push_back(util::move(s));
  return id;
}

void RtcService::release(uint32_t id) {
  Locked lock(mutex_);
  for (uint64_t i = 0; i < slots_.count(); ++i)
    if (slots_[i]->id == id) {
      slots_[i]->pc->close();
      slots_.remove_at(i);
      return;
    }
}

void RtcService::set_devices(IRtcAudioDevices* devices) {
  Locked lock(mutex_);
  devices_ = devices;
  for (const UniquePointer<Slot>& s : slots_) s->pc->set_devices(devices);
}

// Operations that belong to no connection (id 0): the microphone. `origin` is the page's, put there by the page.
bool RtcService::service_op(StringView name, StringView origin, int64_t n1, String* out) {
  auto state = [&](int v) { out->assign(v > 0 ? "granted" : v == 0 ? "denied" : "default"); };
  if (name.equals("mic_present")) {
    out->assign(devices_ && devices_->microphone_present() ? "1" : "0");
    return true;
  }
  if (!devices_ || !devices_->microphone_present()) {
    out->assign("NotFoundError: Requested device not found");
    return false;
  }
  if (name.equals("mic_permission")) {
    state(devices_->microphone_permission(origin));
    return true;
  }
  if (name.equals("mic_request")) {
    state(devices_->microphone_request(origin));
    return true;
  }
  if (name.equals("mic_open")) {
    if (devices_->microphone_permission(origin) <= 0) {
      out->assign("NotAllowedError: Permission denied");
      return false;
    }
    if (sources_.count() >= 8) {
      out->assign("NotReadableError: Too many microphones are open");
      return false;
    }
    UniquePointer<Source> s = UniquePointer<Source>::make(allocator_);
    if (!s) {
      out->assign("NotReadableError: Out of memory");
      return false;
    }
    s->source = devices_->open_microphone();
    if (!s->source) {
      out->assign("NotReadableError: The microphone could not be opened");
      return false;
    }
    s->handle = next_source_++;
    out->append_uint(s->handle);
    sources_.push_back(util::move(s));
    return true;
  }
  if (name.equals("source_close")) {
    for (uint64_t i = 0; i < sources_.count(); ++i)
      if (sources_[i]->handle == static_cast<uint32_t>(n1)) {
        for (const UniquePointer<Slot>& slot : slots_) slot->pc->drop_source(sources_[i]->source.get());
        sources_.remove_at(i);
        break;
      }
    return true;
  }
  out->assign("NotSupportedError: Unknown operation");
  return false;
}

bool RtcService::op(uint32_t id, StringView name, StringView a, StringView b, ConstByteView bytes, int64_t n1, int64_t n2, String* out) {
  Locked lock(mutex_);
  if (id == 0) {
    out->clear();
    return service_op(name, b, n1, out);
  }
  PeerConnection* pc = nullptr;
  for (const UniquePointer<Slot>& s : slots_)
    if (s->id == id) pc = s->pc.get();
  out->clear();
  if (!pc) {
    out->assign("InvalidStateError: The connection is gone");
    return false;
  }
  String error(allocator_);
  auto fail = [&]() {
    out->assign(error.empty() ? StringView("OperationError: The operation failed") : error.view());
    return false;
  };
  if (name.equals("offer")) return pc->create_offer(out, &error) || fail();
  if (name.equals("answer")) return pc->create_answer(out, &error) || fail();
  if (name.equals("setlocal")) return pc->set_local_description(a, b, &error) || fail();
  if (name.equals("setremote")) return pc->set_remote_description(a, b, &error) || fail();
  if (name.equals("addcandidate")) return pc->add_ice_candidate(a, b, static_cast<int32_t>(n1), &error) || fail();
  if (name.equals("localdesc") || name.equals("remotedesc")) {
    String type(allocator_), sdp(allocator_);
    if (!(name.equals("localdesc") ? pc->local_description(&type, &sdp) : pc->remote_description(&type, &sdp))) return true;   // (none: empty)
    out->append("{\"type\":");
    jq(out, type.view());
    out->append(",\"sdp\":");
    jq(out, sdp.view());
    out->append("}");
    return true;
  }
  if (name.equals("state")) {
    static const char* const kSignaling[] = {"stable", "have-local-offer", "have-remote-offer", "have-local-pranswer", "have-remote-pranswer", "closed"};
    static const char* const kConnection[] = {"new", "connecting", "connected", "disconnected", "failed", "closed"};
    static const char* const kIce[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    out->append("{\"signaling\":\"");
    out->append(kSignaling[static_cast<uint32_t>(pc->signaling_state())]);
    out->append("\",\"connection\":\"");
    out->append(kConnection[static_cast<uint32_t>(pc->connection_state())]);
    out->append("\",\"ice\":\"");
    out->append(kIce[static_cast<uint32_t>(pc->ice_state())]);
    out->append("\",\"gathering\":\"");
    out->append(pc->gathering_state() == IceGathering::Complete ? "complete" : pc->gathering_state() == IceGathering::Gathering ? "gathering" : "new");
    out->append("\"}");
    return true;
  }
  if (name.equals("events")) {   // drives the connection, then gives what happened: a JSON array
    pc->poll();
    String e(allocator_);
    out->append("[");
    for (uint32_t i = 0; i < 256 && pc->take_event(&e); ++i) {
      if (i) out->push_back(',');
      out->append(e.view());
    }
    out->append("]");
    return true;
  }
  if (name.equals("createchannel")) {   // a=label b=protocol n1=flags (1 ordered, 2 negotiated) | (maxRetransmits+1) << 8 | (maxPacketLifeTime+1) << 24, n2=id (-1: none)
    const bool ordered = n1 & 1, negotiated = n1 & 2;
    const int32_t rtx = static_cast<int32_t>((n1 >> 8) & 0xFFFF) - 1, life = static_cast<int32_t>((n1 >> 24) & 0xFFFF) - 1;
    const uint32_t handle = pc->create_channel(a, b, ordered, rtx, life, negotiated, static_cast<int32_t>(n2), &error);
    if (handle == 0) return fail();
    out->append_uint(handle);
    return true;
  }
  if (name.equals("send")) return pc->channel_send(static_cast<uint32_t>(n1), n2 != 0, bytes, &error) || fail();
  if (name.equals("closechannel")) {
    pc->channel_close(static_cast<uint32_t>(n1));
    return true;
  }
  if (name.equals("buffered")) {
    out->append_uint(pc->channel_buffered(static_cast<uint32_t>(n1)));
    return true;
  }
  if (name.equals("threshold")) {
    pc->channel_threshold(static_cast<uint32_t>(n1), static_cast<uint64_t>(n2));
    return true;
  }
  if (name.equals("channelinfo")) {
    pc->channel_info(static_cast<uint32_t>(n1), out);
    return true;
  }
  if (name.equals("stats")) {
    pc->stats(out);
    return true;
  }
  if (name.equals("maxmessage")) {
    out->append_uint(pc->max_message_size());
    return true;
  }
  if (name.equals("restartice")) {
    pc->restart_ice();
    return true;
  }
  if (name.equals("addtransceiver")) {
    const int32_t index = pc->add_transceiver(a, static_cast<int>(n1), &error);
    if (index < 0) return fail();
    out->append_uint(static_cast<uint32_t>(index));
    return true;
  }
  if (name.equals("setsender") || name.equals("addtrack")) {   // n2: the handle of the source (0: none), a: the track's id, b: the stream's
    IRtcAudioSource* source = nullptr;
    for (const UniquePointer<Source>& src : sources_)
      if (src->handle == static_cast<uint32_t>(n2)) source = src->source.get();
    if (name.equals("addtrack")) {
      const int32_t index = pc->add_track(source, a, b, &error);
      if (index < 0) return fail();
      out->append_uint(static_cast<uint32_t>(index));
      return true;
    }
    return pc->set_sender(static_cast<uint32_t>(n1), source, a, b, &error) || fail();
  }
  if (name.equals("removesender")) return pc->remove_sender(static_cast<uint32_t>(n1), &error) || fail();
  if (name.equals("setdirection")) return pc->set_direction(static_cast<uint32_t>(n1), static_cast<int>(n2), &error) || fail();
  if (name.equals("stoptransceiver")) return pc->stop_transceiver(static_cast<uint32_t>(n1), &error) || fail();
  if (name.equals("senderenabled")) {
    pc->set_sender_enabled(static_cast<uint32_t>(n1), n2 != 0);
    return true;
  }
  if (name.equals("play")) {
    pc->set_receiver_playing(static_cast<uint32_t>(n1), n2 != 0);
    return true;
  }
  if (name.equals("transceivers")) {
    String info(allocator_);
    out->append("[");
    bool first = true;
    for (uint32_t i = 0; i < pc->transceiver_count(); ++i)
      if (pc->transceiver_info(i, &info)) {
        if (!first) out->push_back(',');
        first = false;
        out->append(info.view());
      }
    out->append("]");
    return true;
  }
  if (name.equals("close")) {
    pc->close();
    return true;
  }
  out->assign("NotSupportedError: Unknown operation");
  return false;
}

}  // namespace rtc
