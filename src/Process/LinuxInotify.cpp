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
#include "Process/LinuxFiles.hpp"
#include "Process/LinuxIpc.hpp"
#include "Process/LinuxSockets.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

// inotify for Linux programs. An instance (inotify_init) is a "linked" descriptor of the AF_UNIX manager; a watch (inotify_add_watch) is a change
// notification of the object manager on the watched folder's host path, with its records kept. The Linux file calls (LinuxFiles.cpp) report what they
// create, remove, rename and write; reading the descriptor turns the records into struct inotify_event.
//
// What a watcher is told: IN_CREATE, IN_DELETE, IN_MODIFY, IN_MOVED_FROM / IN_MOVED_TO (with a cookie), IN_DELETE_SELF, IN_MOVE_SELF (a watched file),
// IN_IGNORED, IN_Q_OVERFLOW, and IN_ISDIR with them. Not told: IN_ACCESS, IN_OPEN, IN_CLOSE_WRITE, IN_CLOSE_NOWRITE, IN_ATTRIB (nothing reports those),
// and changes made by Windows programs or by the system itself to the same files.
namespace process::linuxabi {

namespace {

constexpr int64_t kEnoent = 2, kEbadf = 9, kEagain = 11, kEnomem = 12, kEfault = 14, kEexist = 17, kEnotdir = 20, kEinval = 22, kEmfile = 24, kEnospc = 28, kEintr = 4,
                  kEnosys = 38, kEnametoolong = 36;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint32_t kInModify = 0x2, kInMovedFrom = 0x40, kInMovedTo = 0x80, kInCreate = 0x100, kInDelete = 0x200, kInDeleteSelf = 0x400, kInMoveSelf = 0x800,
                   kInAllEvents = 0xFFF, kInQOverflow = 0x4000, kInIgnored = 0x8000, kInOnlyDir = 0x01000000, kInDontFollow = 0x02000000,
                   kInExclUnlink = 0x04000000, kInMaskCreate = 0x10000000, kInMaskAdd = 0x20000000, kInIsDir = 0x40000000, kInOneShot = 0x80000000;
constexpr uint32_t kIdStep = 32768;
constexpr uint32_t kEveryChange = 0x17F;      // the object manager's FILE_NOTIFY_CHANGE_* bits: every kind of change

uint64_t link_of(const LinuxIpcManager& ipc, const LinuxIpcManager::InotifyInstance* instance) {
  return uint64_t(instance - &ipc.inotify_instances[0]) + uint64_t{instance->sequence} * kIdStep;
}

LinuxIpcManager::InotifyInstance* instance_of(LinuxIpcManager& ipc, uint64_t link) {
  const uint64_t index = link % kIdStep, sequence = link / kIdStep;
  if (index >= LinuxIpcManager::kInotifyInstances || !ipc.inotify_instances[index].used || ipc.inotify_instances[index].sequence != sequence) return nullptr;
  return &ipc.inotify_instances[index];
}

// One struct inotify_event at the end of the instance's buffer: wd, mask, cookie, len (4 bytes each), then the name padded with zeros to a multiple of 16.
void queue_event(LinuxIpcManager::InotifyInstance& instance, int32_t descriptor, uint32_t mask, uint32_t cookie, StringView name) {
  ByteBuffer& pending = *instance.pending;
  const uint64_t padded = name.empty() ? 0 : (name.size() + 1 + 15) & ~uint64_t{15};
  if (pending.size() + 16 + padded > LinuxIpcManager::kInotifyPendingBytes - 16) {      // (room is kept for the event that says so)
    instance.overflowed = true;
    return;
  }
  const uint64_t before = pending.size();
  if (!pending.put32(uint32_t(descriptor)) || !pending.put32(mask) || !pending.put32(cookie) || !pending.put32(uint32_t(padded)) || !pending.append(name.bytes()) ||
      !pending.put_zeros(padded - name.size())) {
    pending.truncate(before);
    instance.overflowed = true;
  }
}

void drop_watch_locked(LinuxIpcManager::InotifyWatch& watch) {
  if (ObjectManager* objects = system_objects()) objects->release_locked(watch.object);
  watch = LinuxIpcManager::InotifyWatch();
}

// Everything the instance's watches have seen since the last time, as events at the end of its buffer (the system's lock held).
void collect_locked(LinuxIpcManager& ipc, LinuxIpcManager::InotifyInstance& instance, uint64_t link) {
  ObjectManager* objects = system_objects();
  if (!objects) return;
  for (LinuxIpcManager::InotifyWatch& watch : ipc.inotify_watches) {
    if (!watch.used || watch.instance != link) continue;
    ObjectManager::WatchRecord records[ObjectManager::kMaxWatchRecords];
    uint32_t count = 0;
    bool overflow = false, fired = false;
    if (!objects->drain_watch_locked(watch.object, records, ObjectManager::kMaxWatchRecords, &count, &overflow, &fired)) continue;
    if (overflow) instance.overflowed = true;
    bool gone = fired && count == 0 && !overflow && !watch.on_file;      // (fired with nothing to show: the watched folder itself was removed)
    bool told = false;
    for (uint32_t i = 0; i < count && !gone; ++i) {
      const ObjectManager::WatchRecord& record = records[i];
      const StringView name(record.name, record.name_size);
      uint32_t event = 0;
      uint32_t cookie = 0;
      if (watch.on_file) {      // only what happens to the one file, told without a name
        if (name != StringView(watch.name, watch.name_size)) continue;
        if (record.action == 3) event = kInModify;
        else if (record.action == 2 && record.rename == 0) { event = kInDeleteSelf; gone = true; }
        else if (record.action == 4 || (record.action == 2 && record.rename != 0)) event = kInMoveSelf;      // (moved within its folder, or out of it)
        if (event & watch.mask) { queue_event(instance, watch.descriptor, event, 0, StringView()); told = true; }
        continue;
      }
      // (A rename out of a watched folder is told to its watch as a removal, one into it as an addition: here they are the two halves of a move, tied
      // together by the rename's number as the two names of a rename inside one folder are.)
      switch (record.action) {
        case 1: event = record.rename != 0 ? kInMovedTo : kInCreate; break;
        case 2: event = record.rename != 0 ? kInMovedFrom : kInDelete; break;
        case 3: event = kInModify; break;
        case 4: event = kInMovedFrom; break;
        case 5: event = kInMovedTo; break;
        default: break;
      }
      cookie = record.rename;
      if (!(event & watch.mask)) continue;
      queue_event(instance, watch.descriptor, event | (record.directory ? kInIsDir : 0), cookie, name);
      told = true;
      if (watch.mask & kInOneShot) break;      // (one event, and the watch is over)
    }
    if (gone && !watch.on_file && (watch.mask & kInDeleteSelf)) queue_event(instance, watch.descriptor, kInDeleteSelf, 0, StringView());
    if (gone || (told && (watch.mask & kInOneShot))) {
      queue_event(instance, watch.descriptor, kInIgnored, 0, StringView());
      drop_watch_locked(watch);
    }
  }
  if (instance.overflowed && instance.pending->size() + 16 <= LinuxIpcManager::kInotifyPendingBytes) {
    ByteBuffer& pending = *instance.pending;
    if (pending.put32(0xFFFFFFFFu) && pending.put32(kInQOverflow) && pending.put32(0) && pending.put32(0)) instance.overflowed = false;
  }
}

// The last descriptor of an instance was closed (called by the descriptor manager with the system's lock held).
void instance_gone(uint64_t link, uint32_t) {
  LinuxIpcManager* ipc = existing_linux_ipc();
  if (!ipc) return;
  LinuxIpcManager::InotifyInstance* instance = instance_of(*ipc, link);
  if (!instance) return;
  for (LinuxIpcManager::InotifyWatch& watch : ipc->inotify_watches)
    if (watch.used && watch.instance == link) drop_watch_locked(watch);
  const uint16_t sequence = uint16_t((instance->sequence + 1) % 0x7FFF);
  *instance = LinuxIpcManager::InotifyInstance();
  instance->sequence = sequence;
}

struct Locked {
  ISystemSync& sync;
  explicit Locked(ISystemSync& s) : sync(s) { sync.lock(); }
  ~Locked() { sync.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};

}  // namespace

bool inotify_ready_locked(uint64_t link) {
  LinuxIpcManager* ipc = existing_linux_ipc();
  ObjectManager* objects = system_objects();
  if (!ipc) return false;
  const LinuxIpcManager::InotifyInstance* instance = instance_of(*ipc, link);
  if (!instance) return false;
  if (instance->pending->size() != 0 || instance->overflowed) return true;
  if (!objects) return false;
  for (const LinuxIpcManager::InotifyWatch& watch : ipc->inotify_watches)
    if (watch.used && watch.instance == link && objects->watch_has_news_locked(watch.object)) return true;
  return false;
}

// inotify_init1(flags) (inotify_init() is inotify_init1(0)): IN_NONBLOCK and IN_CLOEXEC are O_NONBLOCK and O_CLOEXEC.
uint64_t sys_inotify_init1(Process& p, uint64_t flags) {
  constexpr uint64_t kNonBlock = 0x800, kCloseOnExec = 0x80000;
  if (flags & ~(kNonBlock | kCloseOnExec)) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc || !system_objects()) return error(kEnosys);
  UnixSocketManager::set_link_release_hook(UnixType::Inotify, instance_gone);
  uint64_t link = 0;
  {
    Locked locked(ipc->sync());
    LinuxIpcManager::InotifyInstance* instance = nullptr;
    for (LinuxIpcManager::InotifyInstance& candidate : ipc->inotify_instances)
      if (!candidate.used) { instance = &candidate; break; }
    if (!instance) return error(kEmfile);
    UniquePointer<ByteBuffer> pending = UniquePointer<ByteBuffer>::make(ipc->allocator(), ipc->allocator());
    if (!pending) return error(kEnomem);
    const uint16_t sequence = instance->sequence;
    *instance = LinuxIpcManager::InotifyInstance();
    instance->sequence = sequence;
    instance->used = true;
    instance->pending = static_cast<UniquePointer<ByteBuffer>&&>(pending);
    link = link_of(*ipc, instance);
  }
  const uint64_t fd = open_linked(p, UnixType::Inotify, link, 0, (flags & kCloseOnExec) != 0, (flags & kNonBlock) != 0);
  if (int64_t(fd) < 0 && int64_t(fd) != -kEmfile) {      // (EMFILE: the descriptor was made and let go again, which freed the instance)
    Locked locked(ipc->sync());
    instance_gone(link, 0);
  }
  return fd;
}

uint64_t sys_inotify_add_watch(Process& p, uint64_t fd, uint64_t path, uint64_t mask_argument) {
  const uint32_t mask = uint32_t(mask_argument);
  LinuxIpcManager* ipc = system_linux_ipc();
  ObjectManager* objects = system_objects();
  if (!ipc || !objects) return error(kEnosys);
  uint64_t link = 0;
  uint32_t mode = 0;
  if (linked_of(p, fd, UnixType::Inotify, &link, &mode) != 0) return error(p.files().kind(fd) == DescriptorKind::Closed ? kEbadf : kEinval);
  if ((mask & kInAllEvents) == 0 || (mask & ~(kInAllEvents | kInOnlyDir | kInDontFollow | kInExclUnlink | kInMaskCreate | kInMaskAdd | kInOneShot)) != 0) return error(kEinval);
  if ((mask & kInMaskCreate) && (mask & kInMaskAdd)) return error(kEinval);
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator()), host(files.allocator());
  if (const int64_t r = resolve_user_path(p, ~uint64_t{99}, path, false, &virtual_path); r < 0) return error(-r);
  filesystem::DirectoryEntry entry;
  if (virtual_path.view() != StringView("/") && files.stat(virtual_path.view(), &entry) != filesystem::Status::Ok) return error(kEnoent);
  const bool directory = virtual_path.view() == StringView("/") || entry.type == filesystem::NodeType::Directory;
  if (!directory && (mask & kInOnlyDir)) return error(kEnotdir);
  if (!files.host_path_of(virtual_path.view(), &host) || host.size() >= LinuxIpcManager::kInotifyPathBytes) return error(kEnametoolong);
  // A file is watched through its folder: the last name of the path is kept, and the folder is what the change notification is on.
  StringView watched = host.view(), name;
  if (!directory) {
    uint64_t slash = watched.size();
    while (slash > 0 && watched[slash - 1] != '/') --slash;
    name = watched.without_prefix(slash);
    watched = slash > 1 ? watched.first(slash - 1) : StringView("/");
    if (name.empty() || name.size() >= LinuxIpcManager::kInotifyNameBytes) return error(kEnametoolong);
  }
  const uint32_t kept = mask & (kInAllEvents | kInOneShot);
  {
    Locked locked(ipc->sync());
    LinuxIpcManager::InotifyInstance* instance = instance_of(*ipc, link);
    if (!instance) return error(kEbadf);
    for (LinuxIpcManager::InotifyWatch& watch : ipc->inotify_watches) {      // the same thing again: its watch, with the new mask (or the two together)
      if (!watch.used || watch.instance != link || StringView(watch.path, watch.path_size) != host.view()) continue;
      if (mask & kInMaskCreate) return error(kEexist);
      watch.mask = (mask & kInMaskAdd) ? (watch.mask | kept) : kept;
      return uint64_t(watch.descriptor);
    }
  }
  // (the notification is made without the lock: the object manager takes it itself)
  const ObjectManager::Created made = objects->create_change_notification(watched, false, kEveryChange);
  if (made.id == 0) return error(kEnospc);
  objects->watch_set_want_data(made.id, true);
  Locked locked(ipc->sync());
  LinuxIpcManager::InotifyInstance* instance = instance_of(*ipc, link);
  LinuxIpcManager::InotifyWatch* slot = nullptr;
  for (LinuxIpcManager::InotifyWatch& candidate : ipc->inotify_watches)
    if (!candidate.used) { slot = &candidate; break; }
  if (!instance || !slot) {
    objects->release_locked(made.id);
    return error(instance ? kEnospc : kEbadf);
  }
  *slot = LinuxIpcManager::InotifyWatch();
  slot->used = true;
  slot->instance = uint32_t(link);
  slot->descriptor = instance->next_descriptor;
  instance->next_descriptor = instance->next_descriptor == 0x7FFFFFFF ? 1 : instance->next_descriptor + 1;
  slot->object = made.id;
  slot->mask = kept;
  slot->on_file = !directory;
  for (uint64_t i = 0; i < host.size(); ++i) slot->path[i] = host.view()[i];
  slot->path_size = uint32_t(host.size());
  for (uint64_t i = 0; i < name.size(); ++i) slot->name[i] = name[i];
  slot->name_size = uint32_t(name.size());
  return uint64_t(slot->descriptor);
}

uint64_t sys_inotify_rm_watch(Process& p, uint64_t fd, uint64_t descriptor) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnosys);
  uint64_t link = 0;
  uint32_t mode = 0;
  if (linked_of(p, fd, UnixType::Inotify, &link, &mode) != 0) return error(p.files().kind(fd) == DescriptorKind::Closed ? kEbadf : kEinval);
  Locked locked(ipc->sync());
  LinuxIpcManager::InotifyInstance* instance = instance_of(*ipc, link);
  if (!instance) return error(kEbadf);
  for (LinuxIpcManager::InotifyWatch& watch : ipc->inotify_watches) {
    if (!watch.used || watch.instance != link || watch.descriptor != int32_t(descriptor)) continue;
    collect_locked(*ipc, *instance, link);      // (what it saw until now is still told)
    if (!watch.used) return 0;                  // (that told of its end already)
    queue_event(*instance, watch.descriptor, kInIgnored, 0, StringView());
    drop_watch_locked(watch);
    ipc->sync().notify_all();
    return 0;
  }
  return error(kEinval);
}

uint64_t inotify_read(Process& p, uint64_t link, uint64_t buffer, uint64_t count, bool non_blocking) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  ByteBuffer out(ipc->allocator());
  uint64_t result = 0;
  const uint32_t outcome = system_wait(p, ipc->sync(), non_blocking ? 0 : ~uint64_t{0}, [&]() -> bool {
    LinuxIpcManager::InotifyInstance* instance = instance_of(*ipc, link);
    if (!instance) { result = error(kEbadf); return true; }
    collect_locked(*ipc, *instance, link);
    ByteBuffer& pending = *instance->pending;
    if (pending.size() == 0) return false;
    // Whole events, as many as fit. Not even the first: EINVAL, as Linux answers a buffer that is too small.
    uint64_t taken = 0;
    const ConstByteView bytes = pending.view();
    while (taken + 16 <= bytes.size()) {
      ByteReader reader{bytes.subview(taken + 12, 4)};
      const uint64_t size = 16 + uint64_t{reader.le32()};
      if (size > bytes.size() - taken || taken + size > count) break;
      taken += size;
    }
    if (taken == 0) { result = error(kEinval); return true; }
    out.clear();
    if (!out.append(bytes.first(taken))) { result = error(kEnomem); return true; }
    ByteView all = pending.writable_view();
    for (uint64_t i = 0; i + taken < all.size(); ++i) all[i] = all[i + taken];
    pending.truncate(all.size() - taken);
    result = taken;
    return true;
  });
  if (outcome == 1) return error(kEagain);
  if (outcome == 2) return error(kEintr);
  if (int64_t(result) < 0) return result;
  return p.write_user(buffer, out.view()) ? result : error(kEfault);
}

}  // namespace process::linuxabi
