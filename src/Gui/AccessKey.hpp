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

// Access keys (docs/UserInterfaceGuidelines.md, 4.1): the underlined letter of a menu title, a menu item or a control's label. Alt+letter goes to
// the control and activates it; in an open menu the letter alone chooses the item.
//
// Nobody writes them by hand: a scope (one frame, one menu) hands out the letters, so they are unique in it. For each label, in the order the labels
// appear, the first letter is taken if it is free, else the first free consonant, else the first free vowel. A label can ask for a letter of its own
// (Control::set_access_key, MenuItem::access_key), which is served before the automatic ones. A label with no free letter gets none.
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace gui {

class AccessKeys {
 public:
  static constexpr int32_t kNone = -1;

  // The letter as the scope counts it (lower case), 0 for anything that is not a letter.
  static constexpr char fold(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
    return c >= 'a' && c <= 'z' ? c : 0;
  }
  // The access key a label shows for the index it was given (0: none).
  static char key_of(StringView label, int32_t index) {
    return index >= 0 && static_cast<uint64_t>(index) < label.size() ? fold(label[static_cast<uint64_t>(index)]) : 0;
  }

  bool taken(char key) const { return fold(key) && (taken_ & bit(key)) != 0; }
  // Where `key` is in the label, taken for it; kNone if the label has no such letter or the letter is in use.
  int32_t claim(StringView label, char key) {
    const char wanted = fold(key);
    if (!wanted || taken(wanted)) return kNone;
    for (uint64_t i = 0; i < label.size(); ++i) {
      if (fold(label[i]) != wanted) continue;
      taken_ |= bit(wanted);
      return static_cast<int32_t>(i);
    }
    return kNone;
  }
  // The letter the rule gives the label (see above), taken for it: its index, or kNone.
  int32_t pick(StringView label) {
    int32_t first = kNone, consonant = kNone, vowel = kNone;
    for (uint64_t i = 0; i < label.size(); ++i) {
      const char c = fold(label[i]);
      if (!c) continue;
      const bool free_letter = !taken(c);
      if (first == kNone) {
        if (free_letter) return take(label, i);
        first = static_cast<int32_t>(i);
        continue;
      }
      if (!free_letter) continue;
      if (is_vowel(c)) {
        if (vowel == kNone) vowel = static_cast<int32_t>(i);
      } else if (consonant == kNone) {
        consonant = static_cast<int32_t>(i);
      }
    }
    if (consonant != kNone) return take(label, static_cast<uint64_t>(consonant));
    if (vowel != kNone) return take(label, static_cast<uint64_t>(vowel));
    return kNone;
  }

 private:
  static constexpr bool is_vowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
  static constexpr uint32_t bit(char key) { return uint32_t{1} << (fold(key) - 'a'); }
  int32_t take(StringView label, uint64_t index) {
    taken_ |= bit(label[index]);
    return static_cast<int32_t>(index);
  }

  uint32_t taken_ = 0;  // one bit per letter
};

}  // namespace gui
