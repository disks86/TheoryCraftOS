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
#include "Process/LinuxIpc.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

namespace process {

LinuxIpcManager::~LinuxIpcManager() {
  for (Segment& segment : segments)
    if (segment.used && segment.block) segment.block->release();
}

namespace {
LinuxIpcManager* g_ipc_override = nullptr;
LinuxIpcManager* g_ipc_made = nullptr;
}  // namespace

LinuxIpcManager* system_linux_ipc() {
  if (LinuxIpcManager* chosen = __atomic_load_n(&g_ipc_override, __ATOMIC_ACQUIRE)) return chosen;
  if (LinuxIpcManager* made = __atomic_load_n(&g_ipc_made, __ATOMIC_ACQUIRE)) return made;
  ObjectManager* objects = system_objects();
  if (!objects || !objects->allocator()) return nullptr;
  ISystemSync& sync = objects->sync();
  sync.lock();      // (two first users at once: one makes it)
  LinuxIpcManager* made = g_ipc_made;
  if (!made) {
    allocators::IAllocator& allocator = *objects->allocator();
    if (void* memory = allocator.allocate(sizeof(LinuxIpcManager))) {
      made = new (memory) LinuxIpcManager(sync, allocator);      // (lives as long as the system does)
      __atomic_store_n(&g_ipc_made, made, __ATOMIC_RELEASE);
    }
  }
  sync.unlock();
  return made;
}

void set_system_linux_ipc(LinuxIpcManager* manager) { __atomic_store_n(&g_ipc_override, manager, __ATOMIC_RELEASE); }

namespace {
// The manager if there is one already (a process that ends has nothing to take back from one that was never made).
LinuxIpcManager* existing_linux_ipc() {
  if (LinuxIpcManager* chosen = __atomic_load_n(&g_ipc_override, __ATOMIC_ACQUIRE)) return chosen;
  return __atomic_load_n(&g_ipc_made, __ATOMIC_ACQUIRE);
}
}  // namespace

namespace linuxabi {

namespace {

constexpr int64_t kEnoent = 2, kEintr = 4, kE2big = 7, kEagain = 11, kEnomem = 12, kEfault = 14, kEexist = 17, kEinval = 22, kEmfile = 24, kEfbig = 27, kEnospc = 28,
                  kErange = 34, kEnosys = 38, kEnomsg = 42, kEidrm = 43;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint64_t kIpcCreate = 0x200, kIpcExclusive = 0x400, kIpcNoWait = 0x800;
constexpr uint64_t kIpcRemove = 0, kIpcSet = 1, kIpcStat = 2, kIpcInfo = 3;
constexpr uint32_t kIdStep = 32768;      // an id is the slot's index plus this times the slot's sequence number
constexpr uint64_t kPermissionBytes = 48;

struct Locked {
  ISystemSync& sync;
  explicit Locked(ISystemSync& s) : sync(s) { sync.lock(); }
  ~Locked() { sync.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};

uint64_t seconds_now(Process& p) { return p.host().realtime_ns() / 1000000000u; }
uint32_t make_id(uint32_t index, const LinuxIpcManager::Permission& permission) { return index + uint32_t{permission.sequence} * kIdStep; }

// The slot an id names in a table of `Slot`s, or null (never made, removed, or the slot is something newer by now).
template <typename Slot, uint32_t N>
Slot* by_id(Slot (&table)[N], uint64_t id) {
  if (id > 0x7FFFFFFFu) return nullptr;
  const uint32_t index = uint32_t(id) % kIdStep, sequence = uint32_t(id) / kIdStep;
  if (index >= N || !table[index].used || table[index].permission.sequence != sequence) return nullptr;
  return &table[index];
}

template <typename Slot, uint32_t N>
uint32_t index_of(Slot (&table)[N], const Slot* slot) { return uint32_t(slot - &table[0]); }

// What the three "get" calls share: the thing with this key if there is one (and the flags allow it), else a free slot to make it in.
enum class Lookup : uint8_t { Found, Make, Failed };
template <typename Slot, uint32_t N, typename Hidden>
Lookup look_up(Slot (&table)[N], uint32_t key, uint64_t flags, Hidden&& hidden, Slot** out, uint64_t* failure) {
  if (key != 0) {
    for (Slot& slot : table) {
      if (!slot.used || slot.permission.key != key || hidden(slot)) continue;
      if ((flags & kIpcCreate) && (flags & kIpcExclusive)) { *failure = error(kEexist); return Lookup::Failed; }
      *out = &slot;
      return Lookup::Found;
    }
    if (!(flags & kIpcCreate)) { *failure = error(kEnoent); return Lookup::Failed; }
  }
  for (Slot& slot : table)
    if (!slot.used) { *out = &slot; return Lookup::Make; }
  *failure = error(kEnospc);
  return Lookup::Failed;
}

void start(LinuxIpcManager::Permission& permission, uint32_t key, uint64_t flags) {
  const uint16_t sequence = permission.sequence;
  permission = LinuxIpcManager::Permission();
  permission.sequence = sequence;
  permission.key = key;
  permission.mode = uint32_t(flags & 0777);
}
// The slot is free again: its next user gets another id.
void retire(LinuxIpcManager::Permission& permission) { permission.sequence = uint16_t((permission.sequence + 1) % (0x7FFFFFFFu / kIdStep)); }

void write_permission(ByteWriter& writer, const LinuxIpcManager::Permission& permission) {      // struct ipc64_perm
  writer.le32(permission.key);
  writer.le32(permission.uid);
  writer.le32(permission.gid);
  writer.le32(permission.creator_uid);
  writer.le32(permission.creator_gid);
  writer.le32(permission.mode);
  writer.le16(permission.sequence);
  writer.zeros(kPermissionBytes - 26);
}

// IPC_SET: the owner and the permission bits from the structure the program hands in.
bool read_permission(Process& p, uint64_t buffer, LinuxIpcManager::Permission* permission) {
  uint8_t raw[24];
  if (!p.read_user(buffer, ByteView(raw, sizeof(raw)))) return false;
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  reader.skip(4);
  permission->uid = reader.le32();
  permission->gid = reader.le32();
  reader.skip(8);
  permission->mode = (permission->mode & ~0777u) | (reader.le32() & 0777u);
  return true;
}

// A struct timespec to wait at most, as nanoseconds (~0: for ever).
uint64_t read_timeout(Process& p, uint64_t address, uint64_t* out) {
  *out = ~uint64_t{0};
  if (address == 0) return 0;
  uint64_t seconds = 0, nanoseconds = 0;
  if (!p.read_user_u64(address, &seconds) || !p.read_user_u64(address + 8, &nanoseconds)) return error(kEfault);
  if (int64_t(seconds) < 0 || nanoseconds >= 1000000000u) return error(kEinval);
  *out = seconds > (~uint64_t{0} - 999999999u) / 1000000000u ? ~uint64_t{0} - 1 : seconds * 1000000000u + nanoseconds;
  return 0;
}

// --- shared memory -------------------------------------------------------------------------------------------------------------------------------------

// Segments that were removed and that no process has attached any more: their memory goes back. (A mapping holds a use of the block, so "nobody has it
// attached" is "the table's use is the only one". Looked at whenever a shared-memory call is made.)
void sweep_segments(LinuxIpcManager& ipc) {
  for (LinuxIpcManager::Segment& segment : ipc.segments) {
    if (!segment.used || !segment.removed || segment.block->uses() != 1) continue;
    segment.block->release();
    const LinuxIpcManager::Permission old = segment.permission;
    segment = LinuxIpcManager::Segment();
    segment.permission.sequence = old.sequence;
    retire(segment.permission);
  }
}

uint64_t attached_count(const LinuxIpcManager::Segment& segment) { return segment.block->uses() - 1; }

void write_segment(ByteWriter& writer, const LinuxIpcManager::Segment& segment) {      // struct shmid64_ds
  LinuxIpcManager::Permission permission = segment.permission;
  if (segment.removed) permission.mode |= 01000;      // SHM_DEST
  write_permission(writer, permission);
  writer.le64(segment.size);
  writer.le64(segment.attach_time);
  writer.le64(segment.detach_time);
  writer.le64(segment.change_time);
  writer.le32(segment.creator);
  writer.le32(segment.last);
  writer.le64(attached_count(segment));
  writer.zeros(16);
}

}  // namespace

uint64_t sys_shmget(Process& p, uint64_t key, uint64_t size, uint64_t flags) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  if (flags & 04000) return error(kEinval);      // SHM_HUGETLB: there are no huge pages to give
  Locked locked(ipc->sync());
  sweep_segments(*ipc);
  LinuxIpcManager::Segment* segment = nullptr;
  uint64_t failure = 0;
  switch (look_up(ipc->segments, uint32_t(key), flags, [](const LinuxIpcManager::Segment& s) { return s.removed; }, &segment, &failure)) {
    case Lookup::Failed: return failure;
    case Lookup::Found:
      if (size > segment->size) return error(kEinval);
      return make_id(index_of(ipc->segments, segment), segment->permission);
    case Lookup::Make: break;
  }
  uint64_t rounded = 0;
  if (size == 0 || size > LinuxIpcManager::kSegmentMax || !AddressSpace::round_up(size, &rounded)) return error(kEinval);
  MemoryBlock* block = MemoryBlock::make(ipc->allocator(), rounded);
  if (!block) return error(kEnomem);
  start(segment->permission, uint32_t(key), flags);
  segment->used = true;
  segment->removed = false;
  segment->size = size;
  segment->block = block;
  segment->attach_time = segment->detach_time = 0;
  segment->change_time = seconds_now(p);
  segment->creator = uint32_t(p.id());
  segment->last = 0;
  return make_id(index_of(ipc->segments, segment), segment->permission);
}

uint64_t sys_shmat(Process& p, uint64_t id, uint64_t address, uint64_t flags) {
  constexpr uint64_t kReadOnly = 010000, kRound = 020000, kRemap = 040000, kExecute = 0100000;
  LinuxIpcManager* ipc = system_linux_ipc();
  AddressSpace* space = p.address_space();
  if (!ipc || !space) return error(kEnosys);
  if (flags & ~(kReadOnly | kRound | kRemap | kExecute)) return error(kEinval);
  Locked locked(ipc->sync());
  sweep_segments(*ipc);
  LinuxIpcManager::Segment* segment = by_id(ipc->segments, id);
  if (!segment) return error(kEinval);
  const uint64_t size = segment->block->capacity();
  LinuxProcessState::ShmAttach* record = nullptr;
  for (LinuxProcessState::ShmAttach& attach : p.linux_state().shm_attaches)
    if (attach.size == 0) { record = &attach; break; }
  if (!record && !(flags & kRemap)) return error(kEmfile);
  uint64_t where = 0;
  if (address != 0) {
    if (flags & kRound) address &= ~(AddressSpace::kPageSize - 1);
    if (address % AddressSpace::kPageSize != 0 || address >= AddressSpace::kUserTop || size > AddressSpace::kUserTop - address) return error(kEinval);
    where = address;
    if (flags & kRemap) {
      if (!space->unmap(where, size)) return error(kEinval);
      // (what was attached there before is attached no longer)
      for (LinuxProcessState::ShmAttach& attach : p.linux_state().shm_attaches)
        if (attach.size != 0 && attach.address < where + size && where < attach.address + attach.size) attach = LinuxProcessState::ShmAttach();
      if (!record || record->size != 0) {
        record = nullptr;
        for (LinuxProcessState::ShmAttach& attach : p.linux_state().shm_attaches)
          if (attach.size == 0) { record = &attach; break; }
        if (!record) return error(kEmfile);
      }
    } else {
      uint64_t free_at = 0;
      if (!space->find_free(size, where, &free_at) || free_at != where) return error(kEinval);
    }
  } else {
    if (flags & kRemap) return error(kEinval);
    if (!space->find_free(size, p.mmap_cursor(), &where)) return error(kEnomem);
  }
  const uint8_t protection = uint8_t(kProtRead | ((flags & kReadOnly) ? 0 : kProtWrite) | ((flags & kExecute) ? kProtExecute : 0));
  if (!space->map_shared(where, size, protection, segment->block->storage(), segment->block)) return error(kEnomem);      // (the mapping takes a use of the block)
  if (address == 0) p.set_mmap_cursor(where + size);
  record->address = where;
  record->size = size;
  record->id = uint32_t(id);
  segment->attach_time = seconds_now(p);
  segment->last = uint32_t(p.id());
  return where;
}

uint64_t sys_shmdt(Process& p, uint64_t address) {
  LinuxIpcManager* ipc = system_linux_ipc();
  AddressSpace* space = p.address_space();
  if (!ipc || !space) return error(kEnosys);
  Locked locked(ipc->sync());
  for (LinuxProcessState::ShmAttach& attach : p.linux_state().shm_attaches) {
    if (attach.size == 0 || attach.address != address) continue;
    const LinuxProcessState::ShmAttach was = attach;
    attach = LinuxProcessState::ShmAttach();
    uint8_t protection = 0;
    if (!space->protection_at(was.address, &protection) || !space->unmap(was.address, was.size)) return error(kEinval);      // (the program had unmapped it itself)
    if (LinuxIpcManager::Segment* segment = by_id(ipc->segments, was.id)) {
      segment->detach_time = seconds_now(p);
      segment->last = uint32_t(p.id());
    }
    sweep_segments(*ipc);
    return 0;
  }
  return error(kEinval);
}

uint64_t sys_shmctl(Process& p, uint64_t id, uint64_t command, uint64_t buffer) {
  constexpr uint64_t kLock = 11, kUnlock = 12, kStat = 13, kInfo = 14, kStatAny = 15;
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  command &= ~uint64_t{0x100};      // (IPC_64, which 32-bit programs set)
  Locked locked(ipc->sync());
  sweep_segments(*ipc);
  if (command == kIpcInfo || command == kInfo) {
    uint64_t highest = 0, used = 0, pages = 0;
    for (uint32_t i = 0; i < LinuxIpcManager::kSegments; ++i) {
      if (!ipc->segments[i].used) continue;
      highest = i;
      ++used;
      pages += ipc->segments[i].block->capacity() / AddressSpace::kPageSize;
    }
    uint8_t raw[72] = {};
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    if (command == kIpcInfo) {      // struct shminfo64: the most one segment may be, the least, how many segments, how many a process may attach, pages in all
      writer.le64(LinuxIpcManager::kSegmentMax);
      writer.le64(1);
      writer.le64(LinuxIpcManager::kSegments);
      writer.le64(LinuxProcessState::kMaxShmAttaches);
      writer.le64(LinuxIpcManager::kSegments * (LinuxIpcManager::kSegmentMax / AddressSpace::kPageSize));
      writer.zeros(32);
    } else {      // struct shm_info: segments in use, their pages, how many of those have memory (all), and what was swapped (nothing)
      writer.le32(uint32_t(used));
      writer.le32(0);
      writer.le64(pages);
      writer.le64(pages);
      writer.zeros(24);
    }
    if (!p.write_user(buffer, writer.written())) return error(kEfault);
    return highest;
  }
  LinuxIpcManager::Segment* segment = nullptr;
  if (command == kStat || command == kStatAny) {      // by its place in the table, for a program that lists them all
    if (id >= LinuxIpcManager::kSegments || !ipc->segments[id].used) return error(kEinval);
    segment = &ipc->segments[id];
  } else {
    segment = by_id(ipc->segments, id);
    if (!segment) return error(kEinval);
  }
  switch (command) {
    case kIpcStat: case kStat: case kStatAny: {
      uint8_t raw[112];
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      write_segment(writer, *segment);
      if (!writer.ok() || !p.write_user(buffer, writer.written())) return error(kEfault);
      return command == kIpcStat ? 0 : make_id(index_of(ipc->segments, segment), segment->permission);
    }
    case kIpcSet:
      if (!read_permission(p, buffer, &segment->permission)) return error(kEfault);
      segment->change_time = seconds_now(p);
      return 0;
    case kIpcRemove:
      segment->removed = true;
      segment->permission.key = 0;
      sweep_segments(*ipc);
      return 0;
    case kLock: case kUnlock: return 0;      // (nothing is ever paged out)
    default: return error(kEinval);
  }
}

// --- semaphores ----------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr int64_t kSemUndo = 0x1000;

void forget_undos(LinuxIpcManager& ipc, uint32_t id, uint32_t semaphore, bool whole_set) {
  for (LinuxIpcManager::Undo& undo : ipc.undos)
    if (undo.used && undo.id == id && (whole_set || undo.semaphore == semaphore)) undo = LinuxIpcManager::Undo();
}

struct SemaphoreOperation {
  uint32_t semaphore = 0;
  int32_t change = 0;
  uint32_t flags = 0;
};

}  // namespace

uint64_t sys_semget(Process& p, uint64_t key, uint64_t count, uint64_t flags) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  if (int32_t(count) < 0 || uint32_t(count) > LinuxIpcManager::kSemsPerSet) return error(kEinval);
  Locked locked(ipc->sync());
  LinuxIpcManager::SemaphoreSet* set = nullptr;
  uint64_t failure = 0;
  switch (look_up(ipc->sets, uint32_t(key), flags, [](const LinuxIpcManager::SemaphoreSet&) { return false; }, &set, &failure)) {
    case Lookup::Failed: return failure;
    case Lookup::Found:
      if (uint32_t(count) > set->count) return error(kEinval);
      return make_id(index_of(ipc->sets, set), set->permission);
    case Lookup::Make: break;
  }
  if (uint32_t(count) == 0) return error(kEinval);
  const LinuxIpcManager::Permission old = set->permission;
  *set = LinuxIpcManager::SemaphoreSet();
  set->permission = old;
  start(set->permission, uint32_t(key), flags);
  set->used = true;
  set->count = uint32_t(count);
  set->change_time = seconds_now(p);
  return make_id(index_of(ipc->sets, set), set->permission);
}

uint64_t sys_semtimedop(Process& p, uint64_t id, uint64_t operations, uint64_t count, uint64_t timeout) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  if (count == 0) return error(kEinval);
  if (count > LinuxIpcManager::kOperationsMax) return error(kE2big);
  SemaphoreOperation list[LinuxIpcManager::kOperationsMax];
  for (uint64_t i = 0; i < count; ++i) {      // struct sembuf: sem_num (unsigned short), sem_op (short), sem_flg (short)
    uint8_t raw[6];
    if (!p.read_user(operations + i * 6, ByteView(raw, sizeof(raw)))) return error(kEfault);
    ByteReader reader{ConstByteView(raw, sizeof(raw))};
    list[i].semaphore = reader.le16();
    list[i].change = int16_t(reader.le16());
    list[i].flags = reader.le16();
  }
  uint64_t wait_ns = 0;
  if (const uint64_t r = read_timeout(p, timeout, &wait_ns); r != 0) return r;

  uint64_t result = 0;
  bool first = true;
  // While the thread waits it is counted at the semaphore that holds it up (GETNCNT, GETZCNT).
  bool counted = false, counted_zero = false;
  uint32_t counted_semaphore = 0;
  auto uncount = [&]() {
    if (!counted) return;
    counted = false;
    LinuxIpcManager::SemaphoreSet* set = by_id(ipc->sets, id);
    if (!set) return;
    uint16_t& counter = counted_zero ? set->waiting_for_zero[counted_semaphore] : set->waiting_for_more[counted_semaphore];
    if (counter > 0) --counter;
  };
  const uint32_t outcome = system_wait(p, ipc->sync(), wait_ns, [&]() -> bool {
    uncount();
    LinuxIpcManager::SemaphoreSet* set = by_id(ipc->sets, id);
    if (!set) {      // (never there, or removed while this thread waited)
      result = error(first ? kEinval : kEidrm);
      return true;
    }
    first = false;
    // All of the operations or none: they are tried on a copy of the values first.
    uint32_t value[LinuxIpcManager::kSemsPerSet];
    for (uint32_t i = 0; i < set->count; ++i) value[i] = set->value[i];
    for (uint64_t i = 0; i < count; ++i) {
      const SemaphoreOperation& op = list[i];
      if (op.semaphore >= set->count) { result = error(kEfbig); return true; }
      bool blocked = false;
      if (op.change > 0) {
        if (value[op.semaphore] + uint32_t(op.change) > LinuxIpcManager::kSemaphoreMax) { result = error(kErange); return true; }
        value[op.semaphore] += uint32_t(op.change);
      } else if (op.change < 0) {
        if (value[op.semaphore] < uint32_t(-op.change)) blocked = true;
        else value[op.semaphore] -= uint32_t(-op.change);
      } else if (value[op.semaphore] != 0) {
        blocked = true;
      }
      if (!blocked) continue;
      if (op.flags & kIpcNoWait) { result = error(kEagain); return true; }
      counted = true;
      counted_zero = op.change == 0;
      counted_semaphore = op.semaphore;
      uint16_t& counter = counted_zero ? set->waiting_for_zero[counted_semaphore] : set->waiting_for_more[counted_semaphore];
      if (counter < 0xFFFF) ++counter;
      return false;
    }
    // What SEM_UNDO has to remember must fit before anything is changed.
    uint32_t free_undos = 0;
    for (const LinuxIpcManager::Undo& undo : ipc->undos) free_undos += undo.used ? 0 : 1;
    uint32_t wanted = 0;
    for (uint64_t i = 0; i < count; ++i) {
      if (!(list[i].flags & kSemUndo) || list[i].change == 0) continue;
      bool known = false;
      for (const LinuxIpcManager::Undo& undo : ipc->undos) known = known || (undo.used && undo.process == p.id() && undo.id == uint32_t(id) && undo.semaphore == list[i].semaphore);
      for (uint64_t j = 0; j < i; ++j) known = known || ((list[j].flags & kSemUndo) && list[j].change != 0 && list[j].semaphore == list[i].semaphore);
      wanted += known ? 0 : 1;
    }
    if (wanted > free_undos) { result = error(kEnospc); return true; }
    for (uint64_t i = 0; i < count; ++i) {
      const SemaphoreOperation& op = list[i];
      set->last[op.semaphore] = uint32_t(p.id());
      if (!(op.flags & kSemUndo) || op.change == 0) continue;
      LinuxIpcManager::Undo* entry = nullptr;
      for (LinuxIpcManager::Undo& undo : ipc->undos)
        if (undo.used && undo.process == p.id() && undo.id == uint32_t(id) && undo.semaphore == op.semaphore) entry = &undo;
      if (!entry)
        for (LinuxIpcManager::Undo& undo : ipc->undos)
          if (!undo.used) { entry = &undo; break; }
      if (!entry) continue;      // (cannot be: counted above)
      entry->used = true;
      entry->process = p.id();
      entry->id = uint32_t(id);
      entry->semaphore = op.semaphore;
      entry->adjust -= op.change;
    }
    for (uint32_t i = 0; i < set->count; ++i) set->value[i] = uint16_t(value[i]);
    set->operation_time = seconds_now(p);
    ipc->sync().notify_all();
    result = 0;
    return true;
  });
  if (outcome != 0) {
    Locked locked(ipc->sync());
    uncount();
    return error(outcome == 1 ? kEagain : kEintr);
  }
  return result;
}

uint64_t sys_semctl(Process& p, UserArch arch, uint64_t id, uint64_t semaphore, uint64_t command, uint64_t argument) {
  constexpr uint64_t kGetPid = 11, kGetValue = 12, kGetAll = 13, kGetWaitingMore = 14, kGetWaitingZero = 15, kSetValue = 16, kSetAll = 17, kStat = 18, kInfo = 19, kStatAny = 20;
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  command &= ~uint64_t{0x100};
  Locked locked(ipc->sync());
  if (command == kIpcInfo || command == kInfo) {      // struct seminfo: ten ints
    uint32_t highest = 0, used_sets = 0, used_semaphores = 0;
    for (uint32_t i = 0; i < LinuxIpcManager::kSemSets; ++i) {
      if (!ipc->sets[i].used) continue;
      highest = i;
      ++used_sets;
      used_semaphores += ipc->sets[i].count;
    }
    const uint32_t all = LinuxIpcManager::kSemSets * LinuxIpcManager::kSemsPerSet;
    uint8_t raw[40];
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    writer.le32(all);                                 // semmap
    writer.le32(LinuxIpcManager::kSemSets);           // semmni
    writer.le32(all);                                 // semmns
    writer.le32(LinuxIpcManager::kUndos);             // semmnu
    writer.le32(LinuxIpcManager::kSemsPerSet);        // semmsl
    writer.le32(LinuxIpcManager::kOperationsMax);     // semopm
    writer.le32(LinuxIpcManager::kUndos);             // semume
    writer.le32(command == kInfo ? used_sets : 20);   // semusz (SEM_INFO: the sets in use)
    writer.le32(LinuxIpcManager::kSemaphoreMax);      // semvmx
    writer.le32(command == kInfo ? used_semaphores : LinuxIpcManager::kSemaphoreMax);      // semaem (SEM_INFO: the semaphores in use)
    if (!p.write_user(argument, writer.written())) return error(kEfault);
    return highest;
  }
  LinuxIpcManager::SemaphoreSet* set = nullptr;
  if (command == kStat || command == kStatAny) {
    if (id >= LinuxIpcManager::kSemSets || !ipc->sets[id].used) return error(kEinval);
    set = &ipc->sets[id];
  } else {
    set = by_id(ipc->sets, id);
    if (!set) return error(kEinval);
  }
  const bool one = command == kGetPid || command == kGetValue || command == kGetWaitingMore || command == kGetWaitingZero || command == kSetValue;
  if (one && semaphore >= set->count) return error(kEinval);
  switch (command) {
    case kIpcStat: case kStat: case kStatAny: {
      // struct semid64_ds: the permissions, sem_otime, sem_ctime, sem_nsems. x86-64 has an unused word after each of the two times; the other CPUs have none.
      uint8_t raw[104] = {};
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      write_permission(writer, set->permission);
      writer.le64(set->operation_time);
      if (arch == UserArch::X86_64) writer.le64(0);
      writer.le64(set->change_time);
      if (arch == UserArch::X86_64) writer.le64(0);
      writer.le64(set->count);
      writer.zeros(16);
      if (!writer.ok() || !p.write_user(argument, writer.written())) return error(kEfault);
      return command == kIpcStat ? 0 : make_id(index_of(ipc->sets, set), set->permission);
    }
    case kIpcSet:
      if (!read_permission(p, argument, &set->permission)) return error(kEfault);
      set->change_time = seconds_now(p);
      return 0;
    case kIpcRemove: {
      forget_undos(*ipc, make_id(index_of(ipc->sets, set), set->permission), 0, true);
      const LinuxIpcManager::Permission old = set->permission;
      *set = LinuxIpcManager::SemaphoreSet();
      set->permission.sequence = old.sequence;
      retire(set->permission);
      ipc->sync().notify_all();      // (whoever waits on it finds it gone: EIDRM)
      return 0;
    }
    case kGetPid: return set->last[semaphore];
    case kGetValue: return set->value[semaphore];
    case kGetWaitingMore: return set->waiting_for_more[semaphore];
    case kGetWaitingZero: return set->waiting_for_zero[semaphore];
    case kGetAll: {
      uint8_t raw[LinuxIpcManager::kSemsPerSet * 2];
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      for (uint32_t i = 0; i < set->count; ++i) writer.le16(set->value[i]);
      return p.write_user(argument, writer.written()) ? 0 : error(kEfault);
    }
    case kSetValue: {
      if (int32_t(argument) < 0 || uint32_t(argument) > LinuxIpcManager::kSemaphoreMax) return error(kErange);
      set->value[semaphore] = uint16_t(argument);
      set->last[semaphore] = uint32_t(p.id());
      set->change_time = seconds_now(p);
      forget_undos(*ipc, uint32_t(id), uint32_t(semaphore), false);
      ipc->sync().notify_all();
      return 0;
    }
    case kSetAll: {
      uint8_t raw[LinuxIpcManager::kSemsPerSet * 2];
      if (!p.read_user(argument, ByteView(raw, uint64_t{set->count} * 2))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, uint64_t{set->count} * 2)};
      uint16_t value[LinuxIpcManager::kSemsPerSet];
      for (uint32_t i = 0; i < set->count; ++i) {
        value[i] = reader.le16();
        if (value[i] > LinuxIpcManager::kSemaphoreMax) return error(kErange);
      }
      for (uint32_t i = 0; i < set->count; ++i) {
        set->value[i] = value[i];
        set->last[i] = uint32_t(p.id());
      }
      set->change_time = seconds_now(p);
      forget_undos(*ipc, uint32_t(id), 0, true);
      ipc->sync().notify_all();
      return 0;
    }
    default: return error(kEinval);
  }
}

void ipc_process_gone(uint64_t pid) {
  LinuxIpcManager* ipc = existing_linux_ipc();
  if (!ipc) return;
  Locked locked(ipc->sync());
  bool changed = false;
  for (LinuxIpcManager::Undo& undo : ipc->undos) {
    if (!undo.used || undo.process != pid) continue;
    if (LinuxIpcManager::SemaphoreSet* set = by_id(ipc->sets, undo.id); set && undo.semaphore < set->count) {
      const int64_t value = int64_t{set->value[undo.semaphore]} + undo.adjust;
      set->value[undo.semaphore] = uint16_t(value < 0 ? 0 : value > int64_t{LinuxIpcManager::kSemaphoreMax} ? LinuxIpcManager::kSemaphoreMax : value);
      set->last[undo.semaphore] = uint32_t(pid);
      changed = true;
    }
    undo = LinuxIpcManager::Undo();
  }
  if (changed) ipc->sync().notify_all();
}

// --- message queues ------------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr uint64_t kRecordHead = 12;      // a message in a queue's buffer: its type (8 bytes), the size of its text (4 bytes), the text

struct Found {
  bool any = false;
  uint64_t at = 0, type = 0, size = 0;
};

// The message msgrcv(type, flags) takes: the first one (type 0), the first of that type (or, with MSG_EXCEPT, of any other), or for a negative type the
// first of the lowest type that is no greater than its magnitude.
Found find_message(const ByteBuffer& data, int64_t type, bool except) {
  Found found;
  const ConstByteView bytes = data.view();
  for (uint64_t at = 0; at + kRecordHead <= bytes.size();) {
    ByteReader reader{bytes.subview(at, kRecordHead)};
    const uint64_t record_type = reader.le64();
    const uint64_t size = reader.le32();
    if (size > bytes.size() - at - kRecordHead) break;      // (cannot be: the queue only holds what msgsnd put there whole)
    bool match;
    if (type == 0) match = true;
    else if (type > 0) match = (record_type == uint64_t(type)) != except;
    else match = record_type <= uint64_t(-(type + 1)) + 1 && (!found.any || record_type < found.type);
    if (match) {
      found.any = true;
      found.at = at;
      found.type = record_type;
      found.size = size;
      if (type >= 0) break;
    }
    at += kRecordHead + size;
  }
  return found;
}

void take_out(ByteBuffer& data, uint64_t at, uint64_t bytes) {
  ByteView all = data.writable_view();
  for (uint64_t i = at; i + bytes < all.size(); ++i) all[i] = all[i + bytes];
  data.truncate(all.size() - bytes);
}

void free_queue(LinuxIpcManager::Queue& queue) {
  const LinuxIpcManager::Permission old = queue.permission;
  queue = LinuxIpcManager::Queue();
  queue.permission.sequence = old.sequence;
  retire(queue.permission);
}

}  // namespace

uint64_t sys_msgget(Process& p, uint64_t key, uint64_t flags) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  Locked locked(ipc->sync());
  LinuxIpcManager::Queue* queue = nullptr;
  uint64_t failure = 0;
  switch (look_up(ipc->queues, uint32_t(key), flags, [](const LinuxIpcManager::Queue&) { return false; }, &queue, &failure)) {
    case Lookup::Failed: return failure;
    case Lookup::Found: return make_id(index_of(ipc->queues, queue), queue->permission);
    case Lookup::Make: break;
  }
  UniquePointer<ByteBuffer> data = UniquePointer<ByteBuffer>::make(ipc->allocator(), ipc->allocator());
  if (!data) return error(kEnomem);
  const LinuxIpcManager::Permission old = queue->permission;
  *queue = LinuxIpcManager::Queue();
  queue->permission = old;
  start(queue->permission, uint32_t(key), flags);
  queue->used = true;
  queue->data = static_cast<UniquePointer<ByteBuffer>&&>(data);
  queue->change_time = seconds_now(p);
  return make_id(index_of(ipc->queues, queue), queue->permission);
}

// msgsnd(id, struct msgbuf { long mtype; char mtext[size]; }*, size, flags)
uint64_t sys_msgsnd(Process& p, uint64_t id, uint64_t message, uint64_t size, uint64_t flags) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  if (size > LinuxIpcManager::kMessageMax) return error(kEinval);
  ByteBuffer text(ipc->allocator());      // (the text, read before anything is locked)
  if (!text.put_zeros(size)) return error(kEnomem);
  uint64_t type = 0;
  if (!p.read_user_u64(message, &type)) return error(kEfault);
  if (int64_t(type) <= 0) return error(kEinval);
  if (size != 0 && !p.read_user(message + 8, text.writable_view())) return error(kEfault);
  uint64_t result = 0;
  bool first = true;
  const uint32_t outcome = system_wait(p, ipc->sync(), ~uint64_t{0}, [&]() -> bool {
    LinuxIpcManager::Queue* queue = by_id(ipc->queues, id);
    if (!queue) { result = error(first ? kEinval : kEidrm); return true; }
    first = false;
    if (size > queue->limit) { result = error(kEinval); return true; }
    if (queue->bytes + size > queue->limit || queue->messages >= queue->limit) {      // (no room: also a limit on empty messages, which take no bytes)
      if (flags & kIpcNoWait) { result = error(kEagain); return true; }
      return false;
    }
    ByteBuffer& data = *queue->data;
    const uint64_t before = data.size();
    if (!data.put32(uint32_t(type)) || !data.put32(uint32_t(type >> 32)) || !data.put32(uint32_t(size)) || !data.append(text.view())) {
      data.truncate(before);
      result = error(kEnomem);
      return true;
    }
    ++queue->messages;
    queue->bytes += size;
    queue->send_time = seconds_now(p);
    queue->last_sender = uint32_t(p.id());
    ipc->sync().notify_all();
    result = 0;
    return true;
  });
  return outcome == 0 ? result : error(kEintr);
}

uint64_t sys_msgrcv(Process& p, uint64_t id, uint64_t message, uint64_t size, uint64_t type, uint64_t flags) {
  constexpr uint64_t kNoError = 010000, kExcept = 020000, kCopy = 040000;
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  if (flags & kCopy) return error(kEnosys);      // (MSG_COPY is for checkpointing a process, which nothing here does)
  if (int64_t(size) < 0) return error(kEinval);
  if ((flags & kExcept) && int64_t(type) <= 0) flags &= ~kExcept;
  ByteBuffer text(ipc->allocator());      // (what was taken, written to the program after the lock is let go)
  if (!text.put_zeros(size < LinuxIpcManager::kMessageMax ? size : LinuxIpcManager::kMessageMax)) return error(kEnomem);
  uint64_t result = 0, taken_type = 0, taken = 0;
  bool first = true;
  const uint32_t outcome = system_wait(p, ipc->sync(), ~uint64_t{0}, [&]() -> bool {
    LinuxIpcManager::Queue* queue = by_id(ipc->queues, id);
    if (!queue) { result = error(first ? kEinval : kEidrm); return true; }
    first = false;
    ByteBuffer& data = *queue->data;
    const Found found = find_message(data, int64_t(type), (flags & kExcept) != 0);
    if (!found.any) {
      if (flags & kIpcNoWait) { result = error(kEnomsg); return true; }
      return false;
    }
    if (found.size > size && !(flags & kNoError)) { result = error(kE2big); return true; }      // (the message stays in the queue)
    taken = found.size < size ? found.size : size;
    taken_type = found.type;
    text.writable_view().first(taken).copy_from(data.view().subview(found.at + kRecordHead, taken));
    take_out(data, found.at, kRecordHead + found.size);
    --queue->messages;
    queue->bytes -= found.size;
    queue->receive_time = seconds_now(p);
    queue->last_receiver = uint32_t(p.id());
    ipc->sync().notify_all();
    result = taken;
    return true;
  });
  if (outcome != 0) return error(kEintr);
  if (int64_t(result) < 0) return result;
  // (the message is out of the queue by now: a buffer that cannot be written loses it, as on Linux)
  if (!p.write_user_u64(message, taken_type) || (taken != 0 && !p.write_user(message + 8, text.view().first(taken)))) return error(kEfault);
  return result;
}

uint64_t sys_msgctl(Process& p, uint64_t id, uint64_t command, uint64_t buffer) {
  constexpr uint64_t kStat = 11, kInfo = 12, kStatAny = 13;
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  command &= ~uint64_t{0x100};
  Locked locked(ipc->sync());
  if (command == kIpcInfo || command == kInfo) {      // struct msginfo: seven ints and a short
    uint32_t highest = 0, used = 0, messages = 0, bytes = 0;
    for (uint32_t i = 0; i < LinuxIpcManager::kQueues; ++i) {
      if (!ipc->queues[i].used) continue;
      highest = i;
      ++used;
      messages += uint32_t(ipc->queues[i].messages);
      bytes += uint32_t(ipc->queues[i].bytes);
    }
    uint8_t raw[32] = {};
    ByteWriter writer{ByteView(raw, sizeof(raw))};
    writer.le32(command == kInfo ? used : LinuxIpcManager::kQueues * 16);         // msgpool (MSG_INFO: the queues in use)
    writer.le32(command == kInfo ? messages : LinuxIpcManager::kQueueBytes);      // msgmap (MSG_INFO: the messages there are)
    writer.le32(LinuxIpcManager::kMessageMax);                                    // msgmax
    writer.le32(LinuxIpcManager::kQueueBytes);                                    // msgmnb
    writer.le32(LinuxIpcManager::kQueues);                                        // msgmni
    writer.le32(16);                                                              // msgssz
    writer.le32(command == kInfo ? bytes : LinuxIpcManager::kQueueBytes);         // msgtql (MSG_INFO: their bytes)
    writer.le16(0xFFFF);                                                          // msgseg
    if (!p.write_user(buffer, ConstByteView(raw, sizeof(raw)))) return error(kEfault);
    return highest;
  }
  LinuxIpcManager::Queue* queue = nullptr;
  if (command == kStat || command == kStatAny) {
    if (id >= LinuxIpcManager::kQueues || !ipc->queues[id].used) return error(kEinval);
    queue = &ipc->queues[id];
  } else {
    queue = by_id(ipc->queues, id);
    if (!queue) return error(kEinval);
  }
  switch (command) {
    case kIpcStat: case kStat: case kStatAny: {      // struct msqid64_ds
      uint8_t raw[120];
      ByteWriter writer{ByteView(raw, sizeof(raw))};
      write_permission(writer, queue->permission);
      writer.le64(queue->send_time);
      writer.le64(queue->receive_time);
      writer.le64(queue->change_time);
      writer.le64(queue->bytes);
      writer.le64(queue->messages);
      writer.le64(queue->limit);
      writer.le32(queue->last_sender);
      writer.le32(queue->last_receiver);
      writer.zeros(16);
      if (!writer.ok() || !p.write_user(buffer, writer.written())) return error(kEfault);
      return command == kIpcStat ? 0 : make_id(index_of(ipc->queues, queue), queue->permission);
    }
    case kIpcSet: {
      uint64_t limit = 0;
      if (!p.read_user_u64(buffer + 88, &limit)) return error(kEfault);
      if (limit > 64 * LinuxIpcManager::kQueueBytes) return error(kEinval);
      if (!read_permission(p, buffer, &queue->permission)) return error(kEfault);
      if (limit != 0) queue->limit = limit;
      queue->change_time = seconds_now(p);
      ipc->sync().notify_all();
      return 0;
    }
    case kIpcRemove:
      free_queue(*queue);
      ipc->sync().notify_all();      // (whoever waits on it finds it gone: EIDRM)
      return 0;
    default: return error(kEinval);
  }
}

}  // namespace linuxabi
}  // namespace process
