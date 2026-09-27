#pragma once
#include <stddef.h>

// MurmurHash3_x86_32, Austin Appleby's public-domain reference.
unsigned int MurmurHash3_x86_32(const void* key, size_t len, unsigned int seed);

// Ogre::IdString of name: the hash the game's OgreMain computes for a
// compositor node or workspace name (MurmurHash3_x86_32 with Ogre's seed).
unsigned int IdStringHash(const char* name);
