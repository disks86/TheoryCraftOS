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
#include "Process/LinuxProcess.hpp"
#include "Process/LinuxFiles.hpp"
#include "Process/Process.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

// Linux's own asynchronous I/O (io_setup, io_submit, io_getevents, ...; what libaio wraps, not the C library's aio_read, which uses threads). Files here
// answer at once, so a request is carried out when it is submitted and its result waits in the context for io_getevents. A program sees the same thing
// it sees on Linux when the data was in the page cache.
namespace process::linuxabi {

namespace {

constexpr int64_t kEintr = 4, kEbadf = 9, kEagain = 11, kEfault = 14, kEinval = 22;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint64_t kContextBytes = 4096;      // the page a context's id points at (see sys_io_setup)
constexpr uint64_t kControlBytes = 64;        // struct iocb

LinuxProcessState::AioContext* context_of(Process& p, uint64_t id) {
  if (id == 0) return nullptr;
  for (LinuxProcessState::AioContext& context : p.linux_state().aio)
    if (context.id == id) return &context;
  return nullptr;
}

// IOCB_FLAG_RESFD: the eventfd to add one to for each request that completes.
void signal_event(Process& p, uint32_t fd) {
  if (p.files().kind(fd) != DescriptorKind::UnixSocket) return;
  UnixSocketManager* sockets = system_unix_sockets();
  if (!sockets) return;
  const uint32_t id = uint32_t(p.files().token(fd));
  sockets->sync().lock();
  if (sockets->is_event_locked(id)) {
    sockets->event_add_locked(id, 1);
    sockets->sync().notify_all();
  }
  sockets->sync().unlock();
}

}  // namespace

// io_setup(count, aio_context_t* context). The id handed back is the address of a page of the program's memory: libaio looks there for the kernel's ring of
// events before it makes a system call, finds none (the page is zeros), and calls io_getevents.
uint64_t sys_io_setup(Process& p, uint64_t count, uint64_t context_address) {
  uint64_t existing = 0;
  if (!p.read_user_u64(context_address, &existing)) return error(kEfault);
  if (existing != 0 || count == 0) return error(kEinval);
  if (count > 0x10000) return error(kEagain);      // (more than the system gives all contexts together: /proc/sys/fs/aio-max-nr)
  LinuxProcessState::AioContext* slot = nullptr;
  for (LinuxProcessState::AioContext& context : p.linux_state().aio)
    if (context.id == 0) { slot = &context; break; }
  if (!slot) return error(kEagain);
  const uint64_t page = map_user_memory(p, kContextBytes);
  if (page == 0) return error(12);
  if (!p.write_user_u64(context_address, page)) {
    unmap_user_memory(p, page, kContextBytes);
    return error(kEfault);
  }
  *slot = LinuxProcessState::AioContext();
  slot->id = page;
  slot->capacity = count < LinuxProcessState::kMaxAioEvents ? uint32_t(count) : LinuxProcessState::kMaxAioEvents;
  return 0;
}

uint64_t sys_io_destroy(Process& p, uint64_t id) {
  LinuxProcessState::AioContext* context = context_of(p, id);
  if (!context) return error(kEinval);
  unmap_user_memory(p, context->id, kContextBytes);
  *context = LinuxProcessState::AioContext();
  return 0;
}

// io_submit(context, count, struct iocb** requests): how many were taken. A request the call cannot take ends it: with the count so far, or, for the
// first one, with the reason.
uint64_t sys_io_submit(Process& p, uint64_t id, uint64_t count, uint64_t list) {
  constexpr uint32_t kRead = 0, kWrite = 1, kSync = 2, kDataSync = 3, kNothing = 6, kReadVector = 7, kWriteVector = 8;
  LinuxProcessState::AioContext* context = context_of(p, id);
  if (!context || int64_t(count) < 0) return error(kEinval);
  uint64_t done = 0;
  uint64_t failure = 0;
  for (; done < count; ++done) {
    uint64_t at = 0;
    uint8_t raw[kControlBytes];
    if (!p.read_user_u64(list + done * 8, &at) || !p.read_user(at, ByteView(raw, sizeof(raw)))) { failure = error(kEfault); break; }
    // struct iocb: aio_data (8), aio_key (4), aio_rw_flags (4), aio_lio_opcode (2), aio_reqprio (2), aio_fildes (4), aio_buf (8), aio_nbytes (8),
    // aio_offset (8), aio_reserved2 (8), aio_flags (4), aio_resfd (4)
    ByteReader reader{ConstByteView(raw, sizeof(raw))};
    const uint64_t data = reader.le64();
    reader.skip(4);
    const uint32_t rw_flags = reader.le32();
    const uint32_t opcode = reader.le16();
    reader.skip(2);
    const uint32_t fd = reader.le32();
    const uint64_t buffer = reader.le64(), bytes = reader.le64(), offset = reader.le64(), reserved = reader.le64();
    const uint32_t flags = reader.le32(), event_fd = reader.le32();
    if (reserved != 0 || (flags & ~uint32_t{3}) != 0 || rw_flags != 0) { failure = error(kEinval); break; }      // (flags: IOCB_FLAG_RESFD, IOCB_FLAG_IOPRIO)
    if (p.files().kind(fd) == DescriptorKind::Closed) { failure = error(kEbadf); break; }
    if ((flags & 1) && p.files().kind(event_fd) == DescriptorKind::Closed) { failure = error(kEbadf); break; }
    if (opcode != kRead && opcode != kWrite && opcode != kSync && opcode != kDataSync && opcode != kNothing && opcode != kReadVector && opcode != kWriteVector) {
      failure = error(kEinval);      // (IOCB_CMD_POLL among them: a request that would have to wait cannot be kept)
      break;
    }
    if (context->count >= context->capacity) { failure = error(kEagain); break; }      // (no room for its result until some are collected)
    uint64_t result = 0;
    switch (opcode) {
      case kRead: result = sys_pread(p, fd, buffer, bytes, offset); break;
      case kWrite: result = sys_pwrite(p, fd, buffer, bytes, offset); break;
      case kSync: case kDataSync: result = sys_fsync(p, fd); break;
      case kReadVector: result = sys_vector_at(p, fd, buffer, bytes, offset, 0, false, false); break;
      case kWriteVector: result = sys_vector_at(p, fd, buffer, bytes, offset, 0, true, false); break;
      default: break;
    }
    LinuxProcessState::AioEvent& event = context->events[(context->first + context->count) % LinuxProcessState::kMaxAioEvents];
    event.data = data;
    event.request = at;
    event.result = result;
    ++context->count;
    if (flags & 1) signal_event(p, event_fd);
  }
  if (done != 0 && p.runtime()) p.runtime()->notify();      // (a thread in io_getevents looks again)
  return done != 0 ? done : failure;
}

// io_cancel(context, struct iocb*, struct io_event*): nothing is ever in flight, so there is nothing to cancel.
uint64_t sys_io_cancel(Process& p, uint64_t id, uint64_t request, uint64_t) {
  if (!context_of(p, id)) return error(kEinval);
  uint8_t probe[8];
  if (!p.read_user(request, ByteView(probe, sizeof(probe)))) return error(kEfault);
  return error(kEagain);
}

// io_getevents(context, least, most, struct io_event* events, struct timespec* timeout): up to `most` results; with fewer than `least` there it waits
// (another thread may submit) for as long as the timeout says. struct io_event: data, obj (the request's address), res, res2 (8 bytes each).
uint64_t sys_io_getevents(Process& p, uint64_t id, uint64_t least, uint64_t most, uint64_t events, uint64_t timeout) {
  if (!context_of(p, id) || int64_t(least) < 0 || int64_t(most) < 0 || least > most) return error(kEinval);
  uint64_t wait_ns = ~uint64_t{0};
  if (timeout != 0) {
    uint64_t seconds = 0, nanoseconds = 0;
    if (!p.read_user_u64(timeout, &seconds) || !p.read_user_u64(timeout + 8, &nanoseconds)) return error(kEfault);
    if (int64_t(seconds) < 0 || nanoseconds >= 1000000000u) return error(kEinval);
    wait_ns = seconds > (~uint64_t{0} - 999999999u) / 1000000000u ? ~uint64_t{0} - 1 : seconds * 1000000000u + nanoseconds;
  }
  const uint64_t started = p.host().monotonic_ns();
  uint64_t given = 0;
  for (;;) {
    LinuxProcessState::AioContext* context = context_of(p, id);
    if (!context) return given != 0 ? given : error(kEinval);      // (destroyed by another thread meanwhile)
    while (given < most && context->count != 0) {
      const LinuxProcessState::AioEvent& event = context->events[context->first];
      uint8_t raw[32];
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      writer.le64(event.data);
      writer.le64(event.request);
      writer.le64(event.result);
      writer.le64(0);
      if (!p.write_user(events + given * 32, ConstByteView(raw, sizeof(raw)))) return given != 0 ? given : error(kEfault);
      context->first = (context->first + 1) % LinuxProcessState::kMaxAioEvents;
      --context->count;
      ++given;
    }
    if (given >= least) return given;
    const uint64_t used = p.host().monotonic_ns() - started;
    if (wait_ns != ~uint64_t{0} && used >= wait_ns) return given;
    if (p.exited() || p.signal_interrupts() || (p.runtime() && p.runtime()->interrupted())) return given != 0 ? given : error(kEintr);
    const uint64_t left = wait_ns == ~uint64_t{0} ? ~uint64_t{0} : wait_ns - used;
    if (p.runtime()) {
      p.runtime()->wait(p.wait_slice(left));
    } else {      // (one thread and nobody else: nothing more can come; the time is waited out, and a wait without an end has nothing to wait for)
      if (left == ~uint64_t{0}) return given;
      p.block_sleep(left);
    }
  }
}

}  // namespace process::linuxabi
