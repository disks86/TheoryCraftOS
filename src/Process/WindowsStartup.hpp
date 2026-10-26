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
#include "Executable/PeFile.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/ImageLoader.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace process {

// Where the structures a Windows program expects to find ended up.
struct WindowsStartup {
  uint64_t peb = 0;                 // Process Environment Block: the program's first argument, and gs:[0x60]
  uint64_t teb = 0;                 // Thread Environment Block: the GS base
  uint64_t parameters = 0;          // RTL_USER_PROCESS_PARAMETERS (command line, image path, environment, standard handles)
  uint64_t tls_block = 0;           // this thread's copy of the image's static thread-local data (0 if it has none)
  uint64_t shared_user_data = 0;    // KUSER_SHARED_DATA at its fixed address
};

struct WindowsStartupInput {
  allocators::IAllocator* allocator = nullptr;      // for converting the text to UTF-16
  uint64_t process_id = 0, thread_id = 0;
  uint64_t stack_top = 0, stack_size = 0;
  StringView image_path;                            // as Windows would show it ("C:\\dir\\program.exe")
  StringView command_line;
  ArrayView<const StringView> environment;          // "NAME=value" each
  uint32_t processor_count = 1;
};

// KUSER_SHARED_DATA is mapped here in every Windows process; programs and ntdll read the version and the processor count from it.
constexpr uint64_t kSharedUserDataAddress = 0x7FFE0000ull;
// Standard handles as the NT services hand them out (see WindowsSyscalls.cpp): input, output, error.
constexpr uint64_t kStdInputHandle = 4, kStdOutputHandle = 8, kStdErrorHandle = 12;

// Maps and fills the PEB, the TEB, the process parameters, the shared data page and the static TLS block of a loaded PE image, in one new region of the
// address space. The image's TLS directory is honoured (the template is copied, the zero fill is zero, the index variable becomes 0, the TEB points at the
// block); TLS callbacks are not run. Reports a version of Windows 10 (10.0.19045). The shared data is a snapshot, not a live clock.
LoadStatus build_windows_startup(AddressSpace& space, const exe::PeFile& pe, const LoadedImage& image, const WindowsStartupInput& input, WindowsStartup* out);

}  // namespace process
