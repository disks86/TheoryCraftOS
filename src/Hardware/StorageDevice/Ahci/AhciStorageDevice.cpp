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

#include "Hardware/StorageDevice/Ahci/AhciStorageDevice.hpp"
#include "Hardware/Bus/Pci/FirmwareDriverDetach.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Hardware/StorageDevice/WaitFor.hpp"
#include "Kernel/Thread.hpp"
#if defined(__x86_64__)
#include "Kernel/Apic.hpp"
#endif

namespace hardware {

namespace {
// HBA registers.
constexpr uint32_t kCap = 0x00, kGhc = 0x04, kHbaIs = 0x08, kPi = 0x0C;
constexpr uint32_t kGhcReset = 1u, kGhcInterruptEnable = 1u << 1, kGhcAhciEnable = 1u << 31;
constexpr uint32_t kCapS64A = 1u << 31, kCapSncq = 1u << 30;
// Port registers.
constexpr uint32_t kClb = 0x00, kClbu = 0x04, kFb = 0x08, kFbu = 0x0C, kIs = 0x10, kIe = 0x14, kCmd = 0x18,
                   kTfd = 0x20, kSig = 0x24, kSsts = 0x28, kSerr = 0x30, kSact = 0x34, kCi = 0x38;
constexpr uint32_t kCmdStart = 1u, kCmdFisReceive = 1u << 4, kCmdFisRunning = 1u << 14, kCmdListRunning = 1u << 15;
constexpr uint32_t kTfdError = 1u, kTfdDrq = 1u << 3, kTfdBusy = 1u << 7;
constexpr uint32_t kIsTaskFileError = 1u << 30;
constexpr uint32_t kIsD2hRegister = 1u << 0, kIsSetDeviceBits = 1u << 3;
constexpr uint32_t kIsErrors = kIsTaskFileError | (1u << 29) | (1u << 28) | (1u << 27);   // task file, host bus fatal, host bus data, interface fatal
constexpr uint32_t kPortInterrupts = kIsD2hRegister | kIsSetDeviceBits | kIsErrors;
constexpr uint32_t kSigAta = 0x00000101, kSigAtapi = 0xEB140101;
// ATA commands.
constexpr uint8_t kReadDmaExt = 0x25, kWriteDmaExt = 0x35, kReadDma = 0xC8, kWriteDma = 0xCA;
constexpr uint8_t kFlushCache = 0xE7, kFlushCacheExt = 0xEA, kIdentify = 0xEC, kPacket = 0xA0;
constexpr uint8_t kReadFpdmaQueued = 0x60, kWriteFpdmaQueued = 0x61;
constexpr uint32_t kPciCommand = 0x04;

// Layout of the per-port memory: the command list (32 headers), the received FISes, the IDENTIFY data, then one command table per slot.
constexpr uint32_t kCommandListOffset = 0, kFisOffset = 1024, kIdentifyOffset = 3072, kTableOffset = 4096, kTableStride = 256;
constexpr uint32_t kPortMemoryBytes = kTableOffset + 32 * kTableStride;
constexpr uint32_t kGateExclusive = 1u << 31;
constexpr uint64_t kTimeoutUs = 10000000;
constexpr uint64_t kInterruptCheckUs = 20000;
constexpr uint32_t kTablePacketOffset = 0x40, kTablePrdtOffset = 0x80;

struct CommandHeader {
  uint16_t flags;  // bits 4:0 FIS length in dwords, 5 ATAPI, 6 write
  uint16_t prdt_length;
  uint32_t bytes_transferred;
  uint32_t table_low, table_high;
  uint32_t reserved[4];
};
struct PrdEntry {
  uint32_t low, high, reserved;
  uint32_t byte_count;  // minus one
};
}  // namespace

bool AhciController::initialize() {
  registers_ = pci_.memory_bar(device_.address, 5);
  if (!registers_) return false;
  detach_firmware_drivers(st_, device_.address);
  owns_device_ = true;
  const uint32_t command = pci_.read32(device_.address, kPciCommand) & 0xFFFF;
  pci_.write32(device_.address, kPciCommand, command | (1u << 1) | (1u << 2));  // memory space + bus master

  volatile uint32_t* hba = reinterpret_cast<volatile uint32_t*>(registers_);
  hba[kGhc / 4] = hba[kGhc / 4] | kGhcAhciEnable;  // interrupts stay off: everything is polled
  hba[kGhc / 4] = hba[kGhc / 4] & ~2u;
  const uint32_t cap = hba[kCap / 4];
  addresses_64_ = cap & kCapS64A;
  ncq_ = cap & kCapSncq;
  command_slots_ = ((cap >> 8) & 0x1F) + 1;
  ports_ = hba[kPi / 4];
  return ports_ != 0;
}

bool AhciController::enable_interrupts() {
#if defined(__x86_64__)
  if (interrupts_) return true;
  if (!kernel::running() || !registers_) return false;
  interrupt_core_ = kernel::choose_io_interrupt_core();
  const uint64_t apic = kernel::core_hardware_id(interrupt_core_);
  vector_ = claim_storage_vector();
  if (vector_ == 0 || apic > 0xFF) {
    disable_interrupts();
    return false;
  }
  io_device_ = kernel::register_io_device("ahci", static_cast<int32_t>(interrupt_core_));
  if (!kernel::set_device_interrupt_handler(vector_, io_device_, &AhciController::on_interrupt, this))
    kernel::set_interrupt_handler(vector_, &AhciController::on_interrupt, this);
  if (!msi_.enable(device_.address, 0xFEE00000u | (apic << 12), vector_)) {
    disable_interrupts();
    return false;
  }
  volatile uint32_t* hba = reinterpret_cast<volatile uint32_t*>(registers_);
  hba[kHbaIs / 4] = 0xFFFFFFFFu;
  hba[kGhc / 4] = hba[kGhc / 4] | kGhcInterruptEnable;
  interrupts_ = true;
  return true;
#else
  return false;
#endif
}

void AhciController::disable_interrupts() {
#if defined(__x86_64__)
  if (registers_) {
    volatile uint32_t* hba = reinterpret_cast<volatile uint32_t*>(registers_);
    hba[kGhc / 4] = hba[kGhc / 4] & ~kGhcInterruptEnable;
  }
  msi_.disable();
  if (vector_) {
    kernel::set_device_interrupt_handler(vector_, io_device_, nullptr, nullptr);   // (clears the vector either way)
    kernel::set_interrupt_handler(vector_, nullptr, nullptr);
    release_storage_vector(vector_);
    vector_ = 0;
  }
  if (io_device_ >= 0) kernel::unregister_io_device(io_device_);
  io_device_ = -1;
#endif
  interrupts_ = false;
}

void AhciController::attach_port(uint32_t port, AhciStorageDevice* device) {
  if (port < 32) __atomic_store_n(&port_devices_[port], device, __ATOMIC_RELEASE);
}

void AhciController::detach_port(uint32_t port) {
  if (port < 32) __atomic_store_n(&port_devices_[port], static_cast<AhciStorageDevice*>(nullptr), __ATOMIC_RELEASE);
}

// The HBA's interrupt: every port with something pending takes its completions (and clears its PxIS), then the HBA's bits are cleared.
kernel::Frame* AhciController::on_interrupt(kernel::Frame* frame, void* context) {
  AhciController* hba = static_cast<AhciController*>(context);
#if defined(__x86_64__)
  kernel::apic::eoi();
#endif
  __atomic_fetch_add(&hba->interrupt_count_, 1, __ATOMIC_ACQ_REL);
  volatile uint32_t* regs = reinterpret_cast<volatile uint32_t*>(hba->registers_);
  const uint32_t pending = regs[kHbaIs / 4];
  for (uint32_t bits = pending; bits; bits &= bits - 1) {
    const uint32_t port = static_cast<uint32_t>(__builtin_ctz(bits));
    AhciStorageDevice* device = __atomic_load_n(&hba->port_devices_[port], __ATOMIC_ACQUIRE);
    if (device) {
      device->on_port_interrupt();
    } else {
      volatile uint32_t& is = hba->port_register(port, kIs);
      is = is;   // (write 1 to clear what is set)
    }
  }
  regs[kHbaIs / 4] = pending;
  return frame;
}

void AhciController::shutdown() {
  disable_interrupts();
  if (registers_ && owns_device_) {
    volatile uint32_t* hba = reinterpret_cast<volatile uint32_t*>(registers_);
    hba[kGhc / 4] = hba[kGhc / 4] | kGhcReset;  // the HBA stops using our memory
    wait_for(st_, 1000, [&] { return !(hba[kGhc / 4] & kGhcReset); });
  }
  if (owns_device_) {
    owns_device_ = false;
    reattach_firmware_drivers(st_, device_.address);
  }
}

void AhciStorageDevice::discover(const SharedPointer<AhciController>& controller, allocators::IAllocator& allocator,
                                 List<UniquePointer<IStorageDevice>>& out) {
  for (uint32_t port = 0; port < 32; ++port) {
    if (!(controller->ports_implemented() & (1u << port))) continue;
    const uint32_t ssts = controller->port_register(port, kSsts);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;  // no device, or link asleep
    const uint32_t signature = controller->port_register(port, kSig);
    if (signature != kSigAta && signature != kSigAtapi) continue;  // port multiplier, enclosure
    UniquePointer<AhciStorageDevice> device = UniquePointer<AhciStorageDevice>::make(allocator, controller, port, allocator);
    if (!device) continue;
    device->atapi_ = signature == kSigAtapi;
    if (!device->initialize()) {
      device->release_resources();
      continue;
    }
    out.push_back(UniquePointer<IStorageDevice>(util::move(device)));
  }
}

bool AhciStorageDevice::stop_port() {
  volatile uint32_t& cmd = controller_->port_register(port_, kCmd);
  cmd = cmd & ~kCmdStart;
  if (!wait_for(st_, 500, [&] { return !(cmd & kCmdListRunning); })) return false;
  cmd = cmd & ~kCmdFisReceive;
  return wait_for(st_, 500, [&] { return !(cmd & kCmdFisRunning); });
}

bool AhciStorageDevice::start_port() {
  if (!stop_port()) return false;
  const uint64_t list = reinterpret_cast<uint64_t>(memory_.subview(kCommandListOffset).data());
  const uint64_t fis = reinterpret_cast<uint64_t>(memory_.subview(kFisOffset).data());
  if (!controller_->addresses_64_bit() && ((list | fis) >> 32)) return false;
  controller_->port_register(port_, kClb) = static_cast<uint32_t>(list);
  controller_->port_register(port_, kClbu) = static_cast<uint32_t>(list >> 32);
  controller_->port_register(port_, kFb) = static_cast<uint32_t>(fis);
  controller_->port_register(port_, kFbu) = static_cast<uint32_t>(fis >> 32);
  controller_->port_register(port_, kSerr) = 0xFFFFFFFF;
  controller_->port_register(port_, kIs) = 0xFFFFFFFF;
  controller_->port_register(port_, kIe) = interrupts_ ? kPortInterrupts : 0;
  volatile uint32_t& cmd = controller_->port_register(port_, kCmd);
  cmd = cmd | kCmdFisReceive;
  if (!wait_for(st_, 1000, [&] { return !(controller_->port_register(port_, kTfd) & (kTfdBusy | kTfdDrq)); }))
    return false;
  cmd = cmd | kCmdStart;
  return true;
}

// Runs one command through slot 0. `fis` is the host-to-device register FIS;
// `packet` (12 bytes, may be null) turns it into an ATAPI command.
bool AhciStorageDevice::issue(ConstByteView fis, ConstByteView packet, bool write, ByteView data) {
  if (!started_ || fis.size() > 64 || packet.size() > 16) return false;
  const ByteView table = memory_.subview(kTableOffset, kTablePrdtOffset + sizeof(PrdEntry));   // slot 0's
  table.fill(0);
  table.copy_from(fis);                                     // the command FIS fills the front of the table
  table.subview(kTablePacketOffset, 16).copy_from(packet);  // ATAPI packet, if any
  uint32_t prdt = 0;
  if (!data.empty()) {
    PrdEntry& prd = table.subview(kTablePrdtOffset, sizeof(PrdEntry)).cast<PrdEntry>()[0];
    prd.low = static_cast<uint32_t>(reinterpret_cast<uint64_t>(data.data()));
    prd.high = static_cast<uint32_t>(reinterpret_cast<uint64_t>(data.data()) >> 32);
    prd.byte_count = static_cast<uint32_t>(data.size() - 1);
    prdt = 1;
  }
  CommandHeader& header = memory_.subview(kCommandListOffset, sizeof(CommandHeader)).cast<CommandHeader>()[0];
  const uint64_t table_address = reinterpret_cast<uint64_t>(table.data());
  header.flags = static_cast<uint16_t>((fis.size() / 4) | (!packet.empty() ? 1u << 5 : 0) | (write ? 1u << 6 : 0));
  header.prdt_length = static_cast<uint16_t>(prdt);
  header.bytes_transferred = 0;
  header.table_low = static_cast<uint32_t>(table_address);
  header.table_high = static_cast<uint32_t>(table_address >> 32);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  controller_->port_register(port_, kIs) = 0xFFFFFFFF;
  controller_->port_register(port_, kCi) = 1;
  volatile uint32_t& ci = controller_->port_register(port_, kCi);
  const bool done = wait_for(st_, 10000, [&] {
    return !(ci & 1) || (controller_->port_register(port_, kIs) & kIsTaskFileError);
  });
  const bool failed = !done || (controller_->port_register(port_, kIs) & kIsTaskFileError) ||
                      (controller_->port_register(port_, kTfd) & kTfdError);
  if (failed) {
    started_ = start_port();  // recover: restart the port so later commands can run
    return false;
  }
  return true;
}

bool AhciStorageDevice::identify_ata() {
  const uint8_t fis[20] = {0x27, 0x80, kIdentify};
  const ByteView identify_data = memory_.subview(kIdentifyOffset, 512);  // IDENTIFY DEVICE data, 256 words
  if (!issue(ConstByteView(fis), ConstByteView(), false, identify_data)) return false;
  const ArrayView<uint16_t> id = identify_data.cast<uint16_t>();
  copy_device_string(ArrayView<char>(info_.serial), ConstByteView(identify_data).subview(20, 20), true);  // words 10-19
  copy_device_string(ArrayView<char>(info_.model), ConstByteView(identify_data).subview(54, 40), true);   // words 27-46
  lba48_ = id[83] & (1u << 10);
  info_.block_count = lba48_ ? (uint64_t{id[103]} << 48) | (uint64_t{id[102]} << 32) | (uint64_t{id[101]} << 16) | id[100]
                             : (uint64_t{id[61]} << 16) | id[60];
  info_.block_size = 512;
  if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {  // logical sector longer than 256 words
    const uint32_t words = (uint32_t{id[118]} << 16) | id[117];
    if (words >= 256 && words <= 2048) info_.block_size = words * 2;
  }
  info_.removable = id[0] & (1u << 7);
  info_.media = media_from_ata_rotation(id[217]);
  return info_.block_count > 0;
}

bool AhciStorageDevice::atapi_command(ConstByteView cdb, ByteView data) {
  uint8_t fis[20] = {0x27, 0x80, kPacket, 1 /* DMA */};
  fis[7] = 0xA0;
  fis[5] = 0xFE;  // byte count limit, unused with DMA
  fis[6] = 0xFF;
  return issue(ConstByteView(fis), cdb, false, data);
}

bool AhciStorageDevice::identify_atapi() {
  info_.kind = StorageKind::Optical;
  info_.read_only = true;
  info_.removable = true;
  const ByteView buffer = memory_.subview(kIdentifyOffset, 512);
  const uint8_t inquiry[12] = {scsi::kInquiry, 0, 0, 0, 36};
  if (!atapi_command(ConstByteView(inquiry), buffer.first(36))) return false;
  scsi::InquiryInfo unit;
  if (!scsi::parse_inquiry(buffer, &unit)) return false;
  copy_device_string(ArrayView<char>(info_.model), unit.vendor_and_product, false);
  for (int attempt = 0; attempt < 3; ++attempt) {  // the first command after attaching may report a media change
    const uint8_t capacity[12] = {scsi::kReadCapacity10};
    uint64_t last = 0;
    if (atapi_command(ConstByteView(capacity), buffer.first(8)) &&
        scsi::parse_capacity10(buffer, &last, &info_.block_size)) {
      info_.block_count = last + 1;
      return info_.block_size >= 512 && info_.block_size <= 4096;
    }
  }
  return false;  // no medium
}

bool AhciStorageDevice::initialize() {
  memory_ = allocate_dma_view<uint8_t>(allocator_, kPortMemoryBytes, 4096);
  if (memory_.empty()) return false;
  memory_.fill(0);
  started_ = start_port();
  if (!started_) return false;
  if (!(atapi_ ? identify_atapi() : identify_ata())) return false;
  if (!prepare_bounce()) return false;
  if (!atapi_) set_up_ncq(memory_.subview(kIdentifyOffset, 512).cast<uint16_t>());
  info_.queue_depth = ncq_ ? tag_count_ : 1;
  return true;
}

// NCQ when both the HBA (CAP.SNCQ) and the drive (IDENTIFY word 76 bit 8) do it, with as many tags as both have (word 75: depth - 1).
// Then the HBA's interrupt, checked with one queued read; without it the waiters poll.
bool AhciStorageDevice::set_up_ncq(const ArrayView<uint16_t>& id) {
  if (!controller_->supports_ncq() || !(id[76] & (1u << 8))) return false;
  uint32_t depth = (id[75] & 0x1F) + 1u;
  if (depth > controller_->command_slots()) depth = controller_->command_slots();
  if (depth > kMaxTags) depth = kMaxTags;
  if (depth < 2) return false;
  tag_memory_ = allocate_dma_view<uint8_t>(allocator_, uint64_t{depth} * kSlotBytes, 4096);
  if (tag_memory_.empty()) return false;
  for (uint32_t t = 0; t < depth; ++t) {
    tags_[t].buffer = tag_memory_.subview(uint64_t{t} * kSlotBytes, kSlotBytes);
    tags_[t].completion.lost = false;
    tags_[t].completion.arm();
  }
  tag_count_ = depth;
  pool_.reset(depth);
  issued_ = 0;
  needs_restart_ = false;
  __atomic_store_n(&gate_, 0, __ATOMIC_RELEASE);
  ncq_ = true;
  if (controller_->enable_interrupts()) {
    controller_->attach_port(port_, this);
    controller_->port_register(port_, kIs) = 0xFFFFFFFF;
    controller_->port_register(port_, kIe) = kPortInterrupts;   // (waiters poll until the check below says the interrupt comes)
    if (check_interrupts()) {
      interrupts_ = true;
    } else {
      controller_->port_register(port_, kIe) = 0;
      controller_->detach_port(port_);
    }
  }
  return true;
}

bool AhciStorageDevice::check_interrupts() {
  const uint64_t before = controller_->interrupts();
  const uint32_t tag = pool_.acquire();
  bool retry = false;
  const bool read = execute_ncq(tag, false, 0, 1, &retry);
  if (!tags_[tag].completion.lost) pool_.release(tag);
  if (!read) return false;
  const uint64_t start = storage_now_us();
  while (controller_->interrupts() == before) {
    if (storage_now_us() - start > kInterruptCheckUs) return false;
    kernel::cpu_relax();
  }
  return true;
}

void AhciStorageDevice::shutdown() {
  release_resources();
  controller_->shutdown();
}

void AhciStorageDevice::release_resources() {
  if (!memory_.empty() && started_) stop_port();
  started_ = false;
  if (ncq_) controller_->detach_port(port_);
  ncq_ = interrupts_ = false;
  pool_.close();
  for (Tag& t : tags_) t.buffer = ByteView();
  tag_count_ = 0;
  release_view(allocator_, tag_memory_);
  release_bounce();
  release_view(allocator_, memory_);
}

bool AhciStorageDevice::transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) {
  if (atapi_) {
    if (write) return false;
    uint8_t cdb[16];
    // A CD-ROM is read with READ(10) in a 12-byte packet; 16-byte commands do not fit.
    if (scsi::build_read_write(ByteView(cdb), false, lba, blocks) != 10) return false;
    return atapi_command(ConstByteView(cdb).first(12), dma);
  }
  uint8_t fis[20] = {0x27, 0x80};
  fis[2] = lba48_ ? (write ? kWriteDmaExt : kReadDmaExt) : (write ? kWriteDma : kReadDma);
  fis[7] = 0x40;  // LBA mode
  fis[4] = static_cast<uint8_t>(lba);
  fis[5] = static_cast<uint8_t>(lba >> 8);
  fis[6] = static_cast<uint8_t>(lba >> 16);
  if (lba48_) {
    fis[8] = static_cast<uint8_t>(lba >> 24);
    fis[9] = static_cast<uint8_t>(lba >> 32);
    fis[10] = static_cast<uint8_t>(lba >> 40);
  } else {
    fis[7] |= (lba >> 24) & 0x0F;
  }
  fis[12] = static_cast<uint8_t>(blocks);
  fis[13] = static_cast<uint8_t>(blocks >> 8);
  return issue(ConstByteView(fis), ConstByteView(), write, dma);
}

bool AhciStorageDevice::flush_cache() {
  if (atapi_) return true;
  uint8_t fis[20] = {0x27, 0x80, lba48_ ? kFlushCacheExt : kFlushCache};
  fis[7] = 0x40;
  return issue(ConstByteView(fis), ConstByteView(), false, ByteView());
}

// ---- Native Command Queuing ----

void AhciStorageDevice::enter_queued() {
  for (;;) {
    uint32_t gate = __atomic_load_n(&gate_, __ATOMIC_ACQUIRE);
    if (!(gate & kGateExclusive) && __atomic_compare_exchange_n(&gate_, &gate, gate + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    if (kernel::running()) kernel::yield();
    else kernel::cpu_relax();
  }
}

void AhciStorageDevice::leave_queued() { __atomic_fetch_sub(&gate_, 1, __ATOMIC_ACQ_REL); }

void AhciStorageDevice::enter_exclusive() {
  for (;;) {
    uint32_t gate = __atomic_load_n(&gate_, __ATOMIC_ACQUIRE);
    if (!(gate & kGateExclusive) && __atomic_compare_exchange_n(&gate_, &gate, gate | kGateExclusive, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    if (kernel::running()) kernel::yield();
    else kernel::cpu_relax();
  }
  while (__atomic_load_n(&gate_, __ATOMIC_ACQUIRE) & ~kGateExclusive) {   // the queued commands still on the drive finish first
    if (kernel::running()) kernel::yield();
    else kernel::cpu_relax();
  }
}

void AhciStorageDevice::leave_exclusive() { __atomic_fetch_and(&gate_, ~kGateExclusive, __ATOMIC_ACQ_REL); }

// Takes the completed tags (PxSACT bits the drive cleared) and wakes their waiters; after an error, fails every tag on the drive.
void AhciStorageDevice::reap() {
  uint32_t woken[kMaxTags];
  uint32_t n = 0;
  const uint64_t flags = kernel::detail::lock_list(&lock_);
  volatile uint32_t& is = controller_->port_register(port_, kIs);
  const uint32_t status = is;
  is = status;   // (cleared before SACT is read: a completion after the read raises it again)
  if (status & kIsErrors) {
    for (uint32_t bits = issued_; bits; bits &= bits - 1) {
      const uint32_t tag = static_cast<uint32_t>(__builtin_ctz(bits));
      tags_[tag].completion.record(1);
      woken[n++] = tag;
    }
    issued_ = 0;
    needs_restart_ = true;
  } else {
    const uint32_t done = issued_ & ~controller_->port_register(port_, kSact);
    for (uint32_t bits = done; bits; bits &= bits - 1) {
      const uint32_t tag = static_cast<uint32_t>(__builtin_ctz(bits));
      tags_[tag].completion.record(0);
      woken[n++] = tag;
    }
    issued_ &= ~done;
  }
  kernel::detail::unlock_list(&lock_, flags);
  for (uint32_t i = 0; i < n; ++i) tags_[woken[i]].completion.wake();
}

void AhciStorageDevice::on_port_interrupt() {
  if (ncq_) {
    reap();
  } else {
    volatile uint32_t& is = controller_->port_register(port_, kIs);
    is = is;
  }
}

// After a task-file error (or a command that never finished) the port is stopped and started again, with no queued command on it.
bool AhciStorageDevice::restart_after_error() {
  enter_exclusive();
  bool ok = true;
  if (__atomic_load_n(&needs_restart_, __ATOMIC_ACQUIRE)) {
    ok = start_port();   // (stops it first: PxCI and PxSACT clear)
    started_ = ok;
    const uint64_t flags = kernel::detail::lock_list(&lock_);
    issued_ = 0;
    needs_restart_ = false;
    kernel::detail::unlock_list(&lock_, flags);
    ++errors_recovered_;
  }
  leave_exclusive();
  return ok;
}

bool AhciStorageDevice::execute_ncq(uint32_t tag, bool write, uint64_t lba, uint32_t blocks, bool* retry) {
  *retry = false;
  if (!started_) return false;
  Tag& t = tags_[tag];
  const uint64_t bytes = uint64_t{blocks} * info_.block_size;
  const ByteView table = memory_.subview(kTableOffset + uint64_t{tag} * kTableStride, kTableStride);
  table.fill(0);
  // READ/WRITE FPDMA QUEUED: the count goes in the features registers, the tag in bits 7:3 of the count register.
  const ByteView fis = table.first(20);
  fis[0] = 0x27;
  fis[1] = 0x80;
  fis[2] = write ? kWriteFpdmaQueued : kReadFpdmaQueued;
  fis[3] = static_cast<uint8_t>(blocks);
  fis[4] = static_cast<uint8_t>(lba);
  fis[5] = static_cast<uint8_t>(lba >> 8);
  fis[6] = static_cast<uint8_t>(lba >> 16);
  fis[7] = 0x40;
  fis[8] = static_cast<uint8_t>(lba >> 24);
  fis[9] = static_cast<uint8_t>(lba >> 32);
  fis[10] = static_cast<uint8_t>(lba >> 40);
  fis[11] = static_cast<uint8_t>(blocks >> 8);
  fis[12] = static_cast<uint8_t>(tag << 3);
  PrdEntry& prd = table.subview(kTablePrdtOffset, sizeof(PrdEntry)).cast<PrdEntry>()[0];
  const uint64_t data = reinterpret_cast<uint64_t>(t.buffer.data());
  prd.low = static_cast<uint32_t>(data);
  prd.high = static_cast<uint32_t>(data >> 32);
  prd.byte_count = static_cast<uint32_t>(bytes - 1);
  CommandHeader& header = memory_.subview(kCommandListOffset + uint64_t{tag} * sizeof(CommandHeader), sizeof(CommandHeader)).cast<CommandHeader>()[0];
  const uint64_t table_address = reinterpret_cast<uint64_t>(table.data());
  header.flags = static_cast<uint16_t>(5u | (write ? 1u << 6 : 0));   // a 5-dword FIS
  header.prdt_length = 1;
  header.bytes_transferred = 0;
  header.table_low = static_cast<uint32_t>(table_address);
  header.table_high = static_cast<uint32_t>(table_address >> 32);
  t.completion.arm();
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  const uint64_t flags = kernel::detail::lock_list(&lock_);
  if (needs_restart_) {
    kernel::detail::unlock_list(&lock_, flags);
    *retry = true;
    return false;
  }
  issued_ |= 1u << tag;
  controller_->port_register(port_, kSact) = 1u << tag;   // (SACT before CI, as the specification asks)
  controller_->port_register(port_, kCi) = 1u << tag;
  kernel::detail::unlock_list(&lock_, flags);

  if (!t.completion.wait(may_sleep_for_interrupt(interrupts_), kTimeoutUs, [&] { reap(); })) {
    const uint64_t f = kernel::detail::lock_list(&lock_);
    needs_restart_ = true;   // the drive still holds the tag: only a restart takes it back
    kernel::detail::unlock_list(&lock_, f);
    return false;
  }
  return t.completion.status == 0;
}

bool AhciStorageDevice::run_ncq(bool write, uint64_t lba, ByteView into, ConstByteView from) {
  const uint64_t bytes = write ? from.size() : into.size();
  if (!started_ || bytes == 0 || bytes % info_.block_size != 0) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  uint64_t count = bytes / info_.block_size;
  if (lba >= info_.block_count || count > info_.block_count - lba) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  const uint32_t tag = pool_.acquire();
  const uint32_t max_blocks = kSlotBytes / info_.block_size;
  uint64_t done = 0;
  bool ok = true;
  uint32_t restarts = 0;
  while (count > 0 && ok) {
    const uint32_t n = count < max_blocks ? static_cast<uint32_t>(count) : max_blocks;
    const uint64_t chunk = uint64_t{n} * info_.block_size;
    const ByteView dma = tags_[tag].buffer.first(chunk);
    if (write) dma.copy_from(from.subview(done, chunk));
    bool retry = false;
    enter_queued();
    const bool moved = !__atomic_load_n(&needs_restart_, __ATOMIC_ACQUIRE) && execute_ncq(tag, write, lba, n, &retry);
    if (!moved && !retry && !__atomic_load_n(&needs_restart_, __ATOMIC_ACQUIRE)) ok = false;   // the drive refused it
    leave_queued();
    if (!moved) {
      // The queue was stopped by an error (perhaps another command's): restart the port and try this chunk again, a few times.
      if (!ok || tags_[tag].completion.lost || ++restarts > 3 || !restart_after_error()) {
        ok = false;
        break;
      }
      continue;
    }
    if (!write) into.subview(done, chunk).copy_from(dma);
    __atomic_fetch_add(write ? &stats_.blocks_written : &stats_.blocks_read, n, __ATOMIC_RELAXED);
    lba += n;
    count -= n;
    done += chunk;
  }
  if (!tags_[tag].completion.lost) pool_.release(tag);
  __atomic_fetch_add(ok ? (write ? &stats_.writes : &stats_.reads) : &stats_.errors, 1, __ATOMIC_RELAXED);
  return ok;
}

bool AhciStorageDevice::read(uint64_t lba, ByteView buffer) {
  if (!ncq_) return StorageDeviceBase::read(lba, buffer);
  return run_ncq(false, lba, buffer, ConstByteView());
}

bool AhciStorageDevice::write(uint64_t lba, ConstByteView buffer) {
  if (!ncq_) return StorageDeviceBase::write(lba, buffer);
  if (info_.read_only) {
    __atomic_fetch_add(&stats_.errors, 1, __ATOMIC_RELAXED);
    return false;
  }
  return run_ncq(true, lba, ByteView(), buffer);
}

// A cache flush cannot be queued: it runs with the queue empty.
bool AhciStorageDevice::flush() {
  if (!ncq_) return StorageDeviceBase::flush();
  enter_exclusive();
  const bool ok = StorageDeviceBase::flush();
  leave_exclusive();
  return ok;
}

StorageStatistics AhciStorageDevice::statistics() const {
  StorageStatistics s;
  s.reads = __atomic_load_n(&stats_.reads, __ATOMIC_RELAXED);
  s.writes = __atomic_load_n(&stats_.writes, __ATOMIC_RELAXED);
  s.blocks_read = __atomic_load_n(&stats_.blocks_read, __ATOMIC_RELAXED);
  s.blocks_written = __atomic_load_n(&stats_.blocks_written, __ATOMIC_RELAXED);
  s.errors = __atomic_load_n(&stats_.errors, __ATOMIC_RELAXED);
  return s;
}

void AhciStorageDevice::describe(IPropertySink& sink) const {
  sink.add_flag("Native Command Queuing", ncq_);
  const QueueReport report{controller_->interrupt_core(), controller_->interrupts()};
  describe_queues(sink, 1, ncq_ ? tag_count_ : 1, interrupts_, &report);
  if (errors_recovered_) sink.add_number("Queue restarts after errors", errors_recovered_);
}

}  // namespace hardware
