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
#include "Process/AddressSpace.hpp"
#include "Process/LinuxExec.hpp"
#include "Process/LinuxFiles.hpp"
#include "Process/LinuxIpc.hpp"
#include "Process/LinuxProcess.hpp"
#include "Process/LinuxSockets.hpp"
#include "Process/LinuxThreads.hpp"
#include "Process/LinuxPacket.hpp"
#include "Process/LinuxRenderNode.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux system-call interface: the console, the clock, memory, files (LinuxFiles.cpp), threads (LinuxThreads.cpp), processes and signals (LinuxProcess.cpp,
// LinuxSignals.cpp), and AF_UNIX sockets (LinuxSockets.cpp, shared with Windows programs). docs/LinuxSyscalls.md lists what there is. Numbers differ by
// CPU: x86-64 has its own table, AArch64 and RISC-V 64 share the "generic" one. A call not listed returns -ENOSYS, which is what libc expects from a
// kernel that lacks it. Results are the value or -errno in the result register.
namespace process {

namespace {

constexpr int64_t kEperm = 1, kEnoent = 2, kEnxio = 6, kEnotty = 25, kEspipe = 29, kEnomem = 12, kEacces = 13, kEnodev = 19, kEexist = 17, kEbadf = 9, kEfault = 14, kEinval = 22, kEnosys = 38, kEopnotsupp = 95, kEintr = 4;
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u;      // what Linux allows in one read or write
constexpr uint64_t kAtFdCwd = ~uint64_t{99};      // -100

enum class Call : uint16_t { None, Clone3, Read, Write, Writev, Close, Getpid, Gettid, Getuid, Getgid, Getresid, ClockGetres, Exit, ExitGroup, Uname, ClockGettime, Nanosleep, ClockNanosleep, SchedYield, SetTidAddress, Brk, Mmap, Munmap, Mprotect, ArchPrctl, Ioctl, Fstat, Newfstatat, Lseek, Openat, Readlinkat, RtSigaction, RtSigprocmask, Sigaltstack,
  SetRobustList, Rseq, Prlimit64, Getrandom, Gettimeofday, Time, Getppid, Madvise,
  Open, Stat, Lstat, Pread, Pwrite, Readv, Getdents64, Mkdir, Mkdirat, Mknod, Mknodat, Rmdir, Unlink, Unlinkat, Rename, Renameat, Renameat2, Getcwd, Chdir, Fchdir, Access, Faccessat,
  Truncate, Ftruncate, Fsync, Dup, Dup2, Dup3, Fcntl, Readlink, Statx, Umask, Clone, Futex, Tgkill, SchedGetaffinity, Prctl, Succeed,
  Socket, Socketpair, Pipe, Pipe2, Eventfd, Eventfd2, TimerfdCreate, TimerfdSettime, TimerfdGettime, Signalfd, Signalfd4, EpollCreate, EpollCreate1,
  EpollCtl, EpollWait, EpollPwait, EpollPwait2, Bind, Connect, Listen, Accept, Accept4, Sendto, Recvfrom, Sendmsg, Recvmsg, Sendmmsg, Recvmmsg, Shutdown, Getsockname, Getpeername,
  Setsockopt, Getsockopt, Poll, Ppoll, Select, Pselect6, MemfdCreate, Fallocate, Execve, Execveat, Statfs, Fstatfs, Link, Linkat, Flock,
  // files
  Creat, Symlink, Symlinkat, Preadv, Pwritev, Preadv2, Pwritev2, Sendfile, CopyFileRange, Splice, Readahead, Fadvise, SyncFileRange, Syncfs, Getdents, Openat2, CloseRange,
  Xattr, Refuse,
  // signals
  RtSigpending, RtSigtimedwait, RtSigqueueinfo, RtTgsigqueueinfo, RtSigsuspend, RtSigreturn, Kill, Tkill, Pause, Alarm, Getitimer, Setitimer,
  // processes
  Fork, Wait4, Waitid, Getpgrp, Getpgid, Setpgid, Setsid, Getsid, Geteuid, Getegid, Setuid, Setgid, Setreuid, Setregid, Setresuid, Setresgid, Getresuid, Getresgid, Setfsuid, Setfsgid,
  Getgroups, Setgroups, Capget, Capset, Getrlimit, Setrlimit, Getrusage, Times, Sysinfo, Getpriority, Setpriority, SchedSetparam, SchedGetparam, SchedSetscheduler,
  SchedGetscheduler, SchedPriorityMax, SchedPriorityMin, SchedRrInterval, SchedSetaffinity, SchedSetattr, SchedGetattr, Getcpu, Personality, Membarrier, GetRobustList,
  // memory
  Mremap, Msync, Mincore, Mlock, Mlockall,
  // timers, the newer futex calls, and the calls of LinuxMisc.cpp
  TimerCreate, TimerSettime, TimerGettime, TimerGetoverrun, TimerDelete, FutexWaitv, FutexWake2, FutexWait2, FutexRequeue2,
  GetMempolicy, SetMempolicy, Mbind, SetMempolicyHomeNode, MigratePages, MovePages, PkeyAlloc, PkeyFree, PkeyMprotect, Kcmp, ProcessVmReadv, ProcessVmWritev,
  Cachestat, RestartSyscall,
  // System V IPC (LinuxIpc.cpp)
  Shmget, Shmat, Shmdt, Shmctl, Semget, Semop, Semtimedop, Semctl, Msgget, Msgsnd, Msgrcv, Msgctl,
  MqOpen, MqUnlink, MqTimedsend, MqTimedreceive, MqNotify, MqGetsetattr,
  PidfdOpen, PidfdSendSignal, PidfdGetfd, ProcessMadvise, ProcessMrelease,
  IoSetup, IoDestroy, IoSubmit, IoCancel, IoGetevents, InotifyInit, InotifyInit1, InotifyAddWatch, InotifyRmWatch,
  // calls that are in Linux's tables and have a fixed answer here: see the table
  Unsupported, Absent };
constexpr uint16_t kAbsent = 0xFFFF;      // this CPU's table has no such call

struct NumberEntry {
  uint16_t x86_64, generic;
  Call call;
};

constexpr NumberEntry kNumbers[] = {
    {0, 63, Call::Read},           {1, 64, Call::Write},        {20, 66, Call::Writev},       {3, 57, Call::Close},
    {39, 172, Call::Getpid},       {186, 178, Call::Gettid},    {102, 174, Call::Getuid},     {104, 176, Call::Getgid},
    {107, 175, Call::Geteuid},     {108, 177, Call::Getegid},   {118, 148, Call::Getresuid},  {120, 150, Call::Getresgid},
    {60, 93, Call::Exit},          {231, 94, Call::ExitGroup},  {63, 160, Call::Uname},       {228, 113, Call::ClockGettime},
    {229, 114, Call::ClockGetres}, {35, 101, Call::Nanosleep},    {230, 115, Call::ClockNanosleep}, {24, 124, Call::SchedYield}, {218, 96, Call::SetTidAddress},
    {12, 214, Call::Brk},          {9, 222, Call::Mmap},        {11, 215, Call::Munmap},      {10, 226, Call::Mprotect},
    {158, kAbsent, Call::ArchPrctl}, {16, 29, Call::Ioctl},     {5, 80, Call::Fstat},         {262, 79, Call::Newfstatat},
    {8, 62, Call::Lseek},          {257, 56, Call::Openat},     {267, 78, Call::Readlinkat},  {13, 134, Call::RtSigaction},
    {14, 135, Call::RtSigprocmask}, {131, 132, Call::Sigaltstack}, {273, 99, Call::SetRobustList}, {334, 293, Call::Rseq},
    {302, 261, Call::Prlimit64},   {318, 278, Call::Getrandom}, {96, 169, Call::Gettimeofday}, {201, kAbsent, Call::Time}, {110, 173, Call::Getppid},
    {28, 233, Call::Madvise},
    {2, kAbsent, Call::Open},      {4, kAbsent, Call::Stat},    {6, kAbsent, Call::Lstat},    {17, 67, Call::Pread},
    {18, 68, Call::Pwrite},        {19, 65, Call::Readv},       {217, 61, Call::Getdents64},  {83, kAbsent, Call::Mkdir},
    {258, 34, Call::Mkdirat},      {133, kAbsent, Call::Mknod}, {259, 33, Call::Mknodat},      {84, kAbsent, Call::Rmdir},  {87, kAbsent, Call::Unlink},  {263, 35, Call::Unlinkat},
    {82, kAbsent, Call::Rename},   {264, 38, Call::Renameat},   {316, 276, Call::Renameat2},  {79, 17, Call::Getcwd},
    {80, 49, Call::Chdir},         {81, 50, Call::Fchdir},      {21, kAbsent, Call::Access},  {269, 48, Call::Faccessat},
    {439, 439, Call::Faccessat},   {76, 45, Call::Truncate},    {77, 46, Call::Ftruncate},    {74, 82, Call::Fsync},
    {75, 83, Call::Fsync},         {32, 23, Call::Dup},         {33, kAbsent, Call::Dup2},    {292, 24, Call::Dup3},
    {72, 25, Call::Fcntl},         {89, kAbsent, Call::Readlink}, {332, 291, Call::Statx},    {95, 166, Call::Umask},
    {90, kAbsent, Call::Succeed},  {91, 52, Call::Succeed},     {268, 53, Call::Succeed},     {92, kAbsent, Call::Succeed},
    {93, 55, Call::Succeed},       {94, kAbsent, Call::Succeed}, {260, 54, Call::Succeed},    {280, 88, Call::Succeed},
    {162, 81, Call::Succeed},
    {56, 220, Call::Clone},        {435, 435, Call::Clone3},        {202, 98, Call::Futex},      {234, 131, Call::Tgkill},     {200, 130, Call::Tkill},     {204, 123, Call::SchedGetaffinity},
    {203, 122, Call::SchedSetaffinity}, {157, 167, Call::Prctl},
    {59, 221, Call::Execve},       {322, 281, Call::Execveat},
    // sockets (AF_UNIX: LinuxSockets.cpp)
    {22, kAbsent, Call::Pipe},     {293, 59, Call::Pipe2},      {284, kAbsent, Call::Eventfd}, {290, 19, Call::Eventfd2},
    {283, 85, Call::TimerfdCreate}, {286, 86, Call::TimerfdSettime}, {287, 87, Call::TimerfdGettime}, {282, kAbsent, Call::Signalfd},
    {289, 74, Call::Signalfd4},    {213, kAbsent, Call::EpollCreate}, {291, 20, Call::EpollCreate1}, {233, 21, Call::EpollCtl},
    {232, kAbsent, Call::EpollWait}, {281, 22, Call::EpollPwait}, {441, 441, Call::EpollPwait2},
    {41, 198, Call::Socket},       {53, 199, Call::Socketpair}, {49, 200, Call::Bind},       {42, 203, Call::Connect},
    {50, 201, Call::Listen},       {43, 202, Call::Accept},     {288, 242, Call::Accept4},   {44, 206, Call::Sendto},
    {45, 207, Call::Recvfrom},     {46, 211, Call::Sendmsg},    {47, 212, Call::Recvmsg},    {307, 269, Call::Sendmmsg},
    {299, 243, Call::Recvmmsg},    {48, 210, Call::Shutdown},   {51, 204, Call::Getsockname}, {52, 205, Call::Getpeername},
    {54, 208, Call::Setsockopt},   {55, 209, Call::Getsockopt}, {7, kAbsent, Call::Poll},    {271, 73, Call::Ppoll},
    {23, kAbsent, Call::Select},   {270, 72, Call::Pselect6},
    // files in memory (MemoryFile.hpp): what Wayland clients share their pictures through
    {319, 279, Call::MemfdCreate}, {285, 47, Call::Fallocate},
    // the volume of a file, hard links, whole-file locks (what Qt's QStorageInfo, QSaveFile and QLockFile use)
    {137, 43, Call::Statfs},       {138, 44, Call::Fstatfs},    {86, kAbsent, Call::Link},   {265, 37, Call::Linkat},
    {73, 32, Call::Flock},
    // more of the file calls (LinuxFiles.cpp)
    {85, kAbsent, Call::Creat},    {88, kAbsent, Call::Symlink}, {266, 36, Call::Symlinkat},  {295, 69, Call::Preadv},
    {296, 70, Call::Pwritev},      {327, 286, Call::Preadv2},   {328, 287, Call::Pwritev2},   {40, 71, Call::Sendfile},
    {326, 285, Call::CopyFileRange}, {275, 76, Call::Splice},   {276, 77, Call::Splice},      {278, 75, Call::Splice},
    {187, 213, Call::Readahead},   {221, 223, Call::Fadvise},   {277, 84, Call::SyncFileRange}, {306, 267, Call::Syncfs},
    {78, kAbsent, Call::Getdents}, {437, 437, Call::Openat2},   {436, 436, Call::CloseRange},
    {132, kAbsent, Call::Succeed}, {235, kAbsent, Call::Succeed}, {261, kAbsent, Call::Succeed}, {452, 452, Call::Succeed},      // utime, utimes, futimesat, fchmodat2
    {251, 30, Call::Succeed},      {252, 31, Call::Succeed},                                                                       // ioprio_set, ioprio_get
    // extended attributes (setxattr ... fremovexattr): no file system here keeps any
    {188, 5, Call::Xattr},         {189, 6, Call::Xattr},       {190, 7, Call::Xattr},        {191, 8, Call::Xattr},
    {192, 9, Call::Xattr},         {193, 10, Call::Xattr},      {194, 11, Call::Xattr},       {195, 12, Call::Xattr},
    {196, 13, Call::Xattr},        {197, 14, Call::Xattr},      {198, 15, Call::Xattr},       {199, 16, Call::Xattr},
    // what only an administrator of a real system does: chroot, mount, umount2, pivot_root, swapon, swapoff, reboot, sethostname, setdomainname, syslog, ptrace,
    // settimeofday, clock_settime, adjtimex, clock_adjtime, unshare, setns, acct
    {161, 51, Call::Refuse},       {165, 40, Call::Refuse},     {166, 39, Call::Refuse},      {155, 41, Call::Refuse},
    {167, 224, Call::Refuse},      {168, 225, Call::Refuse},    {169, 142, Call::Refuse},     {170, 161, Call::Refuse},
    {171, 162, Call::Refuse},      {103, 116, Call::Refuse},    {101, 117, Call::Refuse},     {164, 170, Call::Refuse},
    {227, 112, Call::Refuse},      {159, 171, Call::Refuse},    {305, 266, Call::Refuse},     {272, 97, Call::Refuse},
    {308, 268, Call::Refuse},      {163, 89, Call::Refuse},
    // signals (LinuxSignals.cpp)
    {127, 136, Call::RtSigpending}, {128, 137, Call::RtSigtimedwait}, {129, 138, Call::RtSigqueueinfo}, {297, 240, Call::RtTgsigqueueinfo},
    {130, 133, Call::RtSigsuspend}, {15, 139, Call::RtSigreturn}, {62, 129, Call::Kill},       {34, kAbsent, Call::Pause},
    {37, kAbsent, Call::Alarm},    {36, 102, Call::Getitimer},  {38, 103, Call::Setitimer},
    // processes (LinuxProcess.cpp)
    {57, kAbsent, Call::Fork},     {58, kAbsent, Call::Fork},   {61, 260, Call::Wait4},       {247, 95, Call::Waitid},
    {111, kAbsent, Call::Getpgrp}, {121, 155, Call::Getpgid},   {109, 154, Call::Setpgid},    {112, 157, Call::Setsid},
    {124, 156, Call::Getsid},      {105, 146, Call::Setuid},    {106, 144, Call::Setgid},     {113, 145, Call::Setreuid},
    {114, 143, Call::Setregid},    {117, 147, Call::Setresuid}, {119, 149, Call::Setresgid},  {122, 151, Call::Setfsuid},
    {123, 152, Call::Setfsgid},    {115, 158, Call::Getgroups}, {116, 159, Call::Setgroups},  {125, 90, Call::Capget},
    {126, 91, Call::Capset},       {97, 163, Call::Getrlimit},  {160, 164, Call::Setrlimit},  {98, 165, Call::Getrusage},
    {100, 153, Call::Times},       {99, 179, Call::Sysinfo},    {140, 141, Call::Getpriority}, {141, 140, Call::Setpriority},
    {142, 118, Call::SchedSetparam}, {143, 121, Call::SchedGetparam}, {144, 119, Call::SchedSetscheduler}, {145, 120, Call::SchedGetscheduler},
    {146, 125, Call::SchedPriorityMax}, {147, 126, Call::SchedPriorityMin}, {148, 127, Call::SchedRrInterval}, {314, 274, Call::SchedSetattr},
    {315, 275, Call::SchedGetattr}, {309, 168, Call::Getcpu},   {135, 92, Call::Personality}, {324, 283, Call::Membarrier},
    {274, 100, Call::GetRobustList},
    // more of memory
    {25, 216, Call::Mremap},       {26, 227, Call::Msync},      {27, 232, Call::Mincore},     {149, 228, Call::Mlock},
    {150, 229, Call::Mlock},       {325, 284, Call::Mlock},     {151, 230, Call::Mlockall},   {152, 231, Call::Mlockall},
    // POSIX timers (LinuxSignals.cpp)
    {222, 107, Call::TimerCreate}, {223, 110, Call::TimerSettime}, {224, 108, Call::TimerGettime}, {225, 109, Call::TimerGetoverrun}, {226, 111, Call::TimerDelete},
    // futex_waitv, futex_wake, futex_wait, futex_requeue (LinuxThreads.cpp)
    {449, 449, Call::FutexWaitv},  {454, 454, Call::FutexWake2}, {455, 455, Call::FutexWait2}, {456, 456, Call::FutexRequeue2},
    // memory policy, protection keys, kcmp, another process's memory, cachestat (LinuxMisc.cpp)
    {239, 236, Call::GetMempolicy}, {238, 237, Call::SetMempolicy}, {237, 235, Call::Mbind},  {450, 450, Call::SetMempolicyHomeNode},
    {256, 238, Call::MigratePages}, {279, 239, Call::MovePages}, {330, 289, Call::PkeyAlloc}, {331, 290, Call::PkeyFree},
    {329, 288, Call::PkeyMprotect}, {312, 272, Call::Kcmp},     {310, 270, Call::ProcessVmReadv}, {311, 271, Call::ProcessVmWritev},
    {451, 451, Call::Cachestat},   {219, 128, Call::RestartSyscall},
    // System V shared memory, semaphores and message queues (LinuxIpc.cpp)
    {29, 194, Call::Shmget},       {30, 196, Call::Shmat},      {67, 197, Call::Shmdt},       {31, 195, Call::Shmctl},
    {64, 190, Call::Semget},       {65, 193, Call::Semop},      {220, 192, Call::Semtimedop}, {66, 191, Call::Semctl},
    {68, 186, Call::Msgget},       {69, 189, Call::Msgsnd},     {70, 188, Call::Msgrcv},      {71, 187, Call::Msgctl},
    // POSIX message queues (LinuxIpc.cpp)
    {240, 180, Call::MqOpen},      {241, 181, Call::MqUnlink},  {242, 182, Call::MqTimedsend}, {243, 183, Call::MqTimedreceive},
    {244, 184, Call::MqNotify},    {245, 185, Call::MqGetsetattr},
    // inotify (LinuxInotify.cpp)
    {253, kAbsent, Call::InotifyInit}, {294, 26, Call::InotifyInit1}, {254, 27, Call::InotifyAddWatch}, {255, 28, Call::InotifyRmWatch},
    // asynchronous I/O (LinuxAio.cpp): io_setup, io_destroy, io_submit, io_cancel, io_getevents, io_pgetevents
    {206, 0, Call::IoSetup},       {207, 1, Call::IoDestroy},   {209, 2, Call::IoSubmit},     {210, 3, Call::IoCancel},
    {208, 4, Call::IoGetevents},   {333, 292, Call::IoGetevents},      // (io_pgetevents: its signal mask is not put in place for the wait)
    // pidfd (LinuxProcess.cpp), and the two calls that take one to say whose memory is meant
    {434, 434, Call::PidfdOpen},   {424, 424, Call::PidfdSendSignal}, {438, 438, Call::PidfdGetfd}, {440, 440, Call::ProcessMadvise},
    {448, 448, Call::ProcessMrelease},
    // more of what only an administrator of a real system does (EPERM): vhangup, iopl, ioperm, init_module, finit_module, delete_module, kexec_load,
    // kexec_file_load, and the newer mount calls open_tree, move_mount, fsopen, fsconfig, fsmount, fspick, mount_setattr, open_tree_attr, and fchroot
    {153, 58, Call::Refuse},       {172, kAbsent, Call::Refuse}, {173, kAbsent, Call::Refuse}, {175, 105, Call::Refuse},
    {313, 273, Call::Refuse},      {176, 106, Call::Refuse},    {246, 104, Call::Refuse},     {320, 294, Call::Refuse},
    {428, 428, Call::Refuse},      {429, 429, Call::Refuse},    {430, 430, Call::Refuse},     {431, 431, Call::Refuse},
    {432, 432, Call::Refuse},      {433, 433, Call::Refuse},    {442, 442, Call::Refuse},     {467, 467, Call::Refuse},
    {472, 472, Call::Refuse},
    // Calls for something no file system or processor here has (EOPNOTSUPP, which is what Linux answers then): file handles (name_to_handle_at,
    // open_by_handle_at), the *xattrat calls, file_getattr and file_setattr (inode flags), map_shadow_stack
    {303, 264, Call::Unsupported}, {304, 265, Call::Unsupported}, {463, 463, Call::Unsupported}, {464, 464, Call::Unsupported},
    {465, 465, Call::Unsupported}, {466, 466, Call::Unsupported}, {468, 468, Call::Unsupported}, {469, 469, Call::Unsupported},
    {453, 453, Call::Unsupported},
    // Parts of Linux that are not here, answered as a kernel built without them answers (ENOSYS, and programs fall back): io_uring (setup, enter,
    // register), fanotify (init, mark), the key retention service (add_key, request_key, keyctl), landlock (three calls), the security-module calls
    // (lsm_get_self_attr, lsm_set_self_attr, lsm_list_modules), memfd_secret, mseal, statmount, listmount, listns, seccomp, remap_file_pages, quotactl,
    // quotactl_fd, bpf, perf_event_open, userfaultfd, rseq_slice_yield
    {425, 425, Call::Absent},      {426, 426, Call::Absent},    {427, 427, Call::Absent},     {300, 262, Call::Absent},
    {301, 263, Call::Absent},      {248, 217, Call::Absent},    {249, 218, Call::Absent},     {250, 219, Call::Absent},
    {444, 444, Call::Absent},      {445, 445, Call::Absent},    {446, 446, Call::Absent},     {459, 459, Call::Absent},
    {460, 460, Call::Absent},      {461, 461, Call::Absent},    {447, 447, Call::Absent},     {462, 462, Call::Absent},
    {457, 457, Call::Absent},      {458, 458, Call::Absent},    {470, 470, Call::Absent},     {317, 277, Call::Absent},
    {216, 234, Call::Absent},      {179, 60, Call::Absent},     {443, 443, Call::Absent},     {321, 280, Call::Absent},
    {298, 241, Call::Absent},      {323, 282, Call::Absent},    {471, 471, Call::Absent},
    // Numbers Linux itself answers ENOSYS for, because the call was never there or was taken out: afs_syscall, getpmsg, putpmsg, security, tuxcall,
    // vserver, create_module, get_kernel_syms, query_module, nfsservctl, _sysctl, uselib, epoll_ctl_old, epoll_wait_old, lookup_dcookie; and what a
    // 64-bit program has no use for or a current kernel is built without: sysfs, ustat, modify_ldt, set_thread_area, get_thread_area, uprobe, uretprobe
    {183, kAbsent, Call::Absent},  {181, kAbsent, Call::Absent}, {182, kAbsent, Call::Absent}, {185, kAbsent, Call::Absent},
    {184, kAbsent, Call::Absent},  {236, kAbsent, Call::Absent}, {174, kAbsent, Call::Absent}, {177, kAbsent, Call::Absent},
    {178, kAbsent, Call::Absent},  {180, 42, Call::Absent},     {156, kAbsent, Call::Absent}, {134, kAbsent, Call::Absent},
    {214, kAbsent, Call::Absent},  {215, kAbsent, Call::Absent}, {212, 18, Call::Absent},     {139, kAbsent, Call::Absent},
    {136, kAbsent, Call::Absent},  {154, kAbsent, Call::Absent}, {205, kAbsent, Call::Absent}, {211, kAbsent, Call::Absent},
    {336, kAbsent, Call::Absent},  {335, kAbsent, Call::Absent},
};

uint64_t error(int64_t errno_value) { return uint64_t(-errno_value); }

Call translate(UserArch arch, uint64_t number) {
  for (const NumberEntry& entry : kNumbers) {
    const uint16_t wanted = arch == UserArch::X86_64 ? entry.x86_64 : entry.generic;
    if (wanted != kAbsent && number == wanted) return entry.call;
  }
  return Call::None;
}

StringView machine_name(UserArch arch) {
  switch (arch) {
    case UserArch::X86_64: return "x86_64";
    case UserArch::AArch64: return "aarch64";
    case UserArch::LoongArch64: return "loongarch64";
    default: return "riscv64";
  }
}


constexpr uint64_t kMapShared = 0x1, kMapPrivate = 0x2, kMapFixed = 0x10, kMapAnonymous = 0x20;
constexpr uint64_t kMapOtherAccepted = 0x100 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x40000;      // GROWSDOWN, DENYWRITE, NORESERVE, POPULATE, NONBLOCK, STACK, HUGETLB: hints we may ignore
constexpr uint64_t kMapFixedNoreplace = 0x100000;      // MAP_FIXED_NOREPLACE: at exactly that address, EEXIST if anything is mapped there

bool valid_protection(uint64_t prot) { return (prot & ~uint64_t{kProtRead | kProtWrite | kProtExecute}) == 0; }

// brk(addr): moves the program break within the heap region right after the image. Like Linux, a request it cannot meet returns the unchanged break.
uint64_t do_brk(Process& process, uint64_t request) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  const uint64_t start = process.heap_start(), current = process.heap_break();
  if (request < start) return current;      // includes 0: "where is it?"
  uint64_t old_end = 0, new_end = 0;
  if (!AddressSpace::round_up(current, &old_end) || !AddressSpace::round_up(request, &new_end)) return current;
  if (new_end == old_end) { process.set_heap_break(request); return request; }
  const uint64_t old_size = old_end - start, new_size = new_end - start;
  bool ok;
  if (old_size == 0) ok = space->map(start, new_size, kProtRead | kProtWrite);
  else ok = space->resize(start, new_size);
  if (!ok) return current;
  process.set_heap_break(request);
  return request;
}

// mmap() of a file in memory (memfd_create): its block is mapped as it is (MAP_SHARED), or as a copy-on-write view (MAP_PRIVATE). The pages past the
// block's end cannot be mapped (ENXIO; Linux would map them and fault on use). A GPU image (a dmabuf of /dev/dxg) maps the CPU's copy of it the same way.
// A regular file (a library ld.so maps, a data file): the pages are a copy of the file made now, private to the mapping. Bytes past the end of the file
// are zero. A shared mapping that is only read is the same copy (what another process writes to the file later is not seen); a shared mapping for
// writing is not supported (ENODEV), as before.
uint64_t map_file_copy(Process& process, AddressSpace& space, OpenObject& object, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t offset) {
  if (!object.file) return error(kEnodev);      // (a directory, a link opened itself)
  if (!object.readable || object.no_data_access) return error(kEacces);
  if ((flags & kMapShared) && (prot & kProtWrite)) return error(kEnodev);
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space.unmap(address, size)) return error(kEnomem);
    where = address;
  } else if (!space.find_free(size, address ? address : process.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  if (!space.map(where, size, uint8_t(prot))) return error(kEnomem);
  {      // (told apart from anonymous memory: madvise(MADV_DONTNEED) must not turn a file's pages into zeros)
    PageAttributes attributes;
    attributes.type = PageAttributes::Mapped;
    attributes.file_view = true;
    space.set_attributes(where, size, attributes);
  }
  const uint64_t file_size = object.file->size();
  const uint64_t available = offset < file_size ? file_size - offset : 0;
  const uint64_t copy = available < size ? available : size;
  uint8_t chunk[16384];
  for (uint64_t done = 0; done < copy;) {
    const uint64_t want = copy - done < sizeof(chunk) ? copy - done : sizeof(chunk);
    uint64_t got = 0;
    if (object.file->read(offset + done, ByteView(chunk, want), &got) != filesystem::Status::Ok || got == 0 ||
        !space.write_privileged(where + done, ConstByteView(chunk, got))) {
      space.unmap(where, size);
      return error(kEnomem);
    }
    done += got;
  }
  if (!(flags & kMapFixed) && !address) process.set_mmap_cursor(where + size);
  return where;
}

// mmap() of an AF_PACKET socket: its rings (LinuxPacketRing.hpp), the receive ring and then the transmit ring, as one run of memory the program and the
// socket share. The size has to be exactly theirs, as on Linux.
uint64_t map_packet_rings(Process& process, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t link, uint64_t offset) {
  BlockRef blocks[2];
  uint64_t sizes[2] = {};
  const uint32_t count = linuxabi::packet_rings(link, blocks, sizes);
  if (count == 0 || offset != 0 || size != sizes[0] + sizes[1]) return error(kEinval);
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space.unmap(address, size)) return error(kEnomem);
    where = address;
  } else if (!space.find_free(size, address ? address : process.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  uint64_t done = 0;
  for (uint32_t i = 0; i < count; ++i) {      // (each mapping holds its block from here on)
    if (!space.map_shared(where + done, sizes[i], uint8_t(prot), blocks[i].get()->storage(), blocks[i].get())) {
      if (done != 0) (void)space.unmap(where, done);
      return error(kEnomem);
    }
    done += sizes[i];
  }
  if (!(flags & kMapFixed) && !address) process.set_mmap_cursor(where + size);
  return where;
}

uint64_t map_memory_file(Process& process, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
  if (uint64_t link = 0; linuxabi::packet_descriptor(process, fd, &link)) return map_packet_rings(process, space, address, size, prot, flags, link, offset);
  if (uint64_t mapped = 0; linuxabi::render_node_map(process, space, address, size, prot, flags, fd, offset, &mapped)) return mapped;      // (a buffer object: LinuxRenderNode.hpp)
  OpenObject* object = process.files().object(fd);
  if (!object) return error(kEbadf);
  if (offset % AddressSpace::kPageSize != 0) return error(kEinval);
  if (!object->memory && !object->gpu_buffer) return map_file_copy(process, space, *object, address, size, prot, flags, offset);
  const bool shared_write = (flags & kMapShared) && (prot & kProtWrite);
  BlockRef block;
  if (object->gpu_buffer) {      // a GPU image: the CPU's copy of it (DxgBufferFile.hpp); a dmabuf is mapped for writing whatever its descriptor allows, as on Linux
    block = object->gpu_buffer->map_block(shared_write);
    if (!block) return error(kEnodev);
  } else {
    if (!object->readable || (shared_write && !object->writable)) return error(kEacces);
    if (shared_write && (object->memory->seals() & (MemoryFile::kSealWrite | MemoryFile::kSealFutureWrite))) return error(kEperm);
    uint64_t file_size = 0;
    block = object->memory->block(&file_size);
  }
  if (!block || offset >= block.get()->capacity() || size > block.get()->capacity() - offset) return error(kEnxio);
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space.unmap(address, size)) return error(kEnomem);
    where = address;
  } else if (!space.find_free(size, address ? address : process.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  if (!space.map_shared(where, size, uint8_t(prot), block.get()->storage() + offset, block.get())) return error(kEnomem);
  if (flags & kMapPrivate) {      // (written pages get copies of their own: AddressSpace::keeps_private_copies)
    PageAttributes attributes;
    attributes.type = PageAttributes::Mapped;
    attributes.protect = attributes.allocation_protect = 0x08;      // PAGE_WRITECOPY
    space.set_attributes(where, size, attributes);
  }
  if (!(flags & kMapFixed) && !address) process.set_mmap_cursor(where + size);
  return where;
}

// MAP_SHARED | MAP_ANONYMOUS: memory that a child made by fork() goes on sharing with its parent (how programs that fork pass results back). It is one block of
// memory, there from the start (a private mapping's pages come one by one as they are touched), which every copy of the mapping holds a use of. Larger
// requests than this are given private memory, as before: nothing could be shared with them anyway until a fork, and reserving that much at once is worse.
constexpr uint64_t kMaxSharedAnonymous = 64ull << 20;

uint64_t map_shared_anonymous(Process& process, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags) {
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    where = address;
  } else if (!space.find_free(size, address ? address : process.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  BlockRef block(MemoryBlock::make(space.allocator(), size));
  if (!block) return error(kEnomem);
  if ((flags & kMapFixed) && !space.unmap(where, size)) return error(kEnomem);
  if (!space.map_shared(where, size, uint8_t(prot), block.get()->storage(), block.get())) return error(kEnomem);      // (the mapping holds the block from here on)
  if (!(flags & kMapFixed) && !address) process.set_mmap_cursor(where + size);
  return where;
}

uint64_t do_mmap(Process& process, uint64_t address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  uint64_t size = 0;
  if (length == 0 || !AddressSpace::round_up(length, &size) || !valid_protection(prot)) return error(kEinval);
  if (flags & ~(kMapShared | kMapPrivate | kMapFixed | kMapAnonymous | kMapFixedNoreplace | kMapOtherAccepted)) return error(kEinval);
  if (((flags & kMapShared) != 0) == ((flags & kMapPrivate) != 0)) return error(kEinval);      // exactly one of the two
  if (flags & kMapFixedNoreplace) {
    uint64_t free_at = 0;
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space->find_free(size, address, &free_at) || free_at != address) return error(kEexist);
    flags = (flags & ~kMapFixedNoreplace) | kMapFixed;
  }
  if (!(flags & kMapAnonymous)) return map_memory_file(process, *space, address, size, prot, flags, fd, offset);
  if ((flags & kMapShared) && size <= kMaxSharedAnonymous) return map_shared_anonymous(process, *space, address, size, prot, flags);
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % AddressSpace::kPageSize != 0) return error(kEinval);
    if (!space->unmap(address, size) || !space->map(address, size, uint8_t(prot))) return error(kEnomem);
    return address;
  }
  if (!space->find_free(size, address ? address : process.mmap_cursor(), &where)) return error(kEnomem);
  if (!space->map(where, size, uint8_t(prot))) return error(kEnomem);
  if (!address) process.set_mmap_cursor(where + size);
  return where;
}

uint64_t do_munmap(Process& process, uint64_t address, uint64_t length) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  if (length == 0 || address % AddressSpace::kPageSize != 0) return error(kEinval);
  return space->unmap(address, length) ? 0 : error(kEinval);
}

uint64_t do_mprotect(Process& process, uint64_t address, uint64_t length, uint64_t prot) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  if (address % AddressSpace::kPageSize != 0 || !valid_protection(prot)) return error(kEinval);
  if (length == 0) return 0;
  return space->protect(address, length, uint8_t(prot)) ? 0 : error(kEnomem);      // part of the range is not mapped
}

// mremap(old, old_size, new_size, flags, new_address): shrinking lets the tail go; growing takes the pages after the mapping when they are free, and
// otherwise (with MREMAP_MAYMOVE) moves the mapping, pages and all, to a free place. A view of shared memory (a file in memory) cannot grow: the program maps
// the file again, as it must after the file itself grew. MREMAP_FIXED and MREMAP_DONTUNMAP are not supported.
uint64_t do_mremap(Process& process, uint64_t old_address, uint64_t old_length, uint64_t new_length, uint64_t flags) {
  constexpr uint64_t kMayMove = 1;
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  uint64_t old_size = 0, new_size = 0;
  if (flags & ~kMayMove) return error(kEinval);
  if (old_address % AddressSpace::kPageSize != 0 || old_length == 0 || new_length == 0) return error(kEinval);
  if (!AddressSpace::round_up(old_length, &old_size) || !AddressSpace::round_up(new_length, &new_size)) return error(kEinval);
  if (old_address >= AddressSpace::kUserTop || old_size > AddressSpace::kUserTop - old_address || !space->check(old_address, old_size, 0)) return error(kEfault);
  if (new_size == old_size) return old_address;
  if (new_size < old_size) return space->unmap(old_address + new_size, old_size - new_size) ? old_address : error(kEnomem);
  if (space->is_shared(old_address) || space->is_shared(old_address + old_size - AddressSpace::kPageSize)) return error(kEnomem);
  uint8_t protection = 0;
  PageAttributes attributes;
  if (!space->protection_at(old_address + old_size - AddressSpace::kPageSize, &protection) || !space->attributes_at(old_address, &attributes)) return error(kEfault);
  const uint64_t extra = new_size - old_size, old_end = old_address + old_size;
  uint64_t free_at = 0;
  if (space->find_free(extra, old_end, &free_at) && free_at == old_end) {      // room right behind it
    if (!space->map(old_end, extra, protection)) return error(kEnomem);
    if (attributes.type != PageAttributes::Private) space->set_attributes(old_end, extra, attributes);
    return old_address;
  }
  if (!(flags & kMayMove)) return error(kEnomem);
  uint64_t where = 0;
  if (!space->find_free(new_size, process.mmap_cursor(), &where) || !space->map(where, new_size, protection)) return error(kEnomem);
  // The regions of the old mapping keep their permissions at the new place; the pages that have memory are copied (the others read as zeros at both).
  bool ok = true;
  for (uint64_t i = space->region_index(old_address);; ++i) {
    AddressSpace::RegionInfo info;
    if (!space->region_at(i, &info) || info.base >= old_end) break;
    const uint64_t from = info.base > old_address ? info.base : old_address;
    const uint64_t to = info.base + info.size < old_end ? info.base + info.size : old_end;
    if (info.protection != protection) ok = ok && space->protect(where + (from - old_address), to - from, info.protection);
  }
  if (attributes.type != PageAttributes::Private) ok = ok && space->set_attributes(where, new_size, attributes);
  space->for_each_private_page(old_address, old_end, [&](uint64_t page, const uint8_t* memory) {
    if (!ok) return;
    uint8_t* copy = space->materialize(where + (page - old_address));
    ok = copy && ByteView(copy, AddressSpace::kPageSize).copy_from(ConstByteView(memory, AddressSpace::kPageSize));
  });
  if (!ok) {
    space->unmap(where, new_size);
    return error(kEnomem);
  }
  space->unmap(old_address, old_size);
  process.set_mmap_cursor(where + new_size);
  return where;
}

// Is [address, address + length) a range of whole mapped pages (what msync, mlock and mincore want)? 0, or the error.
uint64_t check_mapped(Process& process, uint64_t address, uint64_t length, uint64_t* size) {
  AddressSpace* space = process.address_space();
  if (!space) return error(kEnomem);
  if (address % AddressSpace::kPageSize != 0 || !AddressSpace::round_up(length, size)) return error(kEinval);
  if (*size == 0) return 0;
  if (address >= AddressSpace::kUserTop || *size > AddressSpace::kUserTop - address || !space->check(address, *size, 0)) return error(kEnomem);
  return 0;
}

// msync: a view of shared memory is written back to where it came from (nothing to do for a file in memory); private copies of a file are not the file's.
uint64_t do_msync(Process& process, uint64_t address, uint64_t length, uint64_t flags) {
  constexpr uint64_t kAsync = 1, kInvalidate = 2, kSync = 4;
  if ((flags & ~(kAsync | kInvalidate | kSync)) || ((flags & kAsync) && (flags & kSync))) return error(kEinval);
  uint64_t size = 0;
  if (const uint64_t r = check_mapped(process, address, length, &size); r != 0) return r;
  if (size != 0)
    if (SharedMemory* shared = process.address_space()->shared_at(address)) shared->flush();
  return 0;
}

// mincore(address, length, vec): one byte a page, 1 when the page has memory (nothing is ever paged out, so that is "in core").
uint64_t do_mincore(Process& process, uint64_t address, uint64_t length, uint64_t vector) {
  uint64_t size = 0;
  if (const uint64_t r = check_mapped(process, address, length, &size); r != 0) return r;
  AddressSpace& space = *process.address_space();
  uint8_t chunk[256];
  for (uint64_t page = 0, pages = size / AddressSpace::kPageSize; page < pages;) {
    const uint64_t n = pages - page < sizeof(chunk) ? pages - page : sizeof(chunk);
    for (uint64_t i = 0; i < n; ++i) chunk[i] = space.page_memory(address + (page + i) * AddressSpace::kPageSize) ? 1 : 0;
    if (!process.write_user(vector + page, ConstByteView(chunk, n))) return error(kEfault);
    page += n;
  }
  return 0;
}

// madvise: advice the kernel may ignore, except MADV_DONTNEED, which a program may count on: anonymous private pages read as zeros afterwards (allocators give
// memory back this way and expect it clean when they take it again). Views of shared memory and copies of files keep their bytes.
uint64_t do_madvise(Process& process, uint64_t address, uint64_t length, uint64_t advice);

// process_madvise(pidfd, struct iovec*, count, advice, flags): madvise for each range, in a process named by a descriptor. Only in the caller's own.
uint64_t process_madvise(Process& process, uint64_t pidfd, uint64_t vectors, uint64_t count, uint64_t advice, uint64_t flags) {
  uint64_t pid = 0;
  if (!linuxabi::pidfd_process(process, pidfd, &pid)) return error(kEbadf);
  if (flags != 0 || count > 1024) return error(kEinval);
  if (pid != process.id()) return error(kEperm);
  uint64_t done = 0;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t base = 0, length = 0;
    if (!process.read_user_u64(vectors + i * 16, &base) || !process.read_user_u64(vectors + i * 16 + 8, &length)) return done != 0 ? done : error(kEfault);
    if (const uint64_t r = do_madvise(process, base, length, advice); r != 0) return done != 0 ? done : r;
    done += length;
  }
  return done;
}

uint64_t do_madvise(Process& process, uint64_t address, uint64_t length, uint64_t advice) {
  constexpr uint64_t kDontNeed = 4;
  AddressSpace* space = process.address_space();
  if (!space || advice != kDontNeed) return 0;
  uint64_t size = 0;
  if (const uint64_t r = check_mapped(process, address, length, &size); r != 0) return r;
  const uint64_t end = address + size;
  for (uint64_t at = address; at < end;) {      // (looked up again each time: discarding may cut regions)
    AddressSpace::RegionInfo info;
    if (!space->region_at(space->region_index(at), &info) || info.base >= end) break;
    const uint64_t from = info.base > at ? info.base : at;
    const uint64_t to = info.base + info.size < end ? info.base + info.size : end;
    if (!info.storage && info.attributes.type == PageAttributes::Private) space->discard(from, to - from);
    at = to;
  }
  return 0;
}

// --- thread pointer, terminal-ish and signal calls that C libraries make while starting up -------------------------------------------------------------

constexpr uint64_t kArchSetGs = 0x1001, kArchSetFs = 0x1002, kArchGetFs = 0x1003, kArchGetGs = 0x1004;

uint64_t do_arch_prctl(Process& process, uint64_t code, uint64_t address) {
  switch (code) {
    case kArchSetFs:
    case kArchSetGs:
      if (address >= AddressSpace::kUserTop) return error(kEinval);      // must be a user address (this also keeps it canonical)
      if (code == kArchSetFs) process.set_fs_base(address);
      else process.set_gs_base(address);
      return 0;
    case kArchGetFs: return process.write_user_u64(address, process.fs_base()) ? 0 : error(kEfault);
    case kArchGetGs: return process.write_user_u64(address, process.gs_base()) ? 0 : error(kEfault);
    default: return error(kEinval);
  }
}

uint64_t do_getrandom(Process& process, uint64_t address, uint64_t length) {
  if (length > kMaxIoBytes) length = kMaxIoBytes;
  uint8_t chunk[256];
  uint64_t done = 0;
  while (done < length) {
    const uint64_t n = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
    if (!process.host().random_bytes(ByteView(chunk, n))) return done ? done : error(kEnosys);
    if (!process.write_user(address + done, ConstByteView(chunk, n))) return done ? done : error(kEfault);
    done += n;
  }
  return done;
}

uint64_t do_gettimeofday(Process& process, uint64_t tv) {
  if (!tv) return 0;
  const uint64_t ns = process.host().realtime_ns();
  return process.write_user_u64(tv, ns / 1000000000u) && process.write_user_u64(tv + 8, ns % 1000000000u / 1000) ? 0 : error(kEfault);
}


}  // namespace

class LinuxSyscalls final : public ISyscallInterface {
 public:
  explicit constexpr LinuxSyscalls(UserArch arch) : arch_(arch) {}
  SyscallAbi abi() const override { return SyscallAbi::Linux; }
  UserArch arch() const override { return arch_; }

  uint64_t dispatch(Process& process, const SyscallRequest& r) const override {
    const uint64_t* a = r.args;
    linuxabi::check_timers(process);      // (alarm, setitimer: a signal that is due is raised before the call)
    if (process.exited()) return 0;
    switch (translate(arch_, r.number)) {
      case Call::Read: return linuxabi::sys_read(process, a[0], a[1], a[2]);
      case Call::Write: return linuxabi::sys_write(process, a[0], a[1], a[2]);
      case Call::Writev: return linuxabi::sys_writev(process, a[0], a[1], a[2]);
      case Call::Close: return process.close_descriptor(a[0]) ? 0 : error(kEbadf);
      case Call::Getpid: return process.id();
      case Call::Gettid: return linuxabi::sys_gettid(process);
      case Call::Getuid: return linuxabi::sys_id(process, linuxabi::IdCall::Getuid, 0, 0, 0);
      case Call::Geteuid: return linuxabi::sys_id(process, linuxabi::IdCall::Geteuid, 0, 0, 0);
      case Call::Getgid: return linuxabi::sys_id(process, linuxabi::IdCall::Getgid, 0, 0, 0);
      case Call::Getegid: return linuxabi::sys_id(process, linuxabi::IdCall::Getegid, 0, 0, 0);
      case Call::Setuid: return linuxabi::sys_id(process, linuxabi::IdCall::Setuid, a[0], 0, 0);
      case Call::Setgid: return linuxabi::sys_id(process, linuxabi::IdCall::Setgid, a[0], 0, 0);
      case Call::Setreuid: return linuxabi::sys_id(process, linuxabi::IdCall::Setreuid, a[0], a[1], 0);
      case Call::Setregid: return linuxabi::sys_id(process, linuxabi::IdCall::Setregid, a[0], a[1], 0);
      case Call::Setresuid: return linuxabi::sys_id(process, linuxabi::IdCall::Setresuid, a[0], a[1], a[2]);
      case Call::Setresgid: return linuxabi::sys_id(process, linuxabi::IdCall::Setresgid, a[0], a[1], a[2]);
      case Call::Getresuid: return linuxabi::sys_id(process, linuxabi::IdCall::Getresuid, a[0], a[1], a[2]);
      case Call::Getresgid: return linuxabi::sys_id(process, linuxabi::IdCall::Getresgid, a[0], a[1], a[2]);
      case Call::Setfsuid: return linuxabi::sys_id(process, linuxabi::IdCall::Setfsuid, a[0], 0, 0);
      case Call::Setfsgid: return linuxabi::sys_id(process, linuxabi::IdCall::Setfsgid, a[0], 0, 0);
      case Call::Getgroups: return linuxabi::sys_getgroups(process, a[0], a[1]);
      case Call::Setgroups: return linuxabi::sys_setgroups(process, a[0], a[1]);
      case Call::Getpgrp: return linuxabi::sys_getpgid(process, 0);
      case Call::Getpgid: return linuxabi::sys_getpgid(process, a[0]);
      case Call::Setpgid: return linuxabi::sys_setpgid(process, a[0], a[1]);
      case Call::Getsid: return linuxabi::sys_getsid(process, a[0]);
      case Call::Setsid: return linuxabi::sys_setsid(process);
      case Call::Capget: return linuxabi::sys_capget(process, a[0], a[1]);
      case Call::Capset: return linuxabi::sys_capset(process, a[0], a[1]);
      case Call::Fork: return linuxabi::sys_fork(process, linuxabi::ForkOptions());      // (fork and vfork)
      case Call::Wait4: return linuxabi::sys_wait4(process, a[0], a[1], a[2], a[3]);
      case Call::Waitid: return linuxabi::sys_waitid(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Getrlimit: return linuxabi::sys_prlimit(process, 0, a[0], 0, a[1]);
      case Call::Setrlimit: return a[1] == 0 ? error(kEfault) : linuxabi::sys_prlimit(process, 0, a[0], a[1], 0);
      case Call::Getrusage: return linuxabi::sys_getrusage(process, a[0], a[1]);
      case Call::Times: return linuxabi::sys_times(process, a[0]);
      case Call::Sysinfo: return linuxabi::sys_sysinfo(process, a[0]);
      case Call::Getpriority: return linuxabi::sys_getpriority(process, a[0], a[1]);
      case Call::Setpriority: return linuxabi::sys_setpriority(process, a[0], a[1], a[2]);
      case Call::SchedSetparam: return linuxabi::sys_sched(process, linuxabi::SchedCall::SetParam, a[0], a[1], 0, 0);
      case Call::SchedGetparam: return linuxabi::sys_sched(process, linuxabi::SchedCall::GetParam, a[0], a[1], 0, 0);
      case Call::SchedSetscheduler: return linuxabi::sys_sched(process, linuxabi::SchedCall::SetScheduler, a[0], a[1], a[2], 0);
      case Call::SchedGetscheduler: return linuxabi::sys_sched(process, linuxabi::SchedCall::GetScheduler, a[0], 0, 0, 0);
      case Call::SchedPriorityMax: return linuxabi::sys_sched(process, linuxabi::SchedCall::PriorityMax, a[0], 0, 0, 0);
      case Call::SchedPriorityMin: return linuxabi::sys_sched(process, linuxabi::SchedCall::PriorityMin, a[0], 0, 0, 0);
      case Call::SchedRrInterval: return linuxabi::sys_sched(process, linuxabi::SchedCall::RrInterval, a[0], a[1], 0, 0);
      case Call::SchedSetattr: return linuxabi::sys_sched(process, linuxabi::SchedCall::SetAttr, a[0], a[1], a[2], 0);
      case Call::SchedGetattr: return linuxabi::sys_sched(process, linuxabi::SchedCall::GetAttr, a[0], a[1], a[2], a[3]);
      case Call::SchedSetaffinity: return linuxabi::sys_sched_setaffinity(process, a[0], a[1], a[2]);
      case Call::Getcpu: return linuxabi::sys_getcpu(process, a[0], a[1]);
      case Call::Personality: return linuxabi::sys_personality(process, a[0]);
      case Call::Membarrier: return linuxabi::sys_membarrier(process, a[0], a[1]);
      case Call::GetRobustList: return linuxabi::sys_get_robust_list(process, a[0], a[1], a[2]);
      case Call::ClockGetres: {      // every clock counts in nanoseconds (libdbus asks, for its timeouts)
        if (a[0] > 11) return error(kEinval);
        if (a[1] && (!process.write_user_u64(a[1], 0) || !process.write_user_u64(a[1] + 8, 1))) return error(kEfault);
        return 0;
      }
      case Call::SetTidAddress: return linuxabi::sys_set_tid_address(process, a[0]);
      case Call::SchedYield: process.block_yield(); return 0;
      case Call::Exit: return linuxabi::sys_thread_exit(process, uint32_t(a[0] & 0xFF));      // (a thread; the last one takes the process with it)
      case Call::ExitGroup: process.terminate(uint32_t(a[0] & 0xFF)); return 0;
      case Call::Clone: return linuxabi::sys_clone(process, arch_, a);
      case Call::Clone3: return linuxabi::sys_clone3(process, arch_, a[0], a[1]);
      case Call::Execve: return linuxabi::sys_execve(process, kAtFdCwd, a[0], a[1], a[2], 0);
      case Call::Execveat: return linuxabi::sys_execve(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Futex: return linuxabi::sys_futex(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Tgkill: return linuxabi::sys_tgkill(process, a[0], a[1], a[2]);
      case Call::RtTgsigqueueinfo: return linuxabi::sys_tgkill(process, a[0], a[1], a[2]);      // (the siginfo that comes with it is not kept)
      case Call::Tkill: return linuxabi::sys_tgkill(process, process.id(), a[0], a[1]);
      case Call::Kill: return linuxabi::sys_kill(process, a[0], a[1]);
      case Call::RtSigqueueinfo: return linuxabi::sys_kill(process, a[0], a[1]);
      case Call::RtSigpending: return linuxabi::sys_rt_sigpending(process, a[0], a[1]);
      case Call::RtSigtimedwait: return linuxabi::sys_rt_sigtimedwait(process, a[0], a[1], a[2], a[3]);
      case Call::RtSigsuspend: return linuxabi::sys_rt_sigsuspend(process, a[0], a[1], true);
      case Call::Pause: return linuxabi::sys_rt_sigsuspend(process, 0, 0, false);
      case Call::RtSigreturn: return linuxabi::sys_rt_sigreturn(process);
      case Call::Alarm: return linuxabi::sys_alarm(process, a[0]);
      case Call::Getitimer: return linuxabi::sys_getitimer(process, a[0], a[1]);
      case Call::Setitimer: return linuxabi::sys_setitimer(process, a[0], a[1], a[2]);
      case Call::SchedGetaffinity: return linuxabi::sys_sched_getaffinity(process, a[1], a[2]);
      case Call::Prctl: return linuxabi::sys_prctl(process, arch_, a[0], a[1], a[2]);
      case Call::Uname: return write_uname(process, a[0]);
      case Call::ClockGettime: return clock_gettime(process, a[0], a[1]);
      case Call::Nanosleep: return nanosleep(process, a[0], a[1]);
      case Call::ClockNanosleep: return clock_nanosleep(process, a[0], a[1], a[2]);
      case Call::Brk: return do_brk(process, a[0]);
      case Call::Mmap: return do_mmap(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Munmap: return do_munmap(process, a[0], a[1]);
      case Call::Mprotect: return do_mprotect(process, a[0], a[1], a[2]);
      case Call::Mremap: return do_mremap(process, a[0], a[1], a[2], a[3]);
      case Call::Msync: return do_msync(process, a[0], a[1], a[2]);
      case Call::Mincore: return do_mincore(process, a[0], a[1], a[2]);
      case Call::Mlock: {      // mlock, munlock, mlock2: nothing is ever paged out; the range only has to be mapped
        uint64_t size = 0;
        return check_mapped(process, a[0] & ~(AddressSpace::kPageSize - 1), a[1], &size);
      }
      case Call::Mlockall: return 0;      // (mlockall, munlockall)
      case Call::ArchPrctl: return do_arch_prctl(process, a[0], a[1]);
      case Call::Ioctl: return linuxabi::sys_ioctl(process, a[0], a[1], a[2]);
      case Call::Fstat: return linuxabi::sys_fstat(process, arch_, a[0], a[1]);
      case Call::Newfstatat: return linuxabi::sys_statat(process, arch_, a[0], a[1], a[2], a[3]);
      case Call::Stat: return linuxabi::sys_statat(process, arch_, kAtFdCwd, a[0], a[1], 0);
      case Call::Lstat: return linuxabi::sys_statat(process, arch_, kAtFdCwd, a[0], a[1], 0x100);      // (AT_SYMLINK_NOFOLLOW)
      case Call::Statx: return linuxabi::sys_statx(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Lseek: return linuxabi::sys_lseek(process, a[0], a[1], a[2]);
      case Call::Openat: return linuxabi::sys_openat(process, arch_, a[0], a[1], a[2]);
      case Call::Open: return linuxabi::sys_openat(process, arch_, kAtFdCwd, a[0], a[1]);
      case Call::Creat: return linuxabi::sys_openat(process, arch_, kAtFdCwd, a[0], 0x241);      // (O_CREAT | O_WRONLY | O_TRUNC)
      case Call::Openat2: return linuxabi::sys_openat2(process, arch_, a[0], a[1], a[2], a[3]);
      case Call::CloseRange: return linuxabi::sys_close_range(process, a[0], a[1], a[2]);
      case Call::Symlink: return linuxabi::sys_symlinkat(process, a[0], kAtFdCwd, a[1]);
      case Call::Symlinkat: return linuxabi::sys_symlinkat(process, a[0], a[1], a[2]);
      case Call::Preadv: return linuxabi::sys_vector_at(process, a[0], a[1], a[2], a[3], 0, false, false);
      case Call::Pwritev: return linuxabi::sys_vector_at(process, a[0], a[1], a[2], a[3], 0, true, false);
      case Call::Preadv2: return linuxabi::sys_vector_at(process, a[0], a[1], a[2], a[3], a[5], false, true);
      case Call::Pwritev2: return linuxabi::sys_vector_at(process, a[0], a[1], a[2], a[3], a[5], true, true);
      case Call::Sendfile: return linuxabi::sys_sendfile(process, a[0], a[1], a[2], a[3]);
      case Call::CopyFileRange: return linuxabi::sys_copy_file_range(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Splice: return error(kEinval);      // splice, tee, vmsplice: no pipe here hands its pages on; programs fall back to read and write
      case Call::Readahead:
      case Call::Fadvise:
      case Call::SyncFileRange: return linuxabi::sys_file_advice(process, a[0], true);
      case Call::Syncfs: return linuxabi::sys_file_advice(process, a[0], false);
      case Call::Getdents: return linuxabi::sys_getdents(process, a[0], a[1], a[2]);
      case Call::Xattr: return error(kEopnotsupp);
      case Call::Unsupported: return error(kEopnotsupp);
      case Call::Absent: return error(kEnosys);      // (known and not there: nothing for the host to be told)
      case Call::RestartSyscall: return error(kEintr);      // (the kernel's own; there is never a call waiting to be started again)
      case Call::TimerCreate: return linuxabi::sys_timer_create(process, a[0], a[1], a[2]);
      case Call::TimerSettime: return linuxabi::sys_timer_settime(process, a[0], a[1], a[2], a[3]);
      case Call::TimerGettime: return linuxabi::sys_timer_gettime(process, a[0], a[1]);
      case Call::TimerGetoverrun: return linuxabi::sys_timer_getoverrun(process, a[0]);
      case Call::TimerDelete: return linuxabi::sys_timer_delete(process, a[0]);
      case Call::FutexWaitv: return linuxabi::sys_futex_waitv(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::FutexWake2: return linuxabi::sys_futex2_wake(process, a[0], a[1], a[2], a[3]);
      case Call::FutexWait2: return linuxabi::sys_futex2_wait(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::FutexRequeue2: return linuxabi::sys_futex2_requeue(process, a[0], a[1], a[2], a[3]);
      case Call::GetMempolicy: return linuxabi::sys_get_mempolicy(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::SetMempolicy: return linuxabi::sys_set_mempolicy(process, a[0], a[1], a[2]);
      case Call::Mbind: return linuxabi::sys_mbind(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::SetMempolicyHomeNode: return linuxabi::sys_set_mempolicy_home_node(process, a[0], a[1], a[2], a[3]);
      case Call::MigratePages: return linuxabi::sys_migrate_pages(process, a[0], a[1], a[2], a[3]);
      case Call::MovePages: return linuxabi::sys_move_pages(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::PkeyAlloc: return linuxabi::sys_pkey_alloc(process, a[0], a[1]);
      case Call::PkeyFree: return linuxabi::sys_pkey_free(process, a[0]);
      case Call::PkeyMprotect: return int32_t(a[3]) == -1 ? do_mprotect(process, a[0], a[1], a[2]) : error(kEinval);      // (key -1: plain mprotect; there is no other key)
      case Call::Kcmp: return linuxabi::sys_kcmp(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::ProcessVmReadv: return linuxabi::sys_process_vm(process, a[0], a[1], a[2], a[3], a[4], a[5], false);
      case Call::ProcessVmWritev: return linuxabi::sys_process_vm(process, a[0], a[1], a[2], a[3], a[4], a[5], true);
      case Call::Cachestat: return linuxabi::sys_cachestat(process, a[0], a[1], a[2], a[3]);
      case Call::InotifyInit: return linuxabi::sys_inotify_init1(process, 0);
      case Call::InotifyInit1: return linuxabi::sys_inotify_init1(process, a[0]);
      case Call::InotifyAddWatch: return linuxabi::sys_inotify_add_watch(process, a[0], a[1], a[2]);
      case Call::InotifyRmWatch: return linuxabi::sys_inotify_rm_watch(process, a[0], a[1]);
      case Call::IoSetup: return linuxabi::sys_io_setup(process, a[0], a[1]);
      case Call::IoDestroy: return linuxabi::sys_io_destroy(process, a[0]);
      case Call::IoSubmit: return linuxabi::sys_io_submit(process, a[0], a[1], a[2]);
      case Call::IoCancel: return linuxabi::sys_io_cancel(process, a[0], a[1], a[2]);
      case Call::IoGetevents: return linuxabi::sys_io_getevents(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::PidfdOpen: return linuxabi::sys_pidfd_open(process, a[0], a[1]);
      case Call::PidfdSendSignal: return linuxabi::sys_pidfd_send_signal(process, a[0], a[1], a[2], a[3]);
      case Call::PidfdGetfd: return linuxabi::sys_pidfd_getfd(process, a[0], a[1], a[2]);
      case Call::ProcessMadvise: return process_madvise(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::ProcessMrelease: {      // process_mrelease(pidfd, flags): frees the memory of a process that is being killed; none ever is at this point
        uint64_t pid = 0;
        if (!linuxabi::pidfd_process(process, a[0], &pid)) return error(kEbadf);
        return error(kEinval);
      }
      case Call::MqOpen: return linuxabi::sys_mq_open(process, a[0], a[1], a[2], a[3]);
      case Call::MqUnlink: return linuxabi::sys_mq_unlink(process, a[0]);
      case Call::MqTimedsend: return linuxabi::sys_mq_timedsend(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::MqTimedreceive: return linuxabi::sys_mq_timedreceive(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::MqNotify: return linuxabi::sys_mq_notify(process, a[0], a[1]);
      case Call::MqGetsetattr: return linuxabi::sys_mq_getsetattr(process, a[0], a[1], a[2]);
      case Call::Shmget: return linuxabi::sys_shmget(process, a[0], a[1], a[2]);
      case Call::Shmat: return linuxabi::sys_shmat(process, a[0], a[1], a[2]);
      case Call::Shmdt: return linuxabi::sys_shmdt(process, a[0]);
      case Call::Shmctl: return linuxabi::sys_shmctl(process, a[0], a[1], a[2]);
      case Call::Semget: return linuxabi::sys_semget(process, a[0], a[1], a[2]);
      case Call::Semop: return linuxabi::sys_semtimedop(process, a[0], a[1], a[2], 0);
      case Call::Semtimedop: return linuxabi::sys_semtimedop(process, a[0], a[1], a[2], a[3]);
      case Call::Semctl: return linuxabi::sys_semctl(process, arch_, a[0], a[1], a[2], a[3]);
      case Call::Msgget: return linuxabi::sys_msgget(process, a[0], a[1]);
      case Call::Msgsnd: return linuxabi::sys_msgsnd(process, a[0], a[1], a[2], a[3]);
      case Call::Msgrcv: return linuxabi::sys_msgrcv(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Msgctl: return linuxabi::sys_msgctl(process, a[0], a[1], a[2]);
      case Call::Refuse: return error(kEperm);
      case Call::Readlinkat: return linuxabi::sys_readlinkat(process, a[0], a[1], a[2], a[3]);
      case Call::Readlink: return linuxabi::sys_readlinkat(process, kAtFdCwd, a[0], a[1], a[2]);
      case Call::Pread: return linuxabi::sys_pread(process, a[0], a[1], a[2], a[3]);
      case Call::Pwrite: return linuxabi::sys_pwrite(process, a[0], a[1], a[2], a[3]);
      case Call::Readv: return linuxabi::sys_readv(process, a[0], a[1], a[2]);
      case Call::Getdents64: return linuxabi::sys_getdents64(process, a[0], a[1], a[2]);
      case Call::Mkdir: return linuxabi::sys_mkdirat(process, kAtFdCwd, a[0]);
      case Call::Mkdirat: return linuxabi::sys_mkdirat(process, a[0], a[1]);
      case Call::Mknod: return linuxabi::sys_mknodat(process, kAtFdCwd, a[0], a[1], a[2]);
      case Call::Mknodat: return linuxabi::sys_mknodat(process, a[0], a[1], a[2], a[3]);
      case Call::Rmdir: return linuxabi::sys_unlinkat(process, kAtFdCwd, a[0], 0x200);
      case Call::Unlink: return linuxabi::sys_unlinkat(process, kAtFdCwd, a[0], 0);
      case Call::Unlinkat: return linuxabi::sys_unlinkat(process, a[0], a[1], a[2]);
      case Call::Rename: return linuxabi::sys_renameat(process, kAtFdCwd, a[0], kAtFdCwd, a[1]);
      case Call::Renameat: return linuxabi::sys_renameat(process, a[0], a[1], a[2], a[3]);
      case Call::Renameat2: return linuxabi::sys_renameat(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Getcwd: return linuxabi::sys_getcwd(process, a[0], a[1]);
      case Call::Chdir: return linuxabi::sys_chdir(process, a[0]);
      case Call::Fchdir: return linuxabi::sys_fchdir(process, a[0]);
      case Call::Access: return linuxabi::sys_faccessat(process, kAtFdCwd, a[0], a[1]);
      case Call::Faccessat: return linuxabi::sys_faccessat(process, a[0], a[1], a[2]);
      case Call::Truncate: return linuxabi::sys_truncate(process, a[0], a[1]);
      case Call::Ftruncate: return linuxabi::sys_ftruncate(process, a[0], a[1]);
      case Call::Fsync: return linuxabi::sys_fsync(process, a[0]);
      case Call::Statfs: return linuxabi::sys_statfs(process, a[0], a[1]);
      case Call::Fstatfs: return linuxabi::sys_fstatfs(process, a[0], a[1]);
      case Call::Link: return linuxabi::sys_linkat(process, kAtFdCwd, a[0], kAtFdCwd, a[1], 0);
      case Call::Linkat: return linuxabi::sys_linkat(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Flock: return linuxabi::sys_flock(process, a[0], a[1]);
      case Call::Dup: return linuxabi::sys_dup(process, a[0]);
      case Call::Dup2: return linuxabi::sys_dup3(process, a[0], a[1], ~uint64_t{0});
      case Call::Dup3: return linuxabi::sys_dup3(process, a[0], a[1], a[2]);
      case Call::Fcntl: return linuxabi::sys_fcntl(process, a[0], a[1], a[2]);
      case Call::Umask: {      // (kept and given back; files have no permission bits for it to clear)
        const uint32_t old = process.linux_state().file_mode_mask;
        process.linux_state().file_mode_mask = uint32_t(a[0]) & 0777;
        return old;
      }
      case Call::Succeed: return 0;      // permissions, owners, times and syncs: there is nothing to change here
      case Call::RtSigaction: return linuxabi::sys_rt_sigaction(process, arch_, a[0], a[1], a[2], a[3]);
      case Call::RtSigprocmask: return linuxabi::sys_rt_sigprocmask(process, a[0], a[1], a[2], a[3]);
      case Call::Sigaltstack: return linuxabi::sys_sigaltstack(process, a[0], a[1]);
      case Call::SetRobustList: return linuxabi::sys_set_robust_list(process, a[0], a[1]);
      case Call::Madvise: return do_madvise(process, a[0], a[1], a[2]);
      case Call::Rseq: return error(kEnosys);
      case Call::Prlimit64: return linuxabi::sys_prlimit(process, a[0], a[1], a[2], a[3]);
      case Call::Getrandom: return do_getrandom(process, a[0], a[1]);
      case Call::Gettimeofday: return do_gettimeofday(process, a[0]);
      case Call::Time: {      // time(tloc): the seconds since 1970, also stored at tloc when it is not null (x86-64 only: the generic table has no time)
        const uint64_t seconds = process.host().realtime_ns() / 1000000000u;
        if (a[0] != 0 && !process.write_user_u64(a[0], seconds)) return error(kEfault);
        return seconds;
      }
      case Call::Getppid: return process.parent_id();
      case Call::MemfdCreate: return linuxabi::sys_memfd_create(process, a[0], a[1]);
      case Call::Fallocate: return linuxabi::sys_fallocate(process, a[0], a[1], a[2], a[3]);
      case Call::Socket: return linuxabi::sys_socket(process, a[0], a[1], a[2]);
      case Call::Socketpair: return linuxabi::sys_socketpair(process, a[0], a[1], a[2], a[3]);
      case Call::Pipe: return linuxabi::sys_pipe2(process, a[0], 0);
      case Call::Pipe2: return linuxabi::sys_pipe2(process, a[0], a[1]);
      case Call::Eventfd: return linuxabi::sys_eventfd2(process, a[0], 0);
      case Call::Eventfd2: return linuxabi::sys_eventfd2(process, a[0], a[1]);
      case Call::TimerfdCreate: return linuxabi::sys_timerfd_create(process, a[0], a[1]);
      case Call::TimerfdSettime: return linuxabi::sys_timerfd_settime(process, a[0], a[1], a[2], a[3]);
      case Call::TimerfdGettime: return linuxabi::sys_timerfd_gettime(process, a[0], a[1]);
      case Call::Signalfd: return linuxabi::sys_signalfd4(process, a[0], a[1], a[2], 0);
      case Call::Signalfd4: return linuxabi::sys_signalfd4(process, a[0], a[1], a[2], a[3]);
      case Call::EpollCreate: return int32_t(a[0]) <= 0 ? error(kEinval) : linuxabi::sys_epoll_create1(process, 0);
      case Call::EpollCreate1: return linuxabi::sys_epoll_create1(process, a[0]);
      case Call::EpollCtl: return linuxabi::sys_epoll_ctl(process, a[0], a[1], a[2], a[3]);
      case Call::EpollWait: case Call::EpollPwait: return linuxabi::sys_epoll_wait(process, a[0], a[1], a[2], a[3]);
      case Call::EpollPwait2: return linuxabi::sys_epoll_pwait2(process, a[0], a[1], a[2], a[3]);
      case Call::Bind: return linuxabi::sys_bind(process, a[0], a[1], a[2]);
      case Call::Connect: return linuxabi::sys_connect(process, a[0], a[1], a[2]);
      case Call::Listen: return linuxabi::sys_listen(process, a[0], a[1]);
      case Call::Accept: return linuxabi::sys_accept4(process, a[0], a[1], a[2], 0);
      case Call::Accept4: return linuxabi::sys_accept4(process, a[0], a[1], a[2], a[3]);
      case Call::Sendto: return linuxabi::sys_sendto(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Recvfrom: return linuxabi::sys_recvfrom(process, a[0], a[1], a[2], a[3], a[4], a[5]);
      case Call::Sendmsg: return linuxabi::sys_sendmsg(process, a[0], a[1], a[2]);
      case Call::Recvmsg: return linuxabi::sys_recvmsg(process, a[0], a[1], a[2]);
      case Call::Sendmmsg: return linuxabi::sys_sendmmsg(process, a[0], a[1], a[2], a[3]);
      case Call::Recvmmsg: return linuxabi::sys_recvmmsg(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Shutdown: return linuxabi::sys_shutdown(process, a[0], a[1]);
      case Call::Getsockname: return linuxabi::sys_getsockname(process, a[0], a[1], a[2]);
      case Call::Getpeername: return linuxabi::sys_getpeername(process, a[0], a[1], a[2]);
      case Call::Setsockopt: return linuxabi::sys_setsockopt(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Getsockopt: return linuxabi::sys_getsockopt(process, a[0], a[1], a[2], a[3], a[4]);
      case Call::Poll: {      // (timeout in milliseconds; negative waits for ever)
        const int64_t ms = int32_t(a[2]);
        return linuxabi::sys_poll(process, a[0], a[1], ms < 0 ? ~uint64_t{0} : uint64_t(ms) * 1000000u);
      }
      case Call::Ppoll: return linuxabi::sys_ppoll(process, a[0], a[1], a[2]);
      case Call::Select: return linuxabi::sys_select(process, a[0], a[1], a[2], a[3], a[4], false);
      case Call::Pselect6: return linuxabi::sys_select(process, a[0], a[1], a[2], a[3], a[4], true);
      default: process.host().unhandled_syscall(SyscallAbi::Linux, r.number); return error(kEnosys);
    }
  }

 private:
  uint64_t write_uname(Process& process, uint64_t address) const {
    constexpr uint32_t kField = 65;
    uint8_t buffer[kField * 6] = {};
    ByteView all(buffer, sizeof(buffer));
    const StringView fields[6] = {"Linux", "tcos", "6.1.0-tcos", "#1 TheoryCraftOS", machine_name(arch_), ""};
    for (uint32_t i = 0; i < 6; ++i) {
      all.subview(i * kField, kField).copy_from(text_bytes(fields[i].data()));      // every field is a literal, so NUL-terminated and shorter than kField
    }
    return process.write_user(address, ConstByteView(buffer, sizeof(buffer))) ? 0 : error(kEfault);
  }

  static uint64_t clock_gettime(Process& process, uint64_t clock, uint64_t address) {
    uint64_t ns = 0;
    if (clock == 0 || clock == 5) ns = process.host().realtime_ns();            // CLOCK_REALTIME, CLOCK_REALTIME_COARSE
    else if (clock == 1 || clock == 4 || clock == 6 || clock == 7) ns = process.host().monotonic_ns();      // CLOCK_MONOTONIC, _RAW, _COARSE, CLOCK_BOOTTIME
    else if (clock == 2 || clock == 3) ns = linuxabi::cpu_time_ns(process, clock == 3);      // CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID
    else if (clock == 11) ns = process.host().realtime_ns() + 37ull * 1000000000u;          // CLOCK_TAI: 37 leap seconds ahead of UTC
    else return error(kEinval);
    if (!process.write_user_u64(address, ns / 1000000000u) || !process.write_user_u64(address + 8, ns % 1000000000u)) return error(kEfault);
    return 0;
  }

  static uint64_t nanosleep(Process& process, uint64_t request, uint64_t remaining) {
    uint64_t seconds = 0, nanos = 0;
    if (!process.read_user_u64(request, &seconds) || !process.read_user_u64(request + 8, &nanos)) return error(kEfault);
    if (int64_t(seconds) < 0 || nanos >= 1000000000u) return error(kEinval);
    const uint64_t kMaxSeconds = (~uint64_t{0} - 999999999u) / 1000000000u;
    const uint64_t left = sleep_or_signal(process, seconds > kMaxSeconds ? ~uint64_t{0} : seconds * 1000000000u + nanos);
    if (remaining && (!process.write_user_u64(remaining, left / 1000000000u) || !process.write_user_u64(remaining + 8, left % 1000000000u))) return error(kEfault);
    return left ? error(kEintr) : 0;
  }

  // Sleeps `ns`, or until a signal is there for the thread (or the program is told to stop). What was left of the time: 0 when it slept it all.
  static uint64_t sleep_or_signal(Process& process, uint64_t ns) {
    uint64_t now = process.host().monotonic_ns();
    const uint64_t end = ns > ~uint64_t{0} - now ? ~uint64_t{0} : now + ns;
    while (now < end) {
      if (process.signal_interrupts()) return end - now;
      if (process.exited() || (process.runtime() && process.runtime()->interrupted())) return 0;
      const uint64_t slice = process.wait_slice(end - now);
      process.block_sleep(slice);
      // (a host whose clock stands still, as a test's may, still gets through its sleep: every slice counts for at least its length)
      const uint64_t clock = process.host().monotonic_ns();
      now = clock > now && clock - now >= slice ? clock : (slice > ~uint64_t{0} - now ? ~uint64_t{0} : now + slice);
    }
    return 0;
  }

  // clock_nanosleep (what glibc's nanosleep and sleep call): a relative time, or with TIMER_ABSTIME a time of the clock to sleep until (none if it passed).
  // Sleeps are never interrupted, so the time left is never written.
  static uint64_t clock_nanosleep(Process& process, uint64_t clock, uint64_t flags, uint64_t request) {
    constexpr uint64_t kTimerAbstime = 1;
    if (clock != 0 && clock != 1 && clock != 4 && clock != 7) return error(kEinval);      // REALTIME, MONOTONIC, MONOTONIC_RAW, BOOTTIME
    if (flags & ~kTimerAbstime) return error(kEinval);
    uint64_t seconds = 0, nanos = 0;
    if (!process.read_user_u64(request, &seconds) || !process.read_user_u64(request + 8, &nanos)) return error(kEfault);
    if (int64_t(seconds) < 0 || nanos >= 1000000000u) return error(kEinval);
    const uint64_t kMaxSeconds = (~uint64_t{0} - 999999999u) / 1000000000u;
    uint64_t ns = seconds > kMaxSeconds ? ~uint64_t{0} : seconds * 1000000000u + nanos;
    if (flags & kTimerAbstime) {
      const uint64_t now = clock == 0 ? process.host().realtime_ns() : process.host().monotonic_ns();
      ns = ns > now ? ns - now : 0;
    }
    return ns && sleep_or_signal(process, ns) ? error(kEintr) : 0;
  }

  UserArch arch_;
};

namespace {
// Constant-initialised: no run-time setup, nothing to guard.
constexpr LinuxSyscalls kX86(UserArch::X86_64), kArm(UserArch::AArch64), kRiscV(UserArch::RiscV64), kLoong(UserArch::LoongArch64);
}  // namespace

namespace linuxabi {
bool call_restarts(UserArch arch, const SyscallRequest& request) {
  // The calls Linux starts again after a handler that was installed with SA_RESTART. (Not the ones that wait for a time or for signals themselves: nanosleep,
  // poll, select, epoll_wait, sigsuspend, sigtimedwait, pause; and not a futex wait with a timeout, which would wait the whole time again.)
  switch (translate(arch, request.number)) {
    case Call::Read: case Call::Write: case Call::Readv: case Call::Writev: case Call::Recvfrom: case Call::Recvmsg: case Call::Sendto: case Call::Sendmsg:
    case Call::Accept: case Call::Accept4: case Call::Connect: case Call::Wait4: case Call::Waitid: case Call::Flock: case Call::Fcntl: case Call::Open:
    case Call::Openat: case Call::Openat2: case Call::Ioctl: case Call::Sendmmsg:
      return true;
    case Call::Futex: return request.args[3] == 0;
    default: return false;
  }
}

bool call_known(UserArch arch, uint64_t number) { return translate(arch, number) != Call::None; }

uint64_t map_user_memory(Process& process, uint64_t bytes) {
  const uint64_t r = do_mmap(process, 0, bytes, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, ~uint64_t{0}, 0);
  return r > ~uint64_t{4095} ? 0 : r;      // (-errno is a failure)
}
void unmap_user_memory(Process& process, uint64_t address, uint64_t bytes) {
  uint64_t size = 0;
  if (address && AddressSpace::round_up(bytes ? bytes : 1, &size)) do_munmap(process, address, size);
}
}  // namespace linuxabi

const ISyscallInterface& linux_interface(UserArch arch) {
  return arch == UserArch::X86_64 ? kX86 : arch == UserArch::AArch64 ? kArm : arch == UserArch::LoongArch64 ? kLoong : kRiscV;
}

}  // namespace process
