#pragma once
#include "ObjectContainer/ContainerSupport.hpp"
#include "ObjectContainer/IObjectContainer.hpp"

// A doubly linked list: every element has its own allocation, so inserting or
// removing at either end (or at a cursor) is O(1) and elements never move -
// pointers to an element stay valid until that element is removed. No random
// access. Every allocation goes through the IAllocator given.
template <class T>
class LinkedList final : public IObjectContainer<T> {
 public:
  explicit LinkedList(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~LinkedList() { clear(); }

  uint64_t count() const override { return size_; }

  void clear() override {
    while (head_) unlink(head_);
  }

  bool push_back(const T& value) { return link(tail_, nullptr, value); }
  bool push_front(const T& value) { return link(nullptr, head_, value); }

  bool pop_front() {
    if (!head_) return false;
    unlink(head_);
    return true;
  }
  bool pop_back() {
    if (!tail_) return false;
    unlink(tail_);
    return true;
  }

  // First / last element, or nullptr when empty.
  T* front() { return head_ ? &head_->value : nullptr; }
  T* back() { return tail_ ? &tail_->value : nullptr; }
  const T* front() const { return head_ ? &head_->value : nullptr; }
  const T* back() const { return tail_ ? &tail_->value : nullptr; }

  // Inserts a new element just before / after the one at `cursor`.
  bool insert_before(const ObjectCursor& cursor, const T& value) {
    Node* at = node_of(cursor);
    return at && link(at->previous, at, value);
  }
  bool insert_after(const ObjectCursor& cursor, const T& value) {
    Node* at = node_of(cursor);
    return at && link(at, at->next, value);
  }

  // Removes the element at `*cursor` and moves the cursor to the element that
  // followed it. Returns true if there is one; false when the removed element was
  // the last (the cursor is then not valid). Safe to use while walking:
  //   for (bool more = list.first(&c); more;)
  //     more = wanted(*list.element(c)) ? list.next(&c) : list.erase(&c);
  bool erase(ObjectCursor* cursor) {
    Node* at = node_of(*cursor);
    if (!at) return false;
    Node* following = at->next;
    unlink(at);
    cursor->node = following;  // index keeps counting positions along the walk
    return following != nullptr;
  }

  // --- IObjectContainer iteration: the cursor's `node` is the element's node.
  bool first(ObjectCursor* cursor) const override {
    if (!head_) return false;
    cursor->index = 0;
    cursor->node = head_;
    return true;
  }
  bool next(ObjectCursor* cursor) const override {
    Node* following = static_cast<Node*>(cursor->node)->next;
    if (!following) return false;
    ++cursor->index;
    cursor->node = following;
    return true;
  }
  T* element(const ObjectCursor& cursor) const override { return &static_cast<Node*>(cursor.node)->value; }

 private:
  struct Node {
    Node* previous;
    Node* next;
    T value;
  };

  static Node* node_of(const ObjectCursor& cursor) { return static_cast<Node*>(cursor.node); }

  // Allocates a node holding `value` and puts it between `before` and `after`.
  bool link(Node* before, Node* after, const T& value) {
    static_assert(container_support::kFitsAllocatorAlignment<Node>, "type is over-aligned for the allocator");
    void* memory = allocator_.allocate(sizeof(Node));
    if (!memory) return false;
    Node* node = new (memory) Node{before, after, value};
    (before ? before->next : head_) = node;
    (after ? after->previous : tail_) = node;
    ++size_;
    return true;
  }

  void unlink(Node* node) {
    (node->previous ? node->previous->next : head_) = node->next;
    (node->next ? node->next->previous : tail_) = node->previous;
    node->~Node();
    allocator_.deallocate(node);
    --size_;
  }

  allocators::IAllocator& allocator_;
  Node* head_ = nullptr;
  Node* tail_ = nullptr;
  uint64_t size_ = 0;
};
