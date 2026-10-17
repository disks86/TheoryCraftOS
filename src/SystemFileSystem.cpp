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

#include "SystemFileSystem.hpp"
#include "FileSystem/Fat/FatFileSystem.hpp"
#include "Hardware/StorageDevice/Partition/PartitionScanner.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/String.hpp"

using namespace hardware;
using namespace filesystem;

namespace {

// A FAT volume and what it reads through: the device (kept alive by this), the window onto the partition, and the filesystem.
class StorageVolume final : public IVolume {
 public:
  explicit StorageVolume(SharedPointer<IStorageDevice> device) : device_(util::move(device)) {}
  IFileSystem& file_system() override { return *fs_; }
  bool present() const override { return device_->present(); }

  // Mounts the filesystem at `first_block` of the device or of a window onto it.
  bool mount(allocators::IAllocator& allocator, uint64_t first_block, uint64_t block_count, uint32_t index, bool whole) {
    IStorageDevice* target = device_.get();
    if (!whole) {
      partition_ = UniquePointer<PartitionStorageDevice>::make(allocator, *device_, first_block, block_count, index);
      if (!partition_) return false;
      target = partition_.get();
    }
    return FatFileSystem::mount(allocator, *target, 0, &fs_) == Status::Ok;
  }

 private:
  SharedPointer<IStorageDevice> device_;                        // (members are destroyed last to first: the filesystem goes first)
  UniquePointer<PartitionStorageDevice> partition_;
  UniquePointer<IFileSystem> fs_;
};

// The in-memory fallback.
class MemoryVolume final : public IVolume {
 public:
  explicit MemoryVolume(allocators::IAllocator& allocator) : fs_(allocator) {}
  IFileSystem& file_system() override { return fs_; }
  RamFileSystem& ram() { return fs_; }

 private:
  RamFileSystem fs_;
};

}  // namespace

SystemFileSystem::SystemFileSystem(allocators::IAllocator& allocator, SystemStorageDevices& storage)
    : allocator_(allocator), storage_(storage), table_(allocator) {
  for (uint32_t d = 0; d < storage.count(); ++d) mount_device(storage.id(d), storage.share(d));
  from_storage_ = table_.count() > 0;
  if (!from_storage_) mount_fallback();
}

SystemFileSystem::~SystemFileSystem() { stop_service(); }

void SystemFileSystem::set_notifier(Notifier notifier, void* context) {
  notifier_ = notifier;
  notifier_context_ = context;
}

void SystemFileSystem::notify(StringView text) {
  if (notifier_) notifier_(notifier_context_, text);
}

uint32_t SystemFileSystem::mount_device(uint32_t id, const SharedPointer<IStorageDevice>& device) {
  if (!device) return 0;
  List<PartitionInfo> partitions(allocator_);
  PartitionScheme scheme = PartitionScheme::None;
  if (!scan_partitions(*device, allocator_, &partitions, &scheme)) return 0;
  uint32_t mounted = 0;
  const uint64_t windows = scheme == PartitionScheme::None ? 1 : partitions.count();
  for (uint64_t i = 0; i < windows; ++i) {
    const bool whole = scheme == PartitionScheme::None;
    SharedPointer<StorageVolume> volume = SharedPointer<StorageVolume>::make(allocator_, device);
    if (!volume) break;
    if (!volume->mount(allocator_, whole ? 0 : partitions[i].first_block, whole ? 0 : partitions[i].block_count, whole ? 0 : partitions[i].index, whole))
      continue;
    const FileSystemInfo info = volume->file_system().info();
    String name(allocator_);
    if (!info.label.view().trimmed().empty()) name.assign(info.label.view().trimmed());
    else name.assign(device->info().removable ? "Removable disk" : "Disk");
    FixedString<MountTable::kMaxName + 1> assigned;
    if (table_.mount(name.view(), SharedPointer<IVolume>(volume), id, &assigned)) ++mounted;
  }
  return mounted;
}

void SystemFileSystem::mount_fallback() {
  static const char kWelcome[] = "TheoryCraftOS found no FAT volume on this machine.\nThis folder lives in memory.\n";
  SharedPointer<MemoryVolume> volume = SharedPointer<MemoryVolume>::make(allocator_, allocator_);
  if (!volume) return;
  RamFileSystem& ram = volume->ram();
  ram.create_directory("/Documents");
  ram.create_directory("/Programs");
  UniquePointer<IFile> file;
  uint64_t written = 0;
  if (ram.open_file("/Welcome.txt", OpenMode::CreateTruncate, &file) == Status::Ok)
    file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(kWelcome), sizeof kWelcome - 1), &written);
  file.reset();
  table_.mount("Memory", SharedPointer<IVolume>(volume), 0);
}

uint32_t SystemFileSystem::poll() {
  kernel::ScopedMutex guard(changes_);
  uint32_t changes = 0;
  StorageEvent event;
  while (storage_.poll(&event)) {
    ++changes;
    String text(allocator_);
    if (event.kind == StorageEvent::Kind::Added) {
      const uint32_t mounted = mount_device(event.id, event.device);
      text.assign(event.device->name());
      text.append(mounted ? ": mounted" : ": no FAT volume to mount");
      if (mounted > 1) {
        text.append(" (");
        text.append_uint(mounted);
        text.append(" volumes)");
      }
    } else {
      table_.unmount_owner(event.id);
      text.assign(event.device->name());
      text.append(event.ejected ? ": ejected, safe to remove" : ": removed");
    }
    notify(text.view());
  }
  return changes;
}

SystemFileSystem::EjectStatus SystemFileSystem::eject(StringView volume, bool force) {
  kernel::ScopedMutex guard(changes_);
  uint32_t owner = 0;
  if (!table_.owner_of(volume, &owner)) return EjectStatus::NotFound;
  if (owner == 0 || !storage_.can_eject(owner)) return EjectStatus::NotEjectable;
  if (!force && table_.in_use(owner)) return EjectStatus::InUse;
  table_.flush();   // (the volumes first, then the device under them)
  // The device flushes its cache and stops; if that fails nothing has changed. From then on it is not present, so whatever is still open on its
  // volumes (only with `force`) answers NoMedia, and the volumes can be unmounted.
  const EjectResult result = storage_.eject(owner);
  if (result == EjectResult::Failed) return EjectStatus::Failed;
  table_.unmount_owner(owner);   // (the device's Removed event comes through poll() as usual)
  return result == EjectResult::NotSupported ? EjectStatus::NotEjectable : EjectStatus::Ok;   // (NotFound: it vanished meanwhile, as good as ejected)
}

bool SystemFileSystem::start_service() {
  if (service_) return true;
  stop_ = false;
  service_ = kernel::spawn(service_main, this, "hotplug");
  return service_ != nullptr;
}

void SystemFileSystem::stop_service() {
  if (!service_) return;
  __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
  kernel::join(service_);
  service_ = nullptr;
}

void SystemFileSystem::service_main(void* self) {
  SystemFileSystem* files = static_cast<SystemFileSystem*>(self);
  while (!__atomic_load_n(&files->stop_, __ATOMIC_ACQUIRE)) {
    files->poll();
    kernel::sleep_us(250000);
  }
}
