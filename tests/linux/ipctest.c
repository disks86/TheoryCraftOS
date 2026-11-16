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
/* System V IPC, POSIX message queues and timers, inotify, pidfd, asynchronous I/O and the newer futex and memory calls, the way programs use them: through
 * the C library's own functions and structures where it has them. Built static with glibc and run on the real kernel by tests/fork_qemu.sh; what it prints
 * must be what it prints on Linux. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <linux/futex.h>
#include <mqueue.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void nap(long milliseconds) {
  struct timespec t = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  nanosleep(&t, 0);
}

static int wait_code(pid_t pid) {
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

static void shared_memory(void) {
  const int id = shmget(IPC_PRIVATE, 10000, 0600 | IPC_CREAT);
  char* memory = id < 0 ? 0 : shmat(id, 0, 0);
  if (id < 0 || memory == (char*)-1) { printf("shm: failed (%d)\n", errno); return; }
  const pid_t child = fork();
  if (child == 0) {      /* the child has the segment attached too: what it writes the parent reads */
    strcpy(memory + 9000, "written by the child");
    _exit(shmdt(memory) == 0 ? 7 : 8);
  }
  /* (each call is made before the line that prints it: the order in which a function's arguments are worked out is the compiler's) */
  const int code = wait_code(child);
  printf("shm: child %d, memory \"%s\"\n", code, memory + 9000);
  struct shmid_ds info;
  memset(&info, 0xEE, sizeof(info));
  const int stat = shmctl(id, IPC_STAT, &info);
  printf("shm: stat %d, size %lu, attached %lu, creator is this process %d, mode %o\n", stat, (unsigned long)info.shm_segsz,
         (unsigned long)info.shm_nattch, info.shm_cpid == getpid(), (unsigned)(info.shm_perm.mode & 0777));
  const int removed = shmctl(id, IPC_RMID, 0);
  printf("shm: remove %d, still readable \"%.7s\"", removed, memory + 9000);
  const int detached = shmdt(memory);
  const int after = shmctl(id, IPC_STAT, &info);
  printf(", detach %d, stat after %d (errno %d)\n", detached, after, errno);
}

union semun { int val; struct semid_ds* buf; unsigned short* array; };

static void semaphores(void) {
  const int id = semget(IPC_PRIVATE, 3, 0600 | IPC_CREAT);
  if (id < 0) { printf("sem: failed (%d)\n", errno); return; }
  unsigned short values[3] = {0, 5, 9};
  union semun argument;
  argument.array = values;
  const int set = semctl(id, 0, SETALL, argument);
  printf("sem: setall %d", set);
  const pid_t child = fork();
  if (child == 0) {      /* waits for the parent to raise the first one, and takes two of the second with it (all or nothing) */
    struct sembuf take[2] = {{0, -1, 0}, {1, -2, SEM_UNDO}};
    _exit(semop(id, take, 2) == 0 ? 3 : 4);
  }
  nap(60);
  struct sembuf give = {0, 1, 0}, none = {2, -100, IPC_NOWAIT};
  const int waiting = semctl(id, 0, GETNCNT);
  const int given = semop(id, &give, 1);
  const int code = wait_code(child);
  printf(", waiting %d, give %d, child %d", waiting, given, code);
  const int v0 = semctl(id, 0, GETVAL), v1 = semctl(id, 1, GETVAL), v2 = semctl(id, 2, GETVAL);
  printf(", values %d %d %d", v0, v1, v2);      /* (the child's SEM_UNDO gave its two back) */
  const int refused = semop(id, &none, 1);
  printf(", too many %d (errno %d)", refused, errno);
  struct semid_ds info;
  argument.buf = &info;
  memset(&info, 0xEE, sizeof(info));
  const int stat = semctl(id, 0, IPC_STAT, argument);
  const unsigned long count = (unsigned long)info.sem_nsems;
  const int removed = semctl(id, 0, IPC_RMID);
  printf(", stat %d, semaphores %lu, remove %d\n", stat, count, removed);
}

struct text_message { long type; char text[32]; };

static void message_queues(void) {
  const int id = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
  if (id < 0) { printf("msg: failed (%d)\n", errno); return; }
  struct text_message m;
  const char* texts[3] = {"first", "second", "third"};
  for (int i = 0; i < 3; ++i) {
    m.type = 3 - i;
    strcpy(m.text, texts[i]);
    if (msgsnd(id, &m, strlen(m.text) + 1, 0) != 0) printf("msg: send failed (%d)\n", errno);
  }
  struct msqid_ds info;
  memset(&info, 0xEE, sizeof(info));
  const int stat = msgctl(id, IPC_STAT, &info);
  printf("msg: stat %d, messages %lu, bytes %lu;", stat, (unsigned long)info.msg_qnum, (unsigned long)info.msg_cbytes);
  long n = msgrcv(id, &m, sizeof(m.text), 2, 0);
  printf(" type 2: %ld \"%s\";", n, m.text);
  n = msgrcv(id, &m, sizeof(m.text), -5, 0);
  printf(" lowest: %ld type %ld \"%s\";", n, m.type, m.text);
  n = msgrcv(id, &m, sizeof(m.text), 0, 0);
  printf(" next: %ld type %ld \"%s\";", n, m.type, m.text);
  n = msgrcv(id, &m, sizeof(m.text), 0, IPC_NOWAIT);
  const int empty_errno = errno;
  const int removed = msgctl(id, IPC_RMID, 0);
  printf(" empty: %ld (errno %d); remove %d\n", n, empty_errno, removed);
}

static void posix_queue(void) {
  char name[64];
  snprintf(name, sizeof(name), "/tcos-ipctest-%d", (int)getpid());
  struct mq_attr attributes = {0, 4, 32, 0};
  const mqd_t q = mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attributes);
  if (q == (mqd_t)-1) { printf("mq: failed (%d)\n", errno); return; }
  const int s1 = mq_send(q, "low", 4, 1), s2 = mq_send(q, "high", 5, 9), s3 = mq_send(q, "middle", 7, 5);
  printf("mq: send %d %d %d", s1, s2, s3);
  struct mq_attr now;
  memset(&now, 0xEE, sizeof(now));
  const int got = mq_getattr(q, &now);
  printf(", attr %d: %ld of %ld, %ld bytes each;", got, now.mq_curmsgs, now.mq_maxmsg, now.mq_msgsize);
  struct pollfd poll_it = {q, POLLIN | POLLOUT, 0};
  const int polled = poll(&poll_it, 1, 0);
  printf(" poll %d (%d);", polled, poll_it.revents);
  char text[32];
  unsigned priority = 0;
  for (int i = 0; i < 3; ++i) {
    const long n = mq_receive(q, text, sizeof(text), &priority);
    printf(" %ld \"%s\" at %u;", n, text, priority);
  }
  struct timespec past = {1, 0};
  const long none = mq_timedreceive(q, text, sizeof(text), 0, &past);
  printf(" empty %ld (errno %d);", none, errno);
  const pid_t child = fork();
  if (child == 0) {      /* a reader that waits for the parent's message */
    const long n = mq_receive(q, text, sizeof(text), &priority);
    _exit(n == 6 && strcmp(text, "hello") == 0 && priority == 2 ? 5 : 6);
  }
  nap(40);
  const int sent = mq_send(q, "hello", 6, 2);
  const int code = wait_code(child);
  const int unlinked = mq_unlink(name);
  const int closed = mq_close(q);
  printf(" send %d, child %d, unlink %d, close %d\n", sent, code, unlinked, closed);
}

static volatile sig_atomic_t timer_signals, timer_value, timer_code_ok, thread_runs;
static void on_timer(int signal, siginfo_t* info, void* context) {
  (void)signal; (void)context;
  ++timer_signals;
  timer_value = info->si_value.sival_int;
  timer_code_ok = info->si_code == SI_TIMER;
}
static void on_thread_timer(union sigval value) { thread_runs += value.sival_int; }

static void timers(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_timer;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &action, 0);
  struct sigevent event;
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  event.sigev_value.sival_int = 42;
  timer_t timer;
  if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) { printf("timer: failed (%d)\n", errno); return; }
  struct itimerspec once = {{0, 0}, {0, 20000000}}, left;
  const int set = timer_settime(timer, 0, &once, 0);
  printf("timer: set %d", set);
  timer_gettime(timer, &left);
  printf(", armed %d", left.it_value.tv_sec == 0 && left.it_value.tv_nsec > 0 && left.it_value.tv_nsec <= 20000000);
  while (timer_signals == 0) pause();
  timer_gettime(timer, &left);
  const int overrun = timer_getoverrun(timer);
  const int deleted = timer_delete(timer);
  printf(", signals %d, value %d, SI_TIMER %d, armed after %d, overrun %d, delete %d", (int)timer_signals, (int)timer_value, (int)timer_code_ok,
         left.it_value.tv_sec != 0 || left.it_value.tv_nsec != 0, overrun, deleted);
  /* SIGEV_THREAD: the C library runs the function on a thread of its own */
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_THREAD;
  event.sigev_notify_function = on_thread_timer;
  event.sigev_value.sival_int = 3;
  if (timer_create(CLOCK_REALTIME, &event, &timer) != 0) { printf(", thread timer failed (%d)\n", errno); return; }
  struct itimerspec soon = {{0, 0}, {0, 10000000}};
  timer_settime(timer, 0, &soon, 0);
  for (int i = 0; i < 200 && thread_runs == 0; ++i) nap(10);
  const int thread_deleted = timer_delete(timer);
  printf(", thread timer ran %d, delete %d\n", (int)thread_runs, thread_deleted);
}

static void watching(void) {
  const int fd = inotify_init1(IN_NONBLOCK);
  if (fd < 0 || mkdir("ipctest-dir", 0755) != 0) { printf("inotify: failed (%d)\n", errno); return; }
  const int wd = inotify_add_watch(fd, "ipctest-dir", IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF);
  printf("inotify: watch %d;", wd);
  const int file = open("ipctest-dir/a", O_CREAT | O_WRONLY, 0644);
  if (write(file, "x", 1) != 1) printf(" write failed;");
  close(file);
  mkdir("ipctest-dir/sub", 0755);
  rename("ipctest-dir/a", "ipctest-dir/b");
  unlink("ipctest-dir/b");
  rmdir("ipctest-dir/sub");
  rmdir("ipctest-dir");
  struct pollfd poll_it = {fd, POLLIN, 0};
  const int polled = poll(&poll_it, 1, 1000);
  printf(" poll %d;", polled);
  char buffer[4096];
  uint32_t from_cookie = 0, to_cookie = 0;
  for (;;) {
    const long n = read(fd, buffer, sizeof(buffer));
    if (n <= 0) break;
    for (long at = 0; at < n;) {
      const struct inotify_event* e = (const struct inotify_event*)(buffer + at);
      printf(" %x \"%s\"", (unsigned)e->mask, e->len ? e->name : "");
      if (e->mask & IN_MOVED_FROM) from_cookie = e->cookie;
      if (e->mask & IN_MOVED_TO) to_cookie = e->cookie;
      at += (long)sizeof(struct inotify_event) + e->len;
    }
  }
  const int read_errno = errno;
  const int closed = close(fd);
  printf("; one cookie %d, errno %d, close %d\n", from_cookie != 0 && from_cookie == to_cookie, read_errno, closed);
}

static void process_descriptor(void) {
  const pid_t child = fork();
  if (child == 0) { nap(40); _exit(9); }
  const int fd = (int)syscall(SYS_pidfd_open, child, 0);
  if (fd < 0) { printf("pidfd: failed (%d)\n", errno); return; }
  struct pollfd poll_it = {fd, POLLIN, 0};
  const int running = poll(&poll_it, 1, 0);
  const long signalled = syscall(SYS_pidfd_send_signal, fd, 0, 0, 0);
  const int ended = poll(&poll_it, 1, 5000);
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  const int waited = waitid(P_PIDFD, (id_t)fd, &info, WEXITED);
  const int closed = close(fd);
  printf("pidfd: running %d, signal 0: %ld, ended %d, waitid %d: code %d status %d, close %d\n", running, signalled, ended, waited, info.si_code, info.si_status, closed);
}

static void async_io(void) {
  aio_context_t context = 0;
  if (syscall(SYS_io_setup, 8, &context) != 0) { printf("aio: failed (%d)\n", errno); return; }
  const int fd = open("ipctest-aio", O_CREAT | O_RDWR | O_TRUNC, 0644);
  char in[16] = {0};
  struct iocb write_it, read_it, *list[2] = {&write_it, &read_it};
  memset(&write_it, 0, sizeof(write_it));
  memset(&read_it, 0, sizeof(read_it));
  write_it.aio_data = 11; write_it.aio_lio_opcode = IOCB_CMD_PWRITE; write_it.aio_fildes = (uint32_t)fd; write_it.aio_buf = (uint64_t)(uintptr_t)"asynchronous"; write_it.aio_nbytes = 12;
  read_it.aio_data = 22; read_it.aio_lio_opcode = IOCB_CMD_PREAD; read_it.aio_fildes = (uint32_t)fd; read_it.aio_buf = (uint64_t)(uintptr_t)in; read_it.aio_nbytes = 5; read_it.aio_offset = 1;
  const long submitted = syscall(SYS_io_submit, context, 2, list);
  printf("aio: submit %ld,", submitted);
  struct io_event events[4];
  struct timespec wait = {2, 0};
  const long n = syscall(SYS_io_getevents, context, 2, 4, events, &wait);
  printf(" events %ld:", n);
  for (long i = 0; i < n; ++i) printf(" %llu=%lld", (unsigned long long)events[i].data, (long long)events[i].res);
  const long destroyed = syscall(SYS_io_destroy, context);
  printf(", read \"%s\", destroy %ld\n", in, destroyed);
  close(fd);
  unlink("ipctest-aio");
}

static void smaller_calls(void) {
  /* futex_waitv on two words nobody wakes: the time runs out */
  uint32_t words[2] = {1, 2};
  struct futex_waitv waiters[2] = {{1, (uint64_t)(uintptr_t)&words[0], FUTEX_32, 0}, {2, (uint64_t)(uintptr_t)&words[1], FUTEX_32, 0}};
  struct timespec until;
  clock_gettime(CLOCK_MONOTONIC, &until);
  until.tv_nsec += 20000000;
  if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; ++until.tv_sec; }
  long r = syscall(SYS_futex_waitv, waiters, 2, 0, &until, CLOCK_MONOTONIC);
  printf("futex_waitv: %ld (errno %d);", r, errno);
  words[1] = 3;
  r = syscall(SYS_futex_waitv, waiters, 2, 0, &until, CLOCK_MONOTONIC);
  printf(" changed word: %ld (errno %d);", r, errno);
  /* its own memory through process_vm_readv; two descriptors of one open file through kcmp; the memory policy of a machine with one node */
  char from[] = "from here", to[16] = {0};
  struct iovec local = {to, sizeof(to)}, remote = {from, sizeof(from)};
  const long copied = (long)process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  printf(" process_vm_readv: %ld \"%s\";", copied, to);
  const int copy = dup(1);
  const long same = syscall(SYS_kcmp, getpid(), getpid(), 0 /* KCMP_FILE */, 1, copy);
  close(copy);
  int mode = -1;
  const long policy = syscall(SYS_get_mempolicy, &mode, 0, 0, 0, 0);
  printf(" kcmp: %ld; get_mempolicy: %ld, mode %d\n", same, policy, mode);
}

/* Signals read from a descriptor: one sent with kill(), a timer's, and SIGCHLD for a child that ended (which nothing here has a handler for). */
static void signal_descriptor(void) {
  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR2);
  sigaddset(&set, SIGCHLD);
  sigaddset(&set, SIGRTMIN + 3);
  sigprocmask(SIG_BLOCK, &set, &old);
  const int fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
  if (fd < 0) { printf("signalfd: failed (%d)\n", errno); return; }
  struct signalfd_siginfo info;
  const long nothing = read(fd, &info, sizeof(info));
  const int nothing_errno = errno;
  struct pollfd poll_it = {fd, POLLIN, 0};
  const int quiet = poll(&poll_it, 1, 0);
  kill(getpid(), SIGUSR2);
  const int ready = poll(&poll_it, 1, 1000);
  memset(&info, 0xEE, sizeof(info));
  const long first = read(fd, &info, sizeof(info));
  printf("signalfd: empty %ld (errno %d), poll %d then %d; read %ld: signal %u code %d from this process %d;", nothing, nothing_errno, quiet, ready, first,
         info.ssi_signo, info.ssi_code, info.ssi_pid == (uint32_t)getpid());
  struct sigevent event;
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGRTMIN + 3;
  event.sigev_value.sival_int = 77;
  timer_t timer;
  timer_create(CLOCK_MONOTONIC, &event, &timer);
  struct itimerspec once = {{0, 0}, {0, 15000000}};
  timer_settime(timer, 0, &once, 0);
  const int timer_ready = poll(&poll_it, 1, 2000);
  memset(&info, 0xEE, sizeof(info));
  const long second = read(fd, &info, sizeof(info));
  printf(" timer: poll %d, read %ld: realtime signal %d, SI_TIMER %d, value %d, overrun %u;", timer_ready, second, (int)info.ssi_signo - SIGRTMIN, info.ssi_code == SI_TIMER,
         info.ssi_int, info.ssi_overrun);
  timer_delete(timer);
  const pid_t child = fork();
  if (child == 0) _exit(6);
  const int child_ready = poll(&poll_it, 1, 5000);
  memset(&info, 0xEE, sizeof(info));
  const long third = read(fd, &info, sizeof(info));
  const int child_matches = info.ssi_pid == (uint32_t)child;
  const int code = wait_code(child);
  const int closed = close(fd);
  printf(" child: poll %d, read %ld: signal %u, code %d, status %d, its pid %d, waited %d; close %d\n", child_ready, third, info.ssi_signo, info.ssi_code, info.ssi_status,
         child_matches, code, closed);
  sigprocmask(SIG_SETMASK, &old, 0);
}

int main(void) {
  setvbuf(stdout, 0, _IOLBF, 0);
  shared_memory();
  semaphores();
  message_queues();
  posix_queue();
  timers();
  watching();
  process_descriptor();
  async_io();
  smaller_calls();
  signal_descriptor();
  printf("ipc ok\n");
  return 0;
}
