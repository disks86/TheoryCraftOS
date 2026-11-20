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
/* The registry outlasts a restart (tests/registry_qemu.sh boots the same disk three times and runs this at each boot, with QEMU killed afterwards
 * as a power cut would). Each run reads how many runs came before from HKCU and from HKLM\SOFTWARE\Classes (where Chrome's installer puts its COM
 * classes), checks that a volatile key of the run before is gone, and writes the count back: the odd runs leave the save to the kernel's writer thread
 * (they wait a few seconds), the even ones call RegFlushKey and end at once. Prints "regpersist: run N" and "regpersist: all N checks passed", exit 42. */
#include <windows.h>
#include <stdio.h>

static int checks, failures;
static void check(int ok, const char *what) {
  ++checks;
  if (!ok) {
    ++failures;
    printf("regpersist: FAILED %s\n", what);
  }
}

static DWORD read_count(HKEY root, const char *path) {
  HKEY key;
  DWORD value = 0, size = sizeof(value), type = 0;
  if (RegOpenKeyExA(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
  if (RegQueryValueExA(key, "Runs", NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS || type != REG_DWORD) value = 0;
  RegCloseKey(key);
  return value;
}

static int write_count(HKEY root, const char *path, DWORD value, int flush) {
  HKEY key;
  if (RegCreateKeyExA(root, path, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return 0;
  int ok = RegSetValueExA(key, "Runs", 0, REG_DWORD, (const BYTE *)&value, sizeof(value)) == ERROR_SUCCESS;
  if (flush) ok = ok && RegFlushKey(key) == ERROR_SUCCESS;
  RegCloseKey(key);
  return ok;
}

int main(void) {
  static const char *kUser = "Software\\TheoryCraftOS Tests\\RegPersist";
  static const char *kClass = "SOFTWARE\\Classes\\CLSID\\{0D7E2F1A-5C3B-4E8F-9A61-7B2C4D8E9F10}\\LocalServer32";
  const DWORD user = read_count(HKEY_CURRENT_USER, kUser), machine = read_count(HKEY_LOCAL_MACHINE, kClass);
  const DWORD run = user + 1;
  printf("regpersist: run %lu (HKCU says %lu runs before, HKLM %lu)\n", (unsigned long)run, (unsigned long)user, (unsigned long)machine);
  check(user == machine, "HKCU and HKLM agree on the runs before");

  HKEY key;
  const LONG gone = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\TheoryCraftOS Tests\\RegPersist\\Volatile", 0, KEY_READ, &key);
  if (gone == ERROR_SUCCESS) RegCloseKey(key);
  check(gone == ERROR_FILE_NOT_FOUND, "the volatile key of the run before is gone");
  DWORD made = 0;
  check(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\TheoryCraftOS Tests\\RegPersist\\Volatile", 0, NULL, REG_OPTION_VOLATILE, KEY_WRITE, NULL, &key, &made) == ERROR_SUCCESS &&
            made == REG_CREATED_NEW_KEY,
        "a volatile key is made");
  RegCloseKey(key);

  const int flush = (run % 2) == 0;
  check(write_count(HKEY_CURRENT_USER, kUser, run, flush), "HKCU written");
  check(write_count(HKEY_LOCAL_MACHINE, kClass, run, flush), "HKLM written");
  if (!flush) Sleep(5000);      // (the kernel's writer saves within about 2 s)
  printf("regpersist: %s %d checks passed\n", failures ? "not all" : "all", checks - failures);
  fflush(stdout);
  return failures ? 1 : 42;
}
