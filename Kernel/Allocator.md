# Allocator
The allocator is really two allocators. The lowest level is a memory pool allocator wich the higher level allocator being an object allocator.

## Memory Pool Allocator
The memory pool allocator allocates memory in blocks which are multiples of the size of a cache line on the target platform. For example this may be 128 bytes. This allocator may be used directly if needed however the intention is for other componeents to use the object allocator. The memory pool does not initialize or otherwise alter the contents of memory when allocating. 

## Object Pool Allocator
The object pool allocator allocates memory for objects from the memory pool allocator. When a new object is requested a pool should be created for that object with enough space for multiple objects. It is recommended to allocate space for 6,144 objects of the new type. This value may change based on testing and platform specifics. The allocator can also be aware of specific types to allocate more or less objects based on measured real world usage. This allocator also needs to be aware of initialization requirements for each object type so that objects can be recycled. If an anonomous type is allocated (eg only sized information provded) then it is assumed that there are no initalization requirements and that memory can be provided un-altered. If there are security considerations then the client must handle these as the allocator is not aware of them due to not having the type information.
