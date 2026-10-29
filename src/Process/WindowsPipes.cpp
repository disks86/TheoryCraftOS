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

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kInvalidDeviceRequest = 0xC0000010u,
                   kAccessDenied = 0xC0000022u, kObjectNameNotFound = 0xC0000034u, kPipeBusy = 0xC00000AEu, kPipeConnected = 0xC00000AFu, kPipeDisconnected = 0xC00000B0u,
                   kPipeClosing = 0xC00000B1u, kPipeBroken = 0xC000014Bu, kPipeEmpty = 0xC00000D9u, kNotImplemented = 0xC0000002u, kInvalidInfoClass = 0xC0000003u,
                   kInfoLengthMismatch = 0xC0000004u, kTimeout = 0x102, kAlerted = 0x101, kNoMemory = 0xC0000017u, kNoResources = 0xC000009Au;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;
constexpr uint64_t kPending = kWaitPending;

// The token of a pipe descriptor: bits 0..31 the object, 32..55 the end code, 60 read, 61 write, 62 non-blocking, 63 the pipe folder itself.
constexpr uint64_t kRead = 1ull << 60, kWrite = 1ull << 61, kNoWait = 1ull << 62, kFolder = 1ull << 63;
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
    case ObjectManager::PipeResult::Listening: return 0xC00000B3u;      // STATUS_PIPE_LISTENING
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

}  // namespace

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
  const ObjectManager::Created c = objects->create_pipe(StringView(), uint32_t(r.args[3]), 0, 0, ObjectManager::kPipeOutbound);
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
  uint64_t status_block = 0, share = 0, max_instances = 0, in_quota = 0, out_quota = 0;
  if (!argument(p, r, 3, &status_block) || !argument(p, r, 4, &share) || !argument(p, r, 10, &max_instances) || !argument(p, r, 11, &in_quota) || !argument(p, r, 12, &out_quota))
    return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  String name(p.files().allocator());
  if (!pipe_name(p, r.args[2], &name) || name.empty() || name.view().contains('\\')) return 0xC0000033u;      // STATUS_OBJECT_NAME_INVALID
  const uint32_t direction = uint32_t(share) & 3;      // FILE_SHARE_READ: clients read (the server writes); FILE_SHARE_WRITE: clients write (the server reads)
  uint32_t access = 0;
  if (direction == 1) access = ObjectManager::kPipeOutbound;
  else if (direction == 2) access = ObjectManager::kPipeInbound;
  else if (direction == 3) access = ObjectManager::kPipeDuplex;
  else return kInvalidParameter;
  const ObjectManager::Created c = objects->create_pipe(name.view(), uint32_t(out_quota), uint32_t(in_quota), uint32_t(max_instances) == 0xFFFFFFFFu ? 0 : uint32_t(max_instances), access);
  if (c.id == 0) return c.status;
  uint64_t handle = 0;
  uint32_t status = give_pipe_handle(p, make_token(c.id, 0, (access & ObjectManager::kPipeInbound) != 0, (access & ObjectManager::kPipeOutbound) != 0), wants_inherit(p, r.args[2]), &handle);
  if (status != kSuccess) { objects->release_pipe_end(c.id, 0); return status; }
  if (!p.write_user_u64(r.args[0], handle)) { p.close_descriptor(handle / 4 - 1); return kAccessViolation; }
  uint8_t iosb[16] = {};
  iosb[8] = 2;      // FILE_CREATED
  p.write_user(status_block, ConstByteView(iosb, 16));
  return kSuccess;
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
  const ObjectManager::Created c = objects->connect_pipe(name.view(), read, write);
  if (c.id == 0) { *status = c.status; return true; }
  *status = give_pipe_handle(p, make_token(c.id, c.end_code, read || !write, write), inherit, handle);
  if (*status != kSuccess) objects->release_pipe_end(c.id, c.end_code);
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
  if (write) {
    uint32_t failure = kSuccess;
    while (done < length && failure == kSuccess) {
      const uint32_t n = uint32_t(length - done < sizeof(chunk) ? length - done : sizeof(chunk));
      if (!p.read_user(buffer + done, ByteView(chunk, n))) { if (done == 0) return kAccessViolation; break; }
      uint32_t sent = 0;
      while (sent < n) {
        uint32_t moved = 0;
        const uint32_t s = wait_until(p, no_wait ? 0 : ~uint64_t{0}, [&]() -> uint64_t {
          const ObjectManager::PipeResult res = objects->pipe_write_locked(id, end, chunk + sent, n - sent, &moved);
          if (res == ObjectManager::PipeResult::WouldBlock) return kPending;
          return result_status(res, false, true);
        });
        if (s == kSuccess) { sent += moved; continue; }
        failure = s == kTimeout ? kPipeBusy : s;
        break;
      }
      done += sent;
    }
    status = done > 0 ? kSuccess : failure;      // (some of it went: the caller sees how much)
  } else if (length != 0) {
    const uint32_t want = uint32_t(length < sizeof(chunk) ? length : sizeof(chunk));
    uint32_t got = 0;
    status = wait_until(p, no_wait ? 0 : ~uint64_t{0}, [&]() -> uint64_t {
      const ObjectManager::PipeResult res = objects->pipe_read_locked(id, end, chunk, want, &got);
      if (res == ObjectManager::PipeResult::WouldBlock) return kPending;
      return result_status(res, false, false);
    });
    if (status == kTimeout) status = kPipeEmpty;
    if (status == kSuccess) {
      if (!p.write_user(buffer, ConstByteView(chunk, got))) return kAccessViolation;
      done = got;
    }
  }
  if (status != kSuccess) {
    if (status_block != 0) { uint8_t iosb[16] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)}; p.write_user(status_block, ConstByteView(iosb, 16)); }
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
  return wait_until(p, ~uint64_t{0}, [&]() -> uint64_t { return objects->pipe_pending_locked(id, end) == 0 ? kSuccess : kPending; });      // (what this end wrote and the other has not read)
}

uint32_t pipe_query_information(Process& p, uint64_t slot, uint32_t info_class, uint64_t status_block, uint64_t buffer, uint32_t length) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint64_t token = p.files().token(slot);
  uint8_t out[40] = {};
  auto put32 = [&](uint32_t at, uint32_t v) { for (uint32_t i = 0; i < 4; ++i) out[at + i] = uint8_t(v >> (8 * i)); };
  uint32_t size = 0;
  if (info_class == 23) {      // FILE_PIPE_INFORMATION { ReadMode, CompletionMode }
    put32(0, 0);
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
    put32(0, 0);                                           // NamedPipeType: byte stream
    put32(4, info.anonymous ? 1 : (info.access == ObjectManager::kPipeInbound ? 0 : info.access == ObjectManager::kPipeOutbound ? 1 : 2));      // NamedPipeConfiguration (inbound 0, outbound 1, duplex 2)
    put32(8, info.max_instances ? info.max_instances : 255);
    put32(12, info.instances);
    put32(16, info.in_size);
    put32(20, available);
    put32(24, info.out_size);
    put32(28, info.out_size);
    put32(32, info.connected ? 3 : 2);                     // NamedPipeState: FILE_PIPE_CONNECTED_STATE / LISTENING
    put32(36, end == 0 ? 1 : 0);                           // NamedPipeEnd: 1 server, 0 client
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
  token = raw[4] ? (token | kNoWait) : (token & ~kNoWait);      // CompletionMode: FILE_PIPE_COMPLETE_OPERATION (1) fails instead of waiting
  p.files().set_token(slot, token);
  return kSuccess;
}

uint32_t nt_fs_control_file(Process& p, const SyscallRequest& r) {
  uint64_t status_block = 0, code = 0, input = 0, input_length = 0, output = 0, output_length = 0;
  if (!argument(p, r, 4, &status_block) || !argument(p, r, 5, &code) || !argument(p, r, 6, &input) || !argument(p, r, 7, &input_length) || !argument(p, r, 8, &output) ||
      !argument(p, r, 9, &output_length))
    return kAccessViolation;
  uint64_t slot = 0;
  if (!slot_of(r.args[0], &slot) || p.files().kind(slot) == DescriptorKind::Closed) return kInvalidHandle;
  if (p.files().kind(slot) != DescriptorKind::Pipe) return kInvalidDeviceRequest;
  ObjectManager* objects = system_objects();
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
      bool first = true;
      status = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t {
        const ObjectManager::PipeResult res = objects->pipe_listen_locked(id);
        const bool was_connected = first;
        first = false;
        if (res == ObjectManager::PipeResult::Ok) return was_connected ? kPipeConnected : kSuccess;      // (a client that came before the call is reported as such, as NT does)
        if (res == ObjectManager::PipeResult::WouldBlock) return kPending;
        return kPipeBroken;
      });
      break;
    }
    case 0x110004:      // FSCTL_PIPE_DISCONNECT
      if ((token & kFolder) || (token_end(token) & 1)) break;
      status = objects->pipe_disconnect(id) ? kSuccess : kPipeDisconnected;
      break;
    case 0x11400C: {    // FSCTL_PIPE_PEEK: FILE_PIPE_PEEK_BUFFER { State, ReadDataAvailable, NumberOfMessages, MessageLength, Data[] }
      if (token & kFolder) break;
      if (output_length < 16) { status = kInfoLengthMismatch; break; }
      uint8_t data[8192];
      const uint32_t room = uint32_t(output_length - 16 < sizeof(data) ? output_length - 16 : sizeof(data));
      uint32_t got = 0, available = 0;
      objects->sync().lock();
      const ObjectManager::PipeResult res = objects->pipe_read_locked(id, end, data, room, &got, true, &available);
      objects->sync().unlock();
      uint8_t head[16] = {};
      const uint32_t state = res == ObjectManager::PipeResult::Broken ? 4u : res == ObjectManager::PipeResult::NotConnected ? 2u : 3u;      // closing / listening / connected
      for (uint32_t i = 0; i < 4; ++i) { head[i] = uint8_t(state >> (8 * i)); head[4 + i] = uint8_t(available >> (8 * i)); head[12 + i] = uint8_t(got >> (8 * i)); }
      head[8] = available ? 1 : 0;
      if (res == ObjectManager::PipeResult::Broken && available == 0) { status = kPipeBroken; break; }
      if (res == ObjectManager::PipeResult::Listening || (res == ObjectManager::PipeResult::NotConnected && !(end & 1))) { status = 0xC00000ADu; break; }      // STATUS_INVALID_PIPE_STATE (ERROR_BAD_PIPE): the server end has no connection
      if (res == ObjectManager::PipeResult::NotConnected) { status = kPipeDisconnected; break; }
      if (!p.write_user(output, ConstByteView(head, 16)) || (got != 0 && !p.write_user(output + 16, ConstByteView(data, got)))) return kAccessViolation;
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
    case 0x11C017: {    // FSCTL_PIPE_TRANSCEIVE: write the input, then read into the output
      if (token & kFolder) break;
      status = pipe_io(p, slot, true, 0, input, input_length);
      if (status != kSuccess) break;
      status = pipe_io(p, slot, false, status_block, output, output_length);
      return status;      // (pipe_io stored the byte count)
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
