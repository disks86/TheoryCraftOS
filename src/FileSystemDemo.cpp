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

#include "FileSystemDemo.hpp"
#include "console.hpp"
#include "FileSystem/Fat/FatFileSystem.hpp"
#include "Hardware/StorageDevice/Partition/PartitionScanner.hpp"
#include "Hardware/StorageDevice/Partition/PartitionStorageDevice.hpp"
#include "Utility/AllocatedView.hpp"

using namespace hardware;
using namespace filesystem;

namespace {

void put_mib(uint64_t bytes) {
  console::put_dec(bytes >> 20);
  console::puts(" MiB");
}

// On a scratch volume (a TCOSSCR.TXT at its root) of a device that can discard: 1 MiB written, flushed, deleted and flushed again must
// reach the device as discards (FileSystem/DiscardBatch). The file systems' own host tests check that nothing in use is discarded.
bool check_discard(IFileSystem& fs, IStorageDevice& volume) {
  UniquePointer<IFile> marker;
  if (!volume.info().discard || fs.info().read_only || fs.open_file("/TCOSSCR.TXT", OpenMode::Read, &marker) != Status::Ok) return true;
  marker = UniquePointer<IFile>();
  constexpr uint64_t kBytes = 1 << 20;
  bool ok = true;
  const uint64_t zeroed_before = volume.statistics().blocks_zeroed;
  {
    UniquePointer<IFile> f;
    ok = fs.open_file("/DISCARD.TMP", OpenMode::CreateTruncate, &f) == Status::Ok;
    uint8_t chunk[4096];
    for (uint32_t i = 0; i < sizeof chunk; ++i) chunk[i] = static_cast<uint8_t>(i * 7);
    uint64_t done = 0;
    for (uint64_t at = 0; ok && at < kBytes; at += sizeof chunk) ok = f->write(at, ConstByteView(chunk), &done) == Status::Ok && done == sizeof chunk;
  }
  ok = ok && fs.flush() == Status::Ok;
  if (volume.info().write_zeroes) {   // the new clusters were zeroed by the device (FAT zero_cluster), not by writes
    const uint64_t zeroed = volume.statistics().blocks_zeroed - zeroed_before;
    const bool zero_ok = zeroed * volume.info().block_size >= kBytes / 2;
    console::puts("      new clusters zeroed by the device: ");
    console::put_dec(zeroed);
    console::puts(zero_ok ? " blocks: ok\n" : " blocks: FAILED\n");
    ok = ok && zero_ok;
  }
  // Deleted, then sent without a flush (as the file system service does every few seconds).
  const StorageStatistics before = volume.statistics();
  ok = ok && fs.remove("/DISCARD.TMP") == Status::Ok && fs.send_discards() == Status::Ok;
  const StorageStatistics after = volume.statistics();
  const uint64_t blocks = after.blocks_discarded - before.blocks_discarded;
  ok = ok && blocks * volume.info().block_size >= kBytes / 2;   // (most of it: clusters at the edges of a run may share a device block)
  console::puts("      discard after deleting 1 MiB: ");
  console::put_dec(blocks);
  console::puts(" blocks in ");
  console::put_dec(after.discards - before.discards);
  console::puts(ok ? " requests: ok\n" : " requests: FAILED\n");
  // fstrim: the whole free space, and the marker still reads.
  uint64_t trimmed = 0;
  const Status trim = fs.trim("/", &trimmed);
  const bool marker_ok = fs.open_file("/TCOSSCR.TXT", OpenMode::Read, &marker) == Status::Ok && marker && marker->size() > 0;
  const bool trim_ok = trim == Status::Ok && trimmed >= fs.info().free_bytes / 2 && marker_ok;
  console::puts("      fstrim: ");
  console::put_dec(trimmed >> 20);
  console::puts(trim_ok ? " MiB of free space discarded: ok\n" : " MiB of free space discarded: FAILED\n");
  return ok && trim_ok;
}

// Mounts `volume` as FAT and shows what is on it. False on a device/memory failure.
bool show_volume(IStorageDevice& volume, uint64_t first_block, bool efi_system, allocators::IAllocator& allocator) {
  UniquePointer<IFileSystem> fs;
  const Status mounted = FatFileSystem::mount(allocator, volume, first_block, &fs);
  if (mounted == Status::IoError || mounted == Status::OutOfMemory) {
    console::puts("      mount: ");
    console::puts(status_name(mounted));
    console::puts("\n");
    return false;
  }
  if (mounted != Status::Ok) {
    console::puts("      not FAT (");
    console::puts(status_name(mounted));
    console::puts(")\n");
    return true;
  }
  const FileSystemInfo info = fs->info();
  console::puts("      ");
  console::puts(info.type_name.c_str());
  console::puts(" \"");
  console::puts(info.label.c_str());
  console::puts("\", ");
  put_mib(info.total_bytes);
  console::puts("\n");

  bool ok = true;
  UniquePointer<IDirectory> root;
  if (fs->open_directory("/", &root) == Status::Ok) {
    DirectoryEntry entry;
    uint32_t shown = 0;
    Status s;
    while ((s = root->next(&entry)) == Status::Ok) {
      if (shown++ == 8) {
        console::puts("        ...\n");
        break;
      }
      console::puts(entry.type == NodeType::Directory ? "        [dir] " : "        ");
      console::puts(entry.name.c_str());
      if (entry.type == NodeType::File) {
        console::puts(" (");
        console::put_dec(entry.size);
        console::puts(" B)");
      }
      console::puts("\n");
    }
    if (s != Status::Ok && s != Status::EndOfDirectory) {
      console::puts("        directory read failed: ");
      console::puts(status_name(s));
      console::puts("\n");
      ok = s != Status::IoError;
    }
  }
  root.reset();

  if (efi_system) {
    UniquePointer<IFile> loader;
    if (fs->open_file("/EFI/BOOT/BOOTX64.EFI", OpenMode::Read, &loader) == Status::Ok) {
      ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator, loader->size() ? loader->size() : 1);
      uint64_t got = 0;
      bool matches = !bytes.empty() && loader->read(0, bytes.first(loader->size()), &got) == Status::Ok && got == loader->size();
      // A PE image starts with "MZ".
      const bool pe = matches && got >= 2 && bytes[0] == 'M' && bytes[1] == 'Z';
      console::puts("      /EFI/BOOT/BOOTX64.EFI: ");
      console::put_dec(loader->size());
      console::puts(matches ? (pe ? " B, read back, PE image\n" : " B, read back, not a PE image\n") : " B, READ FAILED\n");
      ok = ok && matches;
      release_view(allocator, bytes);
    }
  }
  if (!efi_system) ok = check_discard(*fs, volume) && ok;
  return ok;
}

}  // namespace

bool run_filesystem_demo(SystemStorageDevices& storage, allocators::IAllocator& allocator) {
  console::puts("Filesystems:\n");
  bool ok = true;
  for (uint32_t d = 0; d < storage.count(); ++d) {
    IStorageDevice& device = *storage.at(d);
    List<PartitionInfo> partitions(allocator);
    PartitionScheme scheme = PartitionScheme::None;
    console::puts("  [");
    console::put_dec(d);
    console::puts("] ");
    console::puts(device.name());
    if (!scan_partitions(device, allocator, &partitions, &scheme)) {
      console::puts(": partition scan FAILED\n");
      ok = false;
      continue;
    }
    if (scheme == PartitionScheme::None) {
      console::puts(": no partition table, whole device\n");
      ok = show_volume(device, 0, false, allocator) && ok;
      continue;
    }
    console::puts(scheme == PartitionScheme::Gpt ? ": GPT, " : ": MBR, ");
    console::put_dec(partitions.count());
    console::puts(" partitions\n");
    for (uint64_t i = 0; i < partitions.count(); ++i) {
      const PartitionInfo& p = partitions[i];
      console::puts("    #");
      console::put_dec(p.index);
      console::puts(" ");
      console::puts(partition_type_name(p));
      if (p.name.size()) {
        console::puts(" \"");
        console::puts(p.name.c_str());
        console::puts("\"");
      }
      console::puts(", blocks ");
      console::put_dec(p.first_block);
      console::puts("..");
      console::put_dec(p.first_block + p.block_count - 1);
      console::puts(" (");
      put_mib(p.block_count * device.info().block_size);
      console::puts(")\n");
      PartitionStorageDevice volume(device, p.first_block, p.block_count, p.index);
      ok = show_volume(volume, 0, is_efi_system_partition(p), allocator) && ok;
    }
  }
  return ok;
}
