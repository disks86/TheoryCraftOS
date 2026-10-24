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

// H.264 decoding: the decoded picture buffer (picture order counts 8.2.1, reference marking 8.2.5, reference lists 8.2.4, output order). Shared by the CPU decoder
// (H264Syntax.cpp) and the hardware decode path. Numbers in comments refer to ITU-T H.264.

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

void Dpb::attach(PictureInfo* const* slots, int count) {
  slot_count_ = count < kMaxPictures ? count : kMaxPictures;
  for (int i = 0; i < slot_count_; ++i) {
    slots_[i] = slots[i];
    slots_[i]->slot = i;
  }
  cur_ = returned_ = nullptr;
  out_count_ = out_wait_count_ = 0;
}

void Dpb::clear_pictures() {
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo& p = *slots_[i];
    p.in_use = false;
    p.is_ref = p.is_long = p.needed_for_output = false;
  }
  out_count_ = 0;
  out_wait_count_ = 0;
  returned_ = nullptr;
  cur_ = nullptr;
}

void Dpb::reset() {
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo& p = *slots_[i];
    p.is_ref = false;
    p.is_long = false;
    p.needed_for_output = false;
  }
  out_count_ = 0;
  out_wait_count_ = 0;
  cur_ = nullptr;
  prev_poc_msb_ = prev_poc_lsb_ = 0;
  prev_frame_num_ = prev_frame_num_offset_ = 0;
  returned_ = nullptr;
}

PictureInfo* Dpb::find_free_slot() const {
  PictureInfo* empty = nullptr;
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo* p = slots_[i];
    if (p == cur_ || p == returned_ || p->is_ref || p->needed_for_output) continue;
    if (p->has_memory) return p;  // memory already there
    if (!empty) empty = p;
  }
  return empty;
}

PictureInfo* Dpb::begin_picture(const SliceHeader& sh, int64_t pts) {
  if (sh.idr()) {
    // Everything before an IDR picture is output first.
    bump_output(true);
    for (int i = 0; i < slot_count_; ++i) {
      slots_[i]->is_ref = false;
      slots_[i]->is_long = false;
    }
    max_long_term_idx_ = -1;
  }
  compute_poc(sh);
  PictureInfo* pic = find_free_slot();
  while (!pic) {
    // The buffer is full: push the earliest picture out, and if that frees nothing drop the oldest reference.
    if (out_count_ > 0) {
      bump_output(false);
      pic = find_free_slot();
      if (pic) break;
    }
    PictureInfo* oldest = nullptr;
    for (int i = 0; i < slot_count_; ++i) {
      PictureInfo* q = slots_[i];
      if (q->is_ref && q != returned_ && (!oldest || q->frame_num_wrap < oldest->frame_num_wrap)) oldest = q;
    }
    if (!oldest) return nullptr;
    oldest->is_ref = false;
    oldest->is_long = false;
    pic = find_free_slot();
  }
  cur_ = pic;
  cur_->in_use = true;
  cur_->id = next_pic_id_++;
  cur_->poc = pic_poc_;
  cur_->top_poc = top_poc_;
  cur_->bottom_poc = bottom_poc_;
  cur_->frame_num = sh.frame_num;
  cur_->is_ref = false;
  cur_->is_long = false;
  cur_->needed_for_output = false;
  cur_->non_existing = false;
  cur_->keyframe = sh.idr() || sh.type == kSliceI;
  cur_->pts = pts;
  return cur_;
}

void Dpb::abort_picture() {
  if (cur_) cur_->in_use = false;
  cur_ = nullptr;
}

void Dpb::compute_poc(const SliceHeader& sh) {
  const Sps& s = *sps_;
  const int max_frame_num = 1 << s.log2_max_frame_num;
  if (s.poc_type == 0) {
    int prev_msb, prev_lsb;
    if (sh.idr()) {
      prev_msb = prev_lsb = 0;
    } else {
      prev_msb = prev_poc_msb_;
      prev_lsb = prev_poc_lsb_;
    }
    const int max_lsb = 1 << s.log2_max_poc_lsb;
    int msb;
    if (sh.poc_lsb < prev_lsb && prev_lsb - sh.poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
    else if (sh.poc_lsb > prev_lsb && sh.poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
    else msb = prev_msb;
    top_poc_ = msb + sh.poc_lsb;
    bottom_poc_ = top_poc_ + sh.delta_poc_bottom;
    cur_poc_msb_ = msb;
  } else {
    int offset;
    if (sh.idr()) offset = 0;
    else if (prev_frame_num_ > sh.frame_num) offset = prev_frame_num_offset_ + max_frame_num;
    else offset = prev_frame_num_offset_;
    frame_num_offset_ = offset;
    if (s.poc_type == 1) {
      int abs_frame_num = s.num_ref_in_cycle != 0 ? offset + sh.frame_num : 0;
      if (sh.nal_ref_idc == 0 && abs_frame_num > 0) --abs_frame_num;
      int expected = 0;
      if (abs_frame_num > 0) {
        int per_cycle = 0;
        for (int i = 0; i < s.num_ref_in_cycle; ++i) per_cycle += s.offset_for_ref_frame[i];
        const int cycle = (abs_frame_num - 1) / s.num_ref_in_cycle;
        const int in_cycle = (abs_frame_num - 1) % s.num_ref_in_cycle;
        expected = cycle * per_cycle;
        for (int i = 0; i <= in_cycle; ++i) expected += s.offset_for_ref_frame[i];
      }
      if (sh.nal_ref_idc == 0) expected += s.offset_for_non_ref_pic;
      top_poc_ = expected + sh.delta_poc[0];
      bottom_poc_ = top_poc_ + s.offset_for_top_to_bottom + sh.delta_poc[1];
    } else {
      int temp;
      if (sh.idr()) temp = 0;
      else if (sh.nal_ref_idc == 0) temp = 2 * (offset + sh.frame_num) - 1;
      else temp = 2 * (offset + sh.frame_num);
      top_poc_ = bottom_poc_ = temp;
    }
  }
  pic_poc_ = top_poc_ < bottom_poc_ ? top_poc_ : bottom_poc_;
}

void Dpb::finish_picture(const SliceHeader& sh) {
  if (!cur_) return;
  bool mmco5 = false;
  if (sh.nal_ref_idc != 0) {
    for (int i = 0; i < sh.num_mmco; ++i)
      if (sh.mmco[i].op == 5) mmco5 = true;
    mark_references(sh);
  }
  if (mmco5) {
    // 8.2.1: the picture is treated as having frame_num 0 and the order counts restart from it.
    bump_output(true);
    const int temp = cur_->poc;
    top_poc_ -= temp;
    bottom_poc_ -= temp;
    cur_->poc = top_poc_ < bottom_poc_ ? top_poc_ : bottom_poc_;
    cur_->top_poc = top_poc_;
    cur_->bottom_poc = bottom_poc_;
    cur_->frame_num = 0;
  }
  // State for the next picture's order count.
  const Sps& s = *sps_;
  if (sh.nal_ref_idc != 0) {
    if (mmco5) {
      prev_poc_msb_ = 0;
      prev_poc_lsb_ = top_poc_;  // tempPicOrderCnt-adjusted TopFieldOrderCnt (frame)
    } else {
      prev_poc_msb_ = cur_poc_msb_;
      prev_poc_lsb_ = sh.poc_lsb;
    }
  }
  if (s.poc_type != 0) {
    prev_frame_num_ = mmco5 ? 0 : sh.frame_num;
    prev_frame_num_offset_ = mmco5 ? 0 : frame_num_offset_;
  } else {
    prev_frame_num_ = sh.frame_num;
  }
  prev_ref_frame_num_ = sh.nal_ref_idc != 0 ? (mmco5 ? 0 : sh.frame_num) : prev_ref_frame_num_;
  cur_->needed_for_output = true;
  PictureInfo* done = cur_;
  cur_ = nullptr;
  queue_output(done);
}

// Pictures wait here until the display order allows them out.
void Dpb::queue_output(PictureInfo* p) {
  out_wait_[out_wait_count_++] = p;
  const int depth = sps_->num_reorder_frames >= 0 ? sps_->num_reorder_frames : (sps_->poc_type == 2 ? 0 : sps_->dpb_frames);
  while (out_wait_count_ > depth) bump_output(false);
}

// Moves the earliest waiting picture (or all of them) to the queue the user receives from.
void Dpb::bump_output(bool all) {
  do {
    if (out_wait_count_ == 0) return;
    int best = 0;
    for (int i = 1; i < out_wait_count_; ++i)
      if (out_wait_[i]->poc < out_wait_[best]->poc) best = i;
    PictureInfo* p = out_wait_[best];
    for (int i = best; i + 1 < out_wait_count_; ++i) out_wait_[i] = out_wait_[i + 1];
    --out_wait_count_;
    if (out_count_ < kMaxPictures) {
      out_queue_[out_count_++] = p;
    } else {
      p->needed_for_output = false;
    }
  } while (all);
}

PictureInfo* Dpb::take_output() {
  returned_ = nullptr;
  if (out_count_ == 0) return nullptr;
  PictureInfo* p = out_queue_[0];
  for (int i = 0; i + 1 < out_count_; ++i) out_queue_[i] = out_queue_[i + 1];
  --out_count_;
  p->needed_for_output = false;
  returned_ = p;
  return p;
}

// ---------------------------------------------------------------------------------------------------------------
// Reference picture marking (8.2.5)

void Dpb::sliding_window() {
  int refs = 0;
  for (int i = 0; i < slot_count_; ++i)
    if (slots_[i] != cur_ && slots_[i]->is_ref) ++refs;
  const int max_refs = sps_->max_num_ref_frames > 0 ? sps_->max_num_ref_frames : 1;
  while (refs >= max_refs) {
    PictureInfo* oldest = nullptr;
    for (int i = 0; i < slot_count_; ++i) {
      PictureInfo* p = slots_[i];
      if (p != cur_ && p->is_ref && !p->is_long && (!oldest || p->frame_num_wrap < oldest->frame_num_wrap)) oldest = p;
    }
    if (!oldest) break;
    oldest->is_ref = false;
    --refs;
  }
}

void Dpb::mark_references(const SliceHeader& sh) {
  const int max_frame_num = 1 << sps_->log2_max_frame_num;
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo& p = *slots_[i];
    if (p.is_ref && !p.is_long) p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
  }
  cur_->is_ref = true;
  cur_->is_long = false;
  if (sh.idr()) {
    if (sh.long_term_reference) {
      cur_->is_long = true;
      cur_->long_term_idx = 0;
      max_long_term_idx_ = 0;
    } else {
      max_long_term_idx_ = -1;
    }
    cur_->frame_num_wrap = sh.frame_num;
    return;
  }
  cur_->frame_num_wrap = sh.frame_num;
  if (sh.adaptive_marking) {
    apply_mmco(sh);
  } else {
    cur_->is_ref = false;  // the current picture does not count in the window
    sliding_window();
    cur_->is_ref = true;
  }
}

void Dpb::apply_mmco(const SliceHeader& sh) {
  for (int i = 0; i < sh.num_mmco; ++i) {
    const Mmco& m = sh.mmco[i];
    switch (m.op) {
      case 1: {
        const int pic_num = sh.frame_num - (static_cast<int>(m.a) + 1);
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_ && p.is_ref && !p.is_long && p.frame_num_wrap == pic_num) p.is_ref = false;
        }
        break;
      }
      case 2:
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx == static_cast<int>(m.a)) {
            p.is_ref = false;
            p.is_long = false;
          }
        }
        break;
      case 3: {
        const int pic_num = sh.frame_num - (static_cast<int>(m.a) + 1);
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx == static_cast<int>(m.b)) {
            p.is_ref = false;
            p.is_long = false;
          }
        }
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_ && p.is_ref && !p.is_long && p.frame_num_wrap == pic_num) {
            p.is_long = true;
            p.long_term_idx = static_cast<int>(m.b);
          }
        }
        break;
      }
      case 4:
        max_long_term_idx_ = static_cast<int>(m.a) - 1;
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx > max_long_term_idx_) {
            p.is_ref = false;
            p.is_long = false;
          }
        }
        break;
      case 5:
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_) {
            p.is_ref = false;
            p.is_long = false;
          }
        }
        max_long_term_idx_ = -1;
        break;
      case 6:
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p != cur_ && p.is_ref && p.is_long && p.long_term_idx == static_cast<int>(m.b)) {
            p.is_ref = false;
            p.is_long = false;
          }
        }
        cur_->is_long = true;
        cur_->long_term_idx = static_cast<int>(m.b);
        break;
      default:
        break;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Reference lists (8.2.4)

void Dpb::build_ref_lists(const SliceHeader& sh) {
  ref_count_[0] = ref_count_[1] = 0;
  for (int l = 0; l < 2; ++l)
    for (int i = 0; i <= kMaxRefs; ++i) ref_list_[l][i] = nullptr;
  if (sh.type == kSliceI) return;
  const int max_frame_num = 1 << sps_->log2_max_frame_num;
  PictureInfo* shorts[kMaxPictures];
  PictureInfo* longs[kMaxPictures];
  int ns = 0, nl = 0;
  for (int k = 0; k < slot_count_; ++k) {
    PictureInfo& p = *slots_[k];
    if (&p == cur_ || !p.is_ref) continue;
    if (p.is_long) {
      longs[nl++] = &p;
    } else {
      p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
      shorts[ns++] = &p;
    }
  }
  // sort long-term by index ascending
  for (int i = 1; i < nl; ++i)
    for (int j = i; j > 0 && longs[j]->long_term_idx < longs[j - 1]->long_term_idx; --j) {
      PictureInfo* t = longs[j];
      longs[j] = longs[j - 1];
      longs[j - 1] = t;
    }
  PictureInfo* init[2][kMaxPictures + 1];
  int init_n[2] = {0, 0};
  if (sh.type == kSliceP) {
    for (int i = 1; i < ns; ++i)
      for (int j = i; j > 0 && shorts[j]->frame_num_wrap > shorts[j - 1]->frame_num_wrap; --j) {
        PictureInfo* t = shorts[j];
        shorts[j] = shorts[j - 1];
        shorts[j - 1] = t;
      }
    for (int i = 0; i < ns; ++i) init[0][init_n[0]++] = shorts[i];
    for (int i = 0; i < nl; ++i) init[0][init_n[0]++] = longs[i];
  } else {
    // B: by picture order count around the current picture.
    PictureInfo* before[kMaxPictures];
    PictureInfo* after[kMaxPictures];
    int nb = 0, na = 0;
    for (int i = 0; i < ns; ++i) (shorts[i]->poc < pic_poc_ ? before[nb++] : after[na++]) = shorts[i];
    for (int i = 1; i < nb; ++i)
      for (int j = i; j > 0 && before[j]->poc > before[j - 1]->poc; --j) {
        PictureInfo* t = before[j];
        before[j] = before[j - 1];
        before[j - 1] = t;
      }
    for (int i = 1; i < na; ++i)
      for (int j = i; j > 0 && after[j]->poc < after[j - 1]->poc; --j) {
        PictureInfo* t = after[j];
        after[j] = after[j - 1];
        after[j - 1] = t;
      }
    for (int i = 0; i < nb; ++i) init[0][init_n[0]++] = before[i];
    for (int i = 0; i < na; ++i) init[0][init_n[0]++] = after[i];
    for (int i = 0; i < na; ++i) init[1][init_n[1]++] = after[i];
    for (int i = 0; i < nb; ++i) init[1][init_n[1]++] = before[i];
    for (int i = 0; i < nl; ++i) {
      init[0][init_n[0]++] = longs[i];
      init[1][init_n[1]++] = longs[i];
    }
    if (init_n[1] > 1) {
      bool same = init_n[0] == init_n[1];
      for (int i = 0; same && i < init_n[0]; ++i) same = init[0][i] == init[1][i];
      if (same) {
        PictureInfo* t = init[1][0];
        init[1][0] = init[1][1];
        init[1][1] = t;
      }
    }
  }
  const int max_frame = 1 << sps_->log2_max_frame_num;
  for (int l = 0; l < (sh.type == kSliceB ? 2 : 1); ++l) {
    const int n = sh.num_ref_idx[l];
    PictureInfo* list[kMaxRefs + 2];
    for (int i = 0; i < kMaxRefs + 2; ++i) list[i] = nullptr;
    for (int i = 0; i < n && i < init_n[l]; ++i) list[i] = init[l][i];
    // modification
    int pred = sh.frame_num;
    int idx = 0;
    for (int m = 0; m < sh.num_mod[l] && idx < n; ++m) {
      const RefMod& rm = sh.mods[l][m];
      PictureInfo* target = nullptr;
      if (rm.idc < 2) {
        const int diff = static_cast<int>(rm.value) + 1;
        int no_wrap;
        if (rm.idc == 0) {
          no_wrap = pred - diff;
          if (no_wrap < 0) no_wrap += max_frame;
        } else {
          no_wrap = pred + diff;
          if (no_wrap >= max_frame) no_wrap -= max_frame;
        }
        pred = no_wrap;
        const int pic_num = no_wrap > sh.frame_num ? no_wrap - max_frame : no_wrap;
        for (int i = 0; i < ns; ++i)
          if (shorts[i]->frame_num_wrap == pic_num) target = shorts[i];
      } else {
        for (int i = 0; i < nl; ++i)
          if (longs[i]->long_term_idx == static_cast<int>(rm.value)) target = longs[i];
      }
      if (!target) continue;
      for (int c = n; c > idx; --c) list[c] = list[c - 1];
      list[idx++] = target;
      int w = idx;
      for (int c = idx; c <= n; ++c)
        if (list[c] != target) list[w++] = list[c];
    }
    // Entries without a picture repeat the first one so a damaged stream never reads a missing reference.
    PictureInfo* fallback = nullptr;
    for (int i = 0; i < n; ++i)
      if (list[i]) {
        fallback = list[i];
        break;
      }
    if (!fallback) fallback = shorts_any_or_cur();
    for (int i = 0; i < n; ++i) ref_list_[l][i] = list[i] ? list[i] : fallback;
    ref_count_[l] = n;
  }
}

PictureInfo* Dpb::shorts_any_or_cur() const {
  for (int i = 0; i < slot_count_; ++i)
    if (slots_[i] != cur_ && slots_[i]->is_ref) return slots_[i];
  return cur_;
}

}  // namespace h264
}  // namespace hardware
