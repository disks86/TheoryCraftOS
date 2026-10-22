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
#include <stdint.h>
#include "Media/Audio/OpusRange.hpp"

namespace media {
namespace opus {

// The SILK layer of Opus (RFC 6716 section 4.2): linear-prediction speech coding at 8, 12 or 16 kHz internally, resampled to 48 kHz.
// Integer arithmetic throughout, as in the reference decoder, so the output matches it exactly. See OpusSilk.cpp.
class SilkDecoder {
 public:
  SilkDecoder() { reset(); }  // channels_api_ and channels_internal_ start at 0 (member initialisers) and survive reset()
  void reset();
  // What the Opus layer tells SILK about the packet.
  struct Control {
    int channels_internal = 1;  // 1 or 2, as coded
    int channels_api = 1;       // output channels
    int internal_rate = 16000;  // 8000, 12000 or 16000
    int payload_ms = 20;        // 10, 20, 40 or 60
  };
  // Decodes SILK frames for the audio of one Opus frame from `dec`. `pcm` receives interleaved 16-bit samples at 48 kHz for
  // `channels_api` channels; the number of samples per channel decoded goes to *samples. Call again until the Opus frame is
  // filled (one 40 or 60 ms frame is returned as several 20 ms pieces). Returns 0 or an error code.
  int decode(const Control& control, RangeDecoder& dec, bool first_frame, int16_t* pcm, int* samples);

  static constexpr int kMaxLpcOrder = 16;
  static constexpr int kMaxFrameLength = 320;  // 20 ms at 16 kHz
  static constexpr int kMaxSubframes = 4;
  static constexpr int kMaxFramesPerPacket = 3;

 private:
  struct Resampler {
    int32_t sIIR[6];
    int16_t sFIR[16];
    int16_t delayBuf[48];
    int function, batch_size, fs_in_khz, fs_out_khz, input_delay;
    int32_t inv_ratio_q16;
  };
  struct Indices {
    int8_t gains[kMaxSubframes];
    int8_t ltp[kMaxSubframes];
    int8_t nlsf[kMaxLpcOrder + 1];
    int16_t lag;
    int8_t contour, signal_type, quant_offset_type, nlsf_interp_q2, per_index, ltp_scale_index, seed;
  };
  struct Channel {
    int32_t prev_gain_q16;
    int32_t exc_q14[kMaxFrameLength];
    int32_t slpc_q14_buf[kMaxLpcOrder];
    int16_t out_buf[kMaxFrameLength + 2 * 80];
    int lag_prev;
    int8_t last_gain_index;
    int fs_khz;
    int32_t fs_api_hz;
    int nb_subfr, frame_length, subfr_length, ltp_mem_length, lpc_order;
    int16_t prev_nlsf_q15[kMaxLpcOrder];
    int first_frame_after_reset;
    int pitch_contour_table;  // which of the four pitch contour tables
    int pitch_lag_low_bits;   // 4, 6 or 8: the uniform table for the low bits
    int frames_decoded, frames_per_packet;
    int ec_prev_signal_type;
    int16_t ec_prev_lag_index;
    int vad_flags[kMaxFramesPerPacket];
    int lbrr_flag;
    int lbrr_flags[kMaxFramesPerPacket];
    Resampler resampler;
    bool wideband_codebook;
    Indices indices;
    int prev_signal_type;
  };
  struct Control32 {
    int pitch_l[kMaxSubframes];
    int32_t gains_q16[kMaxSubframes];
    int16_t pred_coef_q12[2][kMaxLpcOrder];
    int16_t ltp_coef_q14[5 * kMaxSubframes];
    int ltp_scale_q14;
  };

  static void init_channel(Channel& c);
  static int set_fs(Channel& c, int fs_khz, int32_t fs_api_hz);
  static int resampler_init(Resampler& r, int fs_in, int fs_out);
  static void resample(Resampler& r, int16_t* out, const int16_t* in, int32_t in_len);
  static void decode_indices(Channel& c, RangeDecoder& dec, int frame_index, bool decode_lbrr, int cond_coding);
  static void decode_pulses(RangeDecoder& dec, int16_t* pulses, int signal_type, int quant_offset_type, int frame_length);
  static void decode_parameters(Channel& c, Control32& ctrl, int cond_coding);
  static void decode_core(Channel& c, const Control32& ctrl, int16_t* xq, const int16_t* pulses);
  static int decode_frame(Channel& c, RangeDecoder& dec, int16_t* out, int32_t* n, int cond_coding);

  Channel channel_[2];
  int16_t stereo_pred_prev_q13_[2], stereo_mid_[2], stereo_side_[2];
  int channels_api_ = 0, channels_internal_ = 0;
  int prev_decode_only_middle_ = 0;
};

}  // namespace opus
}  // namespace media
