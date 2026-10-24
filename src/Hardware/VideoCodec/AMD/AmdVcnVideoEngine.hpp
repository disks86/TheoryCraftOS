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
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Utility/FixedString.hpp"

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
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), allocator_(allocator), gpu_(amd_gpu_identify(gpu_device_id)), discovery_(discovery) {}

  // A Radeon card's register BAR (BAR 5; at least AmdMmIndexVramReader::kRegisterWindow bytes), for reaching video memory the CPU-visible aperture does not cover,
  // and the size of that aperture (BAR 0). Call before initialize(); both are optional.
  void set_registers(const MmioRegion& registers) { registers_ = registers; }
  void set_memory_bar(uint64_t bytes) { memory_bar_ = bytes; }
  uint64_t memory_bar() const { return memory_bar_; }

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
      summary_.append(", IP discovery table not read");
      if (gpu_.discrete && memory_bar_) {
        summary_.append(", memory aperture ");
        summary_.append_uint(memory_bar_ >> 20);
        summary_.append(" MiB");
      }
    }
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
  // H.264 and JPEG are on every generation that has a video engine at all.
  uint32_t base_codecs() const {
    return amd_gpu_has_video_engine(gpu_.architecture) ? codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mjpeg) : 0;
  }

  allocators::IAllocator& allocator_;
  AmdGpuInfo gpu_;
  MmioRegion discovery_;
  MmioRegion registers_;
  uint64_t memory_bar_ = 0;
  AmdMediaEngineStatus status_;
  FixedString<120> summary_;
};

}  // namespace hardware
