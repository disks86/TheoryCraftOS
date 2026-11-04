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

/* dwrite.dll for TheoryCraftOS: DirectWrite, the text API of Windows 7+ (the IDWriteFactory interface family of dwrite.h).
 *
 * Where the work is done. Windows keeps its fonts in files and a font cache service and rasterises in dwrite.dll itself. Here the fonts are the kernel's (src/Font: the bundled
 * Liberation/Noto faces, and any font a program adds), reached with the one service NtTcosFont (src/Process/WindowsFonts.hpp): the number of faces, their metrics and names,
 * code point -> glyph, glyph advances and boxes, kerning, outlines, anti-aliased glyph bitmaps, the bytes of the font file (for the table functions) and adding a font file.
 * Everything that Windows decides in the DLL is done here: the COM objects (font collection, family, font, face, localized strings, font files and their loaders, text format,
 * text layout, text analyzer, glyph run analysis, GDI interop, rendering parameters, typography, number substitution, the ellipsis trimming sign), font matching, the layout
 * (line breaking, alignment, trimming, hit testing, drawing through the program's IDWriteTextRenderer) and the arguments checks and HRESULTs.
 *
 * Fonts. The system collection has the families the bundled faces stand in for (Arial, Segoe UI ... for the sans faces, Times New Roman ... for serif, Courier New ... for
 * mono; the Liberation faces are metric compatible with Arial, Times New Roman and Courier New) with regular/bold/italic/bold italic each. A font file the program refers to is
 * read and added to the kernel's fonts (TrueType and WOFF with glyf outlines; CFF is refused), then it is an ordinary face.
 *
 * Not here: complex-script shaping and bidirectional reordering (text runs left to right, one glyph per code point, kerning from the font's kern table, no ligatures), font
 * simulations (bold/oblique are never synthesised), colour fonts, variable fonts, IDWriteFactory1 and later interfaces, the bitmap render target and CreateFontFaceFromHdc of
 * the GDI interop. Built freestanding over kernel32.dll and ntdll.dll with the OS's mingw compiler. */
#define INITGUID
#define COBJMACROS
#define CONST_VTABLE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <unknwn.h>
#include <d2d1.h>
#include <dwrite.h>
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT

#pragma GCC diagnostic ignored "-Wunused-function"

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "base.inc"
#include "face.inc"
#include "fonts.inc"
#include "format.inc"
#include "layout.inc"
#include "draw.inc"
#include "analysis.inc"
#include "factory.inc"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
  (void)module;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_lock);
  return TRUE;
}
