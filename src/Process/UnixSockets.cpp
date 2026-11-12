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
#include "Process/UnixSockets.hpp"

namespace process {

namespace {

struct Hold {
  ISystemSync& sync;
  explicit Hold(ISystemSync& s) : sync(s) { sync.lock(); }
  ~Hold() { sync.unlock(); }
};

bool is_message_type(UnixType type) { return type != UnixType::Stream; }

}  // namespace

UnixSocketManager::UnixSocketManager(ISystemSync& sync, allocators::IAllocator& allocator) : sync_(sync), allocator_(allocator), dropped_(allocator) {}

UnixSocketManager::~UnixSocketManager() {
  for (UniquePointer<Socket>& slot : slots_) slot.reset();      // (whatever descriptors their queues held go with them unreleased: only at shutdown)
}

UnixSocketManager::Socket* UnixSocketManager::at(uint32_t id) const {
  const uint32_t index = (id & 0xFFFFu);
  if (index == 0 || index > kCapacity) return nullptr;
  Socket* s = slots_[index - 1].get();
  return s && s->id == id ? s : nullptr;
}

UnixStatus UnixSocketManager::make_locked(UnixType type, uint64_t owner_pid, uint32_t* id) {
  for (uint32_t tried = 0; tried < kCapacity; ++tried) {
    const uint32_t index = (next_free_ + tried) % kCapacity;
    if (slots_[index]) continue;
    UniquePointer<Socket> s = UniquePointer<Socket>::make(allocator_, allocator_);
    if (!s) return UnixStatus::NoMemory;
    generations_[index] = uint16_t(generations_[index] + 1 == 0 ? 1 : generations_[index] + 1);      // (never 0: an id is never 0)
    s->id = (uint32_t(generations_[index]) << 16) | (index + 1);
    s->type = type;
    s->owner_pid = owner_pid;
    s->references = 1;
    *id = s->id;
    slots_[index] = static_cast<UniquePointer<Socket>&&>(s);
    next_free_ = (index + 1) % kCapacity;
    return UnixStatus::Ok;
  }
  return UnixStatus::NoSockets;
}

UnixStatus UnixSocketManager::create(UnixType type, uint64_t owner_pid, uint32_t* id) {
  Hold hold(sync_);
  return make_locked(type, owner_pid, id);
}

UnixStatus UnixSocketManager::create_event(uint64_t owner_pid, uint64_t initial, bool semaphore, uint32_t* id) {
  if (initial > kEventMax) return UnixStatus::Invalid;
  Hold hold(sync_);
  const UnixStatus st = make_locked(UnixType::Event, owner_pid, id);
  if (st != UnixStatus::Ok) return st;
  Socket* s = at(*id);
  s->counter = initial;
  s->semaphore = semaphore;
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::create_timer(uint64_t owner_pid, uint32_t clock, uint32_t* id) {
  Hold hold(sync_);
  const UnixStatus st = make_locked(UnixType::Timer, owner_pid, id);
  if (st == UnixStatus::Ok) at(*id)->timer_clock = clock;
  return st;
}

UnixStatus UnixSocketManager::create_epoll(uint64_t owner_pid, uint32_t* id) {
  Hold hold(sync_);
  return make_locked(UnixType::Epoll, owner_pid, id);
}

UnixStatus UnixSocketManager::create_signal(uint64_t owner_pid, uint32_t* id) {
  Hold hold(sync_);
  return make_locked(UnixType::Signal, owner_pid, id);
}

bool UnixSocketManager::timer_state_locked(uint32_t id, uint32_t* clock, uint64_t* deadline, uint64_t* interval) const {
  const Socket* s = at(id);
  if (!s || s->type != UnixType::Timer) return false;
  *clock = s->timer_clock;
  *deadline = s->timer_deadline;
  *interval = s->timer_interval;
  return true;
}

UnixStatus UnixSocketManager::timer_arm_locked(uint32_t id, uint64_t deadline, uint64_t interval) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type != UnixType::Timer) return UnixStatus::WrongType;
  s->timer_deadline = deadline;
  s->timer_interval = deadline ? interval : 0;
  s->counter = 0;
  sync_.notify_all();      // (waiters look again at when it is due)
  return UnixStatus::Ok;
}

void UnixSocketManager::timer_advance_locked(uint32_t id, uint64_t now) {
  Socket* s = at(id);
  if (!s || s->type != UnixType::Timer || s->timer_deadline == 0 || now < s->timer_deadline) return;
  uint64_t fired = 1;
  if (s->timer_interval) {
    fired += (now - s->timer_deadline) / s->timer_interval;
    const uint64_t step = fired > ~uint64_t{0} / s->timer_interval ? ~uint64_t{0} : fired * s->timer_interval;
    s->timer_deadline = step > ~uint64_t{0} - s->timer_deadline ? 0 : s->timer_deadline + step;      // (one that would pass 2^64 ns stops)
  } else {
    s->timer_deadline = 0;
  }
  s->counter = fired > ~uint64_t{0} - s->counter ? ~uint64_t{0} : s->counter + fired;
  sync_.notify_all();
}

UnixStatus UnixSocketManager::timer_take_locked(uint32_t id, uint64_t* expirations) {
  *expirations = 0;
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type != UnixType::Timer) return UnixStatus::WrongType;
  if (s->counter == 0) return UnixStatus::WouldBlock;
  *expirations = s->counter;
  s->counter = 0;
  return UnixStatus::Ok;
}

List<UnixEpollEntry>* UnixSocketManager::epoll_interest_locked(uint32_t id) {
  Socket* s = at(id);
  return s && s->type == UnixType::Epoll ? &s->interest : nullptr;
}

void UnixSocketManager::epoll_changed_locked(uint32_t id) {
  Socket* s = at(id);
  if (!s || s->type != UnixType::Epoll) return;
  ++s->counter;
  sync_.notify_all();
}

uint64_t UnixSocketManager::epoll_changes_locked(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->type == UnixType::Epoll ? s->counter : 0;
}

bool UnixSocketManager::is_event_locked(uint32_t id) const {
  const Socket* s = at(id);
  return s && s->type == UnixType::Event;
}

UnixStatus UnixSocketManager::event_add_locked(uint32_t id, uint64_t value) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type != UnixType::Event) return UnixStatus::WrongType;
  if (value == ~uint64_t{0}) return UnixStatus::Invalid;
  if (value > kEventMax - s->counter) return UnixStatus::WouldBlock;
  if (value == 0) return UnixStatus::Ok;
  s->counter += value;
  sync_.notify_all();
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::event_take_locked(uint32_t id, uint64_t* value) {
  *value = 0;
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type != UnixType::Event) return UnixStatus::WrongType;
  if (s->counter == 0) return UnixStatus::WouldBlock;
  *value = s->semaphore ? 1 : s->counter;
  s->counter -= *value;
  sync_.notify_all();
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::create_pair(UnixType type, uint64_t owner_pid, uint32_t* first, uint32_t* second) {
  Queue<UnixInFlight> dropped(allocator_);
  UnixStatus status;
  {
    Hold hold(sync_);
    status = make_locked(type, owner_pid, first);
    if (status == UnixStatus::Ok) {
      status = make_locked(type, owner_pid, second);
      if (status != UnixStatus::Ok) {
        release_locked(*first);
      } else {
        Socket* a = at(*first);
        Socket* b = at(*second);
        a->peer = b->id;
        b->peer = a->id;
        a->peer_pid = b->peer_pid = owner_pid;
        if (type != UnixType::Datagram) {
          a->state = b->state = State::Connected;
          a->was_connected = b->was_connected = true;
        }
      }
    }
    take_dropped_locked(&dropped);
  }
  release_dropped(dropped);
  return status;
}

void UnixSocketManager::add_ref(uint32_t id) {
  Hold hold(sync_);
  if (Socket* s = at(id)) ++s->references;
}

bool UnixSocketManager::exists(uint32_t id) {
  Hold hold(sync_);
  return at(id) != nullptr;
}

void UnixSocketManager::release(uint32_t id) {
  Queue<UnixInFlight> dropped(allocator_);
  {
    Hold hold(sync_);
    release_locked(id);
    take_dropped_locked(&dropped);
  }
  release_dropped(dropped);
}

void UnixSocketManager::release_locked(uint32_t id) {
  Socket* s = at(id);
  if (!s || --s->references != 0) return;
  // Connections that were never accepted: their clients see the connection reset.
  uint32_t pending = 0;
  while (s->backlog.dequeue(&pending)) {
    if (Socket* server_side = at(pending)) {
      if (Socket* client = at(server_side->peer)) { client->peer_gone = true; client->pending_error = UnixStatus::Reset; }
      server_side->peer = 0;
      release_locked(pending);
    }
  }
  if (s->type != UnixType::Datagram) {
    if (Socket* peer = at(s->peer); peer && peer->peer == s->id) {
      peer->peer_gone = true;
      if (s->queued() != 0) peer->pending_error = UnixStatus::Reset;      // (it went away with data unread: the other end hears a reset, as on Linux)
    }
  }
  UnixInFlight carried;
  while (s->rights.dequeue(&carried)) dropped_.enqueue(static_cast<UnixInFlight&&>(carried));
  const uint32_t index = (id & 0xFFFFu) - 1;
  slots_[index].reset();
  sync_.notify_all();
}

void UnixSocketManager::take_dropped_locked(Queue<UnixInFlight>* out) {
  UnixInFlight entry;
  while (dropped_.dequeue(&entry)) out->enqueue(static_cast<UnixInFlight&&>(entry));
}

void UnixSocketManager::release_dropped(Queue<UnixInFlight>& dropped) {
  UnixInFlight entry;
  while (dropped.dequeue(&entry)) {
    if (entry.token != 0) descriptor_token_release(entry.kind, entry.token);      // (a socket in there comes back here through release())
    entry.object = SharedPointer<OpenObject>();
  }
}

bool UnixSocketManager::type_locked(uint32_t id, UnixType* type) const {
  const Socket* s = at(id);
  if (!s) return false;
  *type = s->type;
  return true;
}

UnixSocketManager::Socket* UnixSocketManager::find_bound_locked(const UnixName& name, StringView host_path) const {
  Socket* newest = nullptr;      // (a file removed and bound again: the latest binding owns the name)
  for (const UniquePointer<Socket>& slot : slots_) {
    Socket* s = slot.get();
    if (!s || !s->bound) continue;
    if (name.kind == UnixName::Kind::Path) {
      if (!host_path.empty() && s->host_path.view() == host_path && (!newest || s->id > newest->id)) newest = s;
    } else if (name.kind == UnixName::Kind::Abstract && s->name.same_as(name)) {
      return s;
    }
  }
  return newest;
}

bool UnixSocketManager::path_bound_locked(StringView host_path) const {
  UnixName any;
  any.kind = UnixName::Kind::Path;
  return find_bound_locked(any, host_path) != nullptr;
}

bool UnixSocketManager::path_bound(StringView host_path) {
  Hold hold(sync_);
  return path_bound_locked(host_path);
}

UnixStatus UnixSocketManager::bind_locked(uint32_t id, const UnixName& name, StringView host_path) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->bound) return UnixStatus::Invalid;      // (already has a name)
  UnixName chosen = name;
  if (name.kind == UnixName::Kind::Unnamed) {      // autobind: an abstract name of five hex digits nobody has
    for (uint32_t tries = 0;; ++tries) {
      if (tries > 0xFFFFFu) return UnixStatus::AddressInUse;
      const uint32_t n = autobind_counter_++ & 0xFFFFFu;
      uint8_t digits[5];
      for (uint32_t i = 0; i < 5; ++i) { const uint32_t d = (n >> (4 * (4 - i))) & 0xF; digits[i] = uint8_t(d < 10 ? '0' + d : 'a' + d - 10); }
      chosen = UnixName::from(UnixName::Kind::Abstract, ConstByteView(digits, 5));
      if (!find_bound_locked(chosen, StringView())) break;
    }
  } else if (name.kind == UnixName::Kind::Abstract) {
    if (find_bound_locked(name, StringView())) return UnixStatus::AddressInUse;
  } else if (host_path.empty()) {
    return UnixStatus::Invalid;
  }
  if (name.kind == UnixName::Kind::Path && !s->host_path.assign(host_path)) return UnixStatus::NoMemory;
  s->name = chosen;
  s->bound = true;
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::listen_locked(uint32_t id, uint32_t backlog) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type == UnixType::Datagram) return UnixStatus::NotSupported;
  if (s->state == State::Connected) return UnixStatus::Invalid;
  if (!s->bound) return UnixStatus::Invalid;      // (Linux: EINVAL for listen() before bind())
  s->state = State::Listening;
  s->backlog_limit = backlog == 0 ? 1 : backlog > kMaxBacklog ? kMaxBacklog : backlog;
  sync_.notify_all();
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::connect_locked(uint32_t id, const UnixName& name, StringView host_path, uint64_t pid, bool disconnect) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type == UnixType::Datagram) {
    if (disconnect) { s->peer = 0; s->peer_pid = 0; s->peer_name = UnixName(); return UnixStatus::Ok; }
    Socket* target = find_bound_locked(name, host_path);
    if (!target) return UnixStatus::Refused;
    if (target->type != UnixType::Datagram) return UnixStatus::WrongType;
    s->peer = target->id;
    s->peer_pid = target->owner_pid;
    s->peer_name = target->name;
    return UnixStatus::Ok;
  }
  if (disconnect) return UnixStatus::Invalid;
  if (s->state == State::Connected) return UnixStatus::AlreadyConnected;
  if (s->state == State::Listening) return UnixStatus::Invalid;
  Socket* listener = find_bound_locked(name, host_path);
  if (!listener) return UnixStatus::Refused;
  if (listener->type != s->type) return UnixStatus::WrongType;
  if (listener->state != State::Listening) return UnixStatus::Refused;
  if (listener->backlog.count() >= listener->backlog_limit) return UnixStatus::WouldBlock;
  uint32_t server_id = 0;
  const UnixStatus made = make_locked(s->type, listener->owner_pid, &server_id);
  if (made != UnixStatus::Ok) return made;
  Socket* server_side = at(server_id);      // (the sockets do not move: `s` and `listener` still point at theirs)
  if (!listener->backlog.enqueue(server_id)) { release_locked(server_id); return UnixStatus::NoMemory; }
  server_side->name = listener->name;      // (getsockname() of an accepted socket reports the listener's name)
  server_side->bound = false;              // (but it does not own that name)
  server_side->peer = s->id;
  server_side->peer_pid = pid;
  server_side->peer_name = s->name;
  server_side->state = State::Connected;
  server_side->was_connected = true;
  server_side->pass_credentials = listener->pass_credentials;
  s->peer = server_id;
  s->peer_pid = listener->owner_pid;
  s->peer_name = listener->name;
  s->state = State::Connected;
  s->was_connected = true;
  sync_.notify_all();
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::accept_locked(uint32_t id, uint32_t* out_id) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type == UnixType::Datagram) return UnixStatus::NotSupported;
  if (s->state != State::Listening) return UnixStatus::Invalid;
  uint32_t taken = 0;
  if (!s->backlog.dequeue(&taken)) return UnixStatus::WouldBlock;
  *out_id = taken;      // (the use the backlog kept is now the caller's)
  sync_.notify_all();   // (a connect() waiting for room in the backlog)
  return UnixStatus::Ok;
}

bool UnixSocketManager::room_for(const Socket& receiver, uint64_t bytes, bool whole) const {
  if (receiver.records.count() >= kMaxRecords) return false;
  const uint64_t queued = receiver.queued();
  if (queued >= kBufferBytes) return false;
  return !whole || bytes <= kBufferBytes - queued;
}

bool UnixSocketManager::enqueue_locked(Socket& r, ConstByteView data, Queue<UnixInFlight>* rights, uint64_t sender_pid, const UnixName& from, bool merge) {
  // The bytes go after what is queued; the part already read is dropped first when it is most of the buffer (so the buffer does not grow without end).
  if (r.data_head != 0 && (r.data_head >= r.data.size() / 2 || r.data.size() + data.size() > kBufferBytes * 2)) {
    const uint64_t keep = r.queued();
    ByteView all = r.data.writable_view();
    all.first(keep).copy_from(ConstByteView(all.subview(r.data_head, keep)));      // (memmove: the ranges may overlap)
    r.data.truncate(keep);
    r.data_head = 0;
  }
  const uint32_t carried = rights ? uint32_t(rights->count()) : 0;
  if (carried != 0 && !r.rights.reserve(r.rights.count() + carried)) return false;      // (room first: once queued, an entry must not be lost)
  Record* last = r.records.back();
  const bool merged = merge && carried == 0 && last && last->rights == 0 && last->sender_pid == sender_pid;      // (a stream: plain bytes after plain bytes stay one segment)
  if (!merged && r.records.count() >= kMaxRecords) return false;
  if (!r.data.append(data)) return false;
  if (merged) {
    last->bytes += uint32_t(data.size());
  } else {
    Record record;
    record.bytes = uint32_t(data.size());
    record.rights = carried;
    record.sender_pid = sender_pid;
    record.from = from;
    if (!r.records.enqueue(record)) { r.data.truncate(r.data.size() - data.size()); return false; }
  }
  if (rights) {
    UnixInFlight entry;
    while (rights->dequeue(&entry)) r.rights.enqueue(static_cast<UnixInFlight&&>(entry));      // (cannot fail: reserved above)
  }
  return true;
}

UnixStatus UnixSocketManager::send_locked(uint32_t id, ConstByteView data, const Destination* to, uint64_t pid, Queue<UnixInFlight>* rights, uint32_t* sent) {
  *sent = 0;
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (!unix_type_is_socket(s->type)) return UnixStatus::NotSupported;      // (an eventfd, a timer, ... is not a socket)
  if (rights && rights->count() > kMaxRights) return UnixStatus::Invalid;
  if (s->shut_write) return UnixStatus::Broken;
  if (s->type == UnixType::Datagram) {
    Socket* target = nullptr;
    if (to && to->name) {
      target = find_bound_locked(*to->name, to->host_path);
      if (!target) return UnixStatus::Refused;
    } else {
      if (s->peer == 0) return UnixStatus::NotConnected;
      target = at(s->peer);
      if (!target) return UnixStatus::Refused;      // (the socket it was connected to is gone)
    }
    if (target->type != UnixType::Datagram) return UnixStatus::WrongType;
    if (target->peer != 0 && target->peer != s->id) return UnixStatus::NotPermitted;
    if (target->shut_read) return UnixStatus::Broken;
    if (data.size() > kBufferBytes) return UnixStatus::MessageTooBig;
    if (!room_for(*target, data.size(), true)) return UnixStatus::WouldBlock;
    if (!enqueue_locked(*target, data, rights, pid, s->bound ? s->name : UnixName(), false)) return UnixStatus::NoMemory;
    *sent = uint32_t(data.size());
    sync_.notify_all();
    return UnixStatus::Ok;
  }
  if (to && to->name) return s->state == State::Connected ? UnixStatus::AlreadyConnected : UnixStatus::NotSupported;
  if (s->state != State::Connected) return s->was_connected ? UnixStatus::Broken : UnixStatus::NotConnected;
  Socket* peer = at(s->peer);
  if (!peer || s->peer_gone || peer->shut_read) return UnixStatus::Broken;
  if (s->type == UnixType::SeqPacket) {
    if (data.size() > kBufferBytes) return UnixStatus::MessageTooBig;
    if (!room_for(*peer, data.size(), true)) return UnixStatus::WouldBlock;
    if (!enqueue_locked(*peer, data, rights, pid, UnixName(), false)) return UnixStatus::NoMemory;
    *sent = uint32_t(data.size());
    sync_.notify_all();
    return UnixStatus::Ok;
  }
  if (data.empty()) return UnixStatus::Ok;      // (as on Linux, descriptors need at least one byte to travel with on a stream: they stay the caller's)
  if (!room_for(*peer, 1, false)) return UnixStatus::WouldBlock;
  const uint64_t room = kBufferBytes - peer->queued();
  const uint64_t n = data.size() < room ? data.size() : room;
  if (!enqueue_locked(*peer, data.first(n), rights, pid, UnixName(), true)) return UnixStatus::NoMemory;
  *sent = uint32_t(n);
  sync_.notify_all();
  return UnixStatus::Ok;
}

void UnixSocketManager::consume_locked(Socket& s, uint64_t bytes) {
  s.data_head += bytes;
  if (s.data_head == s.data.size()) { s.data.clear(); s.data_head = 0; }
}

bool UnixSocketManager::reading_ended(const Socket& s) const {
  if (s.shut_read) return true;
  if (s.type == UnixType::Datagram) return false;
  if (s.peer_gone) return true;
  const Socket* peer = at(s.peer);
  return s.state == State::Connected && (!peer || peer->shut_write);
}

UnixStatus UnixSocketManager::receive_locked(uint32_t id, ByteView buffer, bool peek, Received* out, Queue<UnixInFlight>* rights) {
  *out = Received();
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (!unix_type_is_socket(s->type)) return UnixStatus::NotSupported;
  if (s->records.count() == 0) {
    if (s->pending_error != UnixStatus::Ok) {
      const UnixStatus error = s->pending_error;
      if (!peek) s->pending_error = UnixStatus::Ok;
      return error;
    }
    if (s->type != UnixType::Datagram && s->state == State::Listening) return UnixStatus::Invalid;
    if (s->type != UnixType::Datagram && s->state != State::Connected && !s->was_connected) return UnixStatus::Invalid;      // (Linux: EINVAL on a socket never connected)
    if (reading_ended(*s)) { out->end_of_file = true; return UnixStatus::Ok; }
    return UnixStatus::WouldBlock;
  }
  ConstByteView queued = s->data.view().subview(s->data_head, s->queued());
  if (is_message_type(s->type)) {
    Record* record = s->records.front();
    const uint32_t n = record->bytes < buffer.size() ? record->bytes : uint32_t(buffer.size());
    buffer.first(n).copy_from(queued.first(n));
    out->got = n;
    out->message_size = record->bytes;
    out->from = record->from;
    out->sender_pid = record->sender_pid;
    if (peek) return UnixStatus::Ok;
    for (uint32_t i = 0; i < record->rights; ++i) {
      UnixInFlight entry;
      if (!s->rights.dequeue(&entry)) break;
      if (rights) rights->enqueue(static_cast<UnixInFlight&&>(entry));
      else dropped_.enqueue(static_cast<UnixInFlight&&>(entry));
    }
    consume_locked(*s, record->bytes);
    s->records.dequeue();
    sync_.notify_all();
    return UnixStatus::Ok;
  }
  // A stream: across segments, but not past the start of one that carries descriptors once bytes were taken (each lot of descriptors comes with its own bytes).
  uint64_t got = 0, walked = 0;
  ObjectCursor cursor;
  bool any = s->records.first(&cursor);
  uint32_t first_rights = 0;
  bool first_segment = true;
  while (any && got < buffer.size()) {
    Record* record = s->records.element(cursor);
    if (!first_segment && record->rights != 0) break;
    if (first_segment) { first_rights = record->rights; out->sender_pid = record->sender_pid; }
    const uint64_t want = buffer.size() - got;
    const uint64_t n = record->bytes < want ? record->bytes : want;
    buffer.subview(got, n).copy_from(queued.subview(walked, n));
    got += n;
    walked += record->bytes;
    first_segment = false;
    if (n < record->bytes || first_rights != 0) break;      // (as on Linux, a segment that brought descriptors ends the read)
    any = s->records.next(&cursor);
  }
  out->got = uint32_t(got);
  out->message_size = uint32_t(got);
  if (peek) return UnixStatus::Ok;
  for (uint32_t i = 0; i < first_rights; ++i) {
    UnixInFlight entry;
    if (!s->rights.dequeue(&entry)) break;
    if (rights) rights->enqueue(static_cast<UnixInFlight&&>(entry));
    else dropped_.enqueue(static_cast<UnixInFlight&&>(entry));
  }
  if (first_rights != 0) s->records.front()->rights = 0;      // (its descriptors were handed out with these bytes)
  uint64_t left = got;
  while (left > 0) {
    Record* record = s->records.front();
    if (!record) break;
    if (record->bytes > left) { record->bytes -= uint32_t(left); break; }
    left -= record->bytes;
    s->records.dequeue();
  }
  consume_locked(*s, got);
  sync_.notify_all();
  return UnixStatus::Ok;
}

UnixStatus UnixSocketManager::shutdown_locked(uint32_t id, bool reading, bool writing) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  if (s->type != UnixType::Datagram && s->state != State::Connected && !s->was_connected && s->state != State::Listening) return UnixStatus::NotConnected;
  if (reading) s->shut_read = true;
  if (writing) s->shut_write = true;
  sync_.notify_all();
  return UnixStatus::Ok;
}

UnixReadiness UnixSocketManager::readiness_locked(uint32_t id) const {
  UnixReadiness r;
  const Socket* s = at(id);
  if (!s) { r.error = r.hangup = true; return r; }
  if (s->type == UnixType::Event) {
    r.readable = s->counter > 0;
    r.writable = s->counter < kEventMax;
    return r;
  }
  if (s->type == UnixType::Timer) {
    r.readable = s->counter > 0;      // (as of the last timer_advance_locked)
    return r;
  }
  if (!unix_type_is_socket(s->type)) return r;      // (an epoll instance is evaluated by the Linux layer; a signalfd never fires)
  r.error = s->pending_error != UnixStatus::Ok;
  if (s->state == State::Listening) {
    r.readable = s->backlog.count() != 0;
    return r;
  }
  const bool ended = reading_ended(*s);
  r.readable = s->records.count() != 0 || ended || r.error;
  r.read_hangup = ended;
  if (s->type == UnixType::Datagram) {
    const Socket* target = s->peer ? at(s->peer) : nullptr;
    r.writable = !s->shut_write && (!s->peer || !target || room_for(*target, 1, false));
    return r;
  }
  if (s->state != State::Connected) {
    r.writable = false;
    r.hangup = s->was_connected;
    return r;
  }
  const Socket* peer = at(s->peer);
  const bool broken = !peer || s->peer_gone || peer->shut_read || s->shut_write;
  r.writable = broken || room_for(*peer, 1, false);      // (a send that fails with EPIPE does not block either)
  r.hangup = s->peer_gone || (s->shut_write && ended) || (s->shut_read && s->shut_write);
  return r;
}

uint32_t UnixSocketManager::readable_bytes_locked(uint32_t id) const {
  const Socket* s = at(id);
  if (!s || s->records.count() == 0) return 0;
  if (is_message_type(s->type)) return s->records.front()->bytes;
  return uint32_t(s->queued());
}

bool UnixSocketManager::local_name_locked(uint32_t id, UnixName* out) const {
  const Socket* s = at(id);
  if (!s) return false;
  *out = s->name;      // (unnamed until bound; an accepted socket has the listener's)
  return true;
}

bool UnixSocketManager::peer_name_locked(uint32_t id, UnixName* out) const {
  const Socket* s = at(id);
  if (!s || !s->peer) return false;
  if (s->type != UnixType::Datagram && s->state != State::Connected) return false;
  *out = s->peer_name;      // (kept from when it connected: still reported after the other end went, as on Linux)
  return true;
}

bool UnixSocketManager::peer_pid_locked(uint32_t id, uint64_t* pid) const {
  const Socket* s = at(id);
  if (!s || (s->type != UnixType::Datagram && s->state != State::Connected) || !s->peer) return false;
  *pid = s->peer_pid;
  return true;
}

bool UnixSocketManager::is_listening_locked(uint32_t id) const { const Socket* s = at(id); return s && s->state == State::Listening; }
bool UnixSocketManager::is_connected_locked(uint32_t id) const { const Socket* s = at(id); return s && (s->state == State::Connected || (s->type == UnixType::Datagram && s->peer)); }

UnixStatus UnixSocketManager::take_error_locked(uint32_t id) {
  Socket* s = at(id);
  if (!s) return UnixStatus::BadSocket;
  const UnixStatus error = s->pending_error;
  s->pending_error = UnixStatus::Ok;
  return error;
}

bool UnixSocketManager::non_blocking_locked(uint32_t id) const { const Socket* s = at(id); return s && s->non_blocking; }
void UnixSocketManager::set_non_blocking_locked(uint32_t id, bool on) { if (Socket* s = at(id)) s->non_blocking = on; }
bool UnixSocketManager::pass_credentials_locked(uint32_t id) const { const Socket* s = at(id); return s && s->pass_credentials; }
void UnixSocketManager::set_pass_credentials_locked(uint32_t id, bool on) { if (Socket* s = at(id)) s->pass_credentials = on; }
uint64_t UnixSocketManager::timeout_locked(uint32_t id, bool sending) const {
  const Socket* s = at(id);
  return !s ? ~uint64_t{0} : sending ? s->send_timeout : s->receive_timeout;
}
void UnixSocketManager::set_timeout_locked(uint32_t id, bool sending, uint64_t ns) {
  if (Socket* s = at(id)) (sending ? s->send_timeout : s->receive_timeout) = ns == 0 ? ~uint64_t{0} : ns;
}

// --- the system's one manager -------------------------------------------------------------------------------------------------------------------------------

namespace {
UnixSocketManager* g_override = nullptr;
UnixSocketManager* g_made = nullptr;
}  // namespace

UnixSocketManager* system_unix_sockets() {
  if (UnixSocketManager* chosen = __atomic_load_n(&g_override, __ATOMIC_ACQUIRE)) return chosen;
  if (UnixSocketManager* made = __atomic_load_n(&g_made, __ATOMIC_ACQUIRE)) return made;
  ObjectManager* objects = system_objects();
  if (!objects || !objects->allocator()) return nullptr;
  ISystemSync& sync = objects->sync();
  sync.lock();      // (two first users at once: one makes it)
  UnixSocketManager* made = g_made;
  if (!made) {
    allocators::IAllocator& allocator = *objects->allocator();
    if (void* memory = allocator.allocate(sizeof(UnixSocketManager))) {
      made = new (memory) UnixSocketManager(sync, allocator);      // (lives as long as the system does)
      __atomic_store_n(&g_made, made, __ATOMIC_RELEASE);
    }
  }
  sync.unlock();
  return made;
}

void set_system_unix_sockets(UnixSocketManager* manager) { __atomic_store_n(&g_override, manager, __ATOMIC_RELEASE); }

}  // namespace process
