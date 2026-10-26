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
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The Linux file system calls (LinuxSyscalls.cpp maps numbers to these). Each returns the value for the result register: the result, or -errno.
namespace process::linuxabi {

uint64_t sys_openat(Process& p, UserArch arch, uint64_t dirfd, uint64_t path, uint64_t flags);
uint64_t sys_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t sys_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t sys_pread(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset);
uint64_t sys_pwrite(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset);
uint64_t sys_readv(Process& p, uint64_t fd, uint64_t iov, uint64_t count);
uint64_t sys_writev(Process& p, uint64_t fd, uint64_t iov, uint64_t count);
uint64_t sys_lseek(Process& p, uint64_t fd, uint64_t offset, uint64_t whence);
uint64_t sys_fstat(Process& p, UserArch arch, uint64_t fd, uint64_t buffer);
uint64_t sys_statat(Process& p, UserArch arch, uint64_t dirfd, uint64_t path, uint64_t buffer, uint64_t flags);
uint64_t sys_statx(Process& p, uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t buffer);
uint64_t sys_getdents64(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t sys_mkdirat(Process& p, uint64_t dirfd, uint64_t path);
uint64_t sys_unlinkat(Process& p, uint64_t dirfd, uint64_t path, uint64_t flags);
uint64_t sys_renameat(Process& p, uint64_t old_dirfd, uint64_t old_path, uint64_t new_dirfd, uint64_t new_path);
uint64_t sys_getcwd(Process& p, uint64_t buffer, uint64_t size);
uint64_t sys_chdir(Process& p, uint64_t path);
uint64_t sys_fchdir(Process& p, uint64_t fd);
uint64_t sys_faccessat(Process& p, uint64_t dirfd, uint64_t path, uint64_t mode);
uint64_t sys_truncate(Process& p, uint64_t path, uint64_t length);
uint64_t sys_ftruncate(Process& p, uint64_t fd, uint64_t length);
uint64_t sys_fsync(Process& p, uint64_t fd);
uint64_t sys_dup(Process& p, uint64_t fd);
uint64_t sys_dup3(Process& p, uint64_t old_fd, uint64_t new_fd, uint64_t flags);
uint64_t sys_fcntl(Process& p, uint64_t fd, uint64_t command, uint64_t argument);
uint64_t sys_readlinkat(Process& p, uint64_t dirfd, uint64_t path);
uint64_t sys_ioctl(Process& p, uint64_t fd);

}  // namespace process::linuxabi
