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
#pragma once
#include <stdint.h>

namespace process {

// (src/Windows/tcos_syscalls.h repeats these numbers for the C code of ntdll.dll: keep the two in step. SyscallTest checks that they agree.)
// The service numbers of the NT native interface, as this OS's ntdll passes them in EAX. They are ours to define: real Windows renumbers its services in
// almost every build, which is why programs call ntdll instead of SYSCALL.
enum class WindowsSyscallNumber : uint32_t {
  NtReadFile = 0,
  NtWriteFile = 1,
  NtClose = 2,
  NtTerminateProcess = 3,
  NtYieldExecution = 4,
  NtDelayExecution = 5,
  NtQuerySystemTime = 6,
  NtAllocateVirtualMemory = 7,
  NtFreeVirtualMemory = 8,
  NtProtectVirtualMemory = 9,
  NtCreateFile = 10,
  NtOpenFile = 11,
  NtQueryInformationFile = 12,
  NtSetInformationFile = 13,
  NtQueryDirectoryFile = 14,
  NtQueryAttributesFile = 15,
  NtDeleteFile = 16,
  NtFlushBuffersFile = 17,
  NtQueryVolumeInformationFile = 18,
  NtQueryPerformanceCounter = 19,
  NtQueryVirtualMemory = 20,
  NtQueryInformationProcess = 21,
  NtContinue = 22,
  NtRaiseException = 23,
  NtCreateThreadEx = 24,
  NtTerminateThread = 25,
  NtWaitForSingleObject = 26,
  NtResumeThread = 27,
  NtQueryInformationThread = 28,
  NtCreateEvent = 29,
  NtOpenEvent = 30,
  NtSetEvent = 31,
  NtResetEvent = 32,
  NtPulseEvent = 33,
  NtCreateMutant = 34,
  NtOpenMutant = 35,
  NtReleaseMutant = 36,
  NtCreateSemaphore = 37,
  NtOpenSemaphore = 38,
  NtReleaseSemaphore = 39,
  NtWaitForMultipleObjects = 40,
  NtDuplicateObject = 41,
  NtSuspendThread = 42,
  NtGetContextThread = 43,
  NtSetContextThread = 44,
  NtCreateUserProcess = 45,
  NtQuerySystemInformation = 46,
  NtSetInformationThread = 47,
  NtSetInformationProcess = 48,
  NtGetCurrentProcessorNumber = 49,
  NtLoadModule = 50,
  NtUnloadModule = 51,
  NtReleaseModule = 52,
  NtQueryObject = 53,
  NtSetInformationObject = 54,
  NtCreatePipe = 55,
  NtCreateNamedPipeFile = 56,
  NtFsControlFile = 57,
  NtQueueApcThread = 58,
  NtDequeueApc = 59,
  NtSignalAndWaitForSingleObject = 60,
  NtCreateTimer = 61,
  NtOpenTimer = 62,
  NtSetTimer = 63,
  NtCancelTimer = 64,
  NtQueryTimer = 65,
  NtQueryTimerResolution = 66,
  NtSetTimerResolution = 67,
  NtOpenThread = 68,
  NtOpenProcess = 69,
  NtCreateSection = 70,
  NtOpenSection = 71,
  NtMapViewOfSection = 72,
  NtUnmapViewOfSection = 73,
  NtFlushVirtualMemory = 74,
  NtLockFile = 75,
  NtUnlockFile = 76,
  NtCreateJobObject = 77,
  NtOpenJobObject = 78,
  NtAssignProcessToJobObject = 79,
  NtTerminateJobObject = 80,
  NtQueryInformationJobObject = 81,
  NtSetInformationJobObject = 82,
  NtIsProcessInJob = 83,
  NtReadVirtualMemory = 84,
  NtWriteVirtualMemory = 85,
  NtCreateSymbolicLink = 86,
};

constexpr uint32_t kStatusInvalidSystemService = 0xC000001Cu;      // the answer for a number that is no service

}  // namespace process
