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

#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Move.hpp"

namespace filesystem {

namespace {
// A modification time becomes now - unless there is no clock, which leaves the time alone.
void touch_time(uint64_t* modified) {
  const uint64_t now = unix_time_now();
  if (now) *modified = now;
}
constexpr uint32_t kMaxSymlinkHops = 32;
}  // namespace

// What a file's name(s) point to: its bytes, and the metadata real file systems keep once per file rather than once per name (NTFS hard links work the same way). A
// directory or a symbolic link has no Content; only File nodes do, and create_hard_link() is what gives two Nodes the same one.
struct RamFileSystem::Content {
  explicit Content(allocators::IAllocator& a) : allocator(a) {}
  ~Content() { allocator.deallocate(data); }
  allocators::IAllocator& allocator;
  uint8_t* data = nullptr;
  uint64_t size = 0;
  uint64_t capacity = 0;
  uint32_t open_count = 0;
  uint64_t created = 0, modified = 0;    // seconds since 1970 UTC (0: no clock when it was made)
  bool read_only = false;
};

struct RamFileSystem::Node {
  Node(allocators::IAllocator& a, Node* parent_node, StringView node_name, NodeType node_type)
      : allocator(a), parent(parent_node), type(node_type), children(a), target(a) {
    name.assign(node_name);
    created = modified = unix_time_now();
  }
  ~Node() = default;

  allocators::IAllocator& allocator;
  Node* parent;
  FixedString<kMaxNameLength + 1> name;
  NodeType type;
  List<UniquePointer<Node>> children;      // directories
  SharedPointer<Content> content;          // files (shared by every hard link to the same data)
  String target;                           // symbolic links: where they point (in this filesystem's own path space)
  bool target_is_directory = false;        // symbolic links: a hint for a target that may not exist yet
  uint32_t open_count = 0;                 // directories and symbolic links (files count through their Content instead, shared by every link)
  uint64_t created = 0, modified = 0;      // directories and symbolic links keep their own (a file's are on its Content, shared by every hard link)
};

class RamFileSystem::File final : public IFile {
 public:
  File(RamFileSystem& fs, SharedPointer<Content> content, bool writable) : fs_(fs), content_(util::move(content)), writable_(writable) { ++content_->open_count; }
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    --content_->open_count;
  }

  uint64_t size() const override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    return content_->size;
  }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (offset >= content_->size) return Status::Ok;
    uint64_t count = content_->size - offset;
    if (count > buffer.size()) count = buffer.size();
    if (count) __builtin_memcpy(buffer.data(), content_->data + offset, count);
    *transferred = count;
    return Status::Ok;
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    if (data.empty()) return Status::Ok;
    if (data.size() > ~uint64_t{0} - offset) return Status::InvalidArgument;
    const uint64_t end = offset + data.size();
    if (end > content_->size) {
      const Status grown = fs_.set_size(*content_, end);  // zero-fills the gap
      if (grown != Status::Ok) return grown;
    }
    __builtin_memmove(content_->data + offset, data.data(), data.size());
    touch_time(&content_->modified);
    *transferred = data.size();
    return Status::Ok;
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    const Status s = fs_.set_size(*content_, new_size);
    if (s == Status::Ok) touch_time(&content_->modified);
    return s;
  }
  Status flush() override { return Status::Ok; }

 private:
  RamFileSystem& fs_;
  SharedPointer<Content> content_;
  const bool writable_;
};

class RamFileSystem::Directory final : public IDirectory {
 public:
  Directory(RamFileSystem& fs, Node& node) : fs_(fs), node_(node) { ++node_.open_count; }
  ~Directory() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    --node_.open_count;
  }

  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (index_ >= node_.children.count()) return Status::EndOfDirectory;
    describe(*node_.children[index_++], entry);
    return Status::Ok;
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    index_ = 0;
  }

 private:
  RamFileSystem& fs_;
  Node& node_;
  uint64_t index_ = 0;
};

RamFileSystem::RamFileSystem(allocators::IAllocator& allocator, uint64_t limit_bytes)
    : allocator_(allocator), limit_(limit_bytes), root_(UniquePointer<Node>::make(allocator, allocator, nullptr, StringView(), NodeType::Directory)) {}

RamFileSystem::~RamFileSystem() = default;

FileSystemInfo RamFileSystem::info() const {
  kernel::ScopedRecursiveMutex lock(mutex_);
  FileSystemInfo result;
  result.type_name.assign("ramfs");
  result.total_bytes = limit_;
  result.free_bytes = limit_ ? limit_ - used_ : 0;
  return result;
}

uint64_t RamFileSystem::find_child(const Node& directory, StringView name) {
  for (uint64_t i = 0; i < directory.children.count(); ++i)
    if (directory.children[i]->name.equals(name)) return i;
  return StringView::npos;
}

void RamFileSystem::describe(const Node& node, DirectoryEntry* entry) {
  entry->name = node.name;
  entry->is_symlink = node.type == NodeType::Symlink;
  entry->type = node.type == NodeType::Directory || (node.type == NodeType::Symlink && node.target_is_directory) ? NodeType::Directory : NodeType::File;
  const bool file_node = node.type == NodeType::File && node.content;
  entry->size = file_node ? node.content->size : 0;
  entry->read_only = file_node ? node.content->read_only : false;
  entry->modified = file_node ? node.content->modified : node.modified;
  entry->created = file_node ? node.content->created : node.created;
}

Status RamFileSystem::follow_symlink(const Node& link, Node* from, Node** node, uint32_t* hops) {
  if (++*hops > kMaxSymlinkHops) return Status::TooManyLinks;
  const StringView target = link.target.view();
  if (target.empty()) return Status::NotFound;
  Node* start = !target.empty() && target[0] == '/' ? root_.get() : from;
  return resolve_from(start, target, node, hops, /*allow_dotdot=*/true);
}

Status RamFileSystem::resolve(StringView path, Node** node, uint32_t* hops) {
  if (!root_) return Status::OutOfMemory;
  return resolve_from(root_.get(), path, node, hops);
}

Status RamFileSystem::resolve_from(Node* start, StringView path, Node** node, uint32_t* hops, bool allow_dotdot) {
  Node* current = start;
  StringView rest = path, component;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..")) {
      if (!allow_dotdot) return Status::InvalidPath;
      if (current->parent) current = current->parent;      // a relative symbolic link's target climbing back up; at the root it stays put
      continue;
    }
    if (current->type == NodeType::Symlink) {
      Node* target = nullptr;
      const Status followed = follow_symlink(*current, current->parent, &target, hops);
      if (followed != Status::Ok) return followed;
      current = target;
    }
    if (current->type != NodeType::Directory) return Status::NotADirectory;
    const uint64_t at = find_child(*current, component);
    if (at == StringView::npos) return Status::NotFound;
    current = current->children[at].get();
  }
  if (current->type == NodeType::Symlink) {
    Node* target = nullptr;
    const Status followed = follow_symlink(*current, current->parent, &target, hops);
    if (followed != Status::Ok) return followed;
    current = target;
  }
  *node = current;
  return Status::Ok;
}

Status RamFileSystem::resolve_parent(StringView path, Node** parent, StringView* leaf) {
  StringView parent_path;
  if (!split_parent(path, &parent_path, leaf)) return Status::InvalidPath;  // the root has no parent
  if (!valid_name(*leaf)) return Status::InvalidPath;
  Node* directory = nullptr;
  const Status found = resolve(parent_path, &directory);
  if (found != Status::Ok) return found;
  if (directory->type != NodeType::Directory) return Status::NotADirectory;
  *parent = directory;
  return Status::Ok;
}

Status RamFileSystem::add_child(Node& parent, StringView name, NodeType type, Node** created) {
  UniquePointer<Node> node = UniquePointer<Node>::make(allocator_, allocator_, &parent, name, type);
  if (!node) return Status::OutOfMemory;
  Node* raw = node.get();
  if (!parent.children.push_back(util::move(node))) return Status::OutOfMemory;
  touch_time(&parent.modified);
  *created = raw;
  return Status::Ok;
}

// Sets a file's length, zero-filling anything new, within the size limit.
Status RamFileSystem::set_size(Content& content, uint64_t new_size) {
  if (new_size > content.size) {
    if (limit_ && new_size - content.size > limit_ - used_) return Status::NoSpace;
    if (new_size > content.capacity) {
      uint64_t capacity = content.capacity ? content.capacity * 2 : 64;
      if (capacity < new_size) capacity = new_size;
      if (limit_ && capacity > limit_) capacity = new_size > limit_ ? new_size : limit_;
      uint8_t* fresh = static_cast<uint8_t*>(allocator_.allocate(capacity));
      if (!fresh) return Status::OutOfMemory;
      if (content.size) __builtin_memcpy(fresh, content.data, content.size);
      allocator_.deallocate(content.data);
      content.data = fresh;
      content.capacity = capacity;
    }
    __builtin_memset(content.data + content.size, 0, new_size - content.size);
    used_ += new_size - content.size;
  } else {
    used_ -= content.size - new_size;
  }
  content.size = new_size;
  return Status::Ok;
}

Status RamFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* node = nullptr;
  if (mode == OpenMode::Read || mode == OpenMode::ReadWrite) {
    const Status found = resolve(path, &node);
    if (found != Status::Ok) return found;
    if (node->type != NodeType::File) return node->type == NodeType::Directory ? Status::IsADirectory : Status::NotFound;
    if (mode == OpenMode::ReadWrite && node->content->read_only) return Status::ReadOnly;
  } else {
    Node* parent = nullptr;
    StringView leaf;
    const Status found = resolve_parent(path, &parent, &leaf);
    if (found != Status::Ok) return found;
    const uint64_t at = find_child(*parent, leaf);
    if (at == StringView::npos) {
      const Status added = add_child(*parent, leaf, NodeType::File, &node);
      if (added != Status::Ok) return added;
      node->content = SharedPointer<Content>::make(allocator_, allocator_);
      if (!node->content) { parent->children.remove_at(parent->children.count() - 1); return Status::OutOfMemory; }
      node->content->created = node->content->modified = unix_time_now();
    } else {
      node = parent->children[at].get();
      if (node->type == NodeType::Symlink) {
        uint32_t hops = 0;
        const Status followed = follow_symlink(*node, parent, &node, &hops);
        if (followed != Status::Ok) return followed;
      }
      if (node->type != NodeType::File) return Status::IsADirectory;
      if (node->content->read_only) return Status::ReadOnly;
      if (mode == OpenMode::CreateTruncate) {
        if (node->content->open_count) return Status::Busy;
        set_size(*node->content, 0);
      }
    }
  }
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, node->content, mode != OpenMode::Read);
  if (!handle) return Status::OutOfMemory;
  *file = util::move(handle);
  return Status::Ok;
}

Status RamFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* node = nullptr;
  const Status found = resolve(path, &node);
  if (found != Status::Ok) return found;
  if (node->type != NodeType::Directory) return Status::NotADirectory;
  UniquePointer<IDirectory> handle = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, *node);
  if (!handle) return Status::OutOfMemory;
  *directory = util::move(handle);
  return Status::Ok;
}

Status RamFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* node = nullptr;
  const Status found = resolve(path, &node);
  if (found != Status::Ok) return found;
  describe(*node, entry);
  return Status::Ok;
}

Status RamFileSystem::set_metadata(StringView path, const MetadataChange& change) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* node = nullptr;
  const Status found = resolve(path, &node);
  if (found != Status::Ok) return found;
  if (node->type != NodeType::File) return Status::Unsupported;      // directories and symbolic links keep none of this
  if (change.set_read_only) node->content->read_only = change.read_only;
  if (change.set_modified) node->content->modified = change.modified;
  if (change.set_created) node->content->created = change.created;
  return Status::Ok;
}

Status RamFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* parent = nullptr;
  StringView leaf;
  const Status found = resolve_parent(path, &parent, &leaf);
  if (found != Status::Ok) return found;
  if (find_child(*parent, leaf) != StringView::npos) return Status::AlreadyExists;
  Node* created = nullptr;
  return add_child(*parent, leaf, NodeType::Directory, &created);
}

Status RamFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* parent = nullptr;
  StringView leaf;
  const Status found = resolve_parent(path, &parent, &leaf);
  if (found != Status::Ok) return found;
  const uint64_t at = find_child(*parent, leaf);
  if (at == StringView::npos) return Status::NotFound;
  Node& node = *parent->children[at];
  const bool busy = node.type == NodeType::File ? (node.content && node.content->open_count) : node.open_count != 0;
  if (busy) return Status::Busy;
  if (node.type == NodeType::Directory && node.children.count()) return Status::NotEmpty;
  // Only when this is the last name for the content (no other hard link, and nothing has it open, or this would have been Busy above) does removing it free the bytes.
  if (node.type == NodeType::File && node.content && node.content.use_count() == 1) used_ -= node.content->size;
  parent->children.remove_at(at);
  touch_time(&parent->modified);
  return Status::Ok;
}

Status RamFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* from_parent = nullptr;
  Node* to_parent = nullptr;
  StringView from_leaf, to_leaf;
  Status found = resolve_parent(from, &from_parent, &from_leaf);
  if (found != Status::Ok) return found;
  found = resolve_parent(to, &to_parent, &to_leaf);
  if (found != Status::Ok) return found;
  const uint64_t at = find_child(*from_parent, from_leaf);
  if (at == StringView::npos) return Status::NotFound;
  Node* node = from_parent->children[at].get();
  if (find_child(*to_parent, to_leaf) != StringView::npos)
    return from_parent == to_parent && from_leaf.equals(to_leaf) ? Status::Ok : Status::AlreadyExists;
  for (const Node* up = to_parent; up; up = up->parent)
    if (up == node) return Status::InvalidArgument;  // into itself
  if (!to_parent->children.reserve(to_parent->children.count() + 1)) return Status::OutOfMemory;
  UniquePointer<Node> moved = util::move(from_parent->children[at]);
  from_parent->children.remove_at(at);
  node->name.assign(to_leaf);
  node->parent = to_parent;
  to_parent->children.push_back(util::move(moved));  // cannot fail: reserved above
  touch_time(&from_parent->modified);
  touch_time(&to_parent->modified);
  return Status::Ok;
}

Status RamFileSystem::create_hard_link(StringView existing, StringView link) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* target = nullptr;
  Status found = resolve(existing, &target);
  if (found != Status::Ok) return found;
  if (target->type != NodeType::File) return Status::IsADirectory;      // only files can be hard-linked, as on a real file system
  Node* parent = nullptr;
  StringView leaf;
  found = resolve_parent(link, &parent, &leaf);
  if (found != Status::Ok) return found;
  if (find_child(*parent, leaf) != StringView::npos) return Status::AlreadyExists;
  Node* created = nullptr;
  found = add_child(*parent, leaf, NodeType::File, &created);
  if (found != Status::Ok) return found;
  created->content = target->content;      // the new name shares the same data, size, times and read-only flag
  return Status::Ok;
}

Status RamFileSystem::create_symbolic_link(StringView link, StringView target, bool target_is_directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* parent = nullptr;
  StringView leaf;
  const Status found = resolve_parent(link, &parent, &leaf);
  if (found != Status::Ok) return found;
  if (target.empty() || target.size() > 4096) return Status::InvalidPath;
  if (find_child(*parent, leaf) != StringView::npos) return Status::AlreadyExists;
  Node* created = nullptr;
  const Status added = add_child(*parent, leaf, NodeType::Symlink, &created);
  if (added != Status::Ok) return added;
  if (!created->target.assign(target)) { parent->children.remove_at(parent->children.count() - 1); return Status::OutOfMemory; }
  created->target_is_directory = target_is_directory;
  return Status::Ok;
}

Status RamFileSystem::read_symbolic_link(StringView path, String* target, bool* target_is_directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Node* parent = nullptr;
  StringView leaf;
  const Status found = resolve_parent(path, &parent, &leaf);
  if (found != Status::Ok) return found;
  const uint64_t at = find_child(*parent, leaf);
  if (at == StringView::npos) return Status::NotFound;
  Node& node = *parent->children[at];
  if (node.type != NodeType::Symlink) return Status::InvalidArgument;
  if (!target->assign(node.target.view())) return Status::OutOfMemory;
  *target_is_directory = node.target_is_directory;
  return Status::Ok;
}

}  // namespace filesystem
