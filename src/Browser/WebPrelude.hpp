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

// The parts of the web platform that are written in JavaScript on top of the natives in DomBindingsNet.cpp: URLSearchParams, Headers, Blob, File, FileReader,
// TextEncoder, TextDecoder, AbortController, Request, Response, fetch() and XMLHttpRequest. A page's engine runs this once, before the page's own scripts.
#pragma once

namespace browser {

// The source text of the script.
const char* web_prelude();

}  // namespace browser
