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
#include <new>
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Kernel/Rcu.hpp"
#include "Network/NetworkTypes.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// A socket's address in a lookup table: its ports, its peer (for a connection) and the IP versions it takes (for a listener or a UDP port).
struct SocketKey {
  uint16_t local_port = 0, remote_port = 0;
  IpAddress remote;
  IpFamilies families = IpFamilies::Both;
};

inline uint32_t hash_port(uint16_t port) { return port * 2654435761u; }
// (the remote address and both ports: a connection's whole identity; FNV-1a)
inline uint32_t hash_connection(uint16_t local_port, uint16_t remote_port, const IpAddress& remote) {
  uint32_t h = 2166136261u;
  const auto mix = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
  mix(static_cast<uint8_t>(local_port >> 8));
  mix(static_cast<uint8_t>(local_port));
  mix(static_cast<uint8_t>(remote_port >> 8));
  mix(static_cast<uint8_t>(remote_port));
  if (remote.v6)
    for (uint8_t b : remote.ipv6.bytes) mix(b);
  else
    for (uint8_t b : remote.ipv4.octets) mix(b);
  return h;
}

// A hash table of sockets that is read with RCU (CLAUDE.md, "Concurrent data structures": a lookup table read far more often than it changes -
// every arriving segment looks its socket up, a socket is added once and removed once). Lookups never lock or write: they walk a bucket inside a
// read-side section and take a SharedPointer copy of the socket they find, which keeps it alive after the section. Writers (insert, remove) are
// serialised by the caller's lock; a removed node is unlinked at once and freed after a grace period in the table's own RCU domain, so the
// deleters - which may drop the last reference to a socket - run on the network's threads, not wherever some other domain polls.
//
// kBuckets is fixed (a power of two): with this OS's connection counts the chains stay short; a table that must hold many thousands of sockets
// would want more of them.
template <class Socket, uint32_t kBuckets = 256>
class RcuSocketTable {
  static_assert((kBuckets & (kBuckets - 1)) == 0, "a power of two");

 public:
  explicit RcuSocketTable(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~RcuSocketTable() {
    for (uint32_t b = 0; b < kBuckets; ++b)
      while (Node* n = heads_[b]) {
        heads_[b] = n->next;
        n->retire(Free{}, domain_);
      }
    domain_.barrier();   // (every retired node is freed before the domain and the allocator its deleters use go away)
  }
  RcuSocketTable(const RcuSocketTable&) = delete;
  RcuSocketTable& operator=(const RcuSocketTable&) = delete;

  // Writer (the caller's lock held): adds `socket` under `key` in the bucket of `hash`. False if out of memory.
  bool insert(uint32_t hash, const SocketKey& key, const SharedPointer<Socket>& socket) {
    void* memory = allocator_.allocate(sizeof(Node));
    if (!memory) return false;
    Node* node = new (memory) Node();   // (placement new into allocator memory: RCU ends the node's life, not an owner - see Free)
    node->key = key;
    node->socket = socket;
    node->allocator = &allocator_;
    Node** head = &heads_[hash & (kBuckets - 1)];
    node->next = *head;
    kernel::rcu_assign_pointer(head, node);   // (published whole: a reader sees the node with its key and socket, or not at all)
    __atomic_fetch_add(&count_, 1, __ATOMIC_RELAXED);
    return true;
  }

  // Writer: removes `socket`'s node from the bucket of `hash` (readers already in it may still see it until they leave). False if not there.
  bool remove(uint32_t hash, const Socket* socket) {
    Node** link = &heads_[hash & (kBuckets - 1)];
    for (Node* n = *link; n; link = &n->next, n = *link) {
      if (n->socket.get() != socket) continue;
      kernel::rcu_assign_pointer(link, n->next);
      n->retire(Free{}, domain_);
      __atomic_fetch_sub(&count_, 1, __ATOMIC_RELAXED);
      return true;
    }
    return false;
  }

  // Reader (any thread, no lock): the first socket in the bucket of `hash` that `match(key, socket)` accepts, or null.
  template <typename Match>
  SharedPointer<Socket> find(uint32_t hash, Match&& match) const {
    kernel::RcuReader reader(domain_);
    for (const Node* n = kernel::rcu_dereference(&heads_[hash & (kBuckets - 1)]); n; n = kernel::rcu_dereference(&n->next))
      if (match(n->key, *n->socket)) return n->socket;
    return SharedPointer<Socket>();
  }

  // Reader: calls `visit(key, socket)` for every entry of the bucket of `hash` until it returns true; true if one did. (The socket only for a
  // look at what may be read without its lock: the section ends with the call.)
  template <typename Visit>
  bool any(uint32_t hash, Visit&& visit) const {
    kernel::RcuReader reader(domain_);
    for (const Node* n = kernel::rcu_dereference(&heads_[hash & (kBuckets - 1)]); n; n = kernel::rcu_dereference(&n->next))
      if (visit(n->key, *n->socket)) return true;
    return false;
  }

  uint32_t count() const { return __atomic_load_n(&count_, __ATOMIC_RELAXED); }   // (changed by writers, read by anyone)

 private:
  struct Node;
  struct Free {
    void operator()(Node* node) const {
      allocators::IAllocator* allocator = node->allocator;
      node->~Node();
      allocator->deallocate(node);
    }
  };
  struct Node : kernel::RcuObjectBase<Node, Free> {
    SocketKey key;
    SharedPointer<Socket> socket;
    Node* next = nullptr;
    allocators::IAllocator* allocator = nullptr;
  };

  allocators::IAllocator& allocator_;
  mutable kernel::RcuDomain domain_;
  Node* heads_[kBuckets] = {};
  uint32_t count_ = 0;
};

}  // namespace network
