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

// The GLSL preprocessor: a token-based preprocessor with the observable behaviour of Mesa's glcpp (the same output and the same messages, positions included), checked
// against Mesa's own corpus by tests/GlcppTest.cpp. Input is untrusted: nesting, memory and macro-expansion work are bounded, and running into a bound is an ordinary
// preprocessor error, never a crash.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace shader {

struct PreprocessOptions {
  // Version when the source has no #version (110 desktop, 100 for ES callers); used for __VERSION__ and for which rules apply.
  uint32_t default_version = 110;
  bool default_es = false;
  // Names defined as 1 for every version (the extension table of the caller, e.g. "GL_ARB_gpu_shader5"). May be empty.
  ArrayView<const StringView> extension_macros;
  // Mesa's --disable-line-continuation equivalent, for tests that need it.
  bool disable_line_continuation = false;
};

struct PreprocessResult {
  explicit PreprocessResult(allocators::IAllocator& a) : output(a), log(a) {}
  String output;          // the text with directives processed. As with glcpp: #version, #extension, #pragma and #line remain in the output as lines for the parser's lexer to consume, newlines are preserved so that line numbers keep matching the source.
  String log;             // errors and warnings, Mesa format "0:LINE(COL): preprocessor error: ...\n"
  bool ok = true;         // false if any error was reported
  uint32_t version = 0;   // the number in #version (default_version if none)
  bool version_declared = false;
  bool es = false;        // `#version NNN es`, or #version 100, or default_es without #version
  uint8_t profile = 0;    // 0 none given, 1 core, 2 compatibility
};

// Runs the preprocessor over `source` (which ends at its first NUL, if it has one). Returns result->ok.
bool preprocess_glsl(allocators::IAllocator& allocator, StringView source, const PreprocessOptions& options, PreprocessResult* result);

}  // namespace shader
