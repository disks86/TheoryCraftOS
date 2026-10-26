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

/* Mailslots on TheoryCraftOS: CreateMailslot/GetMailslotInfo/SetMailslotInfo, over NtCreateMailslotFile; a writer is just CreateFile(\\.\mailslot\name) (mingw gcc;
 * see tests/windows/build.sh). Prints "mailslottest: N checks, M failed"; the exit code is the number of failures. */
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

int main(void) {
  HANDLE reader = CreateMailslotA("\\\\.\\mailslot\\tcos_notify", 0, MAILSLOT_WAIT_FOREVER, NULL);
  check(reader != INVALID_HANDLE_VALUE, "CreateMailslot");
  check(CreateMailslotA("\\\\.\\mailslot\\tcos_notify", 0, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS,
        "a second CreateMailslot of the same name is refused");

  HANDLE writer = CreateFileA("\\\\.\\mailslot\\tcos_notify", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  check(writer != INVALID_HANDLE_VALUE, "CreateFile opens a writer on the mailslot's name");
  check(CreateFileA("\\\\.\\mailslot\\no_such_slot", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE,
        "a writer for a name nothing made fails");

  DWORD written = 0;
  check(WriteFile(writer, "hello", 5, &written, NULL) && written == 5, "write one message");
  char buffer[64] = {0};
  DWORD read = 0;
  check(ReadFile(reader, buffer, sizeof(buffer), &read, NULL) && read == 5 && !memcmp(buffer, "hello", 5), "read it back whole");

  check(WriteFile(writer, "one", 3, &written, NULL) && WriteFile(writer, "two", 3, &written, NULL), "two more messages, queued in order");
  check(ReadFile(reader, buffer, sizeof(buffer), &read, NULL) && read == 3 && !memcmp(buffer, "one", 3), "the first one back, not both concatenated");
  check(ReadFile(reader, buffer, sizeof(buffer), &read, NULL) && read == 3 && !memcmp(buffer, "two", 3), "then the second");

  DWORD max_size = 0, next_size = 0, count = 0, timeout = 0;
  check(GetMailslotInfo(reader, &max_size, &next_size, &count, &timeout) && count == 0 && timeout == MAILSLOT_WAIT_FOREVER, "GetMailslotInfo on an empty mailslot");
  check(WriteFile(writer, "queued up", 9, &written, NULL), "queue one more, to check GetMailslotInfo sees it");
  check(GetMailslotInfo(reader, &max_size, &next_size, &count, &timeout) && next_size == 9 && count == 1, "GetMailslotInfo reports the pending message");
  check(SetMailslotInfo(reader, 5000), "SetMailslotInfo changes the read timeout");
  check(GetMailslotInfo(reader, &max_size, &next_size, &count, &timeout) && timeout == 5000, "...and GetMailslotInfo reads the new one back");
  check(ReadFile(reader, buffer, sizeof(buffer), &read, NULL) && read == 9, "drain the queued message");

  check(CloseHandle(reader), "close the reader (destroys the mailslot)");
  check(!WriteFile(writer, "too late", 8, &written, NULL), "a writer with the mailslot gone now fails");
  CloseHandle(writer);

  printf("mailslottest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
