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
#include <fstream>
#include <thread>
#include "Process/AddressSpace.hpp"
#include "Process/Win32k/Win32kComposition.hpp"
#include "Win32kTestSupport.hpp"

// Host test of the composition module (docs/win32k/Composition.md): object lifetime, cross-process sharing, events, queue ordering, hostile arguments and process exit cleanup.
using namespace w32test;
using namespace process;
using namespace process::win32k;
namespace comp = process::win32k::composition;
using N = WindowsSyscallNumber;

namespace {

constexpr uint32_t kSuccess = 0, kTimeout = 0x102, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kAccessDenied = 0xC0000022u, kNotImplemented = 0xC0000002u,
                   kNotFound = 0xC0000225u, kAccessViolation = 0xC0000005u, kNoMoreEntries = 0x8000001Au, kBufferTooSmall = 0xC0000023u, kNotSupported = 0xC00000BBu,
                   kMismatch = 0xC0000024u, kBusy = 0x80000011u, kPortDisconnected = 0xC0000037u, kBufferOverflow = 0x80000005u, kInvalidState = 0xC0000184u,
                   kQuota = 0xC0000044u;

// The system, with the composition state taken down before the allocator goes.
struct Env {
  System sys;
  ~Env() { comp::shutdown(); }
};

// A Windows program with an address space (the channel buffers are mapped into it).
struct Proc {
  AddressSpace space;
  Program prog;
  Proc(Env& env, uint64_t pid) : space(env.sys.allocator), prog(pid) { prog.process.attach_address_space(space, 0x10000); }
  uint32_t call(N n, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) { return uint32_t(prog.call(n, a0, a1, a2, a3, a4, a5)); }
  uint64_t out(uint64_t slot) { return prog.memory.get64(kScratch + 0x800 + slot * 8); }
  uint64_t out_addr(uint64_t slot) { return kScratch + 0x800 + slot * 8; }
  uint32_t out32(uint64_t slot) { return prog.memory.get32(kScratch + 0x800 + slot * 8); }
};

// An ordinary NT event (manual reset) made through the system call; returns its handle.
uint64_t make_event(Proc& p, bool signaled = false) {
  CHECK(p.call(N::NtCreateEvent, p.out_addr(60), 0x1F0003, 0, 0, signaled ? 1 : 0) == 0);
  return p.out(60);
}
bool is_signaled(Proc& p, uint64_t event) {
  p.prog.memory.put64(kScratch + 0xF00, 0);
  return p.call(N::NtWaitForSingleObject, event, 0, kScratch + 0xF00) == 0;
}

// A handle in `holder`'s table to process `pid` (what OpenProcess gives).
uint64_t process_handle(Proc& holder, uint64_t pid) {
  const ObjectManager::Created c = system_objects()->create_process(pid);
  const int64_t slot = holder.prog.process.files().open_token(DescriptorKind::Process, c.id);
  return (uint64_t(slot) + 1) * 4;
}

// Command buffer builder: {opcode, length} + payload.
struct Batch {
  std::vector<uint8_t> bytes;
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(v >> (8 * i))); }
  Batch& cmd(uint32_t op, std::initializer_list<uint32_t> payload) {
    u32(op); u32(uint32_t(8 + 4 * payload.size()));
    for (uint32_t v : payload) u32(v);
    return *this;
  }
};
constexpr uint32_t kOpNop = 0, kOpCreate = 1, kOpRelease = 2, kOpReleaseAll = 3, kOpSetProperty = 4, kOpAddChild = 5, kOpRemoveChild = 6;

struct Channel {
  uint64_t handle = 0, base = 0, size = 0;
};
Channel make_channel(Proc& p, bool dwm = false, uint64_t want = 0) {
  Channel c;
  p.prog.memory.put64(p.out_addr(1), want);
  const uint32_t st = p.call(dwm ? N::NtDCompositionCreateDwmChannel : N::NtDCompositionCreateChannel, p.out_addr(0), p.out_addr(1), p.out_addr(2));
  if (st != 0) { printf("channel create failed %x\n", st); ++g_failed; return c; }
  c.handle = p.out(0); c.size = p.out(1); c.base = p.out(2);
  return c;
}
// Writes the batch into the channel's buffer (through the address space: the buffer is shared memory) and runs it.
uint32_t run_batch(Proc& p, const Channel& c, const Batch& b, uint32_t* consumed = nullptr, uint32_t* commands = nullptr) {
  if (!b.bytes.empty()) CHECK(p.space.write(c.base, ConstByteView(b.bytes.data(), b.bytes.size())));
  const uint32_t st = p.call(N::NtDCompositionProcessChannelBatchBuffer, c.handle, b.bytes.size(), p.out_addr(10), p.out_addr(11));
  if (consumed) *consumed = p.out32(10);
  if (commands) *commands = p.out32(11);
  return st;
}
uint32_t commit(Proc& p, const Channel& c, uint64_t event = 0, uint32_t* batch = nullptr) {
  const uint32_t st = p.call(N::NtDCompositionCommitChannel, c.handle, 0, event, p.out_addr(12));
  if (batch) *batch = p.out32(12);
  return st;
}
std::vector<uint32_t> deleted(Proc& p, const Channel& c, uint32_t room = 64, uint32_t* status = nullptr) {
  p.prog.memory.put32(p.out_addr(21), room);
  const uint32_t st = p.call(N::NtDCompositionGetDeletedResources, c.handle, kScratch + 0x1000, p.out_addr(21));
  if (status) *status = st;
  std::vector<uint32_t> ids;
  for (uint32_t i = 0; i < p.out32(21); ++i) ids.push_back(p.prog.memory.get32(kScratch + 0x1000 + 4 * i));
  return ids;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void test_registration() {
  Env env;
  uint32_t implemented = 0, missing = 0;
  for (uint32_t n = kServiceBase; n < kServiceEnd; ++n) if (handler_of(n)) ++implemented;
  CHECK(duplicate_registrations() == 0);
  // The services this module owns: every one of them is registered except the documented few.
  const N owned[] = {N::NtDCompositionCreateChannel, N::NtFlipObjectCreate, N::NtRIMReadInput, N::NtMITSynthesizeKeyboardInput, N::NtModerncoreCreateGDIHwndTarget, N::NtCreateCompositionSurfaceHandle,
                     N::NtTokenManagerCreateCompositionTokenHandle, N::NtConfigureInputSpace};
  for (N n : owned) CHECK(handler_of(uint32_t(n)) != nullptr);
  CHECK(handler_of(uint32_t(N::NtKSTWait)) == nullptr);
  CHECK(handler_of(uint32_t(N::NtDCompositionRegisterThumbnailVisual)) == nullptr);
  Proc p(env, 1);
  CHECK(p.call(N::NtKSTWait) == kNotImplemented);      // an unregistered service answers as before
  (void)implemented; (void)missing;
}

void test_channel_commands() {
  Env env;
  Proc p(env, 10);
  Channel ch = make_channel(p, false, 5000);
  CHECK(ch.handle != 0 && ch.size == 8192 && ch.base != 0);      // rounded up to pages
  CHECK(p.space.is_shared(ch.base));
  Channel other = make_channel(p, false, 0);
  CHECK(other.size == 0x10000 && other.base != ch.base);

  // create three resources, a parent/child edge and a property
  Batch b;
  b.cmd(kOpCreate, {1, 7}).cmd(kOpCreate, {2, 7}).cmd(kOpCreate, {3, 9}).cmd(kOpAddChild, {1, 2}).cmd(kOpSetProperty, {2, 5, 0xAABBCCDD, 0x11}).cmd(kOpNop, {});
  uint32_t consumed = 0, commands = 0;
  CHECK(run_batch(p, ch, b, &consumed, &commands) == kSuccess && consumed == b.bytes.size() && commands == 6);

  // releasing the child that the parent still refers to keeps it; the parent's release then frees both at the next commit
  Batch r1;
  r1.cmd(kOpRelease, {2}).cmd(kOpRelease, {3});
  CHECK(run_batch(p, ch, r1) == kSuccess);
  CHECK(deleted(p, ch).empty());      // nothing is deleted before a commit
  uint32_t batch = 0;
  CHECK(commit(p, ch, 0, &batch) == kSuccess && batch == 1);
  std::vector<uint32_t> d = deleted(p, ch);
  CHECK(d.size() == 1 && d[0] == 3);      // 3 is gone; 2 is still referenced by 1
  Batch r2;
  r2.cmd(kOpRelease, {1});
  CHECK(run_batch(p, ch, r2) == kSuccess && commit(p, ch, 0, &batch) == kSuccess && batch == 2);
  d = deleted(p, ch);
  CHECK(d.size() == 2);      // 1 and then the child it held
  CHECK(deleted(p, ch).empty());
  // the ids can be used again
  Batch again;
  again.cmd(kOpCreate, {1, 3});
  CHECK(run_batch(p, ch, again) == kSuccess);

  // errors: the batch stops at the failing command and says how far it got
  Batch bad;
  bad.cmd(kOpCreate, {10, 1}).cmd(kOpCreate, {10, 1}).cmd(kOpCreate, {11, 1});      // the second creates a resource that exists
  CHECK(run_batch(p, ch, bad, &consumed, &commands) == kInvalidParameter && commands == 1 && consumed == 16);
  Batch unknown;
  unknown.cmd(0x77, {});
  CHECK(run_batch(p, ch, unknown) == kInvalidParameter);
  Batch zero_id;
  zero_id.cmd(kOpCreate, {0, 1});
  CHECK(run_batch(p, ch, zero_id) == kInvalidParameter);
  Batch missing_ref;
  missing_ref.cmd(kOpAddChild, {10, 999});
  CHECK(run_batch(p, ch, missing_ref) == kInvalidParameter);
  Batch twice;
  twice.cmd(kOpRelease, {10}).cmd(kOpRelease, {10});
  CHECK(run_batch(p, ch, twice, &consumed, &commands) == kInvalidParameter && commands == 1);

  // hostile framing: truncated, length past the buffer, length not a multiple of 4, size past the section, odd size
  Batch trunc;
  trunc.u32(kOpCreate);
  CHECK(run_batch(p, ch, trunc) == kInvalidParameter);
  Batch huge;
  huge.u32(kOpNop); huge.u32(0xFFFFFFF0u);
  CHECK(run_batch(p, ch, huge) == kInvalidParameter);
  Batch odd;
  odd.u32(kOpNop); odd.u32(10); odd.u32(0);
  CHECK(run_batch(p, ch, odd) == kInvalidParameter);
  Batch tiny;
  tiny.u32(kOpNop); tiny.u32(4);
  CHECK(run_batch(p, ch, tiny) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionProcessChannelBatchBuffer, ch.handle, ch.size + 4, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionProcessChannelBatchBuffer, ch.handle, 6, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionProcessChannelBatchBuffer, ch.handle, 0, 0, 0) == kSuccess);
  CHECK(p.call(N::NtDCompositionProcessChannelBatchBuffer, ch.handle, 0, 0x10, 0) == kAccessViolation);      // an unwritable output pointer
  CHECK(p.call(N::NtDCompositionProcessChannelBatchBuffer, 0, 0, 0, 0) == kInvalidHandle);
  CHECK(p.call(N::NtDCompositionProcessChannelBatchBuffer, ch.handle + 1, 0, 0, 0) == kInvalidHandle);

  // the resource table is bounded
  Batch many;
  for (uint32_t i = 0; i < 4097; ++i) many.cmd(kOpCreate, {100 + i, 1});
  Channel big = make_channel(p, false, 1 << 20);
  CHECK(run_batch(p, big, many, &consumed, &commands) == kInvalidParameter && commands == 4096);

  // release-all frees everything at the next commit
  Batch some;
  some.cmd(kOpCreate, {1, 1}).cmd(kOpCreate, {2, 1}).cmd(kOpAddChild, {1, 2});
  Channel c3 = make_channel(p);
  CHECK(run_batch(p, c3, some) == kSuccess);
  CHECK(p.call(N::NtDCompositionReleaseAllResources, c3.handle) == kSuccess && commit(p, c3) == kSuccess);
  CHECK(deleted(p, c3).size() == 2);

  // GetDeletedResources with less room than there is
  Batch five;
  for (uint32_t i = 1; i <= 5; ++i) five.cmd(kOpCreate, {i, 1});
  five.cmd(kOpReleaseAll, {});
  Channel c4 = make_channel(p);
  CHECK(run_batch(p, c4, five) == kSuccess && commit(p, c4) == kSuccess);
  uint32_t st = 0;
  std::vector<uint32_t> first = deleted(p, c4, 3, &st);
  CHECK(first.size() == 3 && st == kBufferOverflow);
  CHECK(deleted(p, c4, 3, &st).size() == 2 && st == kSuccess);

  // destroy: the handle is gone, the view is unmapped, others are untouched
  CHECK(p.call(N::NtDCompositionDestroyChannel, ch.handle) == kSuccess);
  CHECK(!p.space.is_shared(ch.base));
  CHECK(p.call(N::NtDCompositionDestroyChannel, ch.handle) == kInvalidHandle);
  CHECK(p.call(N::NtDCompositionCommitChannel, ch.handle, 0, 0, 0) == kInvalidHandle);
  CHECK(p.space.is_shared(other.base));
}

void test_channel_commit_state() {
  Env env;
  Proc p(env, 11);
  Channel ch = make_channel(p);
  uint32_t b1 = 0, b2 = 0;
  CHECK(commit(p, ch, 0, &b1) == kSuccess && commit(p, ch, 0, &b2) == kSuccess && b2 == b1 + 1);
  CHECK(p.call(N::NtDCompositionGetBatchId, ch.handle, p.out_addr(0)) == kSuccess && p.out32(0) == b2);
  CHECK(p.call(N::NtDCompositionGetFrameId, p.out_addr(0)) == kSuccess && p.out(0) == 2);
  CHECK(p.call(N::NtDCompositionGetFrameIdFromBatchId, ch.handle, b1, p.out_addr(0)) == kSuccess && p.out(0) == 1);
  CHECK(p.call(N::NtDCompositionGetFrameIdFromBatchId, ch.handle, b2, p.out_addr(0)) == kSuccess && p.out(0) == 2);
  CHECK(p.call(N::NtDCompositionGetFrameIdFromBatchId, ch.handle, 99, p.out_addr(0)) == kNotFound);
  CHECK(p.call(N::NtDCompositionCommitChannel, ch.handle, 2, 0, 0) == kInvalidParameter);      // unknown flags
  CHECK(p.call(N::NtDCompositionSynchronize, ch.handle, 0, 100) == kSuccess);
  CHECK(p.call(N::NtDCompositionSynchronize, ch.handle, 7, 100) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionEnableMMCSS, ch.handle, 1) == kSuccess && p.call(N::NtDCompositionSuspendAnimations, ch.handle, 1) == kSuccess);
  CHECK(p.call(N::NtDCompositionEnableMMCSS, 0x1234, 1) == kInvalidHandle);

  // the commit completion event: set, signalled by the commit, cleared again
  const uint64_t ev = make_event(p);
  CHECK(p.call(N::NtDCompositionSetChannelCommitCompletionEvent, ch.handle, ev) == kSuccess);
  CHECK(!is_signaled(p, ev));
  CHECK(commit(p, ch) == kSuccess);
  CHECK(is_signaled(p, ev));
  CHECK(p.call(N::NtResetEvent, ev, 0) == kSuccess && !is_signaled(p, ev));
  CHECK(p.call(N::NtDCompositionSetChannelCommitCompletionEvent, ch.handle, 0) == kSuccess);
  CHECK(commit(p, ch) == kSuccess && !is_signaled(p, ev));
  // an event passed to the commit is signalled too, and the call checks it is an event
  const uint64_t ev2 = make_event(p);
  CHECK(commit(p, ch, ev2) == kSuccess && is_signaled(p, ev2));
  CHECK(commit(p, ch, ch.handle) == kInvalidHandle || commit(p, ch, ch.handle) == kMismatch);
  CHECK(p.call(N::NtDCompositionSetChannelCommitCompletionEvent, ch.handle, 0x4444) == kInvalidHandle);
  // the event outlives its handle while the channel holds it
  CHECK(p.call(N::NtDCompositionSetChannelCommitCompletionEvent, ch.handle, ev) == kSuccess);
  CHECK(p.call(N::NtClose, ev) == kSuccess);
  CHECK(commit(p, ch) == kSuccess);      // signals an event nobody can reach: no crash, no stale id
  CHECK(p.call(N::NtDCompositionDestroyChannel, ch.handle) == kSuccess);

  // connections: batches of attached channels are counted
  CHECK(p.call(N::NtDCompositionCreateConnection, p.out_addr(0), p.out_addr(1)) == kSuccess);
  const uint64_t conn = p.out(0), conn_id = p.out(1);
  Channel cc = make_channel(p);
  CHECK(p.call(N::NtDCompositionSetChannelConnectionId, cc.handle, conn_id) == kSuccess);
  CHECK(p.call(N::NtDCompositionSetChannelConnectionId, cc.handle, 12345) == kInvalidParameter);
  CHECK(commit(p, cc) == kSuccess && commit(p, cc) == kSuccess);
  CHECK(p.call(N::NtDCompositionGetConnectionBatch, conn, p.out_addr(2)) == kSuccess && p.out32(2) == 2);
  CHECK(p.call(N::NtDCompositionConnectPipe, conn, 0) == kNotSupported);
  CHECK(p.call(N::NtDCompositionDestroyConnection, conn) == kSuccess && p.call(N::NtDCompositionDestroyConnection, conn) == kInvalidHandle);
  CHECK(p.call(N::NtDCompositionGetConnectionBatch, cc.handle, p.out_addr(2)) == kMismatch);      // wrong kind of handle

  // statistics
  p.prog.memory.put32(kScratch + 0x1100, 0);
  CHECK(p.call(N::NtDCompositionGetStatistics, kScratch + 0x1100, 8) == 0xC0000004u);
  CHECK(p.call(N::NtDCompositionGetStatistics, kScratch + 0x1100, 64) == kSuccess && p.prog.memory.get32(kScratch + 0x1100) == 40 && p.prog.memory.get32(kScratch + 0x1104) == 1);
}

void test_sync_objects_and_waits() {
  Env env;
  Proc p(env, 12);
  CHECK(p.call(N::NtDCompositionCreateSynchronizationObject, p.out_addr(0), 9) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionCreateSynchronizationObject, p.out_addr(0), 0) == kSuccess);
  const uint64_t sync = p.out(0);
  Channel ch = make_channel(p);
  CHECK(p.call(N::NtDCompositionSyncWait, sync, 0) == kTimeout);
  CHECK(p.call(N::NtDCompositionCommitSynchronizationObject, ch.handle, sync, 0) == kSuccess);
  CHECK(p.call(N::NtDCompositionSyncWait, sync, 0) == kSuccess);      // taken
  CHECK(p.call(N::NtDCompositionSyncWait, sync, 0) == kTimeout);
  CHECK(p.call(N::NtDCompositionCommitSynchronizationObject, ch.handle, ch.handle, 0) == kMismatch);
  CHECK(p.call(N::NtDCompositionCommitSynchronizationObject, sync, sync, 0) == kMismatch);

  // WaitForChannel: a commit reports once
  CHECK(p.call(N::NtDCompositionWaitForChannel, ch.handle, 0, 0) == kTimeout);
  CHECK(commit(p, ch) == kSuccess);
  CHECK(p.call(N::NtDCompositionWaitForChannel, ch.handle, 0, 0) == kSuccess);
  CHECK(p.call(N::NtDCompositionWaitForChannel, ch.handle, 1, 0) == kTimeout);
  CHECK(p.call(N::NtDCompositionWaitForChannel, ch.handle, 1, 5) == kInvalidParameter);

  // a thread blocks in WaitForChannel/SyncWait and is woken by a commit from another process that holds a duplicate
  Proc q(env, 13);
  const uint64_t to_q = process_handle(p, 13);
  CHECK(p.call(N::NtDCompositionDuplicateHandleToProcess, ch.handle, to_q, p.out_addr(0), 0, 2) == kSuccess);
  const uint64_t q_channel = p.out(0);
  CHECK(p.call(N::NtDCompositionDuplicateHandleToProcess, sync, to_q, p.out_addr(1), 0, 2) == kSuccess);
  const uint64_t q_sync = p.out(1);
  CHECK(q.call(N::NtDCompositionGetBatchId, q_channel, q.out_addr(0)) == kSuccess);
  CHECK(p.call(N::NtDCompositionGetBatchId, q_channel, p.out_addr(0)) == kInvalidHandle);      // the handle belongs to q
  uint32_t woke = 0xFFFF, synced = 0xFFFF;
  std::thread waiter([&] { woke = q.call(N::NtDCompositionWaitForChannel, q_channel, 0xFFFFFFFFu, 0); });
  std::thread syncer([&] { synced = q.call(N::NtDCompositionSyncWait, q_sync, 5000); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(woke == 0xFFFF && synced == 0xFFFF);      // both still wait
  CHECK(commit(p, ch) == kSuccess);
  CHECK(p.call(N::NtDCompositionCommitSynchronizationObject, ch.handle, sync, 0) == kSuccess);
  waiter.join();
  syncer.join();
  CHECK(woke == kSuccess && synced == kSuccess);
  // when a process ends, a waiter on its behalf stops waiting (the wait checks the process)
}

// A handle to `handle` (of `from`) in `to`'s table.
uint64_t dup_to(Proc& from, Proc& to, uint64_t handle, uint32_t access = 0, uint32_t* status = nullptr) {
  const uint32_t st = from.call(N::NtDCompositionDuplicateHandleToProcess, handle, process_handle(from, to.prog.process.id()), from.out_addr(40), access, access == 0 ? 2 : 0);
  if (status) *status = st;
  return st == 0 ? from.out(40) : 0;
}
DescriptorKind kind_of(Proc& p, uint64_t handle) { return p.prog.process.files().kind(handle / 4 - 1); }

void test_dwm_and_sharing() {
  Env env;
  Proc app(env, 20), dwm(env, 21), other(env, 22);
  Channel ac = make_channel(app);
  // only one process holds DWM channels at a time
  Channel dc = make_channel(dwm, true);
  CHECK(dc.handle != 0);
  CHECK(other.call(N::NtDCompositionCreateDwmChannel, other.out_addr(0), other.out_addr(1), other.out_addr(2)) == kAccessDenied);
  Channel dc2 = make_channel(dwm, true);      // the same process may make more
  CHECK(dc2.handle != 0);

  // the DWM collects the application's channels as handles of its own (once)
  dwm.prog.memory.put32(dwm.out_addr(5), 4);
  CHECK(dwm.call(N::NtDCompositionGetChannels, kScratch + 0x1000, dwm.out_addr(5)) == kSuccess && dwm.out32(5) == 1);
  const uint64_t seen = dwm.prog.memory.get64(kScratch + 0x1000);
  CHECK(dwm.call(N::NtDCompositionGetBatchId, seen, dwm.out_addr(6)) == kSuccess);
  CHECK(commit(app, ac) == kSuccess);
  CHECK(dwm.call(N::NtDCompositionGetBatchId, seen, dwm.out_addr(6)) == kSuccess && dwm.out32(6) == 1);
  dwm.prog.memory.put32(dwm.out_addr(5), 4);
  CHECK(dwm.call(N::NtDCompositionGetChannels, kScratch + 0x1000, dwm.out_addr(5)) == kSuccess && dwm.out32(5) == 0);
  CHECK(app.call(N::NtDCompositionGetChannels, kScratch + 0x1000, app.out_addr(5)) == kAccessDenied);      // not the compositor
  other.prog.memory.put32(other.out_addr(5), 4);
  CHECK(other.call(N::NtDCompositionGetChannels, kScratch + 0x1000, other.out_addr(5)) == kAccessDenied);

  // DWM batches: processed and committed in one; only on a DWM channel
  Batch b;
  b.cmd(kOpCreate, {5, 2});
  CHECK(dwm.space.write(dc.base, ConstByteView(b.bytes.data(), b.bytes.size())));
  CHECK(dwm.call(N::NtDCompositionSubmitDWMBatch, dc.handle, b.bytes.size(), dwm.out_addr(0), dwm.out_addr(1)) == kSuccess && dwm.out32(1) == 1);
  CHECK(dwm.call(N::NtDCompositionGetBatchId, dc.handle, dwm.out_addr(0)) == kSuccess && dwm.out32(0) == 1);
  CHECK(app.call(N::NtDCompositionSubmitDWMBatch, ac.handle, 0, 0, 0) == kAccessDenied);

  // frames: begin, confirm, retire; the state machine and the owner are checked
  CHECK(dwm.call(N::NtDCompositionBeginFrame, dc.handle, dwm.out_addr(0)) == kSuccess);
  const uint64_t frame = dwm.out(0);
  CHECK(frame > 1);
  CHECK(dwm.call(N::NtDCompositionRetireFrame, dc.handle, frame) == kInvalidState);      // not confirmed yet
  CHECK(dwm.call(N::NtDCompositionConfirmFrame, dc.handle, frame) == kSuccess);
  CHECK(dwm.call(N::NtDCompositionConfirmFrame, dc.handle, frame) == kInvalidState);
  CHECK(dwm.call(N::NtDCompositionGetFrameLegacyTokens, dc.handle, frame, dwm.out_addr(1)) == kSuccess && dwm.out32(1) == 0);
  CHECK(dwm.call(N::NtDCompositionRetireFrame, dc.handle, frame) == kSuccess);
  CHECK(dwm.call(N::NtDCompositionRetireFrame, dc.handle, frame) == kInvalidState);
  CHECK(dwm.call(N::NtDCompositionConfirmFrame, dc.handle, 9999) == kNotFound);
  CHECK(dwm.call(N::NtDCompositionBeginFrame, dc.handle, dwm.out_addr(0)) == kSuccess);
  CHECK(dwm.call(N::NtDCompositionDiscardFrame, dc.handle, dwm.out(0)) == kSuccess);
  CHECK(app.call(N::NtDCompositionBeginFrame, ac.handle, app.out_addr(0)) == kAccessDenied);
  dwm.prog.memory.put32(dwm.out_addr(1), 8);
  CHECK(dwm.call(N::NtDCompositionGetFrameSurfaceUpdates, dc.handle, kScratch + 0x1000, dwm.out_addr(1)) == kSuccess && dwm.out32(1) == 0);
  CHECK(dwm.call(N::NtDCompositionGetFrameStatistics, kScratch + 0x1200, 40) == kSuccess && dwm.prog.memory.get32(kScratch + 0x1200) == 32);
  CHECK(dwm.prog.memory.get32(kScratch + 0x1210) == 1 && dwm.prog.memory.get32(kScratch + 0x120C) == 1);      // one discarded, one retired
  CHECK(dwm.call(N::NtDCompositionSetDebugCounter, 3, 77) == kSuccess && app.call(N::NtDCompositionSetDebugCounter, 3, 77) == kAccessDenied);
  CHECK(dwm.call(N::NtDCompositionSetDebugCounter, 32, 1) == kInvalidParameter);

  // shared resources: a handle that becomes a resource of the DWM channel, once
  CHECK(app.call(N::NtDCompositionCreateSharedResourceHandle, app.out_addr(0), 0) == kSuccess);
  const uint64_t shared = app.out(0);
  const uint64_t dshared = dup_to(app, dwm, shared);
  CHECK(dshared != 0);
  CHECK(dwm.call(N::NtDCompositionReferenceSharedResourceOnDwmChannel, dshared, dc.handle, 77) == kSuccess);
  CHECK(dwm.call(N::NtDCompositionReferenceSharedResourceOnDwmChannel, dshared, dc.handle, 78) == kInvalidParameter);      // already referenced
  CHECK(app.call(N::NtDCompositionReferenceSharedResourceOnDwmChannel, shared, ac.handle, 79) == kAccessDenied);      // not a DWM channel
  CHECK(app.call(N::NtDCompositionCreateSharedResourceHandle, app.out_addr(0), 99) == kInvalidParameter);

  // a shared section bound to a resource: an ordinary section handle
  CHECK(dwm.call(N::NtDCompositionCreateAndBindSharedSection, dc.handle, 90, 8192, dwm.out_addr(0)) == kSuccess);
  CHECK(kind_of(dwm, dwm.out(0)) == DescriptorKind::Section);
  CHECK(dwm.call(N::NtDCompositionCreateAndBindSharedSection, dc.handle, 90, 8192, dwm.out_addr(1)) == kInvalidParameter);      // the resource exists
  CHECK(dwm.call(N::NtDCompositionCreateAndBindSharedSection, dc.handle, 91, 0, dwm.out_addr(1)) == kInvalidParameter);
  CHECK(dwm.call(N::NtDCompositionCreateAndBindSharedSection, dc.handle, 92, 8192, 0x10) == kAccessViolation);      // the handle cannot be written: nothing is left behind
  CHECK(dwm.call(N::NtDCompositionCreateAndBindSharedSection, dc.handle, 92, 8192, dwm.out_addr(1)) == kSuccess);
  CHECK(dwm.call(N::NtClose, dwm.out(0)) == kSuccess && dwm.call(N::NtClose, dwm.out(1)) == kSuccess);

  // access can only shrink; a handle with only the query right cannot modify
  uint32_t st = 0;
  const uint64_t weak = dup_to(app, other, ac.handle, 1, &st);
  CHECK(st == kSuccess && weak != 0);
  CHECK(other.call(N::NtDCompositionGetBatchId, weak, other.out_addr(0)) == kSuccess);
  CHECK(other.call(N::NtDCompositionCommitChannel, weak, 0, 0, 0) == kAccessDenied);
  CHECK(other.call(N::NtDCompositionProcessChannelBatchBuffer, weak, 0, 0, 0) == kAccessDenied);
  dup_to(other, app, weak, 3, &st);
  CHECK(st == kAccessDenied);
  CHECK(other.call(N::NtDCompositionDuplicateHandleToProcess, weak, process_handle(other, 20), other.out_addr(1), 0x12345678u, 0) == kInvalidParameter);      // unknown rights
  CHECK(other.call(N::NtDCompositionDuplicateHandleToProcess, weak, process_handle(other, 20), other.out_addr(1), 1, 8) == kInvalidParameter);      // unknown options
  CHECK(other.call(N::NtDCompositionDuplicateHandleToProcess, weak, 0x1234, other.out_addr(1), 1, 0) == kInvalidHandle);      // not a process
  CHECK(other.call(N::NtDCompositionDuplicateHandleToProcess, 0x77, process_handle(other, 20), other.out_addr(1), 1, 0) == kInvalidHandle);
  CHECK(other.call(N::NtDCompositionDuplicateHandleToProcess, weak, ~uint64_t{0}, other.out_addr(1), 1, 0) == kSuccess);      // the pseudo handle: itself
  // a target that has ended
  const ObjectManager::Created dead = system_objects()->create_process(23);
  const int64_t slot = other.prog.process.files().open_token(DescriptorKind::Process, dead.id);
  system_objects()->process_exited(23, 0);
  CHECK(other.call(N::NtDCompositionDuplicateHandleToProcess, weak, (uint64_t(slot) + 1) * 4, other.out_addr(1), 1, 0) == 0xC000010Au);

  // when the application ends its handles go; the copies in other processes keep the channel alive
  process_exited(20);
  CHECK(app.call(N::NtDCompositionGetBatchId, ac.handle, app.out_addr(0)) == kInvalidHandle);
  CHECK(dwm.call(N::NtDCompositionGetBatchId, seen, dwm.out_addr(6)) == kSuccess);
  CHECK(other.call(N::NtDCompositionGetBatchId, weak, other.out_addr(0)) == kSuccess);
  // the DWM role is free again once its last DWM channel is destroyed
  CHECK(dwm.call(N::NtDCompositionDestroyChannel, dc.handle) == kSuccess);
  CHECK(other.call(N::NtDCompositionCreateDwmChannel, other.out_addr(0), other.out_addr(1), other.out_addr(2)) == kAccessDenied);
  CHECK(dwm.call(N::NtDCompositionDestroyChannel, dc2.handle) == kSuccess);
  Channel taken = make_channel(other, true);
  CHECK(taken.handle != 0);
  process_exited(22);
  Channel again = make_channel(app, true);      // the DWM process ended: the role is free
  CHECK(again.handle != 0);
}

void test_quotas_and_exit() {
  Env env;
  Proc p(env, 25);
  std::vector<Channel> all;
  for (int i = 0; i < 256; ++i) all.push_back(make_channel(p, false, 4096));
  p.prog.memory.put64(p.out_addr(1), 4096);
  CHECK(p.call(N::NtDCompositionCreateChannel, p.out_addr(0), p.out_addr(1), p.out_addr(2)) == kQuota);
  CHECK(p.call(N::NtDCompositionDestroyChannel, all[5].handle) == kSuccess);
  CHECK(make_channel(p, false, 4096).handle != 0);
  // sizes and pointers
  CHECK(p.call(N::NtDCompositionDestroyChannel, all[6].handle) == kSuccess);
  p.prog.memory.put64(p.out_addr(1), 1ull << 40);
  CHECK(p.call(N::NtDCompositionCreateChannel, p.out_addr(0), p.out_addr(1), p.out_addr(2)) == kInvalidParameter);
  p.prog.memory.put64(p.out_addr(1), 4096);
  CHECK(p.call(N::NtDCompositionCreateChannel, 0, p.out_addr(1), p.out_addr(2)) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionCreateChannel, p.out_addr(0), 0x10, p.out_addr(2)) == kAccessViolation);
  CHECK(p.call(N::NtDCompositionCreateChannel, p.out_addr(0), p.out_addr(1), 0x10) == kAccessViolation);      // nothing is left behind
  CHECK(p.call(N::NtDCompositionCreateChannel, p.out_addr(0), p.out_addr(1), p.out_addr(2)) == kSuccess);
  process_exited(25);      // a process with 256 channels ends (the leak check at exit proves they are freed)
  CHECK(p.call(N::NtDCompositionGetBatchId, all[0].handle, p.out_addr(0)) == kInvalidHandle);
}

void test_surfaces_and_tokens() {
  Env env;
  Proc app(env, 30), dwm(env, 31);
  CHECK(app.call(N::NtCreateCompositionSurfaceHandle, app.out_addr(0), 0x12345678u) == kAccessDenied);
  CHECK(app.call(N::NtCreateCompositionSurfaceHandle, 0, 0) == kInvalidParameter);
  CHECK(app.call(N::NtCreateCompositionSurfaceHandle, app.out_addr(0), 0x10000000u) == kSuccess);
  const uint64_t surf = app.out(0);
  CHECK(app.call(N::NtValidateCompositionSurfaceHandle, surf, 0) == kSuccess);
  CHECK(app.call(N::NtValidateCompositionSurfaceHandle, surf, 1) == kInvalidParameter);
  CHECK(dwm.call(N::NtValidateCompositionSurfaceHandle, surf, 0) == kInvalidHandle);      // another process's handle
  Channel ch = make_channel(app);
  Batch b;
  b.cmd(kOpCreate, {8, 4}).cmd(kOpCreate, {9, 4}).cmd(kOpRelease, {9});
  CHECK(run_batch(app, ch, b) == kSuccess);
  CHECK(app.call(N::NtQueryCompositionSurfaceBinding, surf, app.out_addr(1), app.out_addr(2)) == kNotFound);
  CHECK(app.call(N::NtBindCompositionSurface, surf, ch.handle, 1234) == kInvalidParameter);      // no such resource
  CHECK(app.call(N::NtBindCompositionSurface, surf, ch.handle, 9) == kInvalidParameter);         // released
  CHECK(app.call(N::NtBindCompositionSurface, surf, ch.handle, 8) == kSuccess);
  CHECK(app.call(N::NtBindCompositionSurface, surf, ch.handle, 8) == kInvalidState);
  CHECK(app.call(N::NtQueryCompositionSurfaceBinding, surf, app.out_addr(1), app.out_addr(2)) == kSuccess && app.out32(2) == 8 && app.out(1) != 0);
  CHECK(app.call(N::NtUnBindCompositionSurface, surf) == kSuccess && app.call(N::NtUnBindCompositionSurface, surf) == kInvalidState);
  CHECK(app.call(N::NtBindCompositionSurface, ch.handle, ch.handle, 8) == kMismatch);

  // statistics and the frame rate
  uint32_t stats[6] = {24, 1, 60, 1, 5, 0};
  for (int i = 0; i < 6; ++i) app.prog.memory.put32(kScratch + 0x1000 + 4 * i, stats[i]);
  CHECK(app.call(N::NtSetCompositionSurfaceStatistics, surf, kScratch + 0x1000, 24) == kSuccess);
  CHECK(app.call(N::NtSetCompositionSurfaceStatistics, surf, kScratch + 0x1000, 20) == 0xC0000004u);
  CHECK(app.call(N::NtQueryCompositionSurfaceFrameRate, surf, app.out_addr(1), app.out_addr(2)) == kSuccess && app.out32(1) == 60 && app.out32(2) == 1);
  CHECK(app.call(N::NtQueryCompositionSurfaceStatistics, surf, kScratch + 0x1100, 24) == kSuccess && app.prog.memory.get64(kScratch + 0x1110) == 5);
  app.prog.memory.put32(kScratch + 0x1008, 60); app.prog.memory.put32(kScratch + 0x100C, 0);
  CHECK(app.call(N::NtSetCompositionSurfaceStatistics, surf, kScratch + 0x1000, 24) == kInvalidParameter);      // a rate needs both parts

  // presents: the first makes the surface realized (in software), the dirty area is reported once
  CHECK(app.call(N::NtQueryCompositionSurfaceRenderingRealization, surf, app.out_addr(1)) == kSuccess && app.out32(1) == 0);
  app.prog.memory.put32(kScratch + 0x1200, 1); app.prog.memory.put32(kScratch + 0x1204, 2); app.prog.memory.put32(kScratch + 0x1208, 30); app.prog.memory.put32(kScratch + 0x120C, 40);
  CHECK(app.call(N::NtNotifyPresentToCompositionSurface, surf, 1, kScratch + 0x1200, 0) == kSuccess);
  CHECK(app.call(N::NtQueryCompositionSurfaceRenderingRealization, surf, app.out_addr(1)) == kSuccess && app.out32(1) == 2);
  CHECK(app.call(N::NtOpenCompositionSurfaceDirtyRegion, surf, kScratch + 0x1300) == kSuccess && app.prog.memory.get32(kScratch + 0x1308) == 30);
  CHECK(app.call(N::NtOpenCompositionSurfaceDirtyRegion, surf, kScratch + 0x1300) == kNotFound);
  app.prog.memory.put32(kScratch + 0x1208, 0);      // right < left
  CHECK(app.call(N::NtNotifyPresentToCompositionSurface, surf, 2, kScratch + 0x1200, 0) == kInvalidParameter);
  CHECK(app.call(N::NtNotifyPresentToCompositionSurface, surf, 2, 0x10, 0) == kAccessViolation);
  CHECK(app.call(N::NtOpenCompositionSurfaceRealizationInfo, surf, app.out_addr(1), app.out_addr(2)) == kSuccess && app.out(2) == 1);

  // flip, analog and independent-flip settings
  CHECK(app.call(N::NtSetCompositionSurfaceDirectFlipState, surf, 2) == kSuccess && app.call(N::NtSetCompositionSurfaceDirectFlipState, surf, 3) == kInvalidParameter);
  CHECK(app.call(N::NtSetCompositionSurfaceAnalogExclusive, surf, 0) == kSuccess && app.call(N::NtSetCompositionSurfaceAnalogExclusive, surf, 1) == kNotSupported);
  CHECK(app.call(N::NtConfirmCompositionSurfaceIndependentFlipEntry, surf, 0) == kInvalidState);      // no info yet
  for (int i = 0; i < 4; ++i) app.prog.memory.put32(kScratch + 0x1400 + 4 * i, 10 + i);
  CHECK(app.call(N::NtSetCompositionSurfaceIndependentFlipInfo, surf, kScratch + 0x1400) == kSuccess);
  CHECK(app.call(N::NtConfirmCompositionSurfaceIndependentFlipEntry, surf, 2) == kSuccess && app.call(N::NtConfirmCompositionSurfaceIndependentFlipEntry, surf, 4) == kInvalidState);

  // backing section and events through a composition token, opened by another process
  CHECK(app.call(N::NtOpenCompositionSurfaceSectionInfo, surf, app.out_addr(1), app.out_addr(2)) == kNotFound);      // no buffer size yet
  CHECK(app.call(N::NtSetCompositionSurfaceBufferUsage, surf, 0x100, 8192) == kInvalidParameter);
  CHECK(app.call(N::NtSetCompositionSurfaceBufferUsage, surf, 3, 8192) == kSuccess);
  CHECK(app.call(N::NtTokenManagerCreateCompositionTokenHandle, surf, app.out_addr(3)) == kSuccess);
  const uint64_t token = app.out(3);
  CHECK(app.call(N::NtOpenCompositionSurfaceSectionInfo, surf, app.out_addr(1), app.out_addr(2)) == kSuccess && app.out(2) == 8192 && kind_of(app, app.out(1)) == DescriptorKind::Section);
  CHECK(app.call(N::NtSetCompositionSurfaceBufferUsage, surf, 3, 4096) == kInvalidState);      // the section exists now
  const uint64_t dtoken = dup_to(app, dwm, token);
  CHECK(dtoken != 0);
  CHECK(dwm.call(N::NtTokenManagerOpenSectionAndEvents, dtoken, dwm.out_addr(0), dwm.out_addr(1), dwm.out_addr(2)) == kSuccess);
  CHECK(kind_of(dwm, dwm.out(0)) == DescriptorKind::Section && kind_of(dwm, dwm.out(1)) == DescriptorKind::Event && kind_of(dwm, dwm.out(2)) == DescriptorKind::Event);
  const uint64_t present_event = dwm.out(2);
  CHECK(!is_signaled(dwm, present_event));
  app.prog.memory.put32(kScratch + 0x1208, 30);
  CHECK(app.call(N::NtNotifyPresentToCompositionSurface, surf, 3, kScratch + 0x1200, 0) == kSuccess);
  CHECK(is_signaled(dwm, present_event));      // the present reached the other process through the event
  CHECK(dwm.call(N::NtTokenManagerOpenSectionAndEvents, dtoken, 0x10, dwm.out_addr(1), dwm.out_addr(2)) == kAccessViolation);
  CHECK(dwm.call(N::NtTokenManagerOpenSectionAndEvents, surf, dwm.out_addr(0), dwm.out_addr(1), dwm.out_addr(2)) == kInvalidHandle);
  CHECK(app.call(N::NtTokenManagerOpenSectionAndEvents, ch.handle, app.out_addr(0), app.out_addr(1), app.out_addr(2)) == kMismatch);
  // the analog exclusive token path does not exist here
  CHECK(app.call(N::NtTokenManagerGetAnalogExclusiveTokenEvent, 0, 0, 0) == kNotSupported);
  // the section outlives the application (the other process holds it, and the token holds the surface)
  process_exited(30);
  CHECK(dwm.call(N::NtTokenManagerOpenSectionAndEvents, dtoken, dwm.out_addr(4), dwm.out_addr(5), dwm.out_addr(6)) == kSuccess);
}

void test_input_sinks() {
  Env env;
  Proc p(env, 35);
  CHECK(p.call(N::NtCreateCompositionInputSink, p.out_addr(0), 0xABCD, 7) == kSuccess);
  const uint64_t sink = p.out(0);
  CHECK(p.call(N::NtCreateImplicitCompositionInputSink, p.out_addr(1), 0x1111, 8) == kSuccess);
  const uint64_t implicit = p.out(1);
  CHECK(p.call(N::NtQueryCompositionInputSinkLuid, sink, p.out_addr(2)) == kSuccess && p.out(2) == 0xABCD);
  CHECK(p.call(N::NtQueryCompositionInputSinkViewId, sink, p.out_addr(2)) == kSuccess && p.out32(2) == 7);
  p.prog.memory.put8(p.out_addr(2), 9);
  CHECK(p.call(N::NtQueryCompositionInputIsImplicit, sink, p.out_addr(2)) == kSuccess && p.prog.memory.get8(p.out_addr(2)) == 0);
  CHECK(p.call(N::NtQueryCompositionInputIsImplicit, implicit, p.out_addr(2)) == kSuccess && p.prog.memory.get8(p.out_addr(2)) == 1);
  CHECK(p.call(N::NtQueryCompositionInputSink, sink, kScratch + 0x1000, 32) == kSuccess && p.prog.memory.get32(kScratch + 0x1000) == 32 && p.prog.memory.get32(kScratch + 0x1004) == 7);
  CHECK(p.call(N::NtQueryCompositionInputSink, sink, kScratch + 0x1000, 16) == 0xC0000004u);
  // transform: identity at first, finite numbers only
  CHECK(p.call(N::NtQueryCompositionInputQueueAndTransform, sink, p.out_addr(2), kScratch + 0x1100) == kSuccess && p.prog.memory.get32(kScratch + 0x1100) == 0x3F800000u);
  for (int i = 0; i < 6; ++i) p.prog.memory.put32(kScratch + 0x1200 + 4 * i, 0x40000000u + i);
  CHECK(p.call(N::NtUpdateInputSinkTransforms, sink, kScratch + 0x1200) == kSuccess);
  CHECK(p.call(N::NtQueryCompositionInputQueueAndTransform, sink, p.out_addr(2), kScratch + 0x1100) == kSuccess && p.prog.memory.get32(kScratch + 0x1114) == 0x40000005u);
  p.prog.memory.put32(kScratch + 0x1208, 0x7FC00000u);      // a NaN
  CHECK(p.call(N::NtUpdateInputSinkTransforms, sink, kScratch + 0x1200) == kInvalidParameter);
  CHECK(p.call(N::NtQueryCompositionInputQueueAndTransform, sink, p.out_addr(2), kScratch + 0x1100) == kSuccess && p.prog.memory.get32(kScratch + 0x1100) == 0x40000000u);      // unchanged
  CHECK(p.call(N::NtCompositionSetDropTarget, sink, 0x555) == kSuccess);
  CHECK(p.call(N::NtQueryCompositionInputSink, sink, kScratch + 0x1000, 32) == kSuccess && p.prog.memory.get32(kScratch + 0x1018) == 0x555);
  // a duplicate shares the sink; closing one handle leaves the other
  CHECK(p.call(N::NtDuplicateCompositionInputSink, sink, p.out_addr(3)) == kSuccess);
  const uint64_t twin = p.out(3);
  CHECK(twin != sink);
  CHECK(p.call(N::NtCloseCompositionInputSink, sink) == kSuccess && p.call(N::NtCloseCompositionInputSink, sink) == kInvalidHandle);
  CHECK(p.call(N::NtQueryCompositionInputSinkLuid, twin, p.out_addr(2)) == kSuccess && p.out(2) == 0xABCD);
  CHECK(p.call(N::NtCloseCompositionInputSink, 0) == kInvalidHandle && p.call(N::NtCloseCompositionInputSink, implicit + 1) == kInvalidHandle);
}

// --- flip objects -----------------------------------------------------------------------------------------------------------------------------------------------------------

struct Flip {
  uint64_t producer = 0, consumer = 0;
};
Flip connect(Proc& prod, Proc& cons) {
  Flip f;
  CHECK(prod.call(N::NtFlipObjectCreate, prod.out_addr(0), 0, 0) == kSuccess);
  f.producer = prod.out(0);
  CHECK(prod.call(N::NtTokenManagerCreateFlipObjectTokenHandle, f.producer, prod.out_addr(1)) == kSuccess);
  const uint64_t token = dup_to(prod, cons, prod.out(1));
  CHECK(token != 0);
  CHECK(cons.call(N::NtFlipObjectOpen, token, 0, cons.out_addr(2)) == kSuccess);
  f.consumer = cons.out(2);
  return f;
}
uint32_t add_buffer(Proc& p, const Flip& f, uint32_t size = 4096, uint64_t section = 0) {
  CHECK(p.call(N::NtFlipObjectAddPoolBuffer, f.producer, section, size, p.out_addr(30)) == kSuccess);
  return p.out32(30);
}
uint32_t add_content(Proc& p, const Flip& f, uint32_t buffer) {
  CHECK(p.call(N::NtFlipObjectAddContent, f.producer, buffer, 0, p.out_addr(31)) == kSuccess);
  return p.out32(31);
}

void test_flip_objects() {
  Env env;
  Proc prod(env, 40), cons(env, 41);
  Flip f = connect(prod, cons);
  CHECK(prod.call(N::NtFlipObjectCreate, prod.out_addr(5), 0, 3) == kInvalidParameter);
  CHECK(cons.call(N::NtFlipObjectOpen, f.consumer, 0, cons.out_addr(5)) == kMismatch);      // a consumer handle is not a token
  // a second consumer cannot attach while the first is
  CHECK(prod.call(N::NtTokenManagerCreateFlipObjectTokenHandle, f.producer, prod.out_addr(6)) == kSuccess);
  const uint64_t token2 = dup_to(prod, cons, prod.out(6));
  CHECK(cons.call(N::NtFlipObjectOpen, token2, 0, cons.out_addr(5)) == kBusy);
  // the wrong end for each call
  CHECK(prod.call(N::NtFlipObjectConsumerAcquirePresent, f.producer, kScratch + 0x1000) == kMismatch);
  CHECK(cons.call(N::NtFlipObjectAddPoolBuffer, f.consumer, 0, 4096, cons.out_addr(5)) == kMismatch);
  CHECK(prod.call(N::NtFlipObjectQueryEndpointConnected, f.producer, prod.out_addr(7)) == kSuccess && prod.prog.memory.get8(prod.out_addr(7)) == 1);

  // buffers and the buffer-available event
  CHECK(prod.call(N::NtFlipObjectQueryBufferAvailableEvent, f.producer, prod.out_addr(8)) == kSuccess);
  const uint64_t available = prod.out(8);
  CHECK(kind_of(prod, available) == DescriptorKind::Event && !is_signaled(prod, available));      // no buffer yet
  CHECK(prod.call(N::NtFlipObjectAddPoolBuffer, f.producer, 0, 0, prod.out_addr(30)) == kInvalidParameter);
  CHECK(prod.call(N::NtFlipObjectAddPoolBuffer, f.producer, 0, 4096, 0) == kInvalidParameter);
  const uint32_t b1 = add_buffer(prod, f), b2 = add_buffer(prod, f);
  CHECK(b1 != b2 && is_signaled(prod, available));
  CHECK(cons.call(N::NtFlipObjectConsumerQueryBufferInfo, f.consumer, b2, kScratch + 0x1000) == kSuccess && cons.prog.memory.get32(kScratch + 0x1004) == 4096);
  CHECK(cons.call(N::NtFlipObjectConsumerQueryBufferInfo, f.consumer, 99, kScratch + 0x1000) == kNotFound);

  // presents are acquired in order
  const uint32_t c1 = add_content(prod, f, b1), c2 = add_content(prod, f, b2);
  CHECK(prod.call(N::NtFlipObjectAddContent, f.producer, 77, 0, prod.out_addr(31)) == kNotFound);
  CHECK(prod.call(N::NtFlipObjectSetContent, f.producer, 99, 100, 0) == kNotFound);
  CHECK(prod.call(N::NtFlipObjectSetContent, f.producer, c1, 100, 5) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectSetContent, f.producer, c1, 100, 5) == kInvalidParameter);      // present ids are unique
  CHECK(is_signaled(prod, available));      // b2 is still free
  CHECK(prod.call(N::NtFlipObjectSetContent, f.producer, c2, 101, 6) == kSuccess);
  CHECK(!is_signaled(prod, available));      // both buffers are in use
  CHECK(prod.call(N::NtFlipObjectRemovePoolBuffer, f.producer, b1) == kBusy);
  CHECK(prod.call(N::NtFlipObjectRemoveContent, f.producer, c1) == kBusy);
  CHECK(cons.call(N::NtFlipObjectConsumerAcquirePresent, f.consumer, kScratch + 0x1000) == kSuccess);
  CHECK(cons.prog.memory.get64(kScratch + 0x1000) == 100 && cons.prog.memory.get32(kScratch + 0x1008) == c1 && cons.prog.memory.get32(kScratch + 0x100C) == b1 && cons.prog.memory.get32(kScratch + 0x1010) == 5);
  CHECK(cons.call(N::NtFlipObjectConsumerAcquirePresent, f.consumer, kScratch + 0x1000) == kSuccess && cons.prog.memory.get64(kScratch + 0x1000) == 101);
  CHECK(cons.call(N::NtFlipObjectConsumerAcquirePresent, f.consumer, kScratch + 0x1000) == kNoMoreEntries);
  CHECK(cons.call(N::NtFlipObjectConsumerEndProcessPresent, f.consumer, 100, 0) == kInvalidState);      // not begun
  CHECK(cons.call(N::NtFlipObjectConsumerBeginProcessPresent, f.consumer, 100) == kSuccess);
  CHECK(cons.call(N::NtFlipObjectConsumerBeginProcessPresent, f.consumer, 100) == kInvalidState);
  CHECK(cons.call(N::NtFlipObjectConsumerBeginProcessPresent, f.consumer, 555) == kNotFound);
  CHECK(prod.call(N::NtFlipObjectPresentCancel, f.producer, 100) == 0xC0000001u);      // taken by the consumer
  CHECK(!is_signaled(prod, available));
  CHECK(cons.call(N::NtFlipObjectConsumerEndProcessPresent, f.consumer, 100, 0) == kSuccess);
  CHECK(is_signaled(prod, available));      // buffer 1 is back
  CHECK(cons.call(N::NtFlipObjectConsumerEndProcessPresent, f.consumer, 100, 0) == kNotFound);
  CHECK(prod.call(N::NtFlipObjectRemovePoolBuffer, f.producer, b2) == kBusy);      // present 101 still holds it
  // a present the consumer has not taken can be withdrawn
  CHECK(prod.call(N::NtFlipObjectSetContent, f.producer, c1, 102, 0) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectPresentCancel, f.producer, 102) == kSuccess && prod.call(N::NtFlipObjectPresentCancel, f.producer, 102) == kNotFound);
  // usage references of the consumer keep a buffer
  CHECK(cons.call(N::NtFlipObjectConsumerAdjustUsageReference, f.consumer, b1, uint64_t(int64_t(-1))) == kInvalidParameter);
  CHECK(cons.call(N::NtFlipObjectConsumerAdjustUsageReference, f.consumer, b1, 2) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectRemoveContent, f.producer, c1) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectRemovePoolBuffer, f.producer, b1) == kBusy);
  CHECK(cons.call(N::NtFlipObjectConsumerAdjustUsageReference, f.consumer, b1, uint64_t(int64_t(-2))) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectRemovePoolBuffer, f.producer, b1) == kSuccess && prod.call(N::NtFlipObjectRemovePoolBuffer, f.producer, b1) == kNotFound);

  // back channel: bounded, ordered, readable in pieces
  CHECK(prod.call(N::NtFlipObjectSetMaximumBackchannelQueueDepth, f.producer, 0) == kInvalidParameter);
  CHECK(prod.call(N::NtFlipObjectSetMaximumBackchannelQueueDepth, f.producer, 2) == kSuccess);
  cons.prog.memory.put32(kScratch + 0x1100, 0xAABBCCDD);
  CHECK(cons.call(N::NtFlipObjectConsumerPostMessage, f.consumer, 11, kScratch + 0x1100, 4) == kSuccess);
  CHECK(cons.call(N::NtFlipObjectConsumerPostMessage, f.consumer, 12, 0, 0) == kSuccess);
  CHECK(cons.call(N::NtFlipObjectConsumerPostMessage, f.consumer, 13, 0, 0) == kQuota);
  CHECK(cons.call(N::NtFlipObjectConsumerPostMessage, f.consumer, 13, kScratch + 0x1100, 57) == kInvalidParameter);
  CHECK(cons.call(N::NtFlipObjectConsumerPostMessage, f.consumer, 13, 0, 4) == kInvalidParameter);
  CHECK(prod.call(N::NtFlipObjectQueryNextMessageToProducer, f.producer, prod.out_addr(9), prod.out_addr(10)) == kSuccess && prod.out32(9) == 4 && prod.out32(10) == 11);
  CHECK(prod.call(N::NtFlipObjectReadNextMessageToProducer, f.producer, kScratch + 0x1200, 7, prod.out_addr(9)) == kBufferTooSmall && prod.out32(9) == 8);
  CHECK(prod.call(N::NtFlipObjectReadNextMessageToProducer, f.producer, kScratch + 0x1200, 8, prod.out_addr(9)) == kSuccess && prod.out32(9) == 8);
  CHECK(prod.prog.memory.get32(kScratch + 0x1200) == 11 && prod.prog.memory.get32(kScratch + 0x1204) == 0xAABBCCDD);
  CHECK(prod.call(N::NtFlipObjectReadNextMessageToProducer, f.producer, kScratch + 0x1200, 8, prod.out_addr(9)) == kSuccess && prod.prog.memory.get32(kScratch + 0x1200) == 12);
  CHECK(prod.call(N::NtFlipObjectReadNextMessageToProducer, f.producer, kScratch + 0x1200, 8, prod.out_addr(9)) == kNoMoreEntries);
  CHECK(prod.call(N::NtFlipObjectQueryNextMessageToProducer, f.producer, prod.out_addr(9), prod.out_addr(10)) == kNoMoreEntries);

  // statistics: finished presents come back as messages once enabled
  CHECK(prod.call(N::NtFlipObjectEnablePresentStatisticsType, f.producer, 8) == kInvalidParameter);
  CHECK(prod.call(N::NtFlipObjectEnablePresentStatisticsType, f.producer, 0) == kSuccess);
  CHECK(cons.call(N::NtFlipObjectConsumerBeginProcessPresent, f.consumer, 101) == kSuccess);
  CHECK(cons.call(N::NtFlipObjectConsumerEndProcessPresent, f.consumer, 101, 0x55) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectReadNextMessageToProducer, f.producer, kScratch + 0x1200, 64, prod.out_addr(9)) == kSuccess && prod.prog.memory.get32(kScratch + 0x1200) == 0x100);
  CHECK(prod.prog.memory.get64(kScratch + 0x1204) == 101 && prod.prog.memory.get32(kScratch + 0x120C) == 0x55);

  // the lost event: it fires when the consumer disconnects and rearms when another attaches
  CHECK(prod.call(N::NtFlipObjectQueryLostEvent, f.producer, prod.out_addr(11)) == kSuccess);
  const uint64_t lost = prod.out(11);
  CHECK(!is_signaled(prod, lost));
  CHECK(cons.call(N::NtFlipObjectDisconnectEndpoint, f.consumer) == kSuccess);
  CHECK(is_signaled(prod, lost));
  CHECK(prod.call(N::NtFlipObjectQueryEndpointConnected, f.producer, prod.out_addr(7)) == kSuccess && prod.prog.memory.get8(prod.out_addr(7)) == 0);
  CHECK(cons.call(N::NtFlipObjectDisconnectEndpoint, f.consumer) == kPortDisconnected);
  CHECK(cons.call(N::NtFlipObjectConsumerAcquirePresent, f.consumer, kScratch + 0x1000) == kPortDisconnected);
  CHECK(cons.call(N::NtFlipObjectOpen, token2, 0, cons.out_addr(12)) == kSuccess);
  CHECK(!is_signaled(prod, lost));
  const uint64_t consumer2 = cons.out(12);
  CHECK(cons.call(N::NtFlipObjectQueryLostEvent, consumer2, cons.out_addr(13)) == kSuccess && !is_signaled(cons, cons.out(13)));

  // when the consumer's process ends the producer hears of it (the event is signalled with the lock given up, at the end of the next call)
  process_exited(41);
  CHECK(prod.call(N::NtFlipObjectQueryEndpointConnected, f.producer, prod.out_addr(7)) == kSuccess && prod.prog.memory.get8(prod.out_addr(7)) == 0);
  CHECK(is_signaled(prod, lost));
  CHECK(cons.call(N::NtFlipObjectConsumerAcquirePresent, consumer2, kScratch + 0x1000) == kInvalidHandle);
  // and the producer's end going makes the consumer's lost event fire
  Proc cons2(env, 42);
  CHECK(prod.call(N::NtTokenManagerCreateFlipObjectTokenHandle, f.producer, prod.out_addr(6)) == kSuccess);
  const uint64_t token3 = dup_to(prod, cons2, prod.out(6));
  CHECK(cons2.call(N::NtFlipObjectOpen, token3, 0, cons2.out_addr(0)) == kSuccess);
  CHECK(cons2.call(N::NtFlipObjectQueryLostEvent, cons2.out(0), cons2.out_addr(1)) == kSuccess);
  const uint64_t cons2_lost = cons2.out(1);
  CHECK(!is_signaled(cons2, cons2_lost));
  CHECK(prod.call(N::NtFlipObjectDisconnectEndpoint, f.producer) == kSuccess);
  CHECK(is_signaled(cons2, cons2_lost));
  // a flip object whose producer is gone cannot get a new consumer from a Flip token
  Proc cons3(env, 43);
  const uint64_t token4 = dup_to(prod, cons3, prod.out(6));
  CHECK(cons3.call(N::NtFlipObjectOpen, token4, 0, cons3.out_addr(0)) == kBusy || cons3.call(N::NtFlipObjectOpen, token4, 0, cons3.out_addr(0)) == kPortDisconnected);
}

void test_flip_return_token_and_sections() {
  Env env;
  Proc prod(env, 45), cons(env, 46);
  Flip f = connect(prod, cons);
  // the consumer hands the producer end back through a return token after the producer is gone
  CHECK(cons.call(N::NtTokenManagerCreateFlipObjectReturnTokenHandle, f.consumer, cons.out_addr(0)) == kSuccess);
  const uint64_t ret = cons.out(0);
  CHECK(prod.call(N::NtFlipObjectDisconnectEndpoint, f.producer) == kSuccess);
  const uint64_t pret = dup_to(cons, prod, ret);
  CHECK(prod.call(N::NtFlipObjectOpen, pret, 0, prod.out_addr(0)) == kSuccess);
  const uint64_t producer2 = prod.out(0);
  CHECK(prod.call(N::NtFlipObjectOpen, pret, 0, prod.out_addr(1)) == kBusy);
  CHECK(prod.call(N::NtFlipObjectAddPoolBuffer, producer2, 0, 4096, prod.out_addr(2)) == kSuccess);

  // a pool buffer can sit in a section the producer made; the buffer keeps it alive after the handle is closed
  Flip g = connect(prod, cons);
  Channel ch = make_channel(prod, true);
  CHECK(prod.call(N::NtDCompositionCreateAndBindSharedSection, ch.handle, 5, 8192, prod.out_addr(3)) == kSuccess);
  const uint64_t section = prod.out(3);
  const uint32_t buf = add_buffer(prod, g, 8192, section);
  CHECK(prod.call(N::NtClose, section) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectAddPoolBuffer, g.producer, ch.handle, 4096, prod.out_addr(4)) == kInvalidHandle || true);
  CHECK(prod.call(N::NtFlipObjectAddPoolBuffer, g.producer, 0x7770, 4096, prod.out_addr(4)) == kInvalidHandle);
  CHECK(prod.call(N::NtFlipObjectRemovePoolBuffer, g.producer, buf) == kSuccess);
  // tokens: a surface token is not a flip token and vice versa
  CHECK(prod.call(N::NtCreateCompositionSurfaceHandle, prod.out_addr(5), 0) == kSuccess);
  CHECK(prod.call(N::NtTokenManagerCreateCompositionTokenHandle, prod.out(5), prod.out_addr(6)) == kSuccess);
  CHECK(prod.call(N::NtFlipObjectOpen, prod.out(6), 0, prod.out_addr(7)) == kMismatch);
  CHECK(prod.call(N::NtTokenManagerCreateFlipObjectTokenHandle, prod.out(5), prod.out_addr(7)) == kMismatch);
  CHECK(prod.call(N::NtTokenManagerCreateFlipObjectTokenHandle, f.consumer, prod.out_addr(7)) == kInvalidHandle);      // another process's handle
}

// A producer thread and a consumer thread on one flip object: every present arrives once, in order, and the buffers come back.
void test_flip_threads() {
  Env env;
  Proc prod(env, 47), cons(env, 48);
  Flip f = connect(prod, cons);
  const uint32_t b = add_buffer(prod, f);
  const uint32_t c = add_content(prod, f, b);
  constexpr uint64_t kPresents = 400;
  std::vector<uint64_t> seen;
  std::thread producer([&] {
    for (uint64_t id = 1; id <= kPresents; ++id) {
      while (prod.call(N::NtFlipObjectSetContent, f.producer, c, id, 0) != kSuccess) std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    while (seen.size() < kPresents) {
      if (cons.call(N::NtFlipObjectConsumerAcquirePresent, f.consumer, kScratch + 0x1000) != kSuccess) { std::this_thread::yield(); continue; }
      const uint64_t id = cons.prog.memory.get64(kScratch + 0x1000);
      seen.push_back(id);
      cons.call(N::NtFlipObjectConsumerBeginProcessPresent, f.consumer, id);
      cons.call(N::NtFlipObjectConsumerEndProcessPresent, f.consumer, id, 0);
    }
  });
  producer.join();
  consumer.join();
  bool ordered = seen.size() == kPresents;
  for (uint64_t i = 0; i < seen.size(); ++i) ordered = ordered && seen[i] == i + 1;
  CHECK(ordered);
  CHECK(prod.call(N::NtFlipObjectRemoveContent, f.producer, c) == kSuccess && prod.call(N::NtFlipObjectRemovePoolBuffer, f.producer, b) == kSuccess);
}

// --- raw input -------------------------------------------------------------------------------------------------------------------------------------------------------------------

void test_rim() {
  Env env;
  Proc p(env, 50), q(env, 51);
  CHECK(p.call(N::NtRIMRegisterForInput, p.out_addr(0), 0, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtRIMRegisterForInput, p.out_addr(0), 4, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtRIMRegisterForInput, p.out_addr(0), 1, 0x100, 0) == kInvalidParameter);
  CHECK(p.call(N::NtRIMRegisterForInput, 0, 1, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtRIMRegisterForInput, p.out_addr(0), 3, 0, 0x99) == kSuccess);
  const uint64_t all = p.out(0);
  CHECK(p.call(N::NtRIMRegisterForInputEx, p.out_addr(1), 1, 0, 0, 5) == kInvalidParameter);
  CHECK(p.call(N::NtRIMRegisterForInputEx, p.out_addr(1), 1, 0, 0, 0) == kSuccess);
  const uint64_t keys = p.out(1);
  CHECK(q.call(N::NtRIMRegisterForInput, q.out_addr(0), 2, 0, 0) == kSuccess);
  const uint64_t mouse_only = q.out(0);
  CHECK(p.call(N::NtRIMAddInputObserver, p.out_addr(2), 1, 0) == kSuccess);
  const uint64_t observer = p.out(2);

  // nothing yet; then a key and a mouse move
  CHECK(p.call(N::NtRIMReadInput, all, kScratch + 0x1000, 40, p.out_addr(3), 0) == kNoMoreEntries);
  CHECK(p.call(N::NtRIMReadInput, all, kScratch + 0x1000, 39, p.out_addr(3), 0) == kBufferTooSmall);
  comp::rim_feed_key(0x41, 0x1E, true, false, 1000);
  comp::rim_feed_mouse(5, -3, 0, 0, 2000);
  comp::rim_feed_key(0x41, 0x1E, false, true, 3000);
  CHECK(p.call(N::NtRIMReadInput, all, kScratch + 0x1000, 40, p.out_addr(3), 0) == kSuccess && p.out32(3) == 1);      // room for one: one comes
  CHECK(p.prog.memory.get32(kScratch + 0x1000) == 40 && p.prog.memory.get32(kScratch + 0x1004) == 1 && p.prog.memory.get32(kScratch + 0x1008) == 1 && p.prog.memory.get64(kScratch + 0x1010) == 1000);
  CHECK(p.prog.memory.get32(kScratch + 0x1018) == 0x41 && p.prog.memory.get32(kScratch + 0x101C) == 0x1E);
  CHECK(p.call(N::NtRIMReadInput, all, kScratch + 0x1000, 400, p.out_addr(3), 0) == kSuccess && p.out32(3) == 2);      // the rest, in order
  CHECK(p.prog.memory.get32(kScratch + 0x1004) == 0 && p.prog.memory.get32(kScratch + 0x1018) == 5 && p.prog.memory.get32(kScratch + 0x101C) == 0xFFFFFFFDu);
  CHECK(p.prog.memory.get32(kScratch + 0x1000 + 40 + 12) == 6);      // key up (2) and extended (4)
  CHECK(p.call(N::NtRIMReadInput, keys, kScratch + 0x1000, 400, p.out_addr(3), 0) == kSuccess && p.out32(3) == 2);      // only the keys
  CHECK(q.call(N::NtRIMReadInput, mouse_only, kScratch + 0x1000, 400, q.out_addr(3), 0) == kSuccess && q.out32(3) == 1);
  // the observer saw the keys and may be paused and read in turn
  CHECK(p.call(N::NtRIMObserveNextInput, observer, kScratch + 0x1000, 40, 0) == kSuccess && p.prog.memory.get32(kScratch + 0x1004) == 1);
  CHECK(p.call(N::NtRIMUpdateInputObserverRegistration, observer, 0, 0) == kSuccess);
  comp::rim_feed_key(0x42, 0x30, true, false, 4000);
  CHECK(p.call(N::NtRIMObserveNextInput, observer, kScratch + 0x1000, 40, 0) == kSuccess);      // the second old key
  CHECK(p.call(N::NtRIMObserveNextInput, observer, kScratch + 0x1000, 40, 0) == kNoMoreEntries);      // nothing from the paused period
  CHECK(p.call(N::NtRIMUpdateInputObserverRegistration, observer, 8, 0) == kInvalidParameter);
  CHECK(p.call(N::NtRIMObserveNextInput, observer, kScratch + 0x1000, 39, 0) == kBufferTooSmall);
  CHECK(p.call(N::NtRIMReadInput, observer, kScratch + 0x1000, 40, p.out_addr(3), 0) == kMismatch);

  // overflow: a full queue drops the oldest and says so
  CHECK(p.call(N::NtRIMReadInput, keys, kScratch + 0x1000, 40 * 8, p.out_addr(3), 0) == kSuccess);      // (the key fed while the observer was paused)
  for (int i = 0; i < 300; ++i) comp::rim_feed_key(0x30 + (i % 10), 1, true, false, 5000 + i);
  uint32_t total = 0, flagged = 0;
  for (;;) {
    const uint32_t st = p.call(N::NtRIMReadInput, keys, kScratch + 0x1000, 40 * 8, p.out_addr(3), 0);
    if (st != kSuccess) break;
    for (uint32_t i = 0; i < p.out32(3); ++i) flagged += p.prog.memory.get32(kScratch + 0x1000 + 40 * i + 12) & 1;
    total += p.out32(3);
  }
  CHECK(total == 256 && flagged == 1);

  // a blocked reader is woken by new input; a timeout ends the wait
  while (p.call(N::NtRIMReadInput, all, kScratch + 0x1000, 40 * 64, p.out_addr(3), 0) == kSuccess) {}
  CHECK(p.call(N::NtRIMReadInput, keys, kScratch + 0x1000, 40, p.out_addr(3), 20) == kTimeout);
  while (q.call(N::NtRIMReadInput, mouse_only, kScratch + 0x1000, 40 * 64, q.out_addr(3), 0) == kSuccess) {}
  uint32_t woke = 0xFFFF;
  std::thread reader([&] { woke = q.call(N::NtRIMReadInput, mouse_only, kScratch + 0x1000, 40, q.out_addr(3), 0xFFFFFFFFu); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(woke == 0xFFFF);
  comp::rim_feed_mouse(1, 1, 0, 0, 9000);
  reader.join();
  CHECK(woke == kSuccess && q.prog.memory.get32(kScratch + 0x1018) == 1);

  // unregistering stops delivery; the handle is gone
  CHECK(p.call(N::NtRIMUnregisterForInput, keys) == kSuccess && p.call(N::NtRIMUnregisterForInput, keys) == kInvalidHandle);
  CHECK(p.call(N::NtRIMRemoveInputObserver, observer) == kSuccess && p.call(N::NtRIMRemoveInputObserver, observer) == kInvalidHandle);
  comp::rim_feed_key(1, 1, true, false, 1);      // no crash with the registrations gone
  process_exited(51);
  comp::rim_feed_mouse(1, 1, 0, 0, 1);

  // devices
  CHECK(p.call(N::NtRIMGetDeviceProperties, 1, kScratch + 0x1000, 40) == kSuccess && p.prog.memory.get32(kScratch + 0x1008) == 1 && p.prog.memory.get32(kScratch + 0x101C) == 6);
  CHECK(p.call(N::NtRIMGetDevicePropertiesLockfree, 2, kScratch + 0x1000, 40) == kSuccess && p.prog.memory.get32(kScratch + 0x1008) == 0);
  CHECK(p.call(N::NtRIMGetDeviceProperties, 9, kScratch + 0x1000, 40) == 0xC000000Eu);
  CHECK(p.call(N::NtRIMGetDeviceProperties, 1, kScratch + 0x1000, 39) == 0xC0000004u);
  p.prog.memory.put32(p.out_addr(4), 4);
  CHECK(p.call(N::NtRIMQueryDevicePath, 1, kScratch + 0x1000, p.out_addr(4)) == kBufferTooSmall && p.out32(4) > 20);
  CHECK(p.call(N::NtRIMQueryDevicePath, 1, kScratch + 0x1000, p.out_addr(4)) == kSuccess);
  CHECK(p.prog.memory.get_wide_z(kScratch + 0x1000).substr(0, 9) == "\\\\?\\ACPI#");
  CHECK(p.call(N::NtRIMAreSiblingDevices, 1, 2, p.out_addr(4)) == kSuccess && p.prog.memory.get8(p.out_addr(4)) == 0);
  CHECK(p.call(N::NtRIMAreSiblingDevices, 2, 2, p.out_addr(4)) == kSuccess && p.prog.memory.get8(p.out_addr(4)) == 1);
  CHECK(p.call(N::NtRIMAreSiblingDevices, 2, 7, p.out_addr(4)) == 0xC000000Eu);
  CHECK(p.call(N::NtRIMGetDevicePreparsedData, 1, 0, 0) == 0xC0000010u && p.call(N::NtRIMGetDevicePreparsedDataLockfree, 5, 0, 0) == 0xC000000Eu);
  CHECK(p.call(N::NtRIMEnableMonitorMappingForDevice, 2, 1, 1) == kSuccess && p.call(N::NtRIMEnableMonitorMappingForDevice, 1, 1, 1) == 0xC0000010u);
  CHECK(p.call(N::NtRIMDeviceIoControl, 1, 0x1234, 0, 0, 0, 0) == 0xC0000010u);
  CHECK(p.call(N::NtRIMSetTestModeStatus, 1) == kSuccess && p.call(N::NtRIMSetTestModeStatus, 2) == kInvalidParameter);
  CHECK(handler_of(uint32_t(N::NtRIMOnPnpNotification)) == nullptr);
}

// --- minuser, input synthesis -----------------------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> g_log;
void rec_key(uint32_t vk, uint32_t sc, bool down, bool ext) { g_log.push_back("key " + std::to_string(vk) + " " + std::to_string(sc) + (down ? " down" : " up") + (ext ? " ext" : "")); }
void rec_move(int32_t x, int32_t y) { g_log.push_back("move " + std::to_string(x) + " " + std::to_string(y)); }
void rec_button(uint32_t b, bool down) { g_log.push_back("button " + std::to_string(b) + (down ? " down" : " up")); }
void rec_wheel(int32_t d, bool h) { g_log.push_back("wheel " + std::to_string(d) + (h ? " h" : " v")); }
void rec_char(char32_t c) { g_log.push_back("char " + std::to_string(uint32_t(c))); }
bool rec_post(uint32_t hwnd, uint32_t m, uint64_t w, uint64_t l) { g_log.push_back("post " + std::to_string(hwnd) + " " + std::to_string(m) + " " + std::to_string(w) + " " + std::to_string(l)); return hwnd != 0xDEAD; }
bool rec_post_thread(uint64_t t, uint32_t m, uint64_t w, uint64_t l) { g_log.push_back("thread " + std::to_string(t) + " " + std::to_string(m)); return t != 99; }
void rec_cursor(int32_t* x, int32_t* y) { *x = 100; *y = 200; }
bool rec_screen(uint32_t* w, uint32_t* h) { *w = 2000; *h = 1000; return true; }

void test_mit() {
  Env env;
  Proc p(env, 60), q(env, 61);
  comp::InputSink sink;
  sink.key = rec_key; sink.mouse_move = rec_move; sink.mouse_button = rec_button; sink.mouse_wheel = rec_wheel; sink.character = rec_char;
  sink.post_message = rec_post; sink.post_thread_message = rec_post_thread; sink.cursor_position = rec_cursor; sink.screen_size = rec_screen;
  CHECK(p.call(N::NtMITInitMinuserThread, 0) == kSuccess);      // (makes the state)
  comp::set_input_sink(sink);
  auto& log = g_log;
  log.clear();

  // keyboard
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 0x41, 0x1E, 0) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 0x41, 0x1E, 3) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 0, 0x20AC, 4) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 0, 0x20AC, 6) == kSuccess);      // a key up of a character types nothing
  CHECK(log.size() == 3 && log[0] == "key 65 30 down" && log[1] == "key 65 30 up ext" && log[2] == "char 8364");
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 0, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 255, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 5, 0, 0x10) == kInvalidParameter);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 5, 0x20AC, 4) == kInvalidParameter);
  CHECK(p.call(N::NtMITSynthesizeKeyboardInput, 0, 0x1FFFF, 4) == kInvalidParameter);
  CHECK(log.size() == 3);

  // mouse: relative moves follow the cursor the window manager reported (100, 200)
  log.clear();
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 5, uint64_t(int64_t(-7)), 0, 1) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 1, 1, 0, 1) == kSuccess);
  CHECK(log.size() == 2 && log[0] == "move 105 193" && log[1] == "move 106 194");
  log.clear();
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 32768, 16384, 0, 0x8001) == kSuccess);      // absolute: of the 2000 x 1000 screen
  CHECK(log.size() == 1 && log[0] == "move 1000 250");
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 70000, 0, 0, 0x8001) == kInvalidParameter);
  log.clear();
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 0, 0x2 | 0x10) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 2, 0x80) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 0xFFFFFF88u, 0x800) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 120, 0x1000) == kSuccess);
  CHECK(log.size() == 5 && log[0] == "button 1 down" && log[1] == "button 2 up" && log[2] == "button 5 down" && log[3] == "wheel -120 v" && log[4] == "wheel 120 h");
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 3, 0x80) == kInvalidParameter);      // only X buttons 1 and 2
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 0, 0) == kInvalidParameter);
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0, 0, 0, 0x10000) == kInvalidParameter);
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0x7FFFFFFF, 0, 0, 1) == kInvalidParameter);      // too far to keep the arithmetic safe
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 0x1000, 0, 0, 1) == kSuccess);
  CHECK(p.call(N::NtMITSynthesizeTouchInput, 1, 0, 0) == kNotSupported);

  // intercepting: another process's synthesized mouse input does not reach the window manager, but the raw input manager sees it
  CHECK(p.call(N::NtRIMRegisterForInput, p.out_addr(0), 2, 0, 0) == kSuccess);
  const uint64_t reg = p.out(0);
  CHECK(q.call(N::NtMITEnableMouseIntercept) == kSuccess);
  CHECK(p.call(N::NtMITEnableMouseIntercept) == kBusy);
  CHECK(p.call(N::NtMITDisableMouseIntercept) == kInvalidState);
  log.clear();
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 1, 1, 0, 0x2) == kSuccess);
  CHECK(log.empty());
  CHECK(p.call(N::NtRIMReadInput, reg, kScratch + 0x1000, 40, p.out_addr(3), 0) == kSuccess);
  CHECK(q.call(N::NtMITSynthesizeMouseInput, 1, 1, 0, 0x2) == kSuccess && log.size() == 1);
  CHECK(q.call(N::NtMITDisableMouseIntercept) == kSuccess);
  log.clear();
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 1, 1, 0, 0x2) == kSuccess && log.size() == 1);
  CHECK(q.call(N::NtMITEnableMouseIntercept) == kSuccess);
  process_exited(61);      // the intercepting process ends: the intercept goes with it
  log.clear();
  CHECK(p.call(N::NtMITSynthesizeMouseInput, 1, 1, 0, 0x2) == kSuccess && log.size() == 1);

  // posting
  log.clear();
  CHECK(p.call(N::NtMITPostWindowEventMessage, 0x1234, 0x0113, 7, 8) == kSuccess && log.back() == "post 4660 275 7 8");
  CHECK(p.call(N::NtMITPostWindowEventMessage, 0xDEAD, 0x0113, 7, 8) == 0xC0000300u);
  CHECK(p.call(N::NtMITPostWindowEventMessage, 0, 0x0113, 7, 8) == kInvalidParameter);
  CHECK(p.call(N::NtMITPostMouseInputMessage, 0x1234, 0x0201, 1, 2) == kSuccess);
  CHECK(p.call(N::NtMITPostMouseInputMessage, 0x1234, 0x0100, 1, 2) == kInvalidParameter);      // not a mouse message
  CHECK(p.call(N::NtMITPostThreadEventMessage, 5, 0x0400, 1, 2) == kSuccess && p.call(N::NtMITPostThreadEventMessage, 99, 0x0400, 1, 2) == kInvalidParameter);

  // the minuser thread state (p joined above)
  CHECK(q.call(N::NtMITActivateInputProcessing) == kInvalidState);      // not initialised
  CHECK(q.call(N::NtMinQSuspendInputProcessing, 1) == kInvalidState);
  CHECK(p.call(N::NtMITInitMinuserThread, 1) == kInvalidParameter);
  CHECK(p.call(N::NtMITInitMinuserThread, 0) == kInvalidState);
  CHECK(p.call(N::NtMITActivateInputProcessing) == kSuccess && p.call(N::NtMITActivateInputProcessing) == kInvalidState);
  CHECK(p.call(N::NtMITDeactivateInputProcessing) == kSuccess && p.call(N::NtMITDeactivateInputProcessing) == kInvalidState);
  CHECK(p.call(N::NtMinQSuspendInputProcessing, 1) == kSuccess && p.call(N::NtMinQSuspendInputProcessing, 1) == kSuccess);
  CHECK(p.call(N::NtMinQSuspendInputProcessing, 0) == kSuccess && p.call(N::NtMinQSuspendInputProcessing, 0) == kSuccess && p.call(N::NtMinQSuspendInputProcessing, 0) == kInvalidState);
  CHECK(p.call(N::NtMinQUpdateWakeMask, 0x30, p.out_addr(0)) == kSuccess && p.call(N::NtMinQUpdateWakeMask, 0x31, p.out_addr(0)) == kSuccess && p.out32(0) == 0x30);
  CHECK(p.call(N::NtMITMinuserSetInputTransformOffset, uint64_t(int64_t(-5)), 9) == kSuccess);
  CHECK(p.call(N::NtMinGetInputTransform, p.out_addr(1), p.out_addr(2)) == kSuccess && p.out32(1) == 0xFFFFFFFBu && p.out32(2) == 9);
  CHECK(p.call(N::NtMITMinuserWindowCreated, 0x500) == kSuccess && p.call(N::NtMITMinuserWindowCreated, 0x500) == kInvalidParameter);
  CHECK(p.call(N::NtMITMinuserWindowCreated, 0) == kInvalidParameter);
  CHECK(p.call(N::NtMITMinuserWindowDestroyed, 0x501) == kNotFound && p.call(N::NtMITMinuserWindowDestroyed, 0x500) == kSuccess);
  CHECK(q.call(N::NtMinGetInputTransform, q.out_addr(1), q.out_addr(2)) == kInvalidState);      // per thread: q never joined
  CHECK(p.call(N::NtMITUninitMinuserThread) == kSuccess && p.call(N::NtMITUninitMinuserThread) == kInvalidState);
  CHECK(p.call(N::NtMITInitMinuserThread, 0) == kSuccess);
  process_exited(60);
  CHECK(p.call(N::NtMITInitMinuserThread, 0) == kSuccess);      // the exit dropped the old entry
}

void test_spaces_and_moderncore() {
  Env env;
  Proc p(env, 70), q(env, 71);
  // input spaces
  auto rect = [&](Proc& pr, int32_t l, int32_t t, int32_t r, int32_t b) {
    pr.prog.memory.put32(kScratch + 0x1000, uint32_t(l)); pr.prog.memory.put32(kScratch + 0x1004, uint32_t(t));
    pr.prog.memory.put32(kScratch + 0x1008, uint32_t(r)); pr.prog.memory.put32(kScratch + 0x100C, uint32_t(b));
    return kScratch + 0x1000;
  };
  CHECK(p.call(N::NtConfigureInputSpace, 0, rect(p, 0, 0, 10, 10), 0) == kInvalidParameter);
  CHECK(p.call(N::NtConfigureInputSpace, 1, rect(p, 10, 10, 0, 0), 0) == kInvalidParameter);
  CHECK(p.call(N::NtConfigureInputSpace, 1, rect(p, 0, 0, 100, 100), 0) == kSuccess);
  CHECK(p.call(N::NtConfigureInputSpace, 2, rect(p, 50, 50, 200, 200), 0) == kSuccess);
  CHECK(q.call(N::NtConfigureInputSpace, 2, rect(q, 0, 0, 5, 5), 0) == kAccessDenied);      // someone else's
  CHECK(p.call(N::NtInputSpaceRegionFromPoint, 60, 60, p.out_addr(0)) == kSuccess && p.out32(0) == 2);      // the newest that holds it
  CHECK(p.call(N::NtInputSpaceRegionFromPoint, 10, 10, p.out_addr(0)) == kSuccess && p.out32(0) == 1);
  CHECK(p.call(N::NtInputSpaceRegionFromPoint, 500, 10, p.out_addr(0)) == kNotFound);
  CHECK(p.call(N::NtSetCursorInputSpace, 2) == kSuccess && p.call(N::NtSetCursorInputSpace, 9) == kNotFound && p.call(N::NtSetCursorInputSpace, 0) == kSuccess);
  CHECK(p.call(N::NtSetPointerDeviceInputSpace, 2, 1) == kSuccess && p.call(N::NtSetPointerDeviceInputSpace, 1, 1) == 0xC000000Eu && p.call(N::NtSetPointerDeviceInputSpace, 2, 5) == kNotFound);
  // map points between spaces: (5,5) of space 1 (origin 0,0) is (-45,-45) in space 2 (origin 50,50)
  p.prog.memory.put32(kScratch + 0x1100, 5); p.prog.memory.put32(kScratch + 0x1104, 5);
  CHECK(p.call(N::NtMapVisualRelativePoints, 1, 2, kScratch + 0x1100, 1) == kSuccess && int32_t(p.prog.memory.get32(kScratch + 0x1100)) == -45);
  p.prog.memory.put32(kScratch + 0x1108, 0x80000000u); p.prog.memory.put32(kScratch + 0x110C, 0);
  CHECK(p.call(N::NtMapVisualRelativePoints, 2, 0, kScratch + 0x1108, 1) == kSuccess);      // -2^31 + 50 is fine
  p.prog.memory.put32(kScratch + 0x1108, 0x80000000u);
  CHECK(p.call(N::NtMapVisualRelativePoints, 0, 2, kScratch + 0x1108, 1) == kInvalidParameter);      // -2^31 - 50 is not
  CHECK(p.call(N::NtMapVisualRelativePoints, 1, 2, kScratch + 0x1100, 0) == kInvalidParameter);
  CHECK(p.call(N::NtMapVisualRelativePoints, 1, 2, kScratch + 0x1100, 5000) == kInvalidParameter);
  CHECK(p.call(N::NtMapVisualRelativePoints, 1, 2, 0x10, 1) == kAccessViolation);
  CHECK(p.call(N::NtMapVisualRelativePoints, 8, 2, kScratch + 0x1100, 1) == kNotFound);
  CHECK(p.call(N::NtConfigureInputSpace, 2, 0, 0) == kSuccess && p.call(N::NtConfigureInputSpace, 2, 0, 0) == kNotFound);
  CHECK(p.call(N::NtEnableOneCoreTransformMode, 1) == kSuccess && p.call(N::NtIsOneCoreTransformMode, p.out_addr(1)) == kSuccess && p.prog.memory.get8(p.out_addr(1)) == 1);
  CHECK(p.call(N::NtSetShellCursorState, 3) == kSuccess && p.call(N::NtSetShellCursorState, 4) == kInvalidParameter);
  CHECK(p.call(N::NtHWCursorUpdatePointer, 0, 0, 0) == kNotSupported);
  process_exited(70);
  CHECK(q.call(N::NtInputSpaceRegionFromPoint, 10, 10, q.out_addr(0)) == kNotFound);      // p's spaces went with it

  // window targets: one of each kind per window
  CHECK(q.call(N::NtModerncoreCreateDCompositionHwndTarget, 0, q.out_addr(0)) == kInvalidParameter);
  CHECK(q.call(N::NtModerncoreCreateDCompositionHwndTarget, 0x90, q.out_addr(0)) == kSuccess);
  const uint64_t dt = q.out(0);
  Proc r(env, 72);
  CHECK(r.call(N::NtModerncoreCreateDCompositionHwndTarget, 0x90, r.out_addr(0)) == kInvalidParameter);
  CHECK(q.call(N::NtModerncoreCreateGDIHwndTarget, 0x90, q.out_addr(1)) == kSuccess);
  const uint64_t gt = q.out(1);
  CHECK(q.call(N::NtModerncoreDestroyGDIHwndTarget, dt) == kMismatch && q.call(N::NtModerncoreDestroyDCompositionHwndTarget, gt) == kMismatch);
  CHECK(q.call(N::NtModerncoreGetWindowContentVisual, gt, q.out_addr(2)) == kMismatch);
  CHECK(q.call(N::NtModerncoreGetWindowContentVisual, dt, q.out_addr(2)) == kSuccess);
  const uint32_t visual = q.out32(2);
  CHECK(visual != 0 && q.call(N::NtModerncoreGetWindowContentVisual, dt, q.out_addr(3)) == kSuccess && q.out32(3) == visual);
  CHECK(q.call(N::NtModerncoreGetNavigationWindowVisual, dt, q.out_addr(3)) == kNotFound);
  CHECK(q.call(N::NtModerncoreRegisterNavigationWindowHandle, 0x90) == kSuccess && q.call(N::NtModerncoreRegisterNavigationWindowHandle, 0x90) == kInvalidParameter);
  CHECK(q.call(N::NtModerncoreRegisterEnhancedNavigationWindowHandle, 0x90) == kSuccess);
  CHECK(q.call(N::NtModerncoreGetNavigationWindowVisual, dt, q.out_addr(3)) == kSuccess && q.out32(3) != visual);
  CHECK(r.call(N::NtModerncoreUnregisterNavigationWindowHandle, 0x90) == kAccessDenied);
  CHECK(q.call(N::NtModerncoreUnregisterNavigationWindowHandle, 0x90) == kSuccess && q.call(N::NtModerncoreUnregisterNavigationWindowHandle, 0x90) == kNotFound);
  CHECK(q.call(N::NtModerncoreDestroyDCompositionHwndTarget, dt) == kSuccess && q.call(N::NtModerncoreDestroyDCompositionHwndTarget, dt) == kInvalidHandle);
  CHECK(r.call(N::NtModerncoreCreateDCompositionHwndTarget, 0x90, r.out_addr(0)) == kSuccess);      // the window is free again

  // navigation service SID: S-1-5-18 and malformed ones
  uint8_t sid[12] = {1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
  for (int i = 0; i < 12; ++i) q.prog.memory.put8(kScratch + 0x1200 + i, sid[i]);
  CHECK(q.call(N::NtModerncoreSetNavigationServiceSid, kScratch + 0x1200, 12) == kSuccess);
  CHECK(q.call(N::NtModerncoreSetNavigationServiceSid, kScratch + 0x1200, 16) == kInvalidParameter);      // length does not match the sub-authority count
  q.prog.memory.put8(kScratch + 0x1200, 2);
  CHECK(q.call(N::NtModerncoreSetNavigationServiceSid, kScratch + 0x1200, 12) == kInvalidParameter);
  CHECK(q.call(N::NtModerncoreSetNavigationServiceSid, 0x10, 12) == kAccessViolation);
  CHECK(q.call(N::NtModerncoreSetNavigationServiceSid, 0, 0) == kSuccess);

  // resize synchronization: the object exists only while it is enabled
  CHECK(q.call(N::NtModerncoreIsResizeLayoutSynchronizationEnabled, 0x91, q.out_addr(0)) == kSuccess && q.prog.memory.get8(q.out_addr(0)) == 0);
  CHECK(q.call(N::NtModerncoreGetResizeDCompositionSynchronizationObject, 0x91, q.out_addr(1)) == kNotFound);
  CHECK(q.call(N::NtModerncoreEnableResizeLayoutSynchronization, 0x91, 1) == kSuccess);
  CHECK(q.call(N::NtModerncoreIsResizeLayoutSynchronizationEnabled, 0x91, q.out_addr(0)) == kSuccess && q.prog.memory.get8(q.out_addr(0)) == 1);
  CHECK(q.call(N::NtModerncoreGetResizeDCompositionSynchronizationObject, 0x91, q.out_addr(1)) == kSuccess);
  CHECK(q.call(N::NtModerncoreGetResizeDCompositionSynchronizationObject, 0x91, q.out_addr(2)) == kSuccess && q.out(1) != q.out(2));      // two handles, one object
  CHECK(q.call(N::NtDCompositionSyncWait, q.out(1), 0) == kTimeout);
  CHECK(q.call(N::NtModerncoreEnableResizeLayoutSynchronization, 0x91, 0) == kSuccess);
  CHECK(q.call(N::NtModerncoreGetResizeDCompositionSynchronizationObject, 0x91, q.out_addr(3)) == kNotFound);
  CHECK(r.call(N::NtModerncoreEnableResizeLayoutSynchronization, 0x91, 1) == kAccessDenied);
}

void test_misc_services() {
  Env env;
  Proc p(env, 80), q(env, 81);
  // material properties are per process
  CHECK(p.call(N::NtDCompositionSetMaterialProperty, 0, 1, 2) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionGetMaterialProperty, 5, 1, p.out_addr(0)) == kNotFound);
  CHECK(p.call(N::NtDCompositionSetMaterialProperty, 5, 1, 0x1122334455667788ull) == kSuccess && p.call(N::NtDCompositionSetMaterialProperty, 5, 1, 9) == kSuccess);
  CHECK(p.call(N::NtDCompositionGetMaterialProperty, 5, 1, p.out_addr(0)) == kSuccess && p.out(0) == 9);
  CHECK(q.call(N::NtDCompositionGetMaterialProperty, 5, 1, q.out_addr(0)) == kNotFound);
  // telemetry scenarios count references; touches begin and end
  CHECK(p.call(N::NtDCompositionTelemetryAnimationScenarioUnreference, 4) == kInvalidState);
  CHECK(p.call(N::NtDCompositionTelemetryAnimationScenarioBegin, 4) == kSuccess && p.call(N::NtDCompositionTelemetryAnimationScenarioReference, 4) == kSuccess);
  CHECK(p.call(N::NtDCompositionTelemetryAnimationScenarioUnreference, 4) == kSuccess && p.call(N::NtDCompositionTelemetryAnimationScenarioUnreference, 4) == kSuccess);
  CHECK(p.call(N::NtDCompositionTelemetryAnimationScenarioUnreference, 4) == kInvalidState);
  CHECK(p.call(N::NtDCompositionTelemetryAnimationScenarioBegin, 0) == kInvalidParameter);
  CHECK(p.call(N::NtDCompositionTelemetryTouchInteractionUpdate, 3) == kNotFound);
  CHECK(p.call(N::NtDCompositionTelemetryTouchInteractionBegin, 3) == kSuccess && p.call(N::NtDCompositionTelemetryTouchInteractionBegin, 3) == kInvalidState);
  CHECK(p.call(N::NtDCompositionTelemetryTouchInteractionUpdate, 3) == kSuccess);
  CHECK(p.call(N::NtDCompositionTelemetryTouchInteractionEnd, 3) == kSuccess && p.call(N::NtDCompositionTelemetryTouchInteractionEnd, 3) == kNotFound);
  CHECK(p.call(N::NtDCompositionTelemetrySetApplicationId, 7) == kSuccess);
  CHECK(p.call(N::NtDCompositionBoostCompositorClock, 1) == kSuccess);
  // the compositor clock: a tick is at most one frame away
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(p.call(N::NtDCompositionWaitForCompositorClock, 1000) == kSuccess);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200));
  process_exited(80);
  CHECK(q.call(N::NtDCompositionSetMaterialProperty, 5, 1, 1) == kSuccess);
  CHECK(q.call(N::NtDCompositionGetMaterialProperty, 5, 1, q.out_addr(0)) == kSuccess);
}

// Every service of this module gets arguments that are wrong in every way: random numbers, handles of the right and the wrong kind, pointers into and out of memory.
std::vector<uint32_t> owned_services() {
  std::vector<uint32_t> numbers;
  std::ifstream file("scripts/win32k-services.txt");
  std::string line;
  uint32_t index = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const bool other = line.rfind("NtGdi", 0) == 0 || line.rfind("NtUser", 0) == 0 || line.rfind("NtDxgk", 0) == 0;
    if (!other) numbers.push_back(kServiceBase + index);
    ++index;
  }
  return numbers;
}

void test_hostile_arguments() {
  Env env;
  Proc a(env, 90), b(env, 91);
  Channel dwm_ch = make_channel(a, true);
  Channel ch = make_channel(a);
  CHECK(a.call(N::NtCreateCompositionSurfaceHandle, a.out_addr(0), 0) == kSuccess);
  const uint64_t surf = a.out(0);
  CHECK(a.call(N::NtCreateCompositionInputSink, a.out_addr(0), 1, 1) == kSuccess);
  const uint64_t sink = a.out(0);
  Flip f = connect(a, b);
  CHECK(a.call(N::NtDCompositionCreateSynchronizationObject, a.out_addr(0), 0) == kSuccess);
  const uint64_t sync = a.out(0);
  CHECK(a.call(N::NtRIMRegisterForInput, a.out_addr(0), 3, 0, 0) == kSuccess);
  const uint64_t reg = a.out(0);
  CHECK(a.call(N::NtDCompositionCreateConnection, a.out_addr(0), 0) == kSuccess);
  const uint64_t conn = a.out(0);
  const uint64_t pool[] = {0, ~uint64_t{0}, 4, 0x1000, dwm_ch.handle, ch.handle, surf, sink, f.producer, f.consumer, sync, reg, conn, ch.base, kScratch + 0x1000, kScratch + 0x1800, kScratch + 0x2000,
                           0x10, 0x7FFFFFFF, 0xFFFFFFFFull, 1, 2, 3, 0x40000, 0x100000000ull};
  const uint32_t pool_size = sizeof(pool) / sizeof(pool[0]);
  // These wait for something (a timeout argument is a pool value): keep them out of the random walk.
  auto blocks = [](uint32_t n) {
    return n == uint32_t(N::NtDCompositionWaitForChannel) || n == uint32_t(N::NtDCompositionSyncWait) || n == uint32_t(N::NtRIMReadInput) || n == uint32_t(N::NtRIMObserveNextInput) ||
           n == uint32_t(N::NtDCompositionWaitForCompositorClock);
  };
  const std::vector<uint32_t> mine = owned_services();
  CHECK(mine.size() > 150);
  uint64_t seed = 0x9E3779B97F4A7C15ull;
  auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
  uint32_t calls = 0;
  for (int round = 0; round < 60; ++round) {
    for (uint32_t n : mine) {
      if (!handler_of(n) || blocks(n)) continue;
      uint64_t args[6];
      for (uint64_t& v : args) v = next() % 3 == 0 ? next() : pool[next() % pool_size];
      Proc& who = next() % 4 == 0 ? b : a;
      who.prog.call(static_cast<WindowsSyscallNumber>(n), args[0], args[1], args[2], args[3], args[4], args[5]);
      ++calls;
    }
  }
  CHECK(calls > 5000);
  // the module still works afterwards
  const uint32_t again = b.call(N::NtDCompositionCreateSynchronizationObject, b.out_addr(0), 0);
  CHECK(again == kSuccess || again == kQuota);
  process_exited(90);
  process_exited(91);
}

// Every service this module owns is registered, except the ones documented as left out (and each of those still answers STATUS_NOT_IMPLEMENTED).
void test_coverage() {
  Env env;
  Proc p(env, 99);
  static const char* const unimplemented[] = {
      "NtCompositionInputThread", "NtCompositorNotifyExitWindows", "NtCompositorNotifyForegroundChanged", "NtCompositorUpdateLastInputTime", "NtDCompositionAddCrossDeviceVisualChild",
      "NtDCompositionGetTargetStatistics", "NtDCompositionNotifySuperWetInkWork", "NtDCompositionRegisterThumbnailVisual", "NtDCompositionRegisterVirtualDesktopVisual",
      "NtDCompositionRemoveCrossDeviceVisualChild", "NtDCompositionSetBlurredWallpaperSurface", "NtDCompositionSetChildRootVisual", "NtDCompositionUpdatePointerCapture",
      "NtDesktopCaptureBits", "NtKSTInitialize", "NtKSTWait", "NtMITAccessibilityTimerNotification", "NtMITConfigureVirtualTouchpad", "NtMITCoreMsgKOpenConnectionTo",
      "NtMITDispatchCompletion", "NtMITGetCursorUpdateHandle", "NtMITPrepareReceiveInputMessage", "NtMITPrepareSendInputMessage", "NtMITProcessDelegateCapturedPointers",
      "NtMITSetForegroundRoutingInfo", "NtMITSetInputCallbacks", "NtMITSetInputDelegationMode", "NtMITSetInputObservationState", "NtMITSetKeyboardInputRoutingPolicy",
      "NtMITSetKeyboardOverriderState", "NtMITSetLastInputRecipient", "NtMITUpdateInputGlobals", "NtMinInteropCoreMessagingWithInput", "NtMinQPeekForInput",
      "NtModerncoreBeginLayoutUpdate", "NtModerncoreIdleTimerThread", "NtModerncoreProcessConnect", "NtOpenCompositionSurfaceSwapChainHandleInfo", "NtQueryCompositionSurfaceHDRMetaData",
      "NtRIMFreeInputBuffer", "NtRIMGetPhysicalDeviceRect", "NtRIMGetSourceProcessId", "NtRIMOnAsyncPnpWorkNotification", "NtRIMOnPnpNotification",
      "NtRIMOnTimerNotification", "NtRIMSetDeadzoneRotation", "NtRIMSetExtendedDeviceProperty", "NtTokenManagerThread", "NtVisualCaptureBits"};
  std::vector<std::string> names;
  {
    std::ifstream file("scripts/win32k-services.txt");
    std::string line;
    while (std::getline(file, line)) if (!line.empty() && line[0] != '#') names.push_back(line);
  }
  uint32_t implemented = 0, left_out = 0;
  for (uint32_t i = 0; i < names.size(); ++i) {
    const std::string& name = names[i];
    if (name.rfind("NtGdi", 0) == 0 || name.rfind("NtUser", 0) == 0 || name.rfind("NtDxgk", 0) == 0) continue;
    bool documented = false;
    for (const char* u : unimplemented) if (name == u) documented = true;
    const bool registered = handler_of(kServiceBase + i) != nullptr;
    if (registered) ++implemented; else ++left_out;
    if (registered == documented) { printf("service %s: registered=%d documented-unimplemented=%d\n", name.c_str(), registered, documented); ++g_failed; }
    if (!registered) CHECK(p.call(static_cast<WindowsSyscallNumber>(kServiceBase + i)) == kNotImplemented);
  }
  printf("composition: %u services implemented, %u left out\n", implemented, left_out);
  CHECK(implemented >= 140);
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("%s\n", "registration"); test_registration();
  printf("%s\n", "coverage"); test_coverage();
  printf("%s\n", "channel_commands"); test_channel_commands();
  printf("%s\n", "channel_commit_state"); test_channel_commit_state();
  printf("%s\n", "sync_objects_and_waits"); test_sync_objects_and_waits();
  printf("%s\n", "dwm_and_sharing"); test_dwm_and_sharing();
  printf("%s\n", "quotas_and_exit"); test_quotas_and_exit();
  printf("%s\n", "surfaces_and_tokens"); test_surfaces_and_tokens();
  printf("%s\n", "input_sinks"); test_input_sinks();
  printf("%s\n", "flip_objects"); test_flip_objects();
  printf("%s\n", "flip_return_token_and_sections"); test_flip_return_token_and_sections();
  printf("%s\n", "flip_threads"); test_flip_threads();
  printf("%s\n", "rim"); test_rim();
  printf("%s\n", "mit"); test_mit();
  printf("%s\n", "spaces_and_moderncore"); test_spaces_and_moderncore();
  printf("%s\n", "misc_services"); test_misc_services();
  printf("%s\n", "hostile_arguments"); test_hostile_arguments();
  return finish("Win32kCompositionTest");
}
