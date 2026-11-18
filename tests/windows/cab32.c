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

/* cabinet.dll's FDI (src/Windows/cabinet), for tests/wow64_qemu.sh: as cab32, a 32-bit program, and built for x86-64 as well (cab64). It expands cabinets
 * the way Chrome's mini_installer expands its setup.ex_: the library is loaded by its path under %WINDIR%\system32, FDICreate, FDICopy and FDIDestroy are
 * found by name, the file functions are the program's own over CreateFile, ReadFile and WriteFile, and the notification writes each file. The cabinets are
 * three of the cabinet reader's fixtures (tests/data/cabinet: stored, MSZIP and LZX folders), which the script turns into C arrays with what
 * manifest.txt says every file must come out as (cabfixtures.h). Then what must fail: a file that is no cabinet, cabinets cut short, a cabinet that is not
 * there, and a caller that passes files over or stops. Ends with exit code 42 if every check passed.
 *   i686-w64-mingw32-gcc -O1 -I <folder of cabfixtures.h> -o cab32.exe tests/windows/cab32.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <fcntl.h>
#include <fci.h>
#include <fdi.h>
#include "cabfixtures.h"      /* CabFile, CabFixture, cab_fixtures[], CAB_FIXTURES */

#ifdef _WIN64
#define NAME "cab64"
#else
#define NAME "cab32"
#endif

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? NAME ": ok: %s\n" : NAME ": FAILED: %s\n", what);
  if (ok) ++g_passed; else ++g_failed;
}

static unsigned crc32_of(const unsigned char *data, size_t size) {
  unsigned crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

/* ---- the file functions FDI is given: those of chrome/installer/mini_installer/decompress.cc, counted ---- */
static int g_allocations, g_open_files, g_largest_write, g_closed_by_fdi;

static FNALLOC(cab_alloc) { ++g_allocations; return HeapAlloc(GetProcessHeap(), 0, cb); }
static FNFREE(cab_free) { --g_allocations; HeapFree(GetProcessHeap(), 0, pv); }
static FNOPEN(cab_open) {
  (void)pmode;
  const DWORD access = (oflag & _O_RDWR) ? GENERIC_READ | GENERIC_WRITE : (oflag & _O_WRONLY) ? GENERIC_WRITE : GENERIC_READ;
  WCHAR path[MAX_PATH];
  if (!MultiByteToWideChar(CP_UTF8, 0, pszFile, -1, path, MAX_PATH)) return -1;
  HANDLE file = CreateFileW(path, access, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, (oflag & _O_CREAT) ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file != INVALID_HANDLE_VALUE) ++g_open_files;
  return (INT_PTR)file;
}
static FNREAD(cab_read) {
  DWORD read = 0;
  if (!ReadFile((HANDLE)hf, pv, cb, &read, NULL)) read = (DWORD)-1;
  return read;
}
static FNWRITE(cab_write) {
  DWORD written = 0;
  if ((int)cb > g_largest_write) g_largest_write = (int)cb;
  if (!WriteFile((HANDLE)hf, pv, cb, &written, NULL)) written = (DWORD)-1;
  return written;
}
static FNCLOSE(cab_close) {
  --g_open_files;
  ++g_closed_by_fdi;
  return CloseHandle((HANDLE)hf) ? 0 : -1;
}
static FNSEEK(cab_seek) { return (long)SetFilePointer((HANDLE)hf, dist, NULL, seektype); }

/* ---- the notification ---- */
enum { TAKE_ALL, TAKE_NONE, TAKE_EVEN, STOP_AT_SECOND, STOP_AFTER_FIRST, STOP_AT_INFO, FAIL_CLOSE };

typedef struct {
  const CabFixture *fixture;
  int mode;
  char folder[MAX_PATH];      /* where the cabinets are and the files go, with its '\' */
  int infos, offered, written, good, wrong;      /* notifications seen; files that came out right; anything that was not as the manifest has it */
  int next;                   /* which file of the fixture comes next */
  int current;                /* the file being written */
  HANDLE target;
} Expand;

static void output_path(const Expand *e, char *path) { snprintf(path, MAX_PATH, "%scabtest.out", e->folder); }

static unsigned char *read_whole(const char *path, DWORD *size) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) return NULL;
  *size = GetFileSize(file, NULL);
  unsigned char *data = malloc(*size + 1);
  DWORD read = 0;
  if (data && (!ReadFile(file, data, *size, &read, NULL) || read != *size)) { free(data); data = NULL; }
  CloseHandle(file);
  return data;
}

static FNFDINOTIFY(cab_notify) {
  Expand *e = pfdin->pv;
  char path[MAX_PATH];
  switch (fdint) {
    case fdintCABINET_INFO:
      ++e->infos;
      if (!pfdin->psz1 || pfdin->psz1[0] || !pfdin->psz2 || pfdin->psz2[0] || !pfdin->psz3 || strcmp(pfdin->psz3, e->folder) != 0 ||
          pfdin->setID != e->fixture->set_id || pfdin->iCabinet != 0 || pfdin->fdie != FDIERROR_NONE || e->offered != 0)
        ++e->wrong;
      return e->mode == STOP_AT_INFO ? -1 : 0;
    case fdintCOPY_FILE: {
      const int index = e->next++;
      ++e->offered;
      if (index >= e->fixture->count) { ++e->wrong; return -1; }
      const CabFile *f = &e->fixture->files[index];
      if (!pfdin->psz1 || strcmp(pfdin->psz1, f->name) != 0 || pfdin->cb != (long)f->size || pfdin->attribs != f->attributes || pfdin->iFolder != f->folder ||
          pfdin->date == 0)
        ++e->wrong;
      if (e->mode == TAKE_NONE || (e->mode == TAKE_EVEN && (index & 1))) return 0;
      if (e->mode == STOP_AT_SECOND && index == 1) return -1;
      output_path(e, path);
      e->target = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      e->current = index;
      if (e->target != INVALID_HANDLE_VALUE) ++g_open_files;
      return (INT_PTR)e->target;      /* (as decompress.cc: CreateFile's failure is fdintCOPY_FILE's) */
    }
    case fdintCLOSE_FILE_INFO: {
      const CabFile *f = &e->fixture->files[e->current];
      if ((HANDLE)pfdin->hf != e->target || !pfdin->psz1 || strcmp(pfdin->psz1, f->name) != 0 || pfdin->cb != 0 ||
          pfdin->attribs != (f->attributes & 0x27) || pfdin->iFolder != f->folder)
        ++e->wrong;
      /* what mini_installer does with the date and the attributes */
      FILETIME local, utc;
      if (!DosDateTimeToFileTime(pfdin->date, pfdin->time, &local) || !LocalFileTimeToFileTime(&local, &utc)) ++e->wrong;
      CloseHandle((HANDLE)pfdin->hf);
      --g_open_files;
      e->target = INVALID_HANDLE_VALUE;
      ++e->written;
      DWORD size = 0;
      output_path(e, path);
      unsigned char *data = read_whole(path, &size);
      if (data && size == f->size && crc32_of(data, size) == f->crc) ++e->good; else ++e->wrong;
      free(data);
      if (e->mode == FAIL_CLOSE) return FALSE;
      return e->mode == STOP_AFTER_FIRST ? -1 : TRUE;      /* (-1: how mini_installer ends once its one file is out) */
    }
    default:      /* fdintPARTIAL_FILE, fdintNEXT_CABINET, fdintENUMERATE: nothing here is one of a set, and none is expected */
      ++e->wrong;
      return -1;
  }
}

typedef HFDI (DIAMONDAPI *FDICreateFn)(PFNALLOC, PFNFREE, PFNOPEN, PFNREAD, PFNWRITE, PFNCLOSE, PFNSEEK, int, PERF);
typedef BOOL (DIAMONDAPI *FDIDestroyFn)(HFDI);
typedef BOOL (DIAMONDAPI *FDICopyFn)(HFDI, char *, char *, int, PFNFDINOTIFY, PFNFDIDECRYPT, void *);
typedef BOOL (DIAMONDAPI *FDIIsCabinetFn)(HFDI, INT_PTR, PFDICABINETINFO);
typedef BOOL (DIAMONDAPI *FDITruncateCabinetFn)(HFDI, char *, USHORT);
typedef HFCI (DIAMONDAPI *FCICreateFn)(PERF, void *, void *, void *, void *, void *, void *, void *, void *, void *, void *, void *, void *);

static FDICopyFn g_copy;

static BOOL write_file(const char *path, const unsigned char *data, DWORD size) {
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data, size, &written, NULL) && written == size;
  CloseHandle(file);
  return ok;
}

/* FDICopy of `name` in the folder, with what the notification is to do. */
static BOOL expand(HFDI fdi, Expand *e, const CabFixture *fixture, const char *folder, const char *name, int mode) {
  char cabinet[MAX_PATH], path[MAX_PATH];
  memset(e, 0, sizeof *e);
  e->fixture = fixture;
  e->mode = mode;
  e->target = INVALID_HANDLE_VALUE;
  strcpy(e->folder, folder);
  strcpy(cabinet, name);
  strcpy(path, folder);
  return g_copy(fdi, cabinet, path, 0, cab_notify, NULL, e);
}

int main(void) {
  char what[256];
  printf(NAME ": started\n");

  /* The library, found as mini_installer finds it. */
  WCHAR library[MAX_PATH] = {0};
  const DWORD expanded = ExpandEnvironmentStringsW(L"%WINDIR%\\system32\\cabinet.dll", library, MAX_PATH);
  HMODULE module = expanded > 0 && expanded <= MAX_PATH ? LoadLibraryExW(library, NULL, LOAD_WITH_ALTERED_SEARCH_PATH) : NULL;
  check(module != NULL, "LoadLibraryEx of %WINDIR%\\system32\\cabinet.dll");
  FDICreateFn create = module ? (FDICreateFn)(void *)GetProcAddress(module, "FDICreate") : NULL;
  FDIDestroyFn destroy = module ? (FDIDestroyFn)(void *)GetProcAddress(module, "FDIDestroy") : NULL;
  FDIIsCabinetFn is_cabinet = module ? (FDIIsCabinetFn)(void *)GetProcAddress(module, "FDIIsCabinet") : NULL;
  FDITruncateCabinetFn truncate = module ? (FDITruncateCabinetFn)(void *)GetProcAddress(module, "FDITruncateCabinet") : NULL;
  FCICreateFn fci_create = module ? (FCICreateFn)(void *)GetProcAddress(module, "FCICreate") : NULL;
  g_copy = module ? (FDICopyFn)(void *)GetProcAddress(module, "FDICopy") : NULL;
  check(create && destroy && g_copy && is_cabinet && truncate, "FDICreate, FDICopy, FDIDestroy, FDIIsCabinet and FDITruncateCabinet by name");
  check(fci_create && GetProcAddress(module, "FCIAddFile") && GetProcAddress(module, "FCIDestroy") && GetProcAddress(module, "Extract") &&
        GetProcAddress(module, "DllGetVersion") && GetProcAddress(module, (LPCSTR)(UINT_PTR)22) == (FARPROC)(void *)g_copy,
        "the FCI functions and Extract are exports too, and FDICopy is ordinal 22");
  if (!create || !destroy || !g_copy || !is_cabinet || !truncate || !fci_create) {
    printf(NAME ": %d checks passed, some FAILED\n", g_passed);
    return 1;
  }

  char folder[MAX_PATH], path[MAX_PATH];
  const DWORD folder_length = GetTempPathA(MAX_PATH - 40, folder);
  check(folder_length > 0 && folder[folder_length - 1] == '\\', "a folder for temporary files");
  for (int i = 0; i < CAB_FIXTURES; ++i) {
    snprintf(path, sizeof path, "%s%s", folder, cab_fixtures[i].name);
    snprintf(what, sizeof what, "%s written (%u bytes)", cab_fixtures[i].name, cab_fixtures[i].size);
    check(write_file(path, cab_fixtures[i].data, cab_fixtures[i].size), what);
  }

  ERF erf;
  memset(&erf, 0xEE, sizeof erf);
  check(create(cab_alloc, cab_free, cab_open, cab_read, NULL, cab_close, cab_seek, cpuUNKNOWN, &erf) == NULL && erf.fError, "FDICreate without a write function fails");
  memset(&erf, 0xEE, sizeof erf);
  HFDI fdi = create(cab_alloc, cab_free, cab_open, cab_read, cab_write, cab_close, cab_seek, cpuUNKNOWN, &erf);
  check(fdi != NULL && erf.erfOper == FDIERROR_NONE && erf.erfType == 0 && !erf.fError && g_allocations == 1, "FDICreate: a context from the caller's allocator, no error");
  if (!fdi) {
    printf(NAME ": %d checks passed, some FAILED\n", g_passed);
    return 1;
  }

  /* Every file of every cabinet. */
  Expand e;
  for (int i = 0; i < CAB_FIXTURES; ++i) {
    const CabFixture *fixture = &cab_fixtures[i];
    g_largest_write = 0;
    const BOOL ok = expand(fdi, &e, fixture, folder, fixture->name, TAKE_ALL);
    snprintf(what, sizeof what, "FDICopy of %s: %d of %d files as the manifest has them (size, CRC-32, name, attributes, folder)", fixture->name, e.good, fixture->count);
    check(ok && !erf.fError && e.infos == 1 && e.offered == fixture->count && e.written == fixture->count && e.good == fixture->count && e.wrong == 0, what);
    check(g_open_files == 0 && g_allocations == 1 && g_largest_write <= 32768, "its files are closed, its memory freed, and no write is larger than a data block");

    FDICABINETINFO info;
    memset(&info, 0xEE, sizeof info);
    snprintf(path, sizeof path, "%s%s", folder, fixture->name);
    const INT_PTR file = cab_open(path, _O_RDONLY | _O_BINARY, 0);
    const BOOL is = file != -1 && is_cabinet(fdi, file, &info);
    snprintf(what, sizeof what, "FDIIsCabinet of %s: %d bytes, %d folders, %d files, set %d", fixture->name, (int)info.cbCabinet, info.cFolders, info.cFiles, info.setID);
    check(is && info.cbCabinet == (long)fixture->size && info.cFolders == fixture->folders && info.cFiles == fixture->count && info.setID == fixture->set_id &&
          info.iCabinet == 0 && info.fReserve == fixture->reserve && !info.hasprev && !info.hasnext, what);
    if (file != -1) { CloseHandle((HANDLE)file); --g_open_files; }
  }

  /* A caller that passes files over, or stops. (The first fixture has several files.) */
  const CabFixture *first = &cab_fixtures[0];
  BOOL ok = expand(fdi, &e, first, folder, first->name, TAKE_NONE);
  check(ok && !erf.fError && e.offered == first->count && e.written == 0 && e.wrong == 0, "fdintCOPY_FILE answered 0: every file is offered, none written");
  ok = expand(fdi, &e, first, folder, first->name, TAKE_EVEN);
  check(ok && e.offered == first->count && e.written == (first->count + 1) / 2 && e.good == e.written && e.wrong == 0, "every other file passed over: the rest come out right");
  ok = expand(fdi, &e, first, folder, first->name, STOP_AT_SECOND);
  check(!ok && erf.fError && erf.erfOper == FDIERROR_USER_ABORT && e.offered == 2 && e.written == 1 && e.good == 1 && e.wrong == 0 && g_open_files == 0,
        "fdintCOPY_FILE answered -1 at the second file: FDIERROR_USER_ABORT, the first file is out");
  ok = expand(fdi, &e, first, folder, first->name, STOP_AFTER_FIRST);
  check(!ok && erf.erfOper == FDIERROR_USER_ABORT && e.offered == 1 && e.written == 1 && e.good == 1 && e.wrong == 0 && g_open_files == 0,
        "fdintCLOSE_FILE_INFO answered -1 (mini_installer's way to end): the one file is out, nothing more is offered");
  ok = expand(fdi, &e, first, folder, first->name, FAIL_CLOSE);
  check(!ok && erf.erfOper == FDIERROR_USER_ABORT && e.offered == 1 && e.written == 1 && e.wrong == 0, "fdintCLOSE_FILE_INFO answered FALSE: FDIERROR_USER_ABORT");
  ok = expand(fdi, &e, first, folder, first->name, STOP_AT_INFO);
  check(!ok && erf.erfOper == FDIERROR_USER_ABORT && e.infos == 1 && e.offered == 0 && e.wrong == 0 && g_open_files == 0, "fdintCABINET_INFO answered -1: no file is offered");
  /* and the context still works after its errors */
  ok = expand(fdi, &e, first, folder, first->name, TAKE_ALL);
  check(ok && e.good == first->count && e.wrong == 0, "the same context expands the cabinet again");

  /* What is not a cabinet. */
  static unsigned char junk[5000];
  for (unsigned i = 0; i < sizeof junk; ++i) junk[i] = (unsigned char)(i * 7 + 3);
  snprintf(path, sizeof path, "%scabtest_junk.cab", folder);
  write_file(path, junk, sizeof junk);
  memset(&erf, 0, sizeof erf);
  ok = expand(fdi, &e, first, folder, "cabtest_junk.cab", TAKE_ALL);
  check(!ok && erf.fError && erf.erfOper == FDIERROR_NOT_A_CABINET && e.infos == 0 && e.offered == 0 && g_open_files == 0, "a file that is no cabinet: FDIERROR_NOT_A_CABINET");
  {
    FDICABINETINFO info;
    const INT_PTR file = cab_open(path, _O_RDONLY | _O_BINARY, 0);
    check(file != -1 && !is_cabinet(fdi, file, &info), "FDIIsCabinet says no to it");
    if (file != -1) { CloseHandle((HANDLE)file); --g_open_files; }
  }
  write_file(path, junk, 10);
  memset(&erf, 0, sizeof erf);
  ok = expand(fdi, &e, first, folder, "cabtest_junk.cab", TAKE_ALL);
  check(!ok && erf.fError && erf.erfOper == FDIERROR_NOT_A_CABINET && g_open_files == 0, "nor are ten bytes");
  write_file(path, junk, 0);
  ok = expand(fdi, &e, first, folder, "cabtest_junk.cab", TAKE_ALL);
  check(!ok && erf.erfOper == FDIERROR_NOT_A_CABINET && g_open_files == 0, "nor is an empty file");

  /* Cabinets cut short: every fixture at half its length, as it is (the header says more than the file has) and with the header saying the short length (so
   * that the directory reads and the data does not). */
  for (int i = 0; i < CAB_FIXTURES; ++i) {
    const CabFixture *fixture = &cab_fixtures[i];
    const DWORD half = fixture->size / 2;
    unsigned char *cut = malloc(half);
    memcpy(cut, fixture->data, half);
    snprintf(path, sizeof path, "%scabtest_cut.cab", folder);
    write_file(path, cut, half);
    memset(&erf, 0, sizeof erf);
    ok = expand(fdi, &e, fixture, folder, "cabtest_cut.cab", TAKE_ALL);
    snprintf(what, sizeof what, "%s cut to half: FDICopy fails with FDIERROR_CORRUPT_CABINET, nothing is offered", fixture->name);
    check(!ok && erf.fError && erf.erfOper == FDIERROR_CORRUPT_CABINET && e.offered == 0 && g_open_files == 0 && g_allocations == 1, what);
    cut[8] = (unsigned char)half; cut[9] = (unsigned char)(half >> 8); cut[10] = (unsigned char)(half >> 16); cut[11] = (unsigned char)(half >> 24);
    write_file(path, cut, half);
    memset(&erf, 0, sizeof erf);
    g_closed_by_fdi = 0;
    ok = expand(fdi, &e, fixture, folder, "cabtest_cut.cab", TAKE_ALL);
    snprintf(what, sizeof what, "%s cut to half, the header agreeing: fails with FDIERROR_CORRUPT_CABINET after %d good files, and FDI closes the file it could not fill",
             fixture->name, e.good);
    /* (the cabinet's own handle and the one fdintCOPY_FILE gave for the file that does not decode: two closes) */
    check(!ok && erf.fError && erf.erfOper == FDIERROR_CORRUPT_CABINET && e.good == e.written && e.written < fixture->count && e.wrong == 0 &&
          (e.offered == 0 || g_closed_by_fdi == 2) && g_open_files == 0 && g_allocations == 1, what);
    free(cut);
  }

  /* A cabinet that is not there. */
  memset(&erf, 0, sizeof erf);
  ok = expand(fdi, &e, first, folder, "cabtest_missing.cab", TAKE_ALL);
  check(!ok && erf.fError && erf.erfOper == FDIERROR_CABINET_NOT_FOUND && e.infos == 0 && g_open_files == 0, "a cabinet that is not there: FDIERROR_CABINET_NOT_FOUND");
  ok = expand(fdi, &e, first, "Z:\\no\\such\\folder\\", first->name, TAKE_ALL);
  check(!ok && erf.erfOper == FDIERROR_CABINET_NOT_FOUND, "nor in a folder that is not there");

  /* What is not done fails and says so. */
  check(!truncate(fdi, (char *)first->name, 0), "FDITruncateCabinet fails");
  ERF fci_erf;
  memset(&fci_erf, 0, sizeof fci_erf);
  check(fci_create(&fci_erf, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) == NULL && fci_erf.fError, "FCICreate fails, with an error");

  check(!destroy(NULL) && !g_copy(NULL, (char *)first->name, folder, 0, cab_notify, NULL, &e), "FDIDestroy and FDICopy refuse a context that is none");
  check(destroy(fdi) && g_allocations == 0 && g_open_files == 0, "FDIDestroy: everything the caller's allocator gave is back, every file closed");

  for (int i = 0; i < CAB_FIXTURES; ++i) {
    snprintf(path, sizeof path, "%s%s", folder, cab_fixtures[i].name);
    DeleteFileA(path);
  }
  snprintf(path, sizeof path, "%scabtest_junk.cab", folder); DeleteFileA(path);
  snprintf(path, sizeof path, "%scabtest_cut.cab", folder); DeleteFileA(path);
  snprintf(path, sizeof path, "%scabtest.out", folder); DeleteFileA(path);

  printf(g_failed == 0 ? NAME ": all %d checks passed\n" : NAME ": %d checks passed, some FAILED\n", g_passed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
