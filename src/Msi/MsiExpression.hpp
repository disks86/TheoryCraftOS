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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// The two little languages of a Windows Installer package's tables:
//
//   - conditions ("VersionNT >= 601 AND NOT Installed"): properties, numbers and "text" compared with = <> < > <= >= and the substring operators
//     >< (contains), << (starts with), >> (ends with), a ~ in front of an operator for "without regard to case", joined by NOT, AND, OR, XOR, EQV and
//     IMP. A value alone is true if it is not empty (text) or not zero (a number). %NAME is an environment variable; $Component and &Feature are what
//     this run does with a component or feature, ?Component and !Feature what state they are in (3: installed here, 2: absent, -1: none);
//   - formatted text ("[INSTALLDIR]bin\[#MainExe]"): [Property], [%Environment], [#FileKey] and [!FileKey] (where that file goes), [$Component] (the
//     folder of that component), [\x] (the character x). [~] (the separator of a list of strings) and [1] (a field of a record) are left as they are.
//
// What the names stand for is asked of the session that is running the package.
namespace msi {

inline constexpr int32_t kStateNone = -1, kStateAbsent = 2, kStateLocal = 3;

class IExpressionContext {
 public:
  // The property's value ("" if it has none).
  virtual StringView property(StringView name) const = 0;
  // $ ? & ! with a name: what is done with, or the state of, a component or a feature.
  virtual int32_t state(char kind, StringView name) const = 0;
  // Where the file of that key is installed, and the folder of that component (Windows paths). False if there is none.
  [[nodiscard]] virtual bool file_path(StringView key, String* out) const = 0;
  [[nodiscard]] virtual bool component_folder(StringView key, String* out) const = 0;

 protected:
  ~IExpressionContext() = default;
};

// Evaluates `condition` (an empty one is true). False if it cannot be read (a syntax error): *result is then false.
[[nodiscard]] bool evaluate_condition(StringView condition, const IExpressionContext& context, bool* result);

// Appends `text` with everything in square brackets resolved. False only if memory ran out.
[[nodiscard]] bool format_text(allocators::IAllocator& allocator, StringView text, const IExpressionContext& context, String* out);

// Is `text` a whole number ("-12", "007")? *value gets it.
bool parse_integer(StringView text, int64_t* value);
// Without regard to the case of ASCII letters.
constexpr char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
bool equals_folded(StringView a, StringView b);

}  // namespace msi
