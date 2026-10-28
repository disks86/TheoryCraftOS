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

// Host-side tests for the text decoders of the Encoding Standard (src/Dom/LegacyEncodings.cpp): labels, known sequences, decoding in pieces, truncation and bit flips.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "Dom/LegacyEncodings.hpp"

void bounds_violation(const char*, uint64_t, uint64_t) { abort(); }

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { free(p); }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("   FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

using dom::Encoding;

static std::string decode(TestAllocator& a, Encoding e, const std::vector<uint8_t>& bytes, bool fatal = false, bool* ok = nullptr) {
  dom::DecoderState state;
  String out(a);
  const bool good = dom::decode_legacy(e, &state, ConstByteView(bytes.data(), bytes.size()), true, fatal, &out);
  if (ok) *ok = good;
  return std::string(out.view().data(), out.view().size());
}

// The same input given in two pieces (the first one not the last).
static std::string decode_split(TestAllocator& a, Encoding e, const std::vector<uint8_t>& bytes, size_t split) {
  dom::DecoderState state;
  String out(a);
  dom::decode_legacy(e, &state, ConstByteView(bytes.data(), split), false, false, &out);
  dom::decode_legacy(e, &state, ConstByteView(bytes.data() + split, bytes.size() - split), true, false, &out);
  return std::string(out.view().data(), out.view().size());
}

static bool encoding_of(const char* label, Encoding* e) { return dom::find_encoding(StringView(label), e); }

int main() {
  TestAllocator a;
  Encoding e;
  // labels: case, white space, names
  CHECK(encoding_of("UTF-8", &e) && e == Encoding::Utf8);
  CHECK(encoding_of(" \t\nLatin1\f\r", &e) && e == Encoding::Windows1252);
  CHECK(encoding_of("shift_jis", &e) && e == Encoding::ShiftJis && dom::encoding_name(e).equals("shift_jis") && dom::encoding_display_name(e).equals("Shift_JIS"));
  CHECK(encoding_of("big5-hkscs", &e) && e == Encoding::Big5);
  CHECK(encoding_of("iso-8859-8-i", &e) && e == Encoding::Iso8859_8I);
  CHECK(encoding_of("csiso2022kr", &e) && e == Encoding::Replacement);
  CHECK(!encoding_of("", &e) && !encoding_of("utf-32", &e) && !encoding_of("koi8-", &e) && !encoding_of("utf-8x", &e));

  // known sequences
  CHECK(decode(a, Encoding::Gbk, {0xC4, 0xE3, 0xBA, 0xC3}) == "\xE4\xBD\xA0\xE5\xA5\xBD");                          // 你好
  CHECK(decode(a, Encoding::Big5, {0xA7, 0x41, 0xA6, 0x6E}) == "\xE4\xBD\xA0\xE5\xA5\xBD");
  CHECK(decode(a, Encoding::ShiftJis, {0x82, 0xB1, 0x82, 0xF1, 0xB1}) == "\xE3\x81\x93\xE3\x82\x93\xEF\xBD\xB1");   // こんｱ
  CHECK(decode(a, Encoding::EucJp, {0xA4, 0xB3, 0x8E, 0xB1, 0x8F, 0xB0, 0xA1}) == "\xE3\x81\x93\xEF\xBD\xB1\xE4\xB8\x82");
  CHECK(decode(a, Encoding::EucKr, {0xB0, 0xA1}) == "\xEA\xB0\x80");
  CHECK(decode(a, Encoding::Iso2022Jp, {0x1B, 0x24, 0x42, 0x24, 0x33, 0x1B, 0x28, 0x42, 0x41}) == "\xE3\x81\x93" "A");
  CHECK(decode(a, Encoding::Windows1252, {0x80, 0x41, 0xE9}) == "\xE2\x82\xAC" "A\xC3\xA9");
  CHECK(decode(a, Encoding::Koi8R, {0xC1}) == "\xD0\xB0");
  CHECK(decode(a, Encoding::Gb18030, {0x81, 0x30, 0x81, 0x30}) == "\xC2\x80");
  CHECK(decode(a, Encoding::Gb18030, {0x90, 0x30, 0x81, 0x30}) == "\xF0\x90\x80\x80");   // U+10000
  CHECK(decode(a, Encoding::Utf8, {0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80}) == "\xE2\x82\xAC\xF0\x9F\x98\x80");
  CHECK(decode(a, Encoding::Utf8, {0xF0, 0x80, 0x80}) == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");               // three errors, not one
  CHECK(decode(a, Encoding::Utf16Le, {0x3D, 0xD8, 0x00, 0xDE}) == "\xF0\x9F\x98\x80");
  CHECK(decode(a, Encoding::Utf16Be, {0xD8, 0x3D, 0x41}) == "\xEF\xBF\xBD");   // a lone lead surrogate and an odd byte: one error at the end
  CHECK(decode(a, Encoding::XUserDefined, {0x41, 0x80, 0xFF}) == "A\xEF\x9E\x80\xEF\x9F\xBF");
  CHECK(decode(a, Encoding::Utf8, {0x00}) == std::string(1, '\0'));
  bool ok = true;
  decode(a, Encoding::Gbk, {0x81}, true, &ok);
  CHECK(!ok);
  decode(a, Encoding::Utf8, {0xFF}, true, &ok);
  CHECK(!ok);
  decode(a, Encoding::Utf8, {0x41, 0x42}, true, &ok);
  CHECK(ok);

  // decoding in pieces gives what decoding at once gives; truncation and bit flips never go wrong (the sanitizers watch)
  const std::vector<std::vector<uint8_t>> seeds = {
      {0x41, 0xC4, 0xE3, 0xBA, 0xC3, 0x81, 0x30, 0x81, 0x30, 0x42}, {0xA7, 0x41, 0xA6, 0x6E, 0x88, 0x62, 0x88, 0x64}, {0x82, 0xB1, 0xB1, 0x81, 0x40, 0xEB, 0xFC, 0x43},
      {0xA4, 0xB3, 0x8E, 0xB1, 0x8F, 0xB0, 0xA1, 0x8F, 0xA2, 0xAF, 0x20}, {0xB0, 0xA1, 0xC8, 0xFE, 0x30},
      {0x1B, 0x24, 0x42, 0x24, 0x33, 0x1B, 0x28, 0x4A, 0x5C, 0x1B, 0x28, 0x49, 0x31, 0x1B, 0x28, 0x42}, {0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0xC3, 0xA9},
      {0x3D, 0xD8, 0x00, 0xDE, 0x41, 0x00}};
  for (uint32_t k = 0; k < static_cast<uint32_t>(Encoding::XUserDefined) + 1; ++k) {
    const Encoding enc = static_cast<Encoding>(k);
    if (enc == Encoding::Replacement) continue;
    for (const std::vector<uint8_t>& seed : seeds) {
      const std::string whole = decode(a, enc, seed);
      for (size_t split = 0; split <= seed.size(); ++split) CHECK(decode_split(a, enc, seed, split) == whole);
      for (size_t keep = 0; keep <= seed.size(); ++keep) decode(a, enc, std::vector<uint8_t>(seed.begin(), seed.begin() + keep));
      for (size_t byte = 0; byte < seed.size(); ++byte)
        for (int bit = 0; bit < 8; ++bit) {
          std::vector<uint8_t> flipped = seed;
          flipped[byte] = static_cast<uint8_t>(flipped[byte] ^ (1u << bit));
          decode(a, enc, flipped);
          decode(a, enc, flipped, true);
        }
    }
  }

  // documents: a byte order mark, the charset of the header, a <meta>, nothing
  auto document = [&](const char* html, const char* type) {
    String text(a);
    Encoding used;
    std::string raw = html;
    dom::decode_document(ConstByteView(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()), StringView(type), &text, &used);
    return std::string(text.view().data(), text.view().size()) + "|" + std::string(dom::encoding_name(used).data(), dom::encoding_name(used).size());
  };
  CHECK(document("\xEF\xBB\xBF<p>x\xC3\xA9", "") == "<p>x\xC3\xA9|utf-8");
  CHECK(document("<p>x\xE9", "text/html; charset=windows-1252") == "<p>x\xC3\xA9|windows-1252");
  CHECK(document("<p>x\xE9", "text/html; charset=\"latin1\"") == "<p>x\xC3\xA9|windows-1252");
  CHECK(document("<meta charset=koi8-r><p>\xC1", "text/html") == "<meta charset=koi8-r><p>\xD0\xB0|koi8-r");
  CHECK(document("<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=shift_jis\">\x82\xB1", "") == "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=shift_jis\">\xE3\x81\x93|shift_jis");
  CHECK(document("<meta charset=utf-16le><p>a", "") == "<meta charset=utf-16le><p>a|utf-8");
  CHECK(document("<p>\xC3\xA9", "") == "<p>\xC3\xA9|utf-8");
  CHECK(document("<p>\xE9", "text/html; charset=bogus") == "<p>\xEF\xBF\xBD|utf-8");
  CHECK(document("<meta charset=big5><p>\xA7\x41", "text/html; charset=utf-8") == "<meta charset=big5><p>\xEF\xBF\xBD" "A|utf-8");   // (the header wins)

  printf(g_failed ? "%d check(s) failed\n" : "encoding tests passed\n", g_failed);
  return g_failed ? 1 : 0;
}
