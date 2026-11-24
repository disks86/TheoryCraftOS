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
//
// A slot is a frame. Each of its two fields has its own reference marking (PictureInfo::field_ref): a frame picture marks both, a field picture its own. A field
// picture's reference lists are lists of fields (8.2.4.2.2, 8.2.4.2.4, 8.2.4.2.5), a frame's are lists of frames whose two fields are both marked alike.

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

namespace {

void sync_ref(PictureInfo& p) {
  p.is_ref = p.field_ref[0] != 0 || p.field_ref[1] != 0;
  p.is_long = p.field_ref[0] == 2 || p.field_ref[1] == 2;
}

void unmark(PictureInfo& p) {
  p.field_ref[0] = p.field_ref[1] = 0;
  sync_ref(p);
}

bool has_mmco5(const SliceHeader& sh) {
  if (sh.nal_ref_idc == 0) return false;
  for (int i = 0; i < sh.num_mmco; ++i)
    if (sh.mmco[i].op == 5) return true;
  return false;
}

// A frame (both fields) marked short-term / long-term: what a frame picture's lists hold (8.2.4.2.1, 8.2.4.2.3).
bool frame_short(const PictureInfo& p) { return p.field_ref[0] == 1 && p.field_ref[1] == 1; }
bool frame_long(const PictureInfo& p) { return p.field_ref[0] == 2 && p.field_ref[1] == 2; }

}  // namespace

void Dpb::attach(PictureInfo* const* slots, int count) {
  slot_count_ = count < kMaxPictures ? count : kMaxPictures;
  for (int i = 0; i < slot_count_; ++i) {
    slots_[i] = slots[i];
    slots_[i]->slot = i;
  }
  cur_ = returned_ = pending_ = nullptr;
  out_count_ = out_wait_count_ = 0;
}

void Dpb::clear_pictures() {
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo& p = *slots_[i];
    p.in_use = false;
    unmark(p);
    p.needed_for_output = false;
    p.has_key = false;
    p.decoded = 0;
  }
  out_count_ = 0;
  out_wait_count_ = 0;
  returned_ = nullptr;
  cur_ = nullptr;
  pending_ = nullptr;
}

void Dpb::reset() {
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo& p = *slots_[i];
    unmark(p);
    p.needed_for_output = false;
    p.has_key = false;
  }
  out_count_ = 0;
  out_wait_count_ = 0;
  cur_ = nullptr;
  pending_ = nullptr;
  prev_poc_msb_ = prev_poc_lsb_ = 0;
  prev_frame_num_ = prev_frame_num_offset_ = 0;
  returned_ = nullptr;
}

PictureInfo* Dpb::find_free_slot() const {
  PictureInfo* empty = nullptr;
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo* p = slots_[i];
    if (p == cur_ || p == returned_ || p == pending_ || p->is_ref || p->needed_for_output) continue;
    if (p->has_memory) return p;  // memory already there
    if (!empty) empty = p;
  }
  return empty;
}

// A first field whose second field did not come (C.4.5.3: a non-paired field): it goes out as it is.
void Dpb::release_pending() {
  PictureInfo* p = pending_;
  pending_ = nullptr;
  if (!p || decode_order_) return;   // (in decoding order nothing is queued: it went out when it was decoded)
  p->needed_for_output = true;
  queue_output(p);
}

PictureInfo* Dpb::begin_picture(const SliceHeader& sh, int64_t pts, PictureInfo* into) {
  const int structure = !sh.field_pic ? 3 : sh.bottom_field ? 2 : 1;
  PictureInfo* pair = into;
  if (!pair && pending_) {
    // 7.4.1.2.4 / 3.30, 3.29: the second field of a complementary (reference or non-reference) field pair.
    const bool ref = sh.nal_ref_idc != 0;
    if (structure != 3 && structure != pending_structure_ && pending_->frame_num == sh.frame_num && !sh.idr() && ref == pending_ref_ && !has_mmco5(sh)) {
      pair = pending_;
      pending_ = nullptr;
    } else {
      release_pending();
    }
  }
  structure_ = structure;
  second_field_ = pair != nullptr;
  cur_long_ = false;
  if (pair) {
    cur_ = pair;
    compute_poc(sh);
    if (structure == 1) cur_->top_poc = top_poc_;
    else cur_->bottom_poc = bottom_poc_;
    pic_poc_ = structure == 1 ? top_poc_ : bottom_poc_;
    cur_->in_use = true;
    cur_->field_coded = true;
    cur_->field_ref[structure - 1] = 0;
    sync_ref(*cur_);
    cur_->keyframe = cur_->keyframe && sh.type == kSliceI;
    return cur_;
  }
  if (sh.idr()) {
    // Everything before an IDR picture is output first.
    bump_output(true);
    for (int i = 0; i < slot_count_; ++i) unmark(*slots_[i]);
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
    unmark(*oldest);
    pic = find_free_slot();
  }
  cur_ = pic;
  cur_->in_use = true;
  cur_->id = next_pic_id_++;
  if (structure == 3) {
    pic_poc_ = top_poc_ < bottom_poc_ ? top_poc_ : bottom_poc_;
    cur_->top_poc = top_poc_;
    cur_->bottom_poc = bottom_poc_;
  } else {
    // A field: both counts are its own until the other field comes (only the decoded fields' count is used).
    pic_poc_ = structure == 1 ? top_poc_ : bottom_poc_;
    cur_->top_poc = cur_->bottom_poc = pic_poc_;
  }
  cur_->poc = pic_poc_;
  cur_->frame_num = sh.frame_num;
  unmark(*cur_);
  cur_->needed_for_output = false;
  cur_->non_existing = false;
  cur_->decoded = 0;
  cur_->field_coded = structure != 3;
  cur_->mbaff = false;
  cur_->keyframe = sh.idr() || sh.type == kSliceI;
  cur_->pts = pts;
  return cur_;
}

void Dpb::set_current_poc(int top, int bottom) {
  if (structure_ == 3) {
    top_poc_ = top;
    bottom_poc_ = bottom;
    pic_poc_ = top < bottom ? top : bottom;
    if (cur_) {
      cur_->poc = pic_poc_;
      cur_->top_poc = top;
      cur_->bottom_poc = bottom;
    }
    return;
  }
  const int own = structure_ == 1 ? top : bottom;
  top_poc_ = bottom_poc_ = pic_poc_ = own;
  if (!cur_) return;
  if (structure_ == 1) cur_->top_poc = own;
  else cur_->bottom_poc = own;
  if (!second_field_) cur_->top_poc = cur_->bottom_poc = own;
}

void Dpb::abort_picture() {
  if (cur_ && !second_field_) cur_->in_use = false;
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
    // (8-4, 8-5: a field's count is PicOrderCntMsb + pic_order_cnt_lsb whichever its parity; delta_pic_order_cnt_bottom is 0 in a field's header)
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
      // (8-10: a bottom field's count is expected + offset_for_top_to_bottom_field + delta_pic_order_cnt[ 0 ]; delta_pic_order_cnt[ 1 ] is 0 in a field's header)
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
  const bool mmco5 = has_mmco5(sh);
  if (sh.nal_ref_idc != 0) mark_references(sh);
  cur_->decoded = static_cast<uint8_t>(cur_->decoded | structure_);
  if (mmco5) {
    // 8.2.1: tempPicOrderCnt is PicOrderCnt( CurrPic ); the picture is treated as having frame_num 0 and the order counts restart from it.
    bump_output(true);
    if (structure_ == 3) {
      const int temp = top_poc_ < bottom_poc_ ? top_poc_ : bottom_poc_;
      top_poc_ -= temp;
      bottom_poc_ -= temp;
      cur_->top_poc = top_poc_;
      cur_->bottom_poc = bottom_poc_;
    } else {
      // (the field's own count becomes 0; a second field is never one with operation 5, so there is no other)
      top_poc_ = bottom_poc_ = 0;
      cur_->top_poc = cur_->bottom_poc = 0;
    }
    cur_->frame_num = 0;
  }
  if (cur_->decoded == 3) cur_->poc = cur_->top_poc < cur_->bottom_poc ? cur_->top_poc : cur_->bottom_poc;
  else cur_->poc = cur_->decoded == 1 ? cur_->top_poc : cur_->bottom_poc;
  // State for the next picture's order count.
  const Sps& s = *sps_;
  if (sh.nal_ref_idc != 0) {
    if (mmco5) {
      prev_poc_msb_ = 0;
      prev_poc_lsb_ = structure_ == 2 ? 0 : top_poc_;  // tempPicOrderCnt-adjusted TopFieldOrderCnt; 0 after a bottom field (8.2.1.1)
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
  PictureInfo* done = cur_;
  cur_ = nullptr;
  if (structure_ != 3 && !second_field_) {
    // A first field: its frame goes out once the second field is decoded (or when the next picture is not that field).
    pending_ = done;
    pending_structure_ = structure_;
    pending_ref_ = sh.nal_ref_idc != 0;
  }
  if (decode_order_) {
    done->needed_for_output = false;
    returned_ = done;
    return;
  }
  if (pending_ == done) return;
  done->needed_for_output = true;
  queue_output(done);
}

// Pictures wait here until the display order allows them out.
void Dpb::queue_output(PictureInfo* p) {
  if (out_wait_count_ >= kMaxPictures) bump_output(false);
  out_wait_[out_wait_count_++] = p;
  const int depth = sps_->num_reorder_frames >= 0 ? sps_->num_reorder_frames : (sps_->poc_type == 2 ? 0 : sps_->dpb_frames);
  while (out_wait_count_ > depth) bump_output(false);
}

// Moves the earliest waiting picture (or all of them) to the queue the user receives from.
void Dpb::bump_output(bool all) {
  if (all && pending_ && decode_order_) pending_ = nullptr;
  if (all && pending_ && out_wait_count_ < kMaxPictures) {
    PictureInfo* p = pending_;
    pending_ = nullptr;
    p->needed_for_output = true;
    out_wait_[out_wait_count_++] = p;
  }
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

// 8.2.5.3: the oldest short-term frame (or field pair, or field) is dropped when the frames in use reach max_num_ref_frames.
void Dpb::sliding_window() {
  if (structure_ != 3 && second_field_ && cur_->field_ref[2 - structure_] == 1) return;   // the second field of a pair whose first field is short-term
  int refs = 0;
  for (int i = 0; i < slot_count_; ++i)
    if (slots_[i]->is_ref) ++refs;   // (the current picture is not marked yet; the first field of the current frame counts)
  const int max_refs = sps_->max_num_ref_frames > 0 ? sps_->max_num_ref_frames : 1;
  while (refs >= max_refs) {
    PictureInfo* oldest = nullptr;
    for (int i = 0; i < slot_count_; ++i) {
      PictureInfo* p = slots_[i];
      if (p != cur_ && (p->field_ref[0] == 1 || p->field_ref[1] == 1) && (!oldest || p->frame_num_wrap < oldest->frame_num_wrap)) oldest = p;
    }
    if (!oldest) break;
    unmark(*oldest);
    --refs;
  }
}

void Dpb::mark_references(const SliceHeader& sh) {
  const int max_frame_num = 1 << sps_->log2_max_frame_num;
  for (int i = 0; i < slot_count_; ++i) {
    PictureInfo& p = *slots_[i];
    if (p.field_ref[0] == 1 || p.field_ref[1] == 1) p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
  }
  cur_->frame_num_wrap = sh.frame_num;
  const uint8_t own = structure_ == 3 ? 3 : static_cast<uint8_t>(structure_);
  if (sh.idr()) {
    if (sh.long_term_reference) {
      for (int f = 0; f < 2; ++f)
        if (own & (1 << f)) cur_->field_ref[f] = 2;
      cur_->long_term_idx = 0;
      max_long_term_idx_ = 0;
    } else {
      for (int f = 0; f < 2; ++f)
        if (own & (1 << f)) cur_->field_ref[f] = 1;
      max_long_term_idx_ = -1;
    }
    sync_ref(*cur_);
    return;
  }
  if (sh.adaptive_marking) apply_mmco(sh);
  else sliding_window();
  if (!cur_long_) {
    // 8.2.5.1 step 3: marked short-term (both fields of a frame, the field of a field picture).
    for (int f = 0; f < 2; ++f)
      if (own & (1 << f)) cur_->field_ref[f] = 1;
  }
  sync_ref(*cur_);
}

// PicNum of field `parity` (0 top, 1 bottom) of a short-term frame when the current picture is a field (8-30, 8-31).
int Dpb::field_pic_num(const PictureInfo& p, int parity) const {
  return 2 * p.frame_num_wrap + (parity == structure_ - 1 ? 1 : 0);
}

void Dpb::apply_mmco(const SliceHeader& sh) {
  const bool field = structure_ != 3;
  const int cur_parity = structure_ - 1;   // (field pictures)
  const int curr_pic_num = field ? 2 * sh.frame_num + 1 : sh.frame_num;
  // Whether field f of slot p is the field being decoded now (never a target: it is not marked yet).
  auto is_current = [&](const PictureInfo& p, int f) { return &p == cur_ && (!field || f == cur_parity); };
  for (int i = 0; i < sh.num_mmco; ++i) {
    const Mmco& m = sh.mmco[i];
    switch (m.op) {
      case 1: {   // 8.2.5.4.1
        const int pic_num = curr_pic_num - (static_cast<int>(m.a) + 1);
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (!field) {
            if (&p != cur_ && (p.field_ref[0] == 1 || p.field_ref[1] == 1) && p.frame_num_wrap == pic_num) unmark(p);
            continue;
          }
          for (int f = 0; f < 2; ++f)
            if (!is_current(p, f) && p.field_ref[f] == 1 && field_pic_num(p, f) == pic_num) p.field_ref[f] = 0;
          sync_ref(p);
        }
        break;
      }
      case 2:   // 8.2.5.4.2
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (!field) {
            if (&p != cur_ && p.is_long && p.long_term_idx == static_cast<int>(m.a)) unmark(p);
            continue;
          }
          for (int f = 0; f < 2; ++f)
            if (!is_current(p, f) && p.field_ref[f] == 2 && 2 * p.long_term_idx + (f == cur_parity ? 1 : 0) == static_cast<int>(m.a)) p.field_ref[f] = 0;
          sync_ref(p);
        }
        break;
      case 3: {   // 8.2.5.4.3
        const int pic_num = curr_pic_num - (static_cast<int>(m.a) + 1);
        const int idx = static_cast<int>(m.b);
        PictureInfo* target = nullptr;
        int target_field = -1;
        for (int k = 0; k < slot_count_ && !target; ++k) {
          PictureInfo& p = *slots_[k];
          if (!field) {
            if (&p != cur_ && (p.field_ref[0] == 1 || p.field_ref[1] == 1) && p.frame_num_wrap == pic_num) target = &p;
            continue;
          }
          for (int f = 0; f < 2 && !target; ++f)
            if (!is_current(p, f) && p.field_ref[f] == 1 && field_pic_num(p, f) == pic_num) {
              target = &p;
              target_field = f;
            }
        }
        // The index leaves the frame (or the field outside the target's frame) that had it.
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (&p == target || !p.is_long || p.long_term_idx != idx) continue;
          for (int f = 0; f < 2; ++f)
            if (p.field_ref[f] == 2 && !is_current(p, f)) p.field_ref[f] = 0;
          sync_ref(p);
        }
        if (target) {
          if (!field) {
            for (int f = 0; f < 2; ++f)
              if (target->field_ref[f] == 1) target->field_ref[f] = 2;
          } else {
            // (the other field of the frame keeps a different index only in a stream that breaks 8.2.5.4.3; it follows the frame's)
            target->field_ref[target_field] = 2;
          }
          target->long_term_idx = idx;
          sync_ref(*target);
        }
        break;
      }
      case 4:   // 8.2.5.4.4
        max_long_term_idx_ = static_cast<int>(m.a) - 1;
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (!p.is_long || p.long_term_idx <= max_long_term_idx_) continue;
          for (int f = 0; f < 2; ++f)
            if (p.field_ref[f] == 2 && !is_current(p, f)) p.field_ref[f] = 0;
          sync_ref(p);
        }
        break;
      case 5:   // 8.2.5.4.5
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          for (int f = 0; f < 2; ++f)
            if (!is_current(p, f)) p.field_ref[f] = 0;
          sync_ref(p);
        }
        max_long_term_idx_ = -1;
        break;
      case 6: {   // 8.2.5.4.6
        const int idx = static_cast<int>(m.b);
        for (int k = 0; k < slot_count_; ++k) {
          PictureInfo& p = *slots_[k];
          if (!p.is_long || p.long_term_idx != idx) continue;
          if (&p == cur_ && field) continue;   // the first field of the current frame keeps the index the pair shares
          for (int f = 0; f < 2; ++f)
            if (p.field_ref[f] == 2 && !is_current(p, f)) p.field_ref[f] = 0;
          sync_ref(p);
        }
        for (int f = 0; f < 2; ++f)
          if (!field || f == cur_parity) cur_->field_ref[f] = 2;
        cur_->long_term_idx = idx;
        cur_long_ = true;
        sync_ref(*cur_);
        break;
      }
      default:
        break;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Reference lists (8.2.4)

// PicOrderCnt( entry ) of a reference entry for a field's B lists (8.2.4.2.4): a frame or a field pair counts its smaller field, a field alone its own; the first
// field of the current frame counts on its own (NOTE 3).
int Dpb::entry_poc(const PictureInfo& p) const {
  if (&p == cur_) return structure_ == 1 ? p.bottom_poc : p.top_poc;
  if (p.decoded == 3) return p.top_poc < p.bottom_poc ? p.top_poc : p.bottom_poc;
  return p.decoded == 2 ? p.bottom_poc : p.top_poc;
}

void Dpb::build_ref_lists(const SliceHeader& sh) {
  ref_count_[0] = ref_count_[1] = 0;
  for (int l = 0; l < 2; ++l)
    for (int i = 0; i <= kMaxRefs; ++i) {
      ref_list_[l][i] = nullptr;
      ref_parity_[l][i] = 0;
    }
  if (sh.type == kSliceI) return;
  const bool field = structure_ != 3;
  const int same = structure_ - 1;   // (field pictures) the parity of the current field
  const int max_frame_num = 1 << sps_->log2_max_frame_num;
  // The frames holding references. A frame picture uses frames whose fields are both marked alike; a field picture any frame with a field marked, the current
  // frame's first field included.
  PictureInfo* shorts[kMaxPictures];
  PictureInfo* longs[kMaxPictures];
  int ns = 0, nl = 0;
  for (int k = 0; k < slot_count_; ++k) {
    PictureInfo& p = *slots_[k];
    if (!p.is_ref) continue;
    if (!field) {
      if (&p == cur_) continue;
      if (frame_short(p)) {
        p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
        shorts[ns++] = &p;
      } else if (frame_long(p)) {
        longs[nl++] = &p;
      }
      continue;
    }
    if (p.field_ref[0] == 1 || p.field_ref[1] == 1) {
      p.frame_num_wrap = p.frame_num > sh.frame_num ? p.frame_num - max_frame_num : p.frame_num;
      shorts[ns++] = &p;
    }
    if (p.field_ref[0] == 2 || p.field_ref[1] == 2) longs[nl++] = &p;
  }
  // long-term by index, ascending
  for (int i = 1; i < nl; ++i)
    for (int j = i; j > 0 && longs[j]->long_term_idx < longs[j - 1]->long_term_idx; --j) {
      PictureInfo* t = longs[j];
      longs[j] = longs[j - 1];
      longs[j - 1] = t;
    }
  PictureInfo* init[2][kMaxRefs + 1];
  uint8_t init_par[2][kMaxRefs + 1] = {};
  int init_n[2] = {0, 0};
  // 8.2.4.2.5: the fields of an ordered list of frames, alternating parity from the current field's (kind 1 short-term, 2 long-term).
  auto add_fields = [&](int l, PictureInfo* const* frames, int n, uint8_t kind) {
    int idx[2] = {0, 0};
    const int parity[2] = {same, 1 - same};
    for (;;) {
      for (int s = 0; s < 2; ++s)
        while (idx[s] < n && frames[idx[s]]->field_ref[parity[s]] != kind) ++idx[s];
      if (idx[0] >= n && idx[1] >= n) break;
      for (int s = 0; s < 2; ++s) {
        if (idx[s] >= n) continue;
        if (init_n[l] < kMaxRefs) {
          init[l][init_n[l]] = frames[idx[s]];
          init_par[l][init_n[l]] = static_cast<uint8_t>(parity[s] + 1);
          ++init_n[l];
        }
        ++idx[s];
      }
    }
  };
  auto add_frames = [&](int l, PictureInfo* const* frames, int n) {
    for (int i = 0; i < n && init_n[l] < kMaxRefs; ++i) init[l][init_n[l]++] = frames[i];
  };
  if (sh.type == kSliceP) {
    // short-term by FrameNumWrap (PicNum for frames), descending
    for (int i = 1; i < ns; ++i)
      for (int j = i; j > 0 && shorts[j]->frame_num_wrap > shorts[j - 1]->frame_num_wrap; --j) {
        PictureInfo* t = shorts[j];
        shorts[j] = shorts[j - 1];
        shorts[j - 1] = t;
      }
    if (field) {
      add_fields(0, shorts, ns, 1);
      add_fields(0, longs, nl, 2);
    } else {
      add_frames(0, shorts, ns);
      add_frames(0, longs, nl);
    }
  } else {
    // B: by picture order count around the current picture (a field's lists put an entry with the same count before it, 8.2.4.2.4).
    PictureInfo* before[kMaxPictures];
    PictureInfo* after[kMaxPictures];
    int nb = 0, na = 0;
    auto poc_of = [&](const PictureInfo* p) { return field ? entry_poc(*p) : p->poc; };
    for (int i = 0; i < ns; ++i) {
      const int poc = poc_of(shorts[i]);
      if (field ? poc <= pic_poc_ : poc < pic_poc_) before[nb++] = shorts[i];
      else after[na++] = shorts[i];
    }
    for (int i = 1; i < nb; ++i)
      for (int j = i; j > 0 && poc_of(before[j]) > poc_of(before[j - 1]); --j) {
        PictureInfo* t = before[j];
        before[j] = before[j - 1];
        before[j - 1] = t;
      }
    for (int i = 1; i < na; ++i)
      for (int j = i; j > 0 && poc_of(after[j]) < poc_of(after[j - 1]); --j) {
        PictureInfo* t = after[j];
        after[j] = after[j - 1];
        after[j - 1] = t;
      }
    PictureInfo* order[2][kMaxPictures];
    int no = 0;
    for (int i = 0; i < nb; ++i) order[0][no++] = before[i];
    for (int i = 0; i < na; ++i) order[0][no++] = after[i];
    no = 0;
    for (int i = 0; i < na; ++i) order[1][no++] = after[i];
    for (int i = 0; i < nb; ++i) order[1][no++] = before[i];
    for (int l = 0; l < 2; ++l) {
      if (field) {
        add_fields(l, order[l], ns, 1);
        add_fields(l, longs, nl, 2);
      } else {
        add_frames(l, order[l], ns);
        add_frames(l, longs, nl);
      }
    }
    if (init_n[1] > 1) {
      bool identical = init_n[0] == init_n[1];
      for (int i = 0; identical && i < init_n[0]; ++i) identical = init[0][i] == init[1][i] && init_par[0][i] == init_par[1][i];
      if (identical) {
        PictureInfo* t = init[1][0];
        init[1][0] = init[1][1];
        init[1][1] = t;
        const uint8_t tp = init_par[1][0];
        init_par[1][0] = init_par[1][1];
        init_par[1][1] = tp;
      }
    }
  }
  // 8.2.4.3: modifications. Picture numbers count fields in a field picture (CurrPicNum = 2 * frame_num + 1, MaxPicNum = 2 * MaxFrameNum).
  const int max_pic_num = field ? 2 * max_frame_num : max_frame_num;
  const int curr_pic_num = field ? 2 * sh.frame_num + 1 : sh.frame_num;
  for (int l = 0; l < (sh.type == kSliceB ? 2 : 1); ++l) {
    const int n = sh.num_ref_idx[l];
    PictureInfo* list[kMaxRefs + 2];
    uint8_t par[kMaxRefs + 2] = {};
    for (int i = 0; i < kMaxRefs + 2; ++i) list[i] = nullptr;
    for (int i = 0; i < n && i < init_n[l]; ++i) {
      list[i] = init[l][i];
      par[i] = init_par[l][i];
    }
    int pred = curr_pic_num;
    int idx = 0;
    for (int m = 0; m < sh.num_mod[l] && idx < n; ++m) {
      const RefMod& rm = sh.mods[l][m];
      PictureInfo* target = nullptr;
      uint8_t target_par = 0;
      if (rm.idc < 2) {
        const int diff = static_cast<int>(rm.value) + 1;
        int no_wrap;
        if (rm.idc == 0) {
          no_wrap = pred - diff;
          if (no_wrap < 0) no_wrap += max_pic_num;
        } else {
          no_wrap = pred + diff;
          if (no_wrap >= max_pic_num) no_wrap -= max_pic_num;
        }
        pred = no_wrap;
        const int pic_num = no_wrap > curr_pic_num ? no_wrap - max_pic_num : no_wrap;
        for (int i = 0; i < ns; ++i) {
          if (!field) {
            if (shorts[i]->frame_num_wrap == pic_num) target = shorts[i];
            continue;
          }
          for (int f = 0; f < 2; ++f)
            if (shorts[i]->field_ref[f] == 1 && field_pic_num(*shorts[i], f) == pic_num) {
              target = shorts[i];
              target_par = static_cast<uint8_t>(f + 1);
            }
        }
      } else {
        for (int i = 0; i < nl; ++i) {
          if (!field) {
            if (longs[i]->long_term_idx == static_cast<int>(rm.value)) target = longs[i];
            continue;
          }
          for (int f = 0; f < 2; ++f)
            if (longs[i]->field_ref[f] == 2 && 2 * longs[i]->long_term_idx + (f == same ? 1 : 0) == static_cast<int>(rm.value)) {
              target = longs[i];
              target_par = static_cast<uint8_t>(f + 1);
            }
        }
      }
      if (!target) continue;
      for (int c = n; c > idx; --c) {
        list[c] = list[c - 1];
        par[c] = par[c - 1];
      }
      list[idx] = target;
      par[idx] = target_par;
      ++idx;
      int w = idx;
      for (int c = idx; c <= n; ++c)
        if (list[c] != target || par[c] != target_par) {
          list[w] = list[c];
          par[w] = par[c];
          ++w;
        }
    }
    // Entries without a picture repeat the first one so a damaged stream never reads a missing reference.
    PictureInfo* fallback = nullptr;
    uint8_t fallback_par = 0;
    for (int i = 0; i < n; ++i)
      if (list[i]) {
        fallback = list[i];
        fallback_par = par[i];
        break;
      }
    if (!fallback) {
      fallback = shorts_any_or_cur();
      if (field) fallback_par = static_cast<uint8_t>(fallback == cur_ ? 2 - same : same + 1);
      if (field && fallback != cur_ && !(fallback->decoded & (same + 1))) fallback_par = static_cast<uint8_t>(fallback->decoded == 2 ? 2 : 1);
    }
    for (int i = 0; i < n; ++i) {
      ref_list_[l][i] = list[i] ? list[i] : fallback;
      ref_parity_[l][i] = list[i] ? par[i] : fallback_par;
    }
    ref_count_[l] = n;
  }
}

PictureInfo* Dpb::shorts_any_or_cur() const {
  for (int i = 0; i < slot_count_; ++i)
    if (slots_[i] != cur_ && slots_[i]->is_ref) return slots_[i];
  if (cur_ && second_field_) return cur_;   // the first field of the current frame
  for (int i = 0; i < slot_count_; ++i)
    if (slots_[i] != cur_ && slots_[i]->decoded) return slots_[i];
  return cur_;
}

}  // namespace h264
}  // namespace hardware
