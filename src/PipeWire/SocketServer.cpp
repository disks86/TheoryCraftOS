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

#include "PipeWire/SocketServer.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Kernel/Sync.hpp"
#include "Process/DxgBufferFile.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Process.hpp"

namespace pipewire {

using process::UnixName;
using process::UnixStatus;

// Descriptors the protocols let go of that hold a use of a socket (an eventfd): released by the transport's thread at the start of a round (releasing takes
// the system's lock). After the transport is gone they are released at once.
struct SocketServer::Graveyard {
  explicit Graveyard(allocators::IAllocator& allocator) : dropped(allocator) {}
  kernel::Mutex lock;
  Queue<process::UnixInFlight> dropped;
  bool closed = false;
};

namespace {

// A descriptor the server made: the open object (memory) or the use of an eventfd (an Event socket), as it is passed with SCM_RIGHTS (a copy with a use of
// its own goes each time), and the block the server reads and writes (kept, so memory() stays valid as long as the descriptor).
class KernelDescriptor final : public Descriptor {
 public:
  // (`limit`: how much of the block is the file: all of it for memory the server made, the file's size for a client's memfd)
  KernelDescriptor(SharedPointer<SocketServer::Graveyard> graveyard, process::UnixSocketManager& sockets, process::UnixInFlight entry, process::BlockRef block,
                   uint64_t limit = ~0ull, allocators::IAllocator* gpu_copy_allocator = nullptr)
      : graveyard_(util::move(graveyard)), sockets_(sockets), entry_(util::move(entry)), block_(util::move(block)), limit_(limit) {
    // (a dmabuf: the server's copy of its pixels, the image's size (at most kMaxGpuCopy))
    if (gpu_copy_allocator && entry_.object && entry_.object->gpu_buffer) {
      const uint64_t size = entry_.object->gpu_buffer->size();
      gpu_copy_ = UniquePointer<ByteBuffer>::make(*gpu_copy_allocator, *gpu_copy_allocator);
      if (gpu_copy_ && (size > kMaxGpuCopy || !gpu_copy_->put_zeros(size))) gpu_copy_ = UniquePointer<ByteBuffer>();
    }
  }
  static constexpr uint64_t kMaxGpuCopy = 64u << 20;
  ~KernelDescriptor() {
    if (entry_.token == 0) return;
    bool now = false;
    {
      kernel::ScopedMutex hold(graveyard_->lock);
      now = graveyard_->closed || !graveyard_->dropped.enqueue(util::move(entry_));
    }
    if (now && entry_.token != 0) sockets_.release(uint32_t(entry_.token));
  }
  KernelDescriptor(const KernelDescriptor&) = delete;
  KernelDescriptor& operator=(const KernelDescriptor&) = delete;
  const process::UnixInFlight& entry() const { return entry_; }
  ByteView memory() override {
    if (gpu_copy_) return gpu_copy_->writable_view();
    if (!block_) return ByteView();
    const ByteView all = block_.get()->bytes();
    return limit_ < all.size() ? all.first(limit_) : all;
  }
  bool signal() override {
    if (!is_event()) return false;
    process::ISystemSync& sync = sockets_.sync();
    sync.lock();
    const UnixStatus st = sockets_.event_add_locked(uint32_t(entry_.token), 1);
    sync.unlock();
    return st == UnixStatus::Ok || st == UnixStatus::WouldBlock;      // (a counter that is full has been signalled)
  }
  bool gpu() const override { return gpu_copy_.get() != nullptr; }
  bool sync_from_gpu() override {
    if (!gpu_copy_ || !entry_.object || !entry_.object->gpu_buffer) return false;
    return entry_.object->gpu_buffer->copy_image(gpu_copy_->writable_view(), false);
  }
  bool sync_to_gpu() override {
    if (!gpu_copy_ || !entry_.object || !entry_.object->gpu_buffer) return false;
    return entry_.object->gpu_buffer->copy_image(gpu_copy_->writable_view(), true);
  }
  uint64_t take_signals() override {
    if (!is_event()) return 0;
    uint64_t value = 0;
    process::ISystemSync& sync = sockets_.sync();
    sync.lock();
    sockets_.event_take_locked(uint32_t(entry_.token), &value);
    sync.unlock();
    return value;
  }

 private:
  bool is_event() const { return entry_.kind == process::DescriptorKind::UnixSocket && entry_.token != 0; }
  SharedPointer<SocketServer::Graveyard> graveyard_;
  process::UnixSocketManager& sockets_;
  process::UnixInFlight entry_;
  process::BlockRef block_;
  uint64_t limit_;
  UniquePointer<ByteBuffer> gpu_copy_;      // (a dmabuf) the CPU's copy of its pixels
};

// Every descriptor a protocol hands this transport was made by it (make_memory, make_event), so each is a KernelDescriptor.
const KernelDescriptor& kernel_descriptor(const Fd& fd) { return static_cast<const KernelDescriptor&>(*fd); }

}  // namespace

Fd SocketServer::make_memory(uint64_t size, StringView name) {
  if (size == 0 || size > process::MemoryFile::kMaxSize) return Fd();
  SharedPointer<process::OpenObject> object = SharedPointer<process::OpenObject>::make(allocator_, allocator_);
  UniquePointer<process::MemoryFile> file = UniquePointer<process::MemoryFile>::make(allocator_, allocator_, true);
  if (!object || !file || !object->path.append("/memfd:") || !object->path.append(name)) return Fd();
  if (file->resize(size) != filesystem::Status::Ok) return Fd();
  uint64_t file_size = 0;
  process::BlockRef block = file->block(&file_size);
  if (!block || file_size != size) return Fd();
  using process::MemoryFile;
  file->add_seals(MemoryFile::kSealShrink | MemoryFile::kSealGrow | MemoryFile::kSealSeal);      // (the size stays: the block the server holds is the file)
  object->memory = file.get();
  object->file = util::move(file);
  object->readable = true;
  object->writable = true;
  process::UnixInFlight entry;
  entry.kind = process::DescriptorKind::File;
  entry.object = util::move(object);
  return Fd::make_as<KernelDescriptor>(allocator_, graveyard_, sockets_, util::move(entry), util::move(block));
}

Fd SocketServer::make_gpu_image(uint32_t width, uint32_t height, uint32_t* stride) {
  *stride = 0;
  if (!gpu_images_ || width == 0 || height == 0) return Fd();
  process::UnixInFlight entry;
  uint32_t pitch = 0;
  if (!gpu_images_->make(width, height, &entry, &pitch)) return Fd();
  if (entry.kind != process::DescriptorKind::File || !entry.object || !entry.object->gpu_buffer || pitch < uint64_t{width} * 4) return Fd();
  Fd fd = adopt(util::move(entry));      // (a GPU image: its CPU copy, written and then synced to the image)
  if (!fd || !fd->gpu() || fd->memory().size() < uint64_t{pitch} * height) return Fd();
  *stride = pitch;
  return fd;
}

Fd SocketServer::make_event() {
  uint32_t id = 0;
  if (sockets_.create_event(0, 0, false, &id) != UnixStatus::Ok) return Fd();
  {      // (EFD_NONBLOCK, as PipeWire makes them: a client empties one with a read that must not wait)
    process::ISystemSync& sync = sockets_.sync();
    sync.lock();
    sockets_.set_non_blocking_locked(id, true);
    sync.unlock();
  }
  process::UnixInFlight entry;
  entry.kind = process::DescriptorKind::UnixSocket;
  entry.token = id;
  Fd fd = Fd::make_as<KernelDescriptor>(allocator_, graveyard_, sockets_, util::move(entry), process::BlockRef());
  if (!fd) sockets_.release(id);
  return fd;
}

// A descriptor a client sent (the lock not held): its memory when it is a memfd (the bytes the file has now), else just the descriptor (an eventfd can be
// signalled; anything else is held and let go).
Fd SocketServer::adopt(process::UnixInFlight entry) {
  process::BlockRef block;
  uint64_t size = 0;
  if (entry.kind == process::DescriptorKind::File && entry.object && entry.object->memory) block = entry.object->memory->block(&size);
  const uint64_t token = entry.token;
  const process::DescriptorKind kind = entry.kind;
  const bool gpu = entry.kind == process::DescriptorKind::File && entry.object && entry.object->gpu_buffer;
  Fd fd = Fd::make_as<KernelDescriptor>(allocator_, graveyard_, sockets_, util::move(entry), util::move(block), size, gpu ? &allocator_ : nullptr);
  if (!fd && token != 0) {      // (no memory for it: its use is let go here)
    if (kind == process::DescriptorKind::UnixSocket) sockets_.release(uint32_t(token));
    else process::descriptor_token_release(kind, token);
  }
  return fd;
}

// Lets go of descriptors' uses (the lock not held): an eventfd through this transport's manager (which may not be the system's, in a test), others as the
// system lets go of a descriptor.
void SocketServer::release_entries(Queue<process::UnixInFlight>& entries) {
  process::UnixInFlight e;
  while (entries.dequeue(&e)) {
    if (e.token != 0) {
      if (e.kind == process::DescriptorKind::UnixSocket) sockets_.release(uint32_t(e.token));
      else process::descriptor_token_release(e.kind, e.token);
    }
    e.object = SharedPointer<process::OpenObject>();
  }
}

void SocketServer::drain_graveyard() {
  if (!graveyard_) return;
  Queue<process::UnixInFlight> dropped(allocator_);
  {
    kernel::ScopedMutex hold(graveyard_->lock);
    process::UnixInFlight e;
    while (graveyard_->dropped.dequeue(&e)) dropped.enqueue(util::move(e));
  }
  release_entries(dropped);
}

// A take's descriptors: each a copy with a use of its own, to go with the byte at `at` of the connection's output (into the last batch when no bytes came
// after it: two batches never start at the same byte).
void SocketServer::queue_fds(Connection& c, uint64_t at, const List<Fd>& fds) {
  Connection::Batch* batch = c.batches.count() ? c.batches[c.batches.count() - 1].get() : nullptr;
  if (!batch || batch->at != at) {
    UniquePointer<Connection::Batch> made = UniquePointer<Connection::Batch>::make(allocator_, allocator_);
    if (!made) { c.dead = true; return; }
    made->at = at;
    batch = made.get();
    if (!c.batches.push_back(util::move(made))) { c.dead = true; return; }
  }
  for (const Fd& fd : fds.view()) {
    if (!fd) continue;
    process::UnixInFlight entry = kernel_descriptor(fd).entry();
    if (entry.kind == process::DescriptorKind::Closed) continue;
    if (entry.token != 0) sockets_.add_ref(uint32_t(entry.token));
    const uint64_t token = entry.token;
    if (!batch->rights.enqueue(util::move(entry))) {
      if (token != 0) sockets_.release(uint32_t(token));
      c.dead = true;
    }
  }
  if (batch->rights.count() > process::UnixSocketManager::kMaxRights) c.dead = true;
}

// Descriptors that never went (the connection is over): their uses let go (the lock not held).
void SocketServer::drop_rights(Connection& c) {
  for (UniquePointer<Connection::Batch>& b : c.batches.view()) release_entries(b->rights);
  c.batches.clear();
  release_entries(c.arrived);
}

// What a connection is owed, as far as its socket takes it: up to the next bytes that carry descriptors, then those bytes with them (send_locked takes a
// batch's descriptors only when it sends).
void SocketServer::send_owed_locked(Connection& c, bool* moved) {
  while (!c.dead && c.sent < c.pending.size()) {
    uint64_t end = c.pending.size();
    Connection::Batch* carrying = nullptr;
    if (c.batches.count()) {
      if (c.batches[0]->at <= c.sent) {
        carrying = c.batches[0].get();
        if (c.batches.count() > 1) end = c.batches[1]->at;
      } else {
        end = c.batches[0]->at;
      }
    }
    if (end <= c.sent) end = c.pending.size();      // (a batch whose bytes are those of the next: they go together)
    uint32_t sent = 0;
    const UnixStatus st = sockets_.send_locked(c.socket, c.pending.view().subview(c.sent, end - c.sent), nullptr, 0, carrying ? &carrying->rights : nullptr, &sent);
    if (st == UnixStatus::WouldBlock) break;
    if (st != UnixStatus::Ok) { c.dead = true; break; }
    if (carrying && sent > 0) c.batches.remove_at(0);      // (its queue is empty now: the socket has them)
    c.sent += sent;
    *moved = true;
    if (sent == 0) break;
  }
}

SocketServer::SocketServer(process::UnixSocketManager& sockets, allocators::IAllocator& allocator)
    : sockets_(sockets), allocator_(allocator), connections_(allocator), chunk_(allocator) {
  chunk_.put_zeros(kReadChunk);
  graveyard_ = SharedPointer<Graveyard>::make(allocator_, allocator_);
}

SocketServer::~SocketServer() {
  stop();
  for (UniquePointer<Connection>& c : connections_.view()) {
    c->protocol->close_client(c->client);
    drop_rights(*c);
    sockets_.release(c->socket);
  }
  connections_.clear();
  for (uint32_t i = 0; i < listener_count_; ++i) sockets_.release(listeners_[i].socket);
  listener_count_ = 0;
  for (uint32_t i = 0; i < protocol_count_; ++i) protocols_[i]->set_descriptor_maker(nullptr);
  if (graveyard_) {      // (from now on what the protocols let go of is released at once)
    {
      kernel::ScopedMutex hold(graveyard_->lock);
      graveyard_->closed = true;
    }
    drain_graveyard();
  }
}

bool SocketServer::bind_one(UnixName::Kind kind, StringView name, StringView host_path, IProtocolServer& protocol) {
  if (listener_count_ == kMaxListeners) return false;
  uint32_t id = 0;
  if (sockets_.create(process::UnixType::Stream, 0, &id) != UnixStatus::Ok) return false;
  const UnixName un = UnixName::from(kind, name.bytes());
  UnixStatus st;
  {
    process::ISystemSync& sync = sockets_.sync();
    sync.lock();
    st = sockets_.bind_locked(id, un, host_path);
    if (st == UnixStatus::Ok) st = sockets_.listen_locked(id, process::UnixSocketManager::kMaxBacklog);
    sync.unlock();
  }
  if (st != UnixStatus::Ok) {
    sockets_.release(id);
    return false;
  }
  listeners_[listener_count_++] = Listener{id, &protocol};
  return true;
}

bool SocketServer::listen(IProtocolServer& protocol, StringView linux_path, StringView host_path, filesystem::IFileSystem* files) {
  bool known = false;
  for (uint32_t i = 0; i < protocol_count_; ++i) known |= protocols_[i] == &protocol;
  if (!known) {
    if (protocol_count_ == kMaxListeners) return false;
    protocols_[protocol_count_++] = &protocol;
    protocol.set_descriptor_maker(this);
  }
  bool any = bind_one(UnixName::Kind::Abstract, linux_path, StringView(), protocol);
  if (files && !host_path.empty()) {
    // the folders above the socket, then the socket's file made afresh (one left from an earlier start would say "address in use")
    String folder(allocator_);
    for (uint64_t i = 1; i < host_path.size(); ++i)
      if (host_path[i] == '/') {
        if (!folder.assign(host_path.first(i))) return any;
        filesystem::layout::create_folder(*files, folder.c_str());
      }
    String path(allocator_);
    if (!path.assign(host_path)) return any;
    files->remove(path.view());
    UniquePointer<filesystem::IFile> file;
    if (files->open_file(path.view(), filesystem::OpenMode::CreateOrOpen, &file) == filesystem::Status::Ok) {
      file.reset();
      any |= bind_one(UnixName::Kind::Path, linux_path, path.view(), protocol);
    }
  }
  return any;
}

bool listen_standard_sockets(SocketServer& server, IProtocolServer& native, IProtocolServer& pulse, filesystem::IFileSystem* files, allocators::IAllocator& allocator) {
  auto one = [&](IProtocolServer& protocol, const char* leaf) {
    String linux_path(allocator), host_path(allocator);
    if (!linux_path.append(filesystem::layout::kLinuxRuntimeFolder) || !linux_path.push_back('/') || !linux_path.append(leaf)) return false;
    if (!host_path.append(filesystem::layout::kRuntimeFolder) || !host_path.push_back('/') || !host_path.append(leaf)) return false;
    return server.listen(protocol, linux_path.view(), host_path.view(), files);
  };
  const bool a = one(native, filesystem::layout::kPipeWireSocket);
  const bool b = one(pulse, filesystem::layout::kPulseSocket);
  return a && b;
}

bool SocketServer::pump() {
  bool moved = false;
  drain_graveyard();
  List<Accepted> accepted(allocator_);
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  for (uint32_t i = 0; i < listener_count_; ++i) {
    for (;;) {
      uint32_t id = 0;
      if (sockets_.accept_locked(listeners_[i].socket, &id) != UnixStatus::Ok) break;
      uint64_t pid = 0;
      sockets_.peer_pid_locked(id, &pid);
      if (!accepted.push_back(Accepted{id, pid, listeners_[i].protocol})) {
        sync.unlock();
        sockets_.release(id);
        sync.lock();
        continue;
      }
      moved = true;
    }
  }
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.dead) continue;
    send_owed_locked(c, &moved);
    uint64_t taken = 0;
    while (!c.dead && taken < kMaxReadPerRound) {
      process::UnixSocketManager::Received got;
      const UnixStatus st = sockets_.receive_locked(c.socket, chunk_.writable_view(), false, &got, &c.arrived);
      if (st == UnixStatus::WouldBlock) break;
      if (st != UnixStatus::Ok || got.end_of_file) { c.dead = true; break; }
      if (got.got == 0) break;
      if (!c.inbox.append(chunk_.view().first(got.got))) { c.dead = true; break; }
      taken += got.got;
      moved = true;
    }
  }
  sync.unlock();
  for (const Accepted& a : accepted) {
    const uint32_t client = a.protocol->open_client(a.pid);
    UniquePointer<Connection> c = client ? UniquePointer<Connection>::make(allocator_, allocator_) : UniquePointer<Connection>();
    if (!c) {
      if (client) a.protocol->close_client(client);
      sockets_.release(a.socket);
      continue;
    }
    c->socket = a.socket;
    c->client = client;
    c->protocol = a.protocol;
    if (!connections_.push_back(util::move(c))) {
      a.protocol->close_client(client);
      sockets_.release(a.socket);
    }
  }
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    // the descriptors that came (made into the protocol's, outside the lock), then the bytes with them
    process::UnixInFlight e;
    List<Fd> fds(allocator_);
    while (c.arrived.dequeue(&e)) {
      Fd fd = adopt(util::move(e));
      if (!fd || !fds.push_back(util::move(fd))) c.dead = true;
    }
    if (c.inbox.size() > 0 || fds.count() > 0) {
      c.protocol->receive_with_fds(c.client, c.inbox.view(), fds);
      c.inbox.clear();
    }
  }
  // the protocols' own work (graph changes, flow control), then what each connection is owed
  for (uint32_t i = 0; i < protocol_count_; ++i) protocols_[i]->tick();
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.sent == c.pending.size()) {
      c.pending.clear();
      c.sent = 0;
      for (UniquePointer<Connection::Batch>& b : c.batches.view()) b->at = 0;      // (descriptors waiting for bytes that have not come: they go with the next ones)
    }
    const uint64_t before = c.pending.size();
    List<Fd> fds(allocator_);
    if (c.protocol->take_output_with_fds(c.client, &c.pending, &fds)) c.closing = true;
    if (fds.count()) queue_fds(c, before, fds);
    if (c.pending.size() != before) moved = true;
    if (c.pending.size() - c.sent > kMaxPending) c.dead = true;
  }
  for (uint64_t i = 0; i < connections_.count();) {
    Connection& c = *connections_[i];
    if (c.dead || (c.closing && c.sent == c.pending.size())) {
      c.protocol->close_client(c.client);
      drop_rights(c);
      sockets_.release(c.socket);
      connections_.remove_at(i);
      moved = true;
    } else {
      ++i;
    }
  }
  return moved;
}

bool SocketServer::ready_locked() {
  for (uint32_t i = 0; i < listener_count_; ++i)
    if (sockets_.readiness_locked(listeners_[i].socket).readable) return true;
  for (const UniquePointer<Connection>& p : connections_.view()) {
    const process::UnixReadiness r = sockets_.readiness_locked(p->socket);
    if (r.readable || r.error || r.hangup) return true;
    if (p->sent < p->pending.size() && r.writable) return true;
  }
  return false;
}

void SocketServer::wait(uint64_t timeout_ns) {
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  if (!ready_locked()) sync.wait(timeout_ns);
  sync.unlock();
}

void SocketServer::thread_main(void* self) {
  SocketServer& s = *static_cast<SocketServer*>(self);
  while (!s.stopping_) {
    for (uint32_t rounds = 0; rounds < 16 && s.pump(); ++rounds) {}
    // the protocols want to be ticked this often (the PulseAudio streams' flow control every 10 ms)
    uint64_t interval_us = 100000;
    for (uint32_t i = 0; i < s.protocol_count_; ++i)
      if (s.protocols_[i]->tick_interval_us() < interval_us) interval_us = s.protocols_[i]->tick_interval_us();
    s.wait(interval_us * 1000);
  }
}

bool SocketServer::start() {
  if (thread_) return true;
  stopping_ = false;
  thread_ = kernel::spawn(&SocketServer::thread_main, this, "pipewire");
  return thread_ != nullptr;
}

void SocketServer::stop() {
  if (!thread_) return;
  stopping_ = true;
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  sync.notify_all();
  sync.unlock();
  kernel::join(thread_);
  thread_ = nullptr;
}

}  // namespace pipewire
