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

#include "Dom/LegacyEncodings.hpp"
#include "Dom/Entities.hpp"

namespace dom {

namespace {

#include "Dom/LegacyEncodingTables.inc"

static_assert(kEncodingCount == static_cast<uint32_t>(Encoding::XUserDefined) + 1, "the tables and the enum list the encodings in the same order");

constexpr int kEnd = 256;   // the end of the input (only seen by a decoder once the input is known to be complete)

// The bytes to decode, with the bytes that a decoder put back in front of them ("prepend" in the standard).
struct Input {
  ConstByteView bytes;
  uint64_t at = 0;
  bool last = false;
  int back[6];
  int backs = 0;
  int next() {
    if (backs) return back[--backs];
    if (at < bytes.size()) return bytes[at++];
    return last ? kEnd : -1;
  }
  void put_back(int byte) { if (backs < 6) back[backs++] = byte; }   // (the one pushed last is read first)
};

struct Run {
  Input in;
  String* out;
  bool fatal;
  DecoderState* state;
  void emit(uint32_t cp) {
    if (cp == 0) out->push_back('\0');   // (append_utf8 turns a NUL into U+FFFD, as the HTML reader wants)
    else append_utf8(cp, out);
  }
  // A byte that is not part of a character: false when the decoding has to stop.
  bool error() {
    if (fatal) return false;
    emit(0xFFFD);
    return true;
  }
};

bool in_range(int b, int lo, int hi) { return b >= lo && b <= hi; }

bool run_single_byte(Run& r, Encoding e) {
  const int row = kSingleByteRowOf[static_cast<int>(e)];
  for (int b; (b = r.in.next()) >= 0 && b != kEnd;) {
    if (b < 0x80) { r.emit(static_cast<uint32_t>(b)); continue; }
    if (e == Encoding::XUserDefined) { r.emit(0xF780u + static_cast<uint32_t>(b) - 0x80); continue; }
    const uint16_t cp = kSingleByteRows[row][b - 0x80];
    if (cp == 0xFFFD) { if (!r.error()) return false; }
    else r.emit(cp);
  }
  return true;
}

// The code point of a pointer into the ranges of GB18030 (the four byte sequences); 0 for none.
uint32_t gb18030_range(uint32_t pointer) {
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) return 0;
  if (pointer == 7457) return 0xE7C7;
  uint32_t offset = 0, cp_offset = 0;
  for (const Gb18030Range& range : kGb18030Ranges) {
    if (range.pointer > pointer) break;
    offset = range.pointer;
    cp_offset = range.code_point;
  }
  return cp_offset + pointer - offset;
}

// uint32_t v[0..2]: the first, second and third byte of the sequence read so far.
bool run_gb18030(Run& r) {
  uint32_t& first = r.state->v[0];
  uint32_t& second = r.state->v[1];
  uint32_t& third = r.state->v[2];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (first || second || third) {
        first = second = third = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (third) {
      if (!in_range(b, 0x30, 0x39)) {
        r.in.put_back(b);
        r.in.put_back(static_cast<int>(third));
        r.in.put_back(static_cast<int>(second));
        first = second = third = 0;
        if (!r.error()) return false;
        continue;
      }
      const uint32_t pointer = (first - 0x81) * (10 * 126 * 10) + (second - 0x30) * (10 * 126) + (third - 0x81) * 10 + static_cast<uint32_t>(b) - 0x30;
      first = second = third = 0;
      const uint32_t cp = gb18030_range(pointer);
      if (!cp) { if (!r.error()) return false; }
      else r.emit(cp);
      continue;
    }
    if (second) {
      if (in_range(b, 0x81, 0xFE)) { third = static_cast<uint32_t>(b); continue; }
      r.in.put_back(b);
      r.in.put_back(static_cast<int>(second));
      first = second = 0;
      if (!r.error()) return false;
      continue;
    }
    if (first) {
      if (in_range(b, 0x30, 0x39)) { second = static_cast<uint32_t>(b); continue; }
      const uint32_t lead = first;
      first = 0;
      uint32_t cp = 0;
      if (in_range(b, 0x40, 0x7E) || in_range(b, 0x80, 0xFE)) {
        const uint32_t offset = b < 0x7F ? 0x40 : 0x41;
        const uint32_t pointer = (lead - 0x81) * 190 + static_cast<uint32_t>(b) - offset;
        if (pointer < sizeof(kIndexGb18030) / sizeof(kIndexGb18030[0])) cp = kIndexGb18030[pointer];
      }
      if (!cp && b < 0x80) r.in.put_back(b);
      if (!cp) { if (!r.error()) return false; }
      else r.emit(cp);
      continue;
    }
    if (b < 0x80) r.emit(static_cast<uint32_t>(b));
    else if (b == 0x80) r.emit(0x20AC);
    else if (in_range(b, 0x81, 0xFE)) first = static_cast<uint32_t>(b);
    else if (!r.error()) return false;
  }
  return true;
}

bool run_big5(Run& r) {
  uint32_t& lead = r.state->v[0];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (lead) {
        lead = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (lead) {
      const uint32_t l = lead;
      lead = 0;
      uint32_t pointer = 0xFFFFFFFFu;
      const uint32_t offset = b < 0x7F ? 0x40 : 0x62;
      if (in_range(b, 0x40, 0x7E) || in_range(b, 0xA1, 0xFE)) pointer = (l - 0x81) * 157 + static_cast<uint32_t>(b) - offset;
      if (pointer == 1133) { r.emit(0xCA); r.emit(0x304); continue; }
      if (pointer == 1135) { r.emit(0xCA); r.emit(0x30C); continue; }
      if (pointer == 1164) { r.emit(0xEA); r.emit(0x304); continue; }
      if (pointer == 1166) { r.emit(0xEA); r.emit(0x30C); continue; }
      uint32_t cp = 0;
      if (pointer < sizeof(kIndexBig5) / sizeof(kIndexBig5[0])) cp = kIndexBig5[pointer];
      if (!cp && b < 0x80) r.in.put_back(b);
      if (!cp) { if (!r.error()) return false; }
      else r.emit(cp);
      continue;
    }
    if (b < 0x80) r.emit(static_cast<uint32_t>(b));
    else if (in_range(b, 0x81, 0xFE)) lead = static_cast<uint32_t>(b);
    else if (!r.error()) return false;
  }
  return true;
}

bool run_euc_kr(Run& r) {
  uint32_t& lead = r.state->v[0];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (lead) {
        lead = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (lead) {
      const uint32_t l = lead;
      lead = 0;
      uint32_t cp = 0;
      if (in_range(b, 0x41, 0xFE)) {
        const uint32_t pointer = (l - 0x81) * 190 + static_cast<uint32_t>(b) - 0x41;
        if (pointer < sizeof(kIndexEucKr) / sizeof(kIndexEucKr[0])) cp = kIndexEucKr[pointer];
      }
      if (!cp && b < 0x80) r.in.put_back(b);
      if (!cp) { if (!r.error()) return false; }
      else r.emit(cp);
      continue;
    }
    if (b < 0x80) r.emit(static_cast<uint32_t>(b));
    else if (in_range(b, 0x81, 0xFE)) lead = static_cast<uint32_t>(b);
    else if (!r.error()) return false;
  }
  return true;
}

bool run_shift_jis(Run& r) {
  uint32_t& lead = r.state->v[0];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (lead) {
        lead = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (lead) {
      const uint32_t l = lead;
      lead = 0;
      uint32_t pointer = 0xFFFFFFFFu;
      const uint32_t offset = b < 0x7F ? 0x40 : 0x41;
      const uint32_t lead_offset = l < 0xA0 ? 0x81 : 0xC1;
      if (in_range(b, 0x40, 0x7E) || in_range(b, 0x80, 0xFC)) pointer = (l - lead_offset) * 188 + static_cast<uint32_t>(b) - offset;
      if (pointer >= 8836 && pointer <= 10715) { r.emit(0xE000 - 8836 + pointer); continue; }
      uint32_t cp = 0;
      if (pointer < sizeof(kIndexJis0208) / sizeof(kIndexJis0208[0])) cp = kIndexJis0208[pointer];
      if (!cp && b < 0x80) r.in.put_back(b);
      if (!cp) { if (!r.error()) return false; }
      else r.emit(cp);
      continue;
    }
    if (b <= 0x80) r.emit(static_cast<uint32_t>(b));
    else if (in_range(b, 0xA1, 0xDF)) r.emit(0xFF61u - 0xA1 + static_cast<uint32_t>(b));
    else if (in_range(b, 0x81, 0x9F) || in_range(b, 0xE0, 0xFC)) lead = static_cast<uint32_t>(b);
    else if (!r.error()) return false;
  }
  return true;
}

// v[0]: the lead byte; v[1]: 1 when a sequence began with 0x8F (the JIS X 0212 table is used for the next two bytes).
bool run_euc_jp(Run& r) {
  uint32_t& lead = r.state->v[0];
  uint32_t& jis0212 = r.state->v[1];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (lead) {
        lead = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (lead == 0x8E && in_range(b, 0xA1, 0xDF)) {
      lead = 0;
      r.emit(0xFF61u - 0xA1 + static_cast<uint32_t>(b));
      continue;
    }
    if (lead == 0x8F && in_range(b, 0xA1, 0xFE)) {
      jis0212 = 1;
      lead = static_cast<uint32_t>(b);
      continue;
    }
    if (lead) {
      const uint32_t l = lead;
      lead = 0;
      uint32_t cp = 0;
      if (in_range(static_cast<int>(l), 0xA1, 0xFE) && in_range(b, 0xA1, 0xFE)) {
        const uint32_t pointer = (l - 0xA1) * 94 + static_cast<uint32_t>(b) - 0xA1;
        if (jis0212) { if (pointer < sizeof(kIndexJis0212) / sizeof(kIndexJis0212[0])) cp = kIndexJis0212[pointer]; }
        else if (pointer < sizeof(kIndexJis0208) / sizeof(kIndexJis0208[0])) cp = kIndexJis0208[pointer];
      }
      jis0212 = 0;
      if (!cp && b < 0x80) r.in.put_back(b);
      if (!cp) { if (!r.error()) return false; }
      else r.emit(cp);
      continue;
    }
    if (b < 0x80) r.emit(static_cast<uint32_t>(b));
    else if (b == 0x8E || b == 0x8F || in_range(b, 0xA1, 0xFE)) lead = static_cast<uint32_t>(b);
    else if (!r.error()) return false;
  }
  return true;
}

// ISO-2022-JP. v[0]: the decoder state, v[1]: the output state, v[2]: the lead byte, v[3]: the output flag.
enum IsoState : uint32_t { kAscii, kRoman, kKatakana, kLeadByte, kTrailByte, kEscapeStart, kEscape };

bool run_iso_2022_jp(Run& r) {
  uint32_t& state = r.state->v[0];
  uint32_t& output_state = r.state->v[1];
  uint32_t& lead = r.state->v[2];
  uint32_t& flag = r.state->v[3];
  for (int b; (b = r.in.next()) >= 0;) {
    switch (state) {
      case kAscii:
        if (b == 0x1B) state = kEscapeStart;
        else if (b <= 0x7F && b != 0x0E && b != 0x0F && b != kEnd) { flag = 0; r.emit(static_cast<uint32_t>(b)); }
        else if (b == kEnd) return true;
        else { flag = 0; if (!r.error()) return false; }
        break;
      case kRoman:
        if (b == 0x1B) state = kEscapeStart;
        else if (b == 0x5C) { flag = 0; r.emit(0xA5); }
        else if (b == 0x7E) { flag = 0; r.emit(0x203E); }
        else if (b <= 0x7F && b != 0x0E && b != 0x0F && b != kEnd) { flag = 0; r.emit(static_cast<uint32_t>(b)); }
        else if (b == kEnd) return true;
        else { flag = 0; if (!r.error()) return false; }
        break;
      case kKatakana:
        if (b == 0x1B) state = kEscapeStart;
        else if (in_range(b, 0x21, 0x5F)) { flag = 0; r.emit(0xFF61u - 0x21 + static_cast<uint32_t>(b)); }
        else if (b == kEnd) return true;
        else { flag = 0; if (!r.error()) return false; }
        break;
      case kLeadByte:
        if (b == 0x1B) state = kEscapeStart;
        else if (in_range(b, 0x21, 0x7E)) { flag = 0; lead = static_cast<uint32_t>(b); state = kTrailByte; }
        else if (b == kEnd) return true;
        else { flag = 0; if (!r.error()) return false; }
        break;
      case kTrailByte:
        if (b == 0x1B) {
          state = kEscapeStart;
          if (!r.error()) return false;
        } else if (in_range(b, 0x21, 0x7E)) {
          state = kLeadByte;
          const uint32_t pointer = (lead - 0x21) * 94 + static_cast<uint32_t>(b) - 0x21;
          const uint32_t cp = pointer < sizeof(kIndexJis0208) / sizeof(kIndexJis0208[0]) ? kIndexJis0208[pointer] : 0;
          if (!cp) { if (!r.error()) return false; }
          else r.emit(cp);
        } else if (b == kEnd) {
          state = kLeadByte;
          r.in.put_back(kEnd);
          if (!r.error()) return false;
        } else {
          state = kLeadByte;
          if (!r.error()) return false;
        }
        break;
      case kEscapeStart:
        if (b == 0x24 || b == 0x28) { lead = static_cast<uint32_t>(b); state = kEscape; break; }
        r.in.put_back(b);
        flag = 0;
        state = output_state;
        if (!r.error()) return false;
        break;
      case kEscape: {
        const uint32_t l = lead;
        lead = 0;
        int next = -1;
        if (l == 0x28 && b == 0x42) next = kAscii;
        else if (l == 0x28 && b == 0x4A) next = kRoman;
        else if (l == 0x28 && b == 0x49) next = kKatakana;
        else if (l == 0x24 && (b == 0x40 || b == 0x42)) next = kLeadByte;
        if (next >= 0) {
          state = output_state = static_cast<uint32_t>(next);
          const uint32_t was = flag;
          flag = 1;
          if (was && !r.error()) return false;
        } else {
          r.in.put_back(b);
          r.in.put_back(static_cast<int>(l));
          flag = 0;
          state = output_state;
          if (!r.error()) return false;
        }
        break;
      }
      default: state = kAscii; break;
    }
  }
  return true;
}

// UTF-8. v[0]: the code point so far, v[1]: the continuation bytes seen, v[2]: needed, v[3], v[4]: the range the next byte must be in (0: 0x80 and 0xBF).
bool run_utf8(Run& r) {
  uint32_t& cp = r.state->v[0];
  uint32_t& seen = r.state->v[1];
  uint32_t& needed = r.state->v[2];
  uint32_t& lower = r.state->v[3];
  uint32_t& upper = r.state->v[4];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (needed) {
        cp = seen = needed = lower = upper = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (!needed) {
      if (b <= 0x7F) r.emit(static_cast<uint32_t>(b));
      else if (in_range(b, 0xC2, 0xDF)) { needed = 1; cp = static_cast<uint32_t>(b) & 0x1F; }
      else if (in_range(b, 0xE0, 0xEF)) {
        if (b == 0xE0) lower = 0xA0;
        if (b == 0xED) upper = 0x9F;
        needed = 2;
        cp = static_cast<uint32_t>(b) & 0xF;
      } else if (in_range(b, 0xF0, 0xF4)) {
        if (b == 0xF0) lower = 0x90;
        if (b == 0xF4) upper = 0x8F;
        needed = 3;
        cp = static_cast<uint32_t>(b) & 0x7;
      } else if (!r.error()) return false;
      continue;
    }
    const uint32_t low = lower ? lower : 0x80, high = upper ? upper : 0xBF;
    if (static_cast<uint32_t>(b) < low || static_cast<uint32_t>(b) > high) {
      cp = seen = needed = lower = upper = 0;
      r.in.put_back(b);
      if (!r.error()) return false;
      continue;
    }
    lower = upper = 0;
    cp = (cp << 6) | (static_cast<uint32_t>(b) & 0x3F);
    if (++seen == needed) {
      const uint32_t done = cp;
      cp = seen = needed = 0;
      r.emit(done);
    }
  }
  return true;
}

// UTF-16. v[0]/v[1]: a lead byte and whether there is one, v[2]/v[3]: a lead surrogate and whether there is one.
bool run_utf16(Run& r, bool big_endian) {
  uint32_t& lead_byte = r.state->v[0];
  uint32_t& has_byte = r.state->v[1];
  uint32_t& lead_surrogate = r.state->v[2];
  uint32_t& has_surrogate = r.state->v[3];
  for (int b; (b = r.in.next()) >= 0;) {
    if (b == kEnd) {
      if (has_byte || has_surrogate) {
        lead_byte = has_byte = lead_surrogate = has_surrogate = 0;
        if (!r.error()) return false;
      }
      return true;
    }
    if (!has_byte) { lead_byte = static_cast<uint32_t>(b); has_byte = 1; continue; }
    const uint32_t first = lead_byte;
    has_byte = 0;
    const uint32_t unit = big_endian ? (first << 8) | static_cast<uint32_t>(b) : (static_cast<uint32_t>(b) << 8) | first;
    if (has_surrogate) {
      const uint32_t lead = lead_surrogate;
      has_surrogate = 0;
      if (unit >= 0xDC00 && unit <= 0xDFFF) { r.emit(0x10000 + ((lead - 0xD800) << 10) + (unit - 0xDC00)); continue; }
      r.in.put_back(b);   // (the unit is read again, after the error)
      r.in.put_back(static_cast<int>(first));
      if (!r.error()) return false;
      continue;
    }
    if (unit >= 0xD800 && unit <= 0xDBFF) { lead_surrogate = unit; has_surrogate = 1; }
    else if (unit >= 0xDC00 && unit <= 0xDFFF) { if (!r.error()) return false; }
    else r.emit(unit);
  }
  return true;
}

}  // namespace

bool find_encoding(StringView label, Encoding* out) {
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; };   // (ASCII white space)
  while (!label.empty() && space(label[0])) label = label.substr(1);
  while (!label.empty() && space(label[label.size() - 1])) label = label.substr(0, label.size() - 1);
  char key[48];
  if (label.empty() || label.size() >= sizeof(key)) return false;
  uint64_t n = 0;
  for (; n < label.size(); ++n) key[n] = label[n] >= 'A' && label[n] <= 'Z' ? static_cast<char>(label[n] + 32) : label[n];
  const StringView wanted(key, n);
  uint64_t lo = 0, hi = sizeof(kEncodingLabels) / sizeof(kEncodingLabels[0]);
  while (lo < hi) {   // (the labels are sorted)
    const uint64_t mid = lo + (hi - lo) / 2;
    const StringView have(kEncodingLabels[mid].label);
    if (have.equals(wanted)) {
      *out = static_cast<Encoding>(kEncodingLabels[mid].encoding);
      return true;
    }
    uint64_t k = 0;
    while (k < have.size() && k < wanted.size() && have[k] == wanted[k]) ++k;
    const bool less = k < wanted.size() && (k == have.size() || static_cast<uint8_t>(have[k]) < static_cast<uint8_t>(wanted[k]));   // (a prefix is less)
    if (less) lo = mid + 1;
    else hi = mid;
  }
  return false;
}

StringView encoding_name(Encoding e) { return StringView(kEncodingNames[static_cast<int>(e)]); }
StringView encoding_display_name(Encoding e) { return StringView(kEncodingDisplayNames[static_cast<int>(e)]); }

bool decode_legacy(Encoding e, DecoderState* state, ConstByteView in, bool last, bool fatal, String* out) {
  Run run{Input{in, 0, last, {0, 0, 0, 0, 0, 0}, 0}, out, fatal, state};
  for (uint32_t i = state->backs; i > 0; --i) run.in.put_back(static_cast<int>(state->back[i - 1]));   // (put back, the last first: they are read in the order they were left)
  state->backs = 0;
  bool ok = true;
  switch (e) {
    case Encoding::Utf8: ok = run_utf8(run); break;
    case Encoding::Utf16Be: ok = run_utf16(run, true); break;
    case Encoding::Utf16Le: ok = run_utf16(run, false); break;
    case Encoding::Gbk:
    case Encoding::Gb18030: ok = run_gb18030(run); break;
    case Encoding::Big5: ok = run_big5(run); break;
    case Encoding::EucJp: ok = run_euc_jp(run); break;
    case Encoding::Iso2022Jp: ok = run_iso_2022_jp(run); break;
    case Encoding::ShiftJis: ok = run_shift_jis(run); break;
    case Encoding::EucKr: ok = run_euc_kr(run); break;
    case Encoding::Replacement:   // one U+FFFD for the whole input
      if (in.size() && !state->v[0]) {
        state->v[0] = 1;
        ok = run.error();
      }
      break;
    default:
      if (kSingleByteRowOf[static_cast<int>(e)] >= 0 || e == Encoding::XUserDefined) ok = run_single_byte(run, e);
      break;
  }
  if (last) {
    *state = DecoderState();   // (the whole input was given: nothing is kept, also after an error)
  } else if (!ok) {
    // A decoding that stopped on an error keeps the state its algorithm left (most reset their lead bytes before they report the error, ISO-2022-JP does not), and the bytes
    // it had put back stay in the input for the next call.
    for (int i = run.in.backs - 1; i >= 0 && state->backs < 4; --i) state->back[state->backs++] = static_cast<uint32_t>(run.in.back[i]);
  }
  return ok;
}

namespace {

bool ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// The value of `charset=` inside text (a Content-Type value or the content of a <meta>): "utf-8" in "text/html; charset=\"utf-8\"".
bool charset_in(StringView text, StringView* value) {
  for (uint64_t i = 0; i + 7 <= text.size(); ++i) {
    bool same = true;
    for (uint64_t k = 0; k < 7 && same; ++k) same = lower(text[i + k]) == "charset"[k];
    if (!same) continue;
    uint64_t at = i + 7;
    while (at < text.size() && ascii_space(text[at])) ++at;
    if (at >= text.size() || text[at] != '=') continue;
    ++at;
    while (at < text.size() && ascii_space(text[at])) ++at;
    if (at >= text.size()) return false;
    const char quote = text[at] == '"' || text[at] == '\'' ? text[at] : 0;
    if (quote) ++at;
    uint64_t end = at;
    while (end < text.size() && (quote ? text[end] != quote : !(ascii_space(text[end]) || text[end] == ';' || text[end] == '"' || text[end] == '\'' || text[end] == '>'))) ++end;
    *value = text.substr(at, end - at);
    return !value->empty();
  }
  return false;
}

// The first meta tags of a page, looked at as bytes: a charset declaration.
bool meta_charset(ConstByteView bytes, Encoding* found) {
  const uint64_t limit = bytes.size() < 1024 ? bytes.size() : 1024;
  const StringView head(reinterpret_cast<const char*>(bytes.data()), limit);
  for (uint64_t i = 0; i + 5 < head.size(); ++i) {
    if (head[i] != '<') continue;
    bool meta = true;
    for (uint64_t k = 0; k < 4 && meta; ++k) meta = lower(head[i + 1 + k]) == "meta"[k];
    if (!meta || !(ascii_space(head[i + 5]) || head[i + 5] == '/')) continue;
    uint64_t end = i;
    char quote = 0;
    while (end < head.size() && (quote || head[end] != '>')) {   // (the end of the tag; a '>' inside quotes does not count)
      if (quote && head[end] == quote) quote = 0;
      else if (!quote && (head[end] == '"' || head[end] == '\'')) quote = head[end];
      ++end;
    }
    StringView value;
    if (charset_in(head.substr(i, end - i), &value) && find_encoding(value, found)) return true;
  }
  return false;
}

}  // namespace

void decode_document(ConstByteView bytes, StringView content_type, String* text, Encoding* used) {
  Encoding e = Encoding::Utf8;
  uint64_t skip = 0;
  StringView value;
  if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) skip = 3;
  else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) { e = Encoding::Utf16Be; skip = 2; }
  else if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) { e = Encoding::Utf16Le; skip = 2; }
  else if (charset_in(content_type, &value) && find_encoding(value, &e) && e != Encoding::Replacement) {}
  else if (meta_charset(bytes, &e)) {
    if (e == Encoding::Utf16Be || e == Encoding::Utf16Le || e == Encoding::Replacement) e = Encoding::Utf8;   // (a page that can be read as ASCII is not UTF-16)
  } else e = Encoding::Utf8;
  if (e == Encoding::Replacement) e = Encoding::Utf8;
  *used = e;
  DecoderState state;
  text->clear();
  decode_legacy(e, &state, ConstByteView(bytes.data() + skip, bytes.size() - skip), true, false, text);
}

namespace {

// The first pointer of `table` (of `count` entries) that holds `cp`, from `from`; -1 for none.
template <typename T>
int64_t first_pointer(const T* table, uint32_t count, uint32_t cp, uint32_t from = 0) {
  for (uint32_t i = from; i < count; ++i)
    if (table[i] == cp) return i;
  return -1;
}
template <typename T>
int64_t last_pointer(const T* table, uint32_t count, uint32_t cp) {
  for (uint32_t i = count; i > 0; --i)
    if (table[i - 1] == cp) return i - 1;
  return -1;
}
constexpr uint32_t count_of_big5 = sizeof(kIndexBig5) / sizeof(kIndexBig5[0]);
constexpr uint32_t count_of_euc_kr = sizeof(kIndexEucKr) / sizeof(kIndexEucKr[0]);
constexpr uint32_t count_of_gb18030 = sizeof(kIndexGb18030) / sizeof(kIndexGb18030[0]);
constexpr uint32_t count_of_jis0208 = sizeof(kIndexJis0208) / sizeof(kIndexJis0208[0]);

// The index jis0208 pointer for cp, not counting the pointers in [skip_from, skip_to] (Shift_JIS leaves out 8272 to 8835).
int64_t jis0208_pointer(uint32_t cp, uint32_t skip_from = 1, uint32_t skip_to = 0) {
  for (uint32_t i = 0; i < count_of_jis0208; ++i) {
    if (i >= skip_from && i <= skip_to) continue;
    if (kIndexJis0208[i] == cp) return i;
  }
  return -1;
}

void put(String* bytes, uint32_t b) { bytes->push_back(static_cast<char>(b)); }

}  // namespace

Encoding output_encoding(Encoding e) { return e == Encoding::Utf16Be || e == Encoding::Utf16Le || e == Encoding::Replacement ? Encoding::Utf8 : e; }

bool encode_code_point(Encoding e, EncoderState* state, uint32_t cp, String* bytes, uint32_t* unencodable) {
  *unencodable = cp;
  e = output_encoding(e);
  if (e == Encoding::Utf8) {
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    append_utf8(cp, bytes);
    return true;
  }
  if (e == Encoding::Iso2022Jp) {
    enum : uint32_t { kA, kRoman, kJis };
    for (;;) {   // (a code point that is "restored" is looked at again after the bytes that change the character set)
      if ((state->mode == kA || state->mode == kRoman) && (cp == 0x0E || cp == 0x0F || cp == 0x1B)) { *unencodable = 0xFFFD; return false; }
      if (state->mode == kA && cp <= 0x7F) { put(bytes, cp); return true; }
      if (state->mode == kRoman && ((cp <= 0x7F && cp != 0x5C && cp != 0x7E) || cp == 0xA5 || cp == 0x203E)) {
        put(bytes, cp == 0xA5 ? 0x5C : cp == 0x203E ? 0x7E : cp);
        return true;
      }
      if (cp <= 0x7F && state->mode != kA) { state->mode = kA; put(bytes, 0x1B); put(bytes, 0x28); put(bytes, 0x42); continue; }
      if ((cp == 0xA5 || cp == 0x203E) && state->mode != kRoman) { state->mode = kRoman; put(bytes, 0x1B); put(bytes, 0x28); put(bytes, 0x4A); continue; }
      if (cp == 0x2212) cp = 0xFF0D;
      uint32_t looked = cp;
      if (cp >= 0xFF61 && cp <= 0xFF9F) looked = kIndexIso2022JpKatakana[cp - 0xFF61];
      const int64_t pointer = jis0208_pointer(looked);
      if (pointer < 0) {
        if (state->mode == kJis) { state->mode = kA; put(bytes, 0x1B); put(bytes, 0x28); put(bytes, 0x42); continue; }
        *unencodable = cp;
        return false;
      }
      if (state->mode != kJis) { state->mode = kJis; put(bytes, 0x1B); put(bytes, 0x24); put(bytes, 0x42); continue; }
      put(bytes, static_cast<uint32_t>(pointer) / 94 + 0x21);
      put(bytes, static_cast<uint32_t>(pointer) % 94 + 0x21);
      return true;
    }
  }
  if (e == Encoding::Gbk || e == Encoding::Gb18030) {
    if (cp <= 0x7F) { put(bytes, cp); return true; }
    if (cp == 0xE5E5) return false;
    if (e == Encoding::Gbk && cp == 0x20AC) { put(bytes, 0x80); return true; }
    static const struct { uint16_t cp, bytes; } kCompat[] = {   // (what GB18030-2005 had: the decoder gives other characters for these bytes now)
        {0xE78D, 0xA6D9}, {0xE78E, 0xA6DA}, {0xE78F, 0xA6DB}, {0xE790, 0xA6DC}, {0xE791, 0xA6DD}, {0xE792, 0xA6DE}, {0xE793, 0xA6DF}, {0xE794, 0xA6EC}, {0xE795, 0xA6ED},
        {0xE796, 0xA6F3}, {0xE81E, 0xFE59}, {0xE826, 0xFE61}, {0xE82B, 0xFE66}, {0xE82C, 0xFE67}, {0xE832, 0xFE6D}, {0xE843, 0xFE7E}, {0xE854, 0xFE90}, {0xE864, 0xFEA0}};
    for (const auto& row : kCompat)
      if (row.cp == cp) { put(bytes, row.bytes >> 8); put(bytes, row.bytes & 0xFF); return true; }
    const int64_t pointer = cp > 0xFFFF ? -1 : first_pointer(kIndexGb18030, count_of_gb18030, cp);
    if (pointer >= 0) {
      const uint32_t trail = static_cast<uint32_t>(pointer) % 190;
      put(bytes, static_cast<uint32_t>(pointer) / 190 + 0x81);
      put(bytes, trail + (trail < 0x3F ? 0x40 : 0x41));
      return true;
    }
    if (e == Encoding::Gbk) return false;
    uint32_t range_pointer = 0;
    if (cp == 0xE7C7) range_pointer = 7457;
    else {
      uint32_t offset = 0, pointer_offset = 0;
      for (const Gb18030Range& range : kGb18030Ranges) {
        if (range.code_point > cp) break;
        offset = range.code_point;
        pointer_offset = range.pointer;
      }
      range_pointer = pointer_offset + cp - offset;
    }
    put(bytes, range_pointer / (10 * 126 * 10) + 0x81);
    put(bytes, range_pointer % (10 * 126 * 10) / (10 * 126) + 0x30);
    put(bytes, range_pointer % (10 * 126) / 10 + 0x81);
    put(bytes, range_pointer % 10 + 0x30);
    return true;
  }
  if (e == Encoding::Big5) {
    if (cp <= 0x7F) { put(bytes, cp); return true; }
    const uint32_t floor = (0xA1 - 0x81) * 157;   // (the pointers below are not used to encode)
    int64_t pointer = -1;
    if (cp == 0x2550 || cp == 0x255E || cp == 0x2561 || cp == 0x256A || cp == 0x5341 || cp == 0x5345) {
      pointer = last_pointer(kIndexBig5, count_of_big5, cp);
      if (pointer < static_cast<int64_t>(floor)) pointer = -1;
    } else {
      pointer = first_pointer(kIndexBig5, count_of_big5, cp, floor);
    }
    if (pointer < 0) return false;
    const uint32_t trail = static_cast<uint32_t>(pointer) % 157;
    put(bytes, static_cast<uint32_t>(pointer) / 157 + 0x81);
    put(bytes, trail + (trail < 0x3F ? 0x40 : 0x62));
    return true;
  }
  if (e == Encoding::EucJp) {
    if (cp <= 0x7F) { put(bytes, cp); return true; }
    if (cp == 0xA5) { put(bytes, 0x5C); return true; }
    if (cp == 0x203E) { put(bytes, 0x7E); return true; }
    if (cp >= 0xFF61 && cp <= 0xFF9F) { put(bytes, 0x8E); put(bytes, cp - 0xFF61 + 0xA1); return true; }
    if (cp == 0x2212) cp = 0xFF0D;
    const int64_t pointer = jis0208_pointer(cp);
    if (pointer < 0) return false;
    put(bytes, static_cast<uint32_t>(pointer) / 94 + 0xA1);
    put(bytes, static_cast<uint32_t>(pointer) % 94 + 0xA1);
    return true;
  }
  if (e == Encoding::ShiftJis) {
    if (cp <= 0x80) { put(bytes, cp); return true; }
    if (cp == 0xA5) { put(bytes, 0x5C); return true; }
    if (cp == 0x203E) { put(bytes, 0x7E); return true; }
    if (cp >= 0xFF61 && cp <= 0xFF9F) { put(bytes, cp - 0xFF61 + 0xA1); return true; }
    if (cp == 0x2212) cp = 0xFF0D;
    const int64_t pointer = jis0208_pointer(cp, 8272, 8835);
    if (pointer < 0) return false;
    const uint32_t lead = static_cast<uint32_t>(pointer) / 188, trail = static_cast<uint32_t>(pointer) % 188;
    put(bytes, lead + (lead < 0x1F ? 0x81 : 0xC1));
    put(bytes, trail + (trail < 0x3F ? 0x40 : 0x41));
    return true;
  }
  if (e == Encoding::EucKr) {
    if (cp <= 0x7F) { put(bytes, cp); return true; }
    const int64_t pointer = cp > 0xFFFF ? -1 : first_pointer(kIndexEucKr, count_of_euc_kr, cp);
    if (pointer < 0) return false;
    put(bytes, static_cast<uint32_t>(pointer) / 190 + 0x81);
    put(bytes, static_cast<uint32_t>(pointer) % 190 + 0x41);
    return true;
  }
  if (e == Encoding::XUserDefined) {
    if (cp <= 0x7F) { put(bytes, cp); return true; }
    if (cp >= 0xF780 && cp <= 0xF7FF) { put(bytes, cp - 0xF780 + 0x80); return true; }
    return false;
  }
  // the single byte encodings
  if (cp <= 0x7F) { put(bytes, cp); return true; }
  const int row = kSingleByteRowOf[static_cast<int>(e)];
  if (row < 0 || cp > 0xFFFF) return false;
  const int64_t pointer = first_pointer(kSingleByteRows[row], 128, cp);
  if (pointer < 0) return false;
  put(bytes, static_cast<uint32_t>(pointer) + 0x80);
  return true;
}

void encode_finish(Encoding e, EncoderState* state, String* bytes) {
  if (output_encoding(e) == Encoding::Iso2022Jp && state->mode != 0) {
    state->mode = 0;
    put(bytes, 0x1B); put(bytes, 0x28); put(bytes, 0x42);
  }
}

void encode_url_query(Encoding e, StringView text, bool special, String* out) {
  static const char kHex[] = "0123456789ABCDEF";
  auto push_byte = [&](uint32_t b) {   // percent-encode with the query set
    const bool in_set = b <= 0x20 || b == '"' || b == '#' || b == '<' || b == '>' || b > 0x7E || (special && b == '\'');
    if (in_set) {
      out->push_back('%');
      out->push_back(kHex[b >> 4]);
      out->push_back(kHex[b & 15]);
    } else {
      out->push_back(static_cast<char>(b));
    }
  };
  EncoderState state;
  for (uint64_t at = 0; at < text.size();) {
    const uint32_t cp = next_code_point(text, &at);
    String chunk(out->allocator());
    uint32_t bad = 0;
    if (encode_code_point(e, &state, cp, &chunk, &bad)) {
      for (const char c : chunk.view()) push_byte(static_cast<uint8_t>(c));
    } else {
      for (const char c : chunk.view()) push_byte(static_cast<uint8_t>(c));   // (bytes that came before the refusal, such as a switch to ASCII)
      out->append("%26%23");
      String number(out->allocator());
      number.append_uint(bad);
      out->append(number.view());
      out->append("%3B");
    }
  }
  String tail(out->allocator());
  encode_finish(e, &state, &tail);
  for (const char c : tail.view()) push_byte(static_cast<uint8_t>(c));
}

}  // namespace dom
