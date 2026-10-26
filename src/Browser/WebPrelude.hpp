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
// What a Web Worker's script engine runs before the worker's own script: self, postMessage, onmessage, close, importScripts ...
const char* worker_prelude();
// The DOM interfaces that are written in JavaScript (Range, Selection, TreeWalker, NodeIterator, NodeFilter, Attr nodes, CharacterData methods, Option, XMLSerializer ...).
const char* dom_prelude();
// The WebAssembly object (Module, Instance, Memory, Table, Global, the errors, validate / compile / instantiate) over the natives of DomBindingsWasm.cpp. Run by a page's
// engine and a worker's, after install_wasm_natives().
const char* web_assembly_prelude();
// ES modules (import(), the module loader, import maps) and the running of <script> elements put in the document by scripts, over __compile_module. Run by a page's engine.
const char* module_prelude();
// WebGLRenderingContext and WebGL2RenderingContext over the natives of DomBindingsGl.cpp. Run by a page's engine.
const char* webgl_prelude();
// navigator.gpu and the WebGPU interfaces over the natives of DomBindingsGpu.cpp. Run by a page's engine.
const char* webgpu_prelude();

}  // namespace browser
