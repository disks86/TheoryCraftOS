#pragma once
#include "hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

constexpr uint16_t kVirtioBlockDevice = 0x1042;             // 0x1040 + device type 2
constexpr uint16_t kVirtioBlockTransitionalDevice = 0x1001;

// A virtio-blk disk (QEMU's virtio-blk-pci), modern virtio 1.x. One request
// queue; each request is a chain of header, data and status byte, and one
// request is in flight at a time.
class VirtioBlockDevice final : public StorageDeviceBase {
 public:
  VirtioBlockDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                    efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), transport_(pci, device, allocator, st) {}

  const char* name() const override { return "virtio-blk"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) override;
  bool flush_cache() override;

 private:
  struct Request {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;  // always in 512-byte units
  };
  bool submit(uint32_t type, uint64_t sector, void* data, uint32_t length);
  uint64_t read_config64(uint32_t offset) const;

  VirtioPciDevice transport_;
  Virtqueue queue_;
  Request* request_ = nullptr;  // header and status byte share one page
  uint8_t* status_ = nullptr;
  bool has_flush_ = false;
  bool started_ = false;
};

}  // namespace hardware
