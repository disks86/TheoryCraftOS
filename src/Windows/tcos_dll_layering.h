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
/* The libraries of this OS that are built on each other where Windows' are not (docs/WindowsUserMode.md, "Libraries of the same name").
 *
 * Direct3D 8 is a layer on d3d9.dll here, Direct3D 9 and 11 are layers on d3d12.dll, and so on; on Windows each of them is a runtime of its own over the driver.
 * A program that brings its own copy of one of the lower libraries (DXVK's d3d9.dll or d3d11.dll, vkd3d-proton's d3d12.dll, a ReShade proxy) must not change
 * what the upper ones of System32 run on: on Windows a d3d9.dll next to the program never affects ddraw.dll or d3d8.dll. So when the importer is the copy of
 * System32 (or SysWOW64), an import (or a forwarder) of one of these pairs is the System32 file of the imported library: the one loaded from there if there
 * is one, else a new instance of that file, never a library of that name from the program's folder. Every other import follows Windows' rules.
 *
 * Shared by the kernel's loader (src/Process/ModuleLoader.cpp: imports and forwarders bound when a library is loaded) and ntdll.dll (forwarders that
 * GetProcAddress follows). C and C++. */
#pragma once

/* LDR_DATA_TABLE_ENTRY.Flags of a library that was loaded only as such an internal layer (ours; Windows leaves the bit to drivers): it is in the module lists
 * but is not found by its base name (GetModuleHandle("d3d9.dll"), LoadLibrary("d3d9.dll")), as the library is not there at all on Windows. A load of its
 * file by path makes it an ordinary library. */
#define TCOS_LDRP_PRIVATE_LAYER 0x08000000u

/* (importer, imported): canonical names, lower case with the extension. */
static const char *const tcos_dll_layering[][2] = {
  {"d3d8.dll", "d3d9.dll"},          /* Direct3D 8 is wrappers over d3d9.dll's interfaces */
  {"ddraw.dll", "d3d9.dll"},         /* Direct3D 1-7 draw through a Direct3D 9 device (loaded at run time by its System32 path) */
  {"d3d9.dll", "d3d12.dll"},         /* Direct3D 9 is a layer on d3d12.dll */
  {"d3d11.dll", "d3d12.dll"},        /* Direct3D 11 is a layer on d3d12.dll (its dxgi.dll import is Windows' own) */
  {"d3d10core.dll", "d3d11.dll"},    /* D3D10CoreCreateDevice and OpenAdapter10(_2) are d3d11.dll's (TcosD3D10CoreCreateDevice) */
  {"d3d10_1.dll", "d3d10.dll"},      /* the shared functions are forwarders to d3d10.dll (TcosD3D10_1CreateEffect(Pool)FromMemory) */
  {"d3d10_1.dll", "d3d10core.dll"},  /* Windows' d3d10_1.dll uses d3d10_1core.dll; this one makes its devices with d3d10core.dll */
};

static inline char tcos_layering_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
/* Is `name` (`length` characters, any case) the canonical name `canonical`? */
static inline int tcos_layering_same(const char *name, unsigned long length, const char *canonical) {
  unsigned long i = 0;
  for (; i < length && canonical[i]; ++i) if (tcos_layering_lower(name[i]) != canonical[i]) return 0;
  return i == length && !canonical[i];
}
/* Is the import of `imported` by `importer` (both base names with extension, any case) one of the internal layers? */
static inline int tcos_is_internal_layer(const char *importer, unsigned long importer_length, const char *imported, unsigned long imported_length) {
  for (unsigned long i = 0; i < sizeof tcos_dll_layering / sizeof tcos_dll_layering[0]; ++i) {
    if (tcos_layering_same(importer, importer_length, tcos_dll_layering[i][0]) && tcos_layering_same(imported, imported_length, tcos_dll_layering[i][1])) return 1;
  }
  return 0;
}
