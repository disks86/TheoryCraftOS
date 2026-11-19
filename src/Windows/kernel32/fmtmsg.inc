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

/* kernel32.dll: FormatMessage. The message text is a template with inserts (%1 .. %99, optionally %1!printf-type!), a few escapes (%n new line, %r, %t, %0 ends the text, %%
 * and the like give the character) and line breaks that are handled according to the width in the low byte of the flags: 0 keeps them, 255 turns them into blanks, any other
 * width wraps the text at a blank before the line reaches that many characters. The arguments are an array of pointer-sized values (FORMAT_MESSAGE_ARGUMENT_ARRAY) or the
 * slots of a variable argument list, which are read in order as the inserts ask for them and which the call leaves behind the last one it read. The output is made in wide
 * characters and stops at once when the caller's buffer is full. Checked with Wine's tests (tests/windows/wine/kernel32/format_msg.c). Included by kernel32.c. */

#define FMT_HEAP() heap()
#include "../fmtengine.inc"

#include "errtext.inc"

static DWORD fmt_message(DWORD flags, LPCVOID source, DWORD id, void *buffer, DWORD size, PVOID arguments, int narrow) {
  const int alloc = (flags & FMT_ALLOCATE_BUFFER) != 0, ignore = (flags & FMT_IGNORE_INSERTS) != 0;
  if (alloc) {
    if (!buffer) { set_error(narrow ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return 0; }
    *(void **)buffer = NULL;
  } else if (!buffer) {
    set_error(narrow ? ERROR_INSUFFICIENT_BUFFER : ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (!(flags & (FMT_FROM_STRING | FMT_FROM_HMODULE | FMT_FROM_SYSTEM))) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  WCHAR *template_text = NULL, *template_owned = NULL;
  WCHAR system_text[320];
  if (flags & FMT_FROM_STRING) {
    if (!source) { set_error(ERROR_INVALID_PARAMETER); return 0; }
    if (narrow) {
      const int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)source, -1, NULL, 0);
      template_owned = RtlAllocateHeap(heap(), 0, (SIZE_T)(n > 0 ? n : 1) * 2 + 2);
      if (!template_owned) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
      if (n > 0) MultiByteToWideChar(CP_UTF8, 0, (const char *)source, -1, template_owned, n); else template_owned[0] = 0;
      template_text = template_owned;
    } else {
      template_text = (WCHAR *)source;
    }
  } else {
    const char *text = system_message_text(id);
    if (!text) { set_error(317); return 0; }      /* ERROR_MR_MID_NOT_FOUND */
    size_t_ n = 0;
    while (text[n] && n < 300) { system_text[n] = (WCHAR)(BYTE)text[n]; ++n; }
    system_text[n++] = '\r'; system_text[n++] = '\n'; system_text[n] = 0;
    template_text = system_text;
  }
  FmtArgs *args = RtlAllocateHeap(heap(), 0, sizeof(FmtArgs));
  if (!args) { if (template_owned) RtlFreeHeap(heap(), 0, template_owned); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  memset(args, 0, sizeof(*args));
  args->have = arguments != NULL;
  if (arguments) { if (flags & FMT_ARGUMENT_ARRAY) args->array = (const ULONG_PTR *)arguments; else args->list = *(char **)arguments; }
  FmtText out;
  memset(&out, 0, sizeof(out));
  out.width = flags & FMT_MAX_WIDTH_MASK;
  out.limited = !alloc;
  out.size = size;
  fmt_template(template_text, &out, args, ignore, narrow);
  /* the list leaves the caller where this call stopped reading */
  if (arguments && !(flags & FMT_ARGUMENT_ARRAY)) *(char **)arguments = args->list;
  RtlFreeHeap(heap(), 0, args);
  if (template_owned) RtlFreeHeap(heap(), 0, template_owned);
  DWORD result = 0;
  char *narrow_text = NULL;
  if (out.failed) {
    set_error(ERROR_NOT_ENOUGH_MEMORY);
  } else if (out.invalid) {
    if (!narrow && !alloc && out.length && size) memcpy(buffer, out.text, (out.length < size ? out.length : size) * 2);      /* (what came before it is there, without an end) */
    set_error(ERROR_INVALID_PARAMETER);
  } else if (out.overflow) {
    if (!narrow && !alloc && size) { memcpy(buffer, out.text, out.length * 2); ((WCHAR *)buffer)[out.length] = 0; }
    set_error(ERROR_INSUFFICIENT_BUFFER);
  } else if (out.length == 0) {
    if (!narrow && !alloc && size) ((WCHAR *)buffer)[0] = 0;
    set_error(235);      /* ERROR_NO_WORK_DONE */
  } else if (narrow && out.length > FMT_ANSI_LIMIT) {
    set_error(ERROR_INVALID_PARAMETER);
  } else {
    out.text[out.length] = 0;
    DWORD chars = (DWORD)out.length;
    if (narrow) {
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, out.text, (int)out.length + 1, NULL, 0, NULL, NULL);
      narrow_text = RtlAllocateHeap(heap(), 0, (SIZE_T)(bytes > 0 ? bytes : 1));
      if (!narrow_text) { set_error(ERROR_NOT_ENOUGH_MEMORY); goto done; }
      WideCharToMultiByte(CP_UTF8, 0, out.text, (int)out.length + 1, narrow_text, bytes, NULL, NULL);
      chars = (DWORD)bytes - 1;
    }
    const unsigned unit = narrow ? 1 : 2;
    const void *from = narrow ? (const void *)narrow_text : (const void *)out.text;
    if (alloc) {
      void *memory = LocalAlloc(0, ((size_t_)(chars + 1 > size ? chars + 1 : size)) * unit);
      if (!memory) { set_error(ERROR_NOT_ENOUGH_MEMORY); goto done; }
      memcpy(memory, from, ((size_t_)chars + 1) * unit);
      *(void **)buffer = memory;
      result = chars;
    } else if ((size_t_)chars + 1 > size) {
      set_error(ERROR_INSUFFICIENT_BUFFER);      /* (the bytes of a narrow text can outnumber its characters) */
    } else {
      memcpy(buffer, from, ((size_t_)chars + 1) * unit);
      result = chars;
    }
  }
done:
  if (narrow_text) RtlFreeHeap(heap(), 0, narrow_text);
  if (out.text) RtlFreeHeap(heap(), 0, out.text);
  return result;
}
EXPORT DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID source, DWORD id, DWORD language, LPSTR buffer, DWORD capacity, PVOID arguments) { (void)language; return fmt_message(flags, source, id, buffer, capacity, arguments, 1); }
EXPORT DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID source, DWORD id, DWORD language, LPWSTR buffer, DWORD capacity, PVOID arguments) { (void)language; return fmt_message(flags, source, id, buffer, capacity, arguments, 0); }
