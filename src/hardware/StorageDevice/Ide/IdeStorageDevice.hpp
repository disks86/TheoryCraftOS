#pragma once
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/StorageDevice/StorageDeviceBase.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A legacy parallel-ATA (IDE) controller, the disk controller of QEMU's i440fx
// "pc" machine (piix3-ide / piix4-ide). Owns the PCI function; the drives on its
// two channels are IdeStorageDevices that share it.
class IdeController {
 public:
  struct Channel {
    uint16_t command_base = 0;  // eight task-file registers
    uint16_t control_base = 0;  // alternate status / device control
  };

  IdeController(const IPciBus& pci, const PciDeviceInfo& device, efi::SystemTable* st)
      : pci_(pci), device_(device), st_(st) {}

  // PCI class 01, subclass 01 (any programming interface).
  static bool supports(const PciDeviceInfo& d) { return d.class_code == 0x01 && d.subclass == 0x01; }

  bool initialize();
  void shutdown();  // safe to repeat

  Channel channel(uint32_t index) const { return channels_[index]; }
  efi::SystemTable* system_table() const { return st_; }

 private:
  uint16_t io_base(uint8_t bar_index, uint16_t legacy, bool native) const;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  efi::SystemTable* st_;
  Channel channels_[2];
  bool owns_device_ = false;
};

// One ATA disk or ATAPI drive on an IDE channel, moved with programmed I/O
// (the data goes through the CPU word by word), polled, interrupts masked.
class IdeStorageDevice final : public StorageDeviceBase {
 public:
  IdeStorageDevice(SharedPointer<IdeController> controller, uint32_t channel, uint32_t drive,
                   allocators::IAllocator& allocator)
      : StorageDeviceBase(allocator, controller->system_table()),
        controller_(util::move(controller)), channel_(controller_->channel(channel)), drive_(drive) {}

  // Probes both channels of a controller and adds the drives that answer.
  static void discover(const SharedPointer<IdeController>& controller, allocators::IAllocator& allocator,
                       List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return atapi_ ? "IDE optical drive" : "IDE disk"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) override;
  bool flush_cache() override;

 private:
  void release_resources() { release_bounce(); }
  uint8_t reg_read(uint32_t reg) const;
  void reg_write(uint32_t reg, uint8_t value) const;
  uint8_t status() const;
  void select(uint8_t lba_high_bits) const;
  bool wait_not_busy(uint32_t timeout_ms);
  bool wait_data_ready();
  bool identify(uint8_t command, uint16_t* data);
  bool atapi_command(const uint8_t* cdb, void* data, uint32_t bytes);

  SharedPointer<IdeController> controller_;
  IdeController::Channel channel_;
  uint32_t drive_;
  bool atapi_ = false;
  bool lba48_ = false;
};

}  // namespace hardware
