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
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Process/AddressSpace.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace process {

// Synchronization objects shared by every process of the system: events, mutexes (mutants) and semaphores, with names, so that two programs that open the same name get the
// same object. Handles in a process's table refer to an object by its id; the object lives while a handle (or a name lookup in progress) refers to it.

// The one lock and wake-up all the waiting on these objects (and on threads) is built on: whoever runs the system (the kernel) supplies it. A waiter takes the lock, looks, and
// waits; whoever changes something takes the lock, changes it, and wakes everybody (the waiters look again).
class ISystemSync {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;
  // With the lock held: releases it and sleeps until notify_all() or the timeout (nanoseconds, ~0 = none); the lock is held again on return. False on timeout.
  virtual bool wait(uint64_t timeout_ns) = 0;
  virtual void notify_all() = 0;
  // A clock that only goes forward, in nanoseconds (for the timers).
  virtual uint64_t now_ns() { return 0; }

 protected:
  ISystemSync() = default;
  ~ISystemSync() = default;
};

enum class ObjectKind : uint8_t { None, Event, Mutex, Semaphore, Process, Pipe, Timer, Section, Job, ChangeNotification, Mailslot, DebugObject };

class ObjectManager {
 public:
  static constexpr uint32_t kCapacity = 1024;
  static constexpr uint32_t kNameBytes = 128;

  explicit ObjectManager(ISystemSync& sync) : sync_(sync) {}
  ~ObjectManager();
  ObjectManager(const ObjectManager&) = delete;
  ObjectManager& operator=(const ObjectManager&) = delete;

  ISystemSync& sync() { return sync_; }
  // Wakes every waiter (takes the lock itself): to be called after anything a waiter looks at changed outside the manager (a thread ended, a process was told to stop).
  void notify();

  // --- making and finding objects (these take the lock themselves). `status` is an NTSTATUS: 0, 0x40000000 (the name existed: the existing object is returned), or an error.
  // `owned` / `signaled`: a mutex created owned by `owner`; an event created signaled. A semaphore starts at `initial` and can reach `maximum`.
  struct Created {
    uint32_t id = 0;           // 0 on failure
    uint32_t status = 0;
    uint32_t end_code = 0;     // pipes: which end the handle is (see below)
  };
  Created create(ObjectKind kind, StringView name, bool manual_reset, bool initial_state, int32_t initial, int32_t maximum, uint64_t owner);
  Created open(ObjectKind kind, StringView name);
  // A process (waited for by its parent): signalled when it has ended. `pid` finds it again when the process ends.
  Created create_process(uint64_t pid);

  // --- pipes. A pipe is one object with two ends: end 0 is the server's (CreateNamedPipe, or the write side of CreatePipe), end 1 the client's (CreateFile on the name, or the
  // read side of CreatePipe). Each end is counted separately: the other side sees the end of the data / a broken pipe when no handle to it is left. The buffers come from the
  // allocator given to set_allocator().
  void set_allocator(allocators::IAllocator* allocator) { allocator_ = allocator; }
  static constexpr uint32_t kPipeInbound = 1, kPipeOutbound = 2, kPipeDuplex = 3;      // the server's direction (PIPE_ACCESS_*); anonymous pipes are outbound
  // `name` empty: an anonymous pipe, already connected. Otherwise a new instance of the pipe of that name (up to `max_instances` of them, 0: no limit), not connected.
  Created create_pipe(StringView name, uint32_t out_size, uint32_t in_size, uint32_t max_instances, uint32_t access, bool message_type = false, uint64_t pid = 0);
  // A client connects to an instance of the named pipe that is not connected yet (status 0xC0000034 if no pipe has the name, 0xC00000AE if all are connected).
  // `want_read` / `want_write`: what the client asks for; an instance whose direction does not allow it is refused (0xC0000022) and stays unconnected.
  Created connect_pipe(StringView name, bool want_read, bool want_write, uint64_t pid = 0);
  // An end is named by an `end_code`: bit 0 says which end, the rest (for the client end) which connection it belongs to, so that a handle left over from a connection the server
  // ended cannot be mistaken for the next client.
  void add_pipe_end(uint32_t id, uint32_t end_code);
  void release_pipe_end(uint32_t id, uint32_t end_code);
  enum class PipeResult { Ok, WouldBlock, Broken, Closing, NotConnected, Listening, MoreData, NoResources, InvalidState, Busy };      // MoreData: a message was read in part (the data moved is valid)      // (Listening: the server end has not had a client yet; NotConnected: the server disconnected it)
  // The lock must be held (sync().lock()) for the _locked calls. Read/write move what fits (at least one byte) or say why not.
  PipeResult pipe_read_locked(uint32_t id, uint32_t end_code, uint8_t* out, uint32_t capacity, uint32_t* got, bool peek = false, uint32_t* available = nullptr, bool message_mode = false, uint32_t* message_length = nullptr);
  // (A message-type pipe keeps what each write wrote as a message. message_mode reads one message at a time; message_length gets the length of the first one, 0 for a byte pipe.)
  // Writes what fits at once as a message of its own (the part that does not fit stays with the caller): for a pipe that does not make the caller wait.
  PipeResult pipe_write_locked(uint32_t id, uint32_t end_code, const uint8_t* data, uint32_t count, uint32_t* done);
  // The server end listens: Ok if a client is connected (already or now), WouldBlock otherwise.
  PipeResult pipe_listen_locked(uint32_t id);
  // How much this end wrote that the other end has not read yet (0 when the other end is gone).
  uint32_t pipe_room_locked(uint32_t id, uint32_t end_code);      // how much more this end can write before the pipe is full
  uint32_t pipe_pending_locked(uint32_t id, uint32_t end_code);
  bool pipe_disconnect(uint32_t id);      // false if nothing was connected      // the server ends the connection (the instance can listen again)
  // --- operations. A call that has to wait is an operation of the pipe, with a tag: it is served in the order it was made (reads), a write that does not fit is queued whole,
  // as the other end sees it at once (peeking, ReadDataAvailable), and is complete when enough was read to make room. An operation is cancelled by its tag, which takes back
  // the bytes of a write that have not been read. `async` marks an operation that another thread of its owner waits for with pipe_op_poll_locked: whoever completes it
  // waits until that thread has acted on it (pipe_op_release), so that its result is there when the call that completed it returns.
  enum class PipeOpKind : uint8_t { None, Read, Write, Flush, Listen };
  enum class PipeOpPoll { Missing, Pending, Done };
  struct PipeOpResult {
    uint32_t status = 0, transferred = 0;
    ByteView data;      // a read: what it took, to be given back with pipe_op_free_data
  };
  static constexpr uint32_t kMaxPipeWrite = 8u << 20;      // the longest write that is queued as one (longer ones are cut)
  // Read: Ok/error when it can be answered now (nothing was queued), WouldBlock when it was queued as `tag`.
  PipeResult pipe_begin_read_locked(uint32_t id, uint32_t end_code, uint32_t length, bool message_mode, uint64_t owner, bool async, uint32_t* tag);
  // Write: Ok when all of it went at once (`done` bytes, no operation), WouldBlock when it is queued as `tag`, or why not.
  PipeResult pipe_begin_write_locked(uint32_t id, uint32_t end_code, ConstByteView data, uint64_t owner, bool async, uint32_t* tag, uint32_t* done);
  // TransactNamedPipe: `data` is written as a message that does not make it wait (it is queued like any write, but nobody waits for it) and then a read of at most
  // `read_length` is queued as `tag` (WouldBlock). InvalidState if the pipe is not a message pipe read as messages, Busy if there is something to read already.
  PipeResult pipe_begin_transact_locked(uint32_t id, uint32_t end_code, ConstByteView data, uint32_t read_length, bool message_mode, uint64_t owner, bool async, uint32_t* tag);
  PipeResult pipe_begin_listen_locked(uint32_t id, uint64_t owner, bool async, uint32_t* tag);
  // Waits until what this end wrote has been read: Ok at once, WouldBlock when queued as `tag`.
  PipeResult pipe_begin_flush_locked(uint32_t id, uint32_t end_code, uint64_t owner, uint32_t* tag);
  PipeOpPoll pipe_op_poll_locked(uint32_t id, uint32_t end_code, uint32_t tag, PipeOpResult* result);      // Done: the result is moved out, the operation stays until released
  PipeResult pipe_op_cancel_locked(uint32_t id, uint32_t end_code, uint32_t tag, uint32_t status);      // Ok if it was pending (now finished with `status`), else Broken
  void pipe_op_release(uint32_t id, uint32_t tag);      // takes the operation away (what the thread that waited for it has done)
  void pipe_op_free_data(ByteView& data);
  bool pipe_delivered_locked(uint32_t id);      // no operation is finished that its thread has not yet acted on
  void pipe_forgive_locked(uint32_t id);      // gives up waiting for them
  void pipe_owner_gone(uint64_t pid);      // the process ended: what it waits for is cancelled
  // A thread that waits in the kernel for a call on a pipe end (one that is not an operation of the pipe): while it does, the file object is not signalled, and
  // CancelSynchronousIo can end the call. begin returns a slot (1 and up), 0 if there is none.
  uint32_t pipe_sync_begin(uint32_t id, uint32_t end_code, uint64_t thread);
  bool pipe_sync_cancelled_locked(uint32_t slot);
  void pipe_sync_end(uint32_t slot);
  bool pipe_cancel_synchronous(uint64_t thread);      // false if the thread waits for no call
  // A pipe end tied to an I/O completion port (a semaphore object that the library's port is counted by): when a process ends while a call of it waits on the end, the packet for
  // that call cannot be queued by the process, so the kernel keeps it for the port (port_take_foreign) and releases the semaphore once.
  struct PortPacket { uint32_t bytes = 0, status = 0; uint64_t key = 0, overlapped = 0; };
  void pipe_set_port(uint32_t id, uint32_t end_code, uint32_t semaphore_object, uint64_t key);
  bool port_take_foreign(uint32_t semaphore_object, PortPacket* packet);
  bool pipe_client_gone_locked(uint32_t id, uint32_t end_code);      // a client end whose server disconnected it
  // The event of the file object of an end: set when it is made, reset when an operation that waits is started on it, set when one that was started without an event of its
  // own is over (what a wait on the handle looks at).
  bool pipe_file_signaled_locked(uint32_t id, uint32_t end_code);
  void pipe_set_file_signaled(uint32_t id, uint32_t end_code, bool signaled);
  bool pipe_name_exists_locked(StringView name);      // some instance of the pipe exists (listening or not)
  uint32_t object_name(uint32_t id, char* out, uint32_t capacity);      // the name a pipe (or other named object) was made with; its length, 0 if it has none or is not that big
  bool pipe_name_available_locked(StringView name);      // some instance of the named pipe is not connected
  struct PipeInfo {
    bool anonymous = false, connected = false, message_type = false;
    uint32_t access = 0, out_size = 0, in_size = 0, max_instances = 0, instances = 0;
  };
  bool pipe_info(uint32_t id, PipeInfo* out);
  // The processes of a connection: `which` 0 the client, 1 the server. Ok, or why the end has no answer (NotConnected: the server disconnected it, Listening: nobody is connected).
  PipeResult pipe_process(uint32_t id, uint32_t end_code, uint32_t which, uint64_t* pid);
  // A write on a pipe that does not make the caller wait (PIPE_NOWAIT): what the reads that wait take and what fits goes, the rest is not written; `done` says how much.
  PipeResult pipe_write_nowait_locked(uint32_t id, uint32_t end_code, ConstByteView data, uint32_t* done);
  void process_exited(uint64_t pid, uint32_t code);
  // A handle to the process object of `pid` (made if there is none and `create` says the process exists).
  Created open_process(uint64_t pid, bool create);
  bool query_process(uint32_t id, bool* exited, uint32_t* code, uint64_t* pid);
  void add_ref(uint32_t id);
  void release(uint32_t id);
  ObjectKind kind(uint32_t id);

  // --- operations on one object (they take the lock and wake the waiters). They return an NTSTATUS.
  uint32_t set_event(uint32_t id, int32_t* previous);
  uint32_t reset_event(uint32_t id, int32_t* previous);
  uint32_t pulse_event(uint32_t id, int32_t* previous);
  uint32_t release_mutex(uint32_t id, uint64_t owner, int32_t* previous_count);
  uint32_t release_semaphore(uint32_t id, int32_t count, int32_t* previous);
  // A thread (or all the threads of a process) ended: the mutexes it owns become abandoned and free.
  void abandon_thread(uint64_t thread);
  void abandon_process(uint64_t process);

  // --- sections: memory that processes map into their address spaces (CreateFileMapping). Named ones are shared by name. A section backed by a file reads it when made and
  // writes it back when asked to flush, when its last view goes away and when it is gone.
  struct SectionInfo {
    uint64_t size = 0;
    bool writable = false, file_backed = false;
    uint32_t page = 0;      // the PAGE_* protection it was made with
    uint32_t attributes = 0;      // what NtQuerySection reports: SEC_COMMIT / SEC_RESERVE / SEC_FILE / SEC_NOCACHE ...
    uint64_t reported_size = 0;   // an image section: the size NtQuerySection reports (0: the rounded size)
  };
  // `fs` / `host_path`: the file that backs it (nullptr: plain memory, zero at the start). A file is read into the section (its size or `size`, the bigger).
  Created create_section(StringView name, uint64_t size, uint32_t page, uint32_t attributes, filesystem::IFileSystem* fs, StringView host_path);
  // The memory of the section for AddressSpace::map_shared(); false if there is no such section.
  bool section_memory(uint32_t id, uint8_t** storage, uint64_t* size, SharedMemory** owner);
  bool section_info(uint32_t id, SectionInfo* out);
  void section_set_reported_size(uint32_t id, uint64_t size);
  // The lower-case name of the file behind the section (no folders, cut to 39 characters); false if it has no file.
  bool section_file_name(uint32_t id, char (&out)[40]);
  // SEC_RESERVE sections: which pages VirtualAlloc has committed. `owner` is what AddressSpace::map_shared() was given; `offset` counts from the start of the section.
  bool section_reserved(const SharedMemory* owner);
  uint64_t section_offset(const SharedMemory* owner, const uint8_t* pointer);
  void section_commit_pages(const SharedMemory* owner, uint64_t offset, uint64_t length);
  bool section_page_committed(const SharedMemory* owner, uint64_t offset);
  // Is there a section (a mapping handle or a view) of the file at this path in the OS file system? A file in that state cannot be truncated, deleted or renamed.
  // 0: none, 1: a data section, 2: an image section.
  uint32_t file_has_section(StringView host_path);
  // An image section gets a view: was there one before?
  bool section_note_image_view(uint32_t id);
  void flush_section(uint32_t id);
  void flush_section_at(uint32_t index);      // (by its index among the sections)
  // How many views and handles keep the section alive (for the mapping side: see SharedMemory).
  void section_count(uint32_t index, int32_t delta);

  // --- byte-range locks on files (LockFileEx): advisory, for the whole system; a file is named by its path in the OS file system, a lock belongs to an open file (`owner`)
  enum class LockResult { Granted, Conflict, Full };
  LockResult lock_range_locked(StringView path, const void* owner, uint64_t offset, uint64_t length, bool exclusive);      // (the lock must be held)
  bool unlock_range(StringView path, const void* owner, uint64_t offset, uint64_t length);
  void unlock_all(const void* owner);      // the open file is gone

  // --- sharing of files (the access an open file has and the access it lets others have): a new open fails with a sharing violation if the access it wants is not shared by an
  // open file of the same path, or if it shares less than the access an open file already has. `access` and `share` use ShareBits. Opens that want none of the three (attribute
  // queries) neither conflict nor count.
  static constexpr uint32_t kShareRead = 1, kShareWrite = 2, kShareDelete = 4;
  enum class ShareResult { Granted, Violation, Full };
  ShareResult share_acquire(StringView path, const void* owner, uint32_t access, uint32_t share);      // (owner null: only check)
  // Would an open wanting `access` with everything shared be allowed? (Deleting a file, renaming it.) Takes the lock itself.
  bool share_allows(StringView path, uint32_t access);
  void share_release(const void* owner);      // the open file is gone
  void share_retarget(const void* owner, StringView new_path);      // the open file was renamed: its share now belongs to the new name
  // The open files of `path` (at most `capacity` of them are written to `owners`); how many there are.
  uint32_t share_owners(StringView path, const void** owners, uint32_t capacity);

  // --- file times finer than the file systems keep them: the time of a file in 100 ns units since 1601 (Windows' FILETIME), kept by path for files that were written, created or had
  // their times set while the system runs. `which` says which of created (1), accessed (2) and written (4) a call sets; 0 in a field read back means no precise time is known.
  struct FileTimes { uint64_t created = 0, accessed = 0, written = 0; };
  void file_times_set(StringView path, uint32_t which, const FileTimes& times);
  bool file_times_get(StringView path, FileTimes* times);
  void file_times_move(StringView from, StringView to);      // the file was renamed
  void file_times_forget(StringView path);                   // the file is gone

  // --- symbolic links as the program that made them wrote them: the file systems keep the place a link points to, the program also wants the text it gave (a reparse point
  // has it as the "print name", and a relative target stays relative). Kept by the link's path for as long as the link is there.
  void link_note(StringView link_path, StringView target_text);
  uint32_t link_text(StringView link_path, char* out, uint32_t capacity);      // the target text, 0 if none is known
  void link_forget(StringView link_path);

  // --- the file a process was started from (its Win32 path), kept by process id while it runs and for a while after: what NtQueryInformationProcess answers about another process.
  void process_image_note(uint64_t pid, StringView path);
  uint32_t process_image_get(uint64_t pid, char* out, uint32_t capacity);      // the length of the path, 0 if none is known (or it does not fit)

  // --- job objects (kind Job: create() and open() make and find them). A job groups processes: its limits (active process count, kill on close, breakaway) are enforced and
  // its members can be ended together. A process belongs to at most one job; the processes it starts join the same job. Members are ended by a flag that they see themselves
  // at their next system call or timer tick (job_kill_for(), which takes no lock). All these take the lock themselves.
  static constexpr uint32_t kJobMaxMembers = 64, kJobMaxJobs = 32;
  static constexpr uint32_t kJobLimitActiveProcess = 0x8, kJobLimitAffinity = 0x10, kJobLimitPriorityClass = 0x20, kJobLimitBreakawayOk = 0x800, kJobLimitSilentBreakawayOk = 0x1000,
                            kJobLimitKillOnClose = 0x2000, kJobLimitAll = 0x3FFFu;
  struct JobInfo {
    uint64_t per_process_time = 0, per_job_time = 0, min_working_set = 0, max_working_set = 0, affinity = 0, process_memory_limit = 0, job_memory_limit = 0;
    uint32_t limit_flags = 0, active_limit = 0, priority_class = 0, scheduling_class = 0, ui_flags = 0;
    uint32_t total_processes = 0, active_processes = 0, terminated_processes = 0;
  };
  uint32_t job_assign(uint32_t job, uint64_t pid);                  // NTSTATUS: 0, access denied (already in a job), quota exceeded
  uint32_t job_of_process(uint64_t pid);                            // the job object's id, 0 if none
  // A process starts another: the child joins the parent's job unless it breaks away (`breakaway`: asked to; allowed only if the job says so). NTSTATUS.
  uint32_t job_inherit(uint64_t parent, uint64_t child, bool breakaway);
  void job_forget(uint64_t pid);                                    // the process is not a member any more (it ended, or it could not be started)
  uint32_t job_terminate(uint32_t job, uint32_t code);              // flags every member to end with `code`
  bool job_info(uint32_t job, JobInfo* out, uint64_t* pids, uint32_t capacity, uint32_t* pid_count);
  uint32_t job_set(uint32_t job, const JobInfo& in, bool extended);       // NTSTATUS
  bool job_kill_for(uint64_t pid, uint32_t* code);                  // lock-free: has this process been flagged to end?
  // A completion port (JobObjectAssociateCompletionPortInformation): the job keeps the JOB_OBJECT_MSG_* events (new process, exit, no process left) for whoever reads them
  // from the port. NTSTATUS: invalid parameter if a port is associated already (and `present`). Associating queues a new-process event for each process in the job.
  uint32_t job_set_port(uint32_t job, uint64_t key, bool present);
  bool job_take_event(uint32_t job, uint32_t* message, uint64_t* pid, uint64_t* key);      // the oldest event; false if there is none

  // --- change notifications (kind ChangeNotification; FindFirstChangeNotification/ReadDirectoryChangesW): a handle that becomes signalled, exactly like a manual-reset
  // event, when something is created, deleted, renamed or linked directly under the directory it watches (or anywhere under it, if `watch_subtree`). Reset it
  // (FindNextChangeNotification) with reset_event(), which this kind shares with Event. `filter` is kept but not matched against: every one of those changes signals
  // every watch of its directory, regardless of which FILE_NOTIFY_CHANGE_* bits it asked for (a simplification, like this OS's permissive registry access checks).
  // A watch also queues what changed (action + name), FILE_NOTIFY_INFORMATION-style, for ReadDirectoryChangesW - a plain FindFirstChangeNotification watch just never
  // drains that queue, relying only on `signaled`, so one mechanism serves both APIs exactly as it does on real Windows (both go through the same underlying service).
  static constexpr uint32_t kMaxWatches = 64, kWatchPathBytes = 512, kMaxWatchRecords = 32, kWatchNameBytes = 128;
  Created create_change_notification(StringView path, bool watch_subtree, uint32_t filter);
  // Call after any successful change to a directory's contents (a file or subdirectory created, deleted, renamed or linked): `affected_directory` is that directory's
  // own virtual path (the parent of whatever changed, not the changed entry itself); `action` is a FILE_ACTION_* constant, `entry_name` the changed entry's own
  // (not full-path) name. Signals every watch whose directory this is under (or equal to), and queues a record for it if there is room (a full queue just drops the
  // record - `signaled` still fires, so a FindFirstChangeNotification-only watcher is unaffected, and ReadDirectoryChangesW already has to tolerate missed changes
  // under real Windows too, documented as "if the buffer overflows").
  void notify_path_changed(StringView affected_directory, uint32_t action, StringView entry_name);
  // Takes every record a ReadDirectoryChangesW-backed watch has queued (up to `capacity` of them) as parallel arrays; the watch's queue is cleared. False if `id`
  // is not a change-notification handle. Returns how many were taken in *count (0 with no error: nothing was queued).
  struct WatchRecord { uint32_t action = 0; char name[kWatchNameBytes] = {}; uint32_t name_size = 0; };
  bool take_watch_records(uint32_t id, WatchRecord* out, uint32_t capacity, uint32_t* count);
  // The same for a caller that holds the system lock already (the attempt of wait_until); the mutex is not recursive.
  bool take_watch_records_locked(uint32_t id, WatchRecord* out, uint32_t capacity, uint32_t* count);

  // --- mailslots (kind Mailslot; CreateMailslot/CreateFile(\\.\mailslot\name)): one reader (the handle CreateMailslot returns) and any number of writers, all on this
  // machine - there is no network broadcast here, unlike a real mailslot's domain-wide one. A writer's message is queued whole (never split across reads) until the
  // reader takes it; closing the reader destroys the mailslot (a writer with a handle still open then sees it as gone). `read_timeout_100ns`: ~0 (MAILSLOT_WAIT_FOREVER,
  // a null NT timeout) waits forever, 0 does not wait at all, matching every other timeout in this codebase (not real mailslots' own millisecond convention - kernel32
  // converts GetMailslotInfo/SetMailslotInfo/CreateMailslot's millisecond values at that layer, the way it already converts disposition/options for CreateFile).
  static constexpr uint32_t kMaxMailslots = 64, kMailslotBufferBytes = 16384, kMaxMailslotMessages = 256;
  Created create_mailslot(StringView name, uint32_t max_message_size, uint64_t read_timeout_100ns);
  // A writer's handle (CreateFile on the name): finds the reader's mailslot by name. Status 0xC0000034 (STATUS_OBJECT_NAME_NOT_FOUND) if no mailslot has that name.
  Created open_mailslot_writer(StringView name);
  enum class MailslotResult { Ok, NoMessage, TooSmall, TooBig, Full, Gone };
  // Queues one whole message (a no-op boundary, never coalesced with another write's bytes). TooBig: longer than `max_message_size` (0: no cap - just the buffer's own
  // capacity). Full: the buffer or the pending-message count is full right now (there is no "wait for room": a message is queued at once or not at all).
  MailslotResult mailslot_write_locked(uint32_t id, ConstByteView data);
  // Takes the oldest pending message. TooSmall (nothing taken; `*message_size` is how big it actually is, so the caller can retry with a bigger buffer, matching a
  // real mailslot's ERROR_INSUFFICIENT_BUFFER - the message is not lost) if `capacity` is less than the message's size; NoMessage if the queue is empty right now.
  MailslotResult mailslot_read_locked(uint32_t id, uint8_t* out, uint32_t capacity, uint32_t* got, uint32_t* message_size);
  // GetMailslotInfo/SetMailslotInfo (NtQueryInformationFile/NtSetInformationFile classes 26/27).
  uint32_t mailslot_message_count_locked(uint32_t id);      // (the system lock held) how many messages wait; 0 for a gone mailslot
  bool mailslot_info(uint32_t id, uint32_t* max_message_size, uint64_t* read_timeout_100ns, uint32_t* next_message_size, uint32_t* message_count);
  bool mailslot_set_timeout(uint32_t id, uint64_t read_timeout_100ns);
  // The reader's own handle closing (not a writer's): destroys the mailslot at once, same as the real one. A no-op if `id` is a writer or already gone.
  void mailslot_close_reader(uint32_t id);

  // --- timers (kind Timer: create() and open() make and find them; a notification timer stays signalled once it fired, a synchronization timer wakes one waiter)
  struct TimerApc { uint64_t routine = 0, context = 0, time = 0; };
  // Arms the timer: due at `due_ns` on the clock of sync().now_ns() (periodic every `period_ns`, 0: once). With a `routine`, `thread` gets it as an APC each time it fires.
  uint32_t set_timer(uint32_t id, uint64_t due_ns, uint64_t period_ns, uint64_t thread, uint64_t routine, uint64_t context, bool* was_signaled);
  uint32_t cancel_timer(uint32_t id, bool* was_signaled);
  uint32_t query_timer(uint32_t id, uint64_t* remaining_ns, bool* signaled);
  // The timers that fired for `thread` since it asked (the lock must be held). Their routines are returned in `out`; the number in *count.
  void take_timer_apcs_locked(uint64_t thread, TimerApc* out, uint32_t capacity, uint32_t* count);
  // When the next timer of `thread` with a routine is due, or the given timer (~0: none; the lock must be held).
  uint64_t thread_timer_deadline_locked(uint64_t thread);
  uint64_t timer_deadline_locked(uint32_t id);

  // --- waiting (the caller holds the lock: sync().lock())
  bool ready_locked(uint32_t id, uint64_t waiter);
  // Takes what a finished wait takes (the mutex, a unit of the semaphore, the auto-reset event's signal). *abandoned: the mutex was abandoned by its last owner.
  void acquire_locked(uint32_t id, uint64_t waiter, bool* abandoned);
  void waiting_locked(uint32_t id, int32_t delta);      // a waiter starts (+1) or stops (-1) waiting for the object

 private:
  struct Object {
    ObjectKind kind = ObjectKind::None;
    uint32_t references = 0;
    char name[kNameBytes] = {};
    uint32_t name_size = 0;
    bool manual_reset = false, signaled = false, abandoned = false;
    bool dying = false;                    // no handle is left, but a thread waits for it or a mutex is owned: it goes with the last of those
    int32_t count = 0, maximum = 0;        // semaphore
    uint64_t owner = 0;                    // mutex: the thread that owns it, 0 if none
    int32_t recursion = 0;
    int32_t waiters = 0, pulses = 0;       // PulseEvent lets through the threads that are waiting now
    int32_t pipe = -1;                     // pipe: its index in pipes_
    int32_t section = -1;                  // section: its index in sections_
    int32_t job = -1;                      // job: its index in jobs_
    int32_t watch = -1;                    // change notification: its index in watches_
    int32_t mailslot = -1;                 // mailslot: its index in mailslots_
    uint64_t due_ns = 0, period_ns = 0;    // timer: when it fires next, how often
    uint64_t apc_thread = 0, apc_routine = 0, apc_context = 0;      // timer: the APC it queues
    bool armed = false, apc_pending = false;
    uint64_t fired_at = 0;
  };
  Object* at(uint32_t id) { return id >= 1 && id <= kCapacity && objects_[id - 1].kind != ObjectKind::None ? &objects_[id - 1] : nullptr; }
  Object* find_locked(StringView name);
  void fire_timer_locked(Object& o, uint64_t now);
  void free_object_locked(Object& o);
  void finish_dying_locked(Object& o);

  struct Pipe;
  struct Section;
  class SectionRef;
  void write_section_locked(Section& s);
  Section* section_of_owner(const SharedMemory* owner);      // (locked)
  void free_section_locked(Object& o);
  struct JobMember { uint64_t pid = 0; uint32_t kill = 0, code = 0, gone = 0; };      // (gone: the process has ended; it is still said to be in the job)
  struct JobEvent { uint32_t message = 0; uint64_t pid = 0; };
  struct Job {
    bool used = false;
    uint32_t object = 0;
    JobInfo info;
    JobMember members[kJobMaxMembers];
    bool port_set = false;      // a completion port is associated: the events below wait to be taken
    uint64_t port_key = 0;
    static constexpr uint32_t kMaxEvents = 32;
    JobEvent events[kMaxEvents];
    uint32_t event_head = 0, event_count = 0;
  };
  void job_event_locked(Job& j, uint32_t message, uint64_t pid);
  void free_job_locked(Object& o);
  void job_forget_locked(uint64_t pid);
  Job* job_of(uint32_t id);      // (locked)
  void job_flag_all_locked(Job& j, uint32_t code);
  Pipe* pipe_of(uint32_t id);      // (locked)
  void free_pipe_locked(Object& o);
  struct PipeOp;
  bool pipe_grow_locked(Pipe& p, uint32_t q, uint64_t needed);
  bool pipe_append_locked(Pipe& p, uint32_t q, ConstByteView data, uint32_t op);
  uint32_t pipe_take_locked(Pipe& p, uint32_t q, ByteView out, bool message_mode, bool peek, bool* more, uint32_t* front_length);
  void pipe_consume_locked(Pipe& p, uint32_t q, uint32_t n);
  void pipe_pop_record_locked(Pipe& p, uint32_t q);
  void pipe_settle_locked(Pipe& p, uint32_t q);
  void pipe_finish_locked(Pipe& p, PipeOp& op, uint32_t status, uint32_t transferred);
  void pipe_cancel_op_locked(Pipe& p, PipeOp& op, uint32_t status);
  void pipe_fail_ops_locked(Pipe& p, int end, uint32_t status);      // every pending operation of an end (-1: of both)
  void pipe_clear_queue_locked(Pipe& p, uint32_t q);
  PipeOp* pipe_new_op_locked(Pipe& p, PipeOpKind kind, uint32_t end, uint64_t owner);
  PipeOp* pipe_find_op_locked(Pipe& p, uint32_t tag);
  void pipe_free_op_locked(Pipe& p, PipeOp& op);
  bool post_port_packet_locked(Pipe& p, uint32_t end, uint32_t status, uint32_t bytes);
  static constexpr uint32_t kMaxPipes = 128, kMaxSections = 64, kMaxLocks = 256, kMaxShares = 1024, kMaxTimes = 512, kMaxLinkTexts = 64;

  struct Watch {
    bool used = false;
    char path[kWatchPathBytes] = {};
    uint32_t path_size = 0;
    bool subtree = false;
    uint32_t filter = 0;
    WatchRecord records[kMaxWatchRecords];      // a ring, oldest first: ReadDirectoryChangesW's own queue (a plain FindFirstChangeNotification watch never drains it)
    uint32_t record_head = 0, record_count = 0;
  };
  void free_watch_locked(Object& o);

  struct Mailslot;
  Mailslot* mailslot_of(uint32_t id);      // (locked)
  void free_mailslot_locked(Object& o);

  ISystemSync& sync_;
  struct SyncWait { uint64_t thread = 0; uint32_t pipe = 0, end_code = 0; bool used = false, cancelled = false; };
  static constexpr uint32_t kMaxSyncWaits = 64;
  SyncWait sync_waits_[kMaxSyncWaits];
  struct ForeignPacket { uint32_t semaphore = 0; PortPacket packet; };
  static constexpr uint32_t kMaxForeignPackets = 64;
  ForeignPacket foreign_[kMaxForeignPackets];
  uint32_t foreign_head_ = 0, foreign_count_ = 0;
  uint64_t pipe_sequence_ = 0;      // numbers the instances of pipes in the order they start to listen
  allocators::IAllocator* allocator_ = nullptr;
  Object objects_[kCapacity];
  Pipe* pipes_ = nullptr;      // kMaxPipes of them, made when the first pipe is
  struct RangeLock;
  RangeLock* locks_ = nullptr;      // kMaxLocks of them, made with the first
  struct ShareEntry;
  ShareEntry* shares_ = nullptr;      // kMaxShares of them, made with the first
  struct TimesEntry;
  TimesEntry* times_ = nullptr;      // kMaxTimes of them, made with the first
  struct LinkEntry;
  LinkEntry* links_ = nullptr;      // kMaxLinkTexts of them, made with the first
  struct ImageEntry;
  ImageEntry* images_ = nullptr;     // kMaxImages of them, made with the first
  uint32_t image_next_ = 0;
  Section* sections_ = nullptr;      // kMaxSections, made with the first section
  Job* jobs_ = nullptr;              // kJobMaxJobs, made with the first job (never moved: job_kill_for() reads it without the lock)
  uint32_t pending_kills_ = 0;       // members flagged to end that have not ended yet (atomic)
  Watch* watches_ = nullptr;         // kMaxWatches, made with the first change notification
  Mailslot* mailslots_ = nullptr;    // kMaxMailslots, made with the first mailslot
};

// The objects of the system (null until whoever runs it has made them: the kernel, at start-up; host tests set their own).
ObjectManager* system_objects();
void set_system_objects(ObjectManager* manager);

}  // namespace process
