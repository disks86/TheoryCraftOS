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

// /proc/self/exe, readlink and execve on the real kernel (tests/exec_qemu.sh). Started as /usr/bin/exectest: it checks where it is, opens one file to keep
// and one close-on-exec, and execs itself as "exectest second 'two words'" with one variable; the second program checks what it was given and what it
// still has open, prints its results and exits with 7, which must come back as the first program's exit code.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed, failed;
static void check(int ok, const char* what) {
  if (ok) ++passed;
  else { ++failed; printf("exectest: FAILED %s\n", what); }
}

int main(int argc, char** argv) {
  char self[4096];
  const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  self[n > 0 ? n : 0] = 0;
  check(n > 0 && strcmp(self, "/usr/bin/exectest") == 0, "readlink /proc/self/exe");
  struct stat st;
  check(lstat("/proc/self/exe", &st) == 0 && S_ISLNK(st.st_mode), "lstat /proc/self/exe is a link");
  check(stat("/proc/self/exe", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 1000, "stat /proc/self/exe is the program");
  char small[4];
  check(readlink("/proc/self/exe", small, sizeof(small)) == 4 && memcmp(small, "/usr", 4) == 0, "readlink cuts to the buffer");
  check(readlink("/usr/bin/exectest", self, sizeof(self)) == -1 && errno == EINVAL, "readlink of a file: EINVAL");

  if (argc == 1) {
    const int keep = open("/usr/bin/exectest", O_RDONLY);
    const int drop = open("/usr/bin/exectest", O_RDONLY | O_CLOEXEC);
    check(keep == 3 && drop == 4, "two descriptors");
    char* const args[] = {"exectest", "second", "two words", NULL};
    char* const env[] = {"EXECTEST=from the first", NULL};
    printf("exectest: first: %d passed, %d failed\n", passed, failed);
    fflush(stdout);
    execve("/proc/self/exe", args, env);
    printf("exectest: FAILED execve: %s\n", strerror(errno));
    return 1;
  }

  check(argc == 3 && strcmp(argv[1], "second") == 0 && strcmp(argv[2], "two words") == 0, "the arguments");
  const char* variable = getenv("EXECTEST");
  check(variable && strcmp(variable, "from the first") == 0 && !getenv("PATH"), "only the environment given");
  check(fcntl(3, F_GETFD) == 0, "the kept descriptor is still open");
  check(fcntl(4, F_GETFD) == -1 && errno == EBADF, "the close-on-exec one is closed");
  char head[4];
  check(pread(3, head, 4, 0) == 4 && memcmp(head, "\x7f" "ELF", 4) == 0, "the kept descriptor reads the file");
  printf("exectest: second: %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 7;
}
