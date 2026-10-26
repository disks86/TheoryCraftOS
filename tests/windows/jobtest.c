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

/* Job objects on TheoryCraftOS: limits, membership, ending a job, kill on close and the processes a member starts (mingw gcc; see tests/windows/build.sh; procchild.exe has
 * to be in System32). Prints "jobtest: N checks, M failed"; the exit code is the number of failures. */
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

static BOOL start(const char *command, DWORD flags, PROCESS_INFORMATION *pi) {
  STARTUPINFOA si;
  char line[256];
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  strcpy(line, command);
  return CreateProcessA(NULL, line, NULL, NULL, FALSE, flags, NULL, NULL, &si, pi);
}
static void close_process(PROCESS_INFORMATION *pi) { CloseHandle(pi->hThread); CloseHandle(pi->hProcess); }
static DWORD members(HANDLE job) {
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION a;
  return QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &a, sizeof(a), NULL) ? a.ActiveProcesses : 9999;
}
static void wait_members(HANDLE job, DWORD want) {
  for (int i = 0; i < 400 && members(job) != want; ++i) Sleep(10);
}

int main(void) {
  HANDLE job = CreateJobObjectA(NULL, "TcosJob");
  check(job != NULL, "create a job");
  HANDLE same = OpenJobObjectA(JOB_OBJECT_ALL_ACCESS, FALSE, "TcosJob");
  check(same != NULL && same != job, "open it by name");
  check(OpenJobObjectA(JOB_OBJECT_ALL_ACCESS, FALSE, "NoSuchJob") == NULL && GetLastError() == ERROR_FILE_NOT_FOUND, "no such job");
  CloseHandle(same);
  BOOL in = TRUE;
  check(IsProcessInJob(GetCurrentProcess(), NULL, &in) && !in, "not in a job yet");

  /* a process started suspended, put in the job, then run */
  PROCESS_INFORMATION child;
  check(start("procchild.exe exit 3", CREATE_SUSPENDED, &child), "start a child suspended");
  check(AssignProcessToJobObject(job, child.hProcess), "assign it");
  check(members(job) == 1, "one member");
  check(IsProcessInJob(child.hProcess, job, &in) && in && IsProcessInJob(child.hProcess, NULL, &in) && in, "it is in the job");
  HANDLE other = CreateJobObjectA(NULL, NULL);
  check(!AssignProcessToJobObject(other, child.hProcess) && GetLastError() == ERROR_ACCESS_DENIED, "a process is in one job only");
  CloseHandle(other);
  ResumeThread(child.hThread);
  WaitForSingleObject(child.hProcess, 10000);
  DWORD code = 0;
  GetExitCodeProcess(child.hProcess, &code);
  check(code == 3, "the child ran and ended");
  close_process(&child);
  wait_members(job, 0);
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc;
  check(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof(acc), NULL) && acc.TotalProcesses == 1 && acc.ActiveProcesses == 0, "accounting after the end");

  /* limits */
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
  memset(&limits, 0, sizeof(limits));
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 2;
  check(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "set a limit of two active processes");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION back;
  DWORD returned = 0;
  check(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &back, sizeof(back), &returned) && returned == sizeof(back) && back.BasicLimitInformation.ActiveProcessLimit == 2, "read it back");
  limits.BasicLimitInformation.ActiveProcessLimit = 0;
  check(!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && GetLastError() == ERROR_INVALID_PARAMETER, "a limit of zero is refused");
  limits.BasicLimitInformation.ActiveProcessLimit = 2;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  PROCESS_INFORMATION a, b, c;
  check(start("procchild.exe spin", CREATE_SUSPENDED, &a) && AssignProcessToJobObject(job, a.hProcess), "first spinner");
  check(start("procchild.exe spin", CREATE_SUSPENDED, &b) && AssignProcessToJobObject(job, b.hProcess), "second spinner");
  check(start("procchild.exe spin", CREATE_SUSPENDED, &c), "third started");
  check(!AssignProcessToJobObject(job, c.hProcess) && GetLastError() == ERROR_NOT_ENOUGH_QUOTA, "the third is over the limit");
  TerminateProcess(c.hProcess, 9);
  close_process(&c);

  /* the process list and ending the job */
  ResumeThread(a.hThread);
  ResumeThread(b.hThread);
  struct { JOBOBJECT_BASIC_PROCESS_ID_LIST list; ULONG_PTR more[4]; } ids;
  memset(&ids, 0, sizeof(ids));
  check(QueryInformationJobObject(job, JobObjectBasicProcessIdList, &ids, sizeof(ids), NULL) && ids.list.NumberOfAssignedProcesses == 2 && ids.list.NumberOfProcessIdsInList == 2, "two processes listed");
  check(ids.list.ProcessIdList[0] == GetProcessId(a.hProcess) || ids.list.ProcessIdList[1] == GetProcessId(a.hProcess), "the first is listed");
  check(TerminateJobObject(job, 77), "terminate the job");
  check(WaitForSingleObject(a.hProcess, 10000) == WAIT_OBJECT_0 && WaitForSingleObject(b.hProcess, 10000) == WAIT_OBJECT_0, "both end");
  GetExitCodeProcess(a.hProcess, &code);
  check(code == 77, "with the exit code given");
  close_process(&a);
  close_process(&b);
  wait_members(job, 0);
  check(QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof(acc), NULL) && acc.TotalProcesses == 3 && acc.TotalTerminatedProcesses == 2, "terminated processes counted");

  /* what a member starts joins the job; breaking away needs the job's leave */
  limits.BasicLimitInformation.LimitFlags = 0;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  PROCESS_INFORMATION parent;
  check(start("procchild.exe grandchild", CREATE_SUSPENDED, &parent) && AssignProcessToJobObject(job, parent.hProcess), "a member that starts a process");
  ResumeThread(parent.hThread);
  WaitForSingleObject(parent.hProcess, 10000);
  GetExitCodeProcess(parent.hProcess, &code);
  check(code == 0, "it started the grandchild");
  close_process(&parent);
  wait_members(job, 1);
  check(members(job) == 1, "the grandchild is in the job");
  PROCESS_INFORMATION breaker;
  check(start("procchild.exe grandchild breakaway", CREATE_SUSPENDED, &breaker) && AssignProcessToJobObject(job, breaker.hProcess), "a member that wants to break away");
  ResumeThread(breaker.hThread);
  WaitForSingleObject(breaker.hProcess, 10000);
  GetExitCodeProcess(breaker.hProcess, &code);
  check(code == 20, "breaking away is not allowed by default");
  close_process(&breaker);
  TerminateJobObject(job, 1);
  wait_members(job, 0);

  /* kill on close */
  HANDLE closing = CreateJobObjectA(NULL, NULL);
  memset(&limits, 0, sizeof(limits));
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
  check(SetInformationJobObject(closing, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "kill on close set");
  JOBOBJECT_BASIC_LIMIT_INFORMATION basic;
  memset(&basic, 0, sizeof(basic));
  basic.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  check(!SetInformationJobObject(closing, JobObjectBasicLimitInformation, &basic, sizeof(basic)), "the basic structure cannot ask for it");
  PROCESS_INFORMATION victim, free_one;
  check(start("procchild.exe spin", 0, &victim) && AssignProcessToJobObject(closing, victim.hProcess), "a process in the job that will close");
  check(start("procchild.exe grandchild breakaway", 0, &free_one), "a member that breaks away");
  WaitForSingleObject(free_one.hProcess, 10000);
  GetExitCodeProcess(free_one.hProcess, &code);
  close_process(&free_one);
  CloseHandle(closing);
  check(WaitForSingleObject(victim.hProcess, 10000) == WAIT_OBJECT_0, "closing the job ended the process");
  GetExitCodeProcess(victim.hProcess, &code);
  check(code == 1, "with exit code 1");
  close_process(&victim);

  CloseHandle(job);
  printf("jobtest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
