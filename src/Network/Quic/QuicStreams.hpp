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

// The byte streams of QUIC (RFC 9000 2-3) apart from the connection: what is waiting to be sent on a stream, with what is lost sent again and what is
// acknowledged let go; and what has been received, put in order whatever order the packets came in. The same two serve the CRYPTO streams of the handshake.
#pragma once
#include "Network/Quic/QuicCommon.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace network::quic {

class SendBuffer {
 public:
  explicit SendBuffer(allocators::IAllocator& allocator) : allocator_(allocator), data_(allocator), acked_(allocator), lost_(allocator) {}
  SendBuffer(const SendBuffer&) = delete;
  SendBuffer& operator=(const SendBuffer&) = delete;

  // Queues bytes at the end, as many as fit under `limit` bytes that are queued and not acknowledged yet. Returns how many were taken.
  uint64_t write(ConstByteView data, uint64_t limit);
  // No more will be written: the stream ends after the bytes queued.
  void finish() { fin_queued_ = true; }
  bool finishing() const { return fin_queued_; }

  uint64_t end_offset() const { return end_; }                       // bytes written in all
  uint64_t next_offset() const { return next_; }                     // the first offset never sent
  uint64_t unacknowledged() const { return end_ - base_; }          // queued and not acknowledged
  uint64_t unsent() const { return end_ - next_; }                   // never sent
  bool has_pending() const { return !lost_.empty() || next_ < end_ || (fin_queued_ && !fin_sent_) || fin_lost_; }
  // Everything, the end included, has been acknowledged.
  bool done() const { return fin_queued_ && fin_acked_ && base_ == end_; }

  // The next piece to send: lost bytes first, else new ones - new ones only below `flow_limit` (the highest offset the peer allows, exclusive) - at most
  // `max_length` bytes. `*fin` says the piece ends the stream. False if there is nothing that may be sent now. The piece stays valid until the next call
  // of write(), on_acked() or next(). Taking a piece counts it as sent.
  bool next(uint64_t max_length, uint64_t flow_limit, uint64_t* offset, ConstByteView* data, bool* fin);
  // What the peer acknowledged / what was lost of an earlier piece.
  void on_acked(uint64_t offset, uint64_t length, bool fin);
  void on_lost(uint64_t offset, uint64_t length, bool fin);

 private:
  void compact();

  allocators::IAllocator& allocator_;
  ByteBuffer data_;
  uint64_t drop_ = 0;         // bytes at the front of data_ that are no longer needed
  uint64_t base_ = 0;         // stream offset of data_[drop_]: everything before is acknowledged
  uint64_t next_ = 0;         // first offset never sent
  uint64_t end_ = 0;          // offset after the last byte written
  RangeSet acked_;            // acknowledged ranges beyond base_
  RangeSet lost_;             // ranges to send again
  bool fin_queued_ = false, fin_sent_ = false, fin_lost_ = false, fin_acked_ = false;
};

class RecvBuffer {
 public:
  explicit RecvBuffer(allocators::IAllocator& allocator) : allocator_(allocator), ready_(allocator), pending_(allocator) {}
  RecvBuffer(const RecvBuffer&) = delete;
  RecvBuffer& operator=(const RecvBuffer&) = delete;

  enum class Result : uint8_t { Ok, FinalSize, OutOfMemory };
  // Takes `data` that starts at stream offset `offset` (and ends the stream if `fin`). Data already had is dropped. FinalSize: the stream's end moved, or data
  // came past it (a protocol violation).
  Result add(uint64_t offset, ConstByteView data, bool fin);
  // Bytes that can be read now (in order from the read position).
  uint64_t available() const { return ready_.size() - read_; }
  ConstByteView peek() const { return ready_.view().subview(read_); }
  uint64_t read(ByteView out);
  // Throws away the next `n` bytes that can be read.
  void discard(uint64_t n);
  // Offset of the next byte to be read / after the last byte that is in order.
  uint64_t read_offset() const { return read_base_ + read_; }
  uint64_t contiguous_end() const { return read_base_ + ready_.size(); }
  // The end of the stream is known (fin received) / everything has been read.
  bool fin_known() const { return fin_known_; }
  uint64_t final_size() const { return final_size_; }
  bool finished() const { return fin_known_ && read_offset() == final_size_; }
  // The highest offset seen plus one (what flow control counts).
  uint64_t highest() const { return highest_; }

 private:
  struct Segment {
    explicit Segment(allocators::IAllocator& a) : bytes(a) {}
    uint64_t offset = 0;
    ByteBuffer bytes;
  };

  allocators::IAllocator& allocator_;
  ByteBuffer ready_;
  uint64_t read_ = 0;         // bytes of ready_ already read
  uint64_t read_base_ = 0;    // stream offset of ready_[0]
  List<UniquePointer<Segment>> pending_;   // out of order pieces, sorted by offset, none touching the in-order data or each other
  bool fin_known_ = false;
  uint64_t final_size_ = 0, highest_ = 0;
};

}  // namespace network::quic
