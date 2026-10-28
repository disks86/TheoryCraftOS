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


#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaM.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t kMaxFirmwareBytes = 4u << 20;

enum class ReadResult : uint8_t { Ok, NotFound, TooLarge, Failed, NoMemory };

// Reads "/firmware/<name>" whole.
ReadResult read_firmware(filesystem::IFileSystem& file_system, allocators::IAllocator& allocator, const char* name, ByteBuffer* out) {
  String path(allocator);
  if (!path.assign("/firmware/") || !path.append(StringView(name))) return ReadResult::NoMemory;
  UniquePointer<filesystem::IFile> file;
  const filesystem::Status opened = file_system.open_file(StringView(path.c_str()), filesystem::OpenMode::Read, &file);
  if (opened == filesystem::Status::NotFound) return ReadResult::NotFound;
  if (opened != filesystem::Status::Ok || !file) return ReadResult::Failed;
  const uint64_t size = file->size();
  if (size == 0 || size > kMaxFirmwareBytes) return ReadResult::TooLarge;
  if (!out->put_zeros(size)) return ReadResult::NoMemory;
  uint64_t got = 0;
  if (file->read(0, out->writable_view(), &got) != filesystem::Status::Ok || got != size) return ReadResult::Failed;
  return ReadResult::Ok;
}
}  // namespace

const char* AmdVegaM::step_name(Step step) {
  switch (step) {
    case Step::None: return "start";
    case Step::MemoryMap: return "memory map";
    case Step::Firmware: return "firmware";
    case Step::Memory: return "memory";
    case Step::MemoryController: return "memory controller";
    case Step::InterruptHandler: return "interrupt handler";
    case Step::Microcode: return "microcode";
    case Step::GraphicsEngine: return "graphics engine";
    case Step::Sdma: return "SDMA";
    case Step::Done: return "done";
  }
  return "?";
}

AmdVegaM::Layout AmdVegaM::plan(uint64_t smu_bytes, uint64_t gart_table_bytes, uint32_t compute_queues, uint64_t framebuffer_bytes) {
  Layout l;
  if (smu_bytes == 0 || gart_table_bytes == 0 || compute_queues == 0 || compute_queues > AmdGfx8Compute::kMaxQueues || smu_bytes > kRegionLimit || gart_table_bytes > kRegionLimit ||
      framebuffer_bytes > kRegionLimit)
    return l;
  uint64_t at = 0;
  auto take = [&](uint64_t bytes) {
    const uint64_t offset = at;
    at += align_up(bytes, kPageBytes);
    return offset;
  };
  l.dummy_page = take(kPageBytes);
  l.scratch_page = take(kPageBytes);
  l.gart_table = take(gart_table_bytes);
  l.smu = take(smu_bytes);
  l.gfx = take(kGfxBytes);
  l.compute = take(AmdGfx8Compute::block_bytes(compute_queues));
  l.sdma = take(AmdSdma3::kInstances * AmdSdma3::kSlotBytes);
  l.ih = take(kIhBytes);
  l.ib = take(kIbBytes);
  at = align_up(at, kMiB);
  l.framebuffer = at;
  l.framebuffer_bytes = align_up(framebuffer_bytes, kMiB);
  at += l.framebuffer_bytes;
  l.smu_bytes = smu_bytes;
  l.gart_table_bytes = gart_table_bytes;
  l.total = at;
  l.ok = at <= kRegionLimit - kRegionOffset;
  return l;
}

bool AmdVegaM::fail(Step failed, const char* text) {
  String message(status_.allocator());
  message.assign(step_name(failed));
  message.append(": ");
  message.append(text);
  const Step reached = step_;
  shutdown();   // (stops what came up, in reverse)
  status_.assign(message.view());
  step_ = failed == Step::None ? Step::None : reached;
  return false;
}

bool AmdVegaM::bring_up(const Inputs& in) {
  shutdown();
  status_.assign("");
  inputs_ = in;
  // The video BIOS image is kept: the caller's buffer need not outlive the call, and reset_card runs the POST later.
  const bool same_buffer = in.vbios.size() == vbios_.size() && in.vbios.size() != 0 && in.vbios.data() == vbios_.view().data();
  if (!same_buffer) {
    vbios_.clear();
    if (in.vbios.size() != 0 && !vbios_.append(in.vbios)) return fail(Step::None, "out of memory");
  }
  inputs_.vbios = vbios_.view();
  connector_count_ = AtomConnectors::parse(inputs_.vbios, ArrayView<AtomConnectors::Connector>(connectors_, AtomConnectors::kMaxConnectors));
  if (!in.registers || !in.clock || !in.file_system || !in.allocator || !in.mapper || in.aperture_bytes == 0 || in.aperture_physical == 0 || !in.doorbells.valid() ||
      amd_gfx8_firmware_name(in.device_id, AmdGfx8Block::CpPfp) == nullptr)
    return fail(Step::None, "no access to the card (registers, memory BAR, doorbells or a Vega M device)");

  // The memory controller as the card's firmware left it.
  const AmdGmc8MemoryMap map = amd_read_gmc8_memory_map(*in.registers);
  if (!map.read) return fail(Step::MemoryMap, "the memory controller's registers do not answer");
  gpu_base_ = map.gpu_base;
  cpu_base_ = in.aperture_physical;
  step_ = Step::MemoryMap;

  // The microcode files.
  allocators::IAllocator& allocator = *in.allocator;
  ByteBuffer pfp(allocator), me(allocator), ce(allocator), mec(allocator), mec2(allocator), rlc(allocator), sdma0(allocator), sdma1(allocator);
  struct File {
    AmdGfx8Block block;
    ByteBuffer* data;
    AmdGfx8Image* image;
  };
  AmdSmu7Ucode::Images images;
  const File files[] = {{AmdGfx8Block::CpPfp, &pfp, &images.pfp}, {AmdGfx8Block::CpMe, &me, &images.me},       {AmdGfx8Block::CpCe, &ce, &images.ce},
                        {AmdGfx8Block::CpMec, &mec, &images.mec},  {AmdGfx8Block::CpMec2, &mec2, &images.mec2}, {AmdGfx8Block::RlcG, &rlc, &images.rlc},
                        {AmdGfx8Block::Sdma0, &sdma0, &images.sdma0}, {AmdGfx8Block::Sdma1, &sdma1, &images.sdma1}};
  for (const File& f : files) {
    const char* name = amd_gfx8_firmware_name(in.device_id, f.block);
    const ReadResult r = read_firmware(*in.file_system, allocator, name, f.data);
    if (r != ReadResult::Ok) {
      String text(allocator);
      text.assign(name);
      text.append(r == ReadResult::NotFound ? " not found" : r == ReadResult::NoMemory ? ": out of memory" : r == ReadResult::TooLarge ? " has an unusable size" : " could not be read");
      return fail(Step::Firmware, text.c_str());
    }
    if (!amd_gfx8_parse_firmware(f.data->view(), f.block, f.image)) {
      String text(allocator);
      text.assign(name);
      text.append(" is not a microcode file this driver understands");
      return fail(Step::Firmware, text.c_str());
    }
  }
  step_ = Step::Firmware;
  const AmdSmu7Ucode::Layout ucode_layout = AmdSmu7Ucode::plan(images);   // (a set that parsed is complete; a layout that is not ok has no size and the block below refuses it)

  // The block of video memory.
  gart_plan_ = amd_plan_gmc8_gart(map);
  if (!gart_plan_.ok) return fail(Step::Memory, "no room in the GPU's address space for the GART");
  AmdGfx8Compute::Config compute_config;
  layout_ = plan(ucode_layout.total_bytes, gart_plan_.table_bytes, compute_config.queues, in.max_framebuffer_bytes);
  if (!layout_.ok) return fail(Step::Memory, "the block does not fit the part of video memory set aside for it");
  if (kRegionOffset + layout_.total > in.aperture_bytes || kRegionOffset + layout_.total > map.video_memory_bytes) return fail(Step::Memory, "the memory BAR or video memory is too small");
  if (!in.mapper->map_registers(in.aperture_physical + kRegionOffset, layout_.total, &block_)) return fail(Step::Memory, "the memory BAR cannot be mapped");
  block_gpu_ = map.gpu_base + kRegionOffset;
  step_ = Step::Memory;
  auto sub = [&](uint64_t offset, uint64_t bytes) { return block_.subregion(offset, bytes); };

  // The memory controller and the GART.
  gmc_ = UniquePointer<AmdGmc8>::make(allocator, *in.registers, *in.clock);
  if (!gmc_) return fail(Step::Memory, "out of memory");
  AmdGmc8::Memory gmc_memory;
  gmc_memory.table_gpu_address = block_gpu_ + layout_.gart_table;
  gmc_memory.table = sub(layout_.gart_table, gart_plan_.table_bytes);
  gmc_memory.dummy_page_gpu_address = block_gpu_ + layout_.dummy_page;
  gmc_memory.scratch_page_gpu_address = block_gpu_ + layout_.scratch_page;
  const AmdGmc8::Result gmc_result = gmc_->start(gart_plan_, gmc_memory);
  if (gmc_result == AmdGmc8::Result::BadArgument) return fail(Step::MemoryController, AmdGmc8::result_name(gmc_result));
  step_ = Step::MemoryController;   // (a controller that was busy at the end of the start is carried on with, as the driver does)

  // The interrupt handler.
  AmdIh3::Config ih_config;
  ih_config.ring_in_video_memory = true;
  ih_config.msi = in.msi;
  ih_ = UniquePointer<AmdIh3>::make(allocator, *in.registers, *in.clock, ih_config);
  irq_ = UniquePointer<AmdIrq>::make(allocator, *in.registers);
  if (!ih_) return fail(Step::MemoryController, "out of memory");
  AmdIh3::Memory ih_memory;
  ih_memory.ring_gpu_address = block_gpu_ + layout_.ih;
  ih_memory.ring = sub(layout_.ih, kIhBytes);
  ih_memory.dummy_page_gpu_address = block_gpu_ + layout_.dummy_page;
  ih_memory.doorbells = in.doorbells;
  const AmdIh3::Result ih_result = ih_->start(ih_memory);
  if (ih_result != AmdIh3::Result::Ok) return fail(Step::InterruptHandler, AmdIh3::result_name(ih_result));
  step_ = Step::InterruptHandler;

  // The microcode, loaded by the SMC.
  AmdSmu7Ucode loader(*in.registers, *in.clock);
  AmdSmu7Ucode::Memory ucode_memory;
  ucode_memory.gpu_address = block_gpu_ + layout_.smu;
  ucode_memory.block = sub(layout_.smu, layout_.smu_bytes);
  const AmdSmu7Ucode::Result loaded = loader.load(images, ucode_memory);
  if (loaded != AmdSmu7Ucode::Result::Ok) return fail(Step::Microcode, AmdSmu7Ucode::result_name(loaded));
  step_ = Step::Microcode;

  // The graphics engine and its compute queues.
  compute_ = UniquePointer<AmdGfx8Compute>::make(allocator, *in.registers, *in.clock, compute_config);
  gfx_ = UniquePointer<AmdGfx8>::make(allocator, *in.registers, *in.clock);
  if (!compute_ || !gfx_) return fail(Step::Microcode, "out of memory");
  AmdGfx8Compute::Memory compute_memory;
  compute_memory.gpu_address = block_gpu_ + layout_.compute;
  compute_memory.block = sub(layout_.compute, AmdGfx8Compute::block_bytes(compute_config.queues));
  compute_memory.doorbells = in.doorbells;
  const AmdGfx8Compute::Result attached = compute_->attach(compute_memory);
  if (attached != AmdGfx8Compute::Result::Ok) return fail(Step::GraphicsEngine, AmdGfx8Compute::result_name(attached));
  gfx_->attach_compute(compute_.get());
  AmdGfx8::Memory gfx_memory;
  gfx_memory.ring_gpu_address = block_gpu_ + layout_.gfx;
  gfx_memory.ring = sub(layout_.gfx, AmdGfx8::kRingBytes);
  gfx_memory.writeback_gpu_address = block_gpu_ + layout_.gfx + AmdGfx8::kRingBytes;
  gfx_memory.writeback = sub(layout_.gfx + AmdGfx8::kRingBytes, 4096);
  gfx_memory.csb_gpu_address = block_gpu_ + layout_.gfx + AmdGfx8::kRingBytes + 4096;
  gfx_memory.csb = sub(layout_.gfx + AmdGfx8::kRingBytes + 4096, 4096);
  const AmdGfx8::Result gfx_result = gfx_->start(gfx_memory);
  if (gfx_result != AmdGfx8::Result::Ok) return fail(Step::GraphicsEngine, AmdGfx8::result_name(gfx_result));
  step_ = Step::GraphicsEngine;

  // The DMA engines.
  AmdSdma3::Config sdma_config;
  sdma_config.burst_nop[0] = images.sdma0.feature_version >= 20;   // sdma_v3_0_init_microcode
  sdma_config.burst_nop[1] = images.sdma1.feature_version >= 20;
  sdma_ = UniquePointer<AmdSdma3>::make(allocator, *in.registers, *in.clock, sdma_config);
  if (!sdma_) return fail(Step::GraphicsEngine, "out of memory");
  AmdSdma3::Memory sdma_memory;
  sdma_memory.gpu_address = block_gpu_ + layout_.sdma;
  sdma_memory.block = sub(layout_.sdma, AmdSdma3::kInstances * AmdSdma3::kSlotBytes);
  sdma_memory.doorbells = in.doorbells;
  const AmdSdma3::Result sdma_result = sdma_->start(sdma_memory);
  if (sdma_result != AmdSdma3::Result::Ok) return fail(Step::Sdma, AmdSdma3::result_name(sdma_result));
  // The interrupt sources, at the places the driver turns them on (ring init and late_init): the graphics ring's, the compute pipe's, the SDMA engines' and the memory controller's.
  gfx_->set_interrupts(true);
  gfx_->set_ecc_interrupts(true);   // (gfx_v8_0_late_init: priv reg, priv inst, ECC, SQ)
  gfx_->set_sq_interrupts(true);
  compute_->set_eop_interrupt(true);
  sdma_->set_trap_interrupts(true);
  gmc_->set_fault_interrupts(true);
  // The display engine's: page flips of every controller (dce_v11_0_pageflip_interrupt_init) and the hot plug pins (dce_v11_0_hpd_init); vertical blank only on request.
  display_irq_ = UniquePointer<amd_dce11::Interrupts>::make(allocator, *in.registers);
  if (!display_irq_) return fail(Step::Sdma, "out of memory");
  for (uint32_t crtc = 0; crtc < amd_dce11::kMaxCrtcs; ++crtc) display_irq_->set_pageflip(crtc, true);
  for (uint32_t pin = 0; pin < amd_dce11::Interrupts::kMaxHpd; ++pin)
    if (in.hpd_pins >> pin & 1) {
      display_irq_->hpd_init(pin);
      display_irq_->ack_hpd_rx(pin);               // the short pulses of a DisplayPort sink on the pin: a stale one is dropped, then they are counted (irq()->hpd_rx_count)
      display_irq_->hpd_rx_set_interrupt(pin, true);
    }
  // The state of the pins as they are now.
  connected_pins_ = 0;
  throttle_reported_ = 0;
  hotplug_head_ = hotplug_count_ = hotplug_dropped_ = 0;
  for (uint32_t pin = 0; pin < amd_dce11::Interrupts::kMaxHpd; ++pin)
    if ((in.hpd_pins >> pin & 1) && display_irq_->hpd_sense(pin)) connected_pins_ |= 1u << pin;
  audio_pins_ = amd_dce11::AudioPins(*in.registers).connected_mask(kAudioPins);   // (dce_v11_0_audio_get_connected_pins)
  step_ = Step::Done;

  status_.assign("GART ");
  status_.append_uint(gart_plan_.gart_size / kMiB);
  status_.append(" MiB, interrupt ring and sources, microcode loaded by the SMC, graphics engine with ");
  status_.append_uint(compute_config.queues);
  status_.append(" compute queues, 2 SDMA engines");
  return true;
}

void AmdVegaM::shutdown() {
  set_vblank_interrupt(false);
  set_vline_interrupt(false);
  if (display_irq_) {   // (dce_v11_0_hw_fini: the hot plug pins and the page flip interrupts)
    for (uint32_t pin = 0; pin < amd_dce11::Interrupts::kMaxHpd; ++pin)
      if (inputs_.hpd_pins >> pin & 1) display_irq_->hpd_fini(pin);
    for (uint32_t crtc = 0; crtc < amd_dce11::kMaxCrtcs; ++crtc) display_irq_->set_pageflip(crtc, false);
  }
  display_irq_ = UniquePointer<amd_dce11::Interrupts>();
  connected_pins_ = 0;
  audio_pins_ = 0;
  hotplug_head_ = hotplug_count_ = 0;
  display_ = UniquePointer<amd_dce11::Display>();   // (puts the firmware's framebuffer back)
  if (up()) {   // (amdgpu_irq_put in the blocks' hw_fini, before the engines stop)
    gmc_->set_fault_interrupts(false);
    sdma_->set_trap_interrupts(false);
    compute_->set_eop_interrupt(false);
    gfx_->set_interrupts(false);
    gfx_->set_ecc_interrupts(false);
    gfx_->set_sq_interrupts(false);
  }
  if (sdma_) sdma_->stop();
  sdma_ = UniquePointer<AmdSdma3>();
  if (gfx_ && compute_) gfx_->stop();
  gfx_ = UniquePointer<AmdGfx8>();
  compute_ = UniquePointer<AmdGfx8Compute>();
  if (ih_) ih_->stop();
  irq_ = UniquePointer<AmdIrq>();
  ih_ = UniquePointer<AmdIh3>();
  if (gmc_) gmc_->stop();
  gmc_ = UniquePointer<AmdGmc8>();
  step_ = Step::None;
}

// Copies `count` dwords into the staging area (the padding is already in `words`).
static void stage(const MmioRegion& area, const uint32_t* words, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) area.write32(uint64_t{i} * 4, words[i]);
}

bool AmdVegaM::submit_graphics(ArrayView<const uint32_t> commands, uint32_t timeout_us) {
  if (!up() || commands.size() == 0 || commands.size() > kIbDwords) return false;
  uint32_t words[kIbDwords];
  for (size_t i = 0; i < commands.size(); ++i) words[i] = commands[i];
  const uint32_t count = AmdGfx8::pad_ib(ArrayView<uint32_t>(words, kIbDwords), static_cast<uint32_t>(commands.size()));
  if (count == 0) return false;
  stage(block_.subregion(layout_.ib, kIbBytes), words, count);
  amd_gmc8_flush_hdp(*inputs_.registers);
  return gfx_->submit_ib(block_gpu_ + layout_.ib, count, timeout_us);
}

bool AmdVegaM::submit_compute(uint32_t queue, ArrayView<const uint32_t> commands, uint32_t timeout_us) {
  if (!up() || commands.size() == 0 || commands.size() > kIbDwords) return false;
  uint32_t words[kIbDwords];
  for (size_t i = 0; i < commands.size(); ++i) words[i] = commands[i];
  const uint32_t count = AmdGfx8::pad_ib(ArrayView<uint32_t>(words, kIbDwords), static_cast<uint32_t>(commands.size()));
  if (count == 0) return false;
  stage(block_.subregion(layout_.ib, kIbBytes), words, count);
  amd_gmc8_flush_hdp(*inputs_.registers);
  return compute_->submit_ib(queue, block_gpu_ + layout_.ib, count, timeout_us);
}

bool AmdVegaM::submit_sdma(uint32_t instance, ArrayView<const uint32_t> commands, uint32_t timeout_us) {
  if (!up() || commands.size() == 0 || commands.size() > kIbDwords) return false;
  uint32_t words[kIbDwords];
  for (size_t i = 0; i < commands.size(); ++i) words[i] = commands[i];
  const uint32_t count = sdma_->pad_ib(instance, ArrayView<uint32_t>(words, kIbDwords), static_cast<uint32_t>(commands.size()));
  if (count == 0) return false;
  stage(block_.subregion(layout_.ib, kIbBytes), words, count);
  amd_gmc8_flush_hdp(*inputs_.registers);
  return sdma_->submit_ib(instance, block_gpu_ + layout_.ib, count, timeout_us);
}

// The address the card is given for memory the CPU reaches at `p`. The only place a pointer becomes a number: by default the OS keeps its memory mapped at its own addresses
// (UEFI's identity map), and the pointer is not dereferenced.
static uint64_t physical_address(const ISystemAddresses* map, const void* p) { return map ? map->physical_address(p) : reinterpret_cast<uintptr_t>(p); }

bool AmdVegaM::transfer(uint64_t system_address, uint64_t bytes, uint64_t offset, bool to_video, uint32_t instance, uint32_t timeout_us) {
  const uint64_t area = layout_.framebuffer_bytes;
  if (!up() || instance >= AmdSdma3::kInstances || sdma_->wedged(instance) || bytes == 0 || area == 0 || offset > area || bytes > area - offset) return false;
  if (system_address > kAmdGmc8McMask || bytes > kAmdGmc8McMask + 1 - system_address) return false;   // (the end must be inside the 40 bits)
  constexpr uint64_t kWindowBytes = uint64_t{kTransferPages} * kPageBytes;
  const uint64_t video = block_gpu_ + layout_.framebuffer + offset;
  for (uint64_t done = 0; done < bytes;) {
    const uint64_t address = system_address + done;
    const uint64_t in_page = address & (kPageBytes - 1);
    const uint64_t length = bytes - done < kWindowBytes - in_page ? bytes - done : kWindowBytes - in_page;
    const uint64_t pages = (in_page + length + kPageBytes - 1) / kPageBytes;
    uint64_t table[kTransferPages];
    for (uint64_t i = 0; i < pages; ++i) table[i] = (address - in_page) + i * kPageBytes;
    if (!gmc_->map(0, ArrayView<const uint64_t>(table, pages), kAmdGmc8PteSystemPage)) return false;
    const uint64_t window = gmc_->gart_address(0) + in_page;
    amd_gmc8_flush_hdp(*inputs_.registers);
    const bool ok = to_video ? sdma_->copy(instance, window, video + done, length, timeout_us) : sdma_->copy(instance, video + done, window, length, timeout_us);
    if (!ok) {
      if (!sdma_->wedged(instance)) gmc_->unmap(0, kTransferPages);   // (the engine did not take the copy: nothing can still be using the pages)
      return false;
    }
    done += length;
  }
  return gmc_->unmap(0, kTransferPages);
}

bool AmdVegaM::upload(ConstByteView source, uint64_t offset, uint32_t instance, uint32_t timeout_us) {
  return transfer(physical_address(inputs_.system_addresses, source.data()), source.size(), offset, true, instance, timeout_us);
}

bool AmdVegaM::download(ByteView destination, uint64_t offset, uint32_t instance, uint32_t timeout_us) {
  return transfer(physical_address(inputs_.system_addresses, destination.data()), destination.size(), offset, false, instance, timeout_us);
}

uint32_t AmdVegaM::service_interrupts(uint32_t max_vectors) {
  if (!up() || !ih_ || !irq_) return 0;
  const uint32_t handled = irq_->process(*ih_, this, max_vectors);
  if (inputs_.auto_recover && recoveries_ < kMaxRecoveries && wedged()) recover();
  return handled;
}

uint32_t AmdVegaM::refresh_audio_pins() {
  if (!up() || !inputs_.registers) return audio_pins_ = 0;
  return audio_pins_ = amd_dce11::AudioPins(*inputs_.registers).connected_mask(kAudioPins);
}

uint32_t AmdVegaM::audio_outputs(AtomConnectors::Connector* out, uint32_t capacity) const {
  uint32_t n = 0;
  for (const AtomConnectors::Connector& c : connectors()) {
    if (!AtomConnectors::carries_audio(c.type) || c.hpd_pin >= amd_dce11::Interrupts::kMaxHpd || !(connected_pins_ >> c.hpd_pin & 1)) continue;
    if (n < capacity) out[n] = c;
    ++n;
  }
  return n < capacity ? n : capacity;
}

bool AmdVegaM::take_hotplug(HotplugEvent* out) {
  if (hotplug_count_ == 0) return false;
  if (out) *out = hotplug_[hotplug_head_];
  hotplug_head_ = (hotplug_head_ + 1) % kMaxHotplugEvents;
  --hotplug_count_;
  return true;
}

bool AmdVegaM::wedged() const {
  if (!up()) return false;
  if (gfx_->wedged() || !gfx_->running() || sdma_->wedged(0) || sdma_->wedged(1) || !sdma_->running()) return true;   // (not running: an earlier recovery did not bring it back)
  for (uint32_t queue = 0; queue < compute_->queue_count(); ++queue)
    if (compute_->queue_wedged(queue)) return true;
  return false;
}

AmdVegaM::Recovery AmdVegaM::recover() {
  Recovery r;
  if (!up()) return r;
  bool compute_wedged = false;
  for (uint32_t queue = 0; queue < compute_->queue_count(); ++queue) compute_wedged = compute_wedged || compute_->queue_wedged(queue);
  if (gfx_->wedged() || !gfx_->running() || compute_wedged) {
    ++recoveries_;
    r.graphics = true;
    if (gfx_->recover() != AmdGfx8::Result::Ok) r.failed = true;
  }
  if (sdma_->wedged(0) || sdma_->wedged(1) || !sdma_->running()) {
    ++recoveries_;
    r.sdma = true;
    if (sdma_->recover() != AmdSdma3::Result::Ok) r.failed = true;
  }
  // The soft reset did not bring an engine back: the whole card, if that was allowed.
  if (r.failed && inputs_.allow_asic_reset && inputs_.pci && asic_resets_ < kMaxAsicResets) {
    r.card_reset = true;
    r.failed = !reset_card();
  }
  return r;
}

namespace {
// The card as the AtomBIOS interpreter sees it (amdgpu's cail_* callbacks): registers by dword; a discrete card has no memory controller or PLL access through the BIOS.
class PostHost final : public IAtomHost {
 public:
  PostHost(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  uint32_t read_register(uint32_t reg) override { return registers_.read(reg); }
  void write_register(uint32_t reg, uint32_t value) override { registers_.write(reg, value); }
  uint32_t read_mc(uint32_t) override { return 0; }    // (cail_mc_read: "invalid MC read")
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }   // (cail_pll_read)
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t microseconds) override { wait(microseconds); }
  void delay_ms(uint32_t milliseconds) override { wait(uint64_t{milliseconds} * 1000); }
  uint64_t now_us() override { return clock_.now_us(); }

 private:
  void wait(uint64_t microseconds) {
    const uint64_t begin = clock_.now_us();
    while (clock_.now_us() - begin < microseconds) {}
  }
  IVcnRegisters& registers_;
  const IClock& clock_;
};
}  // namespace

bool AmdVegaM::post(const Inputs& in) {
  if (!in.registers || !in.clock || !in.allocator || in.vbios.size() == 0) {
    status_.assign("POST: no video BIOS image");
    return false;
  }
  PostHost host(*in.registers, *in.clock);
  AtomBios bios(host, in.vbios, *in.allocator);
  if (!bios.valid()) {
    status_.assign("POST: the video BIOS is not an AtomBIOS image");
    return false;
  }
  const AtomBios::Status result = bios.asic_init();
  if (result != AtomBios::Status::Ok) {
    status_.assign("POST: ");
    status_.append(StringView(AtomBios::status_name(result)));
    return false;
  }
  return true;
}

bool AmdVegaM::reset_card() {
  if (!inputs_.pci || !inputs_.registers || !inputs_.clock || !inputs_.allocator) return false;
  ++asic_resets_;
  const Inputs in = inputs_;
  shutdown();   // (what can still be stopped is; the display gets the firmware's framebuffer back)
  const AmdAsicReset::Result reset = AmdAsicReset::pci_config_reset(*in.registers, *in.clock, *in.pci, in.pci_address);
  if (reset != AmdAsicReset::Result::Ok) {
    status_.assign("card reset: ");
    status_.append(StringView(AmdAsicReset::result_name(reset)));
    return false;
  }
  if (in.vbios.size() != 0 && !post(in)) {
    String text(*in.allocator);
    text.assign("after a card reset: ");
    text.append(StringView(status_.c_str()));
    status_.assign(text.view());
    return false;
  }
  if (!bring_up(in)) {
    String text(*in.allocator);
    text.assign("after a card reset: ");
    text.append(StringView(status_.c_str()));
    status_.assign(text.view());
    return false;
  }
  return true;
}

// The driver's handlers for the faults hand the ring to the scheduler's timeout handling (drm_sched_fault); wedging the ring is that here.
void AmdVegaM::on_event(const AmdIrq::Event& event) {
  switch (event.kind) {
    case AmdIrq::Kind::GfxPrivilegedRegister:
    case AmdIrq::Kind::GfxPrivilegedInstruction:
      if (event.me == 0) gfx_->fault();
      else if (event.me == 1 || event.me == 2) compute_->fault(event.me, event.pipe, event.queue);
      break;
    case AmdIrq::Kind::SdmaIllegalInstruction: sdma_->fault(event.engine); break;
    case AmdIrq::Kind::DisplayHotplug: {
      connected_pins_ = event.connected ? (connected_pins_ | 1u << event.pin) : (connected_pins_ & ~(1u << event.pin));
      HotplugEvent e;
      e.pin = event.pin;
      e.connected = event.connected;
      if (const AtomConnectors::Connector* connector = AtomConnectors::find_pin(connectors(), event.pin)) {
        e.has_connector = true;
        e.connector = *connector;
      }
      if (irq_->hpd_throttled(event.pin) && !(throttle_reported_ >> event.pin & 1)) {
        e.throttled = true;
        throttle_reported_ |= 1u << event.pin;
      }
      if (hotplug_count_ == kMaxHotplugEvents) {   // full: the oldest goes
        hotplug_head_ = (hotplug_head_ + 1) % kMaxHotplugEvents;
        --hotplug_count_;
        ++hotplug_dropped_;
      }
      hotplug_[(hotplug_head_ + hotplug_count_) % kMaxHotplugEvents] = e;
      ++hotplug_count_;
      break;
    }
    default: break;
  }
  if (observer_) observer_->on_event(event);
}

bool AmdVegaM::engage_display(const amd_dce11::FirmwareFramebuffer& firmware, ByteView* memory) {
  if (!up() || layout_.framebuffer_bytes == 0 || !inputs_.allocator) return false;
  display_ = UniquePointer<amd_dce11::Display>();
  UniquePointer<amd_dce11::Display> display = UniquePointer<amd_dce11::Display>::make(*inputs_.allocator, *inputs_.registers, *inputs_.clock, cpu_base_, gpu_base_,
                                                                                      block_.subregion(layout_.framebuffer, layout_.framebuffer_bytes), block_gpu_ + layout_.framebuffer);
  if (!display || !display->engage(firmware, memory)) return false;
  display_ = util::move(display);
  return true;
}

void AmdVegaM::disengage_display() {
  set_vblank_interrupt(false);
  set_vline_interrupt(false);
  display_ = UniquePointer<amd_dce11::Display>();
}

bool AmdVegaM::set_vline_interrupt(bool enable) {
  if (!enable) {
    if (vline_crtc_ >= 0 && display_irq_) display_irq_->set_vline(static_cast<uint32_t>(vline_crtc_), false);
    vline_crtc_ = -1;
    return true;
  }
  if (!display_irq_ || !display_ || !display_->engaged()) return false;
  set_vline_interrupt(false);
  vline_crtc_ = static_cast<int>(display_->plane().crtc());
  display_irq_->set_vline(static_cast<uint32_t>(vline_crtc_), true);
  return true;
}

bool AmdVegaM::set_vblank_interrupt(bool enable) {
  if (!enable) {
    if (vblank_crtc_ >= 0 && display_irq_) display_irq_->set_vblank(static_cast<uint32_t>(vblank_crtc_), false);
    vblank_crtc_ = -1;
    return true;
  }
  if (!display_irq_ || !display_ || !display_->engaged()) return false;
  set_vblank_interrupt(false);
  vblank_crtc_ = static_cast<int>(display_->plane().crtc());
  display_irq_->set_vblank(static_cast<uint32_t>(vblank_crtc_), true);
  return true;
}

}  // namespace hardware
