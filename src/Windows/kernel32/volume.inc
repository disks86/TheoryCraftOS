/* TheoryCraftOS - a minimal UEFI x86_64 operating system.
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

/* Volumes and DOS devices of kernel32.dll (included by kernel32.c): the table of DOS device names (C:, NUL, ... and the ones DefineDosDevice adds), QueryDosDevice,
 * DefineDosDevice, the logical drives, the volume enumeration (FindFirstVolume), the volume names and the volume paths, GetVolumeInformation[ByHandle] and
 * GetDiskSpaceInformation. There is one volume, "C:", with a fixed volume GUID; a drive that DefineDosDevice makes for a folder ("subst") is redirected by nt_name(). */

#ifndef ERROR_NOT_A_REPARSE_POINT
#define ERROR_NOT_A_REPARSE_POINT 4390
#endif
#ifndef ERROR_DIR_NOT_ROOT
#define ERROR_DIR_NOT_ROOT 144
#endif
#ifndef ERROR_FILENAME_EXCED_RANGE
#define ERROR_FILENAME_EXCED_RANGE 206
#endif
#ifndef ERROR_INVALID_NAME
#define ERROR_INVALID_NAME 123
#endif
#ifndef ERROR_PATH_NOT_FOUND
#define ERROR_PATH_NOT_FOUND 3
#endif
#ifndef ERROR_ACCESS_DENIED
#define ERROR_ACCESS_DENIED 5
#endif

#define DDD_RAW_TARGET_PATH 1
#define DDD_REMOVE_DEFINITION 2
#define DDD_EXACT_MATCH_ON_REMOVE 4
#define DDD_NO_BROADCAST_SYSTEM 8

#define W_(s) ((const WCHAR *)L##s)
#define DOSDEV_MAX 32
#define DOSDEV_NAME_CHARS 48
#define DOSDEV_TARGET_CHARS 160
#define VOLUME_NAME_CHARS 49      /* "\\?\Volume{GUID}\" without the NUL */
#define VOLUME_DEVICE W_("\\Device\\HarddiskVolume1")
#define VOLUME_GUID_TEXT W_("Volume{544f5343-0000-4000-8000-000000000001}")
#define VOLUME_NAME W_("\\\\?\\Volume{544f5343-0000-4000-8000-000000000001}\\")

typedef struct {
  WCHAR name[DOSDEV_NAME_CHARS];
  WCHAR target[DOSDEV_TARGET_CHARS];
  BOOL fixed;      /* part of the system: it cannot be changed or removed */
} DOSDEV;

static DOSDEV g_dosdev[DOSDEV_MAX];
static DWORD g_dosdev_count;
static volatile LONG g_dosdev_lock;
static BOOL g_dosdev_ready;

static void dosdev_lock(void) { while (__sync_lock_test_and_set(&g_dosdev_lock, 1)) NtYieldExecution(); }
static void dosdev_unlock(void) { __sync_lock_release(&g_dosdev_lock); }

static DWORD vol_len(const WCHAR *s) { DWORD n = 0; while (s[n]) ++n; return n; }
static BOOL vol_prefix_ci(const WCHAR *s, const WCHAR *prefix) {
  for (; *prefix; ++s, ++prefix) if (upper(*s) != upper(*prefix)) return FALSE;
  return TRUE;
}

static int dosdev_find(const WCHAR *name) {
  for (DWORD i = 0; i < g_dosdev_count; ++i) if (equals_ignore_case(g_dosdev[i].name, name)) return (int)i;
  return -1;
}
/* Adds or replaces a name; FALSE if the table is full or the text is too long. */
static BOOL dosdev_set(const WCHAR *name, const WCHAR *target, BOOL fixed) {
  const DWORD name_length = vol_len(name), target_length = vol_len(target);
  if (name_length >= DOSDEV_NAME_CHARS || target_length >= DOSDEV_TARGET_CHARS) return FALSE;
  int slot = dosdev_find(name);
  if (slot < 0) {
    if (g_dosdev_count >= DOSDEV_MAX) return FALSE;
    slot = (int)g_dosdev_count++;
  }
  memcpy(g_dosdev[slot].name, name, (name_length + 1) * 2);
  memcpy(g_dosdev[slot].target, target, (target_length + 1) * 2);
  g_dosdev[slot].fixed = fixed;
  return TRUE;
}
static void dosdev_remove(int slot) {
  for (DWORD i = (DWORD)slot; i + 1 < g_dosdev_count; ++i) g_dosdev[i] = g_dosdev[i + 1];
  --g_dosdev_count;
}
/* The table starts with the devices the system has; call with the lock held. */
static void dosdev_init(void) {
  if (g_dosdev_ready) return;
  g_dosdev_ready = TRUE;
  dosdev_set(W_("C:"), VOLUME_DEVICE, TRUE);
  dosdev_set(VOLUME_GUID_TEXT, VOLUME_DEVICE, TRUE);
  dosdev_set(W_("PhysicalDrive0"), W_("\\Device\\Harddisk0\\DR0"), TRUE);
  dosdev_set(W_("NUL"), W_("\\Device\\Null"), TRUE);
  dosdev_set(W_("CON"), W_("\\Device\\ConDrv\\Console"), TRUE);
  dosdev_set(W_("PIPE"), W_("\\Device\\NamedPipe"), TRUE);
  dosdev_set(W_("MAILSLOT"), W_("\\Device\\Mailslot"), TRUE);
  dosdev_set(W_("MountPointManager"), W_("\\Device\\MountPointManager"), TRUE);
}

static BOOL vol_is_letter(WCHAR c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
/* "X:" as the table spells a drive. */
static void vol_drive_name(WCHAR letter, WCHAR out[3]) { out[0] = upper(letter); out[1] = ':'; out[2] = 0; }
/* The index of a drive's entry in the table, or -1 (with the lock held). */
static int vol_drive_slot(WCHAR letter) {
  if (!vol_is_letter(letter)) return -1;
  WCHAR name[3];
  vol_drive_name(letter, name);
  return dosdev_find(name);
}
static BOOL vol_target_is_volume(const WCHAR *target) { return equals_ignore_case(target, VOLUME_DEVICE); }
/* The drive that holds the volume ('C'), with the lock held. */
static WCHAR vol_volume_letter(void) {
  for (DWORD i = 0; i < g_dosdev_count; ++i)
    if (g_dosdev[i].name[1] == ':' && !g_dosdev[i].name[2] && vol_target_is_volume(g_dosdev[i].target)) return g_dosdev[i].name[0];
  return 'C';
}

/* ---- QueryDosDevice, DefineDosDevice ------------------------------------------------------------------------------------------------------------------ */

EXPORT DWORD WINAPI QueryDosDeviceW(LPCWSTR name, LPWSTR buffer, DWORD capacity) {
  DWORD need = 0;
  dosdev_lock();
  dosdev_init();
  if (!name) {
    for (DWORD i = 0; i < g_dosdev_count; ++i) need += vol_len(g_dosdev[i].name) + 1;
    need += 1;
  } else {
    const int slot = dosdev_find(name);
    if (slot < 0) { dosdev_unlock(); set_error(ERROR_FILE_NOT_FOUND); return 0; }
    need = vol_len(g_dosdev[slot].target) + 2;
  }
  if (!buffer || capacity < need) { dosdev_unlock(); set_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
  DWORD at = 0;
  if (!name) {
    for (DWORD i = 0; i < g_dosdev_count; ++i) {
      const DWORD n = vol_len(g_dosdev[i].name) + 1;
      memcpy(buffer + at, g_dosdev[i].name, n * 2);
      at += n;
    }
    buffer[at++] = 0;
  } else {
    const int slot = dosdev_find(name);
    const DWORD n = vol_len(g_dosdev[slot].target) + 1;
    memcpy(buffer, g_dosdev[slot].target, n * 2);
    buffer[n] = 0;
    at = n + 1;
  }
  dosdev_unlock();
  return at;
}
EXPORT DWORD WINAPI QueryDosDeviceA(LPCSTR name, LPSTR buffer, DWORD capacity) {
  WCHAR *w = NULL;
  if (name) { w = to_wide(name); if (!w) return 0; }
  WCHAR *wide = (buffer && capacity) ? RtlAllocateHeap(heap(), 0, (SIZE_T)capacity * 2) : NULL;
  if (buffer && capacity && !wide) { free_wide(w); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
  const DWORD n = QueryDosDeviceW(w, wide, wide ? capacity : 0);
  free_wide(w);
  DWORD result = 0;
  if (n) {
    result = (DWORD)WideCharToMultiByte(CP_UTF8, 0, wide, (int)n, buffer, (int)capacity, NULL, NULL);
    if (!result) set_error(ERROR_INSUFFICIENT_BUFFER);
  }
  if (wide) RtlFreeHeap(heap(), 0, wide);
  return result;
}

EXPORT BOOL WINAPI DefineDosDeviceW(DWORD flags, LPCWSTR name, LPCWSTR target) {
  if ((flags & ~(DWORD)(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM)) || !name || !*name) {
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  WCHAR nt[DOSDEV_TARGET_CHARS];
  if (target) {
    const DWORD n = vol_len(target);
    if (flags & DDD_RAW_TARGET_PATH) {
      if (n >= DOSDEV_TARGET_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
      memcpy(nt, target, (n + 1) * 2);
    } else {
      if (n + 4 >= DOSDEV_TARGET_CHARS) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
      nt[0] = '\\'; nt[1] = '?'; nt[2] = '?'; nt[3] = '\\';
      for (DWORD i = 0; i <= n; ++i) nt[4 + i] = target[i] == '/' ? '\\' : target[i];
    }
  }
  BOOL ok = TRUE;
  DWORD error = 0;
  dosdev_lock();
  dosdev_init();
  const int slot = dosdev_find(name);
  if (flags & DDD_REMOVE_DEFINITION) {
    if (slot < 0 || (target && !equals_ignore_case(g_dosdev[slot].target, nt))) { ok = FALSE; error = ERROR_FILE_NOT_FOUND; }
    else if (g_dosdev[slot].fixed) { ok = FALSE; error = ERROR_ACCESS_DENIED; }
    else dosdev_remove(slot);
  } else if (!target) {
    ok = FALSE;
    error = ERROR_INVALID_PARAMETER;
  } else if (slot >= 0 && g_dosdev[slot].fixed) {
    ok = FALSE;
    error = ERROR_ACCESS_DENIED;
  } else if (!dosdev_set(name, nt, FALSE)) {
    ok = FALSE;
    error = vol_len(name) >= DOSDEV_NAME_CHARS ? ERROR_FILENAME_EXCED_RANGE : ERROR_NOT_ENOUGH_MEMORY;
  }
  dosdev_unlock();
  if (!ok) set_error(error);
  return ok;
}
EXPORT BOOL WINAPI DefineDosDeviceA(DWORD flags, LPCSTR name, LPCSTR target) {
  if (!name) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *wn = to_wide(name), *wt = target ? to_wide(target) : NULL;
  BOOL ok = FALSE;
  if (wn && (!target || wt)) ok = DefineDosDeviceW(flags, wn, wt);
  free_wide(wn);
  free_wide(wt);
  return ok;
}

/* A drive that DefineDosDevice made for a folder: "A:\dir\file" becomes "<target>dir\file". `buffer` holds "\??\A:\..." with `length` characters and `capacity` room; returns the
 * new length, or 0 if the result does not fit. Called by nt_name(). */
static DWORD dosdev_redirect(WCHAR *buffer, DWORD length, DWORD capacity) {
  if (length < 6 || buffer[5] != ':' || !vol_is_letter(buffer[4])) return length;
  WCHAR target[DOSDEV_TARGET_CHARS];
  dosdev_lock();
  const int slot = g_dosdev_ready ? vol_drive_slot(buffer[4]) : -1;
  BOOL redirect = slot >= 0 && !g_dosdev[slot].fixed && g_dosdev[slot].target[0] == '\\' && g_dosdev[slot].target[1] == '?' && g_dosdev[slot].target[2] == '?';
  if (redirect) memcpy(target, g_dosdev[slot].target, (vol_len(g_dosdev[slot].target) + 1) * 2);
  dosdev_unlock();
  if (!redirect) return length;
  DWORD target_length = vol_len(target);
  if (target_length && target[target_length - 1] == '\\') --target_length;
  const WCHAR *rest = buffer + 6;      /* from the backslash after "A:" */
  const DWORD rest_length = length - 6;
  if (target_length + rest_length + 1 > capacity) return 0;
  memmove(buffer + target_length, rest, (rest_length + 1) * 2);
  memcpy(buffer, target, target_length * 2);
  return target_length + rest_length;
}

/* "\\?\Volume{GUID}\rest" names the volume: the NT name is the root of its drive plus the rest. Returns FALSE if `dos` is not such a name. */
static BOOL volume_nt_name(LPCWSTR dos, WCHAR *buffer, DWORD capacity, DWORD *length) {
  if (!dos || dos[0] != '\\' || dos[1] != '\\' || dos[2] != '?' || dos[3] != '\\' || !vol_prefix_ci(dos + 4, VOLUME_GUID_TEXT)) return FALSE;
  const DWORD guid_chars = vol_len(VOLUME_GUID_TEXT);
  const WCHAR *rest = dos + 4 + guid_chars;
  if (*rest && !is_separator(*rest)) return FALSE;
  if (*rest) ++rest;
  dosdev_lock();
  dosdev_init();
  const WCHAR letter = vol_volume_letter();
  dosdev_unlock();
  const DWORD rest_length = vol_len(rest);
  if (rest_length + 8 > capacity) return FALSE;
  buffer[0] = '\\'; buffer[1] = '?'; buffer[2] = '?'; buffer[3] = '\\'; buffer[4] = letter; buffer[5] = ':'; buffer[6] = '\\';
  memcpy(buffer + 7, rest, (rest_length + 1) * 2);
  *length = 7 + rest_length;
  return TRUE;
}

/* ---- the logical drives ---------------------------------------------------------------------------------------------------------------------------------- */

EXPORT DWORD WINAPI GetLogicalDrives(void) {
  DWORD mask = 0;
  dosdev_lock();
  dosdev_init();
  for (DWORD i = 0; i < g_dosdev_count; ++i) {
    const WCHAR *name = g_dosdev[i].name;
    if (vol_is_letter(name[0]) && name[1] == ':' && !name[2]) mask |= 1u << (upper(name[0]) - 'A');
  }
  dosdev_unlock();
  return mask;
}
EXPORT DWORD WINAPI GetLogicalDriveStringsW(DWORD capacity, LPWSTR buffer) {
  const DWORD mask = GetLogicalDrives();
  DWORD count = 0;
  for (DWORD bits = mask; bits; bits &= bits - 1) ++count;
  const DWORD need = count * 4 + 1;
  if (!buffer || capacity < need) return need;
  DWORD at = 0;
  for (DWORD i = 0; i < 26; ++i)
    if (mask & (1u << i)) { buffer[at++] = (WCHAR)('A' + i); buffer[at++] = ':'; buffer[at++] = '\\'; buffer[at++] = 0; }
  buffer[at] = 0;
  return at;
}
EXPORT DWORD WINAPI GetLogicalDriveStringsA(DWORD capacity, LPSTR buffer) {
  const DWORD mask = GetLogicalDrives();
  DWORD count = 0;
  for (DWORD bits = mask; bits; bits &= bits - 1) ++count;
  const DWORD need = count * 4 + 1;
  if (!buffer || capacity < need) return need;
  DWORD at = 0;
  for (DWORD i = 0; i < 26; ++i)
    if (mask & (1u << i)) { buffer[at++] = (char)('A' + i); buffer[at++] = ':'; buffer[at++] = '\\'; buffer[at++] = 0; }
  buffer[at] = 0;
  return at;
}
EXPORT UINT WINAPI GetDriveTypeW(LPCWSTR root) {
  WCHAR letter = 0;
  if (!root) { size_t_ n = 0; letter = current_directory(&n)[0]; }
  else if (vol_is_letter(root[0]) && root[1] == ':') letter = root[0];
  else return 1 /* DRIVE_NO_ROOT_DIR */;
  UINT type = 1;
  dosdev_lock();
  dosdev_init();
  const int slot = vol_drive_slot(letter);
  if (slot >= 0) type = vol_prefix_ci(g_dosdev[slot].target, W_("\\Device\\CdRom")) ? 5 /* DRIVE_CDROM */ : 3 /* DRIVE_FIXED */;
  dosdev_unlock();
  return type;
}
EXPORT UINT WINAPI GetDriveTypeA(LPCSTR root) {
  if (!root) return GetDriveTypeW(NULL);
  WCHAR w[4] = {0};
  for (int i = 0; i < 3 && root[i]; ++i) w[i] = (WCHAR)(BYTE)root[i];
  return GetDriveTypeW(w);
}

/* ---- the names of a volume ----------------------------------------------------------------------------------------------------------------------------- */

/* The volume a path starts with. */
typedef enum { VP_DRIVE, VP_GLOBAL_DRIVE, VP_VOLUME, VP_INVALID } VOLUME_PATH_KIND;
typedef struct {
  VOLUME_PATH_KIND kind;
  WCHAR letter;            /* the drive letter as it was written */
  const WCHAR *rest;       /* what follows the volume */
} VOLUME_PATH;

static BOOL vol_is_global_prefix(const WCHAR *p) { return p[0] == '\\' && p[1] == '\\' && (p[2] == '?' || p[2] == '.') && p[3] == '\\'; }

/* Splits a path into its volume and the rest; a path with no volume ("dir\file", "\dir") is on the current drive. `UNC` and other roots are VP_INVALID. */
static VOLUME_PATH vol_split(const WCHAR *path) {
  VOLUME_PATH result = {VP_INVALID, 0, path};
  if (vol_is_global_prefix(path)) {
    const WCHAR *p = path + 4;
    if (p[0] && p[1] == ':' && !is_separator(p[0]) && (!p[2] || is_separator(p[2]))) {
      result.kind = VP_GLOBAL_DRIVE;
      result.letter = p[0];
      result.rest = p + 2;
    } else if (vol_prefix_ci(p, VOLUME_GUID_TEXT) && (!p[vol_len(VOLUME_GUID_TEXT)] || is_separator(p[vol_len(VOLUME_GUID_TEXT)]))) {
      result.kind = VP_VOLUME;
      result.rest = p + vol_len(VOLUME_GUID_TEXT);
    }
    return result;
  }
  if (is_separator(path[0]) && is_separator(path[1])) return result;      /* \\server\share */
  if (path[0] && path[1] == ':' && !is_separator(path[0])) {
    result.kind = VP_DRIVE;
    result.letter = path[0];
    result.rest = path + 2;
    return result;
  }
  size_t_ n = 0;
  result.kind = VP_DRIVE;
  result.letter = current_directory(&n)[0];
  result.rest = path;
  return result;
}
/* Whether the drive exists; the volume itself always does. */
static BOOL vol_exists(const VOLUME_PATH *v) {
  if (v->kind == VP_VOLUME) return TRUE;
  dosdev_lock();
  dosdev_init();
  const BOOL found = vol_drive_slot(v->letter) >= 0;
  dosdev_unlock();
  return found;
}

/* The root of the volume of a path, in the spelling of GetVolumePathName: "C:\", "\\?\C:\" or "\\?\Volume{...}\". Returns its length, or 0 with the error set. */
static DWORD vol_root(const WCHAR *path, WCHAR out[VOLUME_NAME_CHARS + 1]) {
  const VOLUME_PATH v = vol_split(path);
  if (v.kind == VP_INVALID) { set_error(ERROR_INVALID_NAME); return 0; }
  if (!vol_exists(&v)) { set_error(ERROR_FILE_NOT_FOUND); return 0; }
  if (v.kind == VP_VOLUME) { memcpy(out, VOLUME_NAME, (VOLUME_NAME_CHARS + 1) * 2); return VOLUME_NAME_CHARS; }
  DWORD n = 0;
  if (v.kind == VP_GLOBAL_DRIVE) { out[n++] = '\\'; out[n++] = '\\'; out[n++] = path[2]; out[n++] = '\\'; out[n++] = v.letter; }
  else out[n++] = upper(v.letter);
  out[n++] = ':';
  out[n++] = '\\';
  out[n] = 0;
  return n;
}

EXPORT BOOL WINAPI GetVolumePathNameW(LPCWSTR file, LPWSTR buffer, DWORD capacity) {
  if (!file || !buffer || !capacity) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!*file) { set_error(ERROR_INVALID_NAME); return FALSE; }
  WCHAR root[VOLUME_NAME_CHARS + 1];
  const DWORD n = vol_root(file, root);
  if (n == 0) return FALSE;
  /* The trailing backslash is dropped when only that does not fit. */
  if (capacity >= n + 1) { memcpy(buffer, root, (n + 1) * 2); return TRUE; }
  if (capacity >= n) { memcpy(buffer, root, (n - 1) * 2); buffer[n - 1] = 0; return TRUE; }
  set_error(ERROR_FILENAME_EXCED_RANGE);
  return FALSE;
}
EXPORT BOOL WINAPI GetVolumePathNameA(LPCSTR file, LPSTR buffer, DWORD capacity) {
  if (!file || !buffer || !capacity) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR *w = to_wide(file);
  if (!w) return FALSE;
  WCHAR out[VOLUME_NAME_CHARS + 2];
  const BOOL ok = GetVolumePathNameW(w, out, capacity < VOLUME_NAME_CHARS + 2 ? capacity : VOLUME_NAME_CHARS + 2);
  free_wide(w);
  if (!ok) return FALSE;
  DWORD i = 0;
  for (; out[i]; ++i) buffer[i] = (char)out[i];
  buffer[i] = 0;
  return TRUE;
}

/* A well-formed volume name: "\\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\". */
static BOOL vol_name_well_formed(const WCHAR *name) {
  if (vol_len(name) != VOLUME_NAME_CHARS || !vol_prefix_ci(name, W_("\\\\?\\Volume{")) || name[47] != '}' || name[48] != '\\') return FALSE;
  for (int i = 0; i < 36; ++i) {
    const WCHAR c = name[11 + i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return FALSE; }
    else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return FALSE;
  }
  return TRUE;
}

EXPORT BOOL WINAPI GetVolumeNameForVolumeMountPointW(LPCWSTR mount_point, LPWSTR name, DWORD capacity) {
  if (!mount_point || !*mount_point) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  const DWORD length = vol_len(mount_point);
  if (!is_separator(mount_point[length - 1])) {
    set_error(ERROR_INVALID_NAME);
    return FALSE;
  }
  const VOLUME_PATH v = vol_split(mount_point);
  if (v.kind == VP_INVALID) { set_error(ERROR_INVALID_NAME); return FALSE; }
  if (!vol_exists(&v)) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  const BOOL is_root = v.kind == VP_VOLUME ? (v.rest[0] == 0 || (is_separator(v.rest[0]) && !v.rest[1])) : (is_separator(v.rest[0]) && !v.rest[1]);
  if (!is_root) {
    /* a folder: it would need to be a mounted folder to name a volume, and none is */
    const DWORD attributes = GetFileAttributesW(mount_point);
    if (attributes == 0xFFFFFFFFu) return FALSE;
    set_error(ERROR_NOT_A_REPARSE_POINT);
    return FALSE;
  }
  if (v.kind != VP_VOLUME) {
    BOOL is_volume = FALSE;
    dosdev_lock();
    const int slot = vol_drive_slot(v.letter);
    is_volume = slot >= 0 && vol_target_is_volume(g_dosdev[slot].target);
    dosdev_unlock();
    if (!is_volume) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  }
  if (!name || capacity < VOLUME_NAME_CHARS + 1) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  memcpy(name, VOLUME_NAME, (VOLUME_NAME_CHARS + 1) * 2);
  return TRUE;
}
EXPORT BOOL WINAPI GetVolumeNameForVolumeMountPointA(LPCSTR mount_point, LPSTR name, DWORD capacity) {
  if (!mount_point) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
  WCHAR *w = to_wide(mount_point);
  if (!w) return FALSE;
  WCHAR out[VOLUME_NAME_CHARS + 2];
  const BOOL ok = GetVolumeNameForVolumeMountPointW(w, out, capacity < VOLUME_NAME_CHARS + 2 ? capacity : VOLUME_NAME_CHARS + 2);
  free_wide(w);
  if (!ok) return FALSE;
  DWORD i = 0;
  for (; out[i]; ++i) name[i] = (char)out[i];
  name[i] = 0;
  return TRUE;
}

EXPORT BOOL WINAPI GetVolumePathNamesForVolumeNameW(LPCWSTR volume, LPWSTR names, DWORD capacity, PDWORD returned) {
  if (!volume || !*volume) { set_error(ERROR_INVALID_NAME); return FALSE; }
  if (volume[1] == 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!vol_name_well_formed(volume)) { set_error(ERROR_INVALID_NAME); return FALSE; }
  if (!vol_prefix_ci(volume, VOLUME_NAME) || vol_len(volume) != VOLUME_NAME_CHARS) { set_error(ERROR_FILE_NOT_FOUND); return FALSE; }
  DWORD mask = 0, count = 0;
  dosdev_lock();
  dosdev_init();
  for (DWORD i = 0; i < g_dosdev_count; ++i) {
    const WCHAR *name = g_dosdev[i].name;
    if (vol_is_letter(name[0]) && name[1] == ':' && !name[2] && vol_target_is_volume(g_dosdev[i].target)) { mask |= 1u << (upper(name[0]) - 'A'); ++count; }
  }
  dosdev_unlock();
  const DWORD need = count * 4 + 1;
  if (returned) *returned = need;
  if (!names || capacity < need) { set_error(ERROR_MORE_DATA); return FALSE; }
  DWORD at = 0;
  for (DWORD i = 0; i < 26; ++i)
    if (mask & (1u << i)) { names[at++] = (WCHAR)('A' + i); names[at++] = ':'; names[at++] = '\\'; names[at++] = 0; }
  names[at] = 0;
  return TRUE;
}
EXPORT BOOL WINAPI GetVolumePathNamesForVolumeNameA(LPCSTR volume, LPSTR names, DWORD capacity, PDWORD returned) {
  if (!volume || !*volume) { set_error(ERROR_INVALID_NAME); return FALSE; }
  WCHAR *w = to_wide(volume);
  if (!w) return FALSE;
  WCHAR *wide = (names && capacity) ? RtlAllocateHeap(heap(), 0, (SIZE_T)capacity * 2) : NULL;
  BOOL ok = FALSE;
  if (!names || !capacity || wide) {
    ok = GetVolumePathNamesForVolumeNameW(w, wide, wide ? capacity : 0, returned);
    if (ok) {
      DWORD n = 0;
      while (wide[n] || wide[n + 1]) ++n;
      n += 2;
      for (DWORD i = 0; i < n; ++i) names[i] = (char)wide[i];
    }
  } else {
    set_error(ERROR_NOT_ENOUGH_MEMORY);
  }
  if (wide) RtlFreeHeap(heap(), 0, wide);
  free_wide(w);
  return ok;
}

/* ---- the volume enumeration ------------------------------------------------------------------------------------------------------------------------- */

#define VOLUME_FIND_MAX 8
static struct { HANDLE handle; DWORD next; } g_volume_find[VOLUME_FIND_MAX];

static BOOL volume_find_copy(DWORD index, LPWSTR out, DWORD capacity) {
  if (index >= 1) { set_error(ERROR_NO_MORE_FILES); return FALSE; }
  if (capacity < VOLUME_NAME_CHARS + 1) { set_error(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
  memcpy(out, VOLUME_NAME, (VOLUME_NAME_CHARS + 1) * 2);
  return TRUE;
}
EXPORT HANDLE WINAPI FindFirstVolumeW(LPWSTR name, DWORD capacity) {
  if (!name || capacity < VOLUME_NAME_CHARS + 1) { set_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
  HANDLE h = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!h) return INVALID_HANDLE_VALUE;
  dosdev_lock();
  int slot = -1;
  for (int i = 0; i < VOLUME_FIND_MAX; ++i) if (!g_volume_find[i].handle) { slot = i; break; }
  if (slot >= 0) { g_volume_find[slot].handle = h; g_volume_find[slot].next = 1; }
  dosdev_unlock();
  if (slot < 0) { CloseHandle(h); set_error(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
  volume_find_copy(0, name, capacity);
  return h;
}
EXPORT BOOL WINAPI FindNextVolumeW(HANDLE find, LPWSTR name, DWORD capacity) {
  DWORD index = 0xFFFFFFFFu;
  dosdev_lock();
  for (int i = 0; i < VOLUME_FIND_MAX; ++i) if (find && g_volume_find[i].handle == find) index = g_volume_find[i].next;
  dosdev_unlock();
  if (index == 0xFFFFFFFFu) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!volume_find_copy(index, name, capacity)) return FALSE;
  dosdev_lock();
  for (int i = 0; i < VOLUME_FIND_MAX; ++i) if (g_volume_find[i].handle == find) ++g_volume_find[i].next;
  dosdev_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI FindVolumeClose(HANDLE find) {
  BOOL known = FALSE;
  dosdev_lock();
  for (int i = 0; i < VOLUME_FIND_MAX; ++i) if (find && g_volume_find[i].handle == find) { g_volume_find[i].handle = NULL; known = TRUE; }
  dosdev_unlock();
  if (!known) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  return CloseHandle(find);
}
EXPORT HANDLE WINAPI FindFirstVolumeA(LPSTR name, DWORD capacity) {
  WCHAR out[VOLUME_NAME_CHARS + 2];
  if (!name || capacity < VOLUME_NAME_CHARS + 1) { set_error(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
  HANDLE h = FindFirstVolumeW(out, VOLUME_NAME_CHARS + 2);
  if (h == INVALID_HANDLE_VALUE) return h;
  DWORD i = 0;
  for (; out[i]; ++i) name[i] = (char)out[i];
  name[i] = 0;
  return h;
}
EXPORT BOOL WINAPI FindNextVolumeA(HANDLE find, LPSTR name, DWORD capacity) {
  WCHAR out[VOLUME_NAME_CHARS + 2];
  if (!name) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!FindNextVolumeW(find, out, capacity < VOLUME_NAME_CHARS + 2 ? capacity : VOLUME_NAME_CHARS + 2)) return FALSE;
  DWORD i = 0;
  for (; out[i]; ++i) name[i] = (char)out[i];
  name[i] = 0;
  return TRUE;
}

/* ---- GetVolumeInformation[ByHandle], GetDiskSpaceInformation ----------------------------------------------------------------------------------- */

/* The answers of the file system behind an open file or folder. A text that does not fit its buffer is ERROR_BAD_LENGTH. */
static BOOL volume_information_of(HANDLE h, LPWSTR label, DWORD label_size, LPDWORD serial, LPDWORD max_component, LPDWORD flags, LPWSTR file_system, DWORD file_system_size) {
  IO_STATUS_BLOCK iosb;
  BYTE volume[18 + 2 * 64], attributes[12 + 2 * 32];
  memset(volume, 0, sizeof(volume));
  memset(attributes, 0, sizeof(attributes));
  NTSTATUS status = NtQueryVolumeInformationFile(h, &iosb, volume, sizeof(volume), 1);      /* FileFsVolumeInformation */
  if (NT_SUCCESS(status) || status == STATUS_BUFFER_OVERFLOW) status = NtQueryVolumeInformationFile(h, &iosb, attributes, sizeof(attributes), 5);      /* FileFsAttributeInformation */
  if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW) return fail_nt(status);
  DWORD label_bytes = get32(volume + 12), name_bytes = get32(attributes + 8);
  if (label_bytes > 2 * 64) label_bytes = 2 * 64;
  if (name_bytes > 2 * 32) name_bytes = 2 * 32;
  if (label && label_size && label_size * 2 < label_bytes + 2) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  if (file_system && file_system_size && file_system_size * 2 < name_bytes + 2) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  if (label && label_size) { memcpy(label, volume + 18, label_bytes); label[label_bytes / 2] = 0; }
  if (file_system && file_system_size) { memcpy(file_system, attributes + 12, name_bytes); file_system[name_bytes / 2] = 0; }
  if (serial) *serial = get32(volume + 8);
  if (max_component) *max_component = get32(attributes + 4);
  if (flags) *flags = get32(attributes);
  return TRUE;
}

EXPORT BOOL WINAPI GetVolumeInformationByHandleW(HANDLE file, LPWSTR label, DWORD label_size, LPDWORD serial, LPDWORD max_component, LPDWORD flags, LPWSTR file_system, DWORD file_system_size) {
  return volume_information_of(file, label, label_size, serial, max_component, flags, file_system, file_system_size);
}

EXPORT BOOL WINAPI GetVolumeInformationW(LPCWSTR root, LPWSTR label, DWORD label_size, LPDWORD serial, LPDWORD max_component, LPDWORD flags, LPWSTR file_system, DWORD file_system_size) {
  WCHAR path[VOLUME_NAME_CHARS + 8];
  if (!root) {
    size_t_ n = 0;
    path[0] = current_directory(&n)[0]; path[1] = ':'; path[2] = '\\'; path[3] = 0;
  } else {
    const VOLUME_PATH v = vol_split(root);
    if (v.kind == VP_INVALID) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    if (!vol_exists(&v)) { set_error(ERROR_PATH_NOT_FOUND); return FALSE; }
    const WCHAR *rest = v.rest;
    BOOL is_root = (is_separator(rest[0]) && !rest[1]) || (v.kind != VP_DRIVE && !rest[0]);
    if (!is_root && !*rest && v.kind == VP_DRIVE) {
      /* "C:" is the current directory of the drive, and it has to be the root */
      WCHAR full[PATH_CHARS];
      const DWORD n = full_path_w(root, full, PATH_CHARS);
      if (n == 0) return FALSE;
      is_root = n == 3 && full[2] == '\\';
    }
    if (!is_root) {
      const DWORD n = vol_len(root);
      set_error(n && is_separator(root[n - 1]) ? ERROR_DIR_NOT_ROOT : ERROR_INVALID_NAME);
      return FALSE;
    }
    if (v.kind == VP_VOLUME) { memcpy(path, VOLUME_NAME, (VOLUME_NAME_CHARS + 1) * 2); }
    else { path[0] = upper(v.letter); path[1] = ':'; path[2] = '\\'; path[3] = 0; }
  }
  HANDLE h = open_volume(path);
  if (!h) return FALSE;
  const BOOL ok = volume_information_of(h, label, label_size, serial, max_component, flags, file_system, file_system_size);
  NtClose(h);
  return ok;
}
EXPORT BOOL WINAPI GetVolumeInformationA(LPCSTR root, LPSTR label, DWORD label_size, LPDWORD serial, LPDWORD max_component, LPDWORD flags, LPSTR file_system, DWORD file_system_size) {
  WCHAR *w = root ? to_wide(root) : NULL;
  if (root && !w) return FALSE;
  WCHAR wide_label[64], wide_system[32];
  const BOOL ok = GetVolumeInformationW(w, wide_label, 64, serial, max_component, flags, wide_system, 32);
  free_wide(w);
  if (!ok) return FALSE;
  if (label && label_size && WideCharToMultiByte(CP_UTF8, 0, wide_label, -1, label, (int)label_size, NULL, NULL) == 0) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  if (file_system && file_system_size && WideCharToMultiByte(CP_UTF8, 0, wide_system, -1, file_system, (int)file_system_size, NULL, NULL) == 0) { set_error(ERROR_BAD_LENGTH); return FALSE; }
  return TRUE;
}

/* GetDiskSpaceInformation: the sizes in units of 4096 bytes (8 sectors of 512). The result is an HRESULT; the NT status of a failure is carried in it. */
typedef struct {
  ULONGLONG ActualTotalAllocationUnits, ActualAvailableAllocationUnits, ActualPoolUnavailableAllocationUnits;
  ULONGLONG CallerTotalAllocationUnits, CallerAvailableAllocationUnits, CallerPoolUnavailableAllocationUnits;
  ULONGLONG UsedAllocationUnits, TotalReservedAllocationUnits, VolumeStorageReserveAllocationUnits;
  ULONGLONG AvailableCommittedAllocationUnits, PoolAvailableAllocationUnits;
  DWORD SectorsPerAllocationUnit, BytesPerSector;
} TCOS_DISK_SPACE_INFORMATION;

static LONG hresult_from_nt(NTSTATUS status) { return (LONG)((DWORD)status | 0x10000000u); }

EXPORT LONG WINAPI GetDiskSpaceInformationW(LPCWSTR root, TCOS_DISK_SPACE_INFORMATION *info) {
  if (!info) return hresult_from_nt(STATUS_INVALID_PARAMETER);
  if (root && !*root) return (LONG)0x80070003;      /* HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) */
  WCHAR buffer[PATH_CHARS + 8], local[4];
  UNICODE_STRING name;
  OBJECT_ATTRIBUTES attributes;
  if (!root) { size_t_ n = 0; local[0] = current_directory(&n)[0]; local[1] = ':'; local[2] = '\\'; local[3] = 0; root = local; }
  if (!nt_name(root, buffer, &name, &attributes)) return (LONG)(0x80070000u | (GetLastError() & 0xFFFFu));
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtOpenFile(&h, 0x80100000u, &attributes, &iosb, 7, 0x20 | 0x1);
  if (!NT_SUCCESS(status)) return hresult_from_nt(status);
  BYTE sizes[32];
  status = NtQueryVolumeInformationFile(h, &iosb, sizes, sizeof(sizes), 7);      /* FileFsFullSizeInformation */
  NtClose(h);
  if (!NT_SUCCESS(status)) return hresult_from_nt(status);
  const ULONGLONG unit = (ULONGLONG)get32(sizes + 24) * get32(sizes + 28), bytes = 4096;
  const ULONGLONG total = get64(sizes) * unit / bytes, caller = get64(sizes + 8) * unit / bytes, actual = get64(sizes + 16) * unit / bytes;
  memset(info, 0, sizeof(*info));
  info->UsedAllocationUnits = total > actual ? total - actual : 0;
  info->ActualAvailableAllocationUnits = actual;
  info->ActualTotalAllocationUnits = actual + info->UsedAllocationUnits;
  info->CallerAvailableAllocationUnits = caller;
  info->CallerTotalAllocationUnits = caller + info->UsedAllocationUnits;
  info->SectorsPerAllocationUnit = 8;
  info->BytesPerSector = 512;
  return 0;
}
EXPORT LONG WINAPI GetDiskSpaceInformationA(LPCSTR root, TCOS_DISK_SPACE_INFORMATION *info) {
  WCHAR *w = root ? to_wide(root) : NULL;
  if (root && !w) return (LONG)0x8007000E;      /* E_OUTOFMEMORY */
  const LONG result = GetDiskSpaceInformationW(w, info);
  free_wide(w);
  return result;
}
