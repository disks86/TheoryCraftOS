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

/* winusb.dll for TheoryCraftOS: the user-mode USB API of the WinUSB driver. No device is bound to WinUSB here, so WinUsb_Initialize refuses every device handle
 * (ERROR_NOT_SUPPORTED, NULL interface) and, since no interface handle can exist, every other call fails with ERROR_INVALID_HANDLE and clears its counts and handles.
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

typedef void *WINUSB_INTERFACE_HANDLE;

static BOOL fail(DWORD error) {
  SetLastError(error);
  return FALSE;
}

EXPORT BOOL WINAPI WinUsb_Initialize(HANDLE device, WINUSB_INTERFACE_HANDLE *handle) {
  (void)device;
  if (handle) *handle = NULL;
  return fail(handle ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
}
EXPORT BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE handle) { (void)handle; return fail(ERROR_INVALID_HANDLE); }
EXPORT BOOL WINAPI WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE handle, UCHAR index, WINUSB_INTERFACE_HANDLE *associated) {
  (void)handle; (void)index;
  if (associated) *associated = NULL;
  return fail(ERROR_INVALID_HANDLE);
}
/* (WINUSB_SETUP_PACKET is 8 bytes, passed by value in a register.) */
EXPORT BOOL WINAPI WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE handle, ULONGLONG setup, BYTE *buffer, ULONG length, ULONG *transferred, OVERLAPPED *overlapped) {
  (void)handle; (void)setup; (void)buffer; (void)length; (void)overlapped;
  if (transferred) *transferred = 0;
  return fail(ERROR_INVALID_HANDLE);
}
EXPORT BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, BYTE *buffer, ULONG length, ULONG *transferred, OVERLAPPED *overlapped) {
  (void)handle; (void)pipe; (void)buffer; (void)length; (void)overlapped;
  if (transferred) *transferred = 0;
  return fail(ERROR_INVALID_HANDLE);
}
EXPORT BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, BYTE *buffer, ULONG length, ULONG *transferred, OVERLAPPED *overlapped) {
  (void)handle; (void)pipe; (void)buffer; (void)length; (void)overlapped;
  if (transferred) *transferred = 0;
  return fail(ERROR_INVALID_HANDLE);
}
EXPORT BOOL WINAPI WinUsb_ResetPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe) { (void)handle; (void)pipe; return fail(ERROR_INVALID_HANDLE); }
EXPORT BOOL WINAPI WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE handle, UCHAR setting) { (void)handle; (void)setting; return fail(ERROR_INVALID_HANDLE); }
EXPORT BOOL WINAPI WinUsb_GetOverlappedResult(WINUSB_INTERFACE_HANDLE handle, OVERLAPPED *overlapped, ULONG *transferred, BOOL wait) {
  (void)handle; (void)overlapped; (void)wait;
  if (transferred) *transferred = 0;
  return fail(ERROR_INVALID_HANDLE);
}
