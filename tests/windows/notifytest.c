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

/* Directory change notifications on TheoryCraftOS: FindFirstChangeNotification/FindNextChangeNotification/FindCloseChangeNotification, over NtCreateChangeNotification
 * (mingw gcc; see tests/windows/build.sh). Prints "notifytest: N checks, M failed"; the exit code is the number of failures. */
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

static DWORD WINAPI make_rdc_file(LPVOID unused) {
  (void)unused;
  Sleep(50);      // give the main thread time to actually be blocked in ReadDirectoryChangesW first
  HANDLE f = CreateFileA("C:\\Temp\\notifywatched\\rdc.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  return 0;
}

int main(void) {
  check(CreateDirectoryA("C:\\Temp\\notifywatched", NULL) || GetLastError() == ERROR_ALREADY_EXISTS, "make the watched directory");
  check(CreateDirectoryA("C:\\Temp\\notifywatched\\deep", NULL) || GetLastError() == ERROR_ALREADY_EXISTS, "make a subdirectory to watch with it");
  check(CreateDirectoryA("C:\\Temp\\notifysibling", NULL) || GetLastError() == ERROR_ALREADY_EXISTS, "make an unrelated sibling directory");

  HANDLE plain = FindFirstChangeNotificationA("C:\\Temp\\notifywatched", FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
  check(plain != INVALID_HANDLE_VALUE, "FindFirstChangeNotification on a real directory");
  HANDLE subtree = FindFirstChangeNotificationA("C:\\Temp\\notifywatched", TRUE, FILE_NOTIFY_CHANGE_FILE_NAME);
  check(subtree != INVALID_HANDLE_VALUE, "FindFirstChangeNotification with bWatchSubtree");
  check(FindFirstChangeNotificationA("C:\\Temp\\notifywatched\\deep\\no_such_file.txt", FALSE, 0) == INVALID_HANDLE_VALUE &&
            GetLastError() == ERROR_PATH_NOT_FOUND,
        "a nonexistent directory fails with ERROR_PATH_NOT_FOUND");
  check(FindFirstChangeNotificationA("C:\\Temp\\lock.bin", FALSE, 0) == INVALID_HANDLE_VALUE || GetLastError() == ERROR_DIRECTORY ||
            GetLastError() == ERROR_FILE_NOT_FOUND,
        "a plain file (if one happens to exist there) is not a directory");

  check(WaitForSingleObject(plain, 0) == WAIT_TIMEOUT, "nothing has changed yet");
  HANDLE f = CreateFileA("C:\\Temp\\notifywatched\\new.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "create a file directly under the watched directory");
  CloseHandle(f);
  check(WaitForSingleObject(plain, 1000) == WAIT_OBJECT_0, "the plain watch sees it");
  check(WaitForSingleObject(subtree, 0) == WAIT_OBJECT_0, "the subtree watch sees it too");
  check(FindNextChangeNotification(plain), "FindNextChangeNotification re-arms the plain watch");
  check(FindNextChangeNotification(subtree), "FindNextChangeNotification re-arms the subtree watch");
  check(WaitForSingleObject(plain, 0) == WAIT_TIMEOUT && WaitForSingleObject(subtree, 0) == WAIT_TIMEOUT, "both are quiet again");

  check(DeleteFileA("C:\\Temp\\notifywatched\\new.txt"), "delete the file");
  check(WaitForSingleObject(plain, 1000) == WAIT_OBJECT_0, "deleting fires it too");
  FindNextChangeNotification(plain);
  FindNextChangeNotification(subtree);

  f = CreateFileA("C:\\Temp\\notifysibling\\unrelated.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "create a file in the unrelated sibling directory");
  CloseHandle(f);
  check(WaitForSingleObject(plain, 100) == WAIT_TIMEOUT, "a change in a sibling directory does not fire this watch");
  check(WaitForSingleObject(subtree, 100) == WAIT_TIMEOUT, "...nor the subtree watch on a different directory");

  f = CreateFileA("C:\\Temp\\notifywatched\\deep\\inside.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  check(f != INVALID_HANDLE_VALUE, "create a file two levels under the watched directory");
  CloseHandle(f);
  check(WaitForSingleObject(subtree, 1000) == WAIT_OBJECT_0, "the subtree watch sees a change deep underneath");
  check(WaitForSingleObject(plain, 0) == WAIT_TIMEOUT, "the plain (non-subtree) watch does not see into the subdirectory");

  check(FindCloseChangeNotification(plain), "FindCloseChangeNotification (plain)");
  check(FindCloseChangeNotification(subtree), "FindCloseChangeNotification (subtree)");

  /* ReadDirectoryChangesW: an open directory handle, read synchronously (no OVERLAPPED), gets back what changed and its name - not just that something did. */
  HANDLE dir = CreateFileA("C:\\Temp\\notifywatched", FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
  check(dir != INVALID_HANDLE_VALUE, "CreateFile opens the directory for ReadDirectoryChangesW");
  /* The watch only exists from this thread's first ReadDirectoryChangesW call on; a change made before that call is simply never reported (like a real one), so
     the file is created on a second thread *after* this one is already blocked waiting, not before. */
  HANDLE maker = CreateThread(NULL, 0, make_rdc_file, NULL, 0, NULL);
  check(maker != NULL, "a thread to create the file this call is waiting for");
  BYTE buffer[256];
  DWORD returned = 0;
  check(ReadDirectoryChangesW(dir, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &returned, NULL, NULL) && returned > 0,
        "ReadDirectoryChangesW reports the pending change");
  PFILE_NOTIFY_INFORMATION info = (PFILE_NOTIFY_INFORMATION)buffer;
  check(info->Action == FILE_ACTION_ADDED, "...as FILE_ACTION_ADDED");
  check(info->FileNameLength == 7 * sizeof(WCHAR) && !memcmp(info->FileName, L"rdc.txt", info->FileNameLength), "...with the right name");
  WaitForSingleObject(maker, 5000);
  CloseHandle(maker);
  check(DeleteFileA("C:\\Temp\\notifywatched\\rdc.txt"), "delete it");
  check(ReadDirectoryChangesW(dir, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &returned, NULL, NULL) && returned > 0,
        "ReadDirectoryChangesW reports the deletion too (this one was already queued, like FindFirstChangeNotification's own case above)");
  check(((PFILE_NOTIFY_INFORMATION)buffer)->Action == FILE_ACTION_REMOVED, "...as FILE_ACTION_REMOVED");
  CloseHandle(dir);

  printf("notifytest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
