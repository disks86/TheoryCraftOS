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

// The legacy text encodings of the Encoding Standard (https://encoding.spec.whatwg.org): the labels that name them and their decoders (Chinese, Japanese, Korean
// and the single byte ones). The decoders are the algorithms of the standard, written to run in pieces: a decoder keeps what it has not finished (a lead byte)
// in a State that the caller holds between two calls (UTF-8 and UTF-16 as well).
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace dom {

// In the order of encodings.json (the tables in LegacyEncodingTables.inc follow it).
enum class Encoding : uint8_t {
  Utf8, Ibm866, Iso8859_2, Iso8859_3, Iso8859_4, Iso8859_5, Iso8859_6, Iso8859_7, Iso8859_8, Iso8859_8I, Iso8859_10, Iso8859_13, Iso8859_14, Iso8859_15, Iso8859_16,
  Koi8R, Koi8U, Macintosh, Windows874, Windows1250, Windows1251, Windows1252, Windows1253, Windows1254, Windows1255, Windows1256, Windows1257, Windows1258,
  XMacCyrillic, Gbk, Gb18030, Big5, EucJp, Iso2022Jp, ShiftJis, EucKr, Replacement, Utf16Be, Utf16Le, XUserDefined
};

// The encoding a label names (the label is trimmed of ASCII white space and compared without regard to case). False when there is none.
bool find_encoding(StringView label, Encoding* out);
// "utf-8", "gbk", "shift_jis" ...: the name that TextDecoder.encoding gives.
StringView encoding_name(Encoding e);

// The name document.characterSet gives ("UTF-8", "Big5", "windows-1252" ...).
StringView encoding_display_name(Encoding e);

// What a decoder has read of a character that is not complete: a few bytes, and for ISO-2022-JP the mode it is in.
struct DecoderState {
  uint32_t v[6] = {0, 0, 0, 0, 0, 0};
  // After an error that stopped a `fatal` decoding: the bytes the decoder had put back to read again (the next call reads them first), v[7] of them.
  uint32_t back[4] = {0, 0, 0, 0};
  uint32_t backs = 0;
};

// Decodes `in` as `e` and appends UTF-8 to *out. A byte that is not part of a character gives U+FFFD, or, with `fatal`, makes the call return false
// (the state is reset). `last`: no more bytes follow, so a character that is not complete is an error. The state carries over to the next call when it is not.
bool decode_legacy(Encoding e, DecoderState* state, ConstByteView in, bool last, bool fatal, String* out);

// The text of a document that arrived as bytes: the encoding is that of a byte order mark, else the charset of the Content-Type header, else a <meta charset> (or
// http-equiv Content-Type) in the first 1024 bytes, else UTF-8. The text is UTF-8 without the byte order mark; *used is the encoding that was found.
void decode_document(ConstByteView bytes, StringView content_type, String* text, Encoding* used);

}  // namespace dom
