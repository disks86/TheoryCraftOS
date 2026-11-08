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

/* mfreadwrite.dll for TheoryCraftOS: the source reader and sink writer of Media Foundation. There is no Media Foundation here (see mfplat.dll): the readers and
 * writers cannot be made, E_NOTIMPL with the out-parameter NULL. Built freestanding over kernel32.dll and ntdll.dll. */
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

EXPORT HRESULT WINAPI MFCreateSourceReaderFromMediaSource(void *source, void *attributes, void **reader) { (void)source; (void)attributes; if (reader) *reader = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateSourceReaderFromURL(LPCWSTR url, void *attributes, void **reader) { (void)url; (void)attributes; if (reader) *reader = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateSourceReaderFromByteStream(void *stream, void *attributes, void **reader) { (void)stream; (void)attributes; if (reader) *reader = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateSinkWriterFromURL(LPCWSTR url, void *stream, void *attributes, void **writer) { (void)url; (void)stream; (void)attributes; if (writer) *writer = NULL; return E_NOTIMPL; }
EXPORT HRESULT WINAPI MFCreateSinkWriterFromMediaSink(void *sink, void *attributes, void **writer) { (void)sink; (void)attributes; if (writer) *writer = NULL; return E_NOTIMPL; }
