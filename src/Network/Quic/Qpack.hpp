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

// QPACK (RFC 9204), the header compression of HTTP/3, in the form this client uses it: it tells the server its dynamic table has no room (capacity 0, no
// blocked streams), so header blocks are made of the static table and literals, and the blocks it reads are decoded from the same. Huffman coding of
// strings is understood (HPACK's table, RFC 7541) and not used when writing.
#pragma once
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace network::h3 {

// Header fields, kept as one run of text with offsets, in the order they came.
class HeaderList {
 public:
  explicit HeaderList(allocators::IAllocator& allocator) : pool_(allocator), entries_(allocator) {}
  allocators::IAllocator& allocator() const { return pool_.allocator(); }
  HeaderList(const HeaderList&) = delete;
  HeaderList& operator=(const HeaderList&) = delete;

  bool add(StringView name, StringView value);
  uint64_t count() const { return entries_.count(); }
  StringView name(uint64_t i) const { return pool_.view().substr(entries_[i].name_at, entries_[i].name_length); }
  StringView value(uint64_t i) const { return pool_.view().substr(entries_[i].value_at, entries_[i].value_length); }
  // The value of the first field with that (lower case) name, or empty.
  StringView find(StringView name) const;
  void clear() {
    pool_.clear();
    entries_.clear();
  }
  uint64_t bytes() const { return pool_.size(); }

 private:
  struct Entry {
    uint32_t name_at, name_length, value_at, value_length;
  };
  String pool_;
  List<Entry> entries_;
};

// A field section (RFC 9204 4.5) for the fields, with Required Insert Count 0. False on memory shortage.
bool qpack_encode(const HeaderList& fields, ByteBuffer* out);

enum class QpackResult : uint8_t {
  Ok,
  Malformed,     // not valid QPACK (QPACK_DECOMPRESSION_FAILED)
  DynamicTable,  // it refers to the dynamic table, which this side keeps empty
  TooLarge,      // the fields add up to more than the limit
  OutOfMemory,
};
// Reads a field section. The fields are added to `fields`; at most `max_bytes` of names and values.
QpackResult qpack_decode(ConstByteView section, HeaderList* fields, uint64_t max_bytes);

// HPACK/QPACK Huffman coding of a string (RFC 7541 5.2). False if the data is not valid (an unknown code, EOS inside, padding that is not all ones).
bool huffman_decode(ConstByteView in, String* out, uint64_t max_length);
// How many bytes the string takes Huffman coded, and the coding appended to `out`.
uint64_t huffman_encoded_size(StringView text);
bool huffman_encode(StringView text, ByteBuffer* out);

}  // namespace network::h3
