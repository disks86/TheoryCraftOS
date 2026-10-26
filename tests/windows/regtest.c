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

/* advapi32 on TheoryCraftOS: the registry, backed by files under C:\Windows\System32\config, and a few security functions (mingw gcc; see tests/windows/build.sh). Prints
 * "regtest: N checks, M failed"; the exit code is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, (unsigned long)GetLastError());
}
static void check_status(LSTATUS status, LSTATUS want, const char *what) {
  ++g_checks;
  if (status == want) return;
  ++g_failed;
  printf("FAIL: %s (got %ld, want %ld)\n", what, (long)status, (long)want);
}

int main(void) {
  HKEY key = NULL, sub = NULL, reopened = NULL;
  DWORD disposition = 0;

  /* creating a key chain, and the disposition */
  check(RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub", 0, NULL, 0, 0, NULL, &key, &disposition) == ERROR_SUCCESS && disposition == 1 /* REG_CREATED_NEW_KEY */,
        "RegCreateKeyEx creates the whole chain");
  RegCloseKey(key);
  check(RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub", 0, NULL, 0, 0, NULL, &key, &disposition) == ERROR_SUCCESS && disposition == 2 /* REG_OPENED_EXISTING_KEY */,
        "the second time it is only opened");
  check(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub", 0, 0, &reopened) == ERROR_SUCCESS, "RegOpenKeyEx finds it too");
  RegCloseKey(reopened);
  check(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NoSuchVendor", 0, 0, &reopened) == ERROR_FILE_NOT_FOUND, "a key that was never created");

  /* values: string, dword, binary, multi-string, and the unnamed default */
  check(RegSetValueExA(key, "Name", 0, REG_SZ, (const BYTE *)"hello", 6) == ERROR_SUCCESS, "set a string value");
  char text[32];
  DWORD size = sizeof(text), type = 0;
  check(RegQueryValueExA(key, "Name", NULL, &type, (BYTE *)text, &size) == ERROR_SUCCESS && type == REG_SZ && size == 6 && !strcmp(text, "hello"), "read it back");
  check(RegSetValueExA(key, "Count", 0, REG_DWORD, (const BYTE *)"\x2a\x00\x00\x00", 4) == ERROR_SUCCESS, "set a DWORD value");
  DWORD number = 0;
  size = sizeof(number);
  check(RegQueryValueExA(key, "Count", NULL, &type, (BYTE *)&number, &size) == ERROR_SUCCESS && type == REG_DWORD && number == 42, "read the DWORD back");
  const BYTE blob[] = {1, 2, 3, 4, 5};
  check(RegSetValueExA(key, "Blob", 0, REG_BINARY, blob, sizeof(blob)) == ERROR_SUCCESS, "set a binary value");
  BYTE back[8];
  size = sizeof(back);
  check(RegQueryValueExA(key, "Blob", NULL, &type, back, &size) == ERROR_SUCCESS && type == REG_BINARY && size == 5 && !memcmp(back, blob, 5), "read the binary value back");
  check(RegSetValueExA(key, NULL, 0, REG_SZ, (const BYTE *)"default value", 14) == ERROR_SUCCESS, "set the unnamed default value");
  size = sizeof(text);
  check(RegQueryValueExA(key, NULL, NULL, &type, (BYTE *)text, &size) == ERROR_SUCCESS && !strcmp(text, "default value"), "read the default value back");
  check(RegSetValueExA(key, "", 0, REG_SZ, (const BYTE *)"also default", 13) == ERROR_SUCCESS, "an empty name is the default value too");
  size = sizeof(text);
  RegQueryValueExA(key, NULL, NULL, &type, (BYTE *)text, &size);
  check(!strcmp(text, "also default"), "which the earlier set changed");

  /* a buffer too small */
  size = 2;
  check(RegQueryValueExA(key, "Name", NULL, &type, (BYTE *)text, &size) == ERROR_MORE_DATA && size == 6, "ERROR_MORE_DATA reports the real size");
  /* asking only for the size */
  size = 0;
  check(RegQueryValueExA(key, "Name", NULL, &type, NULL, &size) == ERROR_SUCCESS && size == 6, "a NULL buffer just asks the size");
  check(RegQueryValueExA(key, "NoSuchValue", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND, "a value that was never set");

  /* RegGetValue, the convenience wrapper that can open a subkey itself */
  size = sizeof(number);
  check(RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub", "Count", 0, &type, &number, &size) == ERROR_SUCCESS && number == 42,
        "RegGetValue opens the subkey and reads the value in one call");

  /* enumeration: subkeys and values are kept apart even though both live in the same directory */
  check(RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub\\Deeper", 0, NULL, 0, 0, NULL, &sub, NULL) == ERROR_SUCCESS, "a subkey under the one with values");
  RegCloseKey(sub);
  char name[64];
  DWORD name_size = sizeof(name);
  check_status(RegEnumKeyExA(key, 0, name, &name_size, NULL, NULL, NULL, NULL), ERROR_SUCCESS, "RegEnumKeyEx finds the one subkey");
  check(!strcmp(name, "Deeper"), "and it is named right");
  name_size = sizeof(name);
  check_status(RegEnumKeyExA(key, 1, name, &name_size, NULL, NULL, NULL, NULL), ERROR_NO_MORE_ITEMS, "and only the one");
  int found_name = 0, found_count = 0, found_blob = 0, value_count = 0;
  for (DWORD i = 0;; ++i) {
    name_size = sizeof(name);
    DWORD value_type = 0;
    BYTE value_data[32];
    DWORD value_size = sizeof(value_data);
    const LSTATUS status = RegEnumValueA(key, i, name, &name_size, NULL, &value_type, value_data, &value_size);
    if (status == ERROR_NO_MORE_ITEMS) break;
    check(status == ERROR_SUCCESS, "RegEnumValue");
    ++value_count;
    if (!strcmp(name, "Name")) found_name = value_type == REG_SZ;
    if (!strcmp(name, "Count")) found_count = value_type == REG_DWORD;
    if (!strcmp(name, "Blob")) found_blob = value_type == REG_BINARY;
  }
  check(value_count == 4 && found_name && found_count && found_blob, "RegEnumValue finds the named values and the default, with their types");

  /* RegQueryInfoKey */
  DWORD subkeys = 0, values = 0, max_subkey = 0, max_value_name = 0, max_value_data = 0;
  check_status(RegQueryInfoKeyA(key, NULL, NULL, NULL, &subkeys, &max_subkey, NULL, &values, &max_value_name, &max_value_data, NULL, NULL), ERROR_SUCCESS, "RegQueryInfoKey");
  check(subkeys == 1 && values == 4 && max_subkey == 6 /* "Deeper" */, "RegQueryInfoKey's counts");

  /* sharing: a second process-wide handle to the same key sees the same data (the registry is one tree, not per-process) */
  check(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub", 0, 0, &reopened) == ERROR_SUCCESS, "open it again");
  size = sizeof(number);
  check(RegQueryValueExA(reopened, "Count", NULL, NULL, (BYTE *)&number, &size) == ERROR_SUCCESS && number == 42, "the second handle reads the first handle's value");
  RegCloseKey(reopened);

  /* deleting */
  check(RegDeleteValueA(key, "Blob") == ERROR_SUCCESS, "delete one value");
  check(RegQueryValueExA(key, "Blob", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND, "it is gone");
  check(RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub") == ERROR_ACCESS_DENIED, "a key with a subkey refuses to be deleted");
  check_status(RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub\\Deeper"), ERROR_SUCCESS, "delete the subkey first");
  check_status(RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub"), ERROR_SUCCESS, "now the key itself goes");
  check_status(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest\\Sub", 0, 0, &reopened), ERROR_FILE_NOT_FOUND, "and it is really gone");
  RegCloseKey(key);
  RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\TcosRegTest");

  /* the wide-character entry points, and HKEY_CURRENT_USER as a separate tree from HKEY_LOCAL_MACHINE */
  HKEY wkey = NULL;
  check(RegCreateKeyExW(HKEY_CURRENT_USER, L"TcosWideTest", 0, NULL, 0, 0, NULL, &wkey, NULL) == ERROR_SUCCESS, "RegCreateKeyExW");
  check(RegSetValueExW(wkey, L"Greeting", 0, REG_SZ, (const BYTE *)L"hi", 6) == ERROR_SUCCESS, "RegSetValueExW");
  WCHAR wtext[16];
  size = sizeof(wtext);
  check(RegQueryValueExW(wkey, L"Greeting", NULL, &type, (BYTE *)wtext, &size) == ERROR_SUCCESS && !wcscmp(wtext, L"hi"), "RegQueryValueExW");
  check(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "TcosWideTest", 0, 0, &reopened) == ERROR_FILE_NOT_FOUND, "HKEY_CURRENT_USER is a different tree from HKEY_LOCAL_MACHINE");
  RegCloseKey(wkey);
  RegDeleteKeyW(HKEY_CURRENT_USER, L"TcosWideTest");

  /* a little security: enough that a program only checking these works */
  char user[64];
  DWORD user_size = sizeof(user);
  check(GetUserNameA(user, &user_size) && user_size > 0, "GetUserName");
  HANDLE token = NULL;
  check(OpenProcessToken(GetCurrentProcess(), 0x20008 /* TOKEN_QUERY */, &token) && token != NULL, "OpenProcessToken");
  DWORD elevation = 0, got = 0;
  check(GetTokenInformation(token, 20 /* TokenElevation */, &elevation, sizeof(elevation), &got) && elevation == 1, "GetTokenInformation(TokenElevation)");
  HCRYPTPROV provider = 0;
  check(CryptAcquireContextA(&provider, NULL, NULL, 1 /* PROV_RSA_FULL */, 0), "CryptAcquireContext");
  BYTE random_a[16] = {0}, random_b[16] = {0};
  check(CryptGenRandom(provider, sizeof(random_a), random_a) && CryptGenRandom(provider, sizeof(random_b), random_b) && memcmp(random_a, random_b, sizeof(random_a)) != 0,
        "CryptGenRandom gives different bytes each time");
  check(CryptReleaseContext(provider, 0), "CryptReleaseContext");

  printf("regtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
