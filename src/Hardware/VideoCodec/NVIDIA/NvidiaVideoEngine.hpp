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
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaGpuGenerations.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"

namespace hardware {

// NVIDIA's NVDEC / NVENC engines: the video decode/encode block that comes with the GPU (PCI vendor 0x10DE). The sessions
// still run on the CPU, through the software codecs, until a native driver for the engine exists - this class
// is where it will go, and what it reports now is that the engine is there (info().hardware_engine) and not used
// (info().accelerated is false). H.264, H.265, VP9 and AV1 will be served here once the engine is driven.
class NvidiaVideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;

  // `gpu_device_id` is the PCI device ID of the GPU the engine belongs to; it picks the generation's codec set (0 = unknown). `boot0` is the value of
  // the GPU's NV_PMC_BOOT_0 register when it was read (0 = not read): it names the architecture of a card whose device ID is not in the table.
  explicit NvidiaVideoEngine(allocators::IAllocator& allocator, uint16_t gpu_device_id = 0, uint32_t boot0 = 0)
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), gpu_(nvidia_gpu_identify(gpu_device_id)) {
    if (gpu_.architecture == NvidiaGpuArchitecture::Unknown && boot0 != 0) gpu_ = nvidia_gpu_identify_boot0(boot0);
  }

  const char* name() const override { return "NVIDIA video engine (NVDEC/NVENC) (software path)"; }
  const NvidiaGpuInfo& gpu() const { return gpu_; }
  // Codecs the engine would serve once it is driven: today's sessions all run in software (info().accelerated is false).
  uint32_t engine_decode_codecs() const {
    return base_codecs() | (gpu_.media_hevc ? codec_mask(VideoCodec::Hevc) : 0) | (gpu_.media_vp9 ? codec_mask(VideoCodec::Vp9) : 0) |
           (gpu_.media_av1_decode ? codec_mask(VideoCodec::Av1) : 0);
  }
  uint32_t engine_encode_codecs() const {
    if (!gpu_.has_nvenc) return 0;
    return base_codecs() | codec_mask(VideoCodec::Hevc) | (gpu_.media_av1_encode ? codec_mask(VideoCodec::Av1) : 0);
  }

 private:
  // H.264 and JPEG are on every generation that has a video engine at all.
  uint32_t base_codecs() const {
    return nvidia_gpu_has_video_engine(gpu_.architecture) ? codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mjpeg) : 0;
  }

  NvidiaGpuInfo gpu_;
};

}  // namespace hardware
