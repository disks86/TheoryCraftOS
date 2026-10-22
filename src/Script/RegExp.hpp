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


// Regular expressions for scripts: the RegExp object (on top of the backtracking matcher in src/Regex) and what String does with one - match, matchAll,
// replace, replaceAll, search and split. Patterns and subjects are UTF-8 (the matcher runs in its UTF-8 mode, so one character is one code point), and the
// positions a script sees - index, lastIndex - are byte offsets, as everywhere in this engine. Lookbehind and Unicode properties are not supported (the
// constructor throws a SyntaxError), and a search that would run away stops with a RangeError.
#pragma once
#include "Script/Vm.hpp"

namespace script {

bool install_regexp(Vm& vm);
// Frees what a RegExp object holds (called when the object is collected).
void regexp_release(allocators::IAllocator& allocator, void* compiled);

bool regexp_is(const Value& v);
// A RegExp made from the text of an argument of match(), matchAll() or search() (a string is a pattern).
bool regexp_from_argument(Vm& vm, const Value& argument, const char* flags, Value* out);
bool regexp_match(Vm& vm, JsString* subject, const Value& regexp, Value* result);
bool regexp_match_all(Vm& vm, JsString* subject, const Value& regexp, Value* result);
bool regexp_search(Vm& vm, JsString* subject, const Value& regexp, Value* result);
bool regexp_replace(Vm& vm, JsString* subject, const Value& regexp, const Value& replacement, bool replace_all, Value* result);
bool regexp_split(Vm& vm, JsString* subject, const Value& regexp, int64_t limit, Value* result);

}  // namespace script
