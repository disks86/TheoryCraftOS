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
#include "Utility/AllocatedView.hpp"
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
// The bucket array grows (doubling past two entries a bucket) and shrinks (halving below one in four, never under kMinBuckets) with what the
// table holds. A resize builds a new array with copies of every node, publishes it with one rcu_assign_pointer, and retires the old array and
// nodes: a reader already in the old one finishes its walk there (it is intact until the grace period ends), later readers see only the new
// one. A resize that runs out of memory leaves the table as it was (only its chains are longer). Nothing exists until the first insert.
template <class Socket, uint32_t kMinBuckets = 64>
class RcuSocketTable {
  static_assert(kMinBuckets && (kMinBuckets & (kMinBuckets - 1)) == 0, "a power of two");

 public:
  static constexpr uint32_t kMaxBuckets = 1u << 20;

  explicit RcuSocketTable(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~RcuSocketTable() {
    if (Buckets* b = buckets_) {
      for (Node*& head : b->heads)
        while (Node* n = head) {
          head = n->next;
          n->retire(FreeNode{}, domain_);
        }
      b->retire(FreeBuckets{}, domain_);
    }
    domain_.barrier();   // (every retired node is freed before the domain and the allocator its deleters use go away)
  }
  RcuSocketTable(const RcuSocketTable&) = delete;
  RcuSocketTable& operator=(const RcuSocketTable&) = delete;

  // Writer (the caller's lock held): adds `socket` under `key` in the bucket of `hash`. False if out of memory.
  bool insert(uint32_t hash, const SocketKey& key, const SharedPointer<Socket>& socket) {
    if (!buckets_) {
      Buckets* first = make_buckets(kMinBuckets);
      if (!first) return false;
      kernel::rcu_assign_pointer(&buckets_, first);
    }
    const uint32_t held = count();
    if (held >= 2 * bucket_count() && bucket_count() < kMaxBuckets) resize(2 * bucket_count());   // (failing is fine: longer chains)
    Node* node = make_node(hash, key, socket);
    if (!node) return false;
    Node** head = &buckets_->heads[hash & buckets_->mask];
    node->next = *head;
    kernel::rcu_assign_pointer(head, node);   // (published whole: a reader sees the node with its key and socket, or not at all)
    __atomic_fetch_add(&count_, 1, __ATOMIC_RELAXED);
    return true;
  }

  // Writer: removes `socket`'s node from the bucket of `hash` (readers already in it may still see it until they leave). False if not there.
  bool remove(uint32_t hash, const Socket* socket) {
    if (!buckets_) return false;
    Node** link = &buckets_->heads[hash & buckets_->mask];
    for (Node* n = *link; n; link = &n->next, n = *link) {
      if (n->socket.get() != socket) continue;
      kernel::rcu_assign_pointer(link, n->next);
      n->retire(FreeNode{}, domain_);
      const uint32_t held = __atomic_sub_fetch(&count_, 1, __ATOMIC_RELAXED);
      if (bucket_count() > kMinBuckets && held < bucket_count() / 4) resize(bucket_count() / 2);
      return true;
    }
    return false;
  }

  // Reader (any thread, no lock): the first socket in the bucket of `hash` that `match(key, socket)` accepts, or null.
  template <typename Match>
  SharedPointer<Socket> find(uint32_t hash, Match&& match) const {
    kernel::RcuReader reader(domain_);
    const Buckets* b = kernel::rcu_dereference(&buckets_);
    if (!b) return SharedPointer<Socket>();
    for (const Node* n = kernel::rcu_dereference(&b->heads[hash & b->mask]); n; n = kernel::rcu_dereference(&n->next))
      if (match(n->key, *n->socket)) return n->socket;
    return SharedPointer<Socket>();
  }

  // Reader: calls `visit(key, socket)` for every entry of the bucket of `hash` until it returns true; true if one did. (The socket only for a
  // look at what may be read without its lock: the section ends with the call.)
  template <typename Visit>
  bool any(uint32_t hash, Visit&& visit) const {
    kernel::RcuReader reader(domain_);
    const Buckets* b = kernel::rcu_dereference(&buckets_);
    if (!b) return false;
    for (const Node* n = kernel::rcu_dereference(&b->heads[hash & b->mask]); n; n = kernel::rcu_dereference(&n->next))
      if (visit(n->key, *n->socket)) return true;
    return false;
  }

  uint32_t count() const { return __atomic_load_n(&count_, __ATOMIC_RELAXED); }   // (changed by writers, read by anyone)
  // The current number of buckets (0 before the first insert; writers' view, for tests).
  uint32_t bucket_count() const { return buckets_ ? buckets_->mask + 1 : 0; }

 private:
  struct Node;
  struct Buckets;
  struct FreeNode {
    void operator()(Node* node) const {
      allocators::IAllocator* allocator = node->allocator;
      node->~Node();
      allocator->deallocate(node);
    }
  };
  struct FreeBuckets {
    void operator()(Buckets* buckets) const {
      allocators::IAllocator* allocator = buckets->allocator;
      release_view(*allocator, buckets->heads);
      buckets->~Buckets();
      allocator->deallocate(buckets);
    }
  };
  struct Node : kernel::RcuObjectBase<Node, FreeNode> {
    uint32_t hash = 0;
    SocketKey key;
    SharedPointer<Socket> socket;
    Node* next = nullptr;
    allocators::IAllocator* allocator = nullptr;
  };
  struct Buckets : kernel::RcuObjectBase<Buckets, FreeBuckets> {
    ArrayView<Node*> heads;
    uint32_t mask = 0;
    allocators::IAllocator* allocator = nullptr;
  };

  // (placement new into allocator memory: RCU ends these objects' lives, not an owner - see FreeNode and FreeBuckets)
  Node* make_node(uint32_t hash, const SocketKey& key, const SharedPointer<Socket>& socket) {
    void* memory = allocator_.allocate(sizeof(Node));
    if (!memory) return nullptr;
    Node* node = new (memory) Node();
    node->hash = hash;
    node->key = key;
    node->socket = socket;
    node->allocator = &allocator_;
    return node;
  }
  Buckets* make_buckets(uint32_t count) {
    ArrayView<Node*> heads = allocate_view<Node*>(allocator_, count);
    if (heads.empty()) return nullptr;
    for (Node*& head : heads) head = nullptr;
    void* memory = allocator_.allocate(sizeof(Buckets));
    if (!memory) {
      release_view(allocator_, heads);
      return nullptr;
    }
    Buckets* buckets = new (memory) Buckets();
    buckets->heads = heads;
    buckets->mask = count - 1;
    buckets->allocator = &allocator_;
    return buckets;
  }

  // (writer) Moves every entry into a new array of `count` buckets; nothing changes if memory runs out.
  void resize(uint32_t count) {
    Buckets* old = buckets_;
    Buckets* fresh = make_buckets(count);
    if (!fresh) return;
    bool complete = true;
    for (Node* head : old->heads) {
      for (Node* n = head; n && complete; n = n->next) {
        Node* copy = make_node(n->hash, n->key, n->socket);
        if (!copy) {
          complete = false;
          break;
        }
        Node*& to = fresh->heads[n->hash & fresh->mask];
        copy->next = to;
        to = copy;   // (not published yet: the array goes out whole below)
      }
      if (!complete) break;
    }
    if (!complete) {   // never published, so nobody can be reading it: freed at once
      for (Node* head : fresh->heads)
        while (Node* n = head) {
          head = n->next;
          FreeNode{}(n);
        }
      FreeBuckets{}(fresh);
      return;
    }
    kernel::rcu_assign_pointer(&buckets_, fresh);
    // (the old array is left as it is - readers may be walking it - and freed with its nodes after the grace period)
    for (Node* head : old->heads)
      for (Node* n = head; n;) {
        Node* next = n->next;
        n->retire(FreeNode{}, domain_);
        n = next;
      }
    old->retire(FreeBuckets{}, domain_);
  }

  allocators::IAllocator& allocator_;
  mutable kernel::RcuDomain domain_;
  Buckets* buckets_ = nullptr;
  uint32_t count_ = 0;
};

}  // namespace network
