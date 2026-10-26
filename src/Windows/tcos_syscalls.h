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

/* The numbers of the NT services of this OS (see src/Process/WindowsSyscalls.hpp, which has the same list: SyscallTest compares the two). Real Windows renumbers its
 * services with almost every release, which is why programs call ntdll.dll; here ntdll.dll is ours, so the numbers are ours. */
#ifndef TCOS_SYSCALLS_H
#define TCOS_SYSCALLS_H
#define NT_SERVICE_NtReadFile 0
#define NT_SERVICE_NtWriteFile 1
#define NT_SERVICE_NtClose 2
#define NT_SERVICE_NtTerminateProcess 3
#define NT_SERVICE_NtYieldExecution 4
#define NT_SERVICE_NtDelayExecution 5
#define NT_SERVICE_NtQuerySystemTime 6
#define NT_SERVICE_NtAllocateVirtualMemory 7
#define NT_SERVICE_NtFreeVirtualMemory 8
#define NT_SERVICE_NtProtectVirtualMemory 9
#define NT_SERVICE_NtCreateFile 10
#define NT_SERVICE_NtOpenFile 11
#define NT_SERVICE_NtQueryInformationFile 12
#define NT_SERVICE_NtSetInformationFile 13
#define NT_SERVICE_NtQueryDirectoryFile 14
#define NT_SERVICE_NtQueryAttributesFile 15
#define NT_SERVICE_NtDeleteFile 16
#define NT_SERVICE_NtFlushBuffersFile 17
#define NT_SERVICE_NtQueryVolumeInformationFile 18
#define NT_SERVICE_NtQueryPerformanceCounter 19
#define NT_SERVICE_NtQueryVirtualMemory 20
#define NT_SERVICE_NtQueryInformationProcess 21
#define NT_SERVICE_NtContinue 22
#define NT_SERVICE_NtRaiseException 23
#define NT_SERVICE_NtCreateThreadEx 24
#define NT_SERVICE_NtTerminateThread 25
#define NT_SERVICE_NtWaitForSingleObject 26
#define NT_SERVICE_NtResumeThread 27
#define NT_SERVICE_NtQueryInformationThread 28
#define NT_SERVICE_NtCreateEvent 29
#define NT_SERVICE_NtOpenEvent 30
#define NT_SERVICE_NtSetEvent 31
#define NT_SERVICE_NtResetEvent 32
#define NT_SERVICE_NtPulseEvent 33
#define NT_SERVICE_NtCreateMutant 34
#define NT_SERVICE_NtOpenMutant 35
#define NT_SERVICE_NtReleaseMutant 36
#define NT_SERVICE_NtCreateSemaphore 37
#define NT_SERVICE_NtOpenSemaphore 38
#define NT_SERVICE_NtReleaseSemaphore 39
#define NT_SERVICE_NtWaitForMultipleObjects 40
#define NT_SERVICE_NtDuplicateObject 41
#define NT_SERVICE_NtSuspendThread 42
#define NT_SERVICE_NtGetContextThread 43
#define NT_SERVICE_NtSetContextThread 44
#define NT_SERVICE_NtCreateUserProcess 45
#define NT_SERVICE_NtQuerySystemInformation 46
#define NT_SERVICE_NtSetInformationThread 47
#define NT_SERVICE_NtSetInformationProcess 48
#define NT_SERVICE_NtGetCurrentProcessorNumber 49
#define NT_SERVICE_NtLoadModule 50
#define NT_SERVICE_NtUnloadModule 51
#define NT_SERVICE_NtReleaseModule 52
#define NT_SERVICE_NtQueryObject 53
#define NT_SERVICE_NtSetInformationObject 54
#define NT_SERVICE_NtCreatePipe 55
#define NT_SERVICE_NtCreateNamedPipeFile 56
#define NT_SERVICE_NtFsControlFile 57
#define NT_SERVICE_NtQueueApcThread 58
#define NT_SERVICE_NtDequeueApc 59
#define NT_SERVICE_NtSignalAndWaitForSingleObject 60
#define NT_SERVICE_NtCreateTimer 61
#define NT_SERVICE_NtOpenTimer 62
#define NT_SERVICE_NtSetTimer 63
#define NT_SERVICE_NtCancelTimer 64
#define NT_SERVICE_NtQueryTimer 65
#define NT_SERVICE_NtQueryTimerResolution 66
#define NT_SERVICE_NtSetTimerResolution 67
#define NT_SERVICE_NtOpenThread 68
#define NT_SERVICE_NtOpenProcess 69
#define NT_SERVICE_NtCreateSection 70
#define NT_SERVICE_NtOpenSection 71
#define NT_SERVICE_NtMapViewOfSection 72
#define NT_SERVICE_NtUnmapViewOfSection 73
#define NT_SERVICE_NtFlushVirtualMemory 74
#define NT_SERVICE_NtLockFile 75
#define NT_SERVICE_NtUnlockFile 76
#define NT_SERVICE_NtCreateJobObject 77
#define NT_SERVICE_NtOpenJobObject 78
#define NT_SERVICE_NtAssignProcessToJobObject 79
#define NT_SERVICE_NtTerminateJobObject 80
#define NT_SERVICE_NtQueryInformationJobObject 81
#define NT_SERVICE_NtSetInformationJobObject 82
#define NT_SERVICE_NtIsProcessInJob 83
#define NT_SERVICE_NtReadVirtualMemory 84
#define NT_SERVICE_NtWriteVirtualMemory 85
#define NT_SERVICE_NtCreateSymbolicLink 86
#define NT_SERVICE_NtCreateSocket 87
#define NT_SERVICE_NtBindSocket 88
#define NT_SERVICE_NtConnectSocket 89
#define NT_SERVICE_NtListenSocket 90
#define NT_SERVICE_NtAcceptSocket 91
#define NT_SERVICE_NtSendSocket 92
#define NT_SERVICE_NtReceiveSocket 93
#define NT_SERVICE_NtSocketControl 94
#define NT_SERVICE_NtSelectSockets 95
#define NT_SERVICE_NtCreateChangeNotification 96
#define NT_SERVICE_NtCreateMailslotFile 97
#endif
