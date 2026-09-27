#ifndef KENSHI_ZONE_OPT_FIXES_NAVMESH_LIFE_H
#define KENSHI_ZONE_OPT_FIXES_NAVMESH_LIFE_H

// Section lifecycle rows for the streaming collection.
//
// A navmesh instance borrows six data pointers from the mesh it instances and
// holds one reference to that mesh. A recorded fault found all six pointing
// into an allocator free list while the instance was alive and registered,
// which has three possible causes and they need different fixes: the mesh was
// built by this mod's cache, the mesh's own reference count reached zero under
// a live instance, or the blocks were released by something that never owned
// them. The three are told apart by two numbers that are cheap to read while
// the objects are alive -- the mesh's face array capacity against its count,
// and the mesh's reference count at registration and again at retire.
//
// Both ends only read, and nothing here changes what the collection does.
// Every read of the mesh is made under structured exception handling, because
// an unreadable mesh at retire is itself one of the answers.

// Emitted from the streaming-collection insert hook, after the original has
// filled in the instance's slot index.
void NavMeshLifeOnAdd(void* collection, __int64 instance);

// The retire half: its own detour, since the removal has no other hook.
void InstallNavMeshLife(int* installed, int*);

#endif // KENSHI_ZONE_OPT_FIXES_NAVMESH_LIFE_H
