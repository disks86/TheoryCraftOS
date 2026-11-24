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

// The virtio-gpu DRM render node for Linux programs, as Linux's virtio-gpu driver gives it, so that libdrm and Mesa's venus driver run unchanged:
//
// - /dev/dri (a directory) with /dev/dri/renderD128, a character device 226:128. Each open is one session on the GPU (graphics::virtgpu::Session); dup,
//   fork and SCM_RIGHTS share the open file and so the session, which goes with the last descriptor. ioctl runs the DRM and virtio-gpu calls, mmap maps
//   a buffer object at the offset VIRTGPU_MAP gave (MAP_SHARED only), read() and poll() give the fence events of the rings CONTEXT_INIT asked to hear
//   about (struct drm_event, VIRTGPU_EVENT_FENCE_SIGNALED; read blocks unless O_NONBLOCK).
// - sync_file descriptors (EXECBUFFER's out fence, linux/sync_file.h): poll() POLLIN once every fence of the file has signaled, SYNC_IOC_MERGE,
//   SYNC_IOC_FILE_INFO, SYNC_IOC_SET_DEADLINE; they are taken back as in fences.
// - dma-buf descriptors of buffer objects (PRIME, linux/dma-buf.h): lseek(SEEK_END) gives the size, mmap maps the buffer, poll() says whether the GPU
//   still uses it, DMA_BUF_IOCTL_SYNC (the memory is coherent: nothing to do), DMA_BUF_SET_NAME, DMA_BUF_IOCTL_EXPORT_SYNC_FILE / IMPORT_SYNC_FILE.
// - /sys, the part libdrm reads to find the device (xf86drm.c: drmGetDevices2, drmGetDevice2, drmGetMinorNameForFD, drmGetDeviceNameFromFd2): the PCI
//   function at /sys/devices/pci<domain>:<bus>/<slot> (vendor, device, subsystem_vendor, subsystem_device, revision, class, config, uevent, subsystem ->
//   bus/pci, drm/renderD128 with dev, uevent, device and subsystem), /sys/dev/char/226:128, /sys/class/drm/renderD128 and /sys/bus/pci/devices/<slot>,
//   with Linux's symbolic links between them (readlink and realpath() give what they give on Linux). This is the system's /sys: other devices that need
//   entries there add them to its table (LinuxRenderNode.cpp, kNodes).
//
// None of it exists without a render node (graphics::virtgpu::system_host() null): then the paths are the file tree's (ENOENT), as on a Linux without
// the device. The /sys entries of the PCI function need set_render_node_pci() as well.
#pragma once
#include <stdint.h>
#include "Process/AddressSpace.hpp"
#include "Process/Process.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace process {

// The PCI function of the render node's device, as /sys shows it.
struct RenderNodePci {
  uint16_t domain = 0;
  uint8_t bus = 0, device = 0, function = 0;
  uint8_t config[256] = {};   // the first 256 bytes of its configuration space (vendor, device, revision, class, subsystem come from here)
};
// `pci` must stay alive (static storage); null takes it away. Set at start-up, before any process runs.
void set_render_node_pci(const RenderNodePci* pci);

// The state of one open render node (OpenObject::render), and the files the kernel makes up (OpenObject::virtual_file).
class RenderNodeClient;
class VirtualFile;

namespace linuxabi {

// What a name of the tree is, for stat and friends.
struct RenderNodeMeta {
  uint32_t mode = 0;          // with the S_IF* type bits
  uint64_t rdev = 0, ino = 0, size = 0;
};
// glibc's makedev(major, minor): what st_rdev holds.
constexpr uint64_t linux_device_number(uint32_t major, uint32_t minor) {
  return ((uint64_t{major} & 0xFFFu) << 8) | ((uint64_t{major} & 0xFFFFF000u) << 32) | (uint64_t{minor} & 0xFFu) | ((uint64_t{minor} & 0xFFFFFF00u) << 12);
}

// 1 and *meta when `virtual_path` is a name of the tree (/dev/dri and its node, /sys while there is a render node), 0 when it is not, -errno (ENOENT,
// ENOTDIR, ELOOP). Symbolic links on the way are followed; the last one too unless `no_follow` (then *meta is the link's own).
int64_t render_node_at(StringView virtual_path, bool no_follow, RenderNodeMeta* meta);
// readlink(): 1 and *target (as Linux spells it: relative) when `virtual_path` is a link of the tree, 0 when the path is not the tree's, -EINVAL for a name
// of the tree that is no link, -ENOENT.
int64_t render_node_link(StringView virtual_path, String* target);
// Is it /dev/dri/renderD128 of a system that has the device? (ProcessFiles::device_at)
bool render_node_device_exists(StringView virtual_path);
// open() of a path render_node_at() accepted: a new descriptor or -errno (ELOOP for a link with `no_follow`: O_NOFOLLOW).
int64_t render_node_open(Process& p, StringView virtual_path, uint64_t flags, bool cloexec, bool must_be_directory, bool no_follow);

// The calls on the descriptors (a RenderNode, or a File whose object has a virtual_file). Each answers true when `fd` is one of them, with *result the
// system call's result (a value or the negated errno, as the system call returns it); false for any other descriptor.
bool render_node_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t* result);
bool render_node_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument, uint64_t* result);
bool render_node_seek(Process& p, uint64_t fd, int64_t offset, uint64_t whence, uint64_t* result);
bool render_node_map(Process& p, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset, uint64_t* result);
// poll(): true when `fd` is one of them (a render node, a sync_file, a dma-buf), with *events its POLL* bits.
bool render_node_poll(Process& p, uint64_t fd, uint16_t* events);
// fstat: true and *meta for one of them, or a directory of the tree.
bool render_node_descriptor_meta(Process& p, uint64_t fd, RenderNodeMeta* meta);
// What /proc/self/fd/<fd> shows for one of them: true and *out (false for any other descriptor).
bool render_node_descriptor_path(Process& p, uint64_t fd, String* out);
bool render_node_non_blocking(Process& p, uint64_t fd);
void set_render_node_non_blocking(Process& p, uint64_t fd, bool value);

}  // namespace linuxabi
}  // namespace process
