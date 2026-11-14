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

// statfs / fstatfs, link and flock on the real kernel (tests/lock_qemu.sh). Started as /usr/bin/locktest: it asks for the volume of its files, gives a file a
// second name (a file system without hard links answers EPERM, which is reported and accepted), and takes whole-file locks through two open files and two
// threads. Prints its results and exits with 7.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static void check(int ok, const char* what) {
  if (ok) ++passed;
  else { ++failed; printf("locktest: FAILED %s (errno %d)\n", what, errno); }
}

static int waiter_fd, waiter_result = -1;
static void* waiter(void* unused) {
  (void)unused;
  waiter_result = flock(waiter_fd, LOCK_EX);      // waits for the main thread to let go
  return 0;
}

int main(void) {
  const char* a_path = "/tmp/locktest.a";
  const char* b_path = "/tmp/locktest.b";
  unlink(a_path);
  unlink(b_path);
  int a = open(a_path, O_RDWR | O_CREAT, 0644);
  check(a >= 0 && write(a, "hello", 5) == 5, "create a file");

  struct statfs sf;
  memset(&sf, 0xEE, sizeof(sf));
  check(fstatfs(a, &sf) == 0 && sf.f_bsize > 0 && sf.f_blocks > 0 && sf.f_bfree <= sf.f_blocks && sf.f_bavail <= sf.f_bfree && sf.f_namelen >= 255, "fstatfs");
  printf("locktest: volume type 0x%lx, %lu blocks of %lu, %lu free\n", (unsigned long)sf.f_type, (unsigned long)sf.f_blocks, (unsigned long)sf.f_bsize,
         (unsigned long)sf.f_bfree);
  check(sf.f_bfree > 0, "room on the volume");
  struct statfs root;
  check(statfs("/", &root) == 0 && root.f_bsize > 0, "statfs /");
  check(statfs("/nothing/here", &root) == -1 && errno == ENOENT, "statfs of nothing");
  check(fstatfs(999, &root) == -1 && errno == EBADF, "fstatfs of no descriptor");

  const int linked = link(a_path, b_path);
  if (linked == 0) {
    char text[8] = {0};
    const int b = open(b_path, O_RDONLY);
    check(b >= 0 && read(b, text, 5) == 5 && memcmp(text, "hello", 5) == 0, "the second name has the file");
    if (b >= 0) close(b);
    check(link(a_path, b_path) == -1 && errno == EEXIST, "link over a name");
    check(unlink(b_path) == 0, "unlink the second name");
    printf("locktest: hard links work\n");
  } else {
    check(errno == EPERM, "link answers EPERM without hard links");
    printf("locktest: no hard links on this file system (EPERM)\n");
  }
  check(link("/tmp", "/tmp/locktest.dir") == -1 && errno == EPERM, "link of a directory");
  check(link("/nothing", b_path) == -1 && errno == ENOENT, "link of nothing");

  const int b = open(a_path, O_RDWR);
  check(b >= 0, "open again");
  check(flock(a, LOCK_EX) == 0, "exclusive lock");
  check(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "second exclusive lock refused");
  check(flock(b, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK, "shared lock refused");
  const int a2 = dup(a);
  check(a2 >= 0 && flock(a2, LOCK_EX | LOCK_NB) == 0, "the duplicate has the lock");
  check(flock(a, LOCK_SH) == 0 && flock(b, LOCK_SH | LOCK_NB) == 0, "two shared locks");
  check(flock(a, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "no exclusive lock while shared");
  check(flock(b, LOCK_UN) == 0 && flock(a, LOCK_EX | LOCK_NB) == 0, "exclusive after unlock");

  waiter_fd = b;
  pthread_t thread;
  check(pthread_create(&thread, 0, waiter, 0) == 0, "start a thread");
  struct timespec pause = {0, 100 * 1000 * 1000};
  nanosleep(&pause, 0);
  check(waiter_result == -1, "the thread waits");
  check(flock(a, LOCK_UN) == 0, "unlock");
  pthread_join(thread, 0);
  check(waiter_result == 0, "the thread got the lock");
  check(flock(a, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "and holds it");
  check(close(b) == 0 && flock(a, LOCK_EX | LOCK_NB) == 0, "a closed file gives its lock up");
  check(flock(a, 0) == -1 && errno == EINVAL, "an operation that is none");
  close(a);
  close(a2);
  unlink(a_path);
  printf("locktest: %d passed, %d failed\n", passed, failed);
  return 7;
}
