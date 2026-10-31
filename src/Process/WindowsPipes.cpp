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
#include "Process/WindowsPipes.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsWait.hpp"
#include "Utility/ByteBuffer.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kInvalidDeviceRequest = 0xC0000010u,
                   kAccessDenied = 0xC0000022u, kObjectNameNotFound = 0xC0000034u, kPipeBusy = 0xC00000AEu, kPipeConnected = 0xC00000AFu, kPipeDisconnected = 0xC00000B0u,
                   kPipeClosing = 0xC00000B1u, kPipeBroken = 0xC000014Bu, kPipeEmpty = 0xC00000D9u, kNotImplemented = 0xC0000002u, kInvalidInfoClass = 0xC0000003u,
                   kInfoLengthMismatch = 0xC0000004u, kTimeout = 0x102, kAlerted = 0x101, kNoMemory = 0xC0000017u, kNoResources = 0xC000009Au;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;
constexpr uint64_t kPending = kWaitPending;
constexpr uint32_t kNotSupported = 0xC00000BBu;      // (what the library's BEGIN calls answer when the call is to be made the usual way)
constexpr uint32_t kNotFound = 0xC0000225u;
// The library's own FSCTLs (see nt_fs_control_file).
constexpr uint32_t kFsctlBeginRead = 0x11FFF0, kFsctlBeginWrite = 0x11FFF1, kFsctlBeginListen = 0x11FFF2, kFsctlAwait = 0x11FFF3, kFsctlAck = 0x11FFF4, kFsctlCancel = 0x11FFF5, kFsctlBeginTransact = 0x11FFF6, kFsctlGetProcess = 0x11FFF7, kFsctlSetPort = 0x11FFF8, kFsctlTakePort = 0x11FFF9;

// The token of a pipe descriptor: bits 0..31 the object, 32..55 the end code, 60 read, 61 write, 62 non-blocking, 63 the pipe folder itself.
constexpr uint32_t kCancelled = 0xC0000120u;      // STATUS_CANCELLED
constexpr uint32_t kMoreData = 0x80000005u;      // STATUS_BUFFER_OVERFLOW (ERROR_MORE_DATA): part of a message was read
constexpr uint64_t kMessageRead = 1ull << 59, kRead = 1ull << 60, kWrite = 1ull << 61, kNoWait = 1ull << 62, kFolder = 1ull << 63;
uint64_t make_token(uint32_t id, uint32_t end_code, bool read, bool write) { return id | (uint64_t{end_code} << 32) | (read ? kRead : 0) | (write ? kWrite : 0); }
uint32_t token_id(uint64_t token) { return uint32_t(token); }
uint32_t token_end(uint64_t token) { return uint32_t((token >> 32) & 0xFFFFFF); }

bool slot_of(uint64_t handle, uint64_t* slot) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return false;
  *slot = handle / 4 - 1;
  return true;
}

// The pipe name in an OBJECT_ATTRIBUTES, when it is one: the part after "\\??\\pipe\\" or "\\Device\\NamedPipe\\" in UTF-8 (empty for the folder). False if the path is something else.
bool pipe_name(Process& p, uint64_t attributes, String* name) {
  if (attributes == 0) return false;
  uint64_t name_at = 0;
  if (!p.read_user_u64(attributes + 16, &name_at) || name_at == 0) return false;
  String text(p.files().allocator());
  if (read_unicode_string_argument(p, name_at, &text) != kSuccess) return false;
  static const char* const prefixes[] = {"\\??\\pipe\\", "\\Device\\NamedPipe\\", "\\??\\pipe", "\\Device\\NamedPipe"};
  for (const char* prefix : prefixes) {
    uint64_t n = 0;
    while (prefix[n]) ++n;
    bool same = text.size() >= n;
    for (uint64_t i = 0; same && i < n; ++i) {
      char a = text.view()[i], b = prefix[i];
      if (a >= 'A' && a <= 'Z') a = char(a + 32);
      if (b >= 'A' && b <= 'Z') b = char(b + 32);
      same = a == b;
    }
    if (!same) continue;
    name->assign(text.view().without_prefix(n));
    return true;
  }
  return false;
}

uint32_t result_status(ObjectManager::PipeResult r, bool server, bool write) {
  switch (r) {
    case ObjectManager::PipeResult::Ok: return kSuccess;
    case ObjectManager::PipeResult::Broken: return kPipeBroken;
    case ObjectManager::PipeResult::Closing: return kPipeClosing;
    case ObjectManager::PipeResult::MoreData: return kMoreData;
    case ObjectManager::PipeResult::Listening: return 0xC00000B3u;      // STATUS_PIPE_LISTENING
    case ObjectManager::PipeResult::NoResources: return kNoResources;
    case ObjectManager::PipeResult::InvalidState: return 0xC00000ADu;      // STATUS_INVALID_PIPE_STATE (ERROR_BAD_PIPE)
    case ObjectManager::PipeResult::Busy: return kPipeBusy;
    case ObjectManager::PipeResult::NotConnected: (void)server; (void)write; return kPipeDisconnected;
    default: return kPipeEmpty;
  }
}

uint32_t give_pipe_handle(Process& p, uint64_t token, bool inherit, uint64_t* handle) {
  const int64_t slot = p.files().open_token(DescriptorKind::Pipe, token);
  if (slot < 0) return kNoResources;
  p.files().set_handle_flags(uint64_t(slot), inherit, false);
  *handle = (uint64_t(slot) + 1) * 4;
  return kSuccess;
}

bool wants_inherit(Process& p, uint64_t attributes) {
  uint8_t raw[4];
  return attributes != 0 && p.read_user(attributes + 24, ByteView(raw, 4)) && (raw[0] & 2) != 0;
}

// The operations of other threads that this call finished have been acted on (so that their results are there when the call returns); a thread that does not come is given
// up on after a second.
void await_delivery(Process& p, uint32_t id) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  const uint32_t s = wait_until(p, 1000ull * 1000000, [&]() -> uint64_t { return objects->pipe_delivered_locked(id) ? kSuccess : kPending; });
  if (s == kTimeout) {
    objects->sync().lock();
    objects->pipe_forgive_locked(id);
    objects->sync().unlock();
  }
}

// A call that waits in the kernel for this thread: while it does the file object is not signalled, and CancelSynchronousIo can end it.
class SyncWait {
 public:
  SyncWait(Process& p, uint32_t id, uint32_t end_code) : objects_(system_objects()) {
    if (objects_) slot_ = objects_->pipe_sync_begin(id, end_code, p.runtime() ? p.runtime()->current_thread_id() : p.id());
  }
  ~SyncWait() { if (objects_) objects_->pipe_sync_end(slot_); }
  SyncWait(const SyncWait&) = delete;
  SyncWait& operator=(const SyncWait&) = delete;
  bool cancelled_locked() const { return objects_ && objects_->pipe_sync_cancelled_locked(slot_); }

 private:
  ObjectManager* objects_;
  uint32_t slot_ = 0;
};

// Waits for an operation made with pipe_begin_*_locked to finish: its status (the result is in *result). One that cannot go on (the wait was interrupted) is cancelled.
uint32_t wait_operation(Process& p, uint32_t id, uint32_t end_code, uint32_t tag, ObjectManager::PipeOpResult* result, const SyncWait* waiting = nullptr) {
  ObjectManager* objects = system_objects();
  bool finished = false;
  const uint32_t s = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t {
    if (waiting && waiting->cancelled_locked()) return kCancelled;
    switch (objects->pipe_op_poll_locked(id, end_code, tag, result)) {
      case ObjectManager::PipeOpPoll::Pending: return kPending;
      case ObjectManager::PipeOpPoll::Done: finished = true; return kSuccess;
      default: return kNotFound;
    }
  });
  if (finished) return result->status;
  if (s != kNotFound && !p.exited()) {      // (interrupted: the call does not go on; one of a process that ends is taken care of when it has ended)
    objects->sync().lock();
    objects->pipe_op_cancel_locked(id, end_code, tag, kCancelled);
    objects->sync().unlock();
  }
  return s;
}

}  // namespace

void pipe_closed(Process& p, uint64_t token) {
  if (!(token & kFolder)) await_delivery(p, token_id(token));
}

bool pipe_handle_gone_locked(Process& p, uint64_t slot) {
  ObjectManager* objects = system_objects();
  const uint64_t token = p.files().token(slot);
  return objects && !(token & kFolder) && (objects->pipe_client_gone_locked(token_id(token), token_end(token)) || !objects->pipe_file_signaled_locked(token_id(token), token_end(token)));
}

void pipe_token_add(uint64_t token) {
  if (token & kFolder) return;
  if (ObjectManager* objects = system_objects()) objects->add_pipe_end(token_id(token), token_end(token));
}
void pipe_token_release(uint64_t token) {
  if (token & kFolder) return;
  if (ObjectManager* objects = system_objects()) objects->release_pipe_end(token_id(token), token_end(token));
}

uint32_t nt_create_pipe(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  if (r.args[0] == 0 || r.args[1] == 0) return kInvalidParameter;
  const ObjectManager::Created c = objects->create_pipe(StringView(), uint32_t(r.args[3]), 0, 0, ObjectManager::kPipeOutbound, false, p.id());
  if (c.id == 0) return c.status;
  objects->add_pipe_end(c.id, 1);      // (the pipe is made with its write side counted; the read side is end 1, generation 0)
  const bool inherit = wants_inherit(p, r.args[2]);
  uint64_t write_handle = 0, read_handle = 0;
  uint32_t status = give_pipe_handle(p, make_token(c.id, 0, false, true), inherit, &write_handle);
  if (status == kSuccess) status = give_pipe_handle(p, make_token(c.id, 1, true, false), inherit, &read_handle);
  if (status == kSuccess && (!p.write_user_u64(r.args[0], read_handle) || !p.write_user_u64(r.args[1], write_handle))) status = kAccessViolation;
  if (status != kSuccess) {
    if (write_handle) p.close_descriptor(write_handle / 4 - 1);
    if (read_handle) p.close_descriptor(read_handle / 4 - 1);
  }
  return status;
}

uint32_t nt_create_named_pipe_file(Process& p, const SyscallRequest& r) {
  uint64_t status_block = 0, share = 0, max_instances = 0, in_quota = 0, out_quota = 0, pipe_type = 0, read_mode = 0, completion = 0;
  if (!argument(p, r, 3, &status_block) || !argument(p, r, 4, &share) || !argument(p, r, 7, &pipe_type) || !argument(p, r, 8, &read_mode) || !argument(p, r, 9, &completion) || !argument(p, r, 10, &max_instances) || !argument(p, r, 11, &in_quota) || !argument(p, r, 12, &out_quota))
    return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  String name(p.files().allocator());
  if (!pipe_name(p, r.args[2], &name) || name.empty()) return 0xC0000033u;
  { uint64_t slashes = 0; for (const char ch : name.view()) if (ch == '\\') ++slashes; if (slashes > 1 || (slashes == 1 && name.view()[name.size() - 1] != '\\')) return 0xC0000033u; }      // (a trailing backslash is part of the name; no other folders)      // STATUS_OBJECT_NAME_INVALID
  const uint32_t direction = uint32_t(share) & 3;      // FILE_SHARE_READ: clients read (the server writes); FILE_SHARE_WRITE: clients write (the server reads)
  uint32_t access = 0;
  if (direction == 1) access = ObjectManager::kPipeOutbound;
  else if (direction == 2) access = ObjectManager::kPipeInbound;
  else if (direction == 3) access = ObjectManager::kPipeDuplex;
  else return kInvalidParameter;
  const ObjectManager::Created c = objects->create_pipe(name.view(), uint32_t(out_quota), uint32_t(in_quota), uint32_t(max_instances) == 0xFFFFFFFFu ? 0 : uint32_t(max_instances), access, (pipe_type & 1) != 0, p.id());
  if (c.id == 0) return c.status;
  uint64_t handle = 0;
  uint64_t server_token = make_token(c.id, 0, (access & ObjectManager::kPipeInbound) != 0, (access & ObjectManager::kPipeOutbound) != 0);
  if ((pipe_type & 1) && (read_mode & 1)) server_token |= kMessageRead;
  if (completion & 1) server_token |= kNoWait;
  uint32_t status = give_pipe_handle(p, server_token, wants_inherit(p, r.args[2]), &handle);
  if (status != kSuccess) { objects->release_pipe_end(c.id, 0); return status; }
  if (!p.write_user_u64(r.args[0], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  uint8_t iosb[16] = {};
  iosb[8] = 2;      // FILE_CREATED
  p.write_user(status_block, ConstByteView(iosb, 16));
  return kSuccess;
}

uint32_t nt_cancel_synchronous_io_file(Process& p, const SyscallRequest& r) {
  // NtCancelSynchronousIoFile(ThreadHandle, PIO_STATUS_BLOCK IoRequestToCancel, PIO_STATUS_BLOCK IoStatusBlock): ends the call the thread waits in on a pipe
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint64_t thread = 0;
  if (r.args[0] == ~uint64_t{1} || r.args[0] == 0) {
    thread = p.runtime() ? p.runtime()->current_thread_id() : p.id();
  } else {
    uint64_t slot = 0;
    if (!slot_of(r.args[0], &slot) || p.files().kind(slot) != DescriptorKind::Thread) return kInvalidHandle;
    thread = p.files().token(slot);
  }
  const uint32_t status = objects->pipe_cancel_synchronous(thread) ? kSuccess : kNotFound;
  if (r.args[2] != 0) {
    uint8_t iosb[16] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)};
    p.write_user(r.args[2], ConstByteView(iosb, 16));
  }
  return status;
}

bool pipe_open_by_name(Process& p, uint64_t object_attributes, uint32_t access, uint32_t* status, uint64_t* handle) {
  String name(p.files().allocator());
  if (!pipe_name(p, object_attributes, &name)) return false;
  ObjectManager* objects = system_objects();
  if (!objects) { *status = kNotImplemented; return true; }
  const bool inherit = wants_inherit(p, object_attributes);
  if (name.empty()) {      // the folder: a handle to ask which pipes are available
    *status = give_pipe_handle(p, kFolder, inherit, handle);
    return true;
  }
  const bool read = (access & (0x80000000u | 0x1)) != 0, write = (access & (0x40000000u | 0x2 | 0x4)) != 0;      // GENERIC_READ/FILE_READ_DATA, GENERIC_WRITE/FILE_WRITE_DATA/FILE_APPEND_DATA
  const ObjectManager::Created c = objects->connect_pipe(name.view(), read, write, p.id());
  if (c.id == 0) { *status = c.status; return true; }
  *status = give_pipe_handle(p, make_token(c.id, c.end_code, read || !write, write), inherit, handle);
  if (*status != kSuccess) objects->release_pipe_end(c.id, c.end_code);
  await_delivery(p, c.id);      // (a ConnectNamedPipe that waited has its answer when the client's open returns)
  return true;
}

uint32_t pipe_io(Process& p, uint64_t slot, bool write, uint64_t status_block, uint64_t buffer, uint64_t length) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  if (token & kFolder) return kInvalidDeviceRequest;
  if (write ? !(token & kWrite) : !(token & kRead)) return kAccessDenied;
  const uint32_t id = token_id(token), end = token_end(token);
  const bool no_wait = (token & kNoWait) != 0;
  uint8_t chunk[8192];
  uint64_t done = 0;
  uint32_t status = kSuccess;
  if (write && length == 0) {      // (an empty write still tells a broken pipe from a good one)
    status = wait_until(p, 0, [&]() -> uint64_t {
      uint32_t moved = 0;
      const ObjectManager::PipeResult res = objects->pipe_write_locked(id, end, chunk, 0, &moved);
      return res == ObjectManager::PipeResult::WouldBlock ? kPending : result_status(res, false, true);
    });
  } else if (write && no_wait) {      // (what the reads that wait take and what fits goes; the rest is not written, and nothing waits)
    const uint64_t n = length < ObjectManager::kMaxPipeWrite ? length : ObjectManager::kMaxPipeWrite;
    ByteBuffer staging(p.files().allocator());
    if (!staging.put_zeros(n)) return kNoMemory;
    if (!p.read_user(buffer, staging.writable_view())) return kAccessViolation;
    uint32_t moved = 0;
    objects->sync().lock();
    const ObjectManager::PipeResult res = objects->pipe_write_nowait_locked(id, end, staging.view(), &moved);
    objects->sync().unlock();
    status = result_status(res, false, true);
    done = moved;
  } else if (write) {      // the write is queued whole, and complete when enough was read to make room for all of it
    uint32_t failure = kSuccess;
    ByteBuffer staging(p.files().allocator());
    while (done < length && failure == kSuccess) {
      const uint64_t n = length - done < ObjectManager::kMaxPipeWrite ? length - done : ObjectManager::kMaxPipeWrite;
      ConstByteView piece;
      if (n <= sizeof(chunk)) {
        if (!p.read_user(buffer + done, ByteView(chunk, n))) { if (done == 0) return kAccessViolation; break; }
        piece = ConstByteView(chunk, n);
      } else {
        staging.clear();
        if (!staging.put_zeros(n)) { failure = kNoMemory; break; }
        if (!p.read_user(buffer + done, staging.writable_view())) { if (done == 0) return kAccessViolation; break; }
        piece = staging.view();
      }
      uint32_t tag = 0, moved = 0;
      ObjectManager::PipeResult res = ObjectManager::PipeResult::Ok;
      const uint32_t queued = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t {
        tag = 0;
        res = objects->pipe_begin_write_locked(id, end, piece, p.id(), false, &tag, &moved);
        return res == ObjectManager::PipeResult::WouldBlock && tag == 0 ? kPending : kSuccess;      // (no room in the tables: later)
      });
      if (queued != kSuccess) { failure = queued; break; }
      if (res == ObjectManager::PipeResult::WouldBlock) {
        ObjectManager::PipeOpResult result;
        SyncWait waiting(p, id, end);
        failure = wait_operation(p, id, end, tag, &result, &waiting);
        objects->pipe_op_release(id, tag);
        objects->pipe_op_free_data(result.data);
        if (failure == kSuccess) done += n;
      } else if (res == ObjectManager::PipeResult::Ok) {
        done += moved;
      } else {
        failure = result_status(res, false, true);
      }
    }
    status = done > 0 ? kSuccess : failure;
  } else if (length != 0) {
    bool first = true;
    while (done < length) {      // (what is there now, up to the length: only the first read waits)
      const uint32_t want = uint32_t(length - done < sizeof(chunk) ? length - done : sizeof(chunk));
      uint32_t got = 0;
      auto attempt = [&]() -> uint64_t {
        const ObjectManager::PipeResult res = objects->pipe_read_locked(id, end, chunk, want, &got, false, nullptr, (token & kMessageRead) != 0);
        if (res == ObjectManager::PipeResult::WouldBlock) return kPending;
        return result_status(res, false, false);
      };
      uint32_t s = wait_until(p, 0, attempt);
      if (s == kTimeout && first && !no_wait) {      // (nothing to read yet: the call waits)
        SyncWait waiting(p, id, end);
        s = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t { return waiting.cancelled_locked() ? uint64_t{kCancelled} : attempt(); });
      }
      if (s != kSuccess && s != kMoreData) {
        if (first) status = s == kTimeout ? kPipeEmpty : s;
        break;
      }
      if (!p.write_user(buffer + done, ConstByteView(chunk, got))) return kAccessViolation;
      done += got;
      first = false;
      if (s == kMoreData) { status = kMoreData; break; }
      if (got == 0 || (token & kMessageRead)) break;      // (one message)
    }
  } else {      // a read of no bytes: waits for something to read (or for the pipe to break) and takes nothing
    uint32_t got = 0;
    status = wait_until(p, no_wait ? 0 : ~uint64_t{0}, [&]() -> uint64_t {
      const ObjectManager::PipeResult res = objects->pipe_read_locked(id, end, chunk, 0, &got, true, nullptr, (token & kMessageRead) != 0);
      if (res == ObjectManager::PipeResult::WouldBlock) return kPending;
      return res == ObjectManager::PipeResult::MoreData ? kSuccess : result_status(res, false, false);
    });
    if (status == kTimeout) status = kPipeEmpty;
  }
  await_delivery(p, id);      // (the calls of other threads that this one finished are over before it returns)
  if (status != kSuccess) {
    if (status_block != 0) {
      uint8_t iosb[16] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)};
      if (status == kMoreData) for (uint32_t i = 0; i < 8; ++i) iosb[8 + i] = uint8_t(done >> (8 * i));      // (the part that was read)
      p.write_user(status_block, ConstByteView(iosb, 16));
    }
    return status;
  }
  uint8_t iosb[16] = {};
  for (uint32_t i = 0; i < 8; ++i) iosb[8 + i] = uint8_t(done >> (8 * i));
  if (status_block != 0 && !p.write_user(status_block, ConstByteView(iosb, 16))) return kAccessViolation;
  return kSuccess;
}

uint32_t pipe_flush(Process& p, uint64_t slot) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  if (token & kFolder) return kInvalidDeviceRequest;
  if (!(token & kWrite)) return kSuccess;
  const uint32_t id = token_id(token), end = token_end(token);
  uint32_t tag = 0;
  objects->sync().lock();
  const ObjectManager::PipeResult res = objects->pipe_begin_flush_locked(id, end, p.id(), &tag);      // (what this end wrote and the other has not read)
  objects->sync().unlock();
  if (res == ObjectManager::PipeResult::Ok) return kSuccess;
  if (res != ObjectManager::PipeResult::WouldBlock) return kNoResources;
  ObjectManager::PipeOpResult result;
  SyncWait waiting(p, id, end);
  const uint32_t status = wait_operation(p, id, end, tag, &result, &waiting);
  objects->pipe_op_release(id, tag);
  return status;
}

uint32_t pipe_query_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t status_block, uint64_t buffer, uint32_t length) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  uint8_t out[40] = {};
  auto put32 = [&](uint32_t at, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  uint32_t size = 0;
  if (info_class == 8) {      // FileAccessInformation: what the handle may do, as the end and the pipe's direction give it
    constexpr uint32_t kReadSet = 0x20000 | 0x80 | 0x8 | 0x1, kWriteSet = 0x20000 | 0x100 | 0x10 | 0x4 | 0x2;      // READ_CONTROL+RA+RE+RD / READ_CONTROL+WA+WE+AD+WD
    uint32_t mask = 0x100000;      // SYNCHRONIZE
    ObjectManager::PipeInfo info;
    if (!(token & kFolder) && !objects->pipe_info(token_id(token), &info)) return kInvalidHandle;
    if (token & kFolder) mask |= 0x80 | 0x20000;
    else if (info.anonymous) mask |= (token & kRead) ? (kReadSet | 0x100) : (kWriteSet | 0x80);
    else {
      const bool read = (token & kRead) != 0, write = (token & kWrite) != 0;
      if (token_end(token) & 1) {      // a client: what it asked for (nothing but the attributes if it asked for nothing)
        mask |= write ? kWriteSet : 0;
        mask |= read && write ? kReadSet : 0;
        if (!write) mask |= 0x80;
        if (write && !read) mask |= 0;
      } else {
        if (read) mask |= kReadSet;
        if (write) mask |= kWriteSet;
      }
    }
    put32(0, mask);
    size = 4;
  } else if (info_class == 23) {      // FILE_PIPE_INFORMATION { ReadMode, CompletionMode }
    put32(0, (token & kMessageRead) ? 1 : 0);
    put32(4, (token & kNoWait) ? 1 : 0);
    size = 8;
  } else if (info_class == 24) {      // FILE_PIPE_LOCAL_INFORMATION
    if (token & kFolder) return kInvalidDeviceRequest;
    ObjectManager::PipeInfo info;
    if (!objects->pipe_info(token_id(token), &info)) return kInvalidHandle;
    const uint32_t end = token_end(token) & 1;
    uint32_t available = 0, got = 0;
    uint8_t probe[1];
    objects->sync().lock();
    objects->pipe_read_locked(token_id(token), token_end(token), probe, 1, &got, true, &available);
    objects->sync().unlock();
    put32(0, info.message_type ? 1 : 0);                   // NamedPipeType: byte stream / message
    put32(4, info.anonymous ? 1 : (info.access == ObjectManager::kPipeInbound ? 0 : info.access == ObjectManager::kPipeOutbound ? 1 : 2));      // NamedPipeConfiguration (inbound 0, outbound 1, duplex 2)
    put32(8, info.anonymous ? 1 : (info.max_instances ? info.max_instances : 255));
    put32(12, info.instances);
    put32(16, info.anonymous ? info.out_size : info.in_size);
    put32(20, available);
    put32(24, info.out_size);
    {      // WriteQuotaAvailable
      objects->sync().lock();
      const uint32_t room = objects->pipe_room_locked(token_id(token), token_end(token));
      objects->sync().unlock();
      put32(28, room);
    }
    put32(32, info.connected ? 3 : 2);                     // NamedPipeState: FILE_PIPE_CONNECTED_STATE / LISTENING
    put32(36, (info.anonymous ? end == 1 : end == 0) ? 1 : 0);      // NamedPipeEnd: 1 server, 0 client (an anonymous pipe's read end counts as the server's)
    size = 40;
  } else {
    return kInvalidInfoClass;
  }
  if (length < size) return kInfoLengthMismatch;
  if (!p.write_user(buffer, ConstByteView(out, size))) return kAccessViolation;
  if (status_block != 0) {
    uint8_t iosb[16] = {};
    iosb[8] = uint8_t(size);
    p.write_user(status_block, ConstByteView(iosb, 16));
  }
  return kSuccess;
}

uint32_t pipe_set_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t buffer, uint32_t length) {
  if (info_class != 23) return kInvalidInfoClass;
  if (length < 8) return kInfoLengthMismatch;
  uint8_t raw[8];
  if (!p.read_user(buffer, ByteView(raw, 8))) return kAccessViolation;
  uint64_t token = p.files().token(slot);
  if (raw[0] > 1 || raw[4] > 1) return kInvalidParameter;
  if (raw[0]) {      // message read mode: only a message-type pipe has messages
    ObjectManager* objects = system_objects();
    ObjectManager::PipeInfo info;
    if (!objects || !objects->pipe_info(token_id(token), &info) || !info.message_type) return kInvalidParameter;
  }
  token = raw[0] ? (token | kMessageRead) : (token & ~kMessageRead);
  token = raw[4] ? (token | kNoWait) : (token & ~kNoWait);      // CompletionMode: FILE_PIPE_COMPLETE_OPERATION (1) fails instead of waiting
  p.files().set_token(slot, token);
  return kSuccess;
}

bool pipe_handle_name(Process& p, uint64_t slot, String* name) {
  ObjectManager* objects = system_objects();
  if (!objects || p.files().kind(slot) != DescriptorKind::Pipe) return false;
  char text[ObjectManager::kNameBytes];
  const uint32_t size = objects->object_name(token_id(p.files().token(slot)), text, sizeof(text));
  return name->assign(StringView(text, size));
}

uint32_t nt_fs_control_file(Process& p, const SyscallRequest& r) {
  uint64_t status_block = 0, code = 0, input = 0, input_length = 0, output = 0, output_length = 0;
  if (!argument(p, r, 4, &status_block) || !argument(p, r, 5, &code) || !argument(p, r, 6, &input) || !argument(p, r, 7, &input_length) || !argument(p, r, 8, &output) ||
      !argument(p, r, 9, &output_length))
    return kAccessViolation;
  uint64_t slot = 0;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) == DescriptorKind::File) return file_fs_control(p, slot, status_block, uint32_t(code), output, output_length & 0xFFFFFFFFu);
  ObjectManager* objects = system_objects();
  if (p.files().kind(slot) == DescriptorKind::Semaphore && uint32_t(code) == kFsctlTakePort) {      // (the library's own) a packet the kernel kept for the port: { u32 bytes, u32 status, u64 key, u64 overlapped }
    ObjectManager::PortPacket packet;
    if (!objects || output_length < 24 || !objects->port_take_foreign(uint32_t(p.files().token(slot)), &packet)) return kNotFound;
    uint8_t out[24] = {};
    for (uint32_t i = 0; i < 4; ++i) { out[i] = uint8_t(packet.bytes >> (8 * i)); out[4 + i] = uint8_t(packet.status >> (8 * i)); }
    for (uint32_t i = 0; i < 8; ++i) { out[8 + i] = uint8_t(packet.key >> (8 * i)); out[16 + i] = uint8_t(packet.overlapped >> (8 * i)); }
    return p.write_user(output, ConstByteView(out, 24)) ? kSuccess : kAccessViolation;
  }
  if (p.files().kind(slot) != DescriptorKind::Pipe) return kInvalidDeviceRequest;
  if (!objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  const uint32_t id = token_id(token), end = token_end(token);
  input_length &= 0xFFFFFFFFu;
  output_length &= 0xFFFFFFFFu;
  uint64_t information = 0;
  uint32_t status = kInvalidDeviceRequest;
  switch (uint32_t(code)) {
    case 0x110008: {      // FSCTL_PIPE_LISTEN: wait for a client
      if (token & kFolder) break;
      if (token_end(token) & 1) { status = kInvalidDeviceRequest; break; }
      if (input_length == 4) {      // (the library's own: only say that the server listens, without waiting for the client)
        objects->sync().lock();
        const ObjectManager::PipeResult res = objects->pipe_listen_locked(id);
        objects->sync().unlock();
        status = res == ObjectManager::PipeResult::Ok ? kSuccess : 0x103u;
        break;
      }
      bool first = true;
      auto attempt = [&]() -> uint64_t {
        const ObjectManager::PipeResult res = objects->pipe_listen_locked(id);
        const bool was_connected = first;
        first = false;
        if (res == ObjectManager::PipeResult::Ok) return was_connected ? kPipeConnected : kSuccess;      // (a client that came before the call is reported as such, as NT does)
        if (res == ObjectManager::PipeResult::Closing) return was_connected ? kPipeClosing : kSuccess;      // (and one that came and went)
        if (res == ObjectManager::PipeResult::WouldBlock) return kPending;
        return kPipeBroken;
      };
      status = wait_until(p, 0, attempt);
      if (status == kTimeout && !(token & kNoWait)) {      // (nobody is there yet: the call waits)
        SyncWait waiting(p, id, end);
        status = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t { return waiting.cancelled_locked() ? uint64_t{kCancelled} : attempt(); });
      }
      if (status == kTimeout) status = 0xC00000B3u;      // STATUS_PIPE_LISTENING: a pipe that does not wait says that nobody is there yet
      break;
    }
    case kFsctlBeginRead:      // (the library's own) start a read that may have to wait: { u32 length } -> { u32 tag }; STATUS_PENDING if it waits, kNotSupported if not
    case kFsctlBeginWrite:      // { the bytes } -> { u32 tag }; STATUS_PENDING if the write is queued and not complete, success (with the count) if it went, kNotSupported to write the usual way
    case kFsctlBeginListen: {      // -> { u32 tag }
      if ((token & kFolder) || output_length < 4) { status = kInvalidParameter; break; }
      uint32_t tag = 0, length = 0;
      ObjectManager::PipeResult res = ObjectManager::PipeResult::Ok;
      status = kNotSupported;
      if (uint32_t(code) == kFsctlBeginRead) {
        uint8_t raw[4];
        if (input_length < 4 || !p.read_user(input, ByteView(raw, 4))) { status = kInvalidParameter; break; }
        length = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
        if (token & kNoWait) break;
        objects->sync().lock();
        res = objects->pipe_begin_read_locked(id, end, length, (token & kMessageRead) != 0, p.id(), true, &tag);
        objects->sync().unlock();
      } else if (uint32_t(code) == kFsctlBeginListen) {
        if ((end & 1) || (token & kNoWait)) { status = (end & 1) ? kInvalidDeviceRequest : kNotSupported; break; }
        objects->sync().lock();
        res = objects->pipe_begin_listen_locked(id, p.id(), true, &tag);
        objects->sync().unlock();
      } else {
        if ((token & kNoWait) || input_length > ObjectManager::kMaxPipeWrite) break;
        ByteBuffer data(p.files().allocator());
        if (!data.put_zeros(input_length)) { status = kNoMemory; break; }
        if (input_length != 0 && !p.read_user(input, data.writable_view())) return kAccessViolation;
        uint32_t moved = 0;
        objects->sync().lock();
        res = objects->pipe_begin_write_locked(id, end, data.view(), p.id(), true, &tag, &moved);
        objects->sync().unlock();
        if (res == ObjectManager::PipeResult::Ok) { status = kSuccess; information = moved; await_delivery(p, id); break; }
        if (res != ObjectManager::PipeResult::WouldBlock) { status = result_status(res, false, true); break; }
        if (tag == 0) break;      // (no room in the tables: the usual way waits for it)
      }
      if (res != ObjectManager::PipeResult::WouldBlock) { if (uint32_t(code) == kFsctlBeginWrite) break; status = kNotSupported; break; }
      const uint8_t out_tag[4] = {uint8_t(tag), uint8_t(tag >> 8), uint8_t(tag >> 16), uint8_t(tag >> 24)};
      if (!p.write_user(output, ConstByteView(out_tag, 4))) {
        objects->sync().lock();
        objects->pipe_op_cancel_locked(id, end, tag, kCancelled);
        objects->sync().unlock();
        objects->pipe_op_release(id, tag);
        return kAccessViolation;
      }
      status = 0x103;      // STATUS_PENDING
      break;
    }
    case kFsctlGetProcess: {      // (the library's own) { u32 which: 0 the client, 1 the server } -> { u32 process id }
      uint8_t raw[4];
      if ((token & kFolder) || input_length < 4 || output_length < 4 || !p.read_user(input, ByteView(raw, 4))) { status = kInvalidParameter; break; }
      uint64_t pid = 0;
      const ObjectManager::PipeResult res = objects->pipe_process(id, end, raw[0] & 1, &pid);
      if (res == ObjectManager::PipeResult::Listening) { status = kNotFound; break; }
      if (res != ObjectManager::PipeResult::Ok) { status = result_status(res, false, false); break; }
      const uint8_t out[4] = {uint8_t(pid), uint8_t(pid >> 8), uint8_t(pid >> 16), uint8_t(pid >> 24)};
      if (!p.write_user(output, ConstByteView(out, 4))) return kAccessViolation;
      information = 4;
      status = kSuccess;
      break;
    }
    case kFsctlSetPort: {      // (the library's own) { u64 port handle, u64 key }: calls of a process that ends while they wait are reported to the port
      uint8_t raw[16];
      uint64_t port_slot = 0;
      if ((token & kFolder) || input_length < 16 || !p.read_user(input, ByteView(raw, 16))) { status = kInvalidParameter; break; }
      uint64_t handle = 0, key = 0;
      for (uint32_t i = 0; i < 8; ++i) { handle |= uint64_t{raw[i]} << (8 * i); key |= uint64_t{raw[8 + i]} << (8 * i); }
      if (!slot_of(handle, &port_slot) || p.files().kind(port_slot) != DescriptorKind::Semaphore) { status = kInvalidHandle; break; }
      objects->pipe_set_port(id, end, uint32_t(p.files().token(port_slot)), key);
      status = kSuccess;
      break;
    }
    case kFsctlAwait: {      // { u32 tag }: waits for the operation, gives a read's bytes in the output buffer; its status, the count in Information
      uint8_t raw[4];
      if (token & kFolder) break;
      if (input_length < 4 || !p.read_user(input, ByteView(raw, 4))) { status = kInvalidParameter; break; }
      const uint32_t tag = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
      ObjectManager::PipeOpResult result;
      status = wait_operation(p, id, end, tag, &result);
      information = result.transferred;
      if (!result.data.empty()) {
        const uint64_t n = result.transferred < output_length ? result.transferred : output_length;
        if (n != 0 && !p.write_user(output, ConstByteView(result.data).first(n))) status = kAccessViolation;
        information = n;
        objects->pipe_op_free_data(result.data);
      }
      break;
    }
    case kFsctlAck: {      // { u32 tag }: the thread that waited for the operation has acted on its result
      uint8_t raw[4];
      if (input_length < 4 || !p.read_user(input, ByteView(raw, 4))) { status = kInvalidParameter; break; }
      objects->pipe_op_release(id, raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24));
      if (input_length >= 5 && p.read_user(input + 4, ByteView(raw, 1)) && raw[0] != 0) objects->pipe_set_file_signaled(id, end, true);      // (it had no event of its own: the file is signalled)
      status = kSuccess;
      break;
    }
    case kFsctlCancel: {      // { u32 tag }: ends the operation (CancelIoEx); its owner has its answer when this returns
      uint8_t raw[4];
      if (token & kFolder) break;
      if (input_length < 4 || !p.read_user(input, ByteView(raw, 4))) { status = kInvalidParameter; break; }
      objects->sync().lock();
      const ObjectManager::PipeResult res = objects->pipe_op_cancel_locked(id, end, raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24), kCancelled);
      objects->sync().unlock();
      status = res == ObjectManager::PipeResult::Ok ? kSuccess : kNotFound;
      await_delivery(p, id);
      break;
    }
    case 0x110004:      // FSCTL_PIPE_DISCONNECT
      if ((token & kFolder) || (token_end(token) & 1)) break;
      status = objects->pipe_disconnect(id) ? kSuccess : kPipeDisconnected;
      await_delivery(p, id);
      break;
    case 0x11400C: {    // FSCTL_PIPE_PEEK: FILE_PIPE_PEEK_BUFFER { State, ReadDataAvailable, NumberOfMessages, MessageLength, Data[] }
      if (token & kFolder) break;
      if (output_length < 16) { status = kInfoLengthMismatch; break; }
      constexpr uint32_t kMaxPeek = 1u << 20;
      ByteBuffer peeked(p.files().allocator());
      const uint32_t room = uint32_t(output_length - 16 < kMaxPeek ? output_length - 16 : kMaxPeek);
      if (!peeked.put_zeros(room)) { status = kNoMemory; break; }
      uint8_t* data = peeked.writable_view().data();
      uint32_t got = 0, available = 0, message_length = 0;
      objects->sync().lock();
      const ObjectManager::PipeResult res = objects->pipe_read_locked(id, end, data, room, &got, true, &available, false, &message_length);
      objects->sync().unlock();
      uint8_t head[16] = {};
      const uint32_t state = res == ObjectManager::PipeResult::Broken ? 4u : res == ObjectManager::PipeResult::NotConnected ? 2u : 3u;      // closing / listening / connected
      for (uint32_t i = 0; i < 4; ++i) { head[i] = uint8_t(state >> (8 * i)); head[4 + i] = uint8_t(available >> (8 * i)); head[12 + i] = uint8_t(message_length >> (8 * i)); }
      head[8] = available ? 1 : 0;
      if (res == ObjectManager::PipeResult::Broken && available == 0) { status = kPipeBroken; break; }
      if (res == ObjectManager::PipeResult::Listening || (res == ObjectManager::PipeResult::NotConnected && !(end & 1))) { status = 0xC00000ADu; break; }      // STATUS_INVALID_PIPE_STATE (ERROR_BAD_PIPE): the server end has no connection
      if (res == ObjectManager::PipeResult::NotConnected) { status = kPipeDisconnected; break; }
      if (!p.write_user(output, ConstByteView(head, 16)) || (got != 0 && !p.write_user(output + 16, peeked.view().first(got)))) return kAccessViolation;
      information = 16 + got;
      status = kSuccess;
      break;
    }
    case 0x110018: {    // FSCTL_PIPE_WAIT: FILE_PIPE_WAIT_FOR_BUFFER { LARGE_INTEGER Timeout; ULONG NameLength; BOOLEAN TimeoutSpecified; WCHAR Name[] }
      if (!(token & kFolder)) break;
      if (input_length < 20 || input_length > 20 + 2 * 256) { status = kInvalidParameter; break; }
      uint8_t raw[20 + 2 * 256];
      if (!p.read_user(input, ByteView(raw, input_length))) return kAccessViolation;
      uint64_t timeout = 0;
      for (uint32_t i = 0; i < 8; ++i) timeout |= uint64_t{raw[i]} << (8 * i);
      const uint32_t name_bytes = raw[8] | (uint32_t{raw[9]} << 8) | (uint32_t{raw[10]} << 16) | (uint32_t{raw[11]} << 24);
      if (name_bytes % 2 != 0 || name_bytes > input_length - 20) { status = kInvalidParameter; break; }
      char name[256 + 1];
      uint32_t n = 0;
      for (uint32_t i = 0; i < name_bytes / 2 && n < 256; ++i) {
        const uint32_t unit = raw[20 + 2 * i] | (uint32_t{raw[21 + 2 * i]} << 8);
        if (unit >= 0x80) { status = kInvalidParameter; n = 0xFFFF; break; }
        name[n++] = char(unit);
      }
      if (n == 0xFFFF) break;
      const bool specified = raw[12] != 0;
      uint64_t timeout_ns = ~uint64_t{0};
      if (specified) {
        if (int64_t(timeout) < 0) { const uint64_t units = uint64_t(0) - timeout; timeout_ns = units > ~uint64_t{0} / 100 ? ~uint64_t{0} : units * 100; }
        else { const uint64_t now = p.host().realtime_ns() / 100 + kUnixToNt; timeout_ns = timeout > now ? (timeout - now) * 100 : 0; }
      } else {
        timeout_ns = 50ull * 1000000;      // the default wait of a pipe: 50 ms
      }
      status = wait_until(p, timeout_ns, [&]() -> uint64_t {
        bool known = false;
        if (!objects->pipe_name_exists_locked(StringView(name, n))) return 0xC0000034u;      // STATUS_OBJECT_NAME_NOT_FOUND: no such pipe at all
        if (objects->pipe_name_available_locked(StringView(name, n))) return kSuccess;
        (void)known;
        return kPending;
      });
      if (status == kTimeout) status = 0xC00000B5u;      // STATUS_IO_TIMEOUT (ERROR_SEM_TIMEOUT)
      break;
    }
    case 0x11C017:      // FSCTL_PIPE_TRANSCEIVE: write the input as a message, then read the answer into the output
    case kFsctlBeginTransact: {      // (the library's own) the same for an overlapped call: { u32 read length } -> { u32 tag } in the output, STATUS_PENDING; the answer is awaited
      if (token & kFolder) break;
      const bool library = uint32_t(code) == kFsctlBeginTransact;
      uint64_t read_length = output_length;
      if (library) {
        uint8_t raw[4];
        if (output_length < 4 || !p.read_user(output, ByteView(raw, 4))) { status = kInvalidParameter; break; }
        read_length = raw[0] | (uint32_t{raw[1]} << 8) | (uint32_t{raw[2]} << 16) | (uint32_t{raw[3]} << 24);
      }
      if (input_length > ObjectManager::kMaxPipeWrite) { status = kNoResources; break; }
      ByteBuffer data(p.files().allocator());
      if (!data.put_zeros(input_length)) { status = kNoMemory; break; }
      if (input_length != 0 && !p.read_user(input, data.writable_view())) return kAccessViolation;
      uint32_t tag = 0;
      objects->sync().lock();
      const ObjectManager::PipeResult res = objects->pipe_begin_transact_locked(id, end, data.view(), uint32_t(read_length > 0xFFFFFFFFu ? 0xFFFFFFFFu : read_length), (token & kMessageRead) != 0, p.id(), library, &tag);
      objects->sync().unlock();
      if (res != ObjectManager::PipeResult::WouldBlock) { status = result_status(res, false, true); break; }
      await_delivery(p, id);      // (reads of the other end that waited have the message)
      if (library) {
        const uint8_t out_tag[4] = {uint8_t(tag), uint8_t(tag >> 8), uint8_t(tag >> 16), uint8_t(tag >> 24)};
        if (!p.write_user(output, ConstByteView(out_tag, 4))) {
          objects->sync().lock();
          objects->pipe_op_cancel_locked(id, end, tag, kCancelled);
          objects->sync().unlock();
          objects->pipe_op_release(id, tag);
          return kAccessViolation;
        }
        status = 0x103;      // STATUS_PENDING
        break;
      }
      ObjectManager::PipeOpResult result;
      SyncWait waiting(p, id, end);
      status = wait_operation(p, id, end, tag, &result, &waiting);
      objects->pipe_op_release(id, tag);
      information = result.transferred;
      if (!result.data.empty()) {
        const uint64_t n = result.transferred < output_length ? result.transferred : output_length;
        if (n != 0 && !p.write_user(output, ConstByteView(result.data).first(n))) status = kAccessViolation;
        information = n;
        objects->pipe_op_free_data(result.data);
      }
      break;
    }
    default: status = kInvalidDeviceRequest; break;
  }
  if (status_block != 0) {
    uint8_t iosb[16] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)};
    for (uint32_t i = 0; i < 8; ++i) iosb[8 + i] = uint8_t(information >> (8 * i));
    p.write_user(status_block, ConstByteView(iosb, 16));
  }
  return status;
}

}  // namespace process::windowsabi
