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

// The filesystem the desktop's file explorer browses: a mount table whose folders are the volumes of the machine - every FAT volume (a
// partition, or a whole device without a partition table) on the storage devices, named by its label - and, on a machine with none, a
// small filesystem in memory called "Memory" so the explorer still has something to show.
//
// USB mass-storage devices come and go while the machine runs: poll() (or the service thread, start_service()) mounts the volumes of a
// stick that was plugged in and unmounts those of one that was pulled out. Open files and folders on a volume that went away fail with
// Status::NoMedia and the volume is freed when they are closed.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "FileSystem/Mount/MountTable.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/StorageDevice/Partition/PartitionStorageDevice.hpp"
#include "Hardware/StorageDevice/SystemStorageDevices.hpp"
#include "Kernel/Thread.hpp"

class SystemFileSystem {
 public:
  // Told about each change ("USB mass storage added: ..."), from the thread that polls. May be null.
  using Notifier = void (*)(void* context, StringView text);

  // The storage devices must outlive this object.
  SystemFileSystem(allocators::IAllocator& allocator, hardware::SystemStorageDevices& storage);
  ~SystemFileSystem();
  SystemFileSystem(const SystemFileSystem&) = delete;
  SystemFileSystem& operator=(const SystemFileSystem&) = delete;

  // False when nothing could be mounted from storage at start-up and the in-memory fallback is in use.
  bool from_storage() const { return from_storage_; }
  filesystem::IFileSystem& file_system() { return table_; }
  filesystem::MountTable& mounts() { return table_; }

  void set_notifier(Notifier notifier, void* context);

  // Applies the changes SystemStorageDevices::poll() reports; returns how many devices came or went.
  uint32_t poll();

  // Safe removal of the drive that holds the volume called `volume` (a folder of the root): refused while anything is open on it (InUse, unless
  // `force`: then the handles fail with NoMedia from then on), then the device's cache is flushed, all its volumes are unmounted, and the
  // device is stopped - after Ok it can be unplugged. Failed (nothing changed: still mounted) if the device could not be flushed or stopped.
  // The memory volume and anything that is not on a USB stick cannot be ejected. May be called from any thread; it waits for the hot-plug
  // service if that is busy.
  enum class EjectStatus : uint8_t { Ok, NotFound, NotEjectable, InUse, Failed };
  EjectStatus eject(StringView volume, bool force = false);
  // Runs poll() every 250 ms on a thread of its own ("hotplug").
  bool start_service();
  void stop_service();

 private:
  // Mounts every FAT volume on the device; returns how many.
  uint32_t mount_device(uint32_t id, const SharedPointer<hardware::IStorageDevice>& device);
  void mount_fallback();
  void notify(StringView text);
  static void service_main(void* self);

  allocators::IAllocator& allocator_;
  hardware::SystemStorageDevices& storage_;
  kernel::Mutex changes_;   // mounting, unmounting and ejecting happen one at a time
  filesystem::MountTable table_;
  bool from_storage_ = false;
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};
