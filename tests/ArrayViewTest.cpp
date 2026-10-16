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

// Host-side tests for Slice, ArrayView and ByteReader/ByteWriter.
#include <stdio.h>
#include <stdlib.h>
#include "Utility/ArrayView.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/Slice.hpp"

struct Violation {};
void bounds_violation(const char*, uint64_t, uint64_t) { throw Violation{}; }  // this test checks that violations happen

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define STOPS(expr) do { bool stopped = false; try { (void)(expr); } catch (Violation&) { stopped = true; } \
  if (!stopped) { printf("FAILED line %d: no violation: %s\n", __LINE__, #expr); ++g_failed; } } while (0)

int main() {
  // ---- Slice
  CHECK(Slice(2, 3).end() == 5);
  CHECK(Slice(2, 3).contains(2) && Slice(2, 3).contains(4) && !Slice(2, 3).contains(5) && !Slice(2, 3).contains(1));
  CHECK(Slice(0, 5).fits_in(5) && !Slice(1, 5).fits_in(5) && Slice(5, 0).fits_in(5) && !Slice(6, 0).fits_in(5));
  CHECK(!Slice(~0ull, 2).fits_in(~0ull) && !Slice(~0ull, 2).valid());  // no overflow tricks
  CHECK(Slice(4, ~0ull).clamped_to(10) == Slice(4, 6) && Slice(12, 3).clamped_to(10).empty());
  Slice sub;
  CHECK(Slice(10, 5).try_sub(2, 3, &sub) && sub == Slice(12, 3));
  CHECK(!Slice(10, 5).try_sub(3, 3, &sub) && !Slice(10, 5).try_sub(6, 0, &sub));
  CHECK(Slice(10, 5).try_sub(5, 0, &sub));  // an empty slice at the very end is fine
  STOPS(Slice(10, 5).sub(4, 2));
  CHECK(Slice(0, 4).overlaps(Slice(3, 4)) && !Slice(0, 4).overlaps(Slice(4, 4)) && !Slice(0, 0).overlaps(Slice(0, 4)));
  CHECK(Slice::between(3, 7) == Slice(3, 4) && Slice::between(7, 3).empty());

  // ---- ArrayView
  int values[6] = {10, 11, 12, 13, 14, 15};
  ArrayView<int> view(values);
  CHECK(view.size() == 6 && view[0] == 10 && view[5] == 15 && view.size_bytes() == 6 * sizeof(int));
  STOPS(view[6]);
  STOPS(view[~0ull]);
  int sum = 0;
  for (int v : view) sum += v;
  CHECK(sum == 75);
  int out = 0;
  CHECK(view.try_get(5, &out) && out == 15 && !view.try_get(6, &out) && view.try_at(6) == nullptr);
  CHECK(view.subview(2, 3)[0] == 12 && view.subview(2, 3).size() == 3);
  CHECK(view.subview(6).empty() && view.subview(Slice(6, 0)).empty());
  STOPS(view.subview(7));
  STOPS(view.subview(4, 3));
  STOPS(view.subview(Slice(~0ull, 2)));
  CHECK(view[Slice(1, 2)].size() == 2 && view.first(2)[1] == 11 && view.last(2)[0] == 14 && view.dropped(4).size() == 2);
  STOPS(view.first(7));
  STOPS(view.last(7));
  ArrayView<int> part;
  CHECK(view.try_subview(1, 5, &part) && part.size() == 5);
  CHECK(!view.try_subview(1, 6, &part) && !view.try_subview(Slice(~0ull, 1), &part));
  CHECK(view.at_most(100).size() == 6 && view.at_most(2).size() == 2);
  ArrayView<int> empty;
  CHECK(empty.empty() && empty.size() == 0 && empty.begin() == empty.end());
  STOPS(empty.back());
  STOPS(empty[0]);
  ArrayView<int> null_with_size(nullptr, 5);
  CHECK(null_with_size.empty());  // a null pointer never claims elements

  ArrayView<const int> read_only = view;  // implicit const conversion
  CHECK(read_only[3] == 13);

  int copy[4] = {};
  ArrayView<int> target(copy);
  CHECK(target.copy_from(view.first(4)) && copy[3] == 13);
  CHECK(!target.copy_from(view.first(5)) && copy[3] == 13);  // too big: nothing copied
  ArrayView<int> overlapping(values);
  CHECK(overlapping.copy_from(overlapping.subview(1, 5)) && values[0] == 11 && values[4] == 15);  // memmove semantics
  target.fill(7);
  CHECK(copy[0] == 7 && copy[3] == 7);
  CHECK(ArrayView<const int>(copy).equals(ArrayView<const int>(copy)) && !target.equals(view.first(3)));

  // ---- cast
  alignas(8) uint8_t raw[16] = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0};
  ByteView bytes(raw);
  ArrayView<uint32_t> words = bytes.cast<uint32_t>();
  CHECK(words.size() == 4 && words[2] == 3);
  STOPS(bytes.first(15).cast<uint32_t>());      // not a whole number of words
  STOPS(bytes.subview(1, 8).cast<uint32_t>());  // misaligned
  struct Header { uint16_t a, b; };
  Header header{};
  CHECK(bytes_of(header).size() == 4);
  CHECK(bytes_of(header).copy_from(bytes.first(4)) && header.a == 1);

  // ---- ByteReader
  const uint8_t packet[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 7};
  ByteReader in{ConstByteView(packet)};
  CHECK(in.u8() == 0x12 && in.be16() == 0x3456 && in.be32() == 0x789ABCDE && in.ok() && in.remaining() == 2);
  CHECK(in.be32() == 0 && !in.ok());           // too short: zero and a sticky error
  CHECK(in.u8() == 0 && !in.ok() && in.take(1).empty());  // stays failed
  ByteReader take{ConstByteView(packet)};
  ConstByteView four = take.take(4);
  CHECK(four.size() == 4 && four[0] == 0x12 && take.ok() && take.position() == 4);
  CHECK(take.take(6).empty() && !take.ok());
  ByteReader skip{ConstByteView(packet)};
  CHECK(skip.skip(9) && skip.at_end() && !skip.skip(1) && !skip.ok());
  ByteReader seek{ConstByteView(packet)};
  CHECK(seek.seek(9) && !seek.seek(10) && !seek.ok());
  ByteReader le{ConstByteView(packet)};
  CHECK(le.le16() == 0x3412 && le.le32() == 0xBC9A7856u && le.ok());
  uint8_t peeked;
  ByteReader peek{ConstByteView(packet)};
  CHECK(peek.peek_u8(8, &peeked) && peeked == 7 && !peek.peek_u8(9, &peeked) && peek.ok());
  uint8_t into[3];
  ByteReader copyout{ConstByteView(packet)};
  CHECK(copyout.read_into(ByteView(into)) && into[2] == 0x56 && !copyout.read_into(ByteView(into)) == false);
  ByteReader huge{ConstByteView(packet)};
  CHECK(huge.take(~0ull).empty() && !huge.ok());  // a hostile length field

  // ---- ByteWriter
  uint8_t buffer[8] = {};
  ByteWriter w{ByteView(buffer)};
  w.u8(1);
  w.be16(0x0203);
  w.be32(0x04050607);
  CHECK(w.ok() && w.position() == 7 && buffer[3] == 4 && buffer[6] == 7 && w.space() == 1);
  w.patch_be16(0, 0xAABB);
  CHECK(w.ok() && buffer[0] == 0xAA && buffer[1] == 0xBB);
  w.be16(0);  // one byte left: refused, nothing written
  CHECK(!w.ok() && w.position() == 7 && buffer[7] == 0);
  uint8_t second[4] = {};
  ByteWriter w2{ByteView(second)};
  w2.bytes(ConstByteView(packet).first(4));
  CHECK(w2.ok() && second[3] == 0x78 && w2.written().size() == 4);
  w2.zeros(1);
  CHECK(!w2.ok());
  ByteWriter w3{ByteView(second)};
  w3.u8(9);
  w3.patch_be16(0, 1);  // there is only one byte to patch
  CHECK(!w3.ok());
  ByteWriter w4{ByteView(second)};
  ByteView reserved = w4.reserve(3);
  CHECK(reserved.size() == 3 && w4.position() == 3 && w4.reserve(2).empty() && !w4.ok());

  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("array view ok\n");
  return 0;
}
