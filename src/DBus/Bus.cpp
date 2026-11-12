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

#include "DBus/Bus.hpp"
#include "DBus/BusInternal.hpp"

namespace dbus {

namespace {
uint64_t reply_key(uint32_t caller, uint32_t serial) { return uint64_t(caller) << 32 | serial; }
}  // namespace

Bus::Bus(allocators::IAllocator& allocator, BusKind kind, StringView guid, StringView machine_id)
    : allocator_(allocator), kind_(kind), connections_(allocator), by_id_(allocator), names_(allocator), name_index_(allocator), pending_(allocator),
      expiries_(allocator), service_folders_(allocator), services_(allocator), activations_(allocator), local_queue_(allocator), local_peers_(allocator), wire_(allocator), scratch_(allocator) {
  guid.first(guid.size() < 32 ? guid.size() : 32).copy_to(ArrayView<char>(guid_, 33));
  machine_id.first(machine_id.size() < 32 ? machine_id.size() : 32).copy_to(ArrayView<char>(machine_id_, 33));
}

Bus::~Bus() {
  local_queue_.clear();
  name_index_.clear();
  names_.clear();
  by_id_.clear();
  connections_.clear();
}

Bus::Connection* Bus::find(uint32_t client) const {
  if (client == 0) return nullptr;
  Connection* const* c = by_id_.find(client);
  return c ? *c : nullptr;
}

StringView Bus::unique_name(uint32_t client) const {
  const Connection* c = find(client);
  return c ? c->unique.view() : StringView();
}

uint32_t Bus::owner_of(StringView name) const {
  if (is_unique_name(name)) {
    if (!name.starts_with(":1.")) return 0;
    uint64_t id = 0;
    const StringView digits = name.without_prefix(3);
    if (digits.empty() || digits.size() > 10 || !digits.parse_uint(&id) || id > 0xFFFFFFFFu) return 0;
    const Connection* c = find(uint32_t(id));
    return c && c->hello && !c->monitor && !c->closing && c->unique == name ? c->id : 0;      // (":1.05" is not ":1.5")
  }
  const NameEntry* e = name_entry(name);
  return e && e->queue.count() ? e->queue[0].client : 0;
}

Bus::NameEntry* Bus::name_entry(StringView name) const {
  if (!scratch_.assign(name)) return nullptr;
  NameEntry* const* e = name_index_.find(scratch_.c_str());
  return e ? *e : nullptr;
}

void Bus::make_unique_name(Connection& c) {
  c.unique.clear();
  if (!c.unique.append(":1.") || !c.unique.append_uint(c.id)) c.closing = true;
}

// --- connections

uint32_t Bus::open_client(uint64_t pid, uint32_t uid, bool can_pass_fds) {
  if (connections_.count() >= kMaxConnections) return 0;
  Connection* c = connections_.emplace(allocator_);
  if (!c) return 0;
  while (next_id_ == 0 || find(next_id_)) ++next_id_;
  c->id = next_id_++;
  c->pid = pid;
  c->uid = uid;
  c->auth = UniquePointer<AuthServer>::make(allocator_, allocator_, uid, guid(), can_pass_fds);
  if (!c->auth || !by_id_.put(c->id, c)) {
    connections_.erase(c);
    return 0;
  }
  return c->id;
}

uint32_t Bus::connect_local(LocalPeer& peer) {
  if (connections_.count() >= kMaxConnections) return 0;
  Connection* c = connections_.emplace(allocator_);
  if (!c) return 0;
  while (next_id_ == 0 || find(next_id_)) ++next_id_;
  c->id = next_id_++;
  c->local = &peer;
  if (!by_id_.put(c->id, c) || !local_peers_.push_back(c->id)) {
    by_id_.remove(c->id);
    connections_.erase(c);
    return 0;
  }
  make_unique_name(*c);
  c->hello = true;
  signal_owner_changed(c->unique.view(), StringView(), c->unique.view());
  return c->id;
}

void Bus::disconnect_local(uint32_t client) {
  Connection* c = find(client);
  if (c && c->local) drop(*c);
}

void Bus::close(Connection& c) {
  if (!c.local) c.closing = true;
}

void Bus::close_client(uint32_t client) {
  Connection* c = find(client);
  if (c && !c->local) drop(*c);
  deliver_locals();
}

void Bus::drop(Connection& c) {
  const uint32_t id = c.id;
  c.closing = true;
  drop_pending(c);
  release_all_names(c, false);
  if (c.hello && !c.monitor) signal_owner_changed(c.unique.view(), c.unique.view(), StringView());
  if (c.monitor && monitors_) --monitors_;
  if (!c.monitor)
    for (const UniquePointer<MatchRule>& rule : c.matches.view())
      if (rule->eavesdrop && eavesdrop_rules_) --eavesdrop_rules_;
  for (uint64_t i = 0; i < local_peers_.count(); ++i)
    if (local_peers_[i] == id) { local_peers_.remove_at(i); break; }
  by_id_.remove(id);
  connections_.erase(&c);
}

// The calls a connection was to answer (their callers get NoReply) and the ones it made, forgotten. (In rounds: a list that could not grow still
// makes progress, so nothing is left pointing at the connection.)
void Bus::drop_pending(Connection& c) {
  const uint32_t id = c.id;
  for (;;) {
    List<uint64_t> keys(allocator_);
    for (const KeyValue<uint64_t, Pending>& kv : pending_)
      if ((kv.value.callee == id || uint32_t(kv.key >> 32) == id) && !keys.push_back(kv.key)) break;
    if (keys.count() == 0) return;
    for (uint64_t key : keys.view()) {
      const Pending* callee = pending_.find(key);
      const uint32_t caller = uint32_t(key >> 32);
      const bool owed = callee && callee->callee == id && caller != id;
      pending_.remove(key);
      Connection* waiting = find(caller);
      if (!waiting) continue;
      if (waiting->pending_calls) --waiting->pending_calls;
      if (!owed || waiting->closing) continue;
      Message error(allocator_);
      if (!from_bus(&error, kError) || !error.error_name.assign("org.freedesktop.DBus.Error.NoReply") || !error.destination.assign(waiting->unique.view()))
        continue;
      error.has_reply_serial = true;
      error.reply_serial = uint32_t(key);
      Writer w(error.body);
      w.string("Message recipient disconnected from message bus without replying");
      if (w.ok() && error.signature.assign("s")) send_from_bus(error);
    }
  }
}

// Takes the connection out of every name's queue (the next in each takes over), in rounds as drop_pending().
void Bus::release_all_names(Connection& c, bool tell_loser) {
  const uint32_t id = c.id;
  while (c.names) {
    List<NameEntry*> held(allocator_);
    for (NameEntry& e : names_) {
      bool in = false;
      for (const NameEntry::Owner& o : e.queue.view()) in |= o.client == id;
      if (in && !held.push_back(&e)) break;
    }
    if (held.count() == 0) return;
    for (NameEntry* e : held.view())
      if (remove_from_queue(*e, id, tell_loser)) {
        name_index_.remove(e->name.c_str());
        names_.erase(e);
      }
  }
}

// --- bytes in

void Bus::receive(uint32_t client, ConstByteView bytes, Queue<FdRef>* fds) {
  Connection* c = find(client);
  if (!c || c->local || c->closing) {
    if (fds) fds->clear();
    return;
  }
  if (fds) {
    FdRef fd;
    while (fds->dequeue(&fd))
      if (!c->inbox_fds.enqueue(util::move(fd))) close(*c);
    if (c->inbox_fds.count() > kMaxIncomingFds) close(*c);
  }
  if (!c->auth->done()) {
    const uint64_t used = c->auth->feed(bytes, &c->out);
    if (c->auth->failed()) {
      close(*c);
      return;
    }
    bytes = bytes.subview(used);
  }
  if (!c->closing && !bytes.empty() && !c->inbox.append(bytes)) close(*c);
  if (!c->closing && c->auth->done()) read_messages(*c);
  if (!c->closing && c->auth->done() && !c->auth->unix_fds() && c->inbox_fds.count()) close(*c);      // (descriptors it never asked to pass)
  if (activations_.count()) finish_activations();
  deliver_locals();
}

void Bus::read_messages(Connection& c) {
  uint64_t used = 0;
  Message m(allocator_);
  while (!c.closing) {
    const ConstByteView left = c.inbox.view().subview(used);
    const int64_t size = framed_size(left);
    if (size < 0) { close(c); break; }
    if (size == 0 || left.size() < uint64_t(size)) break;
    if (decode(left.first(uint64_t(size)), &m) != DecodeStatus::Ok) { close(c); break; }
    used += uint64_t(size);
    if (m.unix_fds) {
      if (!c.auth->unix_fds() || m.unix_fds > kMaxFdsPerMessage || m.unix_fds > c.inbox_fds.count()) { close(c); break; }
      for (uint32_t i = 0; i < m.unix_fds; ++i) {
        FdRef fd;
        c.inbox_fds.dequeue(&fd);
        if (!m.fds.push_back(util::move(fd))) { close(c); break; }
      }
      if (c.closing) break;
    }
    handle(c, m);
  }
  if (used == 0) return;
  if (used >= c.inbox.size()) {
    c.inbox.clear();
    return;
  }
  ByteBuffer rest(allocator_);
  if (!rest.append(c.inbox.view().subview(used))) { close(c); return; }
  c.inbox.clear();
  c.inbox.append(rest.view());
}

void Bus::handle(Connection& from, Message& m) {
  if (from.monitor || m.path == "/org/freedesktop/DBus/Local" || m.interface == "org.freedesktop.DBus.Local") {
    close(from);
    return;
  }
  if (!from.hello) {
    if (m.type == kMethodCall && m.destination == kBusName && m.member == "Hello" && (m.interface.empty() || m.interface == kBusName))
      BusDriver::hello(*this, from, m);
    else
      close(from);
    return;
  }
  if (!m.sender.assign(from.unique.view())) {
    close(from);
    return;
  }
  route(&from, m);
}

uint32_t Bus::send(uint32_t client, Message& m) {
  Connection* c = find(client);
  if (!c || !c->hello || c->closing) return 0;
  if (m.serial == 0) {
    if (c->next_serial == 0) c->next_serial = 1;
    m.serial = c->next_serial++;
  }
  if (!m.sender.assign(c->unique.view())) return 0;
  const uint32_t serial = m.serial;
  route(c, m);
  return serial;
}

// --- routing

bool Bus::encode_wire(const Message& m) {
  wire_.clear();
  return encode(m, &wire_);
}

static bool can_take_fds(const Bus::Connection& c) { return c.local || (c.auth && c.auth->unix_fds()); }

void Bus::deliver(Connection& to, const Message& m, const ByteBuffer& wire) {
  if (to.closing) return;
  if (!m.fds.empty() && !can_take_fds(to)) return;
  if (to.local) {
    UniquePointer<Message> copy = UniquePointer<Message>::make(allocator_, allocator_);
    if (!copy || !m.copy_to(copy.get())) return;
    local_queue_.enqueue(Local{to.id, util::move(copy)});
    return;
  }
  if (to.out.size() > kMaxOutgoingBytes) {      // (it does not read: a monitor is closed, as dbus-daemon does; others miss this one)
    if (to.monitor) close(to);
    return;
  }
  const uint64_t at = to.out.size();
  if (!to.out.append(wire.view())) {
    close(to);
    return;
  }
  for (const FdRef& fd : m.fds.view())
    if (!to.out_fds.push_back(OutFd{at, fd})) {
      close(to);
      return;
    }
}

bool Bus::wants(const Connection& c, const Message& m, const StringArgs* args, bool eavesdropping) const {
  const uint32_t sender = is_unique_name(m.sender.view()) ? owner_of(m.sender.view()) : 0;
  auto sender_owns = [&](StringView name) { return sender != 0 && owner_of(name) == sender; };
  for (const UniquePointer<MatchRule>& rule : c.matches.view()) {
    if (eavesdropping && !rule->eavesdrop && !c.monitor) continue;
    if (rule->matches(m, args, sender_owns)) return true;
  }
  return false;
}

// To every connection whose rules match (`everyone`: a message with no destination) or that eavesdrops on it (one addressed to `addressed`), then to the
// monitors. `wire_` holds the message.
void Bus::broadcast(const Message& m, const Connection* addressed, bool everyone) {
  if (!everyone && eavesdrop_rules_ == 0) {
    to_monitors(m, wire_, addressed);
    return;
  }
  StringArgs args;
  bool have_args = false;
  for (Connection& c : connections_) {
    if (&c == addressed || !c.hello || c.monitor || c.closing || c.matches.count() == 0) continue;
    bool any_args = false;
    for (const UniquePointer<MatchRule>& rule : c.matches.view()) any_args |= rule->args_wanted() != 0;
    if (any_args && !have_args) {
      string_args(m.body.view(), m.signature.view(), m.big_endian, StringArgs::kMax, &args);
      have_args = true;
    }
    if (wants(c, m, any_args ? &args : nullptr, !everyone)) deliver(c, m, wire_);
  }
  to_monitors(m, wire_, addressed);
}

void Bus::to_monitors(const Message& m, const ByteBuffer& wire, const Connection* skip) {
  if (monitors_ == 0) return;
  StringArgs args;
  string_args(m.body.view(), m.signature.view(), m.big_endian, StringArgs::kMax, &args);
  for (Connection& c : connections_) {
    if (!c.monitor || c.closing || &c == skip) continue;
    if (c.matches.count() == 0 || wants(c, m, &args, true)) deliver(c, m, wire);
  }
}

void Bus::route(Connection* from, Message& m) {
  if (m.destination == kBusName) {
    if (encode_wire(m)) to_monitors(m, wire_, nullptr);
    if (m.type == kMethodCall) BusDriver::handle(*this, *from, m);
    return;      // (replies and signals to the bus itself go nowhere)
  }
  if (m.destination.empty()) {
    if (encode_wire(m)) broadcast(m, nullptr, true);
    return;
  }
  Connection* to = find(owner_of(m.destination.view()));
  // a service that is starting (or has just taken its name, its queue not sent yet): the message waits behind the ones before it, in order
  Activation* starting = nullptr;
  for (UniquePointer<Activation>& a : activations_.view())
    if (a->name == m.destination.view()) starting = a.get();
  const bool may_start = !to && !is_unique_name(m.destination.view()) && !(m.flags & kNoAutoStart);
  if (!starting && may_start && service_for(m.destination.view())) {
    starting = activate(m.destination.view(), *from, m);
    if (!starting) return;      // (it could not start: the caller has been told)
  }
  if (starting && (to || may_start)) {      // (a message that may not start the service and finds no owner gets the usual error below)
    if (queued_for_activation_ >= kMaxQueuedForActivation || from->queued_for_activation >= kMaxQueuedPerConnection) {
      error_to(*from, m, "org.freedesktop.DBus.Error.LimitsExceeded", "Too many messages are waiting for services to start");
      return;
    }
    UniquePointer<Message> copy = UniquePointer<Message>::make(allocator_, allocator_);
    if (!copy || !m.copy_to(copy.get()) || !starting->queued.push_back(Activation::Queued{from->id, util::move(copy)})) {
      error_to(*from, m, "org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
      return;
    }
    ++queued_for_activation_;
    ++from->queued_for_activation;
    return;
  }
  if (!to) {
    if (m.expects_reply()) {
      String text(allocator_);
      if (is_unique_name(m.destination.view()) || (m.flags & kNoAutoStart)) {
        text.append("Name \"");
        text.append(m.destination.view());
        text.append("\" does not exist");
        error_to(*from, m, "org.freedesktop.DBus.Error.NameHasNoOwner", text.view());
      } else {
        text.append("The name ");
        text.append(m.destination.view());
        text.append(" was not provided by any .service files");
        error_to(*from, m, "org.freedesktop.DBus.Error.ServiceUnknown", text.view());
      }
    }
    return;
  }
  if (m.type == kMethodReturn || m.type == kError) {
    const uint64_t key = reply_key(to->id, m.reply_serial);
    const Pending* callee = pending_.find(key);
    if (callee && callee->callee == from->id) {
      pending_.remove(key);
      if (to->pending_calls) --to->pending_calls;
    } else if (kind_ == BusKind::System) {
      return;      // (a reply nobody asked for)
    }
  }
  if (!m.fds.empty() && !can_take_fds(*to)) {
    if (m.expects_reply())
      error_to(*from, m, "org.freedesktop.DBus.Error.NotSupported", "Tried to send message with Unix file descriptors to a client that doesn't support that.");
    return;
  }
  if (!to->local && to->out.size() > kMaxOutgoingBytes) {
    if (m.expects_reply()) error_to(*from, m, "org.freedesktop.DBus.Error.LimitsExceeded", "The recipient's queue of messages is full");
    return;
  }
  if (m.expects_reply() && to != from) {
    const ExpectStatus st = expect_reply(*from, m.serial, *to);
    if (st == ExpectStatus::TooMany) {
      error_to(*from, m, "org.freedesktop.DBus.Error.LimitsExceeded", "The connection has too many calls waiting for replies");
      return;
    }
    if (st == ExpectStatus::NoMemory) {
      error_to(*from, m, "org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
      return;
    }
  }
  if (!encode_wire(m)) {
    if (m.expects_reply()) error_to(*from, m, "org.freedesktop.DBus.Error.LimitsExceeded", "The message is too big");
    return;
  }
  deliver(*to, m, wire_);
  broadcast(m, to, false);
}

// Notes that `from` waits for `to` to answer its call `serial`, until the reply timeout.
Bus::ExpectStatus Bus::expect_reply(Connection& from, uint32_t serial, Connection& to) {
  const uint64_t key = reply_key(from.id, serial);
  const bool known = pending_.contains(key);
  if (!known && from.pending_calls >= kMaxPendingRepliesPerConnection) return ExpectStatus::TooMany;
  if (++generation_ == 0) generation_ = 1;
  if (reply_timeout_us_ != 0) {
    // stale entries (answered calls) are dropped when they are more than the live ones (the new queue replaces the old only when it was made whole)
    if (expiries_.count() > 2 * pending_.count() + 4096) {
      Queue<Expiry> live(allocator_);
      bool whole = true;
      for (const Expiry& e : expiries_) {
        const Pending* p = pending_.find(e.key);
        if (p && p->generation == e.generation && !live.enqueue(e)) { whole = false; break; }
      }
      if (whole) {
        expiries_.clear();
        Expiry e;
        while (whole && live.dequeue(&e)) whole = expiries_.enqueue(e);
      }
    }
    if (!expiries_.enqueue(Expiry{key, generation_, now_us_ ? now_us_ + reply_timeout_us_ : 0})) return ExpectStatus::NoMemory;
    if (!now_us_) ++unset_deadlines_;
  }
  if (!pending_.put(key, Pending{to.id, generation_})) return ExpectStatus::NoMemory;      // (its Expiry, if made, is stale: skipped)
  if (!known) ++from.pending_calls;
  return ExpectStatus::Ok;
}

void Bus::expire_replies() {
  if (reply_timeout_us_ == 0 || now_us_ == 0) return;
  if (unset_deadlines_) {      // (calls made before the first tick: timed from it)
    for (Expiry& e : expiries_)
      if (e.deadline_us == 0) e.deadline_us = now_us_ + reply_timeout_us_;
    unset_deadlines_ = 0;
  }
  for (Expiry* e = expiries_.front(); e && e->deadline_us <= now_us_; e = expiries_.front()) {
    const Expiry gone = *e;
    expiries_.dequeue();
    const Pending* p = pending_.find(gone.key);
    if (!p || p->generation != gone.generation) continue;      // (answered meanwhile)
    pending_.remove(gone.key);
    Connection* waiting = find(uint32_t(gone.key >> 32));
    if (!waiting) continue;
    if (waiting->pending_calls) --waiting->pending_calls;
    if (waiting->closing) continue;
    Message error(allocator_);
    if (!from_bus(&error, kError) || !error.error_name.assign("org.freedesktop.DBus.Error.NoReply") || !error.destination.assign(waiting->unique.view()) ||
        !error.signature.assign("s"))
      continue;
    error.has_reply_serial = true;
    error.reply_serial = uint32_t(gone.key);
    Writer w(error.body);
    w.string("Did not receive a reply. Possible causes include: the remote application did not send a reply, the message bus security policy blocked the "
             "reply, the reply timeout expired, or the network connection was broken.");
    if (w.ok()) send_from_bus(error);
  }
}

// --- activation

void Bus::set_activation(IServiceLauncher* launcher, filesystem::IFileSystem* files, ArrayView<const StringView> folders) {
  launcher_ = launcher;
  service_files_ = files;
  service_folders_.clear();
  for (const StringView& f : folders) {
    String folder(allocator_);
    if (folder.assign(f)) service_folders_.push_back(util::move(folder));
  }
  reload_services();
}

uint32_t Bus::reload_services() {
  services_.clear();
  services_read_us_ = now_us_ ? now_us_ : 1;
  if (!service_files_) return 0;
  ByteBuffer text(allocator_);
  String path(allocator_);
  for (const String& folder : service_folders_.view()) {
    UniquePointer<filesystem::IDirectory> dir;
    if (service_files_->open_directory(folder.view(), &dir) != filesystem::Status::Ok) continue;
    filesystem::DirectoryEntry entry;
    while (dir->next(&entry) == filesystem::Status::Ok && services_.count() < kMaxServiceFiles) {
      const StringView leaf = entry.name.view();
      if (entry.type != filesystem::NodeType::File || !leaf.ends_with(".service") || entry.size > kMaxServiceFileBytes) continue;
      path.clear();
      if (!path.append(folder.view()) || !path.push_back('/') || !path.append(leaf)) continue;
      UniquePointer<filesystem::IFile> file;
      if (service_files_->open_file(path.view(), filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) continue;
      const uint64_t size = file->size();
      if (size > kMaxServiceFileBytes) continue;
      text.clear();
      uint64_t got = 0;
      if (!text.put_zeros(size) || file->read(0, text.writable_view(), &got) != filesystem::Status::Ok) continue;
      UniquePointer<ServiceFile> service = UniquePointer<ServiceFile>::make(allocator_, allocator_);
      if (!service || !parse_service_file(StringView(text.view().first(got)), service.get())) continue;
      bool known = false;      // (the first folder that names it wins, as in dbus-daemon)
      for (const UniquePointer<ServiceFile>& s : services_.view()) known |= s->name == service->name.view();
      if (!known) services_.push_back(util::move(service));
    }
  }
  return uint32_t(services_.count());
}

const ServiceFile* Bus::service_for(StringView name) {
  if (!launcher_) return nullptr;
  for (int pass = 0; pass < 2; ++pass) {
    for (const UniquePointer<ServiceFile>& s : services_.view())
      if (s->name == name) return s.get();
    // not named: the files may have changed since they were read (at most once a second)
    if (pass == 0 && now_us_ != 0 && now_us_ - services_read_us_ >= 1000000) reload_services();      // (no clock yet: what was read at the start stands)
    else break;
  }
  return nullptr;
}

bool Bus::activatable(StringView name) const {
  for (const UniquePointer<ServiceFile>& s : services_.view())
    if (s->name == name) return true;
  return false;
}

// The activation of `name` (started now, or already on its way). Null when it could not start: `asker` has been told why, for `call`.
Bus::Activation* Bus::activate(StringView name, Connection& asker, const Message& call) {
  for (UniquePointer<Activation>& a : activations_.view())
    if (a->name == name) return a.get();
  const ServiceFile* service = service_for(name);
  if (!service) return nullptr;
  if (activations_.count() >= kMaxActivations) {
    error_to(asker, call, "org.freedesktop.DBus.Error.LimitsExceeded", "Too many services are starting");
    return nullptr;
  }
  List<String> words(allocator_);
  if (!split_exec(allocator_, service->exec.view(), &words)) {
    String text(allocator_);
    text.append("Invalid Exec line in the service file of ");
    text.append(name);
    error_to(asker, call, "org.freedesktop.DBus.Error.Spawn.FileInvalid", text.view());
    return nullptr;
  }
  StringView argv[kMaxExecWords];
  for (uint64_t i = 0; i < words.count(); ++i) argv[i] = words[i].view();
  UniquePointer<Activation> a = UniquePointer<Activation>::make(allocator_, allocator_);
  String why(allocator_);
  if (!a || !a->name.assign(name)) {
    error_to(asker, call, "org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
    return nullptr;
  }
  a->handle = launcher_->start(kind_, name, ArrayView<const StringView>(argv, words.count()), &why);
  if (a->handle == 0) {
    String text(allocator_);
    text.append("Failed to execute program ");
    text.append(name);
    if (!why.empty()) { text.append(": "); text.append(why.view()); }
    error_to(asker, call, "org.freedesktop.DBus.Error.Spawn.ExecFailed", text.view());
    return nullptr;
  }
  a->deadline_us = now_us_ ? now_us_ + activation_timeout_us_ : 0;
  Activation* made = a.get();
  if (!activations_.push_back(util::move(a))) {
    launcher_->forget(made->handle);
    error_to(asker, call, "org.freedesktop.DBus.Error.NoMemory", "Not enough memory");
    return nullptr;
  }
  return made;
}

void Bus::fail_activation(Activation& a, StringView error_name, StringView text) {
  for (Activation::Queued& q : a.queued.view()) {
    if (queued_for_activation_) --queued_for_activation_;
    Connection* c = find(q.from);
    if (c && c->queued_for_activation) --c->queued_for_activation;
    if (c && !c->closing && q.message) error_to(*c, *q.message, error_name, text);
  }
  a.queued.clear();
  for (const Activation::Starter& s : a.starters.view()) {
    Connection* c = find(s.client);
    if (!c || c->closing || !s.wants_reply) continue;
    Message call(allocator_);      // (what error_to needs of the call: its serial, and that it wants an answer)
    call.type = kMethodCall;
    call.serial = s.serial;
    error_to(*c, call, error_name, text);
  }
  a.starters.clear();
}

// Activations whose service has taken its name (the waiting messages go on, StartServiceByName says 1) or failed (everyone waiting is told).
void Bus::finish_activations() {
  for (uint64_t i = 0; i < activations_.count();) {
    Activation& a = *activations_[i];
    bool done = false;
    if (owner_of(a.name.view())) {
      List<Activation::Queued> queued(allocator_);
      for (Activation::Queued& q : a.queued.view()) queued.push_back(util::move(q));
      a.queued.clear();
      for (const Activation::Starter& s : a.starters.view()) {
        Connection* c = find(s.client);
        if (!c || c->closing || !s.wants_reply) continue;
        Message r(allocator_);
        if (!from_bus(&r, kMethodReturn) || !r.destination.assign(c->unique.view()) || !r.signature.assign("u")) continue;
        r.has_reply_serial = true;
        r.reply_serial = s.serial;
        Writer w(r.body);
        w.u32(kStartSuccess);
        if (!w.ok() || !encode_wire(r)) continue;
        deliver(*c, r, wire_);
        broadcast(r, c, false);
      }
      a.starters.clear();
      launcher_->forget(a.handle);
      activations_.remove_at(i);      // (before the messages go: one that asks for the service again finds it running)
      for (Activation::Queued& q : queued.view()) {
        if (queued_for_activation_) --queued_for_activation_;
        Connection* c = find(q.from);
        if (c && c->queued_for_activation) --c->queued_for_activation;
        if (c && !c->closing && q.message) route(c, *q.message);
      }
      continue;
    }
    uint32_t code = 0;
    String why(allocator_);
    const IServiceLauncher::State st = launcher_->poll(a.handle, &code, &why);
    String text(allocator_);
    if (st == IServiceLauncher::State::Exited) {
      text.append("Process ");
      text.append(a.name.view());
      text.append(" exited with status ");
      text.append_uint(code);
      fail_activation(a, "org.freedesktop.DBus.Error.Spawn.ChildExited", text.view());
      done = true;
    } else if (st == IServiceLauncher::State::CouldNotRun) {
      text.append("Failed to execute program ");
      text.append(a.name.view());
      if (!why.empty()) { text.append(": "); text.append(why.view()); }
      fail_activation(a, "org.freedesktop.DBus.Error.Spawn.ExecFailed", text.view());
      done = true;
    } else if (now_us_ && a.deadline_us == 0) {
      a.deadline_us = now_us_ + activation_timeout_us_;
    } else if (now_us_ && now_us_ >= a.deadline_us) {
      text.append("Activation of ");
      text.append(a.name.view());
      text.append(" timed out");
      fail_activation(a, "org.freedesktop.DBus.Error.TimedOut", text.view());
      done = true;
    }
    if (done) {
      launcher_->forget(a.handle);
      activations_.remove_at(i);
    } else {
      ++i;
    }
  }
}

void Bus::deliver_locals() {
  if (delivering_locals_) return;
  delivering_locals_ = true;
  Local item;
  for (uint32_t rounds = 0; rounds < 100000 && local_queue_.dequeue(&item); ++rounds) {
    Connection* c = find(item.client);
    if (c && c->local && !c->closing && item.message) c->local->deliver(*item.message);
    item.message.reset();
  }
  delivering_locals_ = false;
}

bool Bus::take_output(uint32_t client, ByteBuffer* out, List<OutFd>* fds) {
  Connection* c = find(client);
  if (!c || c->local) return true;
  const uint64_t base = out->size();
  if (!out->append(c->out.view())) return true;
  for (const OutFd& f : c->out_fds.view())
    if (!fds->push_back(OutFd{base + f.at, f.fd})) return true;
  c->out.clear();
  c->out_fds.clear();
  return c->closing;
}

uint64_t Bus::output_waiting(uint32_t client) const {
  const Connection* c = find(client);
  return c ? c->out.size() : 0;
}

void Bus::tick(uint64_t now_us) {
  now_us_ = now_us ? now_us : 1;
  expire_replies();
  if (activations_.count()) finish_activations();
  for (uint64_t i = 0; i < local_peers_.count(); ++i) {
    Connection* c = find(local_peers_[i]);
    if (c && c->local) c->local->tick(now_us);
  }
  deliver_locals();
}

// --- the bus's own messages

bool Bus::from_bus(Message* m, uint8_t type) {
  m->type = type;
  m->flags = kNoReplyExpected;
  m->big_endian = false;
  if (++serial_ == 0) serial_ = 1;
  m->serial = serial_;
  return m->sender.assign(kBusName);
}

void Bus::send_from_bus(Message& m) {
  if (!encode_wire(m)) return;
  if (m.destination.empty()) {
    broadcast(m, nullptr, true);
    return;
  }
  Connection* to = find(owner_of(m.destination.view()));
  if (to) deliver(*to, m, wire_);
  broadcast(m, to, false);
}

void Bus::signal_owner_changed(StringView name, StringView old_owner, StringView new_owner) {
  Message s(allocator_);
  if (!from_bus(&s, kSignal) || !s.path.assign(kBusPath) || !s.interface.assign(kBusName) || !s.member.assign("NameOwnerChanged") || !s.signature.assign("sss"))
    return;
  Writer w(s.body);
  w.string(name);
  w.string(old_owner);
  w.string(new_owner);
  if (w.ok()) send_from_bus(s);
}

void Bus::signal_to(Connection& c, const char* member, StringView name) {
  if (c.local == nullptr && c.closing) return;
  Message s(allocator_);
  if (!from_bus(&s, kSignal) || !s.path.assign(kBusPath) || !s.interface.assign(kBusName) || !s.member.assign(member) || !s.signature.assign("s") ||
      !s.destination.assign(c.unique.view()))
    return;
  Writer w(s.body);
  w.string(name);
  if (!w.ok() || !encode_wire(s)) return;
  deliver(c, s, wire_);      // (straight to it: a monitor-to-be still gets its NameLost)
  broadcast(s, &c, false);
}

void Bus::error_to(Connection& c, const Message& call, StringView error_name, StringView text) {
  if (!call.expects_reply()) return;
  Message e(allocator_);
  if (!from_bus(&e, kError) || !e.error_name.assign(error_name) || !e.destination.assign(c.unique.view()) || !e.signature.assign("s")) return;
  e.has_reply_serial = true;
  e.reply_serial = call.serial;
  Writer w(e.body);
  w.string(text);
  if (!w.ok() || !encode_wire(e)) return;
  deliver(c, e, wire_);
  broadcast(e, &c, false);
}

// --- names

bool Bus::remove_from_queue(NameEntry& e, uint32_t client, bool tell_loser) {
  uint64_t at = 0;
  while (at < e.queue.count() && e.queue[at].client != client) ++at;
  if (at == e.queue.count()) return e.queue.count() == 0;
  e.queue.remove_at(at);
  Connection* loser = find(client);
  if (loser && loser->names) --loser->names;
  if (at == 0) {
    if (tell_loser && loser) signal_to(*loser, "NameLost", e.name.view());
    Connection* next = e.queue.count() ? find(e.queue[0].client) : nullptr;
    signal_owner_changed(e.name.view(), loser ? loser->unique.view() : StringView(), next ? next->unique.view() : StringView());
    if (next) signal_to(*next, "NameAcquired", e.name.view());
  }
  return e.queue.count() == 0;
}

uint32_t Bus::request_name(uint32_t client, StringView name, uint32_t flags) {
  Connection* c = find(client);
  if (!c || !c->hello || c->monitor || !valid_bus_name(name) || is_unique_name(name) || name == kBusName) return 0;
  NameEntry* e = name_entry(name);
  if (!e) {
    if (c->names >= kMaxNamesPerConnection) return 0;
    e = names_.emplace(allocator_);
    if (!e) return 0;
    if (!e->name.assign(name) || !e->queue.push_back(NameEntry::Owner{client, flags}) || !name_index_.put(e->name.c_str(), e)) {
      names_.erase(e);
      return 0;
    }
    ++c->names;
    signal_owner_changed(name, StringView(), c->unique.view());
    signal_to(*c, "NameAcquired", name);
    return kRequestPrimaryOwner;
  }
  const NameEntry::Owner primary = e->queue[0];
  if (primary.client == client) {
    e->queue[0].flags = flags;
    return kRequestAlreadyOwner;
  }
  uint64_t queued = 0;
  while (queued < e->queue.count() && e->queue[queued].client != client) ++queued;
  const bool in_queue = queued < e->queue.count();
  if (!in_queue && c->names >= kMaxNamesPerConnection) return 0;
  if ((flags & kNameReplaceExisting) && (primary.flags & kNameAllowReplacement)) {
    Connection* old = find(primary.client);
    if (in_queue) e->queue.remove_at(queued);
    else ++c->names;
    if (!e->queue.insert(0, NameEntry::Owner{client, flags})) {
      --c->names;
      return 0;
    }
    if (primary.flags & kNameDoNotQueue) {      // (the old owner, now second, asked not to wait in the queue)
      e->queue.remove_at(1);
      if (old && old->names) --old->names;
    }
    if (old) signal_to(*old, "NameLost", e->name.view());
    signal_owner_changed(e->name.view(), old ? old->unique.view() : StringView(), c->unique.view());
    signal_to(*c, "NameAcquired", e->name.view());
    return kRequestPrimaryOwner;
  }
  if (flags & kNameDoNotQueue) {
    if (in_queue) {
      e->queue.remove_at(queued);
      --c->names;
    }
    return kRequestExists;
  }
  if (in_queue) {
    e->queue[queued].flags = flags;
  } else {
    if (!e->queue.push_back(NameEntry::Owner{client, flags})) return 0;
    ++c->names;
  }
  return kRequestInQueue;
}

uint32_t Bus::release_name(uint32_t client, StringView name) {
  NameEntry* e = name_entry(name);
  if (!e) return kReleaseNonExistent;
  bool queued = false;
  for (const NameEntry::Owner& o : e->queue.view()) queued |= o.client == client;
  if (!queued) return kReleaseNotOwner;
  if (remove_from_queue(*e, client, true)) {
    name_index_.remove(e->name.c_str());
    names_.erase(e);
  }
  return kReleaseReleased;
}

}  // namespace dbus
