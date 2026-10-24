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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdMediaEngineHw.hpp"
#include "Hardware/VideoCodec/AMD/AmdRavenMemory.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// AMD's Video Core Next (VCN, earlier UVD/VCE) engine: the video decode/encode block that comes with the GPU (PCI vendor 0x1002). The sessions
// still run on the CPU, through the software codecs, until a native driver for the engine exists - this class
// is where it will go, and what it reports now is that the engine is there (info().hardware_engine) and not used
// (info().accelerated is false). H.264, H.265, VP9 and AV1 will be served here once the engine is driven.
class AmdVcnVideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x1002;

  // `gpu_device_id` is the PCI device ID of the GPU the engine belongs to; it picks the generation's codec set (0 = unknown).
  // `discovery` is, for a card, the last 64 KiB of its video memory; for an APU, its memory BAR (see AmdMediaEngineHw); or an empty window to skip looking at the hardware.
  explicit AmdVcnVideoEngine(allocators::IAllocator& allocator, uint16_t gpu_device_id = 0, const MmioRegion& discovery = MmioRegion())
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), allocator_(allocator), device_id_(gpu_device_id), gpu_(amd_gpu_identify(gpu_device_id)), discovery_(discovery), firmware_(allocator) {}

  // A Radeon card's register BAR (BAR 5; at least AmdMmIndexVramReader::kRegisterWindow bytes), for reaching video memory the CPU-visible aperture does not cover,
  // and the size of that aperture (BAR 0). Call before initialize(); both are optional.
  void set_registers(const MmioRegion& registers) { registers_ = registers; }
  void set_memory_bar(uint64_t bytes) { memory_bar_ = bytes; }
  uint64_t memory_bar() const { return memory_bar_; }
  // Which PCI function the engine belongs to (set by SystemVideoCodecs), so what it learns about the memory can be matched to the display device.
  void set_pci_address(const PciAddress& address) { address_ = address; has_address_ = true; }
  bool gpu_location(PciAddress* out) const override {
    if (!has_address_ || !out) return false;
    *out = address_;
    return true;
  }
  // The card's video memory (probe_video_memory, or the aperture when it was all of it), or the APU's carve-out; 0 when the table was not read.
  uint64_t gpu_memory_bytes() const override { return status_.video_memory_bytes ? status_.video_memory_bytes : status_.carve_out_bytes; }

  // The PCI revision of the GPU (config space offset 8): which VCN 1.0 firmware file it needs depends on it. Call before load_firmware().
  void set_pci_revision(uint8_t revision) { revision_ = revision; }

  // VCN 1.0 (Ryzen 2000/3000 "Raven" APUs) runs on microcode the OS has to put into the engine's memory: this reads the right file from /firmware and checks it.
  // It does not start the engine - that needs the GPU's memory controller set up and a way to hand the microcode to the card, which this OS does not have.
  // Other generations have no firmware file here yet (firmware_status() stays empty).
  bool load_firmware(filesystem::IFileSystem& file_system) override {
    const char* name = amd_vcn1_firmware_name(device_id_, revision_);
    if (!name) return false;
    firmware_name_ = name;
    firmware_state_ = firmware_.load(file_system, name);
    engine_note_ = "engine not started";
    rebuild_firmware_text();
    refresh_summary();
    return firmware_state_ == AmdFirmwareStatus::Ok;
  }
  const char* firmware_status() const override { return firmware_text_.c_str(); }

  // Starts VCN 1.0 with the loaded microcode (AmdVcn1: the driver's start sequence). The caller supplies what only it can know - the memory the engine may use,
  // as the GPU addresses it, and the GPU's address configuration - and the registers and a clock that outlive the engine. A wrong address makes the engine's DMA
  // hit whatever is there, so nothing in the OS calls this yet. The engine is only brought up and left idle: no decode path exists, and info().accelerated stays false.
  AmdVcn1::Status start_engine(IVcnRegisters& registers, const IClock& clock, const AmdVcn1::Memory& memory, const AmdVcn1::Config& config) {
    if (!firmware_.loaded()) return AmdVcn1::Status::BadFirmware;
    stop_engine();
    vcn1_ = UniquePointer<AmdVcn1>::make(allocator_, registers, clock);
    if (!vcn1_) return AmdVcn1::Status::NoMemory;
    const AmdVcn1::Status status = vcn1_->start(firmware_, memory, config);
    if (status == AmdVcn1::Status::Ok) {
      engine_note_ = "engine started, idle; nothing decodes on it yet";
    } else {
      vcn1_.reset();
      engine_note_ = status == AmdVcn1::Status::TimedOut ? "engine start failed: the VCPU did not answer" :
                     status == AmdVcn1::Status::BadMemory ? "engine start refused: bad memory block" :
                     status == AmdVcn1::Status::BadFirmware ? "engine start refused: unusable firmware" : "engine start failed: out of memory";
    }
    rebuild_firmware_text();
    refresh_summary();
    return status;
  }
  // Finds, with reads only, the memory block and address configuration the start needs (AmdRavenMemory), maps the block, and says in plan_status() where it is
  // and whether the hardware agrees on the address configuration - or why not. Needs the firmware loaded (its size sets the block size), the register BAR of
  // the card (set_registers: the 512 KiB BAR 5) and the display buffer of the firmware from the environment. Writes nothing to the card; mapping is all it does.
  bool plan_engine(const VideoEngineEnvironment& environment) override {
    planned_ = false;
    plan_text_.assign("");
    if (!amd_vcn1_firmware_name(device_id_, revision_)) return false;   // not a VCN 1.0 part: nothing to plan
    plan_text_.assign("engine memory not planned: ");
    if (!firmware_.loaded()) { plan_text_.append("no firmware"); return false; }
    if (registers_.length() < kAmdRavenRegisterBytes) { plan_text_.append("no access to the registers of the card"); return false; }
    if (!environment.mapper) { plan_text_.append("no way to map memory"); return false; }
    MmioVcnRegisters registers(registers_);
    plan_ = amd_plan_vcn1_memory(amd_read_raven_memory_map(registers), device_id_, revision_, AmdVcn1::block_bytes(firmware_), environment.framebuffer_base,
                                 environment.framebuffer_bytes);
    if (plan_.result != AmdVcn1Plan::Result::Ok) {
      plan_text_.append(amd_vcn1_plan_result_name(plan_.result));
      return false;
    }
    if (!amd_map_vcn1_plan(plan_, *environment.mapper, &memory_)) {
      plan_text_.append("the block at CPU 0x");
      append_hex(plan_text_, plan_.block_physical);
      plan_text_.append(" is memory the OS owns or cannot be mapped");
      return false;
    }
    plan_text_.assign("engine memory: block at video memory +");
    plan_text_.append_uint(plan_.block_offset >> 20);
    plan_text_.append(" MiB (GPU 0x");
    append_hex(plan_text_, plan_.block_gpu_address);
    plan_text_.append(", CPU 0x");
    append_hex(plan_text_, plan_.block_physical);
    plan_text_.append("), ");
    plan_text_.append_uint(plan_.block_bytes >> 10);
    plan_text_.append(" KiB; gb_addr_config 0x");
    append_hex(plan_text_, plan_.gb_addr_config);
    plan_text_.append(plan_.gb_check == AmdGbAddrConfigCheck::Matches ? " (the hardware agrees)" : " (the hardware register is not readable)");
    planned_ = true;
    return true;
  }
  const char* plan_status() const override { return plan_text_.c_str(); }
  const AmdVcn1Plan& plan() const { return plan_; }
  bool engine_planned() const { return planned_; }

  // Starts the engine with what plan_engine found: the planned block, power and clock gating left off (the engine always powered). The DMA of the engine goes to
  // the planned addresses, so this is only as safe as the plan; nothing in the OS calls it yet. It writes the register BAR of the card.
  AmdVcn1::Status start_planned_engine(const IClock& clock) {
    if (!planned_) return AmdVcn1::Status::BadMemory;
    vcn_registers_ = UniquePointer<MmioVcnRegisters>::make(allocator_, registers_);
    if (!vcn_registers_) return AmdVcn1::Status::NoMemory;
    AmdVcn1::Config config;
    config.gb_addr_config = plan_.gb_addr_config;
    return start_engine(*vcn_registers_, clock, memory_, config);
  }

  // Puts the engine back to sleep. Safe when it was never started.
  void stop_engine() {
    if (vcn1_) {
      vcn1_->stop();
      vcn1_.reset();
      engine_note_ = "engine stopped";
      rebuild_firmware_text();
      refresh_summary();
    }
  }
  bool engine_running() const { return vcn1_ != nullptr; }
  const AmdVcnFirmware& firmware() const { return firmware_; }
  AmdFirmwareStatus firmware_state() const { return firmware_state_; }

  // Reads the GPU's IP discovery table (when given a window and the generation has one). Always succeeds: the engine is there either way.
  bool initialize() override {
    // An APU's window is its memory BAR (the table is at the end of the carve-out somewhere inside it); a card's is the last 64 KiB of its own memory.
    if (gpu_.discrete) {
      status_ = AmdMediaEngineHw::probe(allocator_, discovery_, gpu_.architecture);
      if (status_.probed) {
        if (memory_bar_) status_.video_memory_bytes = memory_bar_;   // the table was at the end of the aperture, so the aperture is all of the memory
      } else if (registers_.length() >= AmdMmIndexVramReader::kRegisterWindow) {
        // Not reachable through the aperture (Resizable BAR off): ask the card for the memory past its end, through the register BAR.
        AmdMmIndexVramReader reader(registers_);
        status_ = AmdMediaEngineHw::probe_video_memory(allocator_, reader, gpu_.architecture);
        reader.park();
      }
    } else {
      status_ = AmdMediaEngineHw::probe_aperture(allocator_, discovery_, gpu_.architecture);
    }
    refresh_summary();
    return true;
  }
  const AmdMediaEngineStatus& engine_status() const { return status_; }
  const char* engine_summary() const override { return summary_.c_str(); }

  const char* name() const override { return "AMD video engine (VCN) (software path)"; }
  const AmdGpuInfo& gpu() const { return gpu_; }
  // Codecs the engine would serve once it is driven: today's sessions all run in software (info().accelerated is false).
  uint32_t engine_decode_codecs() const {
    return base_codecs() | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0) |
           (gpu_.media_av1_decode ? codec_mask(VideoCodec::Av1) : 0);
  }
  uint32_t engine_encode_codecs() const {
    return base_codecs() | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_av1_encode ? codec_mask(VideoCodec::Av1) : 0);
  }

 private:
  static void append_hex(FixedString<255>& out, uint64_t value) {
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
      const uint32_t nibble = static_cast<uint32_t>((value >> shift) & 0xF);
      if (nibble == 0 && !started && shift != 0) continue;
      started = true;
      out.push_back("0123456789ABCDEF"[nibble]);
    }
  }

  // "<file>: ENC 1.15 DEC 3 revision 9, 369120 bytes of microcode loaded, <what became of the engine>", or why the file is not usable.
  void rebuild_firmware_text() {
    firmware_text_.assign(firmware_name_ ? firmware_name_ : "");
    firmware_text_.append(": ");
    if (firmware_state_ == AmdFirmwareStatus::Ok) {
      const AmdVcnFirmwareVersion v = firmware_.version();
      firmware_text_.append("ENC ");
      firmware_text_.append_uint(v.enc_major);
      firmware_text_.append(".");
      firmware_text_.append_uint(v.enc_minor);
      firmware_text_.append(" DEC ");
      firmware_text_.append_uint(v.dec);
      firmware_text_.append(" revision ");
      firmware_text_.append_uint(v.revision);
      firmware_text_.append(", ");
      firmware_text_.append_uint(firmware_.ucode().size());
      firmware_text_.append(" bytes of microcode loaded, ");
      firmware_text_.append(engine_note_);
    } else {
      firmware_text_.append(amd_firmware_status_name(firmware_state_));
      firmware_text_.append(" (expected /firmware/");
      firmware_text_.append(firmware_name_ ? firmware_name_ : "");
      firmware_text_.append(")");
    }
  }

  void refresh_summary() {
    summary_.assign(gpu_.name);
    if (status_.probed) {
      const AmdIpDiscoveryInfo& d = status_.discovery;
      summary_.append(", VCN ");
      summary_.append_uint(d.vcn.major);
      summary_.append(".");
      summary_.append_uint(d.vcn.minor);
      summary_.append(".");
      summary_.append_uint(d.vcn.revision);
      summary_.append(" x");
      summary_.append_uint(d.vcn_instances);
      summary_.append(", graphics ");
      summary_.append_uint(d.gc.major);
      summary_.append(".");
      summary_.append_uint(d.gc.minor);
      if (status_.carve_out_bytes) {
        summary_.append(", shared memory ");
        summary_.append_uint(status_.carve_out_bytes >> 20);
        summary_.append(" MiB");
      }
      if (status_.video_memory_bytes) {
        summary_.append(", video memory ");
        summary_.append_uint(status_.video_memory_bytes >> 20);
        summary_.append(" MiB");
        if (memory_bar_ >= status_.video_memory_bytes) {
          summary_.append(" (all CPU-visible)");
        } else if (memory_bar_) {
          summary_.append(", CPU aperture ");
          summary_.append_uint(memory_bar_ >> 20);
          summary_.append(" MiB");
        }
      }
    } else {
      if (AmdMediaEngineHw::supported(gpu_.architecture)) summary_.append(", IP discovery table not read");   // (Vega and older have none)
      if (gpu_.discrete && memory_bar_) {
        summary_.append(", memory aperture ");
        summary_.append_uint(memory_bar_ >> 20);
        summary_.append(" MiB");
      }
    }
    if (!firmware_text_.empty()) {
      summary_.append(", firmware ");
      summary_.append(firmware_text_.view());
    }
  }

  // H.264 and JPEG are on every generation that has a video engine at all.
  uint32_t base_codecs() const {
    return amd_gpu_has_video_engine(gpu_.architecture) ? codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mjpeg) : 0;
  }

  allocators::IAllocator& allocator_;
  uint16_t device_id_ = 0;
  uint8_t revision_ = 0;
  AmdGpuInfo gpu_;
  MmioRegion discovery_;
  AmdVcnFirmware firmware_;
  AmdFirmwareStatus firmware_state_ = AmdFirmwareStatus::NotApplicable;
  const char* firmware_name_ = nullptr;
  FixedString<200> firmware_text_;
  const char* engine_note_ = "engine not started";
  UniquePointer<AmdVcn1> vcn1_;
  UniquePointer<MmioVcnRegisters> vcn_registers_;   // the register BAR of the card, kept for the engine while it runs
  AmdVcn1Plan plan_;
  AmdVcn1::Memory memory_;
  bool planned_ = false;
  FixedString<255> plan_text_;
  MmioRegion registers_;
  uint64_t memory_bar_ = 0;
  PciAddress address_{};
  bool has_address_ = false;
  AmdMediaEngineStatus status_;
  FixedString<200> summary_;
};

}  // namespace hardware
