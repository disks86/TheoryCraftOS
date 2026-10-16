#pragma once
#include "hardware/Bus/Usb/IUsbBus.hpp"
#include "hardware/StorageDevice/StorageDeviceBase.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A USB mass-storage device (QEMU's usb-storage) speaking the Bulk-Only
// Transport: every SCSI command is a 31-byte command block wrapper out, an
// optional data stage, and a 13-byte status wrapper in. Runs on any host
// controller that offers bulk endpoints (xHCI here).
class UsbMassStorageDevice final : public StorageDeviceBase {
 public:
  UsbMassStorageDevice(IUsbHostController& host, uint32_t port, uint8_t interface_number, uint8_t in_endpoint,
                       uint16_t in_max_packet, uint8_t out_endpoint, uint16_t out_max_packet,
                       allocators::IAllocator& allocator, efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), host_(host), port_(port), interface_(interface_number),
        in_(in_endpoint), out_(out_endpoint), in_max_packet_(in_max_packet), out_max_packet_(out_max_packet) {}

  // Adds a device for every mass-storage interface on every enumerated USB device.
  static void discover(IUsbBus& usb, allocators::IAllocator& allocator, efi::SystemTable* st,
                       List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return optical_ ? "USB optical drive" : "USB mass storage"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) override;
  bool flush_cache() override;

 private:
  struct Cbw {
    uint32_t signature, tag, data_length;
    uint8_t flags, lun, cb_length;
    uint8_t cb[16];
  } __attribute__((packed));
  struct Csw {
    uint32_t signature, tag, residue;
    uint8_t status;
  } __attribute__((packed));

  enum class Result { Good, Failed, Error };  // command failed (sense available) / transport broke
  Result command(const uint8_t* cdb, uint32_t cdb_length, void* data, uint32_t length, bool data_in);
  bool run(const uint8_t* cdb, uint32_t cdb_length, void* data, uint32_t length, bool data_in);
  void recover();

  IUsbHostController& host_;
  uint32_t port_;
  uint8_t interface_, in_, out_;
  uint16_t in_max_packet_, out_max_packet_;
  uint8_t* wire_ = nullptr;  // command and status wrappers, in a 64 KiB-aligned page
  uint32_t tag_ = 1;
  bool optical_ = false;
};

}  // namespace hardware
