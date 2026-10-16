#pragma once
#include "Allocators/Allocator.hpp"

// Builds a List, a LinkedList and a Dictionary on the given allocator, walks
// them with range-for, and checks the results and that the allocator ends up
// with exactly the memory it started with. Logs one line per container.
bool run_container_demo(allocators::Allocator& allocator);
