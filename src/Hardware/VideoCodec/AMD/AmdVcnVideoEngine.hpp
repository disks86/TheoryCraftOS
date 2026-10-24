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
  MmioRegion registers_;
  uint64_t memory_bar_ = 0;
  PciAddress address_{};
  bool has_address_ = false;
  AmdMediaEngineStatus status_;
  FixedString<200> summary_;
};

}  // namespace hardware
