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

#include "ContainerDemo.hpp"
#include "ObjectContainer/Array.hpp"
#include "ObjectContainer/BinaryTree.hpp"
#include "ObjectContainer/Dictionary.hpp"
#include "ObjectContainer/LinkedList.hpp"
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/PriorityQueue.hpp"
#include "ObjectContainer/Queue.hpp"
#include "ObjectContainer/SortedDictionary.hpp"
#include "ObjectContainer/Stack.hpp"
#include "console.hpp"

namespace {
void report(const char* what, bool ok) {
  console::puts("  ");
  console::puts(what);
  console::puts(ok ? ": ok\n" : ": FAILED\n");
}
}  // namespace

bool run_container_demo(allocators::Allocator& allocator) {
  const uint64_t free_before = allocator.free_bytes();
  bool all_ok = true;

  {
    List<int> list(allocator);
    bool ok = true;
    for (int i = 1; i <= 100; ++i) ok &= list.push_back(i * i);
    ok &= list.insert(0, -1) && list.remove_at(50);
    int64_t sum = 0;
    for (int value : list) sum += value;  // range-for: no allocation
    // 1 + 4 + ... + 100^2 = 338350, minus 50^2 (the 51st entry, index 50 after the insert), plus -1
    ok &= list.count() == 100 && sum == 338350 - 2500 - 1;
    report("list (100 squares, insert, remove)", ok);
    all_ok &= ok;
  }
  {
    Hive<int> hive(allocator);
    bool ok = true;
    int* first = hive.insert(1);
    ok &= first != nullptr;
    int* doomed[20] = {};
    for (int i = 0; i < 20; ++i) doomed[i] = hive.insert(i + 2);
    for (int* p : doomed) ok &= p && hive.erase(p);
    for (int i = 0; i < 2000; ++i) ok &= hive.insert(i) != nullptr;
    int64_t sum = 0;
    for (int value : hive) sum += value;  // iteration skips the holes
    ok &= first && *first == 1 && hive.count() == 2001 && sum == 1 + 1999 * 2000 / 2;  // the first element never moved
    report("hive (stable pointers, erase, hole reuse)", ok);
    all_ok &= ok;
  }
  {
    Array<int, 8> array;  // no allocator: the elements live inside it
    bool ok = array.count() == 8 && array[7] == 0;
    array.fill(3);
    array[2] = 10;
    int64_t sum = 0;
    for (int value : array) sum += value;
    ok &= sum == 7 * 3 + 10 && array.view().size() == 8 && array.get(8) == nullptr;
    array.clear();
    ok &= array.count() == 8 && array.back() == 0;
    report("fixed array (fill, index, iterate, clear)", ok);
    all_ok &= ok;
  }
  {
    LinkedList<int> list(allocator);
    bool ok = true;
    for (int i = 0; i < 10; ++i) ok &= list.push_back(i);
    ObjectCursor cursor;  // drop the odd numbers while walking
    for (bool more = list.first(&cursor); more;)
      more = (*list.element(cursor) % 2 == 0) ? list.next(&cursor) : list.erase(&cursor);
    int expected = 0;
    for (int value : list) {
      ok &= value == expected;
      expected += 2;
    }
    ok &= list.count() == 5 && expected == 10;
    report("linked list (erase while walking)", ok);
    all_ok &= ok;
  }
  {
    Dictionary<const char*, int> cores(allocator);  // keys compared by text
    static const char* const kNames[] = {"boot", "worker1", "worker2", "worker3", "worker4"};
    bool ok = true;
    for (int i = 0; i < 5; ++i) ok &= cores.put(kNames[i], i * 10);
    ok &= cores.put("worker3", 33);  // replaces the earlier value
    ok &= cores.count() == 5 && cores.find("worker3") && *cores.find("worker3") == 33;
    ok &= cores.remove("boot") && !cores.contains("boot") && cores.count() == 4;
    int total = 0;
    for (auto& entry : cores) total += entry.value;
    ok &= total == 10 + 20 + 33 + 40;

    Dictionary<uint32_t, uint32_t> squares(allocator);  // enough entries to grow the table
    for (uint32_t i = 0; i < 500; ++i) ok &= squares.put(i, i * i);
    ok &= squares.count() == 500 && squares.find(499) && *squares.find(499) == 499 * 499;
    uint64_t visited = 0;
    for (auto& entry : squares) visited += entry.value == entry.key * entry.key;
    ok &= visited == 500;
    report("dictionary (string keys, 500 integer keys)", ok);
    all_ok &= ok;
  }

  {
    BinaryTree<int> tree(allocator);
    bool ok = true;
    for (int i = 0; i < 1000; ++i) ok &= tree.insert(i) == BinaryTree<int>::InsertResult::Inserted;  // sorted input
    ok &= tree.insert(500) == BinaryTree<int>::InsertResult::Duplicate;
    ok &= tree.height() <= 14;  // balanced: a plain tree would be 1000 deep
    for (int i = 0; i < 1000; i += 3) ok &= tree.remove(i);
    int previous = -1;
    uint64_t seen = 0;
    for (int value : tree) {  // in-order walk, smallest first
      ok &= value > previous && value % 3 != 0;
      previous = value;
      ++seen;
    }
    ok &= seen == 666 && tree.count() == 666 && tree.min() && *tree.min() == 1 && *tree.max() == 998;
    report("binary tree (1000 sorted inserts, removals, in-order walk)", ok);
    all_ok &= ok;
  }
  {
    SortedDictionary<const char*, int> ranks(allocator);
    bool ok = ranks.put("scheduler", 3) && ranks.put("allocator", 1) && ranks.put("console", 2) &&
              ranks.put("usb", 5) && ranks.put("input", 4) && ranks.put("allocator", 10);
    static const char* const kExpected[] = {"allocator", "console", "input", "scheduler", "usb"};
    uint64_t index = 0;
    for (auto& entry : ranks) {  // ascending by key
      ok &= index < 5 && entry.key[0] == kExpected[index][0] && entry.key[1] == kExpected[index][1];
      ++index;
    }
    ok &= index == 5 && ranks.count() == 5 && *ranks.find("allocator") == 10;
    ok &= ranks.lower_bound("d") && ranks.lower_bound("d")->value == 4;  // first key >= "d" is "input"
    ok &= ranks.remove("console") && !ranks.contains("console");
    report("sorted dictionary (string keys in order, lower bound)", ok);
    all_ok &= ok;
  }
  {
    Stack<int> stack(allocator);
    Queue<int> queue(allocator);
    PriorityQueue<int> queue_by_priority(allocator);
    bool ok = true;
    for (int i = 1; i <= 5; ++i) {
      ok &= stack.push(i) && queue.enqueue(i);
      ok &= queue_by_priority.push((i * 7) % 5);  // 2 4 1 3 0
    }
    int popped = 0, dequeued = 0, best = 0;
    ok &= stack.pop(&popped) && popped == 5 && *stack.top() == 4;       // last in, first out
    ok &= queue.dequeue(&dequeued) && dequeued == 1 && *queue.front() == 2;  // first in, first out
    ok &= queue_by_priority.pop(&best) && best == 4 && *queue_by_priority.top() == 3;  // highest priority first
    int order = 0;
    for (int value : stack) order = order * 10 + value;  // top to bottom: 4321
    ok &= order == 4321;
    report("stack, queue and priority queue", ok);
    all_ok &= ok;
  }

  const bool freed = allocator.free_bytes() == free_before;
  report("no memory left allocated", freed);
  return all_ok && freed;
}
