#pragma once
#include <stdint.h>

// A position inside a container. It is plain data the caller owns, so walking a
// container never allocates; what the two fields mean is up to the container
// (an index, a node pointer, a bucket and a node ...). Treat it as opaque.
struct ObjectCursor {
  uint64_t index = 0;
  void* node = nullptr;
};

// The common face of every container of T: how many elements there are, how to
// empty it, and how to walk it without allocating.
//
//   for (T& item : container) { ... }                   // range-for
//
//   ObjectCursor cursor;                                // or by hand
//   for (bool more = container.first(&cursor); more; more = container.next(&cursor))
//     use(*container.element(cursor));
//
// Changing the container (insert, remove, clear) invalidates cursors and
// iterators, except where a container documents otherwise. Containers own their
// elements, so they cannot be copied; they are not deleted through this interface.
template <class T>
class IObjectContainer {
 public:
  // Number of elements.
  virtual uint64_t count() const = 0;
  // Destroys every element.
  virtual void clear() = 0;

  // Moves `cursor` to the first element; false (and no cursor) if there is none.
  virtual bool first(ObjectCursor* cursor) const = 0;
  // Moves `cursor` to the following element; false once the end is passed.
  virtual bool next(ObjectCursor* cursor) const = 0;
  // The element at a valid cursor.
  virtual T* element(const ObjectCursor& cursor) const = 0;

  bool empty() const { return count() == 0; }

  // Forward iterator over the elements (range-for). `Elem` is T or const T.
  template <class Elem>
  class BasicIterator {
   public:
    Elem& operator*() const { return *container_->element(cursor_); }
    Elem* operator->() const { return container_->element(cursor_); }
    BasicIterator& operator++() {
      if (!container_->next(&cursor_)) container_ = nullptr;  // reached the end
      return *this;
    }
    bool operator==(const BasicIterator& other) const {
      return container_ == other.container_ &&
             (container_ == nullptr || (cursor_.index == other.cursor_.index && cursor_.node == other.cursor_.node));
    }
    bool operator!=(const BasicIterator& other) const { return !(*this == other); }

   private:
    friend class IObjectContainer;
    BasicIterator() = default;  // the end iterator
    explicit BasicIterator(const IObjectContainer* container) : container_(container) {
      if (!container_->first(&cursor_)) container_ = nullptr;
    }

    const IObjectContainer* container_ = nullptr;  // null = end
    ObjectCursor cursor_;
  };

  using Iterator = BasicIterator<T>;
  using ConstIterator = BasicIterator<const T>;

  Iterator begin() { return Iterator(this); }
  Iterator end() { return Iterator(); }
  ConstIterator begin() const { return ConstIterator(this); }
  ConstIterator end() const { return ConstIterator(); }

 protected:
  constexpr IObjectContainer() = default;
  ~IObjectContainer() = default;
  IObjectContainer(const IObjectContainer&) = delete;
  IObjectContainer& operator=(const IObjectContainer&) = delete;
};
