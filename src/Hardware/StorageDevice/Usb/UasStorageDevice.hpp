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
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/StorageDevice/QueuedStorage.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "Hardware/StorageDevice/Usb/UasProtocol.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A USB disk spoken to with USB Attached SCSI (UAS, Usb/UasProtocol.hpp; QEMU's usb-uas, most USB 3 SSD enclosures). On SuperSpeed, with
// bulk streams and the controller's asynchronous transfers (xHCI), up to kMaxTags commands are on the device at once: each tag has a
// stream and its own command, status and 64 KiB data buffers, and a request queues its status read and data transfer on its stream, then
// sends the Command IU, and waits for all three. Big requests go as several chunks at once (QueuedStorage run_chunks); queue_depth tells
// the I/O scheduler the tag count. Without streams (a USB 2 port), one command at a time, following the device's Read Ready / Write
// Ready. A device that fails a transfer is given up (requests fail; unplugging it ends that).
class UasStorageDevice final : public StorageDeviceBase {
 public:
  static constexpr uint32_t kMaxTags = 16;
  static constexpr uint32_t kSlotBytes = 64 * 1024;

  UasStorageDevice(IUsbHostController& host, uint32_t port, const uas::UasInterface& interface, uint32_t streams,
                   allocators::IAllocator& allocator, efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), host_(host), port_(port), interface_(interface), streams_(streams),
        generation_(host.port_generation(port)) {}

  // UAS for the device on `port` if it has a UAS interface: selects the configuration and the alternate setting, starts the pipes (with
  // streams where the controller and the device have them) and brings the disk up. False - with the interface put back to alternate
  // setting 0, so Bulk-Only can be tried - if it has none or any of that fails.
  static bool attach(IUsbHostController& host, uint32_t port, uint8_t configuration, ConstByteView descriptors, allocators::IAllocator& allocator,
                     efi::SystemTable* st, List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return "USB disk (UAS)"; }
  bool initialize() override;
  void shutdown() override;
  bool present() const override;
  void describe(IPropertySink& sink) const override;

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  bool discard(uint64_t lba, uint64_t blocks) override;
  bool write_zeroes(uint64_t lba, uint64_t blocks) override;
  StorageStatistics statistics() const override;

  uint32_t tag_count() const { return tag_count_; }
  bool uses_streams() const { return streams_ != 0; }

 protected:
  bool transfer(bool, uint64_t, uint32_t, ByteView) override { return false; }   // (read/write above do the work)

 private:
  enum class Result : uint8_t { Good, CheckCondition, Error };
  struct Tag {
    ByteView command;          // the Command IU
    ByteView status;           // what the status pipe answers
    ByteView data;             // kSlotBytes, in one 64 KiB window
    uint32_t tokens[3] = {};   // status, data, command (submit_bulk)
    bool pending[3] = {};
    uint8_t sense_key = 0;
    uint8_t serial_result = 0;   // without streams: what the exchange done by start() gave (a Result), for finish
  };

  // Streams: queues the command on tag `t` (its data, `bytes`, already in the tag's buffer for a write), and collects it.
  bool submit(uint32_t t, ConstByteView cdb, uint64_t bytes, bool data_in);
  Result finish(uint32_t t);
  // Without streams: the whole exchange, one command at a time (under mutex_).
  Result exchange_serial(uint32_t t, ConstByteView cdb, uint64_t bytes, bool data_in);
  // A small command through tag `t`'s buffer (`data` at most kSlotBytes), retried while the device reports UNIT ATTENTION or NOT READY.
  bool run(ConstByteView cdb, ByteView data, bool data_in);
  Result command(uint32_t t, ConstByteView cdb, ByteView data, bool data_in);
  bool run_blocks(bool write, uint64_t lba, ByteView into, ConstByteView from);
  // The command on tag `t` with its data already in (or going to) the tag's buffer: queued (streams) or done at once (without; the
  // result kept for collect()). False if it could not be started.
  bool start(uint32_t t, ConstByteView cdb, uint64_t bytes, bool data_in);
  Result collect(uint32_t t);
  Result interpret(uint32_t t);

  IUsbHostController& host_;
  uint32_t port_;
  uas::UasInterface interface_;
  uint32_t streams_;               // per pipe; 0: no streams (one command at a time)
  uint32_t generation_;
  Tag tags_[kMaxTags];
  uint32_t tag_count_ = 0;
  SlotPool pool_;
  ByteView data_memory_, small_memory_;
  bool broken_ = false;            // atomic: a transfer failed or never ended; everything fails from now on
  bool shut_down_ = false;         // atomic
  scsi::UnmapSupport unmap_;
  uint64_t write_same_max_ = 0;
  bool write_same_refused_ = false;   // atomic
};

}  // namespace hardware
