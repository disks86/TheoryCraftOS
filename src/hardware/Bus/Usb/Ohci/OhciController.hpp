#pragma once
#include "hardware/Bus/Usb/Legacy/LegacyHostController.hpp"

namespace hardware {

// Polling OHCI driver (USB 1.1, Compaq/Microsoft's design: memory-mapped
// registers, endpoint descriptors in linked lists, transfer descriptors queued on
// each endpoint). Control and bulk endpoints live in the control and bulk lists,
// interrupt endpoints in the periodic list, polled every frame. Each endpoint
// keeps a spare "dummy" transfer descriptor at its queue's tail, so new work is
// published by moving one pointer. All structures and data buffers must sit
// below 4 GiB; buffers above that are bounced.
//
// Short IN packets: a multi-descriptor IN transfer that ends early (the device
// sends a short packet before the requested length) is not detected and times
// out; the lengths this OS asks for are exact.
class OhciController final : public LegacyHostController {
 public:
  static constexpr uint32_t kMaxPorts = 15;

  OhciController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                 efi::SystemTable* st)
      : LegacyHostController(pci, device, allocator, st) {}

  const char* name() const override { return "OHCI controller"; }
  bool initialize() override;
  void shutdown() override;

  UsbControllerKind kind() const override { return UsbControllerKind::Ohci; }
  uint32_t port_count() const override { return ports_; }
  bool port_info(uint32_t port, UsbPortInfo* out) const override;
  bool start_interrupt_in(uint32_t port, uint8_t endpoint_address, uint16_t max_packet, uint8_t interval) override;
  bool poll_interrupt_in(uint32_t port, uint8_t endpoint_address, void* buffer, uint16_t capacity,
                         uint16_t* length) override;
  bool start_bulk_endpoint(uint32_t port, uint8_t endpoint_address, uint16_t max_packet) override;
  bool bulk_transfer(uint32_t port, uint8_t endpoint_address, void* data, uint32_t length,
                     uint32_t* transferred) override;
  bool reset_bulk_endpoint(uint32_t port, uint8_t endpoint_address) override;

 protected:
  DeviceState* state(uint32_t port) const override;
  bool control_transfer(uint32_t port, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                        void* data, uint16_t length, bool data_in) override;
  bool open_control_pipe(uint32_t port) override;
  void update_control_pipe(uint32_t port, uint8_t address, uint16_t max_packet) override;
  void close_device(uint32_t port) override;

 private:
  struct alignas(16) Ed {
    uint32_t flags;  // address 6:0, endpoint 10:7, direction 12:11, low speed 13, skip 14, max packet 26:16
    uint32_t tail;   // address of the dummy descriptor that ends the queue
    uint32_t head;   // next descriptor to run; bit 0 halted, bit 1 toggle carry
    uint32_t next;   // next endpoint in the list
  };
  struct alignas(16) Td {
    uint32_t flags;  // rounding 18, direction 20:19, delay interrupt 23:21, toggle 25:24, condition code 31:28
    uint32_t current_buffer;
    uint32_t next;
    uint32_t buffer_end;
  };
  struct Hcca {
    uint32_t interrupt_table[32];
    uint16_t frame_number, pad;
    uint32_t done_head;
    uint8_t reserved[120];
  };
  static_assert(sizeof(Hcca) == 256);
  enum class List : uint8_t { Control, Bulk, Interrupt };

  struct Pipe {
    bool active = false;
    List list = List::Control;
    uint8_t address = 0;  // USB endpoint address
    uint16_t max_packet = 8;
    uint8_t dummy = 0;    // which spare is the queue's current dummy
    Ed* ed = nullptr;
    Td* spare[2] = {};
    Td* armed = nullptr;  // interrupt pipes: the descriptor of the outstanding transfer
    uint8_t* buffer = nullptr;
  };
  struct Device : DeviceState {
    static constexpr uint32_t kPipes = 6;
    Pipe control;
    Pipe pipes[kPipes];
  };

  static constexpr uint32_t kPoolTds = 32;

  uint32_t reg(uint32_t offset) const { return *reinterpret_cast<volatile uint32_t*>(regs_ + offset); }
  void set_reg(uint32_t offset, uint32_t value) const { *reinterpret_cast<volatile uint32_t*>(regs_ + offset) = value; }
  uint32_t port_status(uint32_t port) const { return reg(0x54 + 4 * port); }
  void set_port(uint32_t port, uint32_t value) const { set_reg(0x54 + 4 * port, value); }
  bool reset_port(uint32_t port, bool* low_speed);

  void* low_allocate(uint64_t size, uint64_t alignment);
  bool create_pipe(Device& d, Pipe& p, List list, uint8_t endpoint_address, uint16_t max_packet);
  void link_pipe(Pipe& p);
  void unlink_pipe(Pipe& p);
  void free_pipe(Pipe& p);
  Td* descriptor(Pipe& p, uint32_t index) { return index == 0 ? p.spare[p.dummy] : &pool_[index - 1]; }
  void fill_td(Td* td, uint32_t direction, uint32_t toggle, uint32_t bytes, const void* buffer, bool round);
  void commit(Pipe& p, uint32_t count);
  bool wait(Pipe& p, uint32_t timeout_ms);
  void recover(Pipe& p);
  Pipe* find_pipe(Device& d, uint8_t address);
  uint32_t remaining(const Td* td) const;

  uint64_t regs_ = 0;
  uint32_t ports_ = 0;
  bool owns_controller_ = false;
  bool running_ = false;

  Hcca* hcca_ = nullptr;
  Ed* control_head_ = nullptr;  // list heads: skipped endpoints that only anchor the lists
  Ed* bulk_head_ = nullptr;
  Td* pool_ = nullptr;
  uint8_t* control_buffer_ = nullptr;  // setup packet at 0, data from 64
  uint8_t* bounce_ = nullptr;          // 64 KiB below 4 GiB
  Ed* periodic_first_ = nullptr;
  Device devices_[kMaxPorts];
};

}  // namespace hardware
