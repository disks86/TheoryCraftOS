/* A Linux program linked with glibc that makes processes the way programs do: fork(), posix_spawn() (clone with CLONE_VM | CLONE_VFORK), vfork(), fork() then
   exec with a pipe as standard output, abort() in a child, and sigwait() for a blocked signal. What it prints is the same on every correct kernel:
   tests/fork_qemu.sh compares a run on the real kernel under QEMU with a run on the build machine's Linux. It starts itself, as /proc/self/exe, for its children. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
int main(int argc, char** argv) {
  setvbuf(stdout, 0, _IONBF, 0);
  if (argc > 1 && !strcmp(argv[1], "child")) { printf("spawned child says %s\n", argv[2]); return 23; }
  int st = 0;
  pid_t p = fork();
  if (p == 0) { printf("in fork child, parent ok %d\n", getppid() != getpid()); _exit(5); }
  waitpid(p, &st, 0);
  printf("fork: exited=%d code=%d\n", WIFEXITED(st), WEXITSTATUS(st));
  char* args[] = {argv[0], "child", "hello", 0};
  int r = posix_spawn(&p, "/proc/self/exe", 0, 0, args, environ);
  printf("posix_spawn: %d\n", r);
  waitpid(p, &st, 0);
  printf("spawn: exited=%d code=%d\n", WIFEXITED(st), WEXITSTATUS(st));
  p = fork();
  if (p == 0) abort();
  waitpid(p, &st, 0);
  printf("abort: signaled=%d sig=%d\n", WIFSIGNALED(st), WTERMSIG(st));
  int fds[2];
  if (pipe(fds)) return 1;
  p = fork();
  if (p == 0) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); execv("/proc/self/exe", args); _exit(99); }
  close(fds[1]);
  char buf[64] = {0};
  int n = (int)read(fds[0], buf, 63);
  waitpid(p, &st, 0);
  printf("exec child wrote %d bytes: %s", n, buf);
  printf("exec: code=%d\n", WEXITSTATUS(st));
  sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR1); sigprocmask(SIG_BLOCK, &set, 0);
  raise(SIGUSR1);
  int sig = 0; sigwait(&set, &sig);
  printf("sigwait: %d\n", sig);
  p = vfork();
  if (p == 0) _exit(9);
  waitpid(p, &st, 0);
  printf("vfork: code=%d\n", WEXITSTATUS(st));
  printf("spawntest done\n");
  return 0;
}
