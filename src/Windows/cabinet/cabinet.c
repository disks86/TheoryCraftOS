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

/* cabinet.dll for TheoryCraftOS: the File Decompression Interface (FDI) - FDICreate, FDIIsCabinet, FDICopy, FDIDestroy - which setup programs expand their
 * cabinets with (Chrome's mini_installer loads this library by its path and expands setup.ex_ with FDICopy).
 *
 * There is no decompressor here. FDICopy reads the whole cabinet into memory through its caller's file functions and asks the kernel, which has the cabinet
 * reader of the system (src/Media/Cabinet.hpp: stored, MSZIP and LZX folders): NtTcosCabinet (src/Process/WindowsCabinet.hpp) describes the files, one fixed
 * record each, and decodes one file a call. The notifications are FDI's: fdintCABINET_INFO once, then for every file fdintCOPY_FILE (0: pass it over, -1:
 * stop, else the handle to write it to with the caller's write function) and, once it is written, fdintCLOSE_FILE_INFO (the caller closes the handle; FALSE
 * or -1 stops). A handle the caller gave that could not be filled is closed with the caller's close function. Every failure is in the ERF of FDICreate.
 *
 * Not here: cabinets that are one of a set (a previous or next cabinet: FDIERROR_CABINET_NOT_FOUND, there is no fdintNEXT_CABINET or fdintPARTIAL_FILE),
 * Quantum compression and decryption (the callback is never called, as on Windows), fdintENUMERATE, FDITruncateCabinet, and all of FCI, the compressing
 * half: FCICreate fails, the rest are stubs (src/Windows/stubs/cabinet.inc), as are the functions of the Compression API (CreateCompressor ...).
 * Built freestanding over kernel32.dll and ntdll.dll with the OS's mingw compiler, for x86-64 and i386 (the FDI functions are cdecl on both). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <fci.h>
#include <fdi.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

__declspec(dllimport) NTSTATUS NTAPI NtTcosCabinet(ULONG64 *arguments);

#define ST_NO_MEMORY ((NTSTATUS)0xC0000017)
#define ST_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)

/* Operations of NtTcosCabinet and the record it describes a file with. */
#define OP_DESCRIBE 1
#define OP_EXTRACT 2
#define RECORD_SIZE 280
#define RECORD_NAME 256
#define RECORD_NAME_CUT 2

/* The cabinet's header (CFHEADER): "MSCF", then little-endian numbers. */
#define HEADER_SIZE 36
#define FLAG_PREVIOUS 1
#define FLAG_NEXT 2
#define FLAG_RESERVE 4
#define MAX_CABINET (512u << 20)      /* what the kernel takes */
#define MAX_PATH_BYTES 1024
#define CHUNK 32768u                  /* the most FDI reads or writes at a time: a data block */
#define SEEK_FROM_START 0             /* the caller's seek function takes the C runtime's SEEK_SET, SEEK_CUR, SEEK_END */
#define SEEK_FROM_END 2

/* The attributes of a file in a cabinet. */
#define ATTR_FILE_MASK 0x27           /* read-only, hidden, system, archive */
#define ATTR_EXECUTE 0x40             /* "run after extraction" */

#define FDI_MAGIC 0x49444654u         /* 'TFDI' */

typedef struct {
  DWORD magic;
  PFNALLOC alloc;
  PFNFREE free;
  PFNOPEN open;
  PFNREAD read;
  PFNWRITE write;
  PFNCLOSE close;
  PFNSEEK seek;
  PERF erf;
} Fdi;

typedef struct {
  DWORD cabinet_size;
  WORD folders, files, flags, set_id, cabinet_index;
} Header;

static void xmemcpy(void *d, const void *s, SIZE_T n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; }
static void xmemset(void *d, int v, SIZE_T n) { BYTE *p = d; while (n--) *p++ = (BYTE)v; }
static SIZE_T xstrlen(const char *s) { SIZE_T n = 0; while (s[n]) ++n; return n; }
static WORD le16(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD le32(const BYTE *p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

/* The cabinet and what the kernel made of it are kept on the process heap: they may be large, and a caller's allocator is made for FDI's own small needs. */
static void *heap_alloc(SIZE_T size) { return HeapAlloc(GetProcessHeap(), 0, size ? size : 1); }
static void heap_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static Fdi *fdi_of(HFDI handle) {
  Fdi *fdi = (Fdi *)handle;
  if (!fdi || fdi->magic != FDI_MAGIC) {
    SetLastError(ERROR_INVALID_HANDLE);
    return NULL;
  }
  return fdi;
}

static void set_error(Fdi *fdi, int operation, int type) {
  if (fdi->erf) {
    fdi->erf->erfOper = operation;
    fdi->erf->erfType = type;
    fdi->erf->fError = TRUE;
  }
  if (type) SetLastError((DWORD)type);
}

/* Reads `size` bytes of the caller's file, in pieces. */
static BOOL read_all(Fdi *fdi, INT_PTR file, BYTE *out, DWORD size) {
  DWORD done = 0;
  while (done < size) {
    const UINT want = size - done < CHUNK ? size - done : CHUNK;
    const UINT got = fdi->read(file, out + done, want);
    if (got == 0 || got > want) return FALSE;      /* (the end of the file, or (UINT)-1: an error) */
    done += got;
  }
  return TRUE;
}

/* Reads the header of the cabinet at the file's position. An FDIERROR_*; FDIERROR_NONE when it is one. */
static int read_header(Fdi *fdi, INT_PTR file, Header *header) {
  BYTE raw[HEADER_SIZE];
  if (!read_all(fdi, file, raw, sizeof raw) || raw[0] != 'M' || raw[1] != 'S' || raw[2] != 'C' || raw[3] != 'F') return FDIERROR_NOT_A_CABINET;
  if (raw[24] != 3 || raw[25] != 1) return FDIERROR_UNKNOWN_CABINET_VERSION;      /* the format's version: 1.3 */
  header->cabinet_size = le32(raw + 8);
  header->folders = le16(raw + 26);
  header->files = le16(raw + 28);
  header->flags = le16(raw + 30);
  header->set_id = le16(raw + 32);
  header->cabinet_index = le16(raw + 34);
  return FDIERROR_NONE;
}

EXPORT HFDI DIAMONDAPI FDICreate(PFNALLOC alloc, PFNFREE free, PFNOPEN open, PFNREAD read, PFNWRITE write, PFNCLOSE close, PFNSEEK seek, int cpu, PERF erf) {
  (void)cpu;      /* (which of the 16-bit decompressors to use: nothing since Windows 95) */
  if (erf) {
    erf->erfOper = FDIERROR_NONE;
    erf->erfType = 0;
    erf->fError = FALSE;
  }
  if (!alloc || !free || !open || !read || !write || !close || !seek || !erf) {
    if (erf) {
      erf->erfType = ERROR_BAD_ARGUMENTS;
      erf->fError = TRUE;
    }
    SetLastError(ERROR_BAD_ARGUMENTS);
    return NULL;
  }
  Fdi *fdi = alloc(sizeof(Fdi));
  if (!fdi) {
    erf->erfOper = FDIERROR_ALLOC_FAIL;
    erf->fError = TRUE;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
  }
  fdi->magic = FDI_MAGIC;
  fdi->alloc = alloc;
  fdi->free = free;
  fdi->open = open;
  fdi->read = read;
  fdi->write = write;
  fdi->close = close;
  fdi->seek = seek;
  fdi->erf = erf;
  return fdi;
}

EXPORT BOOL DIAMONDAPI FDIDestroy(HFDI handle) {
  Fdi *fdi = fdi_of(handle);
  if (!fdi) return FALSE;
  fdi->magic = 0;
  fdi->free(fdi);
  return TRUE;
}

/* Whether the file, from where it stands, is a cabinet; what its header says. The file is left where the header ends. */
EXPORT BOOL DIAMONDAPI FDIIsCabinet(HFDI handle, INT_PTR file, PFDICABINETINFO info) {
  Fdi *fdi = fdi_of(handle);
  if (!fdi) return FALSE;
  if (!info) {
    SetLastError(ERROR_BAD_ARGUMENTS);
    return FALSE;
  }
  Header header;
  const int error = read_header(fdi, file, &header);
  if (error != FDIERROR_NONE) {
    set_error(fdi, error, 0);
    return FALSE;
  }
  info->cbCabinet = (LONG)header.cabinet_size;
  info->cFolders = header.folders;
  info->cFiles = header.files;
  info->setID = header.set_id;
  info->iCabinet = header.cabinet_index;
  info->fReserve = (header.flags & FLAG_RESERVE) != 0;
  info->hasprev = (header.flags & FLAG_PREVIOUS) != 0;
  info->hasnext = (header.flags & FLAG_NEXT) != 0;
  return TRUE;
}

/* Opens the cabinet with the caller's functions and reads it whole into a block of the process heap (*out, *size). An FDIERROR_*; *type is the Win32
 * error that goes with it, if one does. */
static int load_cabinet(Fdi *fdi, char *name, char *folder, Header *header, BYTE **out, DWORD *size, int *type) {
  const SIZE_T folder_length = folder ? xstrlen(folder) : 0, name_length = name ? xstrlen(name) : 0;
  *type = ERROR_FILE_NOT_FOUND;
  if (name_length == 0 || folder_length >= MAX_PATH_BYTES || name_length >= MAX_PATH_BYTES - folder_length) return FDIERROR_CABINET_NOT_FOUND;
  char *path = fdi->alloc((ULONG)(folder_length + name_length + 1));
  if (!path) {
    *type = ERROR_NOT_ENOUGH_MEMORY;
    return FDIERROR_ALLOC_FAIL;
  }
  if (folder_length) xmemcpy(path, folder, folder_length);
  xmemcpy(path + folder_length, name, name_length + 1);
  const INT_PTR file = fdi->open(path, 0x8000 /* _O_RDONLY | _O_BINARY */, 0x180 /* _S_IREAD | _S_IWRITE */);
  fdi->free(path);
  if (file == -1) return FDIERROR_CABINET_NOT_FOUND;

  *type = 0;
  int error = read_header(fdi, file, header);
  BYTE *cabinet = NULL;
  if (error == FDIERROR_NONE) {
    /* The header says how long the cabinet is; the file has to have that much (it may have more: something appended, a signature). */
    const LONG length = fdi->seek(file, 0, SEEK_FROM_END);
    if (header->cabinet_size < HEADER_SIZE || header->cabinet_size > MAX_CABINET || length < 0 || (DWORD)length < header->cabinet_size ||
        fdi->seek(file, 0, SEEK_FROM_START) != 0) {
      error = FDIERROR_CORRUPT_CABINET;
    } else if (!(cabinet = heap_alloc(header->cabinet_size))) {
      error = FDIERROR_ALLOC_FAIL;
      *type = ERROR_NOT_ENOUGH_MEMORY;
    } else if (!read_all(fdi, file, cabinet, header->cabinet_size)) {
      error = FDIERROR_CORRUPT_CABINET;
    }
  }
  fdi->close(file);
  if (error != FDIERROR_NONE) {
    heap_free(cabinet);
    return error;
  }
  *out = cabinet;
  *size = header->cabinet_size;
  return FDIERROR_NONE;
}

static int error_of_status(NTSTATUS status) { return status == ST_NO_MEMORY ? FDIERROR_ALLOC_FAIL : FDIERROR_CORRUPT_CABINET; }

EXPORT BOOL DIAMONDAPI FDICopy(HFDI handle, char *name, char *folder, int flags, PFNFDINOTIFY notify, PFNFDIDECRYPT decrypt, void *user) {
  (void)flags;
  (void)decrypt;
  Fdi *fdi = fdi_of(handle);
  if (!fdi) return FALSE;
  if (!notify) {
    set_error(fdi, FDIERROR_NONE, ERROR_BAD_ARGUMENTS);
    return FALSE;
  }
  Header header;
  BYTE *cabinet = NULL, *records = NULL, *data = NULL;
  DWORD cabinet_size = 0;
  int type = 0;
  int error = load_cabinet(fdi, name, folder, &header, &cabinet, &cabinet_size, &type);
  if (error != FDIERROR_NONE) {
    set_error(fdi, error, type);
    return FALSE;
  }
  INT_PTR target = 0;      /* the handle the caller gave for the file being written */
  BOOL target_open = FALSE;
  ULONG64 w[8];
  DWORD files = 0;

  /* One of a set: its files may go on in the next cabinet or come from the one before, which is not read here. */
  if (header.flags & (FLAG_PREVIOUS | FLAG_NEXT)) { error = FDIERROR_CABINET_NOT_FOUND; goto done; }

  records = heap_alloc((SIZE_T)header.files * RECORD_SIZE);
  if (!records) { error = FDIERROR_ALLOC_FAIL; type = ERROR_NOT_ENOUGH_MEMORY; goto done; }
  xmemset(w, 0, sizeof w);
  w[0] = OP_DESCRIBE;
  w[1] = (ULONG64)(ULONG_PTR)cabinet;
  w[2] = cabinet_size;
  w[3] = (ULONG64)(ULONG_PTR)records;
  w[4] = (ULONG64)header.files * RECORD_SIZE;
  NTSTATUS status = NtTcosCabinet(w);
  if (status != 0 || w[5] > header.files || w[6] != RECORD_SIZE) { error = status == 0 ? FDIERROR_CORRUPT_CABINET : error_of_status(status); goto done; }
  files = (DWORD)w[5];

  FDINOTIFICATION n;
  char empty[1] = {0};
  xmemset(&n, 0, sizeof n);
  n.psz1 = empty;      /* the next cabinet's name and the next disk's: there is none */
  n.psz2 = empty;
  n.psz3 = folder;
  n.pv = user;
  n.setID = header.set_id;
  n.iCabinet = header.cabinet_index;
  n.fdie = FDIERROR_NONE;
  if (notify(fdintCABINET_INFO, &n) == -1) { error = FDIERROR_USER_ABORT; goto done; }

  for (DWORD i = 0; i < files; ++i) {
    const BYTE *record = records + (SIZE_T)i * RECORD_SIZE;
    const ULONG64 size = (ULONG64)le32(record) | (ULONG64)le32(record + 4) << 32;
    const DWORD name_length = le32(record + 8), record_flags = le32(record + 20);
    const WORD date = le16(record + 12), time = le16(record + 14), attributes = le16(record + 16), in_folder = le16(record + 18);
    char file_name[RECORD_NAME + 1];
    if (name_length > RECORD_NAME || (record_flags & RECORD_NAME_CUT) || size > 0x7FFFFFFFu) { error = FDIERROR_CORRUPT_CABINET; goto done; }
    xmemcpy(file_name, record + 24, name_length);
    file_name[name_length] = 0;

    xmemset(&n, 0, sizeof n);
    n.cb = (LONG)size;
    n.psz1 = file_name;
    n.pv = user;
    n.date = date;
    n.time = time;
    n.attribs = attributes;
    n.iFolder = in_folder;
    target = notify(fdintCOPY_FILE, &n);
    if (target == 0) continue;      /* not wanted */
    if (target == -1) { error = FDIERROR_USER_ABORT; goto done; }
    target_open = TRUE;

    /* The file, decoded by the kernel (its folder from the start, every time: see src/Media/CabinetService.hpp). */
    if (size != 0) {
      data = heap_alloc((SIZE_T)size);
      if (!data) { error = FDIERROR_ALLOC_FAIL; type = ERROR_NOT_ENOUGH_MEMORY; goto done; }
      xmemset(w, 0, sizeof w);
      w[0] = OP_EXTRACT;
      w[1] = (ULONG64)(ULONG_PTR)cabinet;
      w[2] = cabinet_size;
      w[3] = i;
      w[4] = (ULONG64)(ULONG_PTR)data;
      w[5] = size;
      status = NtTcosCabinet(w);
      if (status != 0 || w[6] != size) { error = status == 0 || status == ST_BUFFER_TOO_SMALL ? FDIERROR_CORRUPT_CABINET : error_of_status(status); goto done; }
      for (DWORD written = 0; written < size;) {
        const UINT piece = size - written < CHUNK ? (UINT)(size - written) : CHUNK;
        if (fdi->write(target, data + written, piece) != piece) { error = FDIERROR_TARGET_FILE; goto done; }
        written += piece;
      }
      heap_free(data);
      data = NULL;
    }

    xmemset(&n, 0, sizeof n);
    n.cb = (attributes & ATTR_EXECUTE) != 0;      /* whether the file asks to be run */
    n.psz1 = file_name;
    n.pv = user;
    n.hf = target;
    n.date = date;
    n.time = time;
    n.attribs = attributes & ATTR_FILE_MASK;
    n.iFolder = in_folder;
    target_open = FALSE;      /* (the caller closes it in the notification) */
    const INT_PTR closed = notify(fdintCLOSE_FILE_INFO, &n);
    if (closed == 0 || closed == -1) { error = FDIERROR_USER_ABORT; goto done; }
  }

done:
  if (target_open) fdi->close(target);
  heap_free(data);
  heap_free(records);
  heap_free(cabinet);
  if (error != FDIERROR_NONE) {
    set_error(fdi, error, type);
    return FALSE;
  }
  return TRUE;
}

/* Cuts a cabinet off after a folder. Not done (and not by Wine either). */
EXPORT BOOL DIAMONDAPI FDITruncateCabinet(HFDI handle, char *name, USHORT folder) {
  (void)name;
  (void)folder;
  if (!fdi_of(handle)) return FALSE;
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return FALSE;
}

/* The compressing half is not here: no context is made, and the error says so. (FCIAddFile and the others, which need one, are stubs that fail.) */
EXPORT HFCI DIAMONDAPI FCICreate(PERF erf, PFNFCIFILEPLACED placed, PFNFCIALLOC alloc, PFNFCIFREE free, PFNFCIOPEN open, PFNFCIREAD read, PFNFCIWRITE write,
                                 PFNFCICLOSE close, PFNFCISEEK seek, PFNFCIDELETE delete_, PFNFCIGETTEMPFILE temp, PCCAB cab, void *user) {
  (void)placed; (void)alloc; (void)free; (void)open; (void)read; (void)write; (void)close; (void)seek; (void)delete_; (void)temp; (void)cab; (void)user;
  if (erf) {
    erf->erfOper = FCIERR_NONE;
    erf->erfType = ERROR_CALL_NOT_IMPLEMENTED;
    erf->fError = TRUE;
  }
  SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  return NULL;
}

/* Extract (what advpack.dll and urlmon.dll take whole cabinets apart with): not done. */
EXPORT HRESULT WINAPI Extract(void *session, LPCSTR cabinet) {
  (void)session;
  (void)cabinet;
  return E_FAIL;
}

typedef struct { DWORD cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID; } CabinetVersionInfo;      /* DLLVERSIONINFO */

EXPORT HRESULT WINAPI DllGetVersion(CabinetVersionInfo *info) {
  if (!info || info->cbSize < sizeof *info) return E_INVALIDARG;
  info->dwMajorVersion = 10;
  info->dwMinorVersion = 0;
  info->dwBuildNumber = 19041;
  info->dwPlatformID = 2;      /* DLLVER_PLATFORM_NT */
  return S_OK;
}

EXPORT LPCSTR WINAPI GetDllVersion(void) { return "10.0.19041.1"; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) { (void)module; (void)reason; (void)reserved; return TRUE; }
