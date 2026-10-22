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

// Opus SILK decoder (RFC 6716 section 4.2). A port of the decoder side of libopus v1.4's SILK (BSD-3-Clause; Copyright Skype Limited,
// Microsoft, Broadcom, Xiph.Org Foundation, Jean-Marc Valin, Koen Vos), integer arithmetic exactly as there, with its constant tables
// generated into OpusSilkTables.inc. Not done: packet loss concealment, comfort noise and in-band forward error correction (the
// redundant LBRR frames are parsed and skipped).
#include "Media/Audio/OpusSilk.hpp"

namespace media {
namespace opus {

#include "Media/Audio/OpusSilkTables.inc"

namespace {

constexpr int kTypeNoVoice = 0, kTypeUnvoiced = 1, kTypeVoiced = 2;
constexpr int kCodeIndependently = 0, kCodeIndependentlyNoLtpScaling = 1, kCodeConditionally = 2;
constexpr int kLtpOrder = 5, kMaxLpcOrder = 16, kMaxNbSubfr = 4, kShellLength = 16;
constexpr int kNLevelsQGain = 64, kMinDeltaGainQuant = -4, kMaxDeltaGainQuant = 36;
constexpr int kNlsfQuantMaxAmplitude = 4;
constexpr int kStereoInterpLenMs = 8;
constexpr int kMaxLpcStabilizeIterations = 16;

// ---- fixed-point primitives (SigProc_FIX.h, macros.h) ------------------------------------------------------------------------

inline int32_t wrap32(int64_t x) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(x))); }
inline int32_t add32(int32_t a, int32_t b) { return wrap32(int64_t{a} + b); }
inline int32_t sub32(int32_t a, int32_t b) { return wrap32(int64_t{a} - b); }
inline int32_t lshift32(int32_t a, int s) { return static_cast<int32_t>(static_cast<uint32_t>(a) << s); }
inline int32_t rshift32(int32_t a, int s) { return a >> s; }
inline int32_t smulwb(int32_t a, int32_t b) { return wrap32((int64_t{a} * static_cast<int16_t>(b)) >> 16); }
inline int32_t smlawb(int32_t a, int32_t b, int32_t c) { return wrap32(a + ((int64_t{b} * static_cast<int16_t>(c)) >> 16)); }
inline int32_t smulww(int32_t a, int32_t b) { return wrap32((int64_t{a} * b) >> 16); }
inline int32_t smlaww(int32_t a, int32_t b, int32_t c) { return wrap32(a + ((int64_t{b} * c) >> 16)); }
inline int32_t smulbb(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<int16_t>(b)); }
inline int32_t smlabb(int32_t a, int32_t b, int32_t c) { return add32(a, smulbb(b, c)); }
inline int32_t smmul(int32_t a, int32_t b) { return static_cast<int32_t>((int64_t{a} * b) >> 32); }
inline int64_t smull(int32_t a, int32_t b) { return int64_t{a} * b; }
inline int32_t mul32(int32_t a, int32_t b) { return wrap32(int64_t{a} * b); }
inline int32_t rshift_round(int32_t a, int s) { return s == 1 ? (a >> 1) + (a & 1) : ((a >> (s - 1)) + 1) >> 1; }
inline int64_t rshift_round64(int64_t a, int s) { return s == 1 ? (a >> 1) + (a & 1) : ((a >> (s - 1)) + 1) >> 1; }
inline int32_t sat16(int32_t a) { return a > 32767 ? 32767 : a < -32768 ? -32768 : a; }
inline int32_t abs32(int32_t a) { return a > 0 ? a : wrap32(-int64_t{a}); }
inline int32_t limit32(int32_t a, int32_t l1, int32_t l2) {
  return l1 > l2 ? (a > l1 ? l1 : (a < l2 ? l2 : a)) : (a > l2 ? l2 : (a < l1 ? l1 : a));
}
inline int ilog32(uint32_t v) { return RangeDecoder::ilog(v); }
inline int32_t clz32(int32_t in) { return in ? 32 - ilog32(static_cast<uint32_t>(in)) : 32; }
inline int32_t div32_16(int32_t a, int32_t b) { return a / b; }
inline int32_t add_sat32(int32_t a, int32_t b) {
  const int64_t s = int64_t{a} + b;
  return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : static_cast<int32_t>(s);
}
inline int32_t sub_sat32(int32_t a, int32_t b) {
  const int64_t s = int64_t{a} - b;
  return s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : static_cast<int32_t>(s);
}
inline int32_t lshift_sat32(int32_t a, int s) { return lshift32(limit32(a, INT32_MIN >> s, INT32_MAX >> s), s); }
inline int32_t ror32(int32_t a32, int rot) {
  const uint32_t x = static_cast<uint32_t>(a32), r = static_cast<uint32_t>(rot), m = static_cast<uint32_t>(-rot);
  if (rot == 0) return a32;
  if (rot < 0) return static_cast<int32_t>((x << m) | (x >> (32 - m)));
  return static_cast<int32_t>((x << (32 - r)) | (x >> r));
}
inline int32_t fix_const(double c, int q) { return static_cast<int32_t>(c * static_cast<double>(int64_t{1} << q) + 0.5); }

int32_t div32_varq(int32_t a32, int32_t b32, int qres) {
  const int a_headrm = clz32(abs32(a32)) - 1;
  int32_t a32_nrm = lshift32(a32, a_headrm);
  const int b_headrm = clz32(abs32(b32)) - 1;
  const int32_t b32_nrm = lshift32(b32, b_headrm);
  const int32_t b32_inv = div32_16(INT32_MAX >> 2, rshift32(b32_nrm, 16));
  int32_t result = smulwb(a32_nrm, b32_inv);
  a32_nrm = sub32(a32_nrm, lshift32(smmul(b32_nrm, result), 3));
  result = smlawb(result, a32_nrm, b32_inv);
  const int lshift = 29 + a_headrm - b_headrm - qres;
  if (lshift < 0) return lshift_sat32(result, -lshift);
  if (lshift < 32) return rshift32(result, lshift);
  return 0;
}

int32_t inverse32_varq(int32_t b32, int qres) {
  const int b_headrm = clz32(abs32(b32)) - 1;
  const int32_t b32_nrm = lshift32(b32, b_headrm);
  const int32_t b32_inv = div32_16(INT32_MAX >> 2, rshift32(b32_nrm, 16));
  int32_t result = lshift32(b32_inv, 16);
  const int32_t err_q32 = lshift32((static_cast<int32_t>(1) << 29) - smulwb(b32_nrm, b32_inv), 3);
  result = smlaww(result, err_q32, b32_inv);
  const int lshift = 61 - b_headrm - qres;
  if (lshift <= 0) return lshift_sat32(result, -lshift);
  if (lshift < 32) return rshift32(result, lshift);
  return 0;
}

int32_t lin2log(int32_t in_lin) {
  const int32_t lz = clz32(in_lin);
  const int32_t frac_q7 = ror32(in_lin, 24 - lz) & 0x7f;
  return add32(smlawb(frac_q7, mul32(frac_q7, 128 - frac_q7), 179), lshift32(31 - lz, 7));
}

int32_t log2lin(int32_t in_log_q7) {
  if (in_log_q7 < 0) return 0;
  if (in_log_q7 >= 3967) return INT32_MAX;
  int32_t out = lshift32(1, rshift32(in_log_q7, 7));
  const int32_t frac_q7 = in_log_q7 & 0x7F;
  if (in_log_q7 < 2048) {
    out = add32(out, rshift32(mul32(out, smlawb(frac_q7, smulbb(frac_q7, 128 - frac_q7), -174)), 7));
  } else {
    out = add32(out, mul32(rshift32(out, 7), smlawb(frac_q7, smulbb(frac_q7, 128 - frac_q7), -174)));
  }
  return out;
}

// ---- LPC helpers -------------------------------------------------------------------------------------------------------------

void bwexpander_32(int32_t* ar, int d, int32_t chirp_q16) {
  const int32_t chirp_minus_one_q16 = chirp_q16 - 65536;
  for (int i = 0; i < d - 1; i++) {
    ar[i] = smulww(chirp_q16, ar[i]);
    chirp_q16 += rshift_round(mul32(chirp_q16, chirp_minus_one_q16), 16);
  }
  ar[d - 1] = smulww(chirp_q16, ar[d - 1]);
}

void bwexpander(int16_t* ar, int d, int32_t chirp_q16) {
  const int32_t chirp_minus_one_q16 = chirp_q16 - 65536;
  for (int i = 0; i < d - 1; i++) {
    ar[i] = static_cast<int16_t>(rshift_round(mul32(chirp_q16, ar[i]), 16));
    chirp_q16 += rshift_round(mul32(chirp_q16, chirp_minus_one_q16), 16);
  }
  ar[d - 1] = static_cast<int16_t>(rshift_round(mul32(chirp_q16, ar[d - 1]), 16));
}

void lpc_fit(int16_t* a_qout, int32_t* a_qin, int qout, int qin, int d) {
  int i, k, idx = 0;
  for (i = 0; i < 10; i++) {
    int32_t maxabs = 0;
    for (k = 0; k < d; k++) {
      const int32_t absval = abs32(a_qin[k]);
      if (absval > maxabs) {
        maxabs = absval;
        idx = k;
      }
    }
    maxabs = rshift_round(maxabs, qin - qout);
    if (maxabs > 32767) {
      maxabs = maxabs < 163838 ? maxabs : 163838;
      const int32_t chirp_q16 = fix_const(0.999, 16) - (lshift32(maxabs - 32767, 14)) / rshift32(mul32(maxabs, idx + 1), 2);
      bwexpander_32(a_qin, d, chirp_q16);
    } else {
      break;
    }
  }
  if (i == 10) {
    for (k = 0; k < d; k++) {
      a_qout[k] = static_cast<int16_t>(sat16(rshift_round(a_qin[k], qin - qout)));
      a_qin[k] = lshift32(static_cast<int32_t>(a_qout[k]), qin - qout);
    }
  } else {
    for (k = 0; k < d; k++) a_qout[k] = static_cast<int16_t>(rshift_round(a_qin[k], qin - qout));
  }
}

int32_t lpc_inverse_pred_gain_qa(int32_t* A_QA, int order) {
  constexpr int QA = 24;
  const int32_t a_limit = fix_const(0.99975, QA);
  int32_t inv_gain_q30 = fix_const(1, 30);
  int k;
  for (k = order - 1; k > 0; k--) {
    if (A_QA[k] > a_limit || A_QA[k] < -a_limit) return 0;
    const int32_t rc_q31 = -lshift32(A_QA[k], 31 - QA);
    const int32_t rc_mult1_q30 = sub32(fix_const(1, 30), smmul(rc_q31, rc_q31));
    inv_gain_q30 = lshift32(smmul(inv_gain_q30, rc_mult1_q30), 2);
    if (inv_gain_q30 < fix_const(1.0f / 1e4f, 30)) return 0;
    const int mult2q = 32 - clz32(abs32(rc_mult1_q30));
    const int32_t rc_mult2 = inverse32_varq(rc_mult1_q30, mult2q + 30);
    for (int n = 0; n < (k + 1) >> 1; n++) {
      const int32_t tmp1 = A_QA[n], tmp2 = A_QA[k - n - 1];
      int64_t tmp64 = rshift_round64(smull(sub_sat32(tmp1, static_cast<int32_t>(rshift_round64(smull(tmp2, rc_q31), 31))), rc_mult2), mult2q);
      if (tmp64 > INT32_MAX || tmp64 < INT32_MIN) return 0;
      A_QA[n] = static_cast<int32_t>(tmp64);
      tmp64 = rshift_round64(smull(sub_sat32(tmp2, static_cast<int32_t>(rshift_round64(smull(tmp1, rc_q31), 31))), rc_mult2), mult2q);
      if (tmp64 > INT32_MAX || tmp64 < INT32_MIN) return 0;
      A_QA[k - n - 1] = static_cast<int32_t>(tmp64);
    }
  }
  if (A_QA[k] > a_limit || A_QA[k] < -a_limit) return 0;
  const int32_t rc_q31 = -lshift32(A_QA[0], 31 - QA);
  const int32_t rc_mult1_q30 = sub32(fix_const(1, 30), smmul(rc_q31, rc_q31));
  inv_gain_q30 = lshift32(smmul(inv_gain_q30, rc_mult1_q30), 2);
  if (inv_gain_q30 < fix_const(1.0f / 1e4f, 30)) return 0;
  return inv_gain_q30;
}

int32_t lpc_inverse_pred_gain(const int16_t* A_Q12, int order) {
  int32_t Atmp_QA[16];
  int32_t dc_resp = 0;
  for (int k = 0; k < order; k++) {
    dc_resp += static_cast<int32_t>(A_Q12[k]);
    Atmp_QA[k] = lshift32(static_cast<int32_t>(A_Q12[k]), 24 - 12);
  }
  if (dc_resp >= 4096) return 0;
  return lpc_inverse_pred_gain_qa(Atmp_QA, order);
}

void lpc_analysis_filter(int16_t* out, const int16_t* in, const int16_t* B, int32_t len, int32_t d) {
  for (int ix = d; ix < len; ix++) {
    const int16_t* in_ptr = &in[ix - 1];
    int32_t out32_q12 = smulbb(in_ptr[0], B[0]);
    out32_q12 = add32(out32_q12, smulbb(in_ptr[-1], B[1]));
    out32_q12 = add32(out32_q12, smulbb(in_ptr[-2], B[2]));
    out32_q12 = add32(out32_q12, smulbb(in_ptr[-3], B[3]));
    out32_q12 = add32(out32_q12, smulbb(in_ptr[-4], B[4]));
    out32_q12 = add32(out32_q12, smulbb(in_ptr[-5], B[5]));
    for (int j = 6; j < d; j += 2) {
      out32_q12 = add32(out32_q12, smulbb(in_ptr[-j], B[j]));
      out32_q12 = add32(out32_q12, smulbb(in_ptr[-j - 1], B[j + 1]));
    }
    out32_q12 = sub32(lshift32(static_cast<int32_t>(in_ptr[1]), 12), out32_q12);
    out[ix] = static_cast<int16_t>(sat16(rshift_round(out32_q12, 12)));
  }
  for (int i = 0; i < d; i++) out[i] = 0;
}

void insertion_sort_increasing_all_values_int16(int16_t* a, int L) {
  for (int i = 1; i < L; i++) {
    const int value = a[i];
    int j;
    for (j = i - 1; j >= 0 && value < a[j]; j--) a[j + 1] = a[j];
    a[j + 1] = static_cast<int16_t>(value);
  }
}

// ---- NLSF (normalised line spectral frequencies) -----------------------------------------------------------------------------

struct NlsfCodebook {
  int n_vectors, order;
  int32_t quant_step_size_q16, inv_quant_step_size_q6;
  const uint8_t* cb1_nlsf_q8;
  const int16_t* cb1_wght_q9;
  const uint8_t* cb1_icdf;
  const uint8_t* pred_q8;
  const uint8_t* ec_sel;
  const uint8_t* ec_icdf;
  const uint8_t* ec_rates_q5;
  const int16_t* delta_min_q15;
};

const NlsfCodebook& nlsf_codebook(bool wideband) {
  static const NlsfCodebook nb = {32, 10, fix_const(0.18, 16), fix_const(1.0 / 0.18, 6), k_silk_NLSF_CB1_NB_MB_Q8, k_silk_NLSF_CB1_Wght_Q9,
                                  k_silk_NLSF_CB1_iCDF_NB_MB, k_silk_NLSF_PRED_NB_MB_Q8, k_silk_NLSF_CB2_SELECT_NB_MB,
                                  k_silk_NLSF_CB2_iCDF_NB_MB, k_silk_NLSF_CB2_BITS_NB_MB_Q5, k_silk_NLSF_DELTA_MIN_NB_MB_Q15};
  static const NlsfCodebook wb = {32, 16, fix_const(0.15, 16), fix_const(1.0 / 0.15, 6), k_silk_NLSF_CB1_WB_Q8, k_silk_NLSF_CB1_WB_Wght_Q9,
                                  k_silk_NLSF_CB1_iCDF_WB, k_silk_NLSF_PRED_WB_Q8, k_silk_NLSF_CB2_SELECT_WB,
                                  k_silk_NLSF_CB2_iCDF_WB, k_silk_NLSF_CB2_BITS_WB_Q5, k_silk_NLSF_DELTA_MIN_WB_Q15};
  return wideband ? wb : nb;
}

void nlsf_unpack(int16_t* ec_ix, uint8_t* pred_q8, const NlsfCodebook& cb, int cb1_index) {
  const uint8_t* ec_sel_ptr = &cb.ec_sel[cb1_index * cb.order / 2];
  for (int i = 0; i < cb.order; i += 2) {
    const uint8_t entry = *ec_sel_ptr++;
    ec_ix[i] = static_cast<int16_t>(smulbb(rshift32(entry, 1) & 7, 2 * kNlsfQuantMaxAmplitude + 1));
    pred_q8[i] = cb.pred_q8[i + (entry & 1) * (cb.order - 1)];
    ec_ix[i + 1] = static_cast<int16_t>(smulbb(rshift32(entry, 5) & 7, 2 * kNlsfQuantMaxAmplitude + 1));
    pred_q8[i + 1] = cb.pred_q8[i + (rshift32(entry, 4) & 1) * (cb.order - 1) + 1];
  }
}

void nlsf_stabilize(int16_t* NLSF_Q15, const int16_t* NDeltaMin_Q15, int L) {
  int i, I = 0, k, loops;
  for (loops = 0; loops < 20; loops++) {
    int32_t min_diff_q15 = NLSF_Q15[0] - NDeltaMin_Q15[0];
    I = 0;
    for (i = 1; i <= L - 1; i++) {
      const int32_t diff_q15 = NLSF_Q15[i] - (NLSF_Q15[i - 1] + NDeltaMin_Q15[i]);
      if (diff_q15 < min_diff_q15) {
        min_diff_q15 = diff_q15;
        I = i;
      }
    }
    const int32_t diff_q15 = (1 << 15) - (NLSF_Q15[L - 1] + NDeltaMin_Q15[L]);
    if (diff_q15 < min_diff_q15) {
      min_diff_q15 = diff_q15;
      I = L;
    }
    if (min_diff_q15 >= 0) return;
    if (I == 0) {
      NLSF_Q15[0] = NDeltaMin_Q15[0];
    } else if (I == L) {
      NLSF_Q15[L - 1] = static_cast<int16_t>((1 << 15) - NDeltaMin_Q15[L]);
    } else {
      int32_t min_center_q15 = 0;
      for (k = 0; k < I; k++) min_center_q15 += NDeltaMin_Q15[k];
      min_center_q15 += rshift32(NDeltaMin_Q15[I], 1);
      int32_t max_center_q15 = 1 << 15;
      for (k = L; k > I; k--) max_center_q15 -= NDeltaMin_Q15[k];
      max_center_q15 -= rshift32(NDeltaMin_Q15[I], 1);
      const int16_t center_freq_q15 =
          static_cast<int16_t>(limit32(rshift_round(static_cast<int32_t>(NLSF_Q15[I - 1]) + static_cast<int32_t>(NLSF_Q15[I]), 1), min_center_q15, max_center_q15));
      NLSF_Q15[I - 1] = static_cast<int16_t>(center_freq_q15 - rshift32(NDeltaMin_Q15[I], 1));
      NLSF_Q15[I] = static_cast<int16_t>(NLSF_Q15[I - 1] + NDeltaMin_Q15[I]);
    }
  }
  insertion_sort_increasing_all_values_int16(&NLSF_Q15[0], L);
  NLSF_Q15[0] = static_cast<int16_t>(NLSF_Q15[0] > NDeltaMin_Q15[0] ? NLSF_Q15[0] : NDeltaMin_Q15[0]);
  for (i = 1; i < L; i++) {
    const int sum = sat16(NLSF_Q15[i - 1] + NDeltaMin_Q15[i]);
    NLSF_Q15[i] = static_cast<int16_t>(NLSF_Q15[i] > sum ? NLSF_Q15[i] : sum);
  }
  NLSF_Q15[L - 1] = static_cast<int16_t>(NLSF_Q15[L - 1] < (1 << 15) - NDeltaMin_Q15[L] ? NLSF_Q15[L - 1] : (1 << 15) - NDeltaMin_Q15[L]);
  for (i = L - 2; i >= 0; i--) {
    const int v = NLSF_Q15[i + 1] - NDeltaMin_Q15[i + 1];
    NLSF_Q15[i] = static_cast<int16_t>(NLSF_Q15[i] < v ? NLSF_Q15[i] : v);
  }
}

void nlsf_decode(int16_t* pNLSF_Q15, const int8_t* NLSFIndices, const NlsfCodebook& cb) {
  uint8_t pred_q8[kMaxLpcOrder];
  int16_t ec_ix[kMaxLpcOrder], res_q10[kMaxLpcOrder];
  nlsf_unpack(ec_ix, pred_q8, cb, NLSFIndices[0]);
  {  // silk_NLSF_residual_dequant
    int32_t out_q10 = 0;
    for (int i = cb.order - 1; i >= 0; i--) {
      const int32_t pred_q10 = rshift32(smulbb(out_q10, static_cast<int16_t>(pred_q8[i])), 8);
      out_q10 = lshift32(NLSFIndices[1 + i], 10);
      if (out_q10 > 0) out_q10 = static_cast<int16_t>(out_q10 - fix_const(0.1, 10));
      else if (out_q10 < 0) out_q10 = static_cast<int16_t>(out_q10 + fix_const(0.1, 10));
      out_q10 = smlawb(pred_q10, out_q10, cb.quant_step_size_q16);
      res_q10[i] = static_cast<int16_t>(out_q10);
    }
  }
  const uint8_t* pCB_element = &cb.cb1_nlsf_q8[NLSFIndices[0] * cb.order];
  const int16_t* pCB_Wght_Q9 = &cb.cb1_wght_q9[NLSFIndices[0] * cb.order];
  for (int i = 0; i < cb.order; i++) {
    const int32_t tmp = add32(div32_16(lshift32(static_cast<int32_t>(res_q10[i]), 14), pCB_Wght_Q9[i]), lshift32(static_cast<int16_t>(pCB_element[i]), 7));
    pNLSF_Q15[i] = static_cast<int16_t>(limit32(tmp, 0, 32767));
  }
  nlsf_stabilize(pNLSF_Q15, cb.delta_min_q15, cb.order);
}

void nlsf2a_find_poly(int32_t* out, const int32_t* cLSF, int dd) {
  constexpr int QA = 16;
  out[0] = lshift32(1, QA);
  out[1] = -cLSF[0];
  for (int k = 1; k < dd; k++) {
    const int32_t ftmp = cLSF[2 * k];
    out[k + 1] = sub32(lshift32(out[k - 1], 1), static_cast<int32_t>(rshift_round64(smull(ftmp, out[k]), QA)));
    for (int n = k; n > 1; n--) out[n] = add32(out[n], sub32(out[n - 2], static_cast<int32_t>(rshift_round64(smull(ftmp, out[n - 1]), QA))));
    out[1] = sub32(out[1], ftmp);
  }
}

void nlsf2a(int16_t* a_Q12, const int16_t* NLSF, int d) {
  constexpr int QA = 16;
  static const uint8_t ordering16[16] = {0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
  static const uint8_t ordering10[10] = {0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
  const uint8_t* ordering = d == 16 ? ordering16 : ordering10;
  int32_t cos_LSF_QA[16], P[9], Q[9], a32_QA1[16];
  for (int k = 0; k < d; k++) {
    const int32_t f_int = rshift32(NLSF[k], 15 - 7);
    const int32_t f_frac = NLSF[k] - lshift32(f_int, 15 - 7);
    const int32_t cos_val = k_silk_LSFCosTab_FIX_Q12[f_int];
    const int32_t delta = k_silk_LSFCosTab_FIX_Q12[f_int + 1] - cos_val;
    cos_LSF_QA[ordering[k]] = rshift_round(lshift32(cos_val, 8) + mul32(delta, f_frac), 20 - QA);
  }
  const int dd = rshift32(d, 1);
  nlsf2a_find_poly(P, &cos_LSF_QA[0], dd);
  nlsf2a_find_poly(Q, &cos_LSF_QA[1], dd);
  for (int k = 0; k < dd; k++) {
    const int32_t Ptmp = P[k + 1] + P[k];
    const int32_t Qtmp = Q[k + 1] - Q[k];
    a32_QA1[k] = -Qtmp - Ptmp;
    a32_QA1[d - k - 1] = Qtmp - Ptmp;
  }
  lpc_fit(a_Q12, a32_QA1, 12, QA + 1, d);
  for (int i = 0; lpc_inverse_pred_gain(a_Q12, d) == 0 && i < kMaxLpcStabilizeIterations; i++) {
    bwexpander_32(a32_QA1, d, 65536 - lshift32(2, i));
    for (int k = 0; k < d; k++) a_Q12[k] = static_cast<int16_t>(rshift_round(a32_QA1[k], QA + 1 - 12));
  }
}

// ---- gains and pitch ------------------------------------------------------------------------------------------------------------

void gains_dequant(int32_t gain_q16[kMaxNbSubfr], const int8_t ind[kMaxNbSubfr], int8_t* prev_ind, bool conditional, int nb_subfr) {
  constexpr int OFFSET = (2 * 128) / 6 + 16 * 128;
  constexpr int INV_SCALE_Q16 = (65536 * (((88 - 2) * 128) / 6)) / (kNLevelsQGain - 1);
  for (int k = 0; k < nb_subfr; k++) {
    if (k == 0 && !conditional) {
      *prev_ind = static_cast<int8_t>(ind[k] > *prev_ind - 16 ? ind[k] : *prev_ind - 16);
    } else {
      const int ind_tmp = ind[k] + kMinDeltaGainQuant;
      const int double_step_size_threshold = 2 * kMaxDeltaGainQuant - kNLevelsQGain + *prev_ind;
      if (ind_tmp > double_step_size_threshold) *prev_ind = static_cast<int8_t>(*prev_ind + (lshift32(ind_tmp, 1) - double_step_size_threshold));
      else *prev_ind = static_cast<int8_t>(*prev_ind + ind_tmp);
    }
    *prev_ind = static_cast<int8_t>(*prev_ind < 0 ? 0 : *prev_ind > kNLevelsQGain - 1 ? kNLevelsQGain - 1 : *prev_ind);
    const int32_t v = smulwb(INV_SCALE_Q16, *prev_ind) + OFFSET;
    gain_q16[k] = log2lin(v < 3967 ? v : 3967);
  }
}

void decode_pitch(int16_t lag_index, int8_t contour_index, int* pitch_lags, int fs_khz, int nb_subfr) {
  const int8_t* cb;
  int cbk_size;
  if (fs_khz == 8) {
    if (nb_subfr == 4) {
      cb = k_silk_CB_lags_stage2;
      cbk_size = 11;
    } else {
      cb = k_silk_CB_lags_stage2_10_ms;
      cbk_size = 3;
    }
  } else if (nb_subfr == 4) {
    cb = k_silk_CB_lags_stage3;
    cbk_size = 34;
  } else {
    cb = k_silk_CB_lags_stage3_10_ms;
    cbk_size = 12;
  }
  const int min_lag = smulbb(2, fs_khz), max_lag = smulbb(18, fs_khz);
  const int lag = min_lag + lag_index;
  for (int k = 0; k < nb_subfr; k++) {
    int v = lag + cb[k * cbk_size + contour_index];
    pitch_lags[k] = v < min_lag ? min_lag : v > max_lag ? max_lag : v;
  }
}

}  // namespace

// ---- the decoder -------------------------------------------------------------------------------------------------------------

void SilkDecoder::reset() {
  for (Channel& c : channel_) init_channel(c);
  for (int i = 0; i < 2; i++) stereo_pred_prev_q13_[i] = stereo_mid_[i] = stereo_side_[i] = 0;
  prev_decode_only_middle_ = 0;
}

void SilkDecoder::init_channel(Channel& c) {
  c = Channel();
  c.first_frame_after_reset = 1;
  c.prev_gain_q16 = 65536;
}

int SilkDecoder::resampler_init(Resampler& r, int fs_in, int fs_out) {
  r = Resampler();
  if ((fs_in != 8000 && fs_in != 12000 && fs_in != 16000) || fs_out != 48000) return -1;
  static const int8_t delay_dec_to_48[3] = {0, 4, 7};  // delay_matrix_dec[in][48 kHz]
  r.input_delay = delay_dec_to_48[fs_in == 8000 ? 0 : fs_in == 12000 ? 1 : 2];
  r.fs_in_khz = fs_in / 1000;
  r.fs_out_khz = fs_out / 1000;
  r.batch_size = r.fs_in_khz * 10;
  r.function = 2;  // IIR/FIR: a fixed-point up2 filter, then fractional FIR interpolation
  const int up2x = 1;
  r.inv_ratio_q16 = lshift32(div32_16(lshift32(fs_in, 14 + up2x), fs_out) /*silk_DIV32*/, 2);
  while (smulww(r.inv_ratio_q16, fs_out) < lshift32(fs_in, up2x)) r.inv_ratio_q16++;
  return 0;
}

namespace {

void resampler_up2_hq(int32_t* S, int16_t* out, const int16_t* in, int32_t len) {
  static const int16_t up2_hq_0[3] = {1746, 14986, 39083 - 65536};
  static const int16_t up2_hq_1[3] = {6854, 25769, 55542 - 65536};
  for (int32_t k = 0; k < len; k++) {
    const int32_t in32 = lshift32(static_cast<int32_t>(in[k]), 10);
    int32_t Y = sub32(in32, S[0]);
    int32_t X = smulwb(Y, up2_hq_0[0]);
    int32_t out32_1 = add32(S[0], X);
    S[0] = add32(in32, X);
    Y = sub32(out32_1, S[1]);
    X = smulwb(Y, up2_hq_0[1]);
    int32_t out32_2 = add32(S[1], X);
    S[1] = add32(out32_1, X);
    Y = sub32(out32_2, S[2]);
    X = smlawb(Y, Y, up2_hq_0[2]);
    out32_1 = add32(S[2], X);
    S[2] = add32(out32_2, X);
    out[2 * k] = static_cast<int16_t>(sat16(rshift_round(out32_1, 10)));
    Y = sub32(in32, S[3]);
    X = smulwb(Y, up2_hq_1[0]);
    out32_1 = add32(S[3], X);
    S[3] = add32(in32, X);
    Y = sub32(out32_1, S[4]);
    X = smulwb(Y, up2_hq_1[1]);
    out32_2 = add32(S[4], X);
    S[4] = add32(out32_1, X);
    Y = sub32(out32_2, S[5]);
    X = smlawb(Y, Y, up2_hq_1[2]);
    out32_1 = add32(S[5], X);
    S[5] = add32(out32_2, X);
    out[2 * k + 1] = static_cast<int16_t>(sat16(rshift_round(out32_1, 10)));
  }
}

}  // namespace

void SilkDecoder::resample(Resampler& S, int16_t* out, const int16_t* in, int32_t in_len) {
  // The first Fs_in_kHz input samples go through the one-sample-delayed buffer, the rest straight from the input.
  const int n_samples = S.fs_in_khz - S.input_delay;
  for (int i = 0; i < n_samples; i++) S.delayBuf[S.input_delay + i] = in[i];
  auto iir_fir = [&](int16_t* o, const int16_t* inp, int32_t len) {
    int16_t buf[2 * 160 + 8];
    for (int i = 0; i < 8; i++) buf[i] = S.sFIR[i];
    const int32_t index_increment_q16 = S.inv_ratio_q16;
    for (;;) {
      const int32_t n_in = len < S.batch_size ? len : S.batch_size;
      resampler_up2_hq(S.sIIR, &buf[8], inp, n_in);
      const int32_t max_index_q16 = lshift32(n_in, 16 + 1);
      for (int32_t index_q16 = 0; index_q16 < max_index_q16; index_q16 += index_increment_q16) {
        const int32_t table_index = smulwb(index_q16 & 0xFFFF, 12);
        const int16_t* buf_ptr = &buf[index_q16 >> 16];
        int32_t res_q15 = smulbb(buf_ptr[0], k_silk_resampler_frac_FIR_12[table_index * 4 + 0]);
        res_q15 = smlabb(res_q15, buf_ptr[1], k_silk_resampler_frac_FIR_12[table_index * 4 + 1]);
        res_q15 = smlabb(res_q15, buf_ptr[2], k_silk_resampler_frac_FIR_12[table_index * 4 + 2]);
        res_q15 = smlabb(res_q15, buf_ptr[3], k_silk_resampler_frac_FIR_12[table_index * 4 + 3]);
        res_q15 = smlabb(res_q15, buf_ptr[4], k_silk_resampler_frac_FIR_12[(11 - table_index) * 4 + 3]);
        res_q15 = smlabb(res_q15, buf_ptr[5], k_silk_resampler_frac_FIR_12[(11 - table_index) * 4 + 2]);
        res_q15 = smlabb(res_q15, buf_ptr[6], k_silk_resampler_frac_FIR_12[(11 - table_index) * 4 + 1]);
        res_q15 = smlabb(res_q15, buf_ptr[7], k_silk_resampler_frac_FIR_12[(11 - table_index) * 4 + 0]);
        *o++ = static_cast<int16_t>(sat16(rshift_round(res_q15, 15)));
      }
      inp += n_in;
      len -= n_in;
      if (len > 0) {
        for (int i = 0; i < 8; i++) buf[i] = buf[(n_in << 1) + i];
      } else {
        for (int i = 0; i < 8; i++) S.sFIR[i] = buf[(n_in << 1) + i];
        break;
      }
    }
  };
  iir_fir(out, S.delayBuf, S.fs_in_khz);
  iir_fir(&out[S.fs_out_khz], &in[n_samples], in_len - S.fs_in_khz);
  for (int i = 0; i < S.input_delay; i++) S.delayBuf[i] = in[in_len - S.input_delay + i];
}

int SilkDecoder::set_fs(Channel& c, int fs_khz, int32_t fs_api_hz) {
  int ret = 0;
  c.subfr_length = smulbb(5, fs_khz);
  const int frame_length = smulbb(c.nb_subfr, c.subfr_length);
  if (c.fs_khz != fs_khz || c.fs_api_hz != fs_api_hz) {
    ret += resampler_init(c.resampler, smulbb(fs_khz, 1000), fs_api_hz);
    c.fs_api_hz = fs_api_hz;
  }
  if (c.fs_khz != fs_khz || frame_length != c.frame_length) {
    if (fs_khz == 8) c.pitch_contour_table = c.nb_subfr == kMaxNbSubfr ? 0 : 1;
    else c.pitch_contour_table = c.nb_subfr == kMaxNbSubfr ? 2 : 3;
    if (c.fs_khz != fs_khz) {
      c.ltp_mem_length = smulbb(20, fs_khz);
      if (fs_khz == 8 || fs_khz == 12) {
        c.lpc_order = 10;
        c.wideband_codebook = false;
      } else {
        c.lpc_order = kMaxLpcOrder;
        c.wideband_codebook = true;
      }
      c.pitch_lag_low_bits = fs_khz == 16 ? 8 : fs_khz == 12 ? 6 : 4;
      c.first_frame_after_reset = 1;
      c.lag_prev = 100;
      c.last_gain_index = 10;
      c.prev_signal_type = kTypeNoVoice;
      for (int16_t& v : c.out_buf) v = 0;
      for (int32_t& v : c.slpc_q14_buf) v = 0;
    }
    c.fs_khz = fs_khz;
    c.frame_length = frame_length;
  }
  return ret;
}

void SilkDecoder::decode_indices(Channel& c, RangeDecoder& dec, int frame_index, bool decode_lbrr, int cond_coding) {
  Indices& ind = c.indices;
  int Ix;
  if (decode_lbrr || c.vad_flags[frame_index]) Ix = dec.icdf(k_silk_type_offset_VAD_iCDF, 8) + 2;
  else Ix = dec.icdf(k_silk_type_offset_no_VAD_iCDF, 8);
  ind.signal_type = static_cast<int8_t>(rshift32(Ix, 1));
  ind.quant_offset_type = static_cast<int8_t>(Ix & 1);
  if (cond_coding == kCodeConditionally) {
    ind.gains[0] = static_cast<int8_t>(dec.icdf(k_silk_delta_gain_iCDF, 8));
  } else {
    ind.gains[0] = static_cast<int8_t>(lshift32(dec.icdf(&k_silk_gain_iCDF[ind.signal_type * 8], 8), 3));
    ind.gains[0] = static_cast<int8_t>(ind.gains[0] + dec.icdf(k_silk_uniform8_iCDF, 8));
  }
  for (int i = 1; i < c.nb_subfr; i++) ind.gains[i] = static_cast<int8_t>(dec.icdf(k_silk_delta_gain_iCDF, 8));
  const NlsfCodebook& cb = nlsf_codebook(c.wideband_codebook);
  ind.nlsf[0] = static_cast<int8_t>(dec.icdf(&cb.cb1_icdf[(ind.signal_type >> 1) * cb.n_vectors], 8));
  int16_t ec_ix[kMaxLpcOrder];
  uint8_t pred_q8[kMaxLpcOrder];
  nlsf_unpack(ec_ix, pred_q8, cb, ind.nlsf[0]);
  for (int i = 0; i < cb.order; i++) {
    Ix = dec.icdf(&cb.ec_icdf[ec_ix[i]], 8);
    if (Ix == 0) Ix -= dec.icdf(k_silk_NLSF_EXT_iCDF, 8);
    else if (Ix == 2 * kNlsfQuantMaxAmplitude) Ix += dec.icdf(k_silk_NLSF_EXT_iCDF, 8);
    ind.nlsf[i + 1] = static_cast<int8_t>(Ix - kNlsfQuantMaxAmplitude);
  }
  if (c.nb_subfr == kMaxNbSubfr) ind.nlsf_interp_q2 = static_cast<int8_t>(dec.icdf(k_silk_NLSF_interpolation_factor_iCDF, 8));
  else ind.nlsf_interp_q2 = 4;
  if (ind.signal_type == kTypeVoiced) {
    bool decode_absolute_lag_index = true;
    if (cond_coding == kCodeConditionally && c.ec_prev_signal_type == kTypeVoiced) {
      int delta_lag_index = static_cast<int16_t>(dec.icdf(k_silk_pitch_delta_iCDF, 8));
      if (delta_lag_index > 0) {
        delta_lag_index = delta_lag_index - 9;
        ind.lag = static_cast<int16_t>(c.ec_prev_lag_index + delta_lag_index);
        decode_absolute_lag_index = false;
      }
    }
    if (decode_absolute_lag_index) {
      ind.lag = static_cast<int16_t>(static_cast<int16_t>(dec.icdf(k_silk_pitch_lag_iCDF, 8)) * rshift32(c.fs_khz, 1));
      const uint8_t* low = c.pitch_lag_low_bits == 8 ? k_silk_uniform8_iCDF : c.pitch_lag_low_bits == 6 ? k_silk_uniform6_iCDF : k_silk_uniform4_iCDF;
      ind.lag = static_cast<int16_t>(ind.lag + static_cast<int16_t>(dec.icdf(low, 8)));
    }
    c.ec_prev_lag_index = ind.lag;
    const uint8_t* contour = c.pitch_contour_table == 0 ? k_silk_pitch_contour_NB_iCDF
                             : c.pitch_contour_table == 1 ? k_silk_pitch_contour_10_ms_NB_iCDF
                             : c.pitch_contour_table == 2 ? k_silk_pitch_contour_iCDF
                                                          : k_silk_pitch_contour_10_ms_iCDF;
    ind.contour = static_cast<int8_t>(dec.icdf(contour, 8));
    ind.per_index = static_cast<int8_t>(dec.icdf(k_silk_LTP_per_index_iCDF, 8));
    for (int k = 0; k < c.nb_subfr; k++) {
      const uint8_t* tab = ind.per_index == 0 ? k_silk_LTP_gain_iCDF_0 : ind.per_index == 1 ? k_silk_LTP_gain_iCDF_1 : k_silk_LTP_gain_iCDF_2;
      ind.ltp[k] = static_cast<int8_t>(dec.icdf(tab, 8));
    }
    if (cond_coding == kCodeIndependently) ind.ltp_scale_index = static_cast<int8_t>(dec.icdf(k_silk_LTPscale_iCDF, 8));
    else ind.ltp_scale_index = 0;
  }
  c.ec_prev_signal_type = ind.signal_type;
  ind.seed = static_cast<int8_t>(dec.icdf(k_silk_uniform4_iCDF, 8));
}

namespace {

void shell_decode_split(int16_t* c1, int16_t* c2, RangeDecoder& dec, int p, const uint8_t* table) {
  if (p > 0) {
    c1[0] = static_cast<int16_t>(dec.icdf(&table[k_silk_shell_code_table_offsets[p]], 8));
    c2[0] = static_cast<int16_t>(p - c1[0]);
  } else {
    c1[0] = 0;
    c2[0] = 0;
  }
}

void shell_decoder(int16_t* pulses0, RangeDecoder& dec, int pulses4) {
  int16_t pulses3[2], pulses2[4], pulses1[8];
  const uint8_t *t0 = k_silk_shell_code_table0, *t1 = k_silk_shell_code_table1, *t2 = k_silk_shell_code_table2, *t3 = k_silk_shell_code_table3;
  shell_decode_split(&pulses3[0], &pulses3[1], dec, pulses4, t3);
  shell_decode_split(&pulses2[0], &pulses2[1], dec, pulses3[0], t2);
  shell_decode_split(&pulses1[0], &pulses1[1], dec, pulses2[0], t1);
  shell_decode_split(&pulses0[0], &pulses0[1], dec, pulses1[0], t0);
  shell_decode_split(&pulses0[2], &pulses0[3], dec, pulses1[1], t0);
  shell_decode_split(&pulses1[2], &pulses1[3], dec, pulses2[1], t1);
  shell_decode_split(&pulses0[4], &pulses0[5], dec, pulses1[2], t0);
  shell_decode_split(&pulses0[6], &pulses0[7], dec, pulses1[3], t0);
  shell_decode_split(&pulses2[2], &pulses2[3], dec, pulses3[1], t2);
  shell_decode_split(&pulses1[4], &pulses1[5], dec, pulses2[2], t1);
  shell_decode_split(&pulses0[8], &pulses0[9], dec, pulses1[4], t0);
  shell_decode_split(&pulses0[10], &pulses0[11], dec, pulses1[5], t0);
  shell_decode_split(&pulses1[6], &pulses1[7], dec, pulses2[3], t1);
  shell_decode_split(&pulses0[12], &pulses0[13], dec, pulses1[6], t0);
  shell_decode_split(&pulses0[14], &pulses0[15], dec, pulses1[7], t0);
}

}  // namespace

void SilkDecoder::decode_pulses(RangeDecoder& dec, int16_t* pulses, int signal_type, int quant_offset_type, int frame_length) {
  int sum_pulses[20], nLshifts[20];
  const int rate_level_index = dec.icdf(&k_silk_rate_levels_iCDF[(signal_type >> 1) * 9], 8);
  int iter = rshift32(frame_length, 4);
  if (iter * kShellLength < frame_length) iter++;
  const uint8_t* cdf_ptr = &k_silk_pulses_per_block_iCDF[rate_level_index * 18];
  for (int i = 0; i < iter; i++) {
    nLshifts[i] = 0;
    sum_pulses[i] = dec.icdf(cdf_ptr, 8);
    while (sum_pulses[i] == 16 + 1) {
      nLshifts[i]++;
      sum_pulses[i] = dec.icdf(&k_silk_pulses_per_block_iCDF[9 * 18 + (nLshifts[i] == 10)], 8);
    }
  }
  for (int i = 0; i < iter; i++) {
    if (sum_pulses[i] > 0) {
      shell_decoder(&pulses[i * kShellLength], dec, sum_pulses[i]);
    } else {
      for (int k = 0; k < kShellLength; k++) pulses[i * kShellLength + k] = 0;
    }
  }
  for (int i = 0; i < iter; i++) {
    if (nLshifts[i] > 0) {
      const int nLS = nLshifts[i];
      int16_t* pulses_ptr = &pulses[i * kShellLength];
      for (int k = 0; k < kShellLength; k++) {
        int abs_q = pulses_ptr[k];
        for (int j = 0; j < nLS; j++) {
          abs_q = lshift32(abs_q, 1);
          abs_q += dec.icdf(k_silk_lsb_iCDF, 8);
        }
        pulses_ptr[k] = static_cast<int16_t>(abs_q);
      }
      sum_pulses[i] |= nLS << 5;
    }
  }
  // signs
  uint8_t icdf[2];
  icdf[1] = 0;
  int16_t* q_ptr = pulses;
  const uint8_t* icdf_ptr = &k_silk_sign_iCDF[7 * (quant_offset_type + (signal_type << 1))];
  const int length = rshift32(frame_length + kShellLength / 2, 4);
  for (int i = 0; i < length; i++) {
    const int p = sum_pulses[i];
    if (p > 0) {
      icdf[0] = icdf_ptr[(p & 0x1F) < 6 ? (p & 0x1F) : 6];
      for (int j = 0; j < kShellLength; j++)
        if (q_ptr[j] > 0) q_ptr[j] = static_cast<int16_t>(q_ptr[j] * (lshift32(dec.icdf(icdf, 8), 1) - 1));
    }
    q_ptr += kShellLength;
  }
}

void SilkDecoder::decode_parameters(Channel& c, Control32& ctrl, int cond_coding) {
  gains_dequant(ctrl.gains_q16, c.indices.gains, &c.last_gain_index, cond_coding == kCodeConditionally, c.nb_subfr);
  int16_t pNLSF_Q15[kMaxLpcOrder], pNLSF0_Q15[kMaxLpcOrder];
  const NlsfCodebook& cb = nlsf_codebook(c.wideband_codebook);
  nlsf_decode(pNLSF_Q15, c.indices.nlsf, cb);
  nlsf2a(ctrl.pred_coef_q12[1], pNLSF_Q15, c.lpc_order);
  if (c.first_frame_after_reset == 1) c.indices.nlsf_interp_q2 = 4;
  if (c.indices.nlsf_interp_q2 < 4) {
    for (int i = 0; i < c.lpc_order; i++)
      pNLSF0_Q15[i] = static_cast<int16_t>(c.prev_nlsf_q15[i] + rshift32(mul32(c.indices.nlsf_interp_q2, pNLSF_Q15[i] - c.prev_nlsf_q15[i]), 2));
    nlsf2a(ctrl.pred_coef_q12[0], pNLSF0_Q15, c.lpc_order);
  } else {
    for (int i = 0; i < c.lpc_order; i++) ctrl.pred_coef_q12[0][i] = ctrl.pred_coef_q12[1][i];
  }
  for (int i = 0; i < c.lpc_order; i++) c.prev_nlsf_q15[i] = pNLSF_Q15[i];
  if (c.indices.signal_type == kTypeVoiced) {
    decode_pitch(c.indices.lag, c.indices.contour, ctrl.pitch_l, c.fs_khz, c.nb_subfr);
    const int8_t* cbk = c.indices.per_index == 0 ? k_silk_LTP_gain_vq_0 : c.indices.per_index == 1 ? k_silk_LTP_gain_vq_1 : k_silk_LTP_gain_vq_2;
    for (int k = 0; k < c.nb_subfr; k++) {
      const int Ix = c.indices.ltp[k];
      for (int i = 0; i < kLtpOrder; i++) ctrl.ltp_coef_q14[k * kLtpOrder + i] = static_cast<int16_t>(lshift32(cbk[Ix * kLtpOrder + i], 7));
    }
    ctrl.ltp_scale_q14 = k_silk_LTPScales_table_Q14[c.indices.ltp_scale_index];
  } else {
    for (int k = 0; k < c.nb_subfr; k++) ctrl.pitch_l[k] = 0;
    for (int i = 0; i < kLtpOrder * c.nb_subfr; i++) ctrl.ltp_coef_q14[i] = 0;
    c.indices.per_index = 0;
    ctrl.ltp_scale_q14 = 0;
  }
}

void SilkDecoder::decode_core(Channel& c, const Control32& ctrl, int16_t* xq, const int16_t* pulses) {
  int16_t sLTP[320];
  int32_t sLTP_Q15[320 + 320];
  int32_t res_Q14[80];
  int32_t sLPC_Q14[80 + kMaxLpcOrder];
  const int32_t offset_Q10 = k_silk_Quantization_Offsets_Q10[(c.indices.signal_type >> 1) * 2 + c.indices.quant_offset_type];
  const bool nlsf_interpolation_flag = c.indices.nlsf_interp_q2 < (1 << 2);
  int32_t rand_seed = c.indices.seed;
  for (int i = 0; i < c.frame_length; i++) {
    rand_seed = add32(907633515, static_cast<int32_t>(static_cast<uint32_t>(rand_seed) * 196314165u));
    c.exc_q14[i] = lshift32(static_cast<int32_t>(pulses[i]), 14);
    if (c.exc_q14[i] > 0) c.exc_q14[i] -= 80 << 4;
    else if (c.exc_q14[i] < 0) c.exc_q14[i] += 80 << 4;
    c.exc_q14[i] += offset_Q10 << 4;
    if (rand_seed < 0) c.exc_q14[i] = -c.exc_q14[i];
    rand_seed = add32(rand_seed, pulses[i]);
  }
  for (int i = 0; i < kMaxLpcOrder; i++) sLPC_Q14[i] = c.slpc_q14_buf[i];
  const int32_t* pexc_Q14 = c.exc_q14;
  int16_t* pxq = xq;
  int sLTP_buf_idx = c.ltp_mem_length;
  int lag = 0;
  for (int k = 0; k < c.nb_subfr; k++) {
    int32_t* pres_Q14 = res_Q14;
    const int16_t* A_Q12 = ctrl.pred_coef_q12[k >> 1];
    int16_t A_Q12_tmp[kMaxLpcOrder];
    for (int i = 0; i < c.lpc_order; i++) A_Q12_tmp[i] = A_Q12[i];
    const int16_t* B_Q14 = &ctrl.ltp_coef_q14[k * kLtpOrder];
    const int signal_type = c.indices.signal_type;
    const int32_t Gain_Q10 = rshift32(ctrl.gains_q16[k], 6);
    int32_t inv_gain_Q31 = inverse32_varq(ctrl.gains_q16[k], 47);
    int32_t gain_adj_Q16;
    if (ctrl.gains_q16[k] != c.prev_gain_q16) {
      gain_adj_Q16 = div32_varq(c.prev_gain_q16, ctrl.gains_q16[k], 16);
      for (int i = 0; i < kMaxLpcOrder; i++) sLPC_Q14[i] = smulww(gain_adj_Q16, sLPC_Q14[i]);
    } else {
      gain_adj_Q16 = static_cast<int32_t>(1) << 16;
    }
    c.prev_gain_q16 = ctrl.gains_q16[k];
    if (signal_type == kTypeVoiced) {
      lag = ctrl.pitch_l[k];
      if (k == 0 || (k == 2 && nlsf_interpolation_flag)) {
        const int start_idx = c.ltp_mem_length - lag - c.lpc_order - kLtpOrder / 2;
        if (start_idx <= 0) return;  // cannot happen for lags the pitch table produces
        if (k == 2)
          for (int i = 0; i < 2 * c.subfr_length; i++) c.out_buf[c.ltp_mem_length + i] = xq[i];
        lpc_analysis_filter(&sLTP[start_idx], &c.out_buf[start_idx + k * c.subfr_length], A_Q12, c.ltp_mem_length - start_idx, c.lpc_order);
        if (k == 0) inv_gain_Q31 = lshift32(smulwb(inv_gain_Q31, ctrl.ltp_scale_q14), 2);
        for (int i = 0; i < lag + kLtpOrder / 2; i++) sLTP_Q15[sLTP_buf_idx - i - 1] = smulwb(inv_gain_Q31, sLTP[c.ltp_mem_length - i - 1]);
      } else if (gain_adj_Q16 != static_cast<int32_t>(1) << 16) {
        for (int i = 0; i < lag + kLtpOrder / 2; i++) sLTP_Q15[sLTP_buf_idx - i - 1] = smulww(gain_adj_Q16, sLTP_Q15[sLTP_buf_idx - i - 1]);
      }
    }
    if (signal_type == kTypeVoiced) {
      const int32_t* pred_lag_ptr = &sLTP_Q15[sLTP_buf_idx - lag + kLtpOrder / 2];
      for (int i = 0; i < c.subfr_length; i++) {
        int32_t LTP_pred_Q13 = 2;
        LTP_pred_Q13 = smlawb(LTP_pred_Q13, pred_lag_ptr[0], B_Q14[0]);
        LTP_pred_Q13 = smlawb(LTP_pred_Q13, pred_lag_ptr[-1], B_Q14[1]);
        LTP_pred_Q13 = smlawb(LTP_pred_Q13, pred_lag_ptr[-2], B_Q14[2]);
        LTP_pred_Q13 = smlawb(LTP_pred_Q13, pred_lag_ptr[-3], B_Q14[3]);
        LTP_pred_Q13 = smlawb(LTP_pred_Q13, pred_lag_ptr[-4], B_Q14[4]);
        pred_lag_ptr++;
        pres_Q14[i] = add32(pexc_Q14[i], lshift32(LTP_pred_Q13, 1));
        sLTP_Q15[sLTP_buf_idx] = lshift32(pres_Q14[i], 1);
        sLTP_buf_idx++;
      }
    } else {
      pres_Q14 = const_cast<int32_t*>(pexc_Q14);  // read-only use below
    }
    for (int i = 0; i < c.subfr_length; i++) {
      int32_t LPC_pred_Q10 = rshift32(c.lpc_order, 1);
      for (int j = 0; j < c.lpc_order; j++) LPC_pred_Q10 = smlawb(LPC_pred_Q10, sLPC_Q14[kMaxLpcOrder + i - 1 - j], A_Q12_tmp[j]);
      sLPC_Q14[kMaxLpcOrder + i] = add_sat32(pres_Q14[i], lshift_sat32(LPC_pred_Q10, 4));
      pxq[i] = static_cast<int16_t>(sat16(rshift_round(smulww(sLPC_Q14[kMaxLpcOrder + i], Gain_Q10), 8)));
    }
    for (int i = 0; i < kMaxLpcOrder; i++) sLPC_Q14[i] = sLPC_Q14[c.subfr_length + i];
    pexc_Q14 += c.subfr_length;
    pxq += c.subfr_length;
  }
  for (int i = 0; i < kMaxLpcOrder; i++) c.slpc_q14_buf[i] = sLPC_Q14[i];
}

int SilkDecoder::decode_frame(Channel& c, RangeDecoder& dec, int16_t* out, int32_t* n, int cond_coding) {
  const int L = c.frame_length;
  Control32 ctrl;
  ctrl.ltp_scale_q14 = 0;
  int16_t pulses[(kMaxFrameLength + kShellLength - 1) & ~(kShellLength - 1)];
  decode_indices(c, dec, c.frames_decoded, false, cond_coding);
  decode_pulses(dec, pulses, c.indices.signal_type, c.indices.quant_offset_type, c.frame_length);
  decode_parameters(c, ctrl, cond_coding);
  decode_core(c, ctrl, out, pulses);
  c.prev_signal_type = c.indices.signal_type;
  c.first_frame_after_reset = 0;
  const int mv_len = c.ltp_mem_length - c.frame_length;
  for (int i = 0; i < mv_len; i++) c.out_buf[i] = c.out_buf[i + c.frame_length];
  for (int i = 0; i < c.frame_length; i++) c.out_buf[mv_len + i] = out[i];
  c.lag_prev = ctrl.pitch_l[c.nb_subfr - 1];
  *n = L;
  return 0;
}

int SilkDecoder::decode(const Control& control, RangeDecoder& dec, bool first_frame, int16_t* pcm, int* samples) {
  Channel* ch = channel_;
  if (first_frame)
    for (int n = 0; n < control.channels_internal; n++) ch[n].frames_decoded = 0;
  if (control.channels_internal > channels_internal_) init_channel(ch[1]);
  const bool stereo_to_mono = control.channels_internal == 1 && channels_internal_ == 2 && control.internal_rate == 1000 * ch[0].fs_khz;
  if (ch[0].frames_decoded == 0) {
    for (int n = 0; n < control.channels_internal; n++) {
      switch (control.payload_ms) {
        case 10: ch[n].frames_per_packet = 1; ch[n].nb_subfr = 2; break;
        case 20: ch[n].frames_per_packet = 1; ch[n].nb_subfr = 4; break;
        case 40: ch[n].frames_per_packet = 2; ch[n].nb_subfr = 4; break;
        case 60: ch[n].frames_per_packet = 3; ch[n].nb_subfr = 4; break;
        default: return -1;
      }
      const int fs_khz_dec = (control.internal_rate >> 10) + 1;
      if (fs_khz_dec != 8 && fs_khz_dec != 12 && fs_khz_dec != 16) return -1;
      if (set_fs(ch[n], fs_khz_dec, 48000) != 0) return -1;
    }
  }
  if (control.channels_api == 2 && control.channels_internal == 2 && (channels_api_ == 1 || channels_internal_ == 1)) {
    stereo_pred_prev_q13_[0] = stereo_pred_prev_q13_[1] = 0;
    stereo_side_[0] = stereo_side_[1] = 0;
    ch[1].resampler = ch[0].resampler;
  }
  channels_api_ = control.channels_api;
  channels_internal_ = control.channels_internal;

  int decode_only_middle = 0;
  int32_t ms_pred_q13[2] = {0, 0};
  if (ch[0].frames_decoded == 0) {
    for (int n = 0; n < control.channels_internal; n++) {
      for (int i = 0; i < ch[n].frames_per_packet; i++) ch[n].vad_flags[i] = dec.bit_logp(1);
      ch[n].lbrr_flag = dec.bit_logp(1);
    }
    for (int n = 0; n < control.channels_internal; n++) {
      for (int& f : ch[n].lbrr_flags) f = 0;
      if (ch[n].lbrr_flag) {
        if (ch[n].frames_per_packet == 1) {
          ch[n].lbrr_flags[0] = 1;
        } else {
          const int32_t sym = dec.icdf(ch[n].frames_per_packet == 2 ? k_silk_LBRR_flags_2_iCDF : k_silk_LBRR_flags_3_iCDF, 8) + 1;
          for (int i = 0; i < ch[n].frames_per_packet; i++) ch[n].lbrr_flags[i] = rshift32(sym, i) & 1;
        }
      }
    }
    // The redundant (LBRR) frames are in the bitstream before the regular ones: parse them to get past them.
    for (int i = 0; i < ch[0].frames_per_packet; i++) {
      for (int n = 0; n < control.channels_internal; n++) {
        if (!ch[n].lbrr_flags[i]) continue;
        int16_t pulses[(kMaxFrameLength + kShellLength - 1) & ~(kShellLength - 1)];
        if (control.channels_internal == 2 && n == 0) {
          // silk_stereo_decode_pred + (mid only flag): consume and discard
          int32_t dummy[2];
          const int nn = dec.icdf(k_silk_stereo_pred_joint_iCDF, 8);
          int ix[2][3];
          ix[0][2] = div32_16(nn, 5);
          ix[1][2] = nn - 5 * ix[0][2];
          for (int m = 0; m < 2; m++) {
            ix[m][0] = dec.icdf(k_silk_uniform3_iCDF, 8);
            ix[m][1] = dec.icdf(k_silk_uniform5_iCDF, 8);
          }
          (void)dummy;
          if (ch[1].lbrr_flags[i] == 0) dec.icdf(k_silk_stereo_only_code_mid_iCDF, 8);
        }
        const int cond = (i > 0 && ch[n].lbrr_flags[i - 1]) ? kCodeConditionally : kCodeIndependently;
        decode_indices(ch[n], dec, i, true, cond);
        decode_pulses(dec, pulses, ch[n].indices.signal_type, ch[n].indices.quant_offset_type, ch[n].frame_length);
      }
    }
  }

  if (control.channels_internal == 2) {
    // silk_stereo_decode_pred
    const int nn = dec.icdf(k_silk_stereo_pred_joint_iCDF, 8);
    int ix[2][3];
    ix[0][2] = div32_16(nn, 5);
    ix[1][2] = nn - 5 * ix[0][2];
    for (int m = 0; m < 2; m++) {
      ix[m][0] = dec.icdf(k_silk_uniform3_iCDF, 8);
      ix[m][1] = dec.icdf(k_silk_uniform5_iCDF, 8);
    }
    for (int m = 0; m < 2; m++) {
      ix[m][0] += 3 * ix[m][2];
      const int32_t low_q13 = k_silk_stereo_pred_quant_Q13[ix[m][0]];
      const int32_t step_q13 = smulwb(k_silk_stereo_pred_quant_Q13[ix[m][0] + 1] - low_q13, fix_const(0.5 / 5, 16));
      ms_pred_q13[m] = smlabb(low_q13, step_q13, 2 * ix[m][1] + 1);
    }
    ms_pred_q13[0] -= ms_pred_q13[1];
    if (ch[1].vad_flags[ch[0].frames_decoded] == 0) decode_only_middle = dec.icdf(k_silk_stereo_only_code_mid_iCDF, 8);
    else decode_only_middle = 0;
  }
  if (control.channels_internal == 2 && decode_only_middle == 0 && prev_decode_only_middle_ == 1) {
    for (int16_t& v : channel_[1].out_buf) v = 0;
    for (int32_t& v : channel_[1].slpc_q14_buf) v = 0;
    channel_[1].lag_prev = 100;
    channel_[1].last_gain_index = 10;
    channel_[1].prev_signal_type = kTypeNoVoice;
    channel_[1].first_frame_after_reset = 1;
  }

  int16_t tmp_storage[2][kMaxFrameLength + 2];
  int16_t* tmp[2] = {tmp_storage[0], tmp_storage[1]};
  const bool has_side = !decode_only_middle;
  int32_t n_samples_dec = 0;
  for (int n = 0; n < control.channels_internal; n++) {
    if (n == 0 || has_side) {
      const int frame_index = ch[0].frames_decoded - n;
      int cond;
      if (frame_index <= 0) cond = kCodeIndependently;
      else if (n > 0 && prev_decode_only_middle_) cond = kCodeIndependentlyNoLtpScaling;
      else cond = kCodeConditionally;
      if (decode_frame(ch[n], dec, &tmp[n][2], &n_samples_dec, cond) != 0) return -1;
    } else {
      for (int i = 0; i < n_samples_dec; i++) tmp[n][2 + i] = 0;
    }
    ch[n].frames_decoded++;
  }
  if (control.channels_api == 2 && control.channels_internal == 2) {
    // silk_stereo_MS_to_LR
    int16_t* x1 = tmp[0];
    int16_t* x2 = tmp[1];
    const int fs_khz = ch[0].fs_khz;
    const int frame_length = n_samples_dec;
    x1[0] = stereo_mid_[0];
    x1[1] = stereo_mid_[1];
    x2[0] = stereo_side_[0];
    x2[1] = stereo_side_[1];
    stereo_mid_[0] = x1[frame_length];
    stereo_mid_[1] = x1[frame_length + 1];
    stereo_side_[0] = x2[frame_length];
    stereo_side_[1] = x2[frame_length + 1];
    int32_t pred0_q13 = stereo_pred_prev_q13_[0], pred1_q13 = stereo_pred_prev_q13_[1];
    const int denom_q16 = div32_16(static_cast<int32_t>(1) << 16, kStereoInterpLenMs * fs_khz);
    const int delta0_q13 = rshift_round(smulbb(ms_pred_q13[0] - stereo_pred_prev_q13_[0], denom_q16), 16);
    const int delta1_q13 = rshift_round(smulbb(ms_pred_q13[1] - stereo_pred_prev_q13_[1], denom_q16), 16);
    int n = 0;
    for (; n < kStereoInterpLenMs * fs_khz; n++) {
      pred0_q13 += delta0_q13;
      pred1_q13 += delta1_q13;
      int32_t sum = lshift32(add32(x1[n] + static_cast<int32_t>(x1[n + 2]), lshift32(x1[n + 1], 1)), 9);
      sum = smlawb(lshift32(static_cast<int32_t>(x2[n + 1]), 8), sum, pred0_q13);
      sum = smlawb(sum, lshift32(static_cast<int32_t>(x1[n + 1]), 11), pred1_q13);
      x2[n + 1] = static_cast<int16_t>(sat16(rshift_round(sum, 8)));
    }
    pred0_q13 = ms_pred_q13[0];
    pred1_q13 = ms_pred_q13[1];
    for (; n < frame_length; n++) {
      int32_t sum = lshift32(add32(x1[n] + static_cast<int32_t>(x1[n + 2]), lshift32(x1[n + 1], 1)), 9);
      sum = smlawb(lshift32(static_cast<int32_t>(x2[n + 1]), 8), sum, pred0_q13);
      sum = smlawb(sum, lshift32(static_cast<int32_t>(x1[n + 1]), 11), pred1_q13);
      x2[n + 1] = static_cast<int16_t>(sat16(rshift_round(sum, 8)));
    }
    stereo_pred_prev_q13_[0] = static_cast<int16_t>(ms_pred_q13[0]);
    stereo_pred_prev_q13_[1] = static_cast<int16_t>(ms_pred_q13[1]);
    for (n = 0; n < frame_length; n++) {
      const int32_t sum = x1[n + 1] + static_cast<int32_t>(x2[n + 1]);
      const int32_t diff = x1[n + 1] - static_cast<int32_t>(x2[n + 1]);
      x1[n + 1] = static_cast<int16_t>(sat16(sum));
      x2[n + 1] = static_cast<int16_t>(sat16(diff));
    }
  } else {
    tmp[0][0] = stereo_mid_[0];
    tmp[0][1] = stereo_mid_[1];
    stereo_mid_[0] = tmp[0][n_samples_dec];
    stereo_mid_[1] = tmp[0][n_samples_dec + 1];
  }
  *samples = n_samples_dec * 48000 / (ch[0].fs_khz * 1000);
  int16_t resampled[960];
  const int api = control.channels_api;
  for (int n = 0; n < (api < control.channels_internal ? api : control.channels_internal); n++) {
    int16_t* dst = api == 2 ? resampled : pcm;
    resample(ch[n].resampler, dst, &tmp[n][1], n_samples_dec);
    if (api == 2)
      for (int i = 0; i < *samples; i++) pcm[n + 2 * i] = dst[i];
  }
  if (api == 2 && control.channels_internal == 1) {
    if (stereo_to_mono) {
      resample(ch[1].resampler, resampled, &tmp[0][1], n_samples_dec);
      for (int i = 0; i < *samples; i++) pcm[1 + 2 * i] = resampled[i];
    } else {
      for (int i = 0; i < *samples; i++) pcm[1 + 2 * i] = pcm[0 + 2 * i];
    }
  }
  prev_decode_only_middle_ = decode_only_middle;
  return 0;
}

}  // namespace opus
}  // namespace media
