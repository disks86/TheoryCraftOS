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
#include "Allocators/IAllocator.hpp"
#include "Crypto/Crypto.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnAv1Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnAvcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnHevcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnMpeg2Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVc1Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVp9Decoder.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// The check that decides whether the OS trusts the VCN engine to decode H.264: a short stream this OS encodes itself (a 64 x 64 picture, one I picture and four P
// pictures, baseline profile) is decoded by the CPU decoder and by the engine, and the pictures must be byte for byte the same. H.264 decoding is exact, so a
// difference means the engine was not driven correctly (a wrong address, a pitch the firmware reads another way, a reference it did not find) and it is not used.
//
// What passing proves: the engine took a session, a message, the bitstream and the buffers, found its references, and wrote linear NV12 where it was told. What it
// does not: CABAC, B pictures, weighted prediction, the deblocking filter (the stream switches it off), 8x8 transform, and picture sizes other than the test's.
class AmdVcnSelfTest {
 public:
  enum class Result : uint8_t { Passed, NoMemory, StreamFailed, SessionRefused, EngineFailed, Mismatch, HashMismatch };
  static constexpr uint32_t kPictures = 5;
  static constexpr uint32_t kSize = 64;

  // The stream: kPictures access units in Annex B, the first with the parameter sets. `ends[i]` is where access unit i ends in `stream` (access unit 0 starts at 0).
  struct Stream {
    explicit Stream(allocators::IAllocator& allocator) : bytes(allocator) {}
    ByteBuffer bytes;
    uint64_t ends[kPictures] = {};
    uint32_t count = 0;
    ConstByteView packet(uint32_t index) const {
      const uint64_t start = index == 0 ? 0 : ends[index - 1];
      return bytes.view().subview(start, ends[index] - start);
    }
  };
  static bool build_stream(allocators::IAllocator& allocator, Stream* stream);

  // Decodes the stream on the engine and on the CPU and compares. The packets go in with presentation times 0, 1000, 2000... microseconds.
  static Result run(allocators::IAllocator& allocator, const AmdVcnAvcDecoder::Engine& engine);
  // The same check on any engine's H.264 session (already prepared): `wedged(context)` says whether the engine's ring gave up. The UVD 6 engine's test is this too.
  static Result run_h264_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* context);
  // The same for H.265: a stream made by libx265 (64 x 64, six pictures, B pictures with reordering; AmdVcnSelfTestHevcStream.inc) is decoded by the CPU decoder and by
  // the engine, and the pictures must be byte for byte the same. What passing proves beyond the above: the reference picture set and slot bookkeeping of the H.265
  // message, 32x32 coding trees with the deblocking filter and SAO the encoder chose, bi-prediction. Not: scaling lists, tiles, slices, weighted prediction, other sizes.
  static Result run_hevc(allocators::IAllocator& allocator, const AmdVcnHevcDecoder::Engine& engine);
  // The same check on any engine's H.265 session (already prepared): `wedged(ring)` says whether the engine's ring gave up, `decoded(decoder)` how many pictures the
  // engine itself decoded (all of them must have gone through it).
  static Result run_hevc_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring, uint64_t (*decoded)(const void*), const void* decoder);
  // The MD5 the decoders check pictures with (see H265Decoder::set_plane_hasher).
  struct Md5PlaneHasher final : IPlaneHasher {
    void md5(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride, uint8_t out[16]) override {
      crypto::Hasher h(crypto::HashKind::Md5);
      for (uint32_t y = 0; y < height; ++y) h.update(ConstByteView(plane + uint64_t{y} * stride, width));
      h.finish(out);
    }
  };
  // The same for H.265 Main 10: a ten-bit libx265 stream (64 x 64, six pictures, B pictures with reordering; AmdVcnSelfTestHevc10Stream.inc). The CPU decoder cannot decode ten
  // bits, so the reference is the stream itself: it carries an MD5 of every decoded picture (an SEI message), which the decoder checks over the engine's pictures once the
  // session has been given an Md5PlaneHasher (`hashes(decoder)` returns the statistics). Passing means every picture went through the engine, came out as P010 and agreed with its CRC. What it proves beyond the 8-bit
  // test: the message's depth fields and the P010 surface, the slot sizes and context of Main 10. Not: other sizes, scaling lists, tiles.
  static Result run_hevc10_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring, uint64_t (*decoded)(const void*),
                                   H265Decoder::HashStatistics (*hashes)(const void*), const void* decoder);
  // The same for AV1: a libaom stream (64 x 64, ten temporal units with hidden frames and show_existing_frame; AmdVcnSelfTestAv1Stream.inc). What passing proves beyond
  // the above: the reference slot bookkeeping of the AV1 message (which engine picture each reference slot holds, across hidden frames and shown-again ones), the initial
  // entropy contexts, the tile table. Not: tiles, film grain, super-resolution, 10-bit, other sizes.
  static Result run_av1(allocators::IAllocator& allocator, const AmdVcnAv1Decoder::Engine& engine);
  // The same for VP9: a libvpx stream (64 x 64, ten packets with a hidden alt-ref frame and a frame shown again; AmdVcnSelfTestVp9Stream.inc). What passing proves beyond
  // the above: the reference slot bookkeeping of the VP9 message, the default probabilities in the context buffer and the table, the compressed header and tiles the
  // engine finds in the frame it is given. Not: segmentation, tiles, scaled references, intra-only frames, other sizes.
  static Result run_vp9(allocators::IAllocator& allocator, const AmdVcnVp9Decoder::Engine& engine);
  // The same for MPEG-2: an ffmpeg stream (64 x 64, eight pictures with B pictures and reordering; AmdVcnSelfTestMpeg2Stream.inc). The engine's inverse transform is not
  // the CPU decoder's, so the pictures must be close (a level or two apart), not identical. What passing proves beyond the above: the message with the matrices and the
  // picture coding parameters, the two reference slots of the picture buffer. Not: field pictures, dual prime, other sizes.
  static Result run_mpeg2(allocators::IAllocator& allocator, const AmdVcnMpeg2Decoder::Engine& engine);
  // The same check on any engine's MPEG-2 session (already prepared); see run_hevc_session for the callbacks.
  static Result run_mpeg2_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring, uint64_t (*decoded)(const void*), const void* decoder);
  // The same for MPEG-4 Part 2 on a UVD 6 session (already prepared, configured for I420 output): a stream from FFmpeg's encoder (32 x 32, eight pictures with B planes and
  // reordering; AmdVcnSelfTestMpeg4Stream.inc) goes in one plane per packet, and the pictures that come out must be, in display order, close to FFmpeg's decode of it (the engine's
  // inverse transform is not FFmpeg's: a level or two, not bit for bit). What passing proves: the layer parameters of the message, the job numbers of the two anchors, the output
  // order. Not: interlace, quarter-sample motion, custom matrices, other sizes. `wedged(ring)` and `decoded(decoder)` as for run_hevc_session.
  static Result run_mpeg4_session(allocators::IAllocator& allocator, IVideoDecoder& session, bool (*wedged)(const void*), const void* ring, uint64_t (*decoded)(const void*), const void* decoder);
  // The same for VC-1, with two streams (a Main profile one and an Advanced profile one, 64 x 64, eight pictures each with B pictures and reordering, made by
  // scripts/gen_vc1_synthetic.py; AmdVcnSelfTestVc1Stream.inc), both of which must pass. The pictures must be close to the CPU decoder's (nothing here has checked the
  // engine's VC-1 decoder against the standard: it is held to a level or two, which a wrong reference or a wrong table is far from). What passing proves beyond the
  // above: the profile and flags of the message (Simple and Main profile sizes in macroblocks), the frame data the firmware parses itself, the reference slots of the
  // picture buffer. Not: intensity compensation, interlace, range reduction, slices, other sizes.
  static Result run_vc1(allocators::IAllocator& allocator, const AmdVcnVc1Decoder::Engine& engine);
  // The same on any engine's VC-1 sessions. The test needs two (one per stream, the Main profile one given its sequence header as the configuration), so the engine's side is
  // a host that makes them: open() makes and prepares a session for the configuration (false: refused), close() ends it, wedged() and decoded() tell what the engine did.
  class Vc1SessionHost {
   public:
    virtual bool open(const DecoderConfig& config) = 0;
    virtual IVideoDecoder& session() = 0;
    virtual bool wedged() const = 0;
    virtual uint64_t decoded() const = 0;   // pictures the engine itself decoded in the open session
    virtual void close() = 0;

   protected:
    ~Vc1SessionHost() = default;
  };
  static Result run_vc1_sessions(allocators::IAllocator& allocator, Vc1SessionHost& host);
  static const char* result_name(Result result);
};

}  // namespace hardware
