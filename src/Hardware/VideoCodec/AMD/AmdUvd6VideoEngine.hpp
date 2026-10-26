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
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"
#include "Hardware/VideoCodec/AMD/AmdSmu7.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6AvcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6HevcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Mpeg2Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Mpeg4Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Vc1Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3VideoEngine.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTest.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnWorkMemory.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The video engine of the Radeon RX Vega M in a Kaby Lake-G machine (Dell XPS 15 9575; PCI 1002:694C / 694E / 694F): the Unified Video Decoder 6.3. The card's register BAR
// (BAR 5) and memory BAR (BAR 0) are mapped by the caller; this reads the memory controller (AmdGmc8Memory) to find a block of video memory nothing else uses, starts the
// engine with the microcode from /firmware (AmdUvd6), runs the decode ring's own test and an H.264 self-test (the same stream and comparison as the VCN engine's:
// AmdVcnSelfTest), and only when the engine's pictures are the CPU's byte for byte does it serve H.264 sessions (AmdUvd6AvcDecoder); H.265 (AmdUvd6HevcDecoder) has its own
// test and the same rule. Anything else, and every session while
// the engine is not up or failed a test, runs on the CPU through the software codecs. info().accelerated says false (the device as a whole is not accelerated);
// info().accelerated_decode_codecs lists what the engine really does.
//
// The card's H.264 encoder (VCE 3.4, AmdVce3VideoEngine) is the opt-in part of the same device (enable_encode()): its microcode is loaded with the decoder's, its block of video
// memory is carved from the end of the decode work area (so the two never share memory), it is started after the decoders and serves H.264 encode sessions when its own
// self-test passed. A VCE that does not come up leaves the decoders as they are and encoding on the CPU.
//
// Nothing here has run on a real card. What the start does not do is power the engine (the driver asks the card's SMC; a card that was not left with the engine powered
// makes the VCPU never answer, which the start reports and undoes), so it is only started when the memory plan is whole and the graphics block reports a plausible
// address configuration.
class AmdUvd6VideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x1002;

  explicit AmdUvd6VideoEngine(allocators::IAllocator& allocator, uint16_t gpu_device_id)
      : SoftwareVideoCodec(allocator, /*hardware_engine=*/true), allocator_(allocator), device_id_(gpu_device_id), firmware_(allocator) {}

  // The card's register BAR (BAR 5, at least kAmdGmc8RegisterBytes) and its memory BAR (BAR 0: the CPU address, and the size of the part of video memory it shows).
  // A window shorter than that leaves the engine without registers (plan_engine() says so).
  void set_registers(const MmioRegion& registers) {
    registers_ = nullptr;
    register_window_.reset();
    if (registers.length() < kAmdGmc8RegisterBytes) return;
    register_window_ = UniquePointer<MmioVcnRegisters>::make(allocator_, registers);
    registers_ = register_window_.get();
  }
  // The registers of the card through something other than a mapped BAR (a test's simulated card). Must outlive the engine.
  void set_register_file(IVcnRegisters& registers) { register_window_.reset(); registers_ = &registers; }
  void set_aperture(uint64_t physical, uint64_t bytes) { aperture_physical_ = physical; aperture_bytes_ = bytes; }
  void set_pci_address(const PciAddress& address) { address_ = address; has_address_ = true; }
  bool gpu_location(PciAddress* out) const override {
    if (!has_address_ || !out) return false;
    *out = address_;
    return true;
  }
  uint64_t gpu_memory_bytes() const override { return memory_map_.video_memory_bytes; }

  // Brings the VCE encoder up with the rest (the microcode, a block of video memory, the start and a self-test). Off by default: it has not run on a real card.
  void enable_encode(bool enable) {
    if (!enable) {
      vce_.reset();
      return;
    }
    if (!vce_) vce_ = UniquePointer<AmdVce3VideoEngine>::make(allocator_, allocator_, device_id_);
  }
  // The decoder's clock level asked of the SMC at the next start (the default is the top level only: the driver's stable-pstate case).
  void set_clock_level(AmdSmu7::UvdLevel level) { clock_level_ = level; }
  bool encode_enabled() const { return vce_ != nullptr; }
  bool encode_ready() const { return vce_ && vce_->ready(); }
  const char* encode_status() const { return vce_ ? vce_->status() : ""; }
  AmdVce3VideoEngine* encoder_engine() { return vce_.get(); }

  bool load_firmware(filesystem::IFileSystem& file_system) override {
    const char* name = amd_uvd6_firmware_name(device_id_);
    if (!name) return false;
    if (vce_) vce_->load_firmware(file_system);   // (its own status says what became of it; the decoder does not depend on it)
    firmware_name_ = name;
    firmware_state_ = firmware_.load(file_system, name);
    note_ = "engine not started";
    rebuild_status();
    return firmware_state_ == AmdFirmwareStatus::Ok;
  }
  const char* firmware_status() const override { return firmware_text_.c_str(); }

  // Reads the memory controller (reads only), chooses the engine's block, ring and work area, and maps them. plan_status() says where, or why not.
  bool plan_engine(const VideoEngineEnvironment& environment) override {
    planned_ = false;
    plan_text_.assign("");
    if (!amd_uvd6_firmware_name(device_id_)) return false;
    plan_text_.assign("engine memory not planned: ");
    if (!firmware_.loaded()) { plan_text_.append("no firmware"); return false; }
    if (!registers_) { plan_text_.append("no access to the registers of the card"); return false; }
    if (!environment.mapper || aperture_bytes_ == 0 || aperture_physical_ == 0) { plan_text_.append("no way to map the card's memory"); return false; }
    mapper_ = environment.mapper;
    memory_map_ = amd_read_gmc8_memory_map(*registers_);
    config_.max_handles = AmdUvd6::max_handles_for(firmware_);
    plan_ = amd_plan_uvd6_memory(memory_map_, aperture_bytes_, AmdUvd6::block_bytes(firmware_, config_.max_handles));
    if (plan_.result != AmdUvd6Plan::Result::Ok) {
      plan_text_.append(amd_uvd6_plan_result_name(plan_.result));
      return false;
    }
    // The encoder's memory is the end of the work area.
    bool carve_vce = false;
    AmdVce3VideoEngine::Region vce_region;
    if (vce_ && vce_->firmware_usable()) {
      const uint64_t reserve = AmdVce3VideoEngine::reserve_bytes();
      if (plan_.work_bytes >= reserve + kAmdUvd6MinWorkBytes) {
        plan_.work_bytes -= reserve;
        vce_region.gpu_address = plan_.work_gpu_address + plan_.work_bytes;
        vce_region.physical = aperture_physical_ + plan_.work_offset + plan_.work_bytes;
        vce_region.bytes = reserve;
        carve_vce = true;
      }
    }
    if (!amd_map_uvd6_plan(plan_, *environment.mapper, aperture_physical_, &memory_)) {
      plan_text_.append("the card's memory cannot be mapped (the OS owns it, or the mapper refuses)");
      return false;
    }
    config_.gb_addr_config = registers_->read(kGbAddrConfig);
    plan_text_.assign("engine memory: block at video memory +");
    plan_text_.append_uint(plan_.block_offset >> 20);
    plan_text_.append(" MiB (GPU 0x");
    append_hex(plan_text_, plan_.block_gpu_address);
    plan_text_.append("), ");
    plan_text_.append_uint(plan_.block_bytes >> 10);
    plan_text_.append(" KiB; work area ");
    plan_text_.append_uint(plan_.work_bytes >> 20);
    plan_text_.append(" MiB; gb_addr_config 0x");
    append_hex(plan_text_, config_.gb_addr_config);
    if (vce_) {
      plan_text_.append("; encoder: ");
      if (carve_vce && vce_->plan(*registers_, *environment.mapper, vce_region)) {
        plan_text_.append_uint(vce_region.bytes >> 20);
        plan_text_.append(" MiB");
      } else {
        plan_text_.append("no memory");
      }
    }
    planned_ = true;
    return true;
  }
  const char* plan_status() const override { return plan_text_.c_str(); }
  const AmdUvd6Plan& plan() const { return plan_; }
  bool engine_planned() const { return planned_; }

  // Starts the engine and brings up decoding, when the plan is whole and the graphics block's address configuration reads as a configured chip (not zero, not all ones,
  // which is what a block that was never set up, or a card that does not answer, reads as). Three tries for the VCPU to answer, not the driver's ten.
  bool start_hardware(const IClock& clock) override {
    const bool decoding = start_decode_hardware(clock);
    start_encode_hardware(clock);
    return decoding;
  }

  // The encoder, after the decoders (whatever became of them): its own start and self-test. True when H.264 encode sessions run on the engine.
  bool start_encode_hardware(const IClock& clock) {
    if (!vce_ || !vce_->planned()) return false;
    const bool ready = vce_->start(clock);
    info_.accelerated_encode_codecs = ready ? codec_mask(VideoCodec::H264) : 0;
    rebuild_status();
    return ready;
  }

  bool start_decode_hardware(const IClock& clock) {
    if (engine_running()) return true;
    clock_ = &clock;
    if (!planned_) return false;
    if (config_.gb_addr_config == 0 || config_.gb_addr_config == 0xFFFFFFFFu) {
      note_ = "engine not started: the graphics block did not report an address configuration";
      rebuild_status();
      return false;
    }
    config_.boot_attempts = kBootAttempts;
    uvd_ = UniquePointer<AmdUvd6>::make(allocator_, *registers_, clock);
    if (!uvd_) return false;
    request_power(clock);
    const AmdUvd6::Status status = uvd_->start(firmware_, memory_.engine, config_);
    if (status != AmdUvd6::Status::Ok) {
      const char* what = status == AmdUvd6::Status::TimedOut ? "engine start failed: VCPU did not answer" :
                         status == AmdUvd6::Status::BadMemory ? "engine start refused: bad memory block" : "engine start refused: unusable firmware";
      note_ = status == AmdUvd6::Status::TimedOut ? describe(what, uvd_->failure_snapshot()) : what;
      release_power(clock);
      uvd_.reset();
      rebuild_status();
      return false;
    }
    note_ = "engine started, idle";
    rebuild_status();
    if (!enable_decode(*registers_, clock, memory_.engine.ring, *mapper_, plan_.work_gpu_address, aperture_physical_ + plan_.work_offset, plan_.work_bytes)) return false;
    if (enable_hevc_decode()) enable_hevc10_decode();   // (H.264 works either way; ten bits is a test on top of the eight-bit one)
    if (engine_running()) enable_mpeg2_decode();
    if (engine_running() && enable_vc1_decode()) enable_vc1_interlaced_decode();
    if (engine_running()) enable_mpeg4_decode();
    return engine_running();
  }

  void shutdown() override {
    stop_encode();
    stop_engine();
  }
  void stop_encode() {
    info_.accelerated_encode_codecs = 0;
    if (vce_) vce_->stop();
  }
  void stop_engine() {
    decode_ready_ = false;
    hevc_ready_ = false;
    hevc10_ready_ = false;
    hevc10_note_.assign("");
    mpeg2_ready_ = false;
    vc1_ready_ = false;
    mpeg4_ready_ = false;
    mpeg4_note_.assign("");
    vc1_interlaced_ready_ = false;
    vc1_interlaced_note_.assign("");
    info_.accelerated_decode_codecs = 0;
    info_.decode_codecs &= ~codec_mask(VideoCodec::Mpeg4);
    ring_.reset();
    if (uvd_) {
      uvd_->stop();
      if (clock_) release_power(*clock_);
      uvd_.reset();
      note_ = "engine stopped";
      rebuild_status();
    }
  }
  bool engine_running() const { return uvd_ != nullptr; }

  // The decode path on a running engine: the work memory, the decode ring (the page AmdUvd6 programmed), the ring's own test, and the H.264 self-test. True when it all
  // passed and H.264 sessions run on the engine. Any failure stops the engine again; status says why.
  bool enable_decode(IVcnRegisters& registers, const IClock& clock, const MmioRegion& decode_ring, IRegisterMapper& mapper, uint64_t work_gpu, uint64_t work_physical,
                     uint64_t work_bytes) {
    decode_ready_ = false;
    hevc_ready_ = false;
    hevc10_ready_ = false;
    mpeg2_ready_ = false;
    vc1_ready_ = false;
    mpeg4_ready_ = false;
    vc1_interlaced_ready_ = false;
    info_.accelerated_decode_codecs = 0;
    info_.decode_codecs &= ~codec_mask(VideoCodec::Mpeg4);
    ring_.reset();
    const char* why = nullptr;
    bool tested = false;
    AmdVcnWorkMemory::Piece ib, fence;
    if (!work_.configure(&mapper, work_gpu, work_physical, work_bytes)) {
      why = "no room in video memory for decoding";
    } else if (!work_.allocate(AmdUvd6Ring::kIndirectBufferBytes, AmdVcnWorkMemory::kPage, &ib) || !work_.allocate(AmdUvd6Ring::kFenceBytes, AmdVcnWorkMemory::kPage, &fence)) {
      why = "the decode buffers could not be mapped";
    } else {
      work_.keep();
      AmdUvd6Ring::Memory ring_memory;
      ring_memory.ib_gpu_address = ib.gpu_address;
      ring_memory.ib = ib.cpu;
      ring_memory.fence_gpu_address = fence.gpu_address;
      ring_memory.fence = fence.cpu;
      ring_memory.ring = decode_ring;
      ring_ = UniquePointer<AmdUvd6Ring>::make(allocator_, registers, clock, ring_memory);
      if (!ring_) {
        why = "out of memory";
      } else if (!ring_->test()) {
        why = "the decode ring did not run a register write";
        ring_detail_ = AmdUvd6::read_diagnostics(registers);
        has_ring_detail_ = true;
      } else {
        engine_.ring = ring_.get();
        engine_.memory = &work_;
        engine_.stream_handle = 1;
        DecoderConfig config;
        config.codec = VideoCodec::H264;
        config.output = PixelFormat::I420;
        AmdUvd6AvcDecoder session(allocator_, engine_, config);
        AmdVcnSelfTest::Result result = AmdVcnSelfTest::Result::SessionRefused;
        if (session.prepare())
          result = AmdVcnSelfTest::run_h264_session(
              allocator_, session, [](const void* ring) { return static_cast<const AmdUvd6Ring*>(ring)->wedged(); }, ring_.get());
        if (result != AmdVcnSelfTest::Result::Passed) {
          why = AmdVcnSelfTest::result_name(result);
          tested = true;
          ring_detail_ = AmdUvd6::read_diagnostics(registers);
          has_ring_detail_ = true;
        }
      }
    }
    if (why) {
      decode_text_.assign(tested ? "H.264 self-test failed (" : "decoding not enabled (");
      decode_text_.append(has_ring_detail_ ? describe(why, ring_detail_) : why);
      decode_text_.append("); engine stopped");
      has_ring_detail_ = false;
      stop_engine();
      note_ = decode_text_.c_str();
      rebuild_status();
      return false;
    }
    decode_ready_ = true;
    info_.accelerated_decode_codecs = codec_mask(VideoCodec::H264);
    note_ = "engine started; H.264 decode runs on it (self-test passed)";
    rebuild_status();
    return true;
  }
  // After enable_decode(): runs the H.265 self-test (AmdVcnSelfTest::run_hevc_session, a libx265 stream with B pictures) and, when it passes, hands H.265 sessions to the
  // engine as well. A failed test leaves H.264 as it was - unless the engine stopped answering, which stops it altogether. True when H.265 is served by the engine now.
  bool enable_hevc_decode() {
    hevc_ready_ = false;
    hevc10_ready_ = false;
    hevc10_note_.assign("");
    if (!ring_) return false;
    AmdVcnSelfTest::Result result = AmdVcnSelfTest::Result::SessionRefused;
    {
      // (The session is closed before anything below can stop the engine and take the ring away.)
      DecoderConfig config;
      config.codec = VideoCodec::Hevc;
      config.output = PixelFormat::I420;
      AmdUvd6HevcDecoder session(allocator_, engine_, config);
      if (session.prepare())
        result = AmdVcnSelfTest::run_hevc_session(
            allocator_, session, [](const void* ring) { return static_cast<const AmdUvd6Ring*>(ring)->wedged(); }, ring_.get(),
            [](const void* decoder) { return static_cast<const AmdUvd6HevcDecoder*>(decoder)->pictures_decoded(); }, &session);
    }
    if (result != AmdVcnSelfTest::Result::Passed) {
      if (ring_->wedged()) {
        decode_ready_ = false;
        info_.accelerated_decode_codecs = 0;
        decode_text_.assign("H.265 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("); engine stopped");
        stop_engine();
      } else {
        decode_text_.assign("engine started; H.264 decode runs on it (self-test passed); H.265 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("), H.265 stays on the CPU");
      }
      note_ = decode_text_.c_str();
      rebuild_status();
      return false;
    }
    hevc_ready_ = true;
    info_.accelerated_decode_codecs |= codec_mask(VideoCodec::Hevc);
    note_ = "engine started; H.264 and H.265 decode run on it (self-tests passed)";
    rebuild_status();
    return true;
  }
  // After enable_hevc_decode(): the Main 10 test (AmdVcnSelfTest::run_hevc10_session: ten-bit libx265 stream, the engine's P010 pictures checked against the stream's own MD5s).
  // Passing lets H.265 sessions take ten-bit streams, which the CPU cannot decode at all; a failure leaves ten-bit streams undecoded and everything else as it was - unless
  // the engine stopped answering, which stops it altogether.
  bool enable_hevc10_decode() {
    hevc10_ready_ = false;
    if (!ring_ || !hevc_ready_) return false;
    AmdVcnSelfTest::Result result = AmdVcnSelfTest::Result::SessionRefused;
    {
      DecoderConfig config;
      config.codec = VideoCodec::Hevc;
      config.output = PixelFormat::P010;
      AmdUvd6HevcDecoder session(allocator_, engine_, config);
      AmdVcnSelfTest::Md5PlaneHasher hasher;
      session.allow_deep(true);
      session.set_plane_hasher(&hasher);
      if (session.prepare()) {
        result = AmdVcnSelfTest::run_hevc10_session(
            allocator_, session, [](const void* ring) { return static_cast<const AmdUvd6Ring*>(ring)->wedged(); }, ring_.get(),
            [](const void* decoder) { return static_cast<const AmdUvd6HevcDecoder*>(decoder)->pictures_decoded(); },
            [](const void* decoder) { return static_cast<const AmdUvd6HevcDecoder*>(decoder)->hash_statistics(); }, &session);
      }
    }
    if (result != AmdVcnSelfTest::Result::Passed) {
      if (ring_->wedged()) {
        decode_ready_ = false;
        hevc_ready_ = false;
        info_.accelerated_decode_codecs = 0;
        decode_text_.assign("Main 10 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("); engine stopped");
        stop_engine();
        note_ = decode_text_.c_str();
      } else {
        hevc10_note_.assign("Main 10 test failed (");   // (kept apart: the notes of the tests after this one replace note_)
        hevc10_note_.append(AmdVcnSelfTest::result_name(result));
        hevc10_note_.append(")");
      }
      rebuild_status();
      return false;
    }
    hevc10_ready_ = true;
    return true;
  }
  // After enable_decode(): the same for MPEG-2 (AmdVcnSelfTest::run_mpeg2_session: an ffmpeg stream with B pictures). The engine's inverse transform is not the CPU decoder's, so
  // its pictures must be close (a level or two apart), not identical. A failed test leaves the others as they were - unless the engine stopped answering.
  bool enable_mpeg2_decode() {
    mpeg2_ready_ = false;
    if (!ring_) return false;
    AmdVcnSelfTest::Result result = AmdVcnSelfTest::Result::SessionRefused;
    {
      DecoderConfig config;
      config.codec = VideoCodec::Mpeg2;
      config.output = PixelFormat::I420;
      AmdUvd6Mpeg2Decoder session(allocator_, engine_, config);
      if (session.prepare())
        result = AmdVcnSelfTest::run_mpeg2_session(
            allocator_, session, [](const void* ring) { return static_cast<const AmdUvd6Ring*>(ring)->wedged(); }, ring_.get(),
            [](const void* decoder) { return static_cast<const AmdUvd6Mpeg2Decoder*>(decoder)->pictures_decoded(); }, &session);
    }
    if (result != AmdVcnSelfTest::Result::Passed) {
      if (ring_->wedged()) {
        decode_ready_ = false;
        hevc_ready_ = false;
        info_.accelerated_decode_codecs = 0;
        decode_text_.assign("MPEG-2 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("); engine stopped");
        stop_engine();
      } else {
        decode_text_.assign(note_);
        decode_text_.append("; MPEG-2 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("), MPEG-2 stays on the CPU");
      }
      note_ = decode_text_.c_str();
      rebuild_status();
      return false;
    }
    mpeg2_ready_ = true;
    info_.accelerated_decode_codecs |= codec_mask(VideoCodec::Mpeg2);
    if (hevc_ready_) note_ = "engine started; H.264, H.265 and MPEG-2 decode run on it (self-tests passed)";   // (else the note says why H.265 stays on the CPU)
    rebuild_status();
    return true;
  }
  // After enable_decode(): the same for VC-1 (AmdVcnSelfTest::run_vc1_sessions: a Main profile and an Advanced profile stream, both must pass, each close to the CPU's pictures).
  bool enable_vc1_decode() {
    vc1_ready_ = false;
    if (!ring_) return false;
    // The test makes a session per stream; the host hands them out and drops each before the engine can be stopped.
    AmdVcnSelfTest::Result result;
    {
      Vc1Host host(allocator_, engine_, /*interlace=*/false);
      result = AmdVcnSelfTest::run_vc1_sessions(allocator_, host);
    }
    if (result != AmdVcnSelfTest::Result::Passed) {
      if (ring_->wedged()) {
        decode_ready_ = false;
        hevc_ready_ = false;
        mpeg2_ready_ = false;
        info_.accelerated_decode_codecs = 0;
        decode_text_.assign("VC-1 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("); engine stopped");
        stop_engine();
      } else {
        decode_text_.assign(note_);
        decode_text_.append("; VC-1 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("), VC-1 stays on the CPU");
      }
      note_ = decode_text_.c_str();
      rebuild_status();
      return false;
    }
    vc1_ready_ = true;
    info_.accelerated_decode_codecs |= codec_mask(VideoCodec::Vc1);
    if (hevc_ready_ && mpeg2_ready_) note_ = "engine started; H.264, H.265, MPEG-2 and VC-1 decode run on it (self-tests passed)";   // (else the note says why one stays on the CPU)
    rebuild_status();
    return true;
  }
  // After enable_vc1_decode(): the interlaced VC-1 test (AmdVcnSelfTest::run_vc1_interlaced_sessions: an Advanced profile stream in an interlaced sequence, close to the CPU's
  // pictures). Passing lets VC-1 sessions give interlaced sequences - frame and field coded pictures too, which the CPU cannot decode at all - to the engine; a failure leaves them
  // undecoded (the CPU decodes the progressive frames of such a sequence) and everything else as it was - unless the engine stopped answering.
  bool enable_vc1_interlaced_decode() {
    vc1_interlaced_ready_ = false;
    vc1_interlaced_note_.assign("");
    if (!ring_ || !vc1_ready_) return false;
    AmdVcnSelfTest::Result result;
    {
      Vc1Host host(allocator_, engine_, /*interlace=*/true);
      result = AmdVcnSelfTest::run_vc1_interlaced_sessions(allocator_, host);
    }
    if (result != AmdVcnSelfTest::Result::Passed) {
      if (ring_->wedged()) {
        decode_ready_ = false;
        hevc_ready_ = false;
        mpeg2_ready_ = false;
        vc1_ready_ = false;
        info_.accelerated_decode_codecs = 0;
        decode_text_.assign("Interlaced VC-1 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("); engine stopped");
        stop_engine();
        note_ = decode_text_.c_str();
      } else {
        vc1_interlaced_note_.assign("interlaced VC-1 test failed (");
        vc1_interlaced_note_.append(AmdVcnSelfTest::result_name(result));
        vc1_interlaced_note_.append(")");
      }
      rebuild_status();
      return false;
    }
    vc1_interlaced_ready_ = true;
    return true;
  }
  bool vc1_interlaced_ready() const { return vc1_interlaced_ready_; }
  // After enable_decode(): the same for MPEG-4 Part 2 (AmdVcnSelfTest::run_mpeg4_session: an FFmpeg-encoded stream with B planes, the engine's pictures held to FFmpeg's decode of
  // it). There is no CPU decoder for this codec, so a pass is what makes MPEG-4 decodable at all (info().decode_codecs gains it); a failure leaves it unsupported and the rest
  // as it was - unless the engine stopped answering, which stops it altogether.
  bool enable_mpeg4_decode() {
    mpeg4_ready_ = false;
    mpeg4_note_.assign("");
    if (!ring_) return false;
    AmdVcnSelfTest::Result result = AmdVcnSelfTest::Result::SessionRefused;
    {
      DecoderConfig config;
      config.codec = VideoCodec::Mpeg4;
      config.output = PixelFormat::I420;
      AmdUvd6Mpeg4Decoder session(allocator_, engine_, config);
      if (session.prepare())
        result = AmdVcnSelfTest::run_mpeg4_session(
            allocator_, session, [](const void* ring) { return static_cast<const AmdUvd6Ring*>(ring)->wedged(); }, ring_.get(),
            [](const void* decoder) { return static_cast<const AmdUvd6Mpeg4Decoder*>(decoder)->pictures_decoded(); }, &session);
    }
    if (result != AmdVcnSelfTest::Result::Passed) {
      if (ring_->wedged()) {
        decode_ready_ = false;
        hevc_ready_ = false;
        mpeg2_ready_ = false;
        vc1_ready_ = false;
        info_.accelerated_decode_codecs = 0;
        decode_text_.assign("MPEG-4 self-test failed (");
        decode_text_.append(AmdVcnSelfTest::result_name(result));
        decode_text_.append("); engine stopped");
        stop_engine();
        note_ = decode_text_.c_str();
      } else {
        mpeg4_note_.assign("MPEG-4 test failed (");
        mpeg4_note_.append(AmdVcnSelfTest::result_name(result));
        mpeg4_note_.append(")");
      }
      rebuild_status();
      return false;
    }
    mpeg4_ready_ = true;
    info_.decode_codecs |= codec_mask(VideoCodec::Mpeg4);
    info_.accelerated_decode_codecs |= codec_mask(VideoCodec::Mpeg4);
    rebuild_status();
    return true;
  }
  bool mpeg4_ready() const { return mpeg4_ready_; }
  bool decode_ready() const { return decode_ready_; }
  bool vc1_ready() const { return vc1_ready_; }
  bool mpeg2_ready() const { return mpeg2_ready_; }
  bool hevc_ready() const { return hevc_ready_; }
  bool hevc10_ready() const { return hevc10_ready_; }
  const char* engine_note() const { return note_; }

  // H.264, H.265, MPEG-2 and VC-1 go to the engine when they passed their tests and it is free (it runs one session at a time); anything else goes to the CPU.
  UniquePointer<IVideoDecoder> create_decoder(const DecoderConfig& config) override {
    if (config.codec == VideoCodec::Mpeg4 && mpeg4_ready_ && ring_ && !ring_->wedged()) {
      AmdUvd6AvcDecoder::Engine engine = engine_;
      engine.stream_handle = ++next_stream_handle_;
      UniquePointer<AmdUvd6Mpeg4Decoder> session = UniquePointer<AmdUvd6Mpeg4Decoder>::make(allocator_, allocator_, engine, config);
      if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
    }
    if (config.codec == VideoCodec::Vc1 && vc1_ready_ && ring_ && !ring_->wedged()) {
      AmdUvd6AvcDecoder::Engine engine = engine_;
      engine.stream_handle = ++next_stream_handle_;
      UniquePointer<AmdUvd6Vc1Decoder> session = UniquePointer<AmdUvd6Vc1Decoder>::make(allocator_, allocator_, engine, config);
      if (session) session->allow_interlace(vc1_interlaced_ready_);
      if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
    }
    if (config.codec == VideoCodec::Mpeg2 && mpeg2_ready_ && ring_ && !ring_->wedged()) {
      AmdUvd6AvcDecoder::Engine engine = engine_;
      engine.stream_handle = ++next_stream_handle_;
      UniquePointer<AmdUvd6Mpeg2Decoder> session = UniquePointer<AmdUvd6Mpeg2Decoder>::make(allocator_, allocator_, engine, config);
      if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
    }
    if (config.codec == VideoCodec::Hevc && hevc_ready_ && ring_ && !ring_->wedged()) {
      AmdUvd6AvcDecoder::Engine engine = engine_;
      engine.stream_handle = ++next_stream_handle_;
      UniquePointer<AmdUvd6HevcDecoder> session = UniquePointer<AmdUvd6HevcDecoder>::make(allocator_, allocator_, engine, config);
      if (session) session->allow_deep(hevc10_ready_);
      if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
    }
    if (config.codec == VideoCodec::H264 && decode_ready_ && ring_ && !ring_->wedged()) {
      AmdUvd6AvcDecoder::Engine engine = engine_;
      engine.stream_handle = ++next_stream_handle_;
      UniquePointer<AmdUvd6AvcDecoder> session = UniquePointer<AmdUvd6AvcDecoder>::make(allocator_, allocator_, engine, config);
      if (session && session->prepare()) return UniquePointer<IVideoDecoder>(util::move(session));
    }
    return SoftwareVideoCodec::create_decoder(config);
  }

  // H.264 goes to the VCE when it passed its self-test and is free (one session at a time); anything else, and every session while it is not up, runs on the CPU.
  UniquePointer<IVideoEncoder> create_encoder(const EncoderConfig& config) override {
    if (config.codec == VideoCodec::H264 && vce_ && vce_->ready()) {
      UniquePointer<IVideoEncoder> session = vce_->create_encoder(config);
      if (session) return session;
    }
    return SoftwareVideoCodec::create_encoder(config);
  }

  const char* name() const override { return "AMD video engine (UVD 6) (software path)"; }
  const char* engine_summary() const override { return summary_.c_str(); }
  const AmdVcnFirmware& firmware() const { return firmware_; }

 private:
  // The sessions of the VC-1 tests, handed out one at a time and dropped before the engine can be stopped; `interlace` lets them take interlaced sequences.
  class Vc1Host final : public AmdVcnSelfTest::Vc1SessionHost {
   public:
    Vc1Host(allocators::IAllocator& allocator, const AmdUvd6AvcDecoder::Engine& engine, bool interlace) : allocator_(allocator), engine_(engine), interlace_(interlace) {}
    bool open(const DecoderConfig& config) override {
      session_ = UniquePointer<AmdUvd6Vc1Decoder>::make(allocator_, allocator_, engine_, config);
      if (session_) session_->allow_interlace(interlace_);
      return session_ && session_->prepare();
    }
    IVideoDecoder& session() override { return *session_; }
    bool wedged() const override { return engine_.ring->wedged(); }
    uint64_t decoded() const override { return session_->pictures_decoded(); }
    void close() override { session_.reset(); }

   private:
    allocators::IAllocator& allocator_;
    AmdUvd6AvcDecoder::Engine engine_;
    bool interlace_;
    UniquePointer<AmdUvd6Vc1Decoder> session_;
  };

  static constexpr uint32_t kBootAttempts = 3;

  // "<what> [st=.. pw=.. rst=.. cn=.. lmi=.. rp=.. wp=..]": the registers of AmdUvd6::Diagnostics in hexadecimal, short enough for the status line.
  const char* describe(const char* what, const AmdUvd6::Diagnostics& d) {
    diag_text_.assign(what);
    diag_text_.append(" [st=0x");
    append_hex(diag_text_, d.status);
    diag_text_.append(" pw=0x");
    append_hex(diag_text_, d.power_status);
    diag_text_.append(" rst=0x");
    append_hex(diag_text_, d.soft_reset);
    diag_text_.append(" cn=0x");
    append_hex(diag_text_, d.vcpu_cntl);
    diag_text_.append(" lmi=0x");
    append_hex(diag_text_, d.lmi_ctrl2);
    diag_text_.append(" rp=0x");
    append_hex(diag_text_, d.rb_rptr);
    diag_text_.append(" wp=0x");
    append_hex(diag_text_, d.rb_wptr);
    diag_text_.append("]");
    return diag_text_.c_str();
  }

  // Asks the card's SMC to power the engine (the driver does this before it touches the engine). The start goes on whatever the answer: an SMC that is not running means the
  // system firmware left the card as it was, and the start itself shows whether the engine is up. Only an acknowledged request is undone at stop.
  void request_power(const IClock& clock) {
    AmdSmu7 smc(*registers_, clock);
    switch (smc.uvd_power(true)) {
      case AmdSmu7::Result::Ok: smc_powered_ = true; power_note_.assign("SMC powered the engine"); break;
      case AmdSmu7::Result::NotAnswering: power_note_.assign("SMC did not answer, engine assumed powered"); return;
      case AmdSmu7::Result::Unsupported: power_note_.assign("SMC does not know the power-on message, engine assumed powered"); break;
      case AmdSmu7::Result::Failed: power_note_.assign("SMC refused to power the engine"); break;
    }
    // The clock, once the SMC is there to ask (smu7_powergate_uvd(false): power, then the DPM table's boot level and DPM on).
    uint32_t level = 0;
    switch (smc.uvd_dpm_enable(clock_level_, &level)) {
      case AmdSmu7::DpmResult::Ok:
        dpm_enabled_ = true;
        power_note_.append(", level ");
        power_note_.append_uint(level);
        break;
      case AmdSmu7::DpmResult::NoDpmTable:
      case AmdSmu7::DpmResult::BadDpmTable: power_note_.append(", no DPM"); break;
      case AmdSmu7::DpmResult::NotAnswering:
      case AmdSmu7::DpmResult::Unsupported:
      case AmdSmu7::DpmResult::Failed: power_note_.append(", DPM refused"); break;
    }
  }
  void release_power(const IClock& clock) {
    if (!registers_) return;
    AmdSmu7 smc(*registers_, clock);
    if (dpm_enabled_) smc.uvd_dpm_disable();   // (before the power goes, as smu7_powergate_uvd(true) does)
    dpm_enabled_ = false;
    if (smc_powered_) smc.uvd_power(false);
    smc_powered_ = false;
  }
  static constexpr uint32_t kGbAddrConfig = 0x263e;   // mmGB_ADDR_CONFIG (gca/gfx_8_0_d.h)

  template <uint64_t N>
  static void append_hex(FixedString<N>& out, uint64_t value) {
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
      const uint32_t nibble = static_cast<uint32_t>((value >> shift) & 0xF);
      if (nibble == 0 && !started && shift != 0) continue;
      started = true;
      out.push_back("0123456789ABCDEF"[nibble]);
    }
  }
  void rebuild_status() {
    firmware_text_.assign(firmware_name_ ? firmware_name_ : "");
    firmware_text_.append(": ");
    if (firmware_state_ == AmdFirmwareStatus::Ok) {
      firmware_text_.append_uint(firmware_.ucode().size());
      firmware_text_.append(" bytes of microcode loaded, ");
      firmware_text_.append(note_);
      if (hevc10_note_.size() != 0) {
        firmware_text_.append("; ");
        firmware_text_.append(hevc10_note_.view());
      }
      if (vc1_interlaced_note_.size() != 0) {
        firmware_text_.append("; ");
        firmware_text_.append(vc1_interlaced_note_.view());
      }
      if (mpeg4_note_.size() != 0) {
        firmware_text_.append("; ");
        firmware_text_.append(mpeg4_note_.view());
      }
      if (power_note_.size() != 0) {
        firmware_text_.append("; ");
        firmware_text_.append(power_note_.view());
      }
    } else {
      firmware_text_.append(amd_firmware_status_name(firmware_state_));
      firmware_text_.append(" (expected /firmware/");
      firmware_text_.append(firmware_name_ ? firmware_name_ : "");
      firmware_text_.append(")");
    }
    summary_.assign("AMD Radeon RX Vega M (UVD 6.3), firmware ");
    summary_.append(firmware_text_.view());
  }

  allocators::IAllocator& allocator_;
  uint16_t device_id_ = 0;
  AmdVcnFirmware firmware_;
  AmdFirmwareStatus firmware_state_ = AmdFirmwareStatus::NotApplicable;
  const char* firmware_name_ = nullptr;
  FixedString<255> firmware_text_, summary_;
  FixedString<255> decode_text_, diag_text_;
  FixedString<127> power_note_;
  AmdSmu7::UvdLevel clock_level_ = AmdSmu7::UvdLevel::Highest;
  bool dpm_enabled_ = false;      // the SMC acknowledged the request for a clock level, so we ask it to switch DPM off again
  bool smc_powered_ = false;   // the SMC acknowledged our request to power the engine, so we ask it to power it down again
  const char* note_ = "engine not started";
  IVcnRegisters* registers_ = nullptr;
  UniquePointer<MmioVcnRegisters> register_window_;   // the mapped BAR, when that is what registers_ is
  uint64_t aperture_physical_ = 0, aperture_bytes_ = 0;
  PciAddress address_{};
  bool has_address_ = false;
  IRegisterMapper* mapper_ = nullptr;
  AmdGmc8MemoryMap memory_map_;
  AmdUvd6Plan plan_;
  AmdUvd6Memory memory_;
  AmdUvd6::Config config_;
  bool planned_ = false;
  FixedString<255> plan_text_;
  UniquePointer<AmdVce3VideoEngine> vce_;   // the encoder, when enabled
  UniquePointer<AmdUvd6> uvd_;
  UniquePointer<AmdUvd6Ring> ring_;
  AmdVcnWorkMemory work_;
  AmdUvd6AvcDecoder::Engine engine_;
  const IClock* clock_ = nullptr;
  AmdUvd6::Diagnostics ring_detail_;
  bool has_ring_detail_ = false;
  bool decode_ready_ = false;
  bool hevc_ready_ = false;
  bool hevc10_ready_ = false;   // ten-bit H.265 passed its test
  FixedString<95> hevc10_note_;   // why not, when it did not
  bool mpeg4_ready_ = false;
  FixedString<95> mpeg4_note_;
  bool vc1_interlaced_ready_ = false;   // interlaced VC-1 passed its test
  FixedString<95> vc1_interlaced_note_;
  bool mpeg2_ready_ = false;
  bool vc1_ready_ = false;
  uint32_t next_stream_handle_ = 1;
};

}  // namespace hardware
