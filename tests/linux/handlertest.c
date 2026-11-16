/* A Linux program linked with glibc that uses signal handlers the way programs do: signal() and raise(), a SIGSEGV handler that jumps out with siglongjmp, a
   timer's handler in the middle of floating-point arithmetic, pthread_kill() to one thread among several, a SIGCHLD handler that reaps, a handler that
   abort() runs before the program ends, and sigwait() in a thread. What it prints is the same on every correct kernel: tests/fork_qemu.sh compares a run on
   the real kernel under QEMU with a run on the build machine's Linux. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t count, last, reaped, reaped_code;
static volatile pthread_t handler_thread;
static sigjmp_buf jump;

static void on_plain(int signal) { last = signal; ++count; }
static void on_segv(int signal, siginfo_t* info, void* context) {
  (void)context;
  last = signal;
  siglongjmp(jump, info->si_addr == (void*)0x1000 ? 2 : 3);
}
static void on_thread(int signal) { last = signal; handler_thread = pthread_self(); ++count; }
static void on_child(int signal) {
  int status = 0;
  (void)signal;
  while (waitpid(-1, &status, WNOHANG) > 0) { ++reaped; reaped_code = WEXITSTATUS(status); }
}
static void on_abort(int signal) { (void)signal; const char text[] = "abort handler ran\n"; (void)!write(1, text, sizeof(text) - 1); }

static pthread_t workers[3];
static volatile int ready;
static void* worker(void* arg) {
  (void)arg;
  __atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
  for (;;) pause();
  return 0;
}
static void* waiter(void* arg) {
  sigset_t* set = arg;
  int signal = 0;
  sigwait(set, &signal);
  return (void*)(long)signal;
}

int main(void) {
  setvbuf(stdout, 0, _IONBF, 0);

  signal(SIGUSR1, on_plain);
  raise(SIGUSR1);
  raise(SIGUSR1);
  printf("signal() and raise(): %d handled, last %d\n", (int)count, (int)last);

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_segv;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, 0);
  for (int round = 0; round < 2; ++round) {      /* twice: siglongjmp puts the mask back, so the second fault is delivered too */
    const int came = sigsetjmp(jump, 1);
    if (came == 0) *(volatile int*)0x1000 = 1;
    printf("SIGSEGV handler jumped out: %d, signal %d\n", came, (int)last);
  }
  signal(SIGSEGV, SIG_DFL);

  /* a timer in the middle of floating-point work */
  count = 0;
  signal(SIGALRM, on_plain);
  struct itimerval timer = {{0, 0}, {0, 20000}};
  setitimer(ITIMER_REAL, &timer, 0);
  double sum = 0;
  long n = 0;
  for (; count == 0 && n < 2000000000; ++n) sum += (double)n * 0.5;
  double check = 0;
  for (long k = 0; k < n; ++k) check += (double)k * 0.5;
  printf("timer in floating-point work: handled %d, sums equal %d\n", (int)count, sum == check && n > 0);

  /* a signal for one thread among several (SIGTERM is blocked first, so that every thread made from here on blocks it: see sigwait below) */
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, 0);
  signal(SIGUSR2, on_thread);
  for (int i = 0; i < 3; ++i) pthread_create(&workers[i], 0, worker, 0);
  while (__atomic_load_n(&ready, __ATOMIC_SEQ_CST) < 3) usleep(1000);
  count = 0;
  pthread_kill(workers[1], SIGUSR2);
  while (count == 0) usleep(1000);
  printf("pthread_kill: handled %d on the thread it was for: %d\n", (int)count, pthread_equal(handler_thread, workers[1]) != 0);

  /* sigwait in a thread for a signal the whole process blocks */
  pthread_t waiting;
  pthread_create(&waiting, 0, waiter, &set);
  usleep(20000);
  kill(getpid(), SIGTERM);
  void* got = 0;
  pthread_join(waiting, &got);
  printf("sigwait in a thread: %ld\n", (long)got);

  /* children are reaped by a SIGCHLD handler */
  memset(&action, 0, sizeof(action));
  action.sa_handler = on_child;
  action.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &action, 0);
  pid_t child = fork();
  if (child == 0) _exit(21);
  for (int i = 0; i < 2000 && reaped == 0; ++i) usleep(1000);
  printf("SIGCHLD handler reaped %d child, code %d\n", (int)reaped, (int)reaped_code);
  signal(SIGCHLD, SIG_DFL);

  /* abort(): its handler runs, then the program ends by SIGABRT all the same */
  child = fork();
  if (child == 0) {
    signal(SIGABRT, on_abort);
    abort();
  }
  int status = 0;
  waitpid(child, &status, 0);
  printf("abort with a handler: signaled %d by %d\n", WIFSIGNALED(status), WTERMSIG(status));
  printf("handlers done\n");
  return 0;
}
