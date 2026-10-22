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

#include "Network/Quic/QuicStreams.hpp"

namespace network::quic {

uint64_t SendBuffer::write(ConstByteView data, uint64_t limit) {
  if (fin_queued_ || data.empty()) return 0;
  const uint64_t room = limit > unacknowledged() ? limit - unacknowledged() : 0;
  const uint64_t n = data.size() < room ? data.size() : room;
  if (n == 0 || !data_.append(data.first(n))) return 0;
  end_ += n;
  return n;
}

bool SendBuffer::next(uint64_t max_length, uint64_t flow_limit, uint64_t* offset, ConstByteView* data, bool* fin) {
  *fin = false;
  if (max_length == 0 && !lost_.empty()) return false;
  // lost data first (it was counted against flow control when it first went out)
  if (!lost_.empty()) {
    const Range r = lost_.at(0);
    uint64_t length = r.last - r.first + 1;
    if (length > max_length) length = max_length;
    *offset = r.first;
    *data = data_.view().subview(drop_ + (r.first - base_), length);
    lost_.remove(r.first, r.first + length - 1);
    if (fin_lost_ && r.first + length == end_) {   // the end of the stream was in the lost packet too
      *fin = true;
      fin_lost_ = false;
    }
    return true;
  }
  if (next_ < end_) {
    uint64_t allowed = flow_limit > next_ ? flow_limit - next_ : 0;
    uint64_t length = end_ - next_;
    if (length > allowed) length = allowed;
    if (length > max_length) length = max_length;
    if (length == 0) return false;
    *offset = next_;
    *data = data_.view().subview(drop_ + (next_ - base_), length);
    next_ += length;
    if (fin_queued_ && next_ == end_ && !fin_sent_) {
      *fin = true;
      fin_sent_ = true;
    }
    return true;
  }
  if ((fin_queued_ && !fin_sent_) || fin_lost_) {   // the end of the stream on its own
    *offset = end_;
    *data = ConstByteView();
    *fin = true;
    fin_sent_ = true;
    fin_lost_ = false;
    return true;
  }
  return false;
}

void SendBuffer::on_acked(uint64_t offset, uint64_t length, bool fin) {
  if (fin) fin_acked_ = true;
  if (length > 0) {
    lost_.remove(offset, offset + length - 1);   // (it arrived after all)
    if (offset + length > base_) acked_.add(offset > base_ ? offset : base_, offset + length - 1);
  }
  // the acknowledged front of the data is let go
  while (!acked_.empty() && acked_.smallest() <= base_) {
    const uint64_t last = acked_.at(0).last;
    acked_.remove(0, last);
    if (last + 1 > base_) {
      drop_ += last + 1 - base_;
      base_ = last + 1;
    }
  }
  lost_.remove_below(base_);
  if (drop_ >= (64u << 10) && drop_ > data_.size() / 2) compact();
}

void SendBuffer::compact() {
  ByteBuffer rest(allocator_);
  if (!rest.append(data_.view().subview(drop_))) return;
  data_.clear();
  if (!data_.append(rest.view())) return;
  drop_ = 0;
}

void SendBuffer::on_lost(uint64_t offset, uint64_t length, bool fin) {
  if (fin && !fin_acked_) fin_lost_ = true;
  if (length == 0) return;
  uint64_t first = offset, last = offset + length - 1;
  if (last < base_) return;
  if (first < base_) first = base_;
  // parts that were acknowledged in the meantime need no repeat
  uint64_t at = first;
  while (at <= last) {
    if (acked_.contains(at)) {
      ++at;
      continue;
    }
    uint64_t end = at;
    while (end + 1 <= last && !acked_.contains(end + 1)) ++end;
    lost_.add(at, end);
    at = end + 1;
  }
}

RecvBuffer::Result RecvBuffer::add(uint64_t offset, ConstByteView data, bool fin) {
  const uint64_t end = offset + data.size();
  if (fin) {
    if ((fin_known_ && final_size_ != end) || end < highest_) return Result::FinalSize;
    fin_known_ = true;
    final_size_ = end;
  } else if (fin_known_ && end > final_size_) {
    return Result::FinalSize;
  }
  if (end > highest_) highest_ = end;
  if (data.empty()) return Result::Ok;
  const uint64_t have = contiguous_end();
  if (end <= have) return Result::Ok;   // all of it was had
  if (offset <= have) {                 // it joins the in-order data
    if (!ready_.append(data.subview(have - offset))) return Result::OutOfMemory;
  } else {                              // out of order: kept apart
    auto seg = UniquePointer<Segment>::make(allocator_, allocator_);
    if (!seg) return Result::OutOfMemory;
    seg->offset = offset;
    if (!seg->bytes.append(data)) return Result::OutOfMemory;
    uint64_t i = 0;
    while (i < pending_.count() && pending_[i]->offset < offset) ++i;
    if (!pending_.insert(i, util::move(seg))) return Result::OutOfMemory;
  }
  // pieces that now touch the in-order data (or overlap what is there) are taken in
  while (pending_.count() > 0 && pending_[0]->offset <= contiguous_end()) {
    const Segment& s = *pending_[0];
    const uint64_t s_end = s.offset + s.bytes.size();
    const uint64_t cur = contiguous_end();
    if (s_end > cur && !ready_.append(s.bytes.view().subview(cur - s.offset))) return Result::OutOfMemory;
    pending_.remove_at(0);
  }
  return Result::Ok;
}

void RecvBuffer::discard(uint64_t n) {
  if (n > available()) n = available();
  read_ += n;
  if (read_ == ready_.size()) {
    read_base_ += read_;
    read_ = 0;
    ready_.clear();
  }
}

uint64_t RecvBuffer::read(ByteView out) {
  const uint64_t n = out.size() < available() ? out.size() : available();
  if (n == 0) return 0;
  out.first(n).copy_from(ready_.view().subview(read_, n));
  read_ += n;
  if (read_ == ready_.size()) {   // all read: start over
    read_base_ += read_;
    read_ = 0;
    ready_.clear();
  } else if (read_ >= (64u << 10) && read_ > ready_.size() / 2) {
    ByteBuffer rest(allocator_);
    if (rest.append(ready_.view().subview(read_))) {
      ready_.clear();
      ready_.append(rest.view());
      read_base_ += read_;
      read_ = 0;
    }
  }
  return n;
}

}  // namespace network::quic
