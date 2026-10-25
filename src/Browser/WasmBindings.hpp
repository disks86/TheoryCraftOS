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

// WebAssembly for a script engine: the natives under the WebAssembly object. A page and a worker each get their own store of modules, instances, memories and tables.
#pragma once
#include "Script/Vm.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {
namespace bind {

struct WasmState;

// Installs the natives (__wasm_*) into the engine and gives the state they work on, which must live as long as the engine does. The engine's extension pointer
// (Vm::set_extension) is taken for it. After this, run web_assembly_prelude() to get the WebAssembly object.
UniquePointer<WasmState> install_wasm_natives(script::Vm& vm);

}  // namespace bind
}  // namespace browser
