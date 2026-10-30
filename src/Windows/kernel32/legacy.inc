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

/* kernel32.dll: the 16-bit era file functions (OpenFile, _lopen, _lread, ...) over the Win32 ones; an HFILE is the file handle's value. Included by kernel32.c. */

#define HFILE_ERROR_ (-1)
#define OF_READ_ 0
#define OF_WRITE_ 1
#define OF_READWRITE_ 2
#define OF_SHARE_COMPAT_ 0
#define OF_SHARE_EXCLUSIVE_ 0x10
#define OF_SHARE_DENY_WRITE_ 0x20
#define OF_SHARE_DENY_READ_ 0x30
#define OF_SHARE_DENY_NONE_ 0x40
#define OF_PARSE_ 0x100
#define OF_DELETE_ 0x200
#define OF_VERIFY_ 0x400
#define OF_CANCEL_ 0x800
#define OF_CREATE_ 0x1000
#define OF_PROMPT_ 0x2000
#define OF_EXIST_ 0x4000
#define OF_REOPEN_ 0x8000

static HANDLE hfile_to_handle(int h) { return (HANDLE)(LONG_PTR)h; }
EXPORT int WINAPI _lopen(LPCSTR path, int mode) {
  DWORD access = (mode & 3) == OF_WRITE_ ? GENERIC_WRITE : (mode & 3) == OF_READWRITE_ ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  DWORD share = 3;      /* FILE_SHARE_READ | FILE_SHARE_WRITE */
  switch (mode & 0x70) {
    case OF_SHARE_EXCLUSIVE_: share = 0; break;
    case OF_SHARE_DENY_WRITE_: share = 1; break;
    case OF_SHARE_DENY_READ_: share = 2; break;
    default: break;
  }
  HANDLE h = CreateFileA(path, access, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  return h == INVALID_HANDLE_VALUE ? HFILE_ERROR_ : (int)(LONG_PTR)h;
}
EXPORT int WINAPI _lcreat(LPCSTR path, int attributes) {
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 3, NULL, CREATE_ALWAYS, (DWORD)attributes & 0x27 ? (DWORD)attributes & 0x27 : FILE_ATTRIBUTE_NORMAL, NULL);
  return h == INVALID_HANDLE_VALUE ? HFILE_ERROR_ : (int)(LONG_PTR)h;
}
EXPORT UINT WINAPI _lread(int file, LPVOID buffer, UINT bytes) {
  DWORD got = 0;
  if (!ReadFile(hfile_to_handle(file), buffer, bytes, &got, NULL)) return (UINT)HFILE_ERROR_;
  return got;
}
EXPORT UINT WINAPI _lwrite(int file, LPCSTR buffer, UINT bytes) {
  DWORD put = 0;
  if (!WriteFile(hfile_to_handle(file), buffer, bytes, &put, NULL)) return (UINT)HFILE_ERROR_;
  return put;
}
EXPORT LONG WINAPI _hread(int file, LPVOID buffer, LONG bytes) { return (LONG)_lread(file, buffer, (UINT)bytes); }
EXPORT LONG WINAPI _hwrite(int file, LPCSTR buffer, LONG bytes) { return (LONG)_lwrite(file, buffer, (UINT)bytes); }
EXPORT LONG WINAPI _llseek(int file, LONG offset, int origin) {
  const DWORD r = SetFilePointer(hfile_to_handle(file), offset, NULL, (DWORD)origin);
  return r == INVALID_SET_FILE_POINTER ? HFILE_ERROR_ : (LONG)r;
}
EXPORT int WINAPI _lclose(int file) { return CloseHandle(hfile_to_handle(file)) ? 0 : HFILE_ERROR_; }

/* OpenFile: the OFSTRUCT gets the full path of the file (and the result code of a failed attempt). OF_PARSE only does that, OF_EXIST checks that the file is there. */
EXPORT HFILE WINAPI OpenFile(LPCSTR name, LPOFSTRUCT of, UINT style) {
  if (!of) { set_error(ERROR_INVALID_PARAMETER); return (HFILE)-1; }
  if (style & OF_REOPEN_) name = of->szPathName;
  else if (!name) { of->nErrCode = ERROR_INVALID_PARAMETER; set_error(ERROR_INVALID_PARAMETER); return (HFILE)-1; }
  /* (the structure is only filled in as far as the work goes: a name that cannot be made a full path leaves it as it was) */
  char full[260];
  LPSTR file_part;
  const DWORD n = GetFullPathNameA(name, sizeof(full), full, &file_part);
  if (!n || n >= sizeof(full) || n >= sizeof(of->szPathName)) {
    of->nErrCode = (WORD)(n ? ERROR_INVALID_DATA : GetLastError());
    set_error(of->nErrCode);
    return (HFILE)-1;
  }
  memcpy(of->szPathName, full, n + 1);
  of->nErrCode = 0;
  if (style & OF_PARSE_) { of->cBytes = sizeof(*of); of->fFixedDisk = 1; return 0; }
  if (style & OF_DELETE_) {
    if (!DeleteFileA(of->szPathName)) { of->nErrCode = (WORD)GetLastError(); return (HFILE)-1; }
    of->cBytes = sizeof(*of);
    of->fFixedDisk = 1;
    return 1;
  }
  if (style & OF_EXIST_) {
    const DWORD attributes = GetFileAttributesA(of->szPathName);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) { of->nErrCode = ERROR_FILE_NOT_FOUND; set_error(ERROR_FILE_NOT_FOUND); return (HFILE)-1; }
    of->cBytes = sizeof(*of);
    of->fFixedDisk = 1;
    return 1;
  }
  const int h = (style & OF_CREATE_) ? _lcreat(of->szPathName, 0) : _lopen(of->szPathName, (int)(style & 0x73));
  if (h == HFILE_ERROR_) { of->nErrCode = (WORD)GetLastError(); return (HFILE)-1; }
  of->cBytes = sizeof(*of);
  of->fFixedDisk = 1;
  of->Reserved1 = of->Reserved2 = 0;
  return (HFILE)h;
}
