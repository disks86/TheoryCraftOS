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

/* winspool.dll (winspool.drv on Windows: canonical_library_name() folds the name) for TheoryCraftOS: the print spooler of a machine with no printers. Enumerations
 * succeed with nothing in them, there is no default printer, and a printer cannot be opened, so every call on a printer handle fails with ERROR_INVALID_HANDLE.
 * Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

#define ERROR_INVALID_PRINTER_NAME_ 1801

/* No printers: TRUE with nothing needed and nothing returned. */
static BOOL enumerate_nothing(LPDWORD needed, LPDWORD returned) {
  if (!needed || !returned) { SetLastError(RPC_X_NULL_REF_POINTER); return FALSE; }
  *needed = 0;
  *returned = 0;
  return TRUE;
}

EXPORT BOOL WINAPI EnumPrintersW(DWORD flags, LPWSTR name, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed, LPDWORD returned) {
  (void)flags; (void)name; (void)level; (void)buffer; (void)size;
  return enumerate_nothing(needed, returned);
}
EXPORT BOOL WINAPI EnumPrintersA(DWORD flags, LPSTR name, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed, LPDWORD returned) {
  (void)flags; (void)name; (void)level; (void)buffer; (void)size;
  return enumerate_nothing(needed, returned);
}

EXPORT BOOL WINAPI GetDefaultPrinterW(LPWSTR buffer, LPDWORD size) {
  (void)buffer;
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *size = 0;
  SetLastError(ERROR_FILE_NOT_FOUND);      /* (what Windows says when no default printer is set) */
  return FALSE;
}
EXPORT BOOL WINAPI GetDefaultPrinterA(LPSTR buffer, LPDWORD size) { return GetDefaultPrinterW((LPWSTR)buffer, size); }
EXPORT BOOL WINAPI SetDefaultPrinterW(LPCWSTR name) { (void)name; SetLastError(ERROR_INVALID_PRINTER_NAME_); return FALSE; }
EXPORT BOOL WINAPI SetDefaultPrinterA(LPCSTR name) { (void)name; SetLastError(ERROR_INVALID_PRINTER_NAME_); return FALSE; }

EXPORT BOOL WINAPI OpenPrinterW(LPWSTR name, HANDLE *printer, void *defaults) {
  (void)name; (void)defaults;
  if (printer) *printer = NULL;
  SetLastError(ERROR_INVALID_PRINTER_NAME_);
  return FALSE;
}
EXPORT BOOL WINAPI OpenPrinterA(LPSTR name, HANDLE *printer, void *defaults) { return OpenPrinterW((LPWSTR)name, printer, defaults); }
EXPORT BOOL WINAPI OpenPrinter2W(LPCWSTR name, HANDLE *printer, void *defaults, void *options) {
  (void)options;
  return OpenPrinterW((LPWSTR)name, printer, defaults);
}
EXPORT BOOL WINAPI ClosePrinter(HANDLE printer) { (void)printer; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }

static BOOL no_printer(LPDWORD needed) {
  if (needed) *needed = 0;
  SetLastError(ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI GetPrinterW(HANDLE printer, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed) { (void)printer; (void)level; (void)buffer; (void)size; return no_printer(needed); }
EXPORT BOOL WINAPI GetPrinterA(HANDLE printer, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed) { (void)printer; (void)level; (void)buffer; (void)size; return no_printer(needed); }
EXPORT BOOL WINAPI GetPrinterDriverW(HANDLE printer, LPWSTR environment, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed) {
  (void)printer; (void)environment; (void)level; (void)buffer; (void)size;
  return no_printer(needed);
}
EXPORT BOOL WINAPI GetPrinterDriverA(HANDLE printer, LPSTR environment, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed) {
  (void)printer; (void)environment; (void)level; (void)buffer; (void)size;
  return no_printer(needed);
}

/* These two return -1 for an error (a count or size otherwise). */
EXPORT LONG WINAPI DocumentPropertiesW(HWND window, HANDLE printer, LPWSTR name, void *out, void *in, DWORD mode) {
  (void)window; (void)printer; (void)name; (void)out; (void)in; (void)mode;
  SetLastError(ERROR_INVALID_HANDLE);
  return -1;
}
EXPORT LONG WINAPI DocumentPropertiesA(HWND window, HANDLE printer, LPSTR name, void *out, void *in, DWORD mode) {
  return DocumentPropertiesW(window, printer, (LPWSTR)name, out, in, mode);
}
EXPORT int WINAPI DeviceCapabilitiesW(LPCWSTR device, LPCWSTR port, WORD capability, LPWSTR output, const DEVMODEW *mode) {
  (void)device; (void)port; (void)capability; (void)output; (void)mode;
  SetLastError(ERROR_INVALID_PRINTER_NAME_);
  return -1;
}
EXPORT int WINAPI DeviceCapabilitiesA(LPCSTR device, LPCSTR port, WORD capability, LPSTR output, const DEVMODEA *mode) {
  (void)device; (void)port; (void)capability; (void)output; (void)mode;
  SetLastError(ERROR_INVALID_PRINTER_NAME_);
  return -1;
}
