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

#include "Rtc/PeerConnection.hpp"
#include "Rtc/G711.hpp"
#include "Rtc/Rtp.hpp"
#include "Utility/Move.hpp"

namespace rtc {

namespace {

uint64_t strlen_of(const char* s) {
  uint64_t n = 0;
  while (s[n]) ++n;
  return n;
}

constexpr uint32_t kPpidDcep = 50, kPpidString = 51, kPpidBinary = 53, kPpidStringEmpty = 56, kPpidBinaryEmpty = 57;
constexpr uint8_t kDcepOpen = 3, kDcepAck = 2;

void jq(String* out, StringView text) {
  out->push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out->append("\\u00");
          out->push_back(kHex[(static_cast<uint8_t>(c) >> 4) & 15]);
          out->push_back(kHex[static_cast<uint8_t>(c) & 15]);
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

void hex(String* out, ConstByteView bytes) {
  static const char kHex[] = "0123456789abcdef";
  for (const uint8_t b : bytes) {
    out->push_back(kHex[b >> 4]);
    out->push_back(kHex[b & 15]);
  }
}

void ip_text(network::Ipv4Address a, String* out) {
  for (int i = 0; i < 4; ++i) {
    if (i) out->push_back('.');
    out->append_uint(a.octets[i]);
  }
}

bool parse_ip(StringView text, network::Ipv4Address* out) {
  StringView rest = text, part;
  for (int i = 0; i < 4; ++i) {
    uint64_t n = 0;
    if (!rest.next_token('.', &part) || part.empty() || part.size() > 3 || !part.parse_uint(&n) || n > 255) return false;
    out->octets[i] = static_cast<uint8_t>(n);
  }
  return rest.empty();
}

const char* kDirections[] = {"sendrecv", "sendonly", "recvonly", "inactive"};

int direction_of(StringView text) {
  for (int i = 0; i < 4; ++i)
    if (text.equals(kDirections[i])) return i;
  return -1;
}
bool sends(int d) { return d == 0 || d == 1; }
bool receives(int d) { return d == 0 || d == 2; }
int make_direction(bool send, bool receive) { return send && receive ? 0 : send ? 1 : receive ? 2 : 3; }

// What a transceiver ends up doing: we send if we want to and the other side receives, and the same the other way round.
int resolve_direction(int wanted, int remote) { return make_direction(sends(wanted) && receives(remote), receives(wanted) && sends(remote)); }

const char* ice_name(IceState s) {
  switch (s) {
    case IceState::New: return "new";
    case IceState::Checking: return "checking";
    case IceState::Connected: return "connected";
    case IceState::Completed: return "completed";
    case IceState::Failed: return "failed";
    case IceState::Disconnected: return "disconnected";
    default: return "closed";
  }
}

const char* connection_name(PeerConnection::Connection s) {
  switch (s) {
    case PeerConnection::Connection::New: return "new";
    case PeerConnection::Connection::Connecting: return "connecting";
    case PeerConnection::Connection::Connected: return "connected";
    case PeerConnection::Connection::Disconnected: return "disconnected";
    case PeerConnection::Connection::Failed: return "failed";
    default: return "closed";
  }
}

const char* signaling_name(PeerConnection::Signaling s) {
  switch (s) {
    case PeerConnection::Signaling::Stable: return "stable";
    case PeerConnection::Signaling::HaveLocalOffer: return "have-local-offer";
    case PeerConnection::Signaling::HaveRemoteOffer: return "have-remote-offer";
    case PeerConnection::Signaling::HaveLocalPranswer: return "have-local-pranswer";
    case PeerConnection::Signaling::HaveRemotePranswer: return "have-remote-pranswer";
    default: return "closed";
  }
}

}  // namespace

struct PeerConnection::Channel {
  struct Pending {
    explicit Pending(allocators::IAllocator& a) : data(a) {}
    ByteBuffer data;
    bool binary = false;
  };
  enum class State : uint8_t { Connecting, Open, Closing, Closed };
  explicit Channel(allocators::IAllocator& a) : label(a), protocol(a), pending(a) {}
  uint32_t handle = 0;
  int32_t sid = -1, max_retransmits = -1, max_lifetime = -1;
  String label, protocol;
  bool ordered = true, negotiated = false, remote = false, announced = false, open_sent = false, reset_asked = false;
  State state = State::Connecting;
  List<UniquePointer<Pending>> pending;
  uint64_t pending_bytes = 0, threshold = 0, last_buffered = 0, closing_ms = 0, messages_sent = 0, messages_received = 0, bytes_sent = 0, bytes_received = 0;
};

PeerConnection::PeerConnection(allocators::IAllocator& allocator, IRtcNetwork& network, crypto::IRandom& random, const DtlsIdentity& identity)
    : allocator_(allocator), net_(network), random_(random), identity_(identity), ice_(allocator, network), dtls_(allocator, identity, random), sctp_(allocator, random), stun_servers_(allocator),
      ufrag_(allocator), pwd_(allocator), remote_ufrag_(allocator), remote_pwd_(allocator), remote_fingerprint_(allocator), session_id_(allocator), local_setup_(allocator), remote_setup_(allocator),
      mlines_(allocator), candidates_(allocator), channels_(allocator), srtp_send_(allocator), srtp_recv_(allocator), cname_(allocator), events_(allocator) {
  random_text(8, &ufrag_);
  random_text(24, &pwd_);
  random_text(16, &cname_);
  uint8_t id[8];
  random_.fill(ByteView(id, 8));
  uint64_t v = 0;
  for (const uint8_t b : id) v = (v << 8) | b;
  session_id_.append_uint(v >> 1);
  ice_.set_local_credentials(ufrag_.view(), pwd_.view());
}

PeerConnection::~PeerConnection() { ice_.close(); }

void PeerConnection::random_text(uint32_t n, String* out) {
  static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  uint8_t b[64];
  if (n > sizeof(b)) n = sizeof(b);
  random_.fill(ByteView(b, n));
  out->clear();
  for (uint32_t i = 0; i < n; ++i) out->push_back(kChars[b[i] % 62]);
}

void PeerConnection::add_stun_server(StringView url) {
  if (!url.starts_with("stun:") || stun_servers_.count() >= IceAgent::kMaxStunServers) return;
  String s(allocator_);
  s.assign(url.substr(5));
  if (s.view().starts_with("//")) s.assign(s.view().substr(2));
  const uint64_t q = s.view().find('?');
  if (q != StringView::npos) s.assign(s.view().first(q));
  stun_servers_.push_back(util::move(s));
}

void PeerConnection::event(StringView json) {
  if (events_.count() >= 4096) events_.remove_at(0);
  String e(allocator_);
  e.assign(json);
  events_.push_back(util::move(e));
}

bool PeerConnection::take_event(String* json) {
  if (events_.count() == 0) return false;
  json->assign(events_[0].view());
  events_.remove_at(0);
  return true;
}

void PeerConnection::set_signaling(Signaling s) {
  if (signaling_ == s) return;
  signaling_ = s;
  String e(allocator_);
  e.append("{\"t\":\"signalingstatechange\",\"state\":\"");
  e.append(signaling_name(s));
  e.append("\"}");
  event(e.view());
}

// --- Descriptions --------------------------------------------------------------------------------------------------------------------

bool PeerConnection::candidate_line(const IceCandidate& c, String* out) const {
  IceCandidateInfo info(allocator_);
  info.foundation.assign("");
  info.foundation.append_uint(c.foundation);
  info.protocol.assign("udp");
  info.type.assign(candidate_type_name(c.type));
  info.priority = c.priority;
  info.port = c.port;
  ip_text(c.address, &info.address);
  if (c.type == CandidateType::ServerReflexive) {
    ip_text(c.related_address, &info.related_address);
    info.related_port = c.related_port;
  }
  write_candidate(info, out);
  return true;
}

bool PeerConnection::write_sdp(bool offer, String* sdp) const {
  Sdp d(allocator_);
  String line(allocator_), fp(allocator_);
  identity_.fingerprint(&fp);
  line.assign("o=- ");
  line.append(session_id_.view());
  line.append(" ");
  line.append_uint(version_ + 1);
  line.append(" IN IP4 127.0.0.1");
  d.add_session(line.view());
  d.add_session("s=-");
  d.add_session("t=0 0");
  line.assign("a=group:BUNDLE");
  bool any = false;
  for (const UniquePointer<Mline>& m : mlines_)
    if (!m->rejected) {
      line.append(" ");
      line.append(m->mid.view());
      any = true;
    }
  if (any) d.add_session(line.view());
  d.add_session("a=msid-semantic: WMS");
  const bool include_candidates = !offer || local_;   // (a description that was set carries what has been gathered)
  (void)include_candidates;
  const Mline* first_live = nullptr;
  for (const UniquePointer<Mline>& m : mlines_)
    if (!m->rejected && !m->stopped && !first_live) first_live = m.get();
  for (const UniquePointer<Mline>& m : mlines_) {
    const bool application = m->kind.view().equals("application"), dead = m->rejected || m->stopped;
    SdpMedia* media = d.add_media(m->kind.view(), dead ? 0 : 9, application ? StringView("UDP/DTLS/SCTP") : StringView("UDP/TLS/RTP/SAVPF"));
    if (!media) return false;
    String f(allocator_);
    if (dead) f.assign(m->formats.view());
    else if (application) f.assign("webrtc-datachannel");
    else if (offer || m->payload < 0) f.assign("0 8");
    else f.append_uint(static_cast<uint32_t>(m->payload));
    if (f.empty()) f.assign(application ? "webrtc-datachannel" : "0");
    StringView rest = f.view(), token;
    while (rest.next_token(' ', &token)) {
      if (token.empty()) continue;
      String t(allocator_);
      t.assign(token);
      media->formats.push_back(util::move(t));
    }
    auto add = [&](StringView text) {
      String l(allocator_);
      l.assign(text);
      media->lines.push_back(util::move(l));
    };
    add("c=IN IP4 0.0.0.0");
    if (dead) {
      line.assign("a=mid:");
      line.append(m->mid.view());
      add(line.view());
      add("a=inactive");
      continue;
    }
    if (!application) add("a=rtcp:9 IN IP4 0.0.0.0");
    line.assign("a=ice-ufrag:");
    line.append(ufrag_.view());
    add(line.view());
    line.assign("a=ice-pwd:");
    line.append(pwd_.view());
    add(line.view());
    add("a=ice-options:trickle");
    line.assign("a=fingerprint:sha-256 ");
    line.append(fp.view());
    add(line.view());
    line.assign("a=setup:");
    line.append(local_setup_.view());
    add(line.view());
    line.assign("a=mid:");
    line.append(m->mid.view());
    add(line.view());
    if (application) {
      add("a=sctp-port:5000");
      add("a=max-message-size:262144");
    } else {
      const int direction = offer ? m->desired : m->negotiated;
      line.assign("a=");
      line.append(kDirections[direction]);
      add(line.view());
      const bool sending = sends(direction) && !m->track_id.empty();
      if (sending) {
        line.assign("a=msid:");
        line.append(m->stream_id.empty() ? StringView("-") : m->stream_id.view());
        line.append(" ");
        line.append(m->track_id.view());
        add(line.view());
      }
      add("a=rtcp-mux");
      if (offer || m->payload == 0) add("a=rtpmap:0 PCMU/8000");
      if (offer || m->payload == 8) add("a=rtpmap:8 PCMA/8000");
      if (sending) {
        line.assign("a=ssrc:");
        line.append_uint(m->ssrc);
        line.append(" cname:");
        line.append(cname_.view());
        add(line.view());
        line.assign("a=ssrc:");
        line.append_uint(m->ssrc);
        line.append(" msid:");
        line.append(m->stream_id.empty() ? StringView("-") : m->stream_id.view());
        line.append(" ");
        line.append(m->track_id.view());
        add(line.view());
      }
    }
    if (m.get() == first_live) {   // (bundled: the transport's candidates go on the first section)
      for (const String& c : candidates_) {
        line.assign("a=");
        line.append(c.view());
        add(line.view());
      }
      if (ice_.gathering() == IceGathering::Complete && gathering_started_) add("a=end-of-candidates");
    }
  }
  return d.write(sdp);
}

bool PeerConnection::read_sdp(StringView text, bool offer, String* error) {
  Sdp d(allocator_);
  if (!d.parse(text)) {
    error->assign("InvalidAccessError: The session description could not be parsed");
    return false;
  }
  if (!offer && d.media.count() != mlines_.count()) {
    error->assign("InvalidAccessError: The answer does not have the media sections of the offer");
    return false;
  }
  String ufrag(allocator_), pwd(allocator_), setup(allocator_), alg(allocator_), fp(allocator_);
  bool have_transport = false;
  List<String> found(allocator_);
  uint32_t first_index = 0;
  for (uint32_t i = 0; i < d.media.count(); ++i) {
    const SdpMedia& m = *d.media[i];
    StringView mid;
    if (!m.attribute("mid", &mid) || mid.empty()) {
      error->assign("InvalidAccessError: A media section has no mid");
      return false;
    }
    const bool application = m.kind.view().equals("application"), audio = m.kind.view().equals("audio");
    int32_t codec = -1;
    if (audio)
      for (const String& f : m.formats)
        if (codec < 0 && (f.view().equals("0") || f.view().equals("8"))) codec = f.view().equals("0") ? 0 : 8;
    const bool usable = m.port != 0 && ((application && m.protocol.view().contains("SCTP")) || (audio && m.protocol.view().contains("RTP/") && codec >= 0));
    if (!offer) {
      if (!d.media[i]->kind.view().equals(mlines_[i]->kind.view()) || !mlines_[i]->mid.view().equals(mid)) {
        error->assign("InvalidAccessError: The answer's media sections do not match the offer's");
        return false;
      }
      if (!usable) mlines_[i]->rejected = true;
    } else if (i >= mlines_.count()) {
      UniquePointer<Mline> ml = UniquePointer<Mline>::make(allocator_, allocator_);
      ml->kind.assign(m.kind.view());
      ml->mid.assign(mid);
      ml->rejected = !usable;
      for (const String& f : m.formats) {
        if (!ml->formats.empty()) ml->formats.append(" ");
        ml->formats.append(f.view());
      }
      if (audio) {   // (a section the other side opened: we receive on it until the script gives it a track)
        ml->desired = 2;
        ml->ssrc = random32();
        ml->timestamp = random32();
        ml->seq = static_cast<uint16_t>(random32());
      }
      mlines_.push_back(util::move(ml));
    }
    if (audio && i < mlines_.count()) {
      Mline& ml = *mlines_[i];
      StringView dir = "sendrecv", v;
      for (const String& line : m.lines) {
        const StringView l = line.view();
        for (int k = 0; k < 4; ++k)
          if (l.size() == 2 + strlen_of(kDirections[k]) && l.starts_with("a=") && l.substr(2).equals(kDirections[k])) dir = kDirections[k];
      }
      ml.remote_direction.assign(dir);
      if (usable) {
        ml.payload = codec;
        ml.remote_sends = sends(direction_of(dir));
        // the sending side, if it says: a=ssrc:<n> ... and a=msid:<stream> <track>
        for (uint32_t k = 0; m.nth("ssrc", k, &v); ++k) {
          uint64_t n = 0;
          const uint64_t space = v.find(' ');
          if (space != StringView::npos && v.first(space).parse_uint(&n) && n <= 0xFFFFFFFFull && ml.remote_ssrc == 0) ml.remote_ssrc = static_cast<uint32_t>(n);
        }
        if (m.attribute("msid", &v)) {
          const uint64_t space = v.find(' ');
          ml.remote_stream_id.assign(space == StringView::npos ? v : v.first(space));
          ml.remote_track_id.assign(space == StringView::npos ? StringView() : v.substr(space + 1).trimmed());
        }
        if (!offer) {   // the answer settles what we do
          ml.negotiated = static_cast<uint8_t>(resolve_direction(ml.desired, direction_of(dir)));
          ml.agreed = true;
        }
      } else {
        ml.rejected = true;
      }
    }
    if (usable && !have_transport) {
      have_transport = true;
      first_index = i;
      StringView v;
      if (!sdp_inherited(d, m, "ice-ufrag", &v) || v.size() < 4 || !ufrag.assign(v) || !sdp_inherited(d, m, "ice-pwd", &v) || v.size() < 22 || !pwd.assign(v)) {
        error->assign("InvalidAccessError: The description has no usable ICE credentials");
        return false;
      }
      if (!sdp_inherited(d, m, "fingerprint", &v) || !parse_fingerprint(v, &alg, &fp) || !alg.view().equals_ignore_case("sha-256")) {
        error->assign("InvalidAccessError: The description has no sha-256 fingerprint");
        return false;
      }
      if (!sdp_inherited(d, m, "setup", &v) || !(v.equals("actpass") || v.equals("active") || v.equals("passive")) || !setup.assign(v)) {
        error->assign("InvalidAccessError: The description has no valid setup attribute");
        return false;
      }
    }
  }
  if (have_transport) {
    if (applied_ && !remote_ufrag_.empty() && !remote_ufrag_.view().equals(ufrag.view())) {
      error->assign("NotSupportedError: Restarting ICE is not supported");
      return false;
    }
    remote_ufrag_ = util::move(ufrag);
    remote_pwd_ = util::move(pwd);
    remote_fingerprint_ = util::move(fp);
    remote_setup_ = util::move(setup);
    const SdpMedia& m = *d.media[first_index];
    StringView c;
    for (uint32_t k = 0; m.nth("candidate", k, &c); ++k) {
      IceCandidateInfo info(allocator_);
      network::Ipv4Address address;
      if (!parse_candidate(c, &info) || !info.protocol.view().equals_ignore_case("udp") || info.component != 1 || !parse_ip(info.address.view(), &address)) continue;   // (IPv6, TCP and .local names are of no use here)
      IceCandidate ic;
      ic.address = address;
      ic.port = info.port;
      ic.priority = info.priority;
      ic.type = info.type.view().equals("srflx") ? CandidateType::ServerReflexive : info.type.view().equals("prflx") ? CandidateType::PeerReflexive : info.type.view().equals("relay") ? CandidateType::Relay : CandidateType::Host;
      ice_.add_remote_candidate(ic);
    }
    if (m.has_flag("end-of-candidates")) ice_.remote_candidates_done();
  }
  return true;
}

bool PeerConnection::create_offer(String* sdp, String* error) {
  if (closed_) {
    error->assign("InvalidStateError: The connection is closed");
    return false;
  }
  if (signaling_ != Signaling::Stable && signaling_ != Signaling::HaveLocalOffer) {
    error->assign("InvalidStateError: The connection is not in a state to create an offer");
    return false;
  }
  bool any = false;
  for (const UniquePointer<Channel>& c : channels_) any = any || (c->state != Channel::State::Closed);
  bool has_application = false;
  for (const UniquePointer<Mline>& m : mlines_) has_application = has_application || m->kind.view().equals("application");
  if (any && !has_application) {
    UniquePointer<Mline> ml = UniquePointer<Mline>::make(allocator_, allocator_);
    ml->kind.assign("application");
    ml->mid.append_uint(static_cast<uint32_t>(mlines_.count()));
    mlines_.push_back(util::move(ml));
  }
  local_setup_.assign("actpass");
  if (!write_sdp(true, sdp)) {
    error->assign("OperationError: The offer could not be created");
    return false;
  }
  last_created_ = UniquePointer<Description>::make(allocator_, allocator_);
  last_created_->type.assign("offer");
  last_created_->sdp.assign(sdp->view());
  return true;
}

bool PeerConnection::create_answer(String* sdp, String* error) {
  if (closed_ || (signaling_ != Signaling::HaveRemoteOffer && signaling_ != Signaling::HaveLocalPranswer)) {
    error->assign("InvalidStateError: The connection has no remote offer to answer");
    return false;
  }
  local_setup_.assign(remote_setup_.view().equals("active") ? "passive" : "active");
  for (const UniquePointer<Mline>& m : mlines_) {   // what each audio section will do: decided by what we want and what the offer allows
    if (!m->kind.view().equals("audio") || m->rejected) continue;
    m->negotiated = static_cast<uint8_t>(m->stopped ? 3 : resolve_direction(m->desired, direction_of(m->remote_direction.view())));
    m->agreed = true;
  }
  if (!write_sdp(false, sdp)) {
    error->assign("OperationError: The answer could not be created");
    return false;
  }
  last_created_ = UniquePointer<Description>::make(allocator_, allocator_);
  last_created_->type.assign("answer");
  last_created_->sdp.assign(sdp->view());
  return true;
}

bool PeerConnection::set_local_description(StringView type, StringView sdp, String* error) {
  if (closed_) {
    error->assign("InvalidStateError: The connection is closed");
    return false;
  }
  if (type.equals("rollback")) {
    if (signaling_ != Signaling::HaveLocalOffer && signaling_ != Signaling::HaveRemoteOffer) {
      error->assign("InvalidStateError: There is nothing to roll back");
      return false;
    }
    local_ = UniquePointer<Description>();
    set_signaling(Signaling::Stable);
    return true;
  }
  const bool offer = type.equals("offer");
  if (!offer && !type.equals("answer")) {
    error->assign(type.equals("pranswer") ? "NotSupportedError: Provisional answers are not supported" : "TypeError: The type is not valid");
    return false;
  }
  if (offer ? (signaling_ != Signaling::Stable && signaling_ != Signaling::HaveLocalOffer) : signaling_ != Signaling::HaveRemoteOffer) {
    error->assign("InvalidStateError: The description does not fit the signaling state");
    return false;
  }
  if (sdp.empty()) {
    if (!last_created_ || !last_created_->type.view().equals(type)) {
      error->assign("InvalidModificationError: There is no description to set");
      return false;
    }
  } else {
    Sdp check(allocator_);
    if (!check.parse(sdp)) {
      error->assign("InvalidAccessError: The session description could not be parsed");
      return false;
    }
    if (!last_created_ || !last_created_->type.view().equals(type) || check.media.count() != mlines_.count()) {
      error->assign("InvalidModificationError: The description does not match what was created");
      return false;
    }
  }
  local_ = UniquePointer<Description>::make(allocator_, allocator_);
  local_->type.assign(type);
  local_->sdp.assign(last_created_->sdp.view());
  ++version_;
  local_offerer_ = offer;
  set_signaling(offer ? Signaling::HaveLocalOffer : Signaling::Stable);
  bool any = false;
  for (const UniquePointer<Mline>& m : mlines_) any = any || !m->rejected;
  if (any && !gathering_started_) {
    gathering_started_ = true;
    StringView servers[IceAgent::kMaxStunServers];
    uint32_t n = 0;
    for (const String& s : stun_servers_)
      if (n < IceAgent::kMaxStunServers) servers[n++] = s.view();
    if (!ice_.start_gathering(servers, n)) {
      error->assign("OperationError: ICE gathering could not start");
      return false;
    }
  }
  if (!offer) apply_descriptions();
  return true;
}

bool PeerConnection::set_remote_description(StringView type, StringView sdp, String* error) {
  if (closed_) {
    error->assign("InvalidStateError: The connection is closed");
    return false;
  }
  if (type.equals("rollback")) {
    if (signaling_ != Signaling::HaveRemoteOffer) {
      error->assign("InvalidStateError: There is nothing to roll back");
      return false;
    }
    remote_ = UniquePointer<Description>();
    set_signaling(Signaling::Stable);
    return true;
  }
  const bool offer = type.equals("offer");
  if (!offer && !type.equals("answer")) {
    error->assign(type.equals("pranswer") ? "NotSupportedError: Provisional answers are not supported" : "TypeError: The type is not valid");
    return false;
  }
  if (offer ? (signaling_ != Signaling::Stable && signaling_ != Signaling::HaveRemoteOffer) : signaling_ != Signaling::HaveLocalOffer) {
    error->assign("InvalidStateError: The description does not fit the signaling state");
    return false;
  }
  if (!read_sdp(sdp, offer, error)) return false;
  remote_ = UniquePointer<Description>::make(allocator_, allocator_);
  remote_->type.assign(type);
  remote_->sdp.assign(sdp);
  set_signaling(offer ? Signaling::HaveRemoteOffer : Signaling::Stable);
  for (uint32_t i = 0; i < mlines_.count(); ++i) {   // receivers of sections the other side sends on
    Mline& m = *mlines_[i];
    if (m.kind.view().equals("audio") && !m.rejected && m.remote_sends && !m.announced) note_track(m, i);
  }
  if (!offer) apply_descriptions();
  return true;
}

// Both descriptions are set: the roles, the credentials and the fingerprint are known, and the checks can start.
void PeerConnection::apply_descriptions() {
  if (applied_ || !local_ || !remote_ || remote_ufrag_.empty()) return;
  applied_ = true;
  ice_.set_controlling(local_offerer_);
  ice_.set_remote_credentials(remote_ufrag_.view(), remote_pwd_.view());
  dtls_client_ = local_offerer_ ? remote_setup_.view().equals("passive") : local_setup_.view().equals("active");
  dtls_.set_role(dtls_client_ ? DtlsTransport::Role::Client : DtlsTransport::Role::Server);
  dtls_.set_expected_fingerprint("sha-256", remote_fingerprint_.view());
  role_known_ = true;
  ice_.start_checks();
  if (!dtls_client_) {
    dtls_.start(net_.now_ms());
    dtls_started_ = true;
  }
  update_connection();
}

bool PeerConnection::add_ice_candidate(StringView candidate, StringView mid, int32_t index, String* error) {
  if (closed_) {
    error->assign("InvalidStateError: The connection is closed");
    return false;
  }
  if (!remote_ && !(signaling_ == Signaling::HaveRemoteOffer)) {
    error->assign("InvalidStateError: There is no remote description");
    return false;
  }
  if (candidate.empty()) {
    ice_.remote_candidates_done();
    return true;
  }
  if (index >= 0 && static_cast<uint64_t>(index) >= mlines_.count()) {
    error->assign("OperationError: The media section does not exist");
    return false;
  }
  if (!mid.empty()) {
    bool known = false;
    for (const UniquePointer<Mline>& m : mlines_) known = known || m->mid.view().equals(mid);
    if (!known) {
      error->assign("OperationError: The media section does not exist");
      return false;
    }
  }
  IceCandidateInfo info(allocator_);
  network::Ipv4Address address;
  if (!parse_candidate(candidate, &info)) {
    error->assign("OperationError: The candidate could not be parsed");
    return false;
  }
  if (!info.protocol.view().equals_ignore_case("udp") || info.component != 1 || !parse_ip(info.address.view(), &address)) return true;   // (a candidate we cannot use is no error)
  IceCandidate ic;
  ic.address = address;
  ic.port = info.port;
  ic.priority = info.priority;
  ic.type = info.type.view().equals("srflx") ? CandidateType::ServerReflexive : info.type.view().equals("prflx") ? CandidateType::PeerReflexive : info.type.view().equals("relay") ? CandidateType::Relay : CandidateType::Host;
  ice_.add_remote_candidate(ic);
  return true;
}

bool PeerConnection::local_description(String* type, String* sdp) const {
  if (!local_) return false;
  type->assign(local_->type.view());
  return write_sdp(local_->type.view().equals("offer"), sdp);
}

bool PeerConnection::remote_description(String* type, String* sdp) const {
  if (!remote_) return false;
  type->assign(remote_->type.view());
  sdp->assign(remote_->sdp.view());
  return true;
}


// --- Data channels ---------------------------------------------------------------------------------------------------------------------

PeerConnection::Channel* PeerConnection::find_channel(uint32_t handle) const {
  for (const UniquePointer<Channel>& c : channels_)
    if (c->handle == handle) return c.get();
  return nullptr;
}

PeerConnection::Channel* PeerConnection::find_by_sid(int32_t sid) const {
  for (const UniquePointer<Channel>& c : channels_)
    if (c->sid == sid && c->state != Channel::State::Closed) return c.get();
  return nullptr;
}

int32_t PeerConnection::allocate_sid() {
  for (int32_t sid = dtls_client_ ? 0 : 1; sid < 65534; sid += 2)
    if (!find_by_sid(sid)) return sid;
  return -1;
}

uint32_t PeerConnection::create_channel(StringView label, StringView protocol, bool ordered, int32_t max_rtx, int32_t max_life, bool negotiated, int32_t id, String* error) {
  if (closed_) {
    error->assign("InvalidStateError: The connection is closed");
    return 0;
  }
  if (label.size() > 65535 || protocol.size() > 65535) {
    error->assign("TypeError: The label or protocol is too long");
    return 0;
  }
  if (max_rtx >= 0 && max_life >= 0) {
    error->assign("SyntaxError: maxRetransmits and maxPacketLifeTime cannot both be set");
    return 0;
  }
  if (negotiated && (id < 0 || id > 65534)) {
    error->assign("TypeError: A negotiated channel needs an id below 65535");
    return 0;
  }
  if (channels_.count() >= kMaxChannels) {
    error->assign("OperationError: Too many data channels");
    return 0;
  }
  if (negotiated && find_by_sid(id)) {
    error->assign("OperationError: The id is in use");
    return 0;
  }
  UniquePointer<Channel> c = UniquePointer<Channel>::make(allocator_, allocator_);
  if (!c || !c->label.assign(label) || !c->protocol.assign(protocol)) {
    error->assign("OperationError: Out of memory");
    return 0;
  }
  c->handle = next_handle_++;
  c->ordered = ordered;
  c->max_retransmits = max_rtx;
  c->max_lifetime = max_life;
  c->negotiated = negotiated;
  c->sid = negotiated ? id : -1;
  c->announced = true;   // (created by the script: it knows it)
  const uint32_t handle = c->handle;
  Channel* raw = c.get();
  channels_.push_back(util::move(c));
  bool had_section = false;
  for (const UniquePointer<Mline>& m : mlines_) had_section = had_section || (!m->rejected && m->kind.view().equals("application"));
  if (!had_section && !negotiation_needed_) {
    negotiation_needed_ = true;
    event("{\"t\":\"negotiationneeded\"}");
  }
  if (sctp_.established()) open_channels();
  (void)raw;
  return handle;
}

bool PeerConnection::channel_send(uint32_t handle, bool binary, ConstByteView data, String* error) {
  Channel* c = find_channel(handle);
  if (!c || c->state != Channel::State::Open) {
    error->assign("InvalidStateError: The data channel is not open");
    return false;
  }
  if (data.size() > kMaxMessage) {
    error->assign("TypeError: The message is larger than the maximum message size");
    return false;
  }
  if (c->pending_bytes + data.size() > kMaxPendingBytes) {
    error->assign("OperationError: The send queue is full");
    return false;
  }
  UniquePointer<Channel::Pending> p = UniquePointer<Channel::Pending>::make(allocator_, allocator_);
  if (!p || !p->data.append(data)) {
    error->assign("OperationError: Out of memory");
    return false;
  }
  p->binary = binary;
  c->pending_bytes += data.size();
  c->pending.push_back(util::move(p));
  flush(*c);
  return true;
}

void PeerConnection::flush(Channel& c) {
  while (c.pending.count() > 0 && c.sid >= 0 && sctp_.established()) {
    Channel::Pending& p = *c.pending[0];
    SctpOptions o;
    o.ordered = c.ordered;
    o.max_retransmits = c.max_retransmits;
    o.max_lifetime_ms = c.max_lifetime;
    const uint8_t zero = 0;
    const bool empty = p.data.size() == 0;
    const uint32_t ppid = p.binary ? (empty ? kPpidBinaryEmpty : kPpidBinary) : (empty ? kPpidStringEmpty : kPpidString);
    if (!sctp_.send(static_cast<uint16_t>(c.sid), ppid, empty ? ConstByteView(&zero, 1) : p.data.view(), o)) return;   // (the association's queue is full: later)
    c.pending_bytes -= p.data.size();
    ++c.messages_sent;
    c.bytes_sent += p.data.size();
    c.pending.remove_at(0);
  }
}

uint64_t PeerConnection::channel_buffered(uint32_t handle) const {
  const Channel* c = find_channel(handle);
  if (!c) return 0;
  return c->pending_bytes + (c->sid >= 0 ? sctp_.buffered(static_cast<uint16_t>(c->sid)) : 0);
}

void PeerConnection::channel_threshold(uint32_t handle, uint64_t threshold) {
  if (Channel* c = find_channel(handle)) c->threshold = threshold;
}

void PeerConnection::channel_close(uint32_t handle) {
  Channel* c = find_channel(handle);
  if (!c || c->state == Channel::State::Closing || c->state == Channel::State::Closed) return;
  if (c->state == Channel::State::Connecting && !c->open_sent) {
    close_channel_now(*c, false);
    return;
  }
  c->state = Channel::State::Closing;
  c->closing_ms = net_.now_ms();
  String e(allocator_);
  e.append("{\"t\":\"dcclosing\",\"id\":");
  e.append_uint(c->handle);
  e.append("}");
  event(e.view());
}

bool PeerConnection::channel_info(uint32_t handle, String* json) const {
  const Channel* c = find_channel(handle);
  if (!c) return false;
  json->assign("{\"id\":");
  json->append_uint(c->handle);
  json->append(",\"label\":");
  jq(json, c->label.view());
  json->append(",\"protocol\":");
  jq(json, c->protocol.view());
  json->append(",\"ordered\":");
  json->append(c->ordered ? "true" : "false");
  json->append(",\"maxRetransmits\":");
  if (c->max_retransmits >= 0) json->append_uint(static_cast<uint32_t>(c->max_retransmits));
  else json->append("null");
  json->append(",\"maxPacketLifeTime\":");
  if (c->max_lifetime >= 0) json->append_uint(static_cast<uint32_t>(c->max_lifetime));
  else json->append("null");
  json->append(",\"negotiated\":");
  json->append(c->negotiated ? "true" : "false");
  json->append(",\"sid\":");
  if (c->sid >= 0) json->append_uint(static_cast<uint32_t>(c->sid));
  else json->append("null");
  json->push_back('}');
  return true;
}

void PeerConnection::announce(Channel& c) {
  if (c.announced) return;
  c.announced = true;
  String e(allocator_), info(allocator_);
  channel_info(c.handle, &info);
  e.append("{\"t\":\"datachannel\",\"channel\":");
  e.append(info.view());
  e.append("}");
  event(e.view());
}

void PeerConnection::close_channel_now(Channel& c, bool error) {
  if (c.state == Channel::State::Closed) return;
  c.state = Channel::State::Closed;
  c.pending.clear();
  c.pending_bytes = 0;
  String e(allocator_);
  if (error) {
    e.append("{\"t\":\"dcerror\",\"id\":");
    e.append_uint(c.handle);
    e.append("}");
    event(e.view());
    e.clear();
  }
  e.append("{\"t\":\"dcclose\",\"id\":");
  e.append_uint(c.handle);
  e.append("}");
  event(e.view());
}

static void channel_open_event(PeerConnection* pc, String* e, uint32_t handle) {
  (void)pc;
  e->assign("{\"t\":\"dcopen\",\"id\":");
  e->append_uint(handle);
  e->append("}");
}

// The association is up: channels made so far are opened (DATA_CHANNEL_OPEN, RFC 8832; negotiated ones need none).
void PeerConnection::open_channels() {
  for (const UniquePointer<Channel>& up : channels_) {
    Channel& c = *up;
    if (c.state != Channel::State::Connecting || c.open_sent || c.remote) continue;
    if (c.sid < 0) c.sid = allocate_sid();
    if (c.sid < 0) {
      close_channel_now(c, true);
      continue;
    }
    c.open_sent = true;
    if (c.negotiated) {
      c.state = Channel::State::Open;
      String e(allocator_);
      channel_open_event(this, &e, c.handle);
      event(e.view());
      flush(c);
      continue;
    }
    ByteBuffer m(allocator_);
    uint8_t type = c.max_retransmits >= 0 ? 0x01 : c.max_lifetime >= 0 ? 0x02 : 0x00;
    if (!c.ordered) type |= 0x80;
    m.put8(kDcepOpen);
    m.put8(type);
    put_be16(&m, 0);
    put_be32(&m, c.max_retransmits >= 0 ? static_cast<uint32_t>(c.max_retransmits) : c.max_lifetime >= 0 ? static_cast<uint32_t>(c.max_lifetime) : 0);
    put_be16(&m, static_cast<uint32_t>(c.label.size()));
    put_be16(&m, static_cast<uint32_t>(c.protocol.size()));
    m.append(ConstByteView(reinterpret_cast<const uint8_t*>(c.label.view().data()), c.label.size()));
    m.append(ConstByteView(reinterpret_cast<const uint8_t*>(c.protocol.view().data()), c.protocol.size()));
    SctpOptions o;   // (reliable and ordered, whatever the channel will be)
    sctp_.send(static_cast<uint16_t>(c.sid), kPpidDcep, m.view(), o);
  }
}

void PeerConnection::handle_dcep(UniquePointer<SctpMessage> m) {
  const ConstByteView d = m->data.view();
  if (d.empty()) return;
  Channel* existing = find_by_sid(m->stream);
  if (d[0] == kDcepAck) {
    if (existing && existing->state == Channel::State::Connecting && existing->open_sent) {
      existing->state = Channel::State::Open;
      String e(allocator_);
      channel_open_event(this, &e, existing->handle);
      event(e.view());
      flush(*existing);
    }
    return;
  }
  if (d[0] != kDcepOpen || d.size() < 12) return;
  const uint8_t type = d[1];
  const uint32_t reliability = load_be32(d, 4);
  const uint64_t label_len = load_be16(d, 8), proto_len = load_be16(d, 10);
  if (12 + label_len + proto_len > d.size() || existing || channels_.count() >= kMaxChannels) return;
  const uint8_t kind = type & 0x7F;
  if (kind > 2) return;
  UniquePointer<Channel> c = UniquePointer<Channel>::make(allocator_, allocator_);
  if (!c) return;
  c->label.assign(StringView(reinterpret_cast<const char*>(d.data() + 12), label_len));
  c->protocol.assign(StringView(reinterpret_cast<const char*>(d.data() + 12 + label_len), proto_len));
  c->handle = next_handle_++;
  c->sid = m->stream;
  c->ordered = !(type & 0x80);
  c->max_retransmits = kind == 1 ? static_cast<int32_t>(reliability > 65535 ? 65535 : reliability) : -1;
  c->max_lifetime = kind == 2 ? static_cast<int32_t>(reliability > 65535 ? 65535 : reliability) : -1;
  c->remote = true;
  c->open_sent = true;
  c->state = Channel::State::Open;
  Channel* raw = c.get();
  channels_.push_back(util::move(c));
  const uint8_t ack = kDcepAck;
  SctpOptions o;
  sctp_.send(m->stream, kPpidDcep, ConstByteView(&ack, 1), o);
  announce(*raw);
  String e(allocator_);
  channel_open_event(this, &e, raw->handle);
  event(e.view());
}

void PeerConnection::deliver(UniquePointer<SctpMessage> m) {
  if (m->ppid == kPpidDcep) {
    handle_dcep(util::move(m));
    return;
  }
  Channel* c = find_by_sid(m->stream);
  if (!c || (c->state != Channel::State::Open && c->state != Channel::State::Closing)) return;
  const bool binary = m->ppid == kPpidBinary || m->ppid == kPpidBinaryEmpty;
  const bool text = m->ppid == kPpidString || m->ppid == kPpidStringEmpty;
  if (!binary && !text) return;
  const bool empty = m->ppid == kPpidBinaryEmpty || m->ppid == kPpidStringEmpty;
  ++c->messages_received;
  c->bytes_received += empty ? 0 : m->data.size();
  String e(allocator_);
  e.append("{\"t\":\"dcmessage\",\"id\":");
  e.append_uint(c->handle);
  e.append(",\"binary\":");
  e.append(binary ? "true" : "false");
  e.append(",\"data\":");
  if (binary) {
    e.push_back('"');
    if (!empty) hex(&e, m->data.view());
    e.push_back('"');
  } else {
    jq(&e, empty ? StringView() : StringView(reinterpret_cast<const char*>(m->data.view().data()), m->data.size()));
  }
  e.append("}");
  event(e.view());
}

// --- Driving ---------------------------------------------------------------------------------------------------------------------------

void PeerConnection::update_connection() {
  Connection now;
  const IceState ice = ice_.state();
  if (closed_) now = Connection::Closed;
  else if (dtls_.state() == DtlsTransport::State::Failed || ice == IceState::Failed) now = Connection::Failed;
  else if (ice == IceState::Disconnected) now = Connection::Disconnected;
  else if (dtls_.state() == DtlsTransport::State::Connected && (ice == IceState::Connected || ice == IceState::Completed)) now = Connection::Connected;
  else if (applied_) now = Connection::Connecting;
  else now = Connection::New;
  connection_ = now;
  if (now != reported_connection_) {
    reported_connection_ = now;
    String e(allocator_);
    e.append("{\"t\":\"connectionstatechange\",\"state\":\"");
    e.append(connection_name(now));
    e.append("\"}");
    event(e.view());
  }
}

void PeerConnection::start_dtls() {
  if (dtls_started_ || !role_known_) return;
  IceCandidate l, r;
  if (!ice_.selected(&l, &r)) return;
  dtls_started_ = true;
  dtls_.start(net_.now_ms());
}

void PeerConnection::pump_transports() {
  const uint64_t now = net_.now_ms();
  uint8_t buffer[1500];
  uint32_t n;
  for (uint32_t guard = 0; guard < 256 && (n = ice_.receive(ByteView(buffer, sizeof(buffer)))) > 0; ++guard) {
    switch (demux(ConstByteView(buffer, n))) {
      case Demux::Dtls: dtls_.receive(ConstByteView(buffer, n), now); break;
      case Demux::Rtp: handle_rtp(ConstByteView(buffer, n), now); break;
      case Demux::Rtcp: handle_rtcp(ConstByteView(buffer, n), now); break;
      default: break;
    }
  }
  start_dtls();
  if (dtls_started_) dtls_.tick(now);
  ByteBuffer d(allocator_);
  IceCandidate l, r;
  if (ice_.selected(&l, &r))
    while (dtls_.next_datagram(&d)) ice_.send(d.view());
  start_srtp();
  pump_media(now);
  bool wants_sctp = false;
  for (const UniquePointer<Mline>& m : mlines_) wants_sctp = wants_sctp || (m->kind.view().equals("application") && !m->rejected);
  if (dtls_.state() == DtlsTransport::State::Connected && !sctp_started_ && wants_sctp) {
    sctp_started_ = true;
    sctp_.set_ports(5000, 5000);
    sctp_.connect(now);
  }
  if (!sctp_started_) return;
  for (uint32_t guard = 0; guard < 256 && dtls_.receive_application(&d); ++guard) sctp_.receive(d.view(), now);
  sctp_.tick(now);
  for (uint32_t guard = 0; guard < 256 && sctp_.next_packet(&d); ++guard) dtls_.send_application(d.view());
  if (sctp_.established()) {
    open_channels();
    UniquePointer<SctpMessage> m;
    for (uint32_t guard = 0; guard < 1024 && sctp_.receive_message(&m); ++guard) deliver(util::move(m));
    uint16_t sid;
    while (sctp_.take_reset(&sid)) {   // the peer closes its side of a channel: close ours too
      Channel* c = find_by_sid(sid);
      if (!c) continue;
      if (c->state == Channel::State::Open) sctp_.reset_stream(sid);
      close_channel_now(*c, false);
    }
    for (const UniquePointer<Channel>& up : channels_) {
      Channel& c = *up;
      if (c.state == Channel::State::Open) flush(c);
      if (c.state == Channel::State::Closing && c.pending.count() == 0 && c.sid >= 0) {
        if (!c.reset_asked) {
          sctp_.reset_stream(static_cast<uint16_t>(c.sid));
          c.reset_asked = true;
        }
        if (net_.now_ms() - c.closing_ms > 5000) close_channel_now(c, false);
      }
      if (c.state == Channel::State::Open || c.state == Channel::State::Closing) {
        const uint64_t buffered = channel_buffered(c.handle);
        if (c.last_buffered > c.threshold && buffered <= c.threshold) {
          String e(allocator_);
          e.append("{\"t\":\"dcbufferedamountlow\",\"id\":");
          e.append_uint(c.handle);
          e.append("}");
          event(e.view());
        }
        c.last_buffered = buffered;
      }
    }
  }
  if (sctp_.state() == SctpAssociation::State::Closed && sctp_started_ && connection_ == Connection::Connected && !closed_) {
    for (const UniquePointer<Channel>& up : channels_) close_channel_now(*up, false);
  }
}

void PeerConnection::poll() {
  if (closed_) return;
  ice_.poll();
  IceCandidate c;
  while (ice_.take_local_candidate(&c)) {
    String line(allocator_), e(allocator_);
    candidate_line(c, &line);
    String stored(allocator_);
    stored.assign(line.view());
    candidates_.push_back(util::move(stored));
    e.append("{\"t\":\"icecandidate\",\"candidate\":");
    jq(&e, line.view());
    e.append(",\"sdpMid\":");
    jq(&e, mlines_.count() ? mlines_[0]->mid.view() : StringView("0"));
    e.append(",\"sdpMLineIndex\":0,\"usernameFragment\":");
    jq(&e, ufrag_.view());
    e.append("}");
    event(e.view());
  }
  if (ice_.gathering() != reported_gathering_) {
    reported_gathering_ = ice_.gathering();
    String e(allocator_);
    e.append("{\"t\":\"icegatheringstatechange\",\"state\":\"");
    e.append(reported_gathering_ == IceGathering::Complete ? "complete" : reported_gathering_ == IceGathering::Gathering ? "gathering" : "new");
    e.append("\"}");
    event(e.view());
    if (reported_gathering_ == IceGathering::Complete && !eoc_sent_) {
      eoc_sent_ = true;
      event("{\"t\":\"icecandidate\",\"candidate\":null}");
    }
  }
  pump_transports();
  if (ice_.state() != reported_ice_) {
    reported_ice_ = ice_.state();
    String e(allocator_);
    e.append("{\"t\":\"iceconnectionstatechange\",\"state\":\"");
    e.append(ice_name(reported_ice_));
    e.append("\"}");
    event(e.view());
  }
  update_connection();
}

void PeerConnection::close() {
  if (closed_) return;
  for (const UniquePointer<Channel>& up : channels_) close_channel_now(*up, false);
  closed_ = true;
  sctp_.abort();
  ByteBuffer d(allocator_);
  dtls_.close();
  IceCandidate l, r;
  if (ice_.selected(&l, &r)) {
    while (sctp_.next_packet(&d)) dtls_.send_application(d.view());
    while (dtls_.next_datagram(&d)) ice_.send(d.view());
  }
  ice_.close();
  set_signaling(Signaling::Closed);
  update_connection();
}

void PeerConnection::stats(String* json) {
  json->assign("[");
  uint32_t opened = 0, closed_count = 0;
  for (const UniquePointer<Channel>& c : channels_) {
    if (c->state == Channel::State::Closed) ++closed_count;
    else if (c->state == Channel::State::Open) ++opened;
  }
  json->append("{\"type\":\"peer-connection\",\"id\":\"P\",\"dataChannelsOpened\":");
  json->append_uint(opened + closed_count);
  json->append(",\"dataChannelsClosed\":");
  json->append_uint(closed_count);
  json->append("},{\"type\":\"transport\",\"id\":\"T\",\"dtlsState\":\"");
  json->append(dtls_.state() == DtlsTransport::State::Connected ? "connected" : dtls_.state() == DtlsTransport::State::Failed ? "failed" : dtls_.state() == DtlsTransport::State::Closed ? "closed" : dtls_.state() == DtlsTransport::State::Connecting ? "connecting" : "new");
  json->append("\",\"iceState\":\"");
  json->append(ice_name(ice_.state()));
  json->append("\",\"stunRequestsSent\":");
  json->append_uint(ice_.requests_sent());
  json->append(",\"stunResponsesReceived\":");
  json->append_uint(ice_.responses_received());
  json->append(",\"selectedCandidatePairId\":\"CP\"}");
  IceCandidate l, r;
  if (ice_.selected(&l, &r)) {
    String a(allocator_);
    json->append(",{\"type\":\"candidate-pair\",\"id\":\"CP\",\"state\":\"succeeded\",\"nominated\":true,\"localCandidateId\":\"LC\",\"remoteCandidateId\":\"RC\"}");
    json->append(",{\"type\":\"local-candidate\",\"id\":\"LC\",\"candidateType\":\"");
    json->append(candidate_type_name(l.type));
    json->append("\",\"address\":\"");
    ip_text(l.address, json);
    json->append("\",\"port\":");
    json->append_uint(l.port);
    json->append(",\"protocol\":\"udp\"},{\"type\":\"remote-candidate\",\"id\":\"RC\",\"candidateType\":\"");
    json->append(candidate_type_name(r.type));
    json->append("\",\"address\":\"");
    ip_text(r.address, json);
    json->append("\",\"port\":");
    json->append_uint(r.port);
    json->append(",\"protocol\":\"udp\"}");
  }
  for (uint32_t i = 0; i < mlines_.count(); ++i) {
    const Mline& m = *mlines_[i];
    if (!m.kind.view().equals("audio") || m.rejected) continue;
    if (m.packets_sent > 0 || m.source) {
      json->append(",{\"type\":\"outbound-rtp\",\"id\":\"OA");
      json->append_uint(i);
      json->append("\",\"kind\":\"audio\",\"ssrc\":");
      json->append_uint(m.ssrc);
      json->append(",\"packetsSent\":");
      json->append_uint(m.packets_sent);
      json->append(",\"bytesSent\":");
      json->append_uint(m.octets_sent);
      json->append("}");
    }
    if (m.packets_received > 0 || m.remote_sends) {
      json->append(",{\"type\":\"inbound-rtp\",\"id\":\"IA");
      json->append_uint(i);
      json->append("\",\"kind\":\"audio\",\"ssrc\":");
      json->append_uint(m.remote_ssrc);
      json->append(",\"packetsReceived\":");
      json->append_uint(m.packets_received);
      json->append(",\"bytesReceived\":");
      json->append_uint(m.octets_received);
      json->append(",\"packetsLost\":");
      json->append_uint(m.packets_lost);
      json->append(",\"jitter\":");
      json->append_uint(m.jitter / 8);
      json->append("}");
    }
  }
  for (const UniquePointer<Channel>& c : channels_) {
    json->append(",{\"type\":\"data-channel\",\"id\":\"D");
    json->append_uint(c->handle);
    json->append("\",\"label\":");
    jq(json, c->label.view());
    json->append(",\"protocol\":");
    jq(json, c->protocol.view());
    json->append(",\"state\":\"");
    json->append(c->state == Channel::State::Open ? "open" : c->state == Channel::State::Closing ? "closing" : c->state == Channel::State::Closed ? "closed" : "connecting");
    json->append("\",\"messagesSent\":");
    json->append_uint(c->messages_sent);
    json->append(",\"bytesSent\":");
    json->append_uint(c->bytes_sent);
    json->append(",\"messagesReceived\":");
    json->append_uint(c->messages_received);
    json->append(",\"bytesReceived\":");
    json->append_uint(c->bytes_received);
    json->append("}");
  }
  json->push_back(']');
}

// --- Audio ------------------------------------------------------------------------------------------------------------------------------

uint32_t PeerConnection::random32() {
  uint8_t b[4] = {};
  random_.fill(ByteView(b, 4));
  return load_be32(ConstByteView(b, 4), 0);
}

PeerConnection::Mline* PeerConnection::audio_at(uint32_t index) const {
  if (index >= mlines_.count() || !mlines_[index]->kind.view().equals("audio")) return nullptr;
  return mlines_[index].get();
}

uint32_t PeerConnection::transceiver_count() const { return static_cast<uint32_t>(mlines_.count()); }

void PeerConnection::media_event(StringView type, uint32_t index) {
  String e(allocator_);
  e.append("{\"t\":\"");
  e.append(type);
  e.append("\",\"index\":");
  e.append_uint(index);
  e.append("}");
  event(e.view());
}

void PeerConnection::note_track(Mline& m, uint32_t index) {
  m.announced = true;
  String e(allocator_);
  e.append("{\"t\":\"track\",\"index\":");
  e.append_uint(index);
  e.append(",\"mid\":");
  jq(&e, m.mid.view());
  e.append(",\"trackId\":");
  jq(&e, m.remote_track_id.empty() ? m.mid.view() : m.remote_track_id.view());
  e.append(",\"streamId\":");
  jq(&e, m.remote_stream_id.view());
  e.append("}");
  event(e.view());
}

int32_t PeerConnection::add_transceiver(StringView kind, int direction, String* error) {
  if (closed_) {
    error->assign("InvalidStateError: The connection is closed");
    return -1;
  }
  if (!kind.equals("audio")) {
    error->assign("NotSupportedError: Only audio is supported");
    return -1;
  }
  if (direction < 0 || direction > 3 || mlines_.count() >= 16) {
    error->assign("TypeError: The direction is not valid");
    return -1;
  }
  UniquePointer<Mline> ml = UniquePointer<Mline>::make(allocator_, allocator_);
  if (!ml) {
    error->assign("OperationError: Out of memory");
    return -1;
  }
  ml->kind.assign("audio");
  ml->mid.append_uint(static_cast<uint32_t>(mlines_.count()));
  ml->desired = static_cast<uint8_t>(direction);
  ml->ssrc = random32();
  ml->timestamp = random32();
  ml->seq = static_cast<uint16_t>(random32());
  mlines_.push_back(util::move(ml));
  media_event("negotiationneeded", 0);
  return static_cast<int32_t>(mlines_.count() - 1);
}

bool PeerConnection::set_sender(uint32_t index, IRtcAudioSource* source, StringView track_id, StringView stream_id, String* error) {
  Mline* m = audio_at(index);
  if (!m || m->stopped) {
    error->assign("InvalidStateError: The transceiver is stopped");
    return false;
  }
  const bool changed = !m->track_id.view().equals(track_id) || sends(m->desired) == false;
  m->source = source;
  m->track_id.assign(track_id);
  m->stream_id.assign(stream_id);
  if (!track_id.empty()) {
    m->ever_sent = true;
    if (m->desired == 2) m->desired = 0;
    else if (m->desired == 3) m->desired = 1;
  }
  m->staged = 0;
  if (changed) media_event("negotiationneeded", 0);
  return true;
}

int32_t PeerConnection::add_track(IRtcAudioSource* source, StringView track_id, StringView stream_id, String* error) {
  for (uint32_t i = 0; i < mlines_.count(); ++i) {   // a transceiver that can take it (RFC 9429 5.2)
    Mline& m = *mlines_[i];
    if (m.kind.view().equals("audio") && !m.stopped && !m.rejected && m.track_id.empty() && !m.ever_sent && (m.desired == 2 || m.desired == 3)) {
      if (!set_sender(i, source, track_id, stream_id, error)) return -1;
      return static_cast<int32_t>(i);
    }
  }
  const int32_t index = add_transceiver("audio", 0, error);
  if (index < 0) return -1;
  if (!set_sender(static_cast<uint32_t>(index), source, track_id, stream_id, error)) return -1;
  return index;
}

bool PeerConnection::remove_sender(uint32_t index, String* error) {
  Mline* m = audio_at(index);
  if (!m || m->stopped || m->track_id.empty()) {
    error->assign("InvalidStateError: There is no track to remove");
    return false;
  }
  m->source = nullptr;
  m->track_id.clear();
  m->stream_id.clear();
  m->staged = 0;
  if (m->desired == 0) m->desired = 2;
  else if (m->desired == 1) m->desired = 3;
  media_event("negotiationneeded", 0);
  return true;
}

bool PeerConnection::set_direction(uint32_t index, int direction, String* error) {
  Mline* m = audio_at(index);
  if (!m || m->stopped || direction < 0 || direction > 3) {
    error->assign("InvalidStateError: The transceiver is stopped");
    return false;
  }
  if (m->desired != direction) {
    m->desired = static_cast<uint8_t>(direction);
    media_event("negotiationneeded", 0);
  }
  return true;
}

bool PeerConnection::stop_transceiver(uint32_t index, String* error) {
  Mline* m = audio_at(index);
  if (!m) {
    error->assign("InvalidStateError: There is no such transceiver");
    return false;
  }
  if (m->stopped) return true;
  m->stopped = true;
  m->source = nullptr;
  m->sink.reset();
  m->playing = false;
  m->negotiated = 3;
  if (m->formats.empty()) m->formats.assign("0");
  media_event("negotiationneeded", 0);
  media_event("trackmute", index);
  return true;
}

bool PeerConnection::set_sender_enabled(uint32_t index, bool enabled) {
  Mline* m = audio_at(index);
  if (!m) return false;
  m->enabled = enabled;
  return true;
}

bool PeerConnection::set_receiver_playing(uint32_t index, bool playing) {
  Mline* m = audio_at(index);
  if (!m) return false;
  if (playing && !m->sink && devices_) m->sink = devices_->open_speaker();
  if (!playing) m->sink.reset();
  m->playing = playing && m->sink;
  return true;
}

void PeerConnection::drop_source(IRtcAudioSource* source) {
  for (const UniquePointer<Mline>& m : mlines_)
    if (m->source == source) {
      m->source = nullptr;
      m->staged = 0;
    }
}

bool PeerConnection::transceiver_info(uint32_t index, String* json) const {
  const Mline* m = audio_at(index);
  if (!m) return false;
  json->assign("{\"index\":");
  json->append_uint(index);
  json->append(",\"mid\":");
  if (m->agreed || (local_ && !m->mid.empty())) jq(json, m->mid.view());
  else json->append("null");
  json->append(",\"kind\":\"audio\",\"direction\":\"");
  json->append(kDirections[m->stopped ? 3 : m->desired]);
  json->append("\",\"currentDirection\":");
  if (m->agreed) {
    json->push_back('"');
    json->append(kDirections[m->negotiated]);
    json->push_back('"');
  } else {
    json->append("null");
  }
  json->append(",\"stopped\":");
  json->append(m->stopped ? "true" : "false");
  json->append(",\"trackId\":");
  if (m->track_id.empty()) json->append("null");
  else jq(json, m->track_id.view());
  json->append(",\"streamId\":");
  jq(json, m->stream_id.view());
  json->append(",\"remoteTrackId\":");
  jq(json, m->remote_track_id.empty() ? m->mid.view() : m->remote_track_id.view());
  json->append(",\"remoteStreamId\":");
  jq(json, m->remote_stream_id.view());
  json->append(",\"muted\":");
  json->append(m->unmuted ? "false" : "true");
  json->append(",\"announced\":");
  json->append(m->announced ? "true" : "false");
  json->append(",\"payload\":");
  json->append_uint(m->payload < 0 ? 0 : static_cast<uint32_t>(m->payload));
  json->push_back('}');
  return true;
}

void PeerConnection::start_srtp() {
  if (srtp_ready_ || dtls_.state() != DtlsTransport::State::Connected) return;
  SrtpProfile profile;
  uint8_t ck[32], sk[32], cs[14], ss[14];
  if (!dtls_.srtp_keys(&profile, ck, sk, cs, ss)) return;
  const uint32_t kl = srtp_key_size(profile), sl = srtp_salt_size(profile);
  const bool ok = dtls_client_ ? srtp_send_.init(profile, ConstByteView(ck, kl), ConstByteView(cs, sl)) && srtp_recv_.init(profile, ConstByteView(sk, kl), ConstByteView(ss, sl))
                               : srtp_send_.init(profile, ConstByteView(sk, kl), ConstByteView(ss, sl)) && srtp_recv_.init(profile, ConstByteView(ck, kl), ConstByteView(cs, sl));
  srtp_ready_ = ok;
}

// A frame is 20 ms: 160 samples, 160 bytes of G.711.
void PeerConnection::pump_media(uint64_t now) {
  if (!srtp_ready_) return;
  IceCandidate l, r;
  if (!ice_.selected(&l, &r)) return;
  for (const UniquePointer<Mline>& up : mlines_) {
    Mline& m = *up;
    if (!m.kind.view().equals("audio") || m.rejected || m.stopped || !m.agreed || !sends(m.negotiated) || m.payload < 0 || !m.source) continue;
    for (uint32_t frames = 0; frames < 10; ++frames) {
      m.staged += m.source->read(ArrayView<int16_t>(m.stage + m.staged, 160 - m.staged));
      if (m.staged < 160) break;
      m.staged = 0;
      ByteBuffer packet(allocator_);
      RtpHeader h;
      h.payload_type = static_cast<uint8_t>(m.payload);
      h.marker = m.first_packet;
      h.sequence = m.seq++;
      h.timestamp = m.timestamp;
      h.ssrc = m.ssrc;
      m.timestamp += 160;
      m.first_packet = false;
      if (!rtp_write_header(&packet, h)) continue;
      for (uint32_t i = 0; i < 160; ++i) {
        const int16_t sample = m.enabled ? m.stage[i] : 0;
        packet.put8(m.payload == 0 ? g711_ulaw_encode(sample) : g711_alaw_encode(sample));
      }
      if (!srtp_send_.protect_rtp(&packet)) continue;
      ice_.send(packet.view());
      ++m.packets_sent;
      m.octets_sent += 160;
    }
  }
  send_reports(now);
}

void PeerConnection::send_reports(uint64_t now) {
  for (uint32_t i = 0; i < mlines_.count(); ++i) {
    Mline& m = *mlines_[i];
    if (!m.kind.view().equals("audio") || m.rejected || m.stopped || !m.agreed || m.payload < 0) continue;
    if (now - m.last_report_ms < 1000 && m.last_report_ms != 0) continue;
    const bool sending = sends(m.negotiated) && m.packets_sent > 0, receiving = receives(m.negotiated) && m.remote_ssrc != 0;
    if (!sending && !receiving) continue;
    if (m.last_report_ms == 0) {   // (the first report goes out a second after the first packet)
      m.last_report_ms = now;
      continue;
    }
    m.last_report_ms = now;
    RtcpReportBlock block;
    uint32_t blocks = 0;
    if (receiving) {
      block.ssrc = m.remote_ssrc;
      block.cumulative_lost = static_cast<int32_t>(m.packets_lost);
      block.highest_sequence = m.have_seq ? static_cast<uint32_t>(m.expected_seq - 1) : 0;
      block.jitter = m.jitter;
      block.last_sr = m.last_sr;
      block.delay_since_sr = m.last_sr ? static_cast<uint32_t>(((now - m.last_sr_ms) * 65536) / 1000) : 0;
      blocks = 1;
    }
    ByteBuffer packet(allocator_);
    const uint32_t ntp_hi = static_cast<uint32_t>(2208988800ull + now / 1000), ntp_lo = static_cast<uint32_t>(((now % 1000) << 32) / 1000);
    const bool ok = sending ? rtcp_write_sender_report(&packet, m.ssrc, ntp_hi, ntp_lo, m.timestamp, static_cast<uint32_t>(m.packets_sent), static_cast<uint32_t>(m.octets_sent), &block, blocks)
                            : rtcp_write_receiver_report(&packet, m.ssrc, &block, blocks);
    if (!ok || !rtcp_write_sdes(&packet, m.ssrc, cname_.view()) || !srtp_send_.protect_rtcp(&packet)) continue;
    ice_.send(packet.view());
  }
}

void PeerConnection::handle_rtp(ConstByteView datagram, uint64_t now) {
  if (!srtp_ready_) return;
  ByteBuffer packet(allocator_);
  if (!packet.append(datagram) || !srtp_recv_.unprotect_rtp(&packet)) return;
  RtpHeader h;
  if (!rtp_parse(packet.view(), &h) || h.payload_size == 0 || h.payload_size > 1200) return;
  uint32_t index = 0;
  Mline* m = nullptr;
  for (uint32_t i = 0; i < mlines_.count() && !m; ++i) {
    Mline& c = *mlines_[i];
    if (!c.kind.view().equals("audio") || c.rejected || c.stopped || !c.agreed || !receives(c.negotiated) || c.payload != h.payload_type) continue;
    if (c.remote_ssrc == h.ssrc || c.remote_ssrc == 0) {
      m = &c;
      index = i;
    }
  }
  if (!m) return;
  if (m->remote_ssrc == 0) m->remote_ssrc = h.ssrc;
  const ConstByteView payload = packet.view().subview(h.header_size, h.payload_size);
  if (m->have_seq) {
    const int16_t gap = static_cast<int16_t>(h.sequence - m->expected_seq);
    if (gap < 0) return;   // late or repeated
    if (gap > 0) {
      m->packets_lost += static_cast<uint32_t>(gap);
      if (m->sink && gap <= 5) {   // the gap is filled with silence to keep the time
        ByteBuffer silence(allocator_);
        silence.put_zeros(static_cast<uint64_t>(gap) * payload.size() * 2);
        m->sink->write(silence.view());
      }
    }
  }
  m->have_seq = true;
  m->expected_seq = static_cast<uint16_t>(h.sequence + 1);
  ++m->packets_received;
  m->octets_received += payload.size();
  if (m->last_arrival_ms != 0) {   // jitter, RFC 3550 6.4.1, in units of 1/8000 s
    const int64_t d = (static_cast<int64_t>(now - m->last_arrival_ms) * 8) - static_cast<int32_t>(h.timestamp - m->last_rtp_timestamp);
    const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
    m->jitter = static_cast<uint32_t>(static_cast<int64_t>(m->jitter) + ((static_cast<int64_t>(ad) - m->jitter) / 16));
  }
  m->last_arrival_ms = now;
  m->last_rtp_timestamp = h.timestamp;
  if (!m->unmuted) {
    m->unmuted = true;
    media_event("trackunmute", index);
  }
  if (m->sink) {
    ByteBuffer pcm(allocator_);
    for (const uint8_t b : payload) {
      const int16_t v = m->payload == 0 ? g711_ulaw_decode(b) : g711_alaw_decode(b);
      pcm.put8(static_cast<uint8_t>(v));
      pcm.put8(static_cast<uint8_t>(v >> 8));
    }
    m->sink->write(pcm.view());
  }
}

void PeerConnection::handle_rtcp(ConstByteView datagram, uint64_t now) {
  if (!srtp_ready_) return;
  ByteBuffer packet(allocator_);
  if (!packet.append(datagram) || !srtp_recv_.unprotect_rtcp(&packet)) return;
  rtcp_walk(packet.view(), [&](const RtcpPacket& p, ConstByteView whole) {
    if (p.type == kRtcpSr) {
      for (const UniquePointer<Mline>& up : mlines_)
        if (up->remote_ssrc == p.sender_ssrc) {
          up->last_sr = (p.ntp_hi << 16) | (p.ntp_lo >> 16);
          up->last_sr_ms = now;
        }
    } else if (p.type == kRtcpBye) {
      for (uint32_t k = 0; k < p.count_or_format; ++k) {
        uint32_t ssrc;
        if (!rtcp_bye_ssrc(whole, p, k, &ssrc)) break;
        for (uint32_t i = 0; i < mlines_.count(); ++i)
          if (mlines_[i]->remote_ssrc == ssrc && mlines_[i]->unmuted) {
            mlines_[i]->unmuted = false;
            media_event("trackmute", i);
          }
      }
    }
  });
}

}  // namespace rtc
