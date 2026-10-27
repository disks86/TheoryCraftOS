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

/* Exports that exist so that a program finds them, but do nothing yet. The list of a DLL is stubs/<dll>.inc (see scripts/gen-windows-stubs.py), the build passes its file name as
 * STUB_FILE and the value the functions return as STUB_RESULT (STATUS_NOT_IMPLEMENTED for ntdll.dll, 0 - that is FALSE, NULL, ERROR_SUCCESS - for the others). A library
 * that is only stubs, or has no entry point of its own, is built with STUB_DLLMAIN too, which adds a default one.
 * Every function takes no parameters and leaves the registers of its caller alone, so a call with any arguments is harmless. */
#ifdef STUB_FILE
#define STUB_STRING_(x) #x
#define STUB_STRING(x) STUB_STRING_(x)
#define X(name) __declspec(dllexport) unsigned long long name(void) { return STUB_RESULT; }
#include STUB_STRING(STUB_FILE)
#endif

#ifdef STUB_DLLMAIN
__attribute__((weak)) int DllMain(void* module, unsigned reason, void* reserved) {
  (void)module;
  (void)reason;
  (void)reserved;
  return 1;
}
#endif
