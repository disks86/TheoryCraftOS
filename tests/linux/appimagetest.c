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

// The program inside the AppImage tests/appimage_qemu.sh builds: run by the OS through the image's AppRun script, it checks what the AppImage runtime
// promises (APPDIR and friends, the image mounted read only at APPDIR, the variables AppRun exported, its arguments) and prints one line per check.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed = 0, passed = 0;
static void check(int ok, const char* what) {
  printf("appimagetest: %s %s\n", ok ? "ok  " : "FAIL", what);
  if (ok) ++passed; else ++failed;
}

int main(int argc, char** argv) {
  const char* appdir = getenv("APPDIR");
  const char* appimage = getenv("APPIMAGE");
  const char* greeting = getenv("GREETING");
  const char* libpath = getenv("LD_LIBRARY_PATH");
  char path[4096];
  check(appdir && strncmp(appdir, "/tmp/.mount_Hello", 17) == 0, "APPDIR is /tmp/.mount_Hello...");
  check(appimage && strcmp(appimage, "/Desktop/Downloads/Hello.AppImage") == 0, "APPIMAGE names the file");
  check(getenv("ARGV0") && getenv("OWD"), "ARGV0 and OWD are set");
  check(greeting && strcmp(greeting, "hello from AppRun") == 0, "AppRun's export arrived");
  check(libpath && appdir && strncmp(libpath, appdir, strlen(appdir)) == 0, "LD_LIBRARY_PATH starts in the image");
  check(argc == 3 && strcmp(argv[1], "one") == 0 && strcmp(argv[2], "two words") == 0, "arguments passed through");
  check(appdir && strstr(argv[0], "/usr/bin/appimagetest") != NULL, "argv[0] is the program in the image");
  if (!appdir) return 1;

  snprintf(path, sizeof(path), "%s/usr/share/hello/data.txt", appdir);
  FILE* f = fopen(path, "r");
  char line[256] = {0};
  check(f && fgets(line, sizeof(line), f) && strcmp(line, "data inside the image\n") == 0, "a file of the image reads back");
  if (f) fclose(f);

  snprintf(path, sizeof(path), "%s/usr/share/hello/big.bin", appdir);
  int fd = open(path, O_RDONLY);
  unsigned long sum = 0, total = 0;
  unsigned char buf[7001];
  ssize_t got;
  while (fd >= 0 && (got = read(fd, buf, sizeof(buf))) > 0) {
    for (ssize_t i = 0; i < got; ++i) sum = sum * 31 + buf[i];
    total += (unsigned long)got;
  }
  if (fd >= 0) close(fd);
  unsigned long want = 0;
  for (unsigned long i = 0; i < 1000000; ++i) want = want * 31 + (unsigned char)(i * 7 + i / 1000);
  check(total == 1000000 && sum == want, "a 1 MB file spread over many blocks reads back");

  snprintf(path, sizeof(path), "%s/usr/share/hello", appdir);
  DIR* d = opendir(path);
  int entries = 0;
  struct dirent* e;
  while (d && (e = readdir(d))) entries += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
  if (d) closedir(d);
  check(entries == 3, "the image's folder lists its 3 entries");

  snprintf(path, sizeof(path), "%s/usr/share/hello/link.txt", appdir);
  f = fopen(path, "r");
  check(f && fgets(line, sizeof(line), f) && strcmp(line, "data inside the image\n") == 0, "a symbolic link in the image is followed");
  if (f) fclose(f);

  snprintf(path, sizeof(path), "%s/usr/share/hello/new", appdir);
  fd = open(path, O_WRONLY | O_CREAT, 0644);
  check(fd < 0, "the image cannot be written");
  if (fd >= 0) close(fd);

  struct stat st;
  check(stat(appdir, &st) == 0 && S_ISDIR(st.st_mode), "APPDIR is a folder");
  printf("appimagetest: %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
