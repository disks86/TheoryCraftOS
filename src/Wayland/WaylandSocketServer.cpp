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

#include "Wayland/WaylandSocketServer.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Process/MemoryFile.hpp"

namespace wayland {

using process::UnixName;
using process::UnixStatus;

// Descriptors the server let go of that hold a use of a token (a pipe's or a socket's end): released by the transport's thread, never under the server's lock
// (releasing a socket takes the system lock, which is never taken inside the server's). After the transport is gone they are released at once.
struct SocketServer::Graveyard {
  explicit Graveyard(allocators::IAllocator& allocator) : dropped(allocator) {}
  kernel::Mutex lock;
  Queue<process::UnixInFlight> dropped;
  bool closed = false;
};

namespace {

// A descriptor as the server holds it: what a client passed (or one made here) - the open object and the use of its token, so that it can be passed on to
// another client as it is - and the block of its memory the server reads (kept, so the view map() gave stays valid even if the client grows the file
// meanwhile).
class KernelFd final : public Memory {
 public:
  KernelFd(SharedPointer<SocketServer::Graveyard> graveyard, process::UnixInFlight entry, SharedPointer<GpuImage> image = SharedPointer<GpuImage>(),
           SharedPointer<GpuTimeline> timeline = SharedPointer<GpuTimeline>())
      : graveyard_(util::move(graveyard)), entry_(util::move(entry)), image_(util::move(image)), timeline_(util::move(timeline)) {}
  ~KernelFd() {
    if (entry_.token == 0 || !graveyard_) return;
    bool now = false;
    {
      kernel::ScopedMutex hold(graveyard_->lock);
      now = graveyard_->closed || !graveyard_->dropped.enqueue(util::move(entry_));
    }
    if (now && entry_.token != 0) process::descriptor_token_release(entry_.kind, entry_.token);
  }
  KernelFd(const KernelFd&) = delete;
  KernelFd& operator=(const KernelFd&) = delete;
  const process::UnixInFlight& entry() const { return entry_; }
  const SharedPointer<process::OpenObject>& object() const { return object_ref(); }
  GpuImage* gpu() override { return image_.get(); }
  GpuTimeline* timeline() override { return timeline_.get(); }
  ByteView map(uint64_t size) override {
    if (!entry_.object || !entry_.object->memory) return ByteView();
    uint64_t file_size = 0;
    process::BlockRef block = entry_.object->memory->block(&file_size);
    if (!block || block.get()->capacity() < size) return ByteView();
    block_ = util::move(block);
    return block_.get()->bytes();
  }

 private:
  const SharedPointer<process::OpenObject>& object_ref() const { return entry_.object; }
  SharedPointer<SocketServer::Graveyard> graveyard_;
  process::UnixInFlight entry_;
  SharedPointer<GpuImage> image_;      // a GPU image (a dmabuf), made by the transport's maker
  SharedPointer<GpuTimeline> timeline_;      // a fence (a sync object's timeline), the same
  process::BlockRef block_;
};

// Every descriptor the server holds came from this transport (received, or made by make_fd), so each is a KernelFd.
const KernelFd& kernel_fd(const Fd& fd) { return static_cast<const KernelFd&>(*fd); }

}  // namespace

SocketServer::SocketServer(Server& server, process::UnixSocketManager& sockets, allocators::IAllocator& allocator)
    : server_(server), sockets_(sockets), allocator_(allocator), connections_(allocator), chunk_(allocator), reads_(allocator), writes_(allocator) {
  chunk_.put_zeros(kReadChunk);
  graveyard_ = SharedPointer<Graveyard>::make(allocator_, allocator_);
  server_.set_wake(&SocketServer::on_wake, this);
  server_.set_fd_maker(&SocketServer::make_fd, this);
}

SocketServer::~SocketServer() {
  stop();
  server_.set_wake(nullptr, nullptr);
  server_.set_fd_maker(nullptr, nullptr);
  for (UniquePointer<Connection>& c : connections_.view()) close(*c);
  connections_.clear();
  if (listening_) sockets_.release(listener_);
  listening_ = false;
  for (UniquePointer<ReadJob>& r : reads_.view()) sockets_.release(r->socket);
  for (UniquePointer<WriteJob>& w : writes_.view()) sockets_.release(w->socket);
  reads_.clear();
  writes_.clear();
  if (graveyard_) {      // (from now on what the server lets go of is released at once)
    {
      kernel::ScopedMutex hold(graveyard_->lock);
      graveyard_->closed = true;
    }
    drain_graveyard();
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
  sockets_.release_rights(dropped);
}

// The server's requests (outside every lock): a pipe made (its write end to the server, which hands it to the client; the read end read here), or a write
// into a client's pipe end queued.
bool SocketServer::take_requests() {
  List<TransportRequest> requests(allocator_);
  server_.take_requests(&requests);
  for (TransportRequest& r : requests.view()) {
    if (r.kind == TransportRequest::Kind::MakePipe) {
      uint32_t a = 0, b = 0;
      UniquePointer<ReadJob> job = UniquePointer<ReadJob>::make(allocator_, allocator_);
      if (!job || sockets_.create_pair(process::UnixType::Stream, 0, &a, &b) != UnixStatus::Ok) {
        server_.pipe_read_done(r.job, ConstByteView());
        continue;
      }
      {
        process::ISystemSync& sync = sockets_.sync();
        sync.lock();
        sockets_.shutdown_locked(a, false, true);
        sockets_.shutdown_locked(b, true, false);
        sync.unlock();
      }
      job->job = r.job;
      job->socket = a;
      process::UnixInFlight end;
      end.kind = process::DescriptorKind::UnixSocket;
      end.token = b;      // (the use create_pair gave it: the descriptor's now)
      Fd fd = Fd::make_as<KernelFd>(allocator_, graveyard_, util::move(end));
      if (!fd || !reads_.push_back(util::move(job))) {
        sockets_.release(a);
        if (!fd) sockets_.release(b);
        server_.pipe_read_done(r.job, ConstByteView());
        continue;
      }
      server_.pipe_ready(r.job, util::move(fd));
    } else if (r.fd && r.data) {
      const process::UnixInFlight& e = kernel_fd(r.fd).entry();
      if (e.kind != process::DescriptorKind::UnixSocket || e.token == 0) continue;      // (only a pipe's or a socket's end can be written here)
      UniquePointer<WriteJob> job = UniquePointer<WriteJob>::make(allocator_);
      if (!job) continue;
      sockets_.add_ref(uint32_t(e.token));
      job->socket = uint32_t(e.token);
      job->data = r.data;
      if (!writes_.push_back(util::move(job))) sockets_.release(uint32_t(e.token));
    }
  }
  return requests.count() != 0;
}

// Reads and writes what the sockets take now (the system lock held).
bool SocketServer::progress_jobs_locked() {
  bool moved = false;
  for (UniquePointer<ReadJob>& r : reads_.view()) {
    while (!r->done) {
      process::UnixSocketManager::Received got;
      Queue<process::UnixInFlight> rights(allocator_);
      const UnixStatus st = sockets_.receive_locked(r->socket, chunk_.writable_view(), false, &got, &rights);
      process::UnixInFlight e;
      while (rights.dequeue(&e)) graveyard_->dropped.enqueue(util::move(e));      // (nothing should come with the data: let go later)
      if (st == UnixStatus::WouldBlock) break;
      moved = true;
      if (st != UnixStatus::Ok || got.end_of_file || got.got == 0) {
        r->done = true;
        break;
      }
      const uint64_t room = kMaxPipeRead - r->data.size();
      if (!r->data.append(chunk_.view().first(got.got < room ? got.got : room)) || got.got >= room) r->done = true;
    }
  }
  for (UniquePointer<WriteJob>& w : writes_.view()) {
    while (!w->done && w->sent < w->data->size()) {
      uint32_t sent = 0;
      const UnixStatus st = sockets_.send_locked(w->socket, w->data->view().subview(w->sent), nullptr, 0, nullptr, &sent);
      if (st == UnixStatus::WouldBlock) break;
      moved = true;
      if (st != UnixStatus::Ok || sent == 0) w->done = true;
      w->sent += sent;
    }
    if (w->sent >= w->data->size()) w->done = true;
  }
  return moved;
}

// The jobs that ended: what was read goes to the server; their sockets are let go (a writer's end closing is the reader's end of file).
bool SocketServer::finish_jobs() {
  bool moved = false;
  for (uint64_t i = 0; i < reads_.count();) {
    if (!reads_[i]->done) {
      ++i;
      continue;
    }
    server_.pipe_read_done(reads_[i]->job, reads_[i]->data.view());
    sockets_.release(reads_[i]->socket);
    reads_.remove_at(i);
    moved = true;
  }
  for (uint64_t i = 0; i < writes_.count();) {
    if (!writes_[i]->done) {
      ++i;
      continue;
    }
    sockets_.release(writes_[i]->socket);
    writes_.remove_at(i);
    moved = true;
  }
  return moved;
}

void SocketServer::on_wake(void* self) {
  SocketServer& s = *static_cast<SocketServer*>(self);
  process::ISystemSync& sync = s.sockets_.sync();
  sync.lock();
  s.woken_ = true;
  sync.notify_all();
  sync.unlock();
}

Fd SocketServer::make_fd(void* self, ConstByteView contents) {
  SocketServer& s = *static_cast<SocketServer*>(self);
  SharedPointer<process::OpenObject> object = SharedPointer<process::OpenObject>::make(s.allocator_, s.allocator_);
  UniquePointer<process::MemoryFile> file = UniquePointer<process::MemoryFile>::make(s.allocator_, s.allocator_, true);
  if (!object || !file || !object->path.append("/memfd:wayland-keymap")) return Fd();
  uint64_t written = 0;
  if (file->write(0, contents, &written) != filesystem::Status::Ok || written != contents.size()) return Fd();
  using process::MemoryFile;
  file->add_seals(MemoryFile::kSealShrink | MemoryFile::kSealGrow | MemoryFile::kSealWrite | MemoryFile::kSealSeal);
  object->memory = file.get();
  object->file = util::move(file);
  object->readable = true;      // (every client maps the same pages, read-only)
  process::UnixInFlight entry;
  entry.kind = process::DescriptorKind::File;
  entry.object = util::move(object);
  return Fd::make_as<KernelFd>(s.allocator_, s.graveyard_, util::move(entry));
}

void SocketServer::set_gpu_images(MakeGpuImage make, MakeGpuTimeline make_timeline, void* context) {
  make_gpu_ = make;
  make_timeline_ = make_timeline;
  make_gpu_context_ = context;
  server_.set_dmabuf(make != nullptr);
}

bool SocketServer::listen(filesystem::IFileSystem& files) {
  if (listening_) return true;
  String linux_path(allocator_), host_path(allocator_);
  namespace layout = filesystem::layout;
  if (!linux_path.append(layout::kLinuxRuntimeFolder) || !linux_path.push_back('/') || !linux_path.append(layout::kWaylandDisplay)) return false;
  if (!host_path.append(layout::kRuntimeFolder) || !host_path.push_back('/') || !host_path.append(layout::kWaylandDisplay)) return false;
  // the socket's file, made afresh (one left from an earlier start would say "address in use")
  layout::create_folder(files, layout::kLinux);
  layout::create_folder(files, layout::kLinuxRun);
  layout::create_folder(files, layout::kLinuxRunUser);
  layout::create_folder(files, layout::kRuntimeFolder);
  files.remove(host_path.view());
  UniquePointer<filesystem::IFile> file;
  if (files.open_file(host_path.view(), filesystem::OpenMode::CreateOrOpen, &file) != filesystem::Status::Ok) return false;
  file.reset();
  uint32_t id = 0;
  if (sockets_.create(process::UnixType::Stream, 0, &id) != UnixStatus::Ok) return false;
  const UnixName name = UnixName::from(UnixName::Kind::Path, linux_path.view().bytes());
  UnixStatus st;
  {
    process::ISystemSync& sync = sockets_.sync();
    sync.lock();
    st = sockets_.bind_locked(id, name, host_path.view());
    if (st == UnixStatus::Ok) st = sockets_.listen_locked(id, process::UnixSocketManager::kMaxBacklog);
    sync.unlock();
  }
  if (st != UnixStatus::Ok) {
    sockets_.release(id);
    return false;
  }
  listener_ = id;
  listening_ = true;
  return true;
}

bool SocketServer::pump() {
  server_.tick();      // (commits waiting for their acquire point, the desktop's clipboard)
  drain_graveyard();
  bool moved = false;
  List<uint32_t> accepted(allocator_);
  List<uint64_t> accepted_pids(allocator_);
  Queue<process::UnixInFlight> received(allocator_);      // (all the connections', in order; each entry goes to `owners` too)
  List<Connection*> owners(allocator_);
  Queue<process::UnixInFlight> unsent(allocator_);
  process::ISystemSync& sync = sockets_.sync();
  sync.lock();
  woken_ = false;
  for (;;) {
    uint32_t id = 0;
    if (!listening_ || sockets_.accept_locked(listener_, &id) != UnixStatus::Ok) break;
    uint64_t pid = 0;
    sockets_.peer_pid_locked(id, &pid);
    if (!accepted.push_back(id) || !accepted_pids.push_back(pid)) {
      sync.unlock();
      sockets_.release(id);
      sync.lock();
      continue;
    }
    moved = true;
  }
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.dead) continue;
    // what is owed, as far as the socket takes it (the descriptors with the first bytes that go)
    while (c.sent < c.pending.size()) {
      uint32_t sent = 0;
      const UnixStatus st = sockets_.send_locked(c.socket, c.pending.view().subview(c.sent), nullptr, 0, c.pending_rights.count() ? &c.pending_rights : nullptr, &sent);
      if (st == UnixStatus::WouldBlock) break;
      if (st != UnixStatus::Ok) { c.dead = true; break; }
      c.sent += sent;
      moved = true;
      if (sent == 0) break;
    }
    // what came
    uint64_t taken = 0;
    while (!c.dead && taken < kMaxReadPerRound) {
      process::UnixSocketManager::Received got;
      Queue<process::UnixInFlight> rights(allocator_);
      const UnixStatus st = sockets_.receive_locked(c.socket, chunk_.writable_view(), false, &got, &rights);
      process::UnixInFlight entry;
      while (rights.dequeue(&entry)) {
        if (received.enqueue(util::move(entry)) && owners.push_back(&c)) continue;
        c.dead = true;      // (no memory to hold it: the connection cannot go on correctly)
      }
      if (st == UnixStatus::WouldBlock) break;
      if (st != UnixStatus::Ok || got.end_of_file) { c.dead = true; break; }
      if (got.got == 0) break;
      if (!c.inbox.append(chunk_.view().first(got.got))) { c.dead = true; break; }
      taken += got.got;
      moved = true;
    }
  }
  if (graveyard_) {
    kernel::ScopedMutex hold(graveyard_->lock);      // (progress_jobs_locked may leave descriptors there)
    moved |= progress_jobs_locked();
  }
  sync.unlock();
  moved |= finish_jobs();
  // the descriptors that came: memory files for the server; the others are let go (the server gets a stand-in with no memory)
  {
    process::UnixInFlight entry;
    for (uint64_t i = 0; received.dequeue(&entry); ++i) {
      Connection& c = *owners[i];
      SharedPointer<GpuImage> image;
      SharedPointer<GpuTimeline> timeline;
      const bool file = entry.kind == process::DescriptorKind::File && entry.object;
      if (file && entry.object->gpu_buffer && make_gpu_) image = make_gpu_(make_gpu_context_, allocator_, entry.object);
      if (file && entry.object->gpu_sync && make_timeline_) timeline = make_timeline_(make_gpu_context_, allocator_, entry.object);
      // (every kind is kept as it came, with its use of its token: a pipe's end is passed on to another client as it is)
      Fd fd = Fd::make_as<KernelFd>(allocator_, graveyard_, util::move(entry), util::move(image), util::move(timeline));
      if (!fd || !c.fds.push_back(util::move(fd))) c.dead = true;
    }
  }
  sockets_.release_rights(unsent);
  // new clients
  for (uint64_t i = 0; i < accepted.count(); ++i) {
    const uint32_t client = server_.open_client(accepted_pids[i]);
    UniquePointer<Connection> c = client ? UniquePointer<Connection>::make(allocator_, allocator_) : UniquePointer<Connection>();
    if (!c) {
      if (client) server_.close_client(client);
      sockets_.release(accepted[i]);      // (the table is full: the client sees the connection end)
      continue;
    }
    c->socket = accepted[i];
    c->client = client;
    if (!connections_.push_back(util::move(c))) {
      server_.close_client(client);
      sockets_.release(accepted[i]);
    }
  }
  // the server: requests in, events out
  for (UniquePointer<Connection>& p : connections_.view()) {
    Connection& c = *p;
    if (c.inbox.size() > 0 || c.fds.count() > 0) {
      server_.receive(c.client, c.inbox.view(), c.fds.view());
      c.inbox.clear();
      c.fds.clear();
    }
    if (c.sent == c.pending.size()) {
      c.pending.clear();
      c.sent = 0;
    }
    const uint64_t before = c.pending.size();
    List<Fd> fds(allocator_);
    if (server_.take_output(c.client, &c.pending, &fds)) c.closing = true;
    for (const Fd& fd : fds.view()) {
      if (!fd) continue;
      process::UnixInFlight entry = kernel_fd(fd).entry();      // (a copy goes, with a use of its own)
      if (entry.kind == process::DescriptorKind::Closed) continue;
      if (entry.token != 0) process::descriptor_token_add(entry.kind, entry.token);
      if (!c.pending_rights.enqueue(util::move(entry))) c.closing = true;
    }
    if (c.pending.size() != before) moved = true;
  }
  moved |= take_requests();
  // the connections that are over: gone, or done and everything sent
  for (uint64_t i = 0; i < connections_.count();) {
    Connection& c = *connections_[i];
    if (c.dead || (c.closing && c.sent == c.pending.size())) {
      close(c);
      connections_.remove_at(i);
      moved = true;
    } else {
      ++i;
    }
  }
  return moved;
}

void SocketServer::close(Connection& c) {
  server_.close_client(c.client);
  sockets_.release_rights(c.pending_rights);
  sockets_.release(c.socket);
}

bool SocketServer::ready_locked() {
  if (woken_) return true;
  if (listening_ && sockets_.readiness_locked(listener_).readable) return true;
  for (const UniquePointer<Connection>& p : connections_.view()) {
    const process::UnixReadiness r = sockets_.readiness_locked(p->socket);
    if (r.readable || r.error || r.hangup) return true;
    if (p->sent < p->pending.size() && r.writable) return true;
  }
  for (const UniquePointer<ReadJob>& j : reads_.view()) {
    const process::UnixReadiness r = sockets_.readiness_locked(j->socket);
    if (r.readable || r.error || r.hangup || r.read_hangup) return true;
  }
  for (const UniquePointer<WriteJob>& j : writes_.view())
    if (sockets_.readiness_locked(j->socket).writable) return true;
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
    for (uint32_t rounds = 0; rounds < 64 && s.pump(); ++rounds) {}
    s.wait(100u * 1000 * 1000);
  }
}

bool SocketServer::start() {
  if (thread_) return true;
  stopping_ = false;
  thread_ = kernel::spawn(&SocketServer::thread_main, this, "wayland");
  return thread_ != nullptr;
}

void SocketServer::stop() {
  if (!thread_) return;
  stopping_ = true;
  on_wake(this);
  kernel::join(thread_);
  thread_ = nullptr;
}

}  // namespace wayland
