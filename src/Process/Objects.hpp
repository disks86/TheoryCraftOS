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

enum class ObjectKind : uint8_t { None, Event, Mutex, Semaphore, Process, Pipe, Timer, Section, Job, ChangeNotification };

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
  Created create_pipe(StringView name, uint32_t out_size, uint32_t in_size, uint32_t max_instances, uint32_t access);
  // A client connects to an instance of the named pipe that is not connected yet (status 0xC0000034 if no pipe has the name, 0xC00000AE if all are connected).
  // `want_read` / `want_write`: what the client asks for; an instance whose direction does not allow it is refused (0xC0000022) and stays unconnected.
  Created connect_pipe(StringView name, bool want_read, bool want_write);
  // An end is named by an `end_code`: bit 0 says which end, the rest (for the client end) which connection it belongs to, so that a handle left over from a connection the server
  // ended cannot be mistaken for the next client.
  void add_pipe_end(uint32_t id, uint32_t end_code);
  void release_pipe_end(uint32_t id, uint32_t end_code);
  enum class PipeResult { Ok, WouldBlock, Broken, Closing, NotConnected };
  // The lock must be held (sync().lock()) for the _locked calls. Read/write move what fits (at least one byte) or say why not.
  PipeResult pipe_read_locked(uint32_t id, uint32_t end_code, uint8_t* out, uint32_t capacity, uint32_t* got, bool peek = false, uint32_t* available = nullptr);
  PipeResult pipe_write_locked(uint32_t id, uint32_t end_code, const uint8_t* data, uint32_t count, uint32_t* done);
  // The server end listens: Ok if a client is connected (already or now), WouldBlock otherwise.
  PipeResult pipe_listen_locked(uint32_t id);
  // How much this end wrote that the other end has not read yet (0 when the other end is gone).
  uint32_t pipe_pending_locked(uint32_t id, uint32_t end_code);
  void pipe_disconnect(uint32_t id);      // the server ends the connection (the instance can listen again)
  bool pipe_name_available_locked(StringView name);      // some instance of the named pipe is not connected
  struct PipeInfo {
    bool anonymous = false, connected = false;
    uint32_t access = 0, out_size = 0, in_size = 0, max_instances = 0, instances = 0;
  };
  bool pipe_info(uint32_t id, PipeInfo* out);
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
  };
  // `fs` / `host_path`: the file that backs it (nullptr: plain memory, zero at the start). A file is read into the section (its size or `size`, the bigger).
  Created create_section(StringView name, uint64_t size, bool writable, filesystem::IFileSystem* fs, StringView host_path);
  // The memory of the section for AddressSpace::map_shared(); false if there is no such section.
  bool section_memory(uint32_t id, uint8_t** storage, uint64_t* size, SharedMemory** owner);
  bool section_info(uint32_t id, SectionInfo* out);
  void flush_section(uint32_t id);
  void flush_section_at(uint32_t index);      // (by its index among the sections)
  // How many views and handles keep the section alive (for the mapping side: see SharedMemory).
  void section_count(uint32_t index, int32_t delta);

  // --- byte-range locks on files (LockFileEx): advisory, for the whole system; a file is named by its path in the OS file system, a lock belongs to an open file (`owner`)
  enum class LockResult { Granted, Conflict, Full };
  LockResult lock_range_locked(StringView path, const void* owner, uint64_t offset, uint64_t length, bool exclusive);      // (the lock must be held)
  bool unlock_range(StringView path, const void* owner, uint64_t offset, uint64_t length);
  void unlock_all(const void* owner);      // the open file is gone

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

  // --- change notifications (kind ChangeNotification; FindFirstChangeNotification/ReadDirectoryChangesW): a handle that becomes signalled, exactly like a manual-reset
  // event, when something is created, deleted, renamed or linked directly under the directory it watches (or anywhere under it, if `watch_subtree`). It carries no
  // detail about what changed, matching FindFirstChangeNotification's own contract; reset it (FindNextChangeNotification) with reset_event(), which this kind shares with
  // Event. `filter` is kept but not matched against: every one of those changes signals every watch of its directory, regardless of which FILE_NOTIFY_CHANGE_* bits it asked
  // for (a simplification, like this OS's permissive registry access checks).
  static constexpr uint32_t kMaxWatches = 64, kWatchPathBytes = 512;
  Created create_change_notification(StringView path, bool watch_subtree, uint32_t filter);
  // Call after any successful change to a directory's contents (a file or subdirectory created, deleted, renamed or linked): `affected_directory` is that directory's
  // own virtual path (the parent of whatever changed, not the changed entry itself). Signals every watch whose directory this is under (or equal to).
  void notify_path_changed(StringView affected_directory);

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
    int32_t count = 0, maximum = 0;        // semaphore
    uint64_t owner = 0;                    // mutex: the thread that owns it, 0 if none
    int32_t recursion = 0;
    int32_t waiters = 0, pulses = 0;       // PulseEvent lets through the threads that are waiting now
    int32_t pipe = -1;                     // pipe: its index in pipes_
    int32_t section = -1;                  // section: its index in sections_
    int32_t job = -1;                      // job: its index in jobs_
    int32_t watch = -1;                    // change notification: its index in watches_
    uint64_t due_ns = 0, period_ns = 0;    // timer: when it fires next, how often
    uint64_t apc_thread = 0, apc_routine = 0, apc_context = 0;      // timer: the APC it queues
    bool armed = false, apc_pending = false;
    uint64_t fired_at = 0;
  };
  Object* at(uint32_t id) { return id >= 1 && id <= kCapacity && objects_[id - 1].kind != ObjectKind::None ? &objects_[id - 1] : nullptr; }
  Object* find_locked(StringView name);
  void fire_timer_locked(Object& o, uint64_t now);

  struct Pipe;
  struct Section;
  class SectionRef;
  void write_section_locked(Section& s);
  void free_section_locked(Object& o);
  struct JobMember { uint64_t pid = 0; uint32_t kill = 0, code = 0; };
  struct Job {
    bool used = false;
    uint32_t object = 0;
    JobInfo info;
    JobMember members[kJobMaxMembers];
  };
  void free_job_locked(Object& o);
  void job_forget_locked(uint64_t pid);
  Job* job_of(uint32_t id);      // (locked)
  void job_flag_all_locked(Job& j, uint32_t code);
  Pipe* pipe_of(uint32_t id);      // (locked)
  void free_pipe_locked(Object& o);
  static constexpr uint32_t kMaxPipes = 128, kMaxSections = 64, kMaxLocks = 256;

  struct Watch { bool used = false; char path[kWatchPathBytes] = {}; uint32_t path_size = 0; bool subtree = false; uint32_t filter = 0; };
  void free_watch_locked(Object& o);

  ISystemSync& sync_;
  allocators::IAllocator* allocator_ = nullptr;
  Object objects_[kCapacity];
  Pipe* pipes_ = nullptr;      // kMaxPipes of them, made when the first pipe is
  struct RangeLock;
  RangeLock* locks_ = nullptr;      // kMaxLocks of them, made with the first
  Section* sections_ = nullptr;      // kMaxSections, made with the first section
  Job* jobs_ = nullptr;              // kJobMaxJobs, made with the first job (never moved: job_kill_for() reads it without the lock)
  uint32_t pending_kills_ = 0;       // members flagged to end that have not ended yet (atomic)
  Watch* watches_ = nullptr;         // kMaxWatches, made with the first change notification
};

// The objects of the system (null until whoever runs it has made them: the kernel, at start-up; host tests set their own).
ObjectManager* system_objects();
void set_system_objects(ObjectManager* manager);

}  // namespace process
