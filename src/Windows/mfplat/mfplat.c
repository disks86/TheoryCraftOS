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

/* mfplat.dll for TheoryCraftOS: the platform of Media Foundation. There is no Media Foundation here, so MFStartup fails (E_NOTIMPL) and a program takes its other path
 * (Chrome then decodes and captures without it); every function that would hand out an object fails the same way with its out-parameters cleared, so nothing is used
 * that does not exist. Real: MFShutdown (nothing to shut down), MFGetSystemTime (the performance counter in 100 ns units) and MFFrameRateToAverageTimePerFrame
 * (arithmetic). Built freestanding over kernel32.dll and ntdll.dll. */
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

#define CLEAR(p) do { if (p) *(p) = NULL; } while (0)

EXPORT HRESULT WINAPI MFStartup(ULONG version, DWORD flags) { (void)version; (void)flags; return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFShutdown(void) { return S_OK; }

/* The system time of Media Foundation: 100 ns units of the performance counter. */
EXPORT LONGLONG WINAPI MFGetSystemTime(void) {
  LARGE_INTEGER counter, frequency;
  if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return (LONGLONG)GetTickCount64() * 10000;
  const ULONGLONG c = (ULONGLONG)counter.QuadPart, f = (ULONGLONG)frequency.QuadPart;
  return (LONGLONG)((c / f) * 10000000ull + (c % f) * 10000000ull / f);      /* (split so that it cannot overflow) */
}

/* The average duration of a frame (100 ns units) at numerator/denominator frames a second, rounded to the nearest. */
EXPORT HRESULT WINAPI MFFrameRateToAverageTimePerFrame(UINT32 numerator, UINT32 denominator, UINT64 *average) {
  if (!average) return E_POINTER;
  *average = numerator ? (10000000ull * denominator + numerator / 2) / numerator : 0;
  return S_OK;
}

EXPORT HRESULT WINAPI MFCreateAttributes(void **attributes, UINT32 size) { (void)size; CLEAR(attributes); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateMediaType(void **type) { CLEAR(type); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateSample(void **sample) { CLEAR(sample); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateMemoryBuffer(DWORD length, void **buffer) { (void)length; CLEAR(buffer); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateAlignedMemoryBuffer(DWORD length, DWORD alignment, void **buffer) { (void)length; (void)alignment; CLEAR(buffer); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateAsyncResult(void *object, void *callback, void *state, void **result) { (void)object; (void)callback; (void)state; CLEAR(result); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateEventQueue(void **queue) { CLEAR(queue); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateMediaEvent(DWORD type, const GUID *extended_type, HRESULT status, const void *value, void **event) {
  (void)type; (void)extended_type; (void)status; (void)value;
  CLEAR(event);
  return E_NOTIMPL;
}
EXPORT HRESULT WINAPI MFCreateDXGIDeviceManager(UINT *token, void **manager) { if (token) *token = 0; CLEAR(manager); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFLockDXGIDeviceManager(UINT *token, void **manager) { if (token) *token = 0; CLEAR(manager); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFUnlockDXGIDeviceManager(void) { return S_OK; }
EXPORT HRESULT WINAPI MFCreateDXGISurfaceBuffer(const GUID *riid, void *surface, UINT subresource, BOOL bottom_up, void **buffer) {
  (void)riid; (void)surface; (void)subresource; (void)bottom_up;
  CLEAR(buffer);
  return E_NOTIMPL;
}
EXPORT HRESULT WINAPI MFCreatePresentationDescriptor(DWORD count, void **streams, void **descriptor) { (void)count; (void)streams; CLEAR(descriptor); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateStreamDescriptor(DWORD id, DWORD count, void **types, void **descriptor) { (void)id; (void)count; (void)types; CLEAR(descriptor); return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFSerializeAttributesToStream(void *attributes, DWORD options, void *stream) { (void)attributes; (void)options; (void)stream; return E_NOTIMPL; }

/* No transforms are registered. (The category GUID is passed by value, which the x64 convention turns into a pointer.) */
EXPORT HRESULT WINAPI MFTEnumEx(const GUID *category, UINT32 flags, const void *input, const void *output, void ***activates, UINT32 *count) {
  (void)category; (void)flags; (void)input; (void)output;
  if (activates) *activates = NULL;
  if (count) *count = 0;
  return E_NOTIMPL;
}
