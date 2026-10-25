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

#include "Rtc/Sctp.hpp"
#include "Crypto/Crypto.hpp"
#include "Utility/Move.hpp"

namespace rtc {

namespace {

constexpr uint8_t kData = 0, kInit = 1, kInitAck = 2, kSack = 3, kHeartbeat = 4, kHeartbeatAck = 5, kAbort = 6, kShutdown = 7, kShutdownAck = 8, kError = 9, kCookieEcho = 10, kCookieAck = 11,
                  kShutdownComplete = 14, kReconfig = 130, kForwardTsn = 192;
constexpr uint32_t kReceiveWindow = 1024 * 1024, kMtu = 1100, kDataPayload = 1000, kCookieSize = 24 + 20, kMaxQueued = 4 * 1024 * 1024, kMaxGap = 4096;

bool tsn_le(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }
bool tsn_lt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
uint64_t padded(uint64_t n) { return (n + 3) & ~uint64_t{3}; }

}  // namespace

uint32_t crc32c(ConstByteView data) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
      table[i] = c;
    }
    ready = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (const uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

SctpAssociation::SctpAssociation(allocators::IAllocator& allocator, crypto::IRandom& random)
    : allocator_(allocator), random_(random), cookie_(allocator), init_value_(allocator), out_(allocator), streams_(allocator), gaps_(allocator), dups_(allocator), in_(allocator), inbox_(allocator),
      control_(allocator), reset_wanted_(allocator), reset_inflight_(allocator), resets_taken_(allocator) {
  random_.fill(ByteView(secret_, 20));
}

uint32_t SctpAssociation::random32() {
  uint8_t b[4] = {};
  random_.fill(ByteView(b, 4));
  return load_be32(ConstByteView(b, 4), 0);
}

SctpAssociation::StreamState& SctpAssociation::stream_state(uint16_t id) {
  for (StreamState& s : streams_)
    if (s.id == id) return s;
  StreamState fresh;
  fresh.id = id;
  streams_.push_back(fresh);
  return streams_[streams_.count() - 1];
}

// --- Packets out ------------------------------------------------------------------------------------------------------------------

void SctpAssociation::queue_packet(uint32_t tag, const ByteBuffer& chunks) {
  UniquePointer<ByteBuffer> p = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
  if (!p) return;
  put_be16(p.get(), local_port_);
  put_be16(p.get(), remote_port_);
  put_be32(p.get(), tag);
  put_be32(p.get(), 0);
  p->append(chunks.view());
  const uint32_t crc = crc32c(p->view());
  ByteView v = p->writable_view();
  v[8] = static_cast<uint8_t>(crc);   // (the checksum goes out least significant byte first)
  v[9] = static_cast<uint8_t>(crc >> 8);
  v[10] = static_cast<uint8_t>(crc >> 16);
  v[11] = static_cast<uint8_t>(crc >> 24);
  control_.push_back(util::move(p));
}

static bool put_chunk(ByteBuffer* out, uint8_t type, uint8_t flags, ConstByteView value) {
  if (!out->put8(type) || !out->put8(flags) || !put_be16(out, static_cast<uint32_t>(4 + value.size())) || !out->append(value)) return false;
  return out->put_zeros(padded(value.size()) - value.size());
}

void SctpAssociation::queue_control(uint8_t type, uint8_t flags, ConstByteView value, uint32_t tag) {
  ByteBuffer c(allocator_);
  if (put_chunk(&c, type, flags, value)) queue_packet(tag, c);
}

void SctpAssociation::send_init() {
  ByteBuffer v(allocator_);
  put_be32(&v, my_tag_);
  put_be32(&v, kReceiveWindow);
  put_be16(&v, 65535);
  put_be16(&v, 65535);
  put_be32(&v, init_tsn_);
  put_be16(&v, 0xC000);   // forward TSN supported
  put_be16(&v, 4);
  put_be16(&v, 0x8008);   // supported extensions: FORWARD-TSN and RE-CONFIG
  put_be16(&v, 6);
  v.put8(kForwardTsn);
  v.put8(kReconfig);
  v.put_zeros(2);
  queue_control(kInit, 0, v.view(), 0);
}

void SctpAssociation::send_cookie_echo() { queue_control(kCookieEcho, 0, cookie_.view(), peer_tag_); }

void SctpAssociation::connect(uint64_t now) {
  now_ms_ = now;
  if (state_ != State::Closed) return;
  my_tag_ = random32() | 1;
  init_tsn_ = random32();
  next_tsn_ = init_tsn_;
  state_ = State::CookieWait;
  t1_attempts_ = 1;
  t1_at_ = now + 1000;
  send_init();
}

bool SctpAssociation::next_packet(ByteBuffer* out) {
  if (control_.count() > 0) {
    out->clear();
    out->append(control_[0]->view());
    control_.remove_at(0);
    return true;
  }
  return build_data_packet(out);
}

uint32_t SctpAssociation::flight_size() const {
  uint32_t n = 0;
  for (const UniquePointer<Out>& o : out_)
    if (o->sent && !o->acked && !o->abandoned && !o->retransmit) n += static_cast<uint32_t>(o->data.size()) + 16;
  return n;
}

bool SctpAssociation::build_data_packet(ByteBuffer* out) {
  if (state_ != State::Established && state_ != State::ShutdownPending && state_ != State::ShutdownReceived) return false;
  ByteBuffer chunks(allocator_);
  if (forward_pending_) {   // FORWARD-TSN: the peer is told to skip what was given up on
    uint32_t advance = cum_acked_;
    bool any = false;
    ByteBuffer streams(allocator_);
    for (const UniquePointer<Out>& o : out_) {
      if (o->tsn != advance + 1 || !(o->abandoned || o->acked)) break;
      advance = o->tsn;
      if (o->abandoned) any = true;
    }
    if (!any || tsn_le(advance, cum_acked_)) {
      forward_pending_ = false;
    } else {
      for (const UniquePointer<Out>& o : out_) {
        if (tsn_lt(advance, o->tsn)) break;
        if (o->abandoned && !(o->flags & 4)) {   // ordered: the stream's sequence number moves on
          put_be16(&streams, o->stream);
          put_be16(&streams, o->ssn);
        }
      }
      ByteBuffer v(allocator_);
      put_be32(&v, advance);
      v.append(streams.view());
      put_chunk(&chunks, kForwardTsn, 0, v.view());
      forward_tsn_sent_ = advance;
      forward_pending_ = tsn_lt(cum_acked_, advance) && false;   // (sent again only when the timer runs out)
    }
  }
  const uint32_t window = peer_rwnd_;
  uint32_t flight = flight_size();
  bool retransmit_pass = true;
  for (int pass = 0; pass < 2; ++pass, retransmit_pass = false) {
    for (const UniquePointer<Out>& o : out_) {
      if (o->abandoned || o->acked) continue;
      if (retransmit_pass ? !o->retransmit : o->sent) continue;
      const uint32_t size = static_cast<uint32_t>(o->data.size()) + 16;
      if (chunks.size() + padded(size) + 12 > kMaxPacket) goto full;
      if (!retransmit_pass || !o->retransmit) {
        if (flight + size > cwnd_ && flight > 0) goto full;
        if (flight + size > window && flight > 0) goto full;
      }
      ByteBuffer v(allocator_);
      put_be32(&v, o->tsn);
      put_be16(&v, o->stream);
      put_be16(&v, o->ssn);
      put_be32(&v, o->ppid);
      v.append(o->data.view());
      put_chunk(&chunks, kData, o->flags, v.view());
      if (o->retransmit) ++retransmissions_;
      o->sent = true;
      o->retransmit = false;
      o->sent_ms = now_ms_;
      ++o->transmissions;
      flight += size;
      if (t3_at_ == 0) t3_at_ = now_ms_ + rto_ms_;
      if (queued_bytes_ >= o->data.size() && o->transmissions == 1) {}
    }
  }
full:
  if (chunks.size() == 0) return false;
  out->clear();
  put_be16(out, local_port_);
  put_be16(out, remote_port_);
  put_be32(out, peer_tag_);
  put_be32(out, 0);
  out->append(chunks.view());
  const uint32_t crc = crc32c(out->view());
  ByteView v = out->writable_view();
  v[8] = static_cast<uint8_t>(crc);
  v[9] = static_cast<uint8_t>(crc >> 8);
  v[10] = static_cast<uint8_t>(crc >> 16);
  v[11] = static_cast<uint8_t>(crc >> 24);
  return true;
}

bool SctpAssociation::send(uint16_t stream, uint32_t ppid, ConstByteView data, const SctpOptions& o) {
  if (state_ != State::Established || data.empty() || data.size() > kMaxMessage || queued_bytes_ + data.size() > kMaxQueued) return false;
  StreamState& ss = stream_state(stream);
  const uint16_t ssn = o.ordered ? ss.next_out_ssn++ : 0;
  const uint32_t message = next_message_++;
  uint64_t offset = 0;
  while (offset < data.size()) {
    const uint64_t n = data.size() - offset < kDataPayload ? data.size() - offset : kDataPayload;
    UniquePointer<Out> c = UniquePointer<Out>::make(allocator_, allocator_);
    if (!c || !c->data.append(data.subview(offset, n))) return false;
    c->tsn = next_tsn_++;
    c->stream = stream;
    c->ssn = ssn;
    c->ppid = ppid;
    c->message = message;
    c->flags = static_cast<uint8_t>((offset == 0 ? 2 : 0) | (offset + n == data.size() ? 1 : 0) | (o.ordered ? 0 : 4));
    c->max_rtx = o.max_retransmits;
    c->expires_ms = o.max_lifetime_ms >= 0 ? now_ms_ + static_cast<uint64_t>(o.max_lifetime_ms) + 1 : 0;
    out_.push_back(util::move(c));
    offset += n;
  }
  queued_bytes_ += static_cast<uint32_t>(data.size());
  return true;
}

uint64_t SctpAssociation::buffered(uint16_t stream) const {
  uint64_t n = 0;
  for (const UniquePointer<Out>& o : out_)
    if (o->stream == stream && !o->acked && !o->abandoned) n += o->data.size();
  return n;
}

// --- Packets in -------------------------------------------------------------------------------------------------------------------

void SctpAssociation::receive(ConstByteView p, uint64_t now) {
  now_ms_ = now;
  if (p.size() < 12 + 4) return;
  ByteBuffer copy(allocator_);
  if (!copy.append(p)) return;
  ByteView v = copy.writable_view();
  const uint32_t wire = static_cast<uint32_t>(v[8]) | (uint32_t{v[9]} << 8) | (uint32_t{v[10]} << 16) | (uint32_t{v[11]} << 24);
  v[8] = v[9] = v[10] = v[11] = 0;
  if (crc32c(copy.view()) != wire) return;
  const uint32_t tag = load_be32(p, 4);
  bool got_data = false;
  uint64_t at = 12;
  bool first = true;
  while (p.size() - at >= 4) {
    const uint8_t type = p[at], flags = p[at + 1];
    const uint64_t len = load_be16(p, at + 2);
    if (len < 4 || len > p.size() - at) return;
    // the verification tag: INIT has none; everything else carries ours (ABORT and SHUTDOWN COMPLETE may say "T" and carry the sender's)
    if (type == kInit) {
      if (tag != 0 || !first) return;
    } else if (tag != my_tag_ && !((type == kAbort || type == kShutdownComplete) && (flags & 1) && tag == peer_tag_)) {
      return;
    }
    handle_chunk(type, flags, p.subview(at + 4, len - 4), tag, &got_data);
    if (state_ == State::Closed && type != kInit && type != kCookieEcho) {
      if (type == kAbort || type == kShutdownComplete) return;
    }
    at += padded(len);
    first = false;
  }
  if (got_data) send_sack();
}

void SctpAssociation::handle_chunk(uint8_t type, uint8_t flags, ConstByteView value, uint32_t, bool* got_data) {
  switch (type) {
    case kInit: handle_init(value); break;
    case kInitAck: handle_init_ack(value); break;
    case kCookieEcho: handle_cookie_echo(value); break;
    case kCookieAck:
      if (state_ == State::CookieEchoed) {
        cwnd_ = 4 * kMtu;
        ssthresh_ = peer_rwnd_ ? peer_rwnd_ : kReceiveWindow;
        cum_acked_ = init_tsn_ - 1;
        state_ = State::Established;
        t1_at_ = 0;
      }
      break;
    case kData:
      if (state_ == State::Established || state_ == State::ShutdownPending || state_ == State::ShutdownSent) {
        handle_data(flags, value);
        *got_data = true;
      }
      break;
    case kSack: handle_sack(value); break;
    case kHeartbeat: queue_control(kHeartbeatAck, 0, value, peer_tag_); break;
    case kForwardTsn: handle_forward_tsn(value); *got_data = true; break;
    case kReconfig: handle_reconfig(value); break;
    case kAbort: enter_closed(); break;
    case kShutdown:
      if (state_ == State::Established || state_ == State::ShutdownPending) {
        state_ = State::ShutdownAckSent;
        queue_control(kShutdownAck, 0, ConstByteView(), peer_tag_);
      }
      break;
    case kShutdownAck:
      if (state_ == State::ShutdownSent || state_ == State::ShutdownPending) {
        queue_control(kShutdownComplete, 0, ConstByteView(), peer_tag_);
        enter_closed();
      }
      break;
    case kShutdownComplete:
      if (state_ == State::ShutdownAckSent) enter_closed();
      break;
    default: break;
  }
}

void SctpAssociation::enter_closed() {
  state_ = State::Closed;
  out_.clear();
  queued_bytes_ = 0;
  t1_at_ = t3_at_ = 0;
}

void SctpAssociation::abort() {
  if (state_ == State::Closed) return;
  queue_control(kAbort, 0, ConstByteView(), peer_tag_);
  enter_closed();
}

void SctpAssociation::shutdown() {
  if (state_ == State::Established) {
    state_ = State::ShutdownPending;
    shutdown_wanted_ = true;
  }
}

void SctpAssociation::handle_init(ConstByteView v) {
  if (v.size() < 16 || state_ == State::Established) {
    if (v.size() >= 16 && state_ == State::Established) { /* a restart is not supported: the packet is ignored */ }
    return;
  }
  const uint32_t peer_tag = load_be32(v, 0), rwnd = load_be32(v, 4), peer_tsn = load_be32(v, 12);
  if (peer_tag == 0) return;
  if (my_tag_ == 0) {
    my_tag_ = random32() | 1;
    init_tsn_ = random32();
    next_tsn_ = init_tsn_;
  }
  ByteBuffer cookie(allocator_), reply(allocator_);
  put_be32(&cookie, peer_tag);
  put_be32(&cookie, my_tag_);
  put_be32(&cookie, peer_tsn);
  put_be32(&cookie, init_tsn_);
  put_be32(&cookie, rwnd);
  put_be32(&cookie, 0);
  uint8_t mac[20];
  crypto::hmac(crypto::HashKind::Sha1, ConstByteView(secret_, 20), cookie.view(), mac);
  cookie.append(ConstByteView(mac, 20));
  put_be32(&reply, my_tag_);
  put_be32(&reply, kReceiveWindow);
  put_be16(&reply, 65535);
  put_be16(&reply, 65535);
  put_be32(&reply, init_tsn_);
  put_be16(&reply, 7);   // state cookie
  put_be16(&reply, static_cast<uint32_t>(4 + cookie.size()));
  reply.append(cookie.view());
  reply.put_zeros(padded(cookie.size()) - cookie.size());
  put_be16(&reply, 0xC000);
  put_be16(&reply, 4);
  put_be16(&reply, 0x8008);
  put_be16(&reply, 6);
  reply.put8(kForwardTsn);
  reply.put8(kReconfig);
  reply.put_zeros(2);
  queue_control(kInitAck, 0, reply.view(), peer_tag);
}

void SctpAssociation::handle_init_ack(ConstByteView v) {
  if (state_ != State::CookieWait || v.size() < 16) return;
  const uint32_t peer_tag = load_be32(v, 0), rwnd = load_be32(v, 4), peer_tsn = load_be32(v, 12);
  if (peer_tag == 0) return;
  uint64_t at = 16;
  bool found = false;
  while (v.size() - at >= 4) {
    const uint16_t type = load_be16(v, at), len = load_be16(v, at + 2);
    if (len < 4 || len > v.size() - at) return;
    if (type == 7) {
      cookie_.clear();
      cookie_.append(v.subview(at + 4, len - 4));
      found = true;
    }
    at += padded(len);
  }
  if (!found) return;
  peer_tag_ = peer_tag;
  cum_tsn_ = peer_tsn - 1;
  peer_rwnd_ = rwnd;
  state_ = State::CookieEchoed;
  t1_attempts_ = 1;
  t1_at_ = now_ms_ + 1000;
  send_cookie_echo();
}

void SctpAssociation::establish(uint32_t peer_tag, uint32_t peer_tsn, uint32_t rwnd) {
  peer_tag_ = peer_tag;
  cum_tsn_ = peer_tsn - 1;
  peer_rwnd_ = rwnd;
  cwnd_ = 4 * kMtu;
  ssthresh_ = rwnd ? rwnd : kReceiveWindow;
  next_tsn_ = init_tsn_;
  cum_acked_ = init_tsn_ - 1;
  state_ = State::Established;
  t1_at_ = 0;
}

void SctpAssociation::handle_cookie_echo(ConstByteView v) {
  if (v.size() != kCookieSize) return;
  uint8_t mac[20];
  crypto::hmac(crypto::HashKind::Sha1, ConstByteView(secret_, 20), v.first(24), mac);
  if (!crypto::equal_constant_time(mac, v.data() + 24, 20) || load_be32(v, 4) != my_tag_) return;
  if (state_ == State::Closed || state_ == State::CookieWait || state_ == State::CookieEchoed) {
    if (state_ != State::Closed) init_tsn_ = load_be32(v, 12);
    establish(load_be32(v, 0), load_be32(v, 8), load_be32(v, 16));
  }
  queue_control(kCookieAck, 0, ConstByteView(), peer_tag_);
}

// --- Data in ---------------------------------------------------------------------------------------------------------------------------

void SctpAssociation::handle_data(uint8_t flags, ConstByteView v) {
  if (v.size() < 13) return;
  const uint32_t tsn = load_be32(v, 0);
  if (tsn_le(tsn, cum_tsn_)) {
    if (dups_.count() < 16) dups_.push_back(tsn);
    return;
  }
  if (static_cast<uint32_t>(tsn - cum_tsn_) > kMaxGap) return;
  uint64_t index = 0;
  while (index < gaps_.count() && tsn_lt(gaps_[index], tsn)) ++index;
  if (index < gaps_.count() && gaps_[index] == tsn) {
    if (dups_.count() < 16) dups_.push_back(tsn);
    return;
  }
  if (buffered_in_ + v.size() > kReceiveWindow) return;   // (no room: not acknowledged, the peer sends it again)
  UniquePointer<In> c = UniquePointer<In>::make(allocator_, allocator_);
  if (!c || !c->data.append(v.subview(12))) return;
  c->tsn = tsn;
  c->stream = load_be16(v, 4);
  c->ssn = load_be16(v, 6);
  c->ppid = load_be32(v, 8);
  c->flags = flags;
  uint64_t at = 0;
  while (at < in_.count() && tsn_lt(in_[at]->tsn, tsn)) ++at;
  buffered_in_ += static_cast<uint32_t>(c->data.size());
  in_.insert(at, util::move(c));
  gaps_.insert(index, tsn);
  while (gaps_.count() > 0 && gaps_[0] == cum_tsn_ + 1) {
    ++cum_tsn_;
    gaps_.remove_at(0);
  }
  deliver();
}

void SctpAssociation::deliver() {
  for (bool progress = true; progress;) {
    progress = false;
    for (uint64_t i = 0; i < in_.count() && !progress; ++i) {
      const In& first = *in_[i];
      if (!(first.flags & 2)) continue;   // (a message starts at a B chunk)
      const bool unordered = first.flags & 4;
      if (!unordered && stream_state(first.stream).next_in_ssn != first.ssn) continue;
      uint64_t last = i;
      bool complete = first.flags & 1;
      while (!complete && last + 1 < in_.count()) {
        const In& next = *in_[last + 1];
        if (next.tsn != in_[last]->tsn + 1 || next.stream != first.stream || (next.flags & 2) || ((next.flags & 4) != (first.flags & 4)) || (!unordered && next.ssn != first.ssn)) break;
        ++last;
        complete = next.flags & 1;
      }
      if (!complete) continue;
      UniquePointer<SctpMessage> m = UniquePointer<SctpMessage>::make(allocator_, allocator_);
      if (!m) return;
      m->stream = first.stream;
      m->ppid = first.ppid;
      for (uint64_t k = i; k <= last; ++k) m->data.append(in_[k]->data.view());
      if (!unordered) ++stream_state(first.stream).next_in_ssn;
      for (uint64_t k = last + 1; k-- > i;) {
        buffered_in_ -= static_cast<uint32_t>(in_[k]->data.size());
        in_.remove_at(k);
      }
      if (inbox_.count() < 1024) inbox_.push_back(util::move(m));
      progress = true;
    }
  }
}

bool SctpAssociation::receive_message(UniquePointer<SctpMessage>* out) {
  if (inbox_.count() == 0) return false;
  *out = util::move(inbox_[0]);
  inbox_.remove_at(0);
  return true;
}

void SctpAssociation::send_sack() {
  ByteBuffer v(allocator_), blocks(allocator_);
  uint32_t nblocks = 0;
  for (uint64_t i = 0; i < gaps_.count() && nblocks < 64;) {
    uint64_t j = i;
    while (j + 1 < gaps_.count() && gaps_[j + 1] == gaps_[j] + 1) ++j;
    put_be16(&blocks, gaps_[i] - cum_tsn_);
    put_be16(&blocks, gaps_[j] - cum_tsn_);
    ++nblocks;
    i = j + 1;
  }
  put_be32(&v, cum_tsn_);
  put_be32(&v, buffered_in_ < kReceiveWindow ? kReceiveWindow - buffered_in_ : 0);
  put_be16(&v, nblocks);
  put_be16(&v, static_cast<uint32_t>(dups_.count()));
  v.append(blocks.view());
  for (const uint32_t d : dups_) put_be32(&v, d);
  dups_.clear();
  queue_control(kSack, 0, v.view(), peer_tag_);
}

void SctpAssociation::handle_forward_tsn(ConstByteView v) {
  if (v.size() < 4) return;
  const uint32_t new_cum = load_be32(v, 0);
  if (tsn_lt(cum_tsn_, new_cum)) {
    while (gaps_.count() > 0 && tsn_le(gaps_[0], new_cum)) gaps_.remove_at(0);
    for (uint64_t i = in_.count(); i-- > 0;)
      if (tsn_le(in_[i]->tsn, new_cum)) {
        buffered_in_ -= static_cast<uint32_t>(in_[i]->data.size());
        in_.remove_at(i);
      }
    cum_tsn_ = new_cum;
    while (gaps_.count() > 0 && gaps_[0] == cum_tsn_ + 1) {
      ++cum_tsn_;
      gaps_.remove_at(0);
    }
  }
  for (uint64_t at = 4; at + 4 <= v.size(); at += 4) {
    StreamState& s = stream_state(load_be16(v, at));
    const uint16_t next = static_cast<uint16_t>(load_be16(v, at + 2) + 1);
    if (static_cast<int16_t>(next - s.next_in_ssn) > 0) s.next_in_ssn = next;
  }
  // chunks of the skipped messages are gone: later ones in the same stream can come out now
  for (uint64_t i = in_.count(); i-- > 0;) {
    const In& c = *in_[i];
    if (!(c.flags & 4) && static_cast<int16_t>(c.ssn - stream_state(c.stream).next_in_ssn) < 0) {
      buffered_in_ -= static_cast<uint32_t>(c.data.size());
      in_.remove_at(i);
    }
  }
  deliver();
}

// --- Acknowledgements ----------------------------------------------------------------------------------------------------------------

void SctpAssociation::handle_sack(ConstByteView v) {
  if (v.size() < 12) return;
  const uint32_t cum = load_be32(v, 0), rwnd = load_be32(v, 4), gaps = load_be16(v, 8), dups = load_be16(v, 10);
  if (v.size() < 12 + uint64_t{gaps} * 4 + uint64_t{dups} * 4) return;
  process_acks(cum, rwnd, v.subview(12, uint64_t{gaps} * 4), gaps);
}

void SctpAssociation::process_acks(uint32_t cum, uint32_t rwnd, ConstByteView gap_blocks, uint32_t gap_count) {
  if (tsn_lt(cum, cum_acked_) || tsn_le(next_tsn_, cum)) return;   // (old, or acknowledging what was never sent)
  uint32_t acked_bytes = 0;
  bool progressed = tsn_lt(cum_acked_, cum);
  for (uint64_t i = 0; i < out_.count();) {
    Out& o = *out_[i];
    if (tsn_le(o.tsn, cum)) {
      if (!o.acked && !o.abandoned) {
        acked_bytes += static_cast<uint32_t>(o.data.size());
        if (o.transmissions == 1 && o.sent) {   // an RTT sample (RFC 4960 6.3.1)
          const uint64_t r = now_ms_ - o.sent_ms;
          if (!have_rtt_) {
            srtt_ms_ = r;
            rttvar_ms_ = r / 2;
            have_rtt_ = true;
          } else {
            const uint64_t d = srtt_ms_ > r ? srtt_ms_ - r : r - srtt_ms_;
            rttvar_ms_ = (3 * rttvar_ms_ + d) / 4;
            srtt_ms_ = (7 * srtt_ms_ + r) / 8;
          }
          rto_ms_ = srtt_ms_ + 4 * rttvar_ms_;
          if (rto_ms_ < 200) rto_ms_ = 200;
          if (rto_ms_ > 8000) rto_ms_ = 8000;
        }
        if (queued_bytes_ >= o.data.size()) queued_bytes_ -= static_cast<uint32_t>(o.data.size());
      }
      out_.remove_at(i);
    } else {
      ++i;
    }
  }
  cum_acked_ = cum;
  uint32_t highest = cum;
  for (uint32_t g = 0; g < gap_count; ++g) {
    const uint32_t start = cum + load_be16(gap_blocks, g * 4), end = cum + load_be16(gap_blocks, g * 4 + 2);
    for (const UniquePointer<Out>& o : out_) {
      if (tsn_le(start, o->tsn) && tsn_le(o->tsn, end) && !o->acked && !o->abandoned) {
        o->acked = true;
        acked_bytes += static_cast<uint32_t>(o->data.size());
        if (queued_bytes_ >= o->data.size()) queued_bytes_ -= static_cast<uint32_t>(o->data.size());
        progressed = true;
      }
    }
    if (tsn_lt(highest, end)) highest = end;
  }
  bool fast = false;
  for (const UniquePointer<Out>& o : out_) {   // fast retransmit: three reports of a gap before this TSN (RFC 4960 7.2.4)
    if (o->acked || o->abandoned || !o->sent || !tsn_lt(o->tsn, highest) || o->retransmit) continue;
    if (++o->misses >= 3) {
      o->retransmit = true;
      o->misses = 0;
      fast = true;
    }
  }
  if (fast) {
    ssthresh_ = cwnd_ / 2 > 4 * kMtu ? cwnd_ / 2 : 4 * kMtu;
    cwnd_ = ssthresh_;
  } else if (progressed && acked_bytes > 0) {
    if (cwnd_ <= ssthresh_) {
      cwnd_ += acked_bytes < kMtu ? acked_bytes : kMtu;
    } else {
      partial_acked_ += acked_bytes;
      if (partial_acked_ >= cwnd_) {
        partial_acked_ -= cwnd_;
        cwnd_ += kMtu;
      }
    }
  }
  const uint32_t flight = flight_size();
  peer_rwnd_ = rwnd > flight ? rwnd - flight : 0;
  bool outstanding = false;
  for (const UniquePointer<Out>& o : out_) outstanding = outstanding || (o->sent && !o->acked && !o->abandoned);
  if (!outstanding) t3_at_ = 0;
  else if (progressed) t3_at_ = now_ms_ + rto_ms_;
  if (progressed) t3_failures_ = 0;
  for (const UniquePointer<Out>& o : out_)
    if (o->abandoned) forward_pending_ = true;
  if (forward_pending_ && tsn_le(forward_tsn_sent_, cum_acked_) && forward_tsn_sent_ != 0) forward_pending_ = true;
}

void SctpAssociation::abandon(const Out& chunk) {
  for (const UniquePointer<Out>& o : out_)
    if (o->message == chunk.message && !o->acked) {
      if (queued_bytes_ >= o->data.size() && !o->abandoned) queued_bytes_ -= static_cast<uint32_t>(o->data.size());
      o->abandoned = true;
      o->retransmit = false;
    }
  forward_pending_ = true;
}

// --- Timers ---------------------------------------------------------------------------------------------------------------------------

void SctpAssociation::tick(uint64_t now) {
  now_ms_ = now;
  if ((state_ == State::CookieWait || state_ == State::CookieEchoed) && t1_at_ && now >= t1_at_) {
    if (++t1_attempts_ > 8) {
      enter_closed();
      return;
    }
    t1_at_ = now + (1000u << (t1_attempts_ < 4 ? t1_attempts_ - 1 : 3));
    if (state_ == State::CookieWait) send_init();
    else send_cookie_echo();
    return;
  }
  if (state_ != State::Established && state_ != State::ShutdownPending && state_ != State::ShutdownReceived) {
    if (state_ == State::ShutdownSent && now - shutdown_sent_ms_ >= 1000) {
      shutdown_sent_ms_ = now;
      ByteBuffer v(allocator_);
      put_be32(&v, cum_tsn_);
      queue_control(kShutdown, 0, v.view(), peer_tag_);
    }
    return;
  }
  for (const UniquePointer<Out>& o : out_) {   // partial reliability: messages given up on
    if (o->acked || o->abandoned) continue;
    if ((o->expires_ms && now >= o->expires_ms && o->sent) || (o->max_rtx >= 0 && o->transmissions > static_cast<uint32_t>(o->max_rtx) && o->sent && o->retransmit)) {
      abandon(*o);
      break;
    }
  }
  if (t3_at_ && now >= t3_at_) {
    if (++t3_failures_ > 10) {
      abort();
      return;
    }
    rto_ms_ = rto_ms_ * 2 > 8000 ? 8000 : rto_ms_ * 2;
    ssthresh_ = cwnd_ / 2 > 4 * kMtu ? cwnd_ / 2 : 4 * kMtu;
    cwnd_ = kMtu;
    for (const UniquePointer<Out>& o : out_)
      if (o->sent && !o->acked && !o->abandoned) o->retransmit = true;
    t3_at_ = now + rto_ms_;
    if (forward_tsn_sent_ != 0) forward_pending_ = true;
    for (const UniquePointer<Out>& o : out_)
      if (o->abandoned) {
        forward_pending_ = true;
        break;
      }
    for (const UniquePointer<Out>& o : out_)   // those over their retransmission limit are given up now
      if (!o->acked && !o->abandoned && o->max_rtx >= 0 && o->transmissions > static_cast<uint32_t>(o->max_rtx)) {
        abandon(*o);
        break;
      }
  }
  // stream resets, once what was queued on the streams is through
  if (reset_inflight_.count() > 0 && now - reset_sent_ms_ >= 1000) {   // (asked again)
    ByteBuffer p(allocator_);
    put_be16(&p, 13);
    put_be16(&p, static_cast<uint32_t>(16 + 2 * reset_inflight_.count()));
    put_be32(&p, pending_reset_seq_);
    put_be32(&p, peer_reset_seq_);
    put_be32(&p, next_tsn_ - 1);
    for (const uint16_t s : reset_inflight_) put_be16(&p, s);
    p.put_zeros(padded(p.size()) - p.size());
    queue_control(kReconfig, 0, p.view(), peer_tag_);
    reset_sent_ms_ = now;
  } else if (reset_wanted_.count() > 0 && reset_inflight_.count() == 0) {
    bool clear = true;
    for (const UniquePointer<Out>& o : out_)
      for (const uint16_t s : reset_wanted_) clear = clear && o->stream != s;
    if (clear) {
      for (const uint16_t s : reset_wanted_) reset_inflight_.push_back(s);
      reset_wanted_.clear();
      pending_reset_seq_ = ++reset_seq_ + 0x1000;
      reset_sent_ms_ = 0;
      tick(now);
    }
  }
  if (shutdown_wanted_ && out_.count() == 0 && state_ == State::ShutdownPending) {
    state_ = State::ShutdownSent;
    shutdown_sent_ms_ = now;
    ByteBuffer v(allocator_);
    put_be32(&v, cum_tsn_);
    queue_control(kShutdown, 0, v.view(), peer_tag_);
  }
}

bool SctpAssociation::reset_stream(uint16_t stream) {
  if (state_ != State::Established) return false;
  for (const uint16_t s : reset_wanted_)
    if (s == stream) return true;
  for (const uint16_t s : reset_inflight_)
    if (s == stream) return true;
  return reset_wanted_.push_back(stream);
}

bool SctpAssociation::take_reset(uint16_t* stream) {
  if (resets_taken_.count() == 0) return false;
  *stream = resets_taken_[0];
  resets_taken_.remove_at(0);
  return true;
}

void SctpAssociation::handle_reconfig(ConstByteView v) {
  uint64_t at = 0;
  while (v.size() - at >= 4) {
    const uint16_t type = load_be16(v, at), len = load_be16(v, at + 2);
    if (len < 4 || len > v.size() - at) return;
    const ConstByteView body = v.subview(at + 4, len - 4);
    if (type == 13 && body.size() >= 12) {   // outgoing SSN reset request
      const uint32_t seq = load_be32(body, 0), last_tsn = load_be32(body, 8);
      uint32_t result = 6;   // in progress: data is still on its way
      if (seq == peer_reset_seq_ || (peer_reset_seq_ != 0 && seq == peer_reset_seq_)) {
        result = 1;
      } else if (tsn_le(last_tsn, cum_tsn_)) {
        for (uint64_t k = 12; k + 2 <= body.size(); k += 2) {
          const uint16_t sid = load_be16(body, k);
          stream_state(sid).next_in_ssn = 0;
          resets_taken_.push_back(sid);
        }
        peer_reset_seq_ = seq;
        result = 1;
      }
      ByteBuffer p(allocator_);
      put_be16(&p, 16);
      put_be16(&p, 12);
      put_be32(&p, seq);
      put_be32(&p, result);
      queue_control(kReconfig, 0, p.view(), peer_tag_);
    } else if (type == 16 && body.size() >= 8) {   // the answer to ours
      if (load_be32(body, 0) == pending_reset_seq_ && reset_inflight_.count() > 0) {
        const uint32_t result = load_be32(body, 4);
        if (result == 0 || result == 1) {
          for (const uint16_t s : reset_inflight_) stream_state(s).next_out_ssn = 0;
          reset_inflight_.clear();
        } else if (result == 6 || result == 4) {
          reset_sent_ms_ = now_ms_;   // (asked again a little later)
        } else {
          reset_inflight_.clear();
        }
      }
    }
    at += padded(len);
  }
}

}  // namespace rtc
