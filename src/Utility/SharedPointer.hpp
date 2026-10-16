#pragma once
#include <new>
#include "Allocators/IAllocator.hpp"
#include "Utility/Move.hpp"
#include "Utility/UniquePointer.hpp"

namespace shared_detail {

// Common head of both kinds of control block. `release` destroys the object
// and frees the block once the strong count has reached zero.
struct Control {
  uint32_t strong = 1;
  void (*release)(Control*) = nullptr;
};

// Control block with the object stored inline (make / make_as).
template <class U>
struct InlineControl : Control {
  allocators::IAllocator& allocator;
  alignas(U) unsigned char storage[sizeof(U)];

  explicit InlineControl(allocators::IAllocator& a) : allocator(a) { this->release = &release_inline; }
  static void release_inline(Control* c) {
    auto* self = static_cast<InlineControl*>(c);
    allocators::IAllocator& a = self->allocator;
    reinterpret_cast<U*>(self->storage)->~U();
    self->~InlineControl();
    a.deallocate(self);
  }
};

// Control block around an object some UniquePointer allocated (from()).
struct AdoptedControl : Control {
  allocators::IAllocator& allocator;
  void* object;
  void (*destroy)(allocators::IAllocator*, void*);

  AdoptedControl(allocators::IAllocator& a, void* o, void (*d)(allocators::IAllocator*, void*))
      : allocator(a), object(o), destroy(d) {
    this->release = &release_adopted;
  }
  static void release_adopted(Control* c) {
    auto* self = static_cast<AdoptedControl*>(c);
    allocators::IAllocator& a = self->allocator;
    self->destroy(&a, self->object);
    self->~AdoptedControl();
    a.deallocate(self);
  }
};

}  // namespace shared_detail

// Shared ownership of one object allocated from an IAllocator: the object is
// destroyed and its memory freed when the last SharedPointer to it goes away.
// The reference count is atomic, so copies may be made and dropped on different
// cores at the same time (the pointed-to object itself is not synchronized).
// As with UniquePointer, a pointer to a base class safely shares a derived
// object without a virtual destructor.
//
// Not provided: weak pointers. Cycles of SharedPointers are never freed.
template <class T>
class SharedPointer {
 public:
  constexpr SharedPointer() = default;
  constexpr SharedPointer(decltype(nullptr)) {}
  ~SharedPointer() { reset(); }

  SharedPointer(const SharedPointer& other) : ptr_(other.ptr_), control_(other.control_) { retain(); }
  SharedPointer(SharedPointer&& other) : ptr_(other.ptr_), control_(other.control_) {
    other.ptr_ = nullptr;
    other.control_ = nullptr;
  }
  SharedPointer& operator=(const SharedPointer& other) {
    SharedPointer copy(other);  // retains before releasing, so self-assignment is fine
    swap(copy);
    return *this;
  }
  SharedPointer& operator=(SharedPointer&& other) {
    SharedPointer moved(util::move(other));
    swap(moved);
    return *this;
  }

  // From a pointer to a derived class.
  template <class U>
    requires requires(U* u) { [](T*) {}(u); }
  SharedPointer(const SharedPointer<U>& other) : ptr_(other.ptr_), control_(other.control_) {
    retain();
  }
  template <class U>
    requires requires(U* u) { [](T*) {}(u); }
  SharedPointer(SharedPointer<U>&& other) : ptr_(other.ptr_), control_(other.control_) {
    other.ptr_ = nullptr;
    other.control_ = nullptr;
  }

  // Constructs a T from `args`; the object and the reference count share one
  // allocation. Null if the allocator ran out.
  template <class... Args>
  static SharedPointer make(allocators::IAllocator& allocator, Args&&... args) {
    return make_as<T>(allocator, util::forward<Args>(args)...);
  }

  // Constructs a U (T or a class derived from it) and shares it as a T.
  template <class U, class... Args>
  static SharedPointer make_as(allocators::IAllocator& allocator, Args&&... args) {
    using Block = shared_detail::InlineControl<U>;
    static_assert(alignof(Block) <= 64, "type is over-aligned for the allocator");
    void* memory = allocator.allocate(sizeof(Block));
    if (!memory) return SharedPointer();
    Block* block = new (memory) Block(allocator);
    U* object = new (block->storage) U(util::forward<Args>(args)...);
    return SharedPointer(object, block);
  }

  // Takes shared ownership of what a UniquePointer owns. Needs a small
  // allocation of its own; if that fails the result is null and `owner` is left
  // untouched, still owning the object.
  template <class U>
    requires requires(U* u) { [](T*) {}(u); }
  static SharedPointer from(UniquePointer<U>&& owner) {
    if (!owner) return SharedPointer();
    void* memory = owner.allocator_->allocate(sizeof(shared_detail::AdoptedControl));
    if (!memory) return SharedPointer();
    auto* block = new (memory) shared_detail::AdoptedControl(*owner.allocator_, owner.object_, owner.destroy_);
    T* object = owner.ptr_;
    owner.ptr_ = nullptr;  // ownership has moved into the control block
    owner.object_ = nullptr;
    owner.destroy_ = nullptr;
    owner.allocator_ = nullptr;
    return SharedPointer(object, block);
  }

  void reset() {
    if (control_ && __atomic_fetch_sub(&control_->strong, 1, __ATOMIC_ACQ_REL) == 1) control_->release(control_);
    ptr_ = nullptr;
    control_ = nullptr;
  }

  void swap(SharedPointer& other) {
    T* p = ptr_;
    ptr_ = other.ptr_;
    other.ptr_ = p;
    shared_detail::Control* c = control_;
    control_ = other.control_;
    other.control_ = c;
  }

  T* get() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  // How many SharedPointers share the object (0 when null).
  uint32_t use_count() const {
    return control_ ? __atomic_load_n(&control_->strong, __ATOMIC_ACQUIRE) : 0;
  }
  // True when both share the same object (or are both null).
  template <class U>
  bool operator==(const SharedPointer<U>& other) const { return ptr_ == other.ptr_; }

 private:
  template <class U>
  friend class SharedPointer;

  SharedPointer(T* object, shared_detail::Control* control) : ptr_(object), control_(control) {}

  void retain() {
    if (control_) __atomic_fetch_add(&control_->strong, 1, __ATOMIC_RELAXED);
  }

  T* ptr_ = nullptr;
  shared_detail::Control* control_ = nullptr;
};
