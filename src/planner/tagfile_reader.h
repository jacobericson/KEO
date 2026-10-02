// tagfile_reader.h - Reader for Havok binary tagfiles (tagfile versions 3-5), the format of the
// game's navmesh tile files. Pure: no Windows, KenshiLib or game header. Any thread; allocates
// through the CRT only, and nothing is shared between calls.
#ifndef KEO_PLANNER_TAGFILE_READER_H
#define KEO_PLANNER_TAGFILE_READER_H

#include <stddef.h>
#include <string>
#include <vector>

namespace planner {

const int    TAGFILE_READER_VERSION = 1;          // any change to what TfParse accepts or yields bumps it
const size_t TF_MAX_FILE_BYTES      = 4u << 20;   // a larger input is refused

enum TfKind { TFK_NONE = 0, TFK_INT, TFK_REAL, TFK_VEC, TFK_BYTES, TFK_STRING, TFK_OBJECT, TFK_STRUCT, TFK_ARRAY };

// One value. The meaning of count and first depends on kind:
//   VEC     count = components (1-16), first = index of the first float in reals
//   BYTES   count = bytes, first = index into bytes
//   STRING  first = index into strings
//   OBJECT  first = object id, 0 for a null reference
//   STRUCT  count = fields present, first = index of the first TfField; type = type-table index
//   ARRAY   count = elements, first = index of element 0 in values (elements are contiguous)
struct TfValue
{
	int     kind;
	int     count;
	int     first;
	int     type;
	__int64 i;       // INT
	float   r;       // REAL
};

struct TfField  { int name; int value; };   // name: into strings; value: into values
struct TfMember { int name; int type; int tupleSize; int className; };   // className: into strings, -1 none
struct TfType   { int name; int parent; int firstMember; int memberCount; };

struct TfDoc
{
	std::vector<TfValue>       values;
	std::vector<TfField>       fields;
	std::vector<float>         reals;
	std::vector<unsigned char> bytes;
	std::vector<std::string>   strings;   // the file's string pool, in pool order
	std::vector<TfType>        types;     // [0] is the builtin void type
	std::vector<TfMember>      members;
	std::vector<int>           objects;   // object id -> STRUCT value; -1 while referenced but not complete
	int         version;                  // FILE_INFO version: 3, 4 or 5
	std::string sdk;                      // a version 4 or 5 file's SDK string
	size_t      stopOffset;               // body offset of a tolerant stop; 0 when FILE_END was read
	std::string stopReason;               // empty when FILE_END was read
};

enum TfResult
{
	TF_OK = 0,        // parsed to FILE_END
	TF_TAIL_STOP,     // a format error after object 1 completed: completed objects kept
	TF_BAD_MAGIC,
	TF_BAD_VERSION,
	TF_NO_ROOT,       // a format error before object 1 completed
	TF_TOO_LARGE,
	TF_EMPTY
};

// Parses data[0..size). Every result other than TF_OK and TF_TAIL_STOP leaves *out holding
// stopReason alone.
TfResult TfParse(const unsigned char* data, size_t size, TfDoc* out);

// Lookups; each answers -1 (or false, or "") when absent or of another kind.
int         TfRoot(const TfDoc& d);                                  // object 1's STRUCT, if complete
int         TfFind(const TfDoc& d, int structValue, const char* name); // a present field's value
int         TfDeref(const TfDoc& d, int value);                      // an OBJECT's STRUCT, if complete
const char* TfClassName(const TfDoc& d, int structValue);            // the most-derived type's name
int         TfArrayCount(const TfDoc& d, int value);                 // ARRAY elements, else -1
int         TfArrayAt(const TfDoc& d, int value, int k);             // element k's value index
bool        TfAsInt(const TfDoc& d, int value, __int64* out);        // INT
bool        TfAsReal(const TfDoc& d, int value, float* out);         // REAL, or an INT converted
int         TfAsVec(const TfDoc& d, int value, float* out, int max); // VEC components copied, else -1
const char* TfAsString(const TfDoc& d, int value);                   // STRING

} // namespace planner

#endif
