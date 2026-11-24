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

#include "Process/LinuxRenderNode.hpp"
#include "FileSystem/IDirectory.hpp"
#include "FileSystem/IFile.hpp"
#include "Graphics/VirtGpu/VirtGpuSession.hpp"
#include "Graphics/VirtGpu/VirtioVirtGpuHost.hpp"
#include "Kernel/Sync.hpp"
#include "Process/Objects.hpp"
#include "Process/ProcessFiles.hpp"
#include "Process/VirtGpuDevice.h"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/Move.hpp"

namespace process {

namespace {

using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;
using graphics::virtgpu::BufferRef;
using graphics::virtgpu::FenceRef;
using graphics::virtgpu::IVirtGpuHost;

constexpr int64_t kEnoent = 2, kEintr = 4, kEbadf = 9, kEagain = 11, kEnomem = 12, kEacces = 13, kEfault = 14, kEexist = 17, kEnodev = 19, kEnotdir = 20,
                  kEisdir = 21, kEinval = 22, kEmfile = 24, kEnotty = 25, kEspipe = 29, kEloop = 40;
constexpr uint64_t kOAccMode = 3, kOWrite = 1, kORdOnly = 0, kONonBlock = 0x800;
constexpr uint32_t kSIfDir = 040000, kSIfReg = 0100000, kSIfChr = 020000, kSIfLnk = 0120000;
constexpr uint16_t kPollIn = 0x1, kPollOut = 0x4, kPollRdNorm = 0x40, kPollWrNorm = 0x100;
constexpr uint64_t kPage = 4096;
constexpr uint64_t kMapShared = 0x01, kMapPrivate = 0x02, kMapFixed = 0x10;
constexpr uint64_t kWaitSliceNs = 1000000;   // a blocking read of the node looks for an event every millisecond (nothing tells it when a fence signals)

uint64_t error(int64_t value) { return uint64_t(-value); }

const RenderNodePci* g_pci = nullptr;   // set once at start-up (set_render_node_pci)

const RenderNodePci* pci() { return __atomic_load_n(&g_pci, __ATOMIC_ACQUIRE); }
IVirtGpuHost* host() { return graphics::virtgpu::system_host(); }

uint64_t fnv(StringView text) {
  uint64_t h = 1469598103934665603ull;
  for (char c : text) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
  return h | 1;
}

template <class Text>
bool append_hex(Text& out, uint64_t value, uint32_t digits, bool upper) {
  char chars[16];
  uint32_t n = 0;
  do {
    const uint32_t d = uint32_t(value & 0xF);
    chars[n++] = static_cast<char>(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
    value >>= 4;
  } while (value != 0 && n < sizeof(chars));
  while (n < digits && n < sizeof(chars)) chars[n++] = '0';
  while (n > 0)
    if (!out.push_back(chars[--n])) return false;
  return true;
}

// --- the tree: /dev/dri and /sys -------------------------------------------------------------------------------------------------------------------------
//
// Every name of the tree is a row of kNodes (its parent, how it is named, what it is). A symbolic link names the row it leads to; readlink() spells the
// way there from the link's folder as Linux does ("../../devices/..."). Paths come here cleaned (no "." or ".."), so lookups walk the rows name by name.

enum class N : uint8_t {
  Sys, SysDev, SysDevChar, DevCharLink, SysClass, SysClassDrm, ClassDrmLink, ClassDrmVersion, SysBus, SysBusPci, SysBusPciDevices, BusPciLink,
  SysDevices, PciRoot, Pci, PciVendor, PciDevice, PciSubVendor, PciSubDevice, PciRevision, PciClass, PciUevent, PciConfig, PciSubsystem, PciDrm,
  Minor, MinorDev, MinorUevent, MinorDevice, MinorSubsystem, DevDri, DevNode, Count
};
enum class Type : uint8_t { Directory, Link, Attribute, Config, Device };
enum class Naming : uint8_t { Fixed, CharNumber, PciRoot, PciSlot };

struct Row {
  N id;
  N parent;            // (a root names itself)
  Type type;
  Naming naming;
  const char* name;    // Fixed
  N target;            // Link
  uint32_t mode;       // permission bits
};

constexpr Row kNodes[] = {
  {N::Sys, N::Sys, Type::Directory, Naming::Fixed, "/sys", N::Sys, 0555},
  {N::SysDev, N::Sys, Type::Directory, Naming::Fixed, "dev", N::Sys, 0755},
  {N::SysDevChar, N::SysDev, Type::Directory, Naming::Fixed, "char", N::Sys, 0755},
  {N::DevCharLink, N::SysDevChar, Type::Link, Naming::CharNumber, nullptr, N::Minor, 0777},
  {N::SysClass, N::Sys, Type::Directory, Naming::Fixed, "class", N::Sys, 0755},
  {N::SysClassDrm, N::SysClass, Type::Directory, Naming::Fixed, "drm", N::Sys, 0755},
  {N::ClassDrmLink, N::SysClassDrm, Type::Link, Naming::Fixed, "renderD128", N::Minor, 0777},
  {N::ClassDrmVersion, N::SysClassDrm, Type::Attribute, Naming::Fixed, "version", N::Sys, 0444},
  {N::SysBus, N::Sys, Type::Directory, Naming::Fixed, "bus", N::Sys, 0755},
  {N::SysBusPci, N::SysBus, Type::Directory, Naming::Fixed, "pci", N::Sys, 0755},
  {N::SysBusPciDevices, N::SysBusPci, Type::Directory, Naming::Fixed, "devices", N::Sys, 0755},
  {N::BusPciLink, N::SysBusPciDevices, Type::Link, Naming::PciSlot, nullptr, N::Pci, 0777},
  {N::SysDevices, N::Sys, Type::Directory, Naming::Fixed, "devices", N::Sys, 0755},
  {N::PciRoot, N::SysDevices, Type::Directory, Naming::PciRoot, nullptr, N::Sys, 0755},
  {N::Pci, N::PciRoot, Type::Directory, Naming::PciSlot, nullptr, N::Sys, 0755},
  {N::PciVendor, N::Pci, Type::Attribute, Naming::Fixed, "vendor", N::Sys, 0444},
  {N::PciDevice, N::Pci, Type::Attribute, Naming::Fixed, "device", N::Sys, 0444},
  {N::PciSubVendor, N::Pci, Type::Attribute, Naming::Fixed, "subsystem_vendor", N::Sys, 0444},
  {N::PciSubDevice, N::Pci, Type::Attribute, Naming::Fixed, "subsystem_device", N::Sys, 0444},
  {N::PciRevision, N::Pci, Type::Attribute, Naming::Fixed, "revision", N::Sys, 0444},
  {N::PciClass, N::Pci, Type::Attribute, Naming::Fixed, "class", N::Sys, 0444},
  {N::PciUevent, N::Pci, Type::Attribute, Naming::Fixed, "uevent", N::Sys, 0644},
  {N::PciConfig, N::Pci, Type::Config, Naming::Fixed, "config", N::Sys, 0644},
  {N::PciSubsystem, N::Pci, Type::Link, Naming::Fixed, "subsystem", N::SysBusPci, 0777},
  {N::PciDrm, N::Pci, Type::Directory, Naming::Fixed, "drm", N::Sys, 0755},
  {N::Minor, N::PciDrm, Type::Directory, Naming::Fixed, "renderD128", N::Sys, 0755},
  {N::MinorDev, N::Minor, Type::Attribute, Naming::Fixed, "dev", N::Sys, 0444},
  {N::MinorUevent, N::Minor, Type::Attribute, Naming::Fixed, "uevent", N::Sys, 0644},
  {N::MinorDevice, N::Minor, Type::Link, Naming::Fixed, "device", N::Pci, 0777},
  {N::MinorSubsystem, N::Minor, Type::Link, Naming::Fixed, "subsystem", N::SysClassDrm, 0777},
  {N::DevDri, N::DevDri, Type::Directory, Naming::Fixed, "/dev/dri", N::DevDri, 0755},
  {N::DevNode, N::DevDri, Type::Device, Naming::Fixed, "renderD128", N::DevDri, 0666},
};
static_assert(sizeof(kNodes) / sizeof(kNodes[0]) == uint32_t(N::Count), "one row per node");

const Row& row(N id) { return kNodes[uint32_t(id)]; }
bool is_root(N id) { return row(id).parent == id; }

constexpr uint32_t kMaxDepth = 8;

// The rows from the root down to `id` (the root first). How many.
uint32_t chain(N id, N out[kMaxDepth]) {
  N reversed[kMaxDepth];
  uint32_t n = 0;
  for (N at = id; n < kMaxDepth; at = row(at).parent) {
    reversed[n++] = at;
    if (is_root(at)) break;
  }
  for (uint32_t i = 0; i < n; ++i) out[i] = reversed[n - 1 - i];
  return n;
}

using Name = FixedString<64>;

bool name_of(N id, const RenderNodePci* p, Name* out) {
  out->clear();
  const Row& r = row(id);
  switch (r.naming) {
    case Naming::Fixed: return out->assign(StringView(r.name));
    case Naming::CharNumber:
      return out->append_uint(TCOS_DRM_MAJOR) && out->push_back(':') && out->append_uint(TCOS_VIRTGPU_RENDER_MINOR);
    case Naming::PciRoot:      // pci<domain>:<bus>
      return p && out->assign("pci") && append_hex(*out, p->domain, 4, false) && out->push_back(':') && append_hex(*out, p->bus, 2, false);
    case Naming::PciSlot:      // pci_name(): <domain>:<bus>:<device>.<function>
      return p && append_hex(*out, p->domain, 4, false) && out->push_back(':') && append_hex(*out, p->bus, 2, false) && out->push_back(':') &&
             append_hex(*out, p->device, 2, false) && out->push_back('.') && out->append_uint(p->function);
  }
  return false;
}

bool path_of(N id, const RenderNodePci* p, String* out) {
  N rows[kMaxDepth];
  const uint32_t n = chain(id, rows);
  out->clear();
  for (uint32_t i = 0; i < n; ++i) {
    Name name;
    if (!name_of(rows[i], p, &name)) return false;
    if (i != 0 && !out->push_back('/')) return false;
    if (!out->append(name.view())) return false;
  }
  return true;
}

// Is the tree under this root there? /dev/dri with a render node; /sys with a render node whose PCI function is known.
bool root_present(N root) {
  if (!host()) return false;
  return root == N::DevDri || pci() != nullptr;
}

// The child of `parent` called `name`.
bool child_named(N parent, StringView name, const RenderNodePci* p, N* out) {
  for (const Row& r : kNodes) {
    if (r.parent != parent || is_root(r.id)) continue;
    Name candidate;
    if (name_of(r.id, p, &candidate) && candidate.view() == name) {
      *out = r.id;
      return true;
    }
  }
  return false;
}

// The row a cleaned path names: 1 and *out, 0 when the path is not the tree's, -errno. Links on the way are followed, the last one too if `follow_last`.
int64_t lookup(StringView path, bool follow_last, N* out) {
  N at;
  StringView rest;
  const StringView sys("/sys"), dri("/dev/dri");
  if (path.starts_with(sys) && (path.size() == sys.size() || path[sys.size()] == '/')) {
    at = N::Sys;
    rest = path.without_prefix(sys.size());
  } else if (path.starts_with(dri) && (path.size() == dri.size() || path[dri.size()] == '/')) {
    at = N::DevDri;
    rest = path.without_prefix(dri.size());
  } else {
    return 0;
  }
  if (!root_present(at)) return 0;
  const RenderNodePci* p = pci();
  uint32_t hops = 0;
  while (!rest.empty()) {
    if (rest[0] == '/') {
      rest = rest.without_prefix(1);
      continue;
    }
    const uint64_t slash = rest.find('/');
    const StringView component = slash == StringView::npos ? rest : rest.first(slash);
    rest = slash == StringView::npos ? StringView() : rest.without_prefix(slash);
    while (row(at).type == Type::Link) {      // (a link in the middle of the path)
      if (++hops > 40) return -kEloop;
      at = row(at).target;
    }
    if (row(at).type != Type::Directory) return -kEnotdir;
    N child;
    if (!child_named(at, component, p, &child)) return -kEnoent;
    at = child;
  }
  if (follow_last)
    while (row(at).type == Type::Link) {
      if (++hops > 40) return -kEloop;
      at = row(at).target;
    }
  *out = at;
  return 1;
}

linuxabi::RenderNodeMeta meta_of(N id) {
  linuxabi::RenderNodeMeta m;
  const Row& r = row(id);
  Name name;
  // (the inode number: the path's, made from the names of the rows; no allocation)
  uint64_t h = 1469598103934665603ull;
  N rows[kMaxDepth];
  const uint32_t n = chain(id, rows);
  for (uint32_t i = 0; i < n; ++i)
    if (name_of(rows[i], pci(), &name)) h = (fnv(name.view()) ^ h) * 1099511628211ull;
  m.ino = h | 1;
  switch (r.type) {
    case Type::Directory: m.mode = kSIfDir | r.mode; m.size = 0; break;
    case Type::Link: m.mode = kSIfLnk | 0777; m.size = 0; break;   // (sysfs links report no size)
    case Type::Attribute: m.mode = kSIfReg | r.mode; m.size = kPage; break;   // (an attribute shows a page, whatever it holds)
    case Type::Config: m.mode = kSIfReg | r.mode; m.size = sizeof(RenderNodePci::config); break;
    case Type::Device:
      m.mode = kSIfChr | r.mode;
      m.rdev = linuxabi::linux_device_number(TCOS_DRM_MAJOR, TCOS_VIRTGPU_RENDER_MINOR);
      break;
  }
  return m;
}

// What an attribute says (Linux's formats: drivers/pci/pci-sysfs.c, drivers/base/core.c dev_uevent, drivers/gpu/drm/drm_sysfs.c).
bool attribute_text(N id, const RenderNodePci& p, String* out) {
  const auto le16 = [&](uint32_t at) { return uint32_t(p.config[at]) | uint32_t(p.config[at + 1]) << 8; };
  const uint32_t vendor = le16(0), device = le16(2), sub_vendor = le16(0x2C), sub_device = le16(0x2E), revision = p.config[8];
  const uint32_t klass = uint32_t(p.config[9]) | uint32_t(p.config[10]) << 8 | uint32_t(p.config[11]) << 16;
  out->clear();
  switch (id) {
    case N::PciVendor: return out->assign("0x") && append_hex(*out, vendor, 4, false) && out->push_back('\n');
    case N::PciDevice: return out->assign("0x") && append_hex(*out, device, 4, false) && out->push_back('\n');
    case N::PciSubVendor: return out->assign("0x") && append_hex(*out, sub_vendor, 4, false) && out->push_back('\n');
    case N::PciSubDevice: return out->assign("0x") && append_hex(*out, sub_device, 4, false) && out->push_back('\n');
    case N::PciRevision: return out->assign("0x") && append_hex(*out, revision, 2, false) && out->push_back('\n');
    case N::PciClass: return out->assign("0x") && append_hex(*out, klass, 6, false) && out->push_back('\n');
    case N::PciUevent: {
      Name slot;
      if (!name_of(N::Pci, &p, &slot)) return false;
      return out->assign("DRIVER=virtio-pci\nPCI_CLASS=") && append_hex(*out, klass, 4, true) && out->append("\nPCI_ID=") && append_hex(*out, vendor, 4, true) &&
             out->push_back(':') && append_hex(*out, device, 4, true) && out->append("\nPCI_SUBSYS_ID=") && append_hex(*out, sub_vendor, 4, true) &&
             out->push_back(':') && append_hex(*out, sub_device, 4, true) && out->append("\nPCI_SLOT_NAME=") && out->append(slot.view()) &&
             out->append("\nMODALIAS=pci:v") && append_hex(*out, vendor, 8, true) && out->push_back('d') && append_hex(*out, device, 8, true) &&
             out->append("sv") && append_hex(*out, sub_vendor, 8, true) && out->append("sd") && append_hex(*out, sub_device, 8, true) && out->append("bc") &&
             append_hex(*out, klass >> 16, 2, true) && out->append("sc") && append_hex(*out, (klass >> 8) & 0xFF, 2, true) && out->push_back('i') &&
             append_hex(*out, klass & 0xFF, 2, true) && out->push_back('\n');
    }
    case N::MinorDev:
      return out->append_uint(TCOS_DRM_MAJOR) && out->push_back(':') && out->append_uint(TCOS_VIRTGPU_RENDER_MINOR) && out->push_back('\n');
    case N::MinorUevent:
      return out->assign("MAJOR=") && out->append_uint(TCOS_DRM_MAJOR) && out->append("\nMINOR=") && out->append_uint(TCOS_VIRTGPU_RENDER_MINOR) &&
             out->append("\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n");
    case N::ClassDrmVersion: return out->assign("drm 1.1.0 20060810\n");
    default: return false;
  }
}

// A folder of the tree listed: its rows, as they are when it is read.
class TreeDirectory final : public filesystem::IDirectory {
 public:
  explicit TreeDirectory(N folder) : folder_(folder) {}
  Status next(DirectoryEntry* entry) override {
    while (index_ < uint32_t(N::Count)) {
      const Row& r = kNodes[index_++];
      if (r.parent != folder_ || is_root(r.id)) continue;
      Name name;
      if (!name_of(r.id, pci(), &name)) continue;
      *entry = DirectoryEntry();
      if (!entry->name.assign(name.view())) return Status::OutOfMemory;
      entry->type = r.type == Type::Directory ? NodeType::Directory : NodeType::File;
      entry->is_symlink = r.type == Type::Link;
      if (r.type == Type::Device) entry->special = filesystem::SpecialNode::CharDevice;
      return Status::Ok;
    }
    return Status::EndOfDirectory;
  }
  void rewind() override { index_ = 0; }
  N folder() const { return folder_; }

 private:
  N folder_;
  uint32_t index_ = 0;
};

}  // namespace

// --- the made-up files ------------------------------------------------------------------------------------------------------------------------------------

class VirtualFile : public filesystem::IFile {
 public:
  enum class Kind : uint8_t { Attribute, Buffer, Fence };
  Kind kind() const { return kind_; }

  uint64_t size() const override { return size_; }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    *transferred = 0;
    if (kind_ != Kind::Attribute) return Status::InvalidArgument;   // (a dma-buf and a sync_file have no read: EINVAL)
    const ConstByteView all = contents_.view();
    if (offset >= all.size()) return Status::Ok;
    const uint64_t n = all.size() - offset < buffer.size() ? all.size() - offset : buffer.size();
    buffer.first(n).copy_from(all.subview(offset, n));
    *transferred = n;
    return Status::Ok;
  }
  Status write(uint64_t, ConstByteView, uint64_t* transferred) override {
    *transferred = 0;
    return Status::InvalidArgument;
  }
  Status resize(uint64_t) override { return Status::InvalidArgument; }
  Status flush() override { return Status::Ok; }

  // An attribute: its bytes.
  ByteBuffer& contents() { return contents_; }
  void set_size(uint64_t size) { size_ = size; }

 protected:
  VirtualFile(Kind kind, allocators::IAllocator& allocator) : kind_(kind), contents_(allocator) {}
  ~VirtualFile() = default;

 private:
  Kind kind_;
  uint64_t size_ = 0;
  ByteBuffer contents_;
};

namespace {

class AttributeFile final : public VirtualFile {
 public:
  explicit AttributeFile(allocators::IAllocator& allocator) : VirtualFile(Kind::Attribute, allocator) {}
};

// A sync_file: the fences it waits for (at most kMaxFences, each on its own timeline: what dma_fence_unwrap_merge keeps). A fence of number 0 is the
// signaled stub Linux gives when nothing is left to wait for.
class FenceFile final : public VirtualFile {
 public:
  static constexpr uint32_t kMaxFences = 32;
  static constexpr uint32_t kUnknownRing = ~1u;   // a fence whose timeline is not known (taken from a buffer): never folded with another
  FenceFile(allocators::IAllocator& allocator, IVirtGpuHost& host) : VirtualFile(Kind::Fence, allocator), host_(host) {}

  IVirtGpuHost& host() const { return host_; }
  kernel::Mutex lock;   // the timestamps and the name
  FenceRef fences[kMaxFences];
  uint64_t signaled_at[kMaxFences] = {};   // CLOCK_MONOTONIC when it was first seen signaled (0: not yet)
  uint32_t count = 0;
  char name[32] = {};   // the name SYNC_IOC_MERGE gave (empty: made by the driver)

  bool signaled(uint32_t i) { return fences[i].fence == 0 || host_.fence_signaled(fences[i].fence); }
  // Notes when each signaled one was first seen. True if all have.
  bool look(uint64_t now) {
    kernel::ScopedMutex hold(lock);
    bool all = true;
    for (uint32_t i = 0; i < count; ++i) {
      if (signaled(i)) {
        if (signaled_at[i] == 0) signaled_at[i] = now ? now : 1;
      } else {
        all = false;
      }
    }
    return all;
  }

 private:
  IVirtGpuHost& host_;
};

// A dma-buf: one buffer object, shared by every descriptor of it.
class BufferFile final : public VirtualFile {
 public:
  BufferFile(allocators::IAllocator& allocator, const BufferRef& buffer) : VirtualFile(Kind::Buffer, allocator), buffer_(buffer) { set_size(buffer->size()); }
  const BufferRef& buffer() const { return buffer_; }
  kernel::Mutex lock;   // the name
  char name[32] = {};   // DMA_BUF_SET_NAME

 private:
  BufferRef buffer_;
};

}  // namespace

// --- one open render node ---------------------------------------------------------------------------------------------------------------------------------

class RenderNodeClient {
 public:
  RenderNodeClient(IVirtGpuHost& host, allocators::IAllocator& allocator) : host(host), session(host, allocator) {}
  IVirtGpuHost& host;
  graphics::virtgpu::Session session;
  kernel::Mutex read_lock;   // one reader at a time takes events (drm_read's event_read_lock)
  bool non_blocking = false;   // (atomic access)
};

namespace {

// The allocator for objects that may outlive the process (descriptors passed on): the system's when there is one, as /dev/dxg's exports do.
allocators::IAllocator& object_allocator(ProcessFiles& files) {
  ObjectManager* objects = system_objects();
  return objects && objects->allocator() ? *objects->allocator() : files.allocator();
}

// The program's memory, for the session's calls.
class ProcessMemory final : public IUserMemory {
 public:
  explicit ProcessMemory(Process& p) : p_(p) {}
  bool read(uint64_t address, ByteView out) override { return p_.read_user(address, out); }
  bool write(uint64_t address, ConstByteView data) override { return p_.write_user(address, data); }

 private:
  Process& p_;
};

VirtualFile* virtual_file_of(const SharedPointer<OpenObject>& object, VirtualFile::Kind kind) {
  return object && object->virtual_file && object->virtual_file->kind() == kind ? object->virtual_file : nullptr;
}
FenceFile* fence_file(const SharedPointer<OpenObject>& object) { return static_cast<FenceFile*>(virtual_file_of(object, VirtualFile::Kind::Fence)); }
BufferFile* buffer_file(const SharedPointer<OpenObject>& object) { return static_cast<BufferFile*>(virtual_file_of(object, VirtualFile::Kind::Buffer)); }

// A new descriptor for a made-up file `file` (named `path`, as /proc/self/fd shows nothing for it). The descriptor or -errno.
template <class File>
int64_t install_file(Process& p, UniquePointer<File> file, StringView path, bool readable, bool writable, bool cloexec) {
  ProcessFiles& files = p.files();
  allocators::IAllocator& allocator = object_allocator(files);
  SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(allocator, allocator);
  if (!object || !file || !object->path.assign(path)) return -kEnomem;
  object->virtual_file = file.get();
  object->file = util::move(file);
  object->readable = readable;
  object->writable = writable;
  const int64_t fd = files.install(DescriptorKind::File, object, 0);
  if (fd < 0) return -kEmfile;
  files.set_close_on_exec(uint64_t(fd), cloexec);
  return fd;
}

int64_t install_fences(Process& p, IVirtGpuHost& host, const FenceRef* fences, uint32_t count, StringView name, bool cloexec) {
  allocators::IAllocator& allocator = object_allocator(p.files());
  UniquePointer<FenceFile> file = UniquePointer<FenceFile>::make(allocator, allocator, host);
  if (!file) return -kEnomem;
  for (uint32_t i = 0; i < count && i < FenceFile::kMaxFences; ++i) file->fences[file->count++] = fences[i];
  for (uint64_t i = 0; i < name.size() && i + 1 < sizeof(file->name); ++i) file->name[i] = name[i];
  return install_file(p, util::move(file), StringView("anon_inode:sync_file"), true, false, cloexec);
}

// The fences of a sync_file still to wait for, the stub left out. How many.
uint32_t pending_fences(FenceFile& f, FenceRef* out) {
  uint32_t n = 0;
  kernel::ScopedMutex hold(f.lock);
  for (uint32_t i = 0; i < f.count; ++i)
    if (!f.signaled(i)) out[n++] = f.fences[i];
  return n;
}

// Folds fence `add` into the set (dma_fence_unwrap_merge): a signaled one adds nothing; on a timeline the set has already, the later of the two stays.
// False if the set is full.
bool fold(FenceRef* set, uint32_t* count, const FenceRef& add) {
  const bool shared = add.ring == ~0u;   // (fences without a ring are on the device's one timeline, whoever sent them)
  for (uint32_t i = 0; i < *count; ++i) {
    FenceRef& have = set[i];
    if (have.fence == add.fence) return true;
    const bool same = add.ring != FenceFile::kUnknownRing && have.ring == add.ring && (shared || have.context == add.context);
    if (same) {
      if (add.fence > have.fence) have = add;
      return true;
    }
  }
  if (*count == FenceFile::kMaxFences) return false;
  set[(*count)++] = add;
  return true;
}

// ISessionFiles for a Linux process: sync_files and dma-bufs as descriptors.
class LinuxSessionFiles final : public graphics::virtgpu::ISessionFiles {
 public:
  LinuxSessionFiles(Process& p, IVirtGpuHost& host) : p_(p), host_(host) {}
  int32_t make_fence_fd(const FenceRef& fence, bool close_on_exec) override {
    return int32_t(install_fences(p_, host_, &fence, 1, StringView(), close_on_exec));
  }
  bool fence_of_fd(int32_t fd, FenceRef* out) override {
    if (fd < 0) return false;
    SharedPointer<OpenObject> object = p_.files().shared_object(uint64_t(fd));
    FenceFile* f = fence_file(object);
    if (!f || &f->host() != &host_) return false;
    FenceRef pending[FenceFile::kMaxFences];
    const uint32_t n = pending_fences(*f, pending);
    *out = FenceRef{};
    if (n == 0) return true;   // (all signaled: nothing to wait for)
    // Several: the in fence is all of them (a fence array, which Linux waits for whole); all but the last are waited for here, the session handles that one.
    for (uint32_t i = 0; i + 1 < n; ++i) host_.wait_fence(pending[i].fence, ~uint64_t{0});
    *out = pending[n - 1];
    return true;
  }
  int32_t make_buffer_fd(const BufferRef& buffer, uint32_t flags) override {
    allocators::IAllocator& allocator = object_allocator(p_.files());
    UniquePointer<BufferFile> file = UniquePointer<BufferFile>::make(allocator, allocator, buffer);
    return int32_t(install_file(p_, util::move(file), StringView("/dmabuf:"), true, (flags & TCOS_DRM_RDWR) != 0, (flags & TCOS_DRM_CLOEXEC) != 0));
  }
  BufferRef buffer_of_fd(int32_t fd) override {
    if (fd < 0) return BufferRef();
    BufferFile* b = buffer_file(p_.files().shared_object(uint64_t(fd)));
    return b ? b->buffer() : BufferRef();
  }

 private:
  Process& p_;
  IVirtGpuHost& host_;
};

bool interrupted(Process& p) {
  IProcessRuntime* runtime = p.runtime();
  return p.exited() || (runtime && runtime->interrupted()) || p.signal_interrupts();
}

void put_le32(uint8_t* at, uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (8 * i));
}
void put_le64(uint8_t* at, uint64_t v) {
  for (int i = 0; i < 8; ++i) at[i] = uint8_t(v >> (8 * i));
}
void put_text(uint8_t* at, uint32_t room, StringView text) {   // strscpy: cut to fit, NUL-terminated, the rest zero
  for (uint32_t i = 0; i < room; ++i) at[i] = 0;
  for (uint64_t i = 0; i < text.size() && i + 1 < room; ++i) at[i] = uint8_t(text[i]);
}

// --- the sync_file ioctls (drivers/dma-buf/sync_file.c) ---------------------------------------------------------------------------------------------------

constexpr uint32_t kSyncIocMerge = 0xC0303E03u;        // _IOWR('>', 3, struct sync_merge_data) (48 bytes)
constexpr uint32_t kSyncIocFileInfo = 0xC0383E04u;     // _IOWR('>', 4, struct sync_file_info) (56 bytes)
constexpr uint32_t kSyncIocSetDeadline = 0x40103E05u;  // _IOW('>', 5, struct sync_set_deadline) (16 bytes)

uint64_t sync_merge(Process& p, FenceFile& self, uint64_t argument) {
  uint8_t data[48];
  if (!p.read_user(argument, ByteView(data, sizeof(data)))) return error(kEfault);
  ByteReader reader{ConstByteView(data, sizeof(data))};
  reader.skip(32);
  const int32_t fd2 = int32_t(reader.le32());
  reader.skip(4);   // (fence: written)
  const uint32_t flags = reader.le32(), pad = reader.le32();
  if (flags || pad) return error(kEinval);
  SharedPointer<OpenObject> other = fd2 < 0 ? SharedPointer<OpenObject>() : p.files().shared_object(uint64_t(fd2));
  FenceFile* second = fence_file(other);
  if (!second) return error(kEnoent);
  FenceRef merged[FenceFile::kMaxFences];
  uint32_t count = 0;
  FenceFile* const sources[2] = {&self, second};
  for (FenceFile* f : sources) {
    FenceRef pending[FenceFile::kMaxFences];
    const uint32_t n = pending_fences(*f, pending);
    for (uint32_t i = 0; i < n; ++i)
      if (!fold(merged, &count, pending[i])) return error(kEnomem);
  }
  if (count == 0) merged[count++] = FenceRef{};   // (everything has signaled: the stub)
  char name[32] = {};      // (the name, cut at its terminator or at 31 characters)
  for (uint32_t i = 0; i < 31 && data[i]; ++i) name[i] = char(data[i]);
  uint32_t name_length = 0;
  while (name[name_length]) ++name_length;
  const int64_t fd = install_fences(p, self.host(), merged, count, StringView(name, name_length), true);
  if (fd < 0) return error(-fd);
  put_le32(data + 36, uint32_t(int32_t(fd)));
  if (!p.write_user(argument, ConstByteView(data, sizeof(data)))) {
    p.files().close(uint64_t(fd));
    return error(kEfault);
  }
  return 0;
}

uint64_t sync_file_info(Process& p, FenceFile& self, uint64_t argument) {
  uint8_t info[56];
  if (!p.read_user(argument, ByteView(info, sizeof(info)))) return error(kEfault);
  ByteReader reader{ConstByteView(info, sizeof(info))};
  reader.skip(36);   // name, status
  const uint32_t flags = reader.le32(), asked = reader.le32(), pad = reader.le32();
  const uint64_t array = reader.le64();
  if (flags || pad) return error(kEinval);
  const bool all = self.look(p.host().monotonic_ns());
  kernel::ScopedMutex hold(self.lock);
  const uint32_t n = self.count;
  int32_t status = all ? 1 : 0;
  if (asked != 0) {
    if (asked < n) return error(kEinval);
    // struct sync_fence_info: obj_name[32], driver_name[32], status, flags, timestamp_ns
    uint8_t entries[FenceFile::kMaxFences * 80] = {};
    status = 1;
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* e = entries + i * 80;
      const bool stub = self.fences[i].fence == 0;
      const bool done = self.signaled_at[i] != 0;
      put_text(e, 32, StringView(stub ? "stub" : "controlq"));
      put_text(e + 32, 32, StringView(stub ? "stub" : TCOS_VIRTGPU_DRIVER_NAME));
      put_le32(e + 64, done ? 1u : 0u);
      put_le32(e + 68, 0);
      put_le64(e + 72, done ? self.signaled_at[i] : 0);
      if (!done) status = 0;
    }
    if (!p.write_user(array, ConstByteView(entries, uint64_t{n} * 80))) return error(kEfault);
  }
  // The name: the one MERGE gave, else "<driver>-<timeline><context>-<seqno>" of its (first) fence.
  FixedString<64> name;
  if (self.name[0]) {
    uint32_t length = 0;
    while (length < sizeof(self.name) && self.name[length]) ++length;
    (void)name.assign(StringView(self.name, length));
  } else if (n > 0) {
    const FenceRef& f = self.fences[0];
    const uint64_t timeline = f.fence == 0 ? 0 : f.ring == ~0u ? 1 : 2 + uint64_t{f.context} * graphics::virtgpu::Session::kMaxRings + (f.ring & 0xFFu);
    (void)(name.assign(f.fence == 0 ? StringView("stub-stub") : StringView(TCOS_VIRTGPU_DRIVER_NAME "-controlq")) && name.append_uint(timeline) &&
           name.push_back('-') && name.append_uint(f.fence));
  }
  put_text(info, 32, name.view());
  put_le32(info + 32, uint32_t(status));
  put_le32(info + 40, n);
  return p.write_user(argument, ConstByteView(info, sizeof(info))) ? 0 : error(kEfault);
}

uint64_t sync_ioctl(Process& p, FenceFile& self, uint32_t request, uint64_t argument) {
  switch (request) {
    case kSyncIocMerge: return sync_merge(p, self, argument);
    case kSyncIocFileInfo: return sync_file_info(p, self, argument);
    case kSyncIocSetDeadline: {
      uint8_t raw[16];
      if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, sizeof(raw))};
      reader.skip(8);
      if (reader.le64() != 0) return error(kEinval);
      return 0;   // (a hint; virtio-gpu's fences take no deadline)
    }
    default: return error(kEnotty);
  }
}

// --- the dma-buf ioctls (drivers/dma-buf/dma-buf.c) -------------------------------------------------------------------------------------------------------

constexpr uint32_t kDmaBufIoctlSync = 0x40086200u;         // _IOW('b', 0, struct dma_buf_sync)
constexpr uint32_t kDmaBufSetNameA = 0x40046201u;          // _IOW('b', 1, u32)
constexpr uint32_t kDmaBufSetNameB = 0x40086201u;          // _IOW('b', 1, u64) (DMA_BUF_SET_NAME on 64-bit)
constexpr uint32_t kDmaBufExportSyncFile = 0xC0086202u;    // _IOWR('b', 2, struct dma_buf_export_sync_file)
constexpr uint32_t kDmaBufImportSyncFile = 0x40086203u;    // _IOW('b', 3, struct dma_buf_import_sync_file)
constexpr uint64_t kDmaBufSyncRead = 1, kDmaBufSyncWrite = 2, kDmaBufSyncEnd = 4;

uint64_t dmabuf_ioctl(Process& p, BufferFile& self, uint32_t request, uint64_t argument) {
  switch (request) {
    case kDmaBufIoctlSync: {
      uint64_t flags = 0;
      if (!p.read_user_u64(argument, &flags)) return error(kEfault);
      if (flags & ~(kDmaBufSyncRead | kDmaBufSyncWrite | kDmaBufSyncEnd)) return error(kEinval);
      if ((flags & (kDmaBufSyncRead | kDmaBufSyncWrite)) == 0) return error(kEinval);
      return 0;   // (the memory is the same for the CPU and the device: nothing to copy or flush; virtio-gpu's begin/end_cpu_access do nothing either)
    }
    case kDmaBufSetNameA:
    case kDmaBufSetNameB: {
      // strndup_user(name, DMA_BUF_NAME_LEN): at most 31 characters and the terminator, EINVAL for a longer one
      char name[32] = {};
      uint32_t length = 0;
      for (;; ++length) {
        uint8_t c = 0;
        if (!p.read_user(argument + length, ByteView(&c, 1))) return error(kEfault);
        if (c == 0) break;
        if (length + 1 >= sizeof(name)) return error(kEinval);
        name[length] = char(c);
      }
      kernel::ScopedMutex hold(self.lock);
      for (uint32_t i = 0; i < sizeof(name); ++i) self.name[i] = name[i];
      return 0;
    }
    case kDmaBufExportSyncFile: {
      uint8_t raw[8];
      if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, sizeof(raw))};
      const uint32_t flags = reader.le32();
      if (flags & ~uint32_t(kDmaBufSyncRead | kDmaBufSyncWrite)) return error(kEinval);
      if ((flags & (kDmaBufSyncRead | kDmaBufSyncWrite)) == 0) return error(kEinval);
      // The buffer's fences (one: the last work that used it, which is a write as virtio-gpu adds it); the stub if it is idle.
      const BufferRef& b = self.buffer();
      const uint64_t last = b->fence();
      FenceRef fence{};
      if (last && !b->idle()) fence = FenceRef{last, FenceFile::kUnknownRing, 0};
      const int64_t fd = install_fences(p, b->host(), &fence, 1, StringView(), true);
      if (fd < 0) return error(-fd);
      put_le32(raw + 4, uint32_t(int32_t(fd)));
      if (!p.write_user(argument, ConstByteView(raw, sizeof(raw)))) {
        p.files().close(uint64_t(fd));
        return error(kEfault);
      }
      return 0;
    }
    case kDmaBufImportSyncFile: {
      uint8_t raw[8];
      if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
      ByteReader reader{ConstByteView(raw, sizeof(raw))};
      const uint32_t flags = reader.le32();
      const int32_t fd = int32_t(reader.le32());
      if (flags & ~uint32_t(kDmaBufSyncRead | kDmaBufSyncWrite)) return error(kEinval);
      if ((flags & (kDmaBufSyncRead | kDmaBufSyncWrite)) == 0) return error(kEinval);
      SharedPointer<OpenObject> object = fd < 0 ? SharedPointer<OpenObject>() : p.files().shared_object(uint64_t(fd));
      FenceFile* f = fence_file(object);
      if (!f || &f->host() != &self.buffer()->host()) return error(kEinval);
      // The buffer keeps one fence: the latest of its own and the imported ones stays, and the others are waited for now, so that whoever waits for the
      // buffer afterwards has waited for all of them.
      FenceRef pending[FenceFile::kMaxFences];
      const uint32_t n = pending_fences(*f, pending);
      if (n == 0) return 0;
      IVirtGpuHost& h = self.buffer()->host();
      uint64_t keep = self.buffer()->idle() ? 0 : self.buffer()->fence();
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t other = pending[i].fence > keep ? keep : pending[i].fence;
        if (pending[i].fence > keep) keep = pending[i].fence;
        if (other) h.wait_fence(other, ~uint64_t{0});
      }
      self.buffer()->set_fence(keep);
      return 0;
    }
    default: return error(kEnotty);
  }
}

// --- reading the node's events ----------------------------------------------------------------------------------------------------------------------------

uint64_t node_read(Process& p, OpenObject& object, uint64_t buffer, uint64_t count) {
  RenderNodeClient& c = *object.render;
  if (!object.readable) return error(kEbadf);
  constexpr uint64_t kEventBytes = 8, kChunk = 64 * kEventBytes;
  uint8_t chunk[kChunk];
  const uint64_t room = count < kChunk ? count : kChunk;
  for (;;) {
    {
      kernel::ScopedMutex hold(c.read_lock);
      if (room >= kEventBytes && c.session.event_ready()) {
        // (the program's buffer is looked at first: an event taken is not put back, so one that cannot be written must not be taken)
        if (!p.read_user(buffer, ByteView(chunk, room)) || !p.write_user(buffer, ConstByteView(chunk, room))) return error(kEfault);
        const uint32_t got = c.session.take_events(ByteView(chunk, room));
        if (got != 0) return p.write_user(buffer, ConstByteView(chunk, got)) ? got : error(kEfault);
      } else if (room < kEventBytes && c.session.event_ready()) {
        return 0;   // (an event is there but does not fit: drm_read gives back nothing)
      }
    }
    if (__atomic_load_n(&c.non_blocking, __ATOMIC_ACQUIRE)) return error(kEagain);
    if (interrupted(p)) return error(kEintr);
    p.block_sleep(kWaitSliceNs);
  }
}

// Places a mapping of `size` bytes of `storage` (owned by `owner`, cached as `cache` says: a host blob's mode) as do_mmap does.
uint64_t place(Process& p, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint8_t* storage, SharedMemory* owner, MemoryCache cache) {
  uint64_t where = 0;
  if (flags & kMapFixed) {
    if (address % kPage != 0) return error(kEinval);
    if (!space.unmap(address, size)) return error(kEnomem);
    where = address;
  } else if (!space.find_free(size, address ? address : p.mmap_cursor(), &where)) {
    return error(kEnomem);
  }
  if (!space.map_shared(where, size, uint8_t(prot), storage, owner, cache)) return error(kEnomem);
  if (!(flags & kMapFixed) && !address) p.set_mmap_cursor(where + size);
  return where;
}

}  // namespace

void set_render_node_pci(const RenderNodePci* info) { __atomic_store_n(&g_pci, info, __ATOMIC_RELEASE); }

namespace linuxabi {

int64_t render_node_at(StringView virtual_path, bool no_follow, RenderNodeMeta* meta) {
  N id;
  const int64_t r = lookup(virtual_path, !no_follow, &id);
  if (r <= 0) return r;
  *meta = meta_of(id);
  return 1;
}

int64_t render_node_link(StringView virtual_path, String* target) {
  N id;
  const int64_t r = lookup(virtual_path, false, &id);
  if (r <= 0) return r;
  if (row(id).type != Type::Link) return -kEinval;
  // The way from the link's folder to its target: up to where the two paths part, then down.
  N from[kMaxDepth], to[kMaxDepth];
  const uint32_t a = chain(row(id).parent, from), b = chain(row(id).target, to);
  // (kernfs_get_target_path: the common ancestor of the link's folder and the target's parent, so the target's own name is always spelled)
  uint32_t common = 0;
  while (common < a && common + 1 < b && from[common] == to[common]) ++common;
  target->clear();
  for (uint32_t i = common; i < a; ++i)
    if (!target->append("../")) return -kEnomem;
  for (uint32_t i = common; i < b; ++i) {
    Name name;
    if (!name_of(to[i], pci(), &name) || (i != common && !target->push_back('/')) || !target->append(name.view())) return -kEnomem;
  }
  return 1;
}

bool render_node_device_exists(StringView virtual_path) {
  N id;
  return lookup(virtual_path, true, &id) == 1 && id == N::DevNode;
}

int64_t render_node_open(Process& p, StringView virtual_path, uint64_t flags, bool cloexec, bool must_be_directory, bool no_follow) {
  N id;
  const int64_t r = lookup(virtual_path, !no_follow, &id);
  if (r < 0) return r;
  if (r == 0) return -kEnoent;
  ProcessFiles& files = p.files();
  if (!files.attached()) return -kEnoent;
  const Row& node = row(id);
  if (node.type == Type::Link) return -kEloop;   // (O_NOFOLLOW on a link)
  if (must_be_directory && node.type != Type::Directory) return -kEnotdir;
  const uint64_t access = flags & kOAccMode;
  allocators::IAllocator& allocator = object_allocator(files);
  String canonical(files.allocator());
  if (!path_of(id, pci(), &canonical)) return -kEnomem;
  SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(allocator, allocator);
  if (!object || !object->path.assign(canonical.view())) return -kEnomem;
  DescriptorKind kind = DescriptorKind::File;
  switch (node.type) {
    case Type::Directory: {
      if (access != kORdOnly) return -kEisdir;
      object->directory = UniquePointer<filesystem::IDirectory>::make_as<TreeDirectory>(allocator, id);
      if (!object->directory) return -kEnomem;
      object->readable = true;
      kind = DescriptorKind::Directory;
      break;
    }
    case Type::Attribute:
    case Type::Config: {
      // (the attributes are read-only here: an attribute a driver can be told something through, uevent, takes nothing either: EACCES, as Linux
      // answers an open for writing of an attribute without a store method)
      if (access != kORdOnly) return -kEacces;
      const RenderNodePci* info = pci();
      if (!info) return -kEnoent;
      UniquePointer<AttributeFile> file = UniquePointer<AttributeFile>::make(allocator, allocator);
      if (!file) return -kEnomem;
      if (node.type == Type::Config) {
        // An unprivileged reader sees the first 64 bytes (the standard header), as pci_read_config gives them; the system's own user all 256.
        const uint64_t visible = p.linux_state().effective_user == 0 ? sizeof(info->config) : 64;
        if (!file->contents().append(ConstByteView(info->config, visible))) return -kEnomem;
        file->set_size(sizeof(info->config));
      } else {
        String text(files.allocator());
        if (!attribute_text(id, *info, &text) || !file->contents().append(text.view().bytes())) return -kEnomem;
        file->set_size(kPage);
      }
      object->virtual_file = file.get();
      object->file = util::move(file);
      object->readable = true;
      break;
    }
    case Type::Device: {
      IVirtGpuHost* device = host();
      if (!device) return -kEnoent;
      object->render = UniquePointer<RenderNodeClient>::make(allocator, *device, allocator);
      if (!object->render) return -kEnomem;
      object->render->non_blocking = (flags & kONonBlock) != 0;
      object->readable = access != kOWrite;
      object->writable = access != kORdOnly;
      kind = DescriptorKind::RenderNode;
      break;
    }
    case Type::Link: return -kEloop;
  }
  const int64_t fd = files.install(kind, object, 0);
  if (fd < 0) return -kEmfile;
  files.set_close_on_exec(uint64_t(fd), cloexec);
  return fd;
}

bool render_node_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t* result) {
  ProcessFiles& files = p.files();
  SharedPointer<OpenObject> object = files.shared_object(fd);
  if (files.kind(fd) == DescriptorKind::RenderNode && object && object->render) {
    *result = node_read(p, *object, buffer, count);
    return true;
  }
  if (fence_file(object) || buffer_file(object)) {      // (no read on a sync_file or a dma-buf: EBADF when not readable, else EINVAL, as vfs_read checks)
    *result = object->readable ? error(kEinval) : error(kEbadf);
    return true;
  }
  return false;
}

bool render_node_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument, uint64_t* result) {
  ProcessFiles& files = p.files();
  SharedPointer<OpenObject> object = files.shared_object(fd);      // (held: another thread may close the descriptor meanwhile)
  if (!object) return false;
  const uint32_t code = uint32_t(request);      // (an unsigned int: the upper half is not part of it)
  if (files.kind(fd) == DescriptorKind::RenderNode && object->render) {
    RenderNodeClient& c = *object->render;
    ProcessMemory memory(p);
    LinuxSessionFiles session_files(p, c.host);
    const int32_t r = c.session.ioctl(memory, code, argument, &session_files);
    *result = uint64_t(int64_t(r));
    return true;
  }
  if (FenceFile* f = fence_file(object)) {
    *result = sync_ioctl(p, *f, code, argument);
    return true;
  }
  if (BufferFile* b = buffer_file(object)) {
    *result = dmabuf_ioctl(p, *b, code, argument);
    return true;
  }
  if (object->virtual_file) {      // (a /sys attribute)
    *result = error(kEnotty);
    return true;
  }
  return false;
}

bool render_node_seek(Process& p, uint64_t fd, int64_t offset, uint64_t whence, uint64_t* result) {
  ProcessFiles& files = p.files();
  SharedPointer<OpenObject> object = files.shared_object(fd);
  if (files.kind(fd) == DescriptorKind::RenderNode) {      // (noop_llseek: the position stays 0)
    *result = 0;
    return true;
  }
  if (fence_file(object)) {      // (no llseek)
    *result = error(kEspipe);
    return true;
  }
  if (BufferFile* b = buffer_file(object)) {      // dma_buf_llseek: SEEK_SET or SEEK_END, offset 0 only
    if ((whence != 0 && whence != 2) || offset != 0) *result = error(kEinval);
    else *result = whence == 2 ? b->buffer()->size() : 0;
    return true;
  }
  return false;
}

bool render_node_map(Process& p, AddressSpace& space, uint64_t address, uint64_t size, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset, uint64_t* result) {
  ProcessFiles& files = p.files();
  SharedPointer<OpenObject> object = files.shared_object(fd);
  if (!object) return false;
  const bool node = files.kind(fd) == DescriptorKind::RenderNode && object->render;
  BufferFile* dmabuf = buffer_file(object);
  if (!node && !dmabuf) {
    if (!object->virtual_file) return false;
    *result = error(kEnodev);      // (a sync_file or a /sys attribute cannot be mapped)
    return true;
  }
  // (mmap's own checks on the open file: it must be readable, and writable for a shared writable mapping)
  if (!object->readable || ((flags & kMapShared) && (prot & kProtWrite) && !object->writable)) {
    *result = error(kEacces);
    return true;
  }
  if (!(flags & kMapShared) || (flags & kMapPrivate) || offset % kPage != 0) {      // (GEM objects are not mapped copy-on-write: drm_gem_mmap_obj)
    *result = error(kEinval);
    return true;
  }
  BufferRef buffer;
  uint64_t at = 0;
  if (node) {
    buffer = object->render->session.buffer_at(offset, size);      // (drm_vma_offset_exact_lookup_locked: the buffer MAP gave `offset` for, if `size` fits)
  } else {
    buffer = dmabuf->buffer();
    if (offset > buffer->size() || size > buffer->size() - offset) buffer.reset();
    else if (!buffer->mappable() && !buffer->map_now()) buffer.reset();      // (a host blob the device has not mapped yet; one that is not mappable fails)
    at = offset;
  }
  if (!buffer || !buffer->mappable()) {
    *result = error(kEinval);
    return true;
  }
  *result = place(p, space, address, size, prot, flags, buffer->storage() + at, buffer.get(), buffer->cpu_cache());      // (the mapping holds the buffer from here on, with the host's cache mode)
  return true;
}

bool render_node_poll(Process& p, uint64_t fd, uint16_t* events) {
  ProcessFiles& files = p.files();
  SharedPointer<OpenObject> object = files.shared_object(fd);
  if (files.kind(fd) == DescriptorKind::RenderNode && object && object->render) {      // drm_poll: readable when an event waits
    *events = object->render->session.event_ready() ? uint16_t(kPollIn | kPollRdNorm) : 0;
    return true;
  }
  if (FenceFile* f = fence_file(object)) {      // sync_file_poll: EPOLLIN once signaled
    *events = f->look(p.host().monotonic_ns()) ? kPollIn : 0;
    return true;
  }
  if (BufferFile* b = buffer_file(object)) {      // dma_buf_poll: EPOLLIN once the writers are done, EPOLLOUT once everyone is (one fence: a write)
    *events = b->buffer()->idle() ? uint16_t(kPollIn | kPollOut) : 0;
    return true;
  }
  return false;
}

bool render_node_descriptor_meta(Process& p, uint64_t fd, RenderNodeMeta* meta) {
  ProcessFiles& files = p.files();
  const DescriptorKind kind = files.kind(fd);
  OpenObject* object = files.object(fd);
  if (!object) return false;
  if (kind == DescriptorKind::RenderNode) {
    *meta = meta_of(N::DevNode);
    return true;
  }
  if (kind == DescriptorKind::Directory && object->directory) {
    N id;
    if (lookup(object->path.view(), true, &id) != 1 || row(id).type != Type::Directory) return false;
    *meta = meta_of(id);
    return true;
  }
  if (!object->virtual_file) return false;
  *meta = RenderNodeMeta();
  switch (object->virtual_file->kind()) {
    case VirtualFile::Kind::Attribute: {
      N id;
      if (lookup(object->path.view(), true, &id) == 1) *meta = meta_of(id);
      else meta->mode = kSIfReg | 0444, meta->size = kPage, meta->ino = fnv(object->path.view());
      return true;
    }
    case VirtualFile::Kind::Buffer: {      // (an inode of the dma-buf file system: S_IRUSR | S_IWUSR, no type, the buffer's size; one per buffer)
      const BufferFile& b = *static_cast<BufferFile*>(object->virtual_file);
      meta->mode = 0600;
      meta->size = b.buffer()->size();
      meta->ino = 0x646D616200000000ull | b.buffer()->resource();   // (the buffer's resource: every descriptor of it shows the same)
      return true;
    }
    case VirtualFile::Kind::Fence:      // (the anonymous inode every sync_file shares)
      meta->mode = 0600;
      meta->ino = 0x73796E63;
      return true;
  }
  return false;
}

bool render_node_descriptor_path(Process& p, uint64_t fd, String* out) {
  ProcessFiles& files = p.files();
  OpenObject* object = files.object(fd);
  if (files.kind(fd) == DescriptorKind::RenderNode) return out->assign(TCOS_VIRTGPU_RENDER_NODE_PATH);
  if (!object || !object->virtual_file) return false;
  return out->assign(object->path.view());      // (an attribute's path; "/dmabuf:", "anon_inode:sync_file" for the others, as Linux names them)
}

bool render_node_non_blocking(Process& p, uint64_t fd) {
  OpenObject* object = p.files().object(fd);
  return p.files().kind(fd) == DescriptorKind::RenderNode && object && object->render && __atomic_load_n(&object->render->non_blocking, __ATOMIC_ACQUIRE);
}

void set_render_node_non_blocking(Process& p, uint64_t fd, bool value) {
  OpenObject* object = p.files().object(fd);
  if (p.files().kind(fd) == DescriptorKind::RenderNode && object && object->render) __atomic_store_n(&object->render->non_blocking, value, __ATOMIC_RELEASE);
}

}  // namespace linuxabi
}  // namespace process
