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

#include "Media/Lzx.hpp"
#include "Utility/ByteStream.hpp"

namespace media {

namespace {

constexpr uint32_t kVerbatim = 1, kAligned = 2, kUncompressed = 3;
constexpr uint32_t kLiterals = 256, kMinMatch = 2, kPrimaryLengths = 7;
constexpr uint32_t kMaxPositionSlots = 50;
constexpr uint32_t kMaxMainSymbols = kLiterals + kMaxPositionSlots * 8, kLengthSymbols = 249, kPreSymbols = 20, kAlignedSymbols = 8;
// A run in the coding of the code lengths may go on past the end of its tree (the reference decoders let it: up to 51 lengths), into the tree's next part
// or into this slack.
constexpr uint32_t kLengthSlack = 64;
constexpr uint32_t kMaxCodeLength = 16;
constexpr uint32_t kFastBits = 10;
// The call translation stops after the first gigabyte of a stream.
constexpr uint64_t kTranslationLimit = uint64_t{1} << 30;

// A match's offset is sent as a position slot and, for all but the first few, some more bits: slot s covers the offsets base[s] .. base[s] + 2^extra[s] - 1
// (plus two: the first three values are the repeated offsets).
struct PositionSlots {
  uint8_t extra[kMaxPositionSlots + 1];
  uint32_t base[kMaxPositionSlots + 1];
  constexpr PositionSlots() : extra(), base() {
    uint32_t next = 0;
    for (uint32_t slot = 0; slot <= kMaxPositionSlots; ++slot) {
      const uint32_t bits = slot < 4 ? 0 : (slot >> 1) - 1;
      extra[slot] = static_cast<uint8_t>(bits > 17 ? 17 : bits);
      base[slot] = next;
      next += uint32_t{1} << extra[slot];
    }
  }
};
constexpr PositionSlots kSlots;

// How many position slots a window of 2^bits bytes has: as many as reach its size.
constexpr uint32_t position_slots(uint32_t window_bits) {
  uint32_t slots = 0;
  while (slots < kMaxPositionSlots && kSlots.base[slots] < (uint32_t{1} << window_bits)) ++slots;
  return slots;
}

// The bit stream: 16 bit little-endian words, the most significant bit of each first. Past the end of the data it reads zeros (a code is looked up 16 bits
// at a time, so the last one of a block is looked at beyond its end); having *used* bits that are not there is the error, and overrun() tells.
class BitReader {
 public:
  void start(ConstByteView data, uint64_t at) {
    data_ = data;
    start_ = next_ = at;
    bits_ = 0;
    count_ = 0;
    used_ = 0;
  }
  // The next `count` bits (at most 32), without using them.
  uint32_t peek(uint32_t count) {
    while (count_ < count) {
      uint8_t low = 0, high = 0;
      if (next_ < data_.size()) {
        low = data_[next_];
        if (next_ + 1 < data_.size()) high = data_[next_ + 1];
        next_ += 2;
      }
      bits_ = (bits_ << 16) | static_cast<uint64_t>(low | (high << 8));
      count_ += 16;
    }
    return static_cast<uint32_t>((bits_ >> (count_ - count)) & ((uint64_t{1} << count) - 1));
  }
  void drop(uint32_t count) {
    count_ -= count;      // (only after a peek of at least as many)
    used_ += count;
  }
  uint32_t read(uint32_t count) {
    if (count == 0) return 0;
    const uint32_t value = peek(count);
    drop(count);
    return value;
  }
  // Before the raw bytes of an uncompressed block: on to the next 16 bit boundary - a whole word if the stream is at one already.
  void skip_to_word() {
    const uint32_t pad = 16 - static_cast<uint32_t>(used_ & 15);
    peek(pad);
    drop(pad);
  }
  // Where the byte after the bits used so far is (meaningful at a 16 bit boundary).
  uint64_t byte_position() const { return start_ + used_ / 8; }
  bool overrun() const { return start_ > data_.size() || used_ > (data_.size() - start_) * 8; }

 private:
  ConstByteView data_;
  uint64_t start_ = 0, next_ = 0;
  uint64_t bits_ = 0;
  uint32_t count_ = 0;
  uint64_t used_ = 0;
};

// A canonical Huffman code: a table for the codes of up to kFastBits bits, the counting form for the longer ones.
struct HuffmanCode {
  uint16_t fast[1 << kFastBits];             // symbol | length << 10; 0 where the code is longer
  uint16_t sorted[kMaxMainSymbols];          // the symbols by length, then by number
  uint16_t count[kMaxCodeLength + 1];        // how many codes have each length
  uint32_t first[kMaxCodeLength + 1];        // the first code of each length
  uint16_t offset[kMaxCodeLength + 1];       // ... and where its symbols start in `sorted`
  bool usable = false;

  // From the code lengths (0: the symbol is not used). False if they do not make a complete prefix code. All zeros is accepted - a block need not use its
  // length tree - but nothing can then be decoded with it.
  bool build(ConstByteView lengths) {
    usable = false;
    if (lengths.size() > kMaxMainSymbols) return false;
    for (uint16_t& c : count) c = 0;
    for (const uint8_t length : lengths) {
      if (length > kMaxCodeLength) return false;
      ++count[length];
    }
    if (count[0] == lengths.size()) return true;
    uint32_t code = 0, at = 0;
    uint16_t next[kMaxCodeLength + 1];
    for (uint32_t length = 1; length <= kMaxCodeLength; ++length) {
      code <<= 1;
      first[length] = code;
      offset[length] = next[length] = static_cast<uint16_t>(at);
      code += count[length];
      at += count[length];
      if (code > (uint32_t{1} << length)) return false;      // more codes than this length has room for
    }
    if (code != (uint32_t{1} << kMaxCodeLength)) return false;      // codes are left over: no encoder makes such a tree, and the reference decoders refuse it
    for (uint16_t& entry : fast) entry = 0;
    uint32_t fast_code[kFastBits + 1];
    for (uint32_t length = 1; length <= kFastBits; ++length) fast_code[length] = first[length];
    for (uint32_t symbol = 0; symbol < lengths.size(); ++symbol) {
      const uint32_t length = lengths[symbol];
      if (length == 0) continue;
      sorted[next[length]++] = static_cast<uint16_t>(symbol);
      if (length > kFastBits) continue;
      const uint32_t shift = kFastBits - length;
      const uint16_t entry = static_cast<uint16_t>(symbol | (length << 10));
      for (uint16_t& slot : ArrayView<uint16_t>(fast).subview(uint64_t{fast_code[length]++} << shift, uint64_t{1} << shift)) slot = entry;
    }
    usable = true;
    return true;
  }

  bool decode(BitReader& in, uint32_t* symbol) const {
    if (!usable) return false;
    const uint32_t bits = in.peek(kMaxCodeLength);
    const uint16_t entry = fast[bits >> (kMaxCodeLength - kFastBits)];
    if (entry != 0) {
      in.drop(entry >> 10);
      *symbol = entry & 0x3FF;
      return true;
    }
    for (uint32_t length = kFastBits + 1; length <= kMaxCodeLength; ++length) {
      const uint32_t code = bits >> (kMaxCodeLength - length);
      if (code < first[length] || code - first[length] >= count[length]) continue;
      in.drop(length);
      *symbol = sorted[offset[length] + (code - first[length])];
      return true;
    }
    return false;
  }
};

}  // namespace

struct LzxDecoder::State {
  explicit State(allocators::IAllocator& allocator) : window(allocator) {}

  ByteBuffer window;                 // the last window_size bytes of output, as a ring: frames never straddle its end, matches reach back over it
  uint32_t window_size = 0;
  uint32_t window_position = 0;      // where the next frame goes
  uint32_t main_symbols = 0;
  uint64_t produced = 0;             // bytes of output in the frames before this one
  bool header_read = false;
  bool failed = false;
  bool ended = false;                // a short frame was the stream's last
  uint32_t translation_size = 0;     // the "file size" of the call translation; 0: none
  uint32_t r0 = 1, r1 = 1, r2 = 1;   // the repeated offsets
  uint32_t block_type = 0;
  uint32_t block_remaining = 0;      // bytes of the current block not decoded yet (blocks go on over frames)
  bool pad_pending = false;          // the byte that follows an uncompressed block of odd size has not been passed yet
  uint8_t main_lengths[kMaxMainSymbols + kLengthSlack];
  uint8_t length_lengths[kLengthSymbols + kLengthSlack];
  HuffmanCode main_code, length_code, aligned_code, pre_code;

  bool start(uint32_t window_bits);
  bool read_lengths(BitReader& in, ByteView lengths, uint32_t count);
  bool read_block_header(BitReader& in, ConstByteView input, uint64_t* raw_at, bool* in_raw);
  bool decode_symbols(BitReader& in, ByteView window_bytes, uint32_t frame_start, uint32_t* position, uint32_t run);
  bool decode_frame(ConstByteView input, uint32_t out_size);
  void translate_calls(ByteView frame) const;
};

bool LzxDecoder::State::start(uint32_t window_bits) {
  window.clear();
  window_size = uint32_t{1} << window_bits;
  if (!window.put_zeros(window_size)) return false;
  window_position = 0;
  main_symbols = kLiterals + position_slots(window_bits) * 8;
  produced = 0;
  header_read = failed = ended = pad_pending = false;
  translation_size = 0;
  r0 = r1 = r2 = 1;
  block_type = block_remaining = 0;
  ByteView(main_lengths).fill(0);
  ByteView(length_lengths).fill(0);
  return true;
}

// The lengths of `count` codes, each sent as how far it is below the length the same symbol had in the block before (modulo 17), or as a run, in a
// Huffman code of its own (the pre-tree). `lengths` starts at the first of them and goes on to the end of the array they are in.
bool LzxDecoder::State::read_lengths(BitReader& in, ByteView lengths, uint32_t count) {
  uint8_t pre[kPreSymbols];
  for (uint8_t& length : pre) length = static_cast<uint8_t>(in.read(4));
  if (!pre_code.build(ConstByteView(pre))) return false;
  for (uint32_t at = 0; at < count;) {
    uint32_t symbol;
    if (!pre_code.decode(in, &symbol)) return false;
    if (symbol <= 16) {
      lengths[at] = static_cast<uint8_t>((lengths[at] + 17 - symbol) % 17);
      ++at;
      continue;
    }
    uint32_t run;
    uint8_t value = 0;
    if (symbol == 17) {
      run = 4 + in.read(4);
    } else if (symbol == 18) {
      run = 20 + in.read(5);
    } else {
      run = 4 + in.read(1);
      if (!pre_code.decode(in, &symbol) || symbol > 16) return false;
      value = static_cast<uint8_t>((lengths[at] + 17 - symbol) % 17);
    }
    ByteView target;
    if (!lengths.try_subview(at, run, &target)) return false;
    target.fill(value);
    at += run;
  }
  return true;
}

// The header of the next block. The bit reader is at it. For an uncompressed block, *raw_at becomes where its bytes start in the input.
bool LzxDecoder::State::read_block_header(BitReader& in, ConstByteView input, uint64_t* raw_at, bool* in_raw) {
  block_type = in.read(3);
  const uint32_t high = in.read(16);
  const uint32_t size = (high << 8) | in.read(8);
  if (size == 0) return false;
  switch (block_type) {
    case kAligned: {
      uint8_t aligned[kAlignedSymbols];
      for (uint8_t& length : aligned) length = static_cast<uint8_t>(in.read(3));
      if (!aligned_code.build(ConstByteView(aligned))) return false;
      [[fallthrough]];
    }
    case kVerbatim: {
      // The main tree comes in two parts, the literals and the matches, each with a pre-tree of its own.
      const ByteView main(main_lengths);
      if (!read_lengths(in, main, kLiterals) || !read_lengths(in, main.subview(kLiterals), main_symbols - kLiterals)) return false;
      if (!main_code.build(ConstByteView(main).first(main_symbols))) return false;
      if (!read_lengths(in, ByteView(length_lengths), kLengthSymbols)) return false;
      if (!length_code.build(ConstByteView(length_lengths).first(kLengthSymbols))) return false;
      break;
    }
    case kUncompressed: {
      in.skip_to_word();
      if (in.overrun()) return false;
      ConstByteView rest;
      if (!input.try_subview(in.byte_position(), input.size() - in.byte_position(), &rest)) return false;
      ByteReader offsets(rest);
      r0 = offsets.le32();
      r1 = offsets.le32();
      r2 = offsets.le32();
      if (!offsets.ok()) return false;
      *raw_at = in.byte_position() + 12;
      *in_raw = true;
      break;
    }
    default:
      return false;
  }
  block_remaining = size;
  pad_pending = block_type == kUncompressed && (size & 1) != 0;
  return !in.overrun();
}

// Decodes `run` bytes of a verbatim or aligned offset block into the window at *position.
bool LzxDecoder::State::decode_symbols(BitReader& in, ByteView window_bytes, uint32_t frame_start, uint32_t* position, uint32_t run) {
  uint32_t at = *position;
  const uint32_t end = at + run;
  while (at < end) {
    uint32_t symbol;
    if (!main_code.decode(in, &symbol)) return false;
    if (symbol < kLiterals) {
      window_bytes[at++] = static_cast<uint8_t>(symbol);
      continue;
    }
    symbol -= kLiterals;
    uint32_t length = symbol & kPrimaryLengths;
    if (length == kPrimaryLengths) {
      uint32_t more;
      if (!length_code.decode(in, &more)) return false;
      length += more;
    }
    length += kMinMatch;
    const uint32_t slot = symbol >> 3;
    uint32_t offset;
    if (slot == 0) {
      offset = r0;
    } else if (slot == 1) {
      offset = r1;
      r1 = r0;
      r0 = offset;
    } else if (slot == 2) {
      offset = r2;
      r2 = r0;
      r0 = offset;
    } else {
      const uint32_t extra = kSlots.extra[slot];
      uint32_t formatted = kSlots.base[slot];
      if (block_type == kAligned && extra >= 3) {
        // The low three bits of the offset have a Huffman code of their own.
        formatted += in.read(extra - 3) << 3;
        uint32_t low;
        if (!aligned_code.decode(in, &low)) return false;
        formatted += low;
      } else {
        formatted += in.read(extra);
      }
      offset = formatted - 2;      // (slot 3 and up: at least 1)
      r2 = r1;
      r1 = r0;
      r0 = offset;
    }
    // The match must lie in this block's part of the frame and copy from bytes that were output (the repeated offsets of an uncompressed block's header
    // can be anything).
    const uint64_t available = produced + (at - frame_start);
    if (length > end - at || offset == 0 || offset > available || offset > window_size) return false;
    const uint32_t from = at >= offset ? at - offset : at + (window_size - offset);
    if (offset >= length && from <= window_size - length) {
      window_bytes.subview(at, length).copy_from(ConstByteView(window_bytes).subview(from, length));
    } else {
      const uint32_t mask = window_size - 1;      // (the source may run over the window's end, or into the bytes this match writes)
      for (uint32_t i = 0; i < length; ++i) window_bytes[at + i] = window_bytes[(from + i) & mask];
    }
    at += length;
  }
  *position = at;
  return true;
}

// One frame of `out_size` bytes into the window at window_position.
bool LzxDecoder::State::decode_frame(ConstByteView input, uint32_t out_size) {
  const ByteView window_bytes = window.writable_view();
  const uint32_t frame_start = window_position;
  uint32_t position = frame_start;
  uint32_t todo = out_size;
  BitReader in;
  // An uncompressed block that goes on from the frame before has its next bytes first; anything else starts with bits.
  bool in_raw = block_remaining != 0 && block_type == kUncompressed;
  uint64_t raw_at = 0;
  if (!in_raw) {
    // The pad byte of an uncompressed block that ended with the last frame is this frame's first byte, unless that frame's data had it at its end.
    in.start(input, pad_pending ? 1 : 0);
    pad_pending = false;
    if (!header_read) {
      // Once per stream: whether calls are translated, and the size to translate them with.
      if (in.read(1) != 0) {
        const uint32_t high = in.read(16);
        translation_size = (high << 16) | in.read(16);
      }
      header_read = true;
    }
  }
  while (todo != 0) {
    if (block_remaining == 0) {
      if (in_raw) {
        // After the raw bytes (and the pad byte of an odd count) the bit stream starts again.
        in.start(input, raw_at + (pad_pending ? 1 : 0));
        pad_pending = false;
        in_raw = false;
      }
      if (!read_block_header(in, input, &raw_at, &in_raw)) return false;
    }
    const uint32_t run = block_remaining < todo ? block_remaining : todo;
    if (block_type == kUncompressed) {
      ConstByteView bytes;
      if (!input.try_subview(raw_at, run, &bytes)) return false;
      window_bytes.subview(position, run).copy_from(bytes);
      raw_at += run;
      position += run;
    } else if (!decode_symbols(in, window_bytes, frame_start, &position, run)) {
      return false;
    }
    block_remaining -= run;
    todo -= run;
  }
  if (in_raw) {
    if (block_remaining == 0 && pad_pending && raw_at < input.size()) pad_pending = false;      // the pad byte is this frame's last
    return true;
  }
  return !in.overrun();
}

// Undoes what the compressor did to the operand of every E8 (the x86 CALL with a relative target) so that calls of one function look alike: it had
// made it absolute. The last ten bytes of a frame are left alone, and so are the four bytes after an E8.
void LzxDecoder::State::translate_calls(ByteView frame) const {
  const int64_t size = static_cast<int32_t>(translation_size);
  for (uint64_t i = 0; i + 10 < frame.size();) {
    if (frame[i] != 0xE8) {
      ++i;
      continue;
    }
    const int64_t here = static_cast<int64_t>(produced + i);
    const ByteView operand = frame.subview(i + 1, 4);
    const int64_t absolute = static_cast<int32_t>(uint32_t{operand[0]} | (uint32_t{operand[1]} << 8) | (uint32_t{operand[2]} << 16) | (uint32_t{operand[3]} << 24));
    if (absolute >= -here && absolute < size) {
      const uint32_t relative = static_cast<uint32_t>(absolute >= 0 ? absolute - here : absolute + size);
      for (uint32_t k = 0; k < 4; ++k) operand[k] = static_cast<uint8_t>(relative >> (8 * k));
    }
    i += 5;
  }
}

bool LzxDecoder::reset(uint32_t window_bits) {
  if (window_bits < kMinWindowBits || window_bits > kMaxWindowBits) {
    state_.reset();
    return false;
  }
  if (!state_) state_ = UniquePointer<State>::make(allocator_, allocator_);
  if (!state_) return false;
  if (!state_->start(window_bits)) {
    state_.reset();
    return false;
  }
  return true;
}

bool LzxDecoder::decode_block(ConstByteView in, uint32_t out_size, ByteBuffer* out) {
  if (!state_) return false;
  State& s = *state_;
  if (s.failed || s.ended || out_size == 0 || out_size > kFrameSize) return false;
  s.failed = true;      // (until the frame is out)
  // Frames before the last are all 32768 bytes and the window is a multiple of that: a frame always fits before the window's end.
  if (!s.decode_frame(in, out_size)) return false;
  const uint64_t start = out->size();
  if (!out->append(s.window.view().subview(s.window_position, out_size))) return false;
  if (s.translation_size != 0 && s.produced < kTranslationLimit) s.translate_calls(out->writable_view().subview(start));
  s.produced += out_size;
  s.window_position = (s.window_position + out_size) & (s.window_size - 1);
  s.ended = out_size != kFrameSize;
  s.failed = false;
  return true;
}

}  // namespace media
