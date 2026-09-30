// tagfile_reader.cpp - Havok binary tagfile parse into TfDoc's flat value tree: the tag loop, the
// string pool, the type table, member bitmaps and the columnar struct-array layout. Pure and
// reentrant: every call works on its own cursor and document; no lock, no shared state.
#include "planner/tagfile_reader.h"

#include <stdio.h>
#include <string.h>
#include <new>

namespace planner {

namespace tagfile_reader_detail {

enum TfTag { TAG_FILE_INFO = 1, TAG_METADATA = 2, TAG_OBJECT_REMEMBER = 4, TAG_FILE_END = 7 };

enum TfBasic
{
	T_VOID = 0, T_BYTE, T_INT, T_REAL, T_VEC4, T_VEC8, T_VEC12, T_VEC16, T_OBJECT, T_STRUCT, T_CSTRING,
	BASIC_MASK = 15, ARRAY_FLAG = 16, TUPLE_FLAG = 32
};

// One parse's position and failure state. The first failure wins: every reader returns false
// once failed is set, and the caller unwinds without reading further.
struct Cursor
{
	const unsigned char* body;   // the bytes after the 8-byte magic
	size_t size;
	size_t pos;
	TfDoc* doc;
	size_t poolBase;             // strings index of the current pool's entry 0
	int    nextObject;
	int    depth;
	size_t nodeCap;              // bound on values and fields together
	bool   failed;
	bool   badVersion;
	size_t failPos;
	char   reason[192];
};

} // namespace tagfile_reader_detail
using namespace tagfile_reader_detail;

static const int    MAX_DEPTH        = 64;   // struct nesting, and a class's parent chain
static const int    VARINT_MAX_BYTES = 10;
static const size_t NODES_PER_BYTE   = 4;    // values plus fields a byte may yield before the cap

static bool Fail(Cursor& c, const char* what)
{
	if (!c.failed)
	{
		c.failed = true;
		c.failPos = c.pos;
		_snprintf_s(c.reason, sizeof(c.reason), _TRUNCATE, "%s at body offset %u", what, (unsigned)c.pos);
	}
	return false;
}

static bool ReadBytes(Cursor& c, size_t n, const unsigned char** out)
{
	if (n > c.size - c.pos)
		return Fail(c, "read past the end");
	*out = c.body + c.pos;
	c.pos += n;
	return true;
}

static bool ReadByte(Cursor& c, unsigned char* out)
{
	if (c.pos >= c.size)
		return Fail(c, "read past the end");
	*out = c.body[c.pos++];
	return true;
}

// Bit 0 of the first byte is the sign, bits 1-6 the low six value bits; bit 7 of every byte
// continues, and each further byte adds seven bits from bit 6 up.
static bool ReadVarInt(Cursor& c, __int64* out)
{
	unsigned char b = 0;
	if (!ReadByte(c, &b))
		return false;
	bool negative = (b & 1) != 0;
	unsigned __int64 value = (unsigned __int64)((b & 0x7E) >> 1);
	int shift = 6;
	int n = 1;
	while (b & 0x80)
	{
		if (n == VARINT_MAX_BYTES)
			return Fail(c, "VarInt longer than 10 bytes");
		if (!ReadByte(c, &b))
			return false;
		++n;
		unsigned __int64 bits = (unsigned __int64)(b & 0x7F);
		if (bits != 0)
		{
			if (shift >= 63 || (bits << shift) >> shift != bits)
				return Fail(c, "VarInt out of range");
			value |= bits << shift;
		}
		shift += 7;
	}
	if (value > 0x7FFFFFFFFFFFFFFFULL)
		return Fail(c, "VarInt out of range");
	*out = negative ? -(__int64)value : (__int64)value;
	return true;
}

static bool ReadVarIntIn(Cursor& c, __int64 lo, __int64 hi, int* out, const char* what)
{
	__int64 v = 0;
	if (!ReadVarInt(c, &v))
		return false;
	if (v < lo || v > hi)
		return Fail(c, what);
	*out = (int)v;
	return true;
}

static bool ReadF32(Cursor& c, float* out)
{
	const unsigned char* p = 0;
	if (!ReadBytes(c, 4, &p))
		return false;
	memcpy(out, p, 4);
	return true;
}

static int Remaining(const Cursor& c)
{
	size_t left = c.size - c.pos;
	return left > 0x7FFFFFFF ? 0x7FFFFFFF : (int)left;
}

// A length <= 0 names pool entry -length; a positive length is a new string appended to the pool.
static bool ReadString(Cursor& c, int* out)
{
	__int64 length = 0;
	if (!ReadVarInt(c, &length))
		return false;
	TfDoc& d = *c.doc;
	if (length <= 0)
	{
		size_t poolSize = d.strings.size() - c.poolBase;
		if ((unsigned __int64)(-length) >= poolSize)
			return Fail(c, "string pool back-reference out of range");
		*out = (int)(c.poolBase + (size_t)(-length));
		return true;
	}
	if ((unsigned __int64)length > (unsigned __int64)(c.size - c.pos))
		return Fail(c, "string past the end");
	const unsigned char* p = 0;
	ReadBytes(c, (size_t)length, &p);
	d.strings.push_back(std::string((const char*)p, (size_t)length));
	*out = (int)d.strings.size() - 1;
	return true;
}

static bool HasName(const TfDoc& d, int s)
{
	return s >= 0 && !d.strings[s].empty();
}

static bool ReadTypeInfo(Cursor& c)
{
	TfDoc& d = *c.doc;
	TfType t;
	__int64 unused = 0;
	if (!ReadString(c, &t.name) || !ReadVarInt(c, &unused))
		return false;
	if (!ReadVarIntIn(c, 0, (__int64)d.types.size() - 1, &t.parent, "parent type index out of range"))
		return false;
	if (!ReadVarIntIn(c, 0, Remaining(c), &t.memberCount, "member count out of range"))
		return false;
	t.firstMember = (int)d.members.size();
	for (int k = 0; k < t.memberCount; ++k)
	{
		TfMember m;
		m.tupleSize = 0;
		m.className = -1;
		if (!ReadString(c, &m.name))
			return false;
		if (!ReadVarIntIn(c, 0, 0x7FFFFFFFLL, &m.type, "member type out of range"))
			return false;
		if ((m.type & TUPLE_FLAG) && !ReadVarIntIn(c, 0, 0x7FFFFFFFLL, &m.tupleSize, "tuple size out of range"))
			return false;
		int basic = m.type & BASIC_MASK;
		if ((basic == T_OBJECT || basic == T_STRUCT) && !ReadString(c, &m.className))
			return false;
		d.members.push_back(m);
	}
	d.types.push_back(t);
	return true;
}

// The most recent type of that name; the builtin void type is never a match.
static bool LookupClass(Cursor& c, int nameIndex, int* out)
{
	const TfDoc& d = *c.doc;
	const std::string& name = d.strings[nameIndex];
	for (size_t t = d.types.size(); t-- > 1;)
	{
		if (d.strings[d.types[t].name] == name)
		{
			*out = (int)t;
			return true;
		}
	}
	return Fail(c, "unknown class name");
}

static bool UnderCap(Cursor& c, unsigned __int64 count)
{
	const TfDoc& d = *c.doc;
	if (count <= c.nodeCap && d.values.size() + d.fields.size() + count <= c.nodeCap)
		return true;
	return Fail(c, "value table over its cap");
}

static bool NewValues(Cursor& c, unsigned __int64 count, int* first)
{
	if (!UnderCap(c, count))
		return false;
	TfValue v;
	memset(&v, 0, sizeof(v));
	v.type = -1;
	*first = (int)c.doc->values.size();
	c.doc->values.resize(c.doc->values.size() + (size_t)count, v);
	return true;
}

static bool NewFields(Cursor& c, unsigned __int64 count, int* first)
{
	if (!UnderCap(c, count))
		return false;
	TfField f = { -1, -1 };
	*first = (int)c.doc->fields.size();
	c.doc->fields.resize(c.doc->fields.size() + (size_t)count, f);
	return true;
}

static void SetValue(TfDoc& d, int at, int kind, int count, int first, int type)
{
	TfValue& v = d.values[at];
	v.kind = kind;
	v.count = count;
	v.first = first;
	v.type = type;
}

static bool SetBytes(Cursor& c, const unsigned char* p, int n, int dst)
{
	TfDoc& d = *c.doc;
	int base = (int)d.bytes.size();
	d.bytes.insert(d.bytes.end(), p, p + n);
	SetValue(d, dst, TFK_BYTES, n, base, -1);
	return true;
}

static bool SetInt(TfDoc& d, int dst, __int64 v)
{
	SetValue(d, dst, TFK_INT, 0, 0, -1);
	d.values[dst].i = v;
	return true;
}

// A struct's layout: its member bitmap over the class chain, read into the members it marks
// present, the root-most class's members first.
static bool ReadLayout(Cursor& c, int cls, std::vector<int>* present)
{
	const TfDoc& d = *c.doc;
	int chain[MAX_DEPTH];
	int n = 0;
	for (int idx = cls; idx != 0; idx = d.types[idx].parent)
	{
		if (n == MAX_DEPTH)
			return Fail(c, "class chain deeper than 64");
		chain[n++] = idx;
	}
	int members = 0;
	for (int k = 0; k < n; ++k)
		members += d.types[chain[k]].memberCount;
	const unsigned char* bitmap = 0;
	if (!ReadBytes(c, (size_t)(members + 7) / 8, &bitmap))
		return false;
	int bit = 0;
	for (int k = n; k-- > 0;)
	{
		const TfType& t = d.types[chain[k]];
		for (int j = 0; j < t.memberCount; ++j, ++bit)
		{
			if ((bitmap[bit >> 3] >> (bit & 7)) & 1)
				present->push_back(t.firstMember + j);
		}
	}
	return true;
}

static bool ParseStruct(Cursor& c, int cls, int dst);
static bool ParseStructArray(Cursor& c, int member, int count, int base);
static bool ParseField(Cursor& c, int member, int dst);

// The prefix an array's values share: an INT width (read, unused) or a VEC4 component count.
static bool ArrayPrefix(Cursor& c, int type, __int64* prefix)
{
	int basic = type & BASIC_MASK;
	*prefix = -1;
	if (basic == T_INT || basic == T_VEC4)
		return ReadVarInt(c, prefix);
	return true;
}

static bool ReadReals(Cursor& c, int stored, int total, int dst)
{
	TfDoc& d = *c.doc;
	int base = (int)d.reals.size();
	for (int k = 0; k < total; ++k)
	{
		float f = 0.0f;
		if (k < stored && !ReadF32(c, &f))
			return false;
		d.reals.push_back(f);
	}
	SetValue(d, dst, TFK_VEC, total, base, -1);
	return true;
}

static bool ParseFieldValue(Cursor& c, int basic, int className, __int64 prefix, int dst)
{
	TfDoc& d = *c.doc;
	unsigned char b = 0;
	__int64 i = 0;
	float f = 0.0f;
	switch (basic)
	{
	case T_BYTE:
		return ReadByte(c, &b) && SetInt(d, dst, b);
	case T_INT:
		return ReadVarInt(c, &i) && SetInt(d, dst, i);
	case T_REAL:
		if (!ReadF32(c, &f))
			return false;
		SetValue(d, dst, TFK_REAL, 0, 0, -1);
		d.values[dst].r = f;
		return true;
	case T_VEC4:
	{
		__int64 n = prefix < 0 ? 4 : prefix;
		if (n < 1 || n > 4)
			return Fail(c, "unsupported vec4 length");
		return ReadReals(c, (int)n, 4, dst);
	}
	case T_VEC12:
		return ReadReals(c, 12, 12, dst);
	case T_VEC16:
		return ReadReals(c, 16, 16, dst);
	case T_OBJECT:
	{
		__int64 id = 0;
		if (!ReadVarInt(c, &id))
			return false;
		if (id < 0 || (unsigned __int64)id > (unsigned __int64)c.size)
			return Fail(c, "object id out of range");
		if (id > 0 && d.objects.size() <= (size_t)id)
			d.objects.resize((size_t)id + 1, -1);
		SetValue(d, dst, TFK_OBJECT, 0, (int)id, -1);
		return true;
	}
	case T_STRUCT:
	{
		int cls = 0;
		if (HasName(d, className) && !LookupClass(c, className, &cls))
			return false;
		return ParseStruct(c, cls, dst);
	}
	case T_CSTRING:
	{
		int s = 0;
		if (!ReadString(c, &s))
			return false;
		SetValue(d, dst, TFK_STRING, 0, s, -1);
		return true;
	}
	default:
		return Fail(c, "unsupported field type");
	}
}

// count values of one member, contiguous from base: a nested struct array, or count scalars
// sharing one prefix.
static bool ParseColumn(Cursor& c, int member, int count, int base)
{
	const TfMember m = c.doc->members[member];
	__int64 prefix = -1;
	if (!ArrayPrefix(c, m.type, &prefix))
		return false;
	int basic = m.type & BASIC_MASK;
	if (basic == T_STRUCT)
		return ParseStructArray(c, member, count, base);
	for (int k = 0; k < count; ++k)
	{
		if (!ParseFieldValue(c, basic, m.className, prefix, base + k))
			return false;
	}
	return true;
}

static bool ParseField(Cursor& c, int member, int dst)
{
	TfDoc& d = *c.doc;
	const TfMember m = d.members[member];
	const unsigned char* p = 0;
	if (m.type & ~(ARRAY_FLAG | TUPLE_FLAG | BASIC_MASK))
		return Fail(c, "unsupported flags in field type");
	if (m.type == (TUPLE_FLAG | T_BYTE))
		return ReadBytes(c, (size_t)m.tupleSize, &p) && SetBytes(c, p, m.tupleSize, dst);
	if (m.type == (ARRAY_FLAG | T_BYTE))
	{
		int n = 0;
		return ReadVarIntIn(c, 0, Remaining(c), &n, "byte array length out of range")
			&& ReadBytes(c, (size_t)n, &p) && SetBytes(c, p, n, dst);
	}
	if ((m.type & (ARRAY_FLAG | TUPLE_FLAG)) == 0)
		return ParseFieldValue(c, m.type & BASIC_MASK, m.className, -1, dst);
	if ((m.type & (ARRAY_FLAG | TUPLE_FLAG)) == (ARRAY_FLAG | TUPLE_FLAG))
		return Fail(c, "field is both array and tuple");
	int count = m.tupleSize;
	if ((m.type & ARRAY_FLAG) && !ReadVarIntIn(c, 0, 0x7FFFFFFFLL, &count, "array count out of range"))
		return false;
	int base = 0;
	if (!NewValues(c, (unsigned __int64)count, &base))
		return false;
	SetValue(d, dst, TFK_ARRAY, count, base, -1);
	return ParseColumn(c, member, count, base);
}

// A struct: an optional class index, the member bitmap over the class chain, then each present
// member in chain order, the root-most class's members first.
static bool ParseStruct(Cursor& c, int cls, int dst)
{
	TfDoc& d = *c.doc;
	if (c.depth == MAX_DEPTH)
		return Fail(c, "struct nesting deeper than 64");
	if (cls == 0 && !ReadVarIntIn(c, 1, (__int64)d.types.size() - 1, &cls, "struct class index out of range"))
		return false;
	std::vector<int> present;
	int fieldBase = 0;
	if (!ReadLayout(c, cls, &present) || !NewFields(c, present.size(), &fieldBase))
		return false;
	int np = (int)present.size();
	SetValue(d, dst, TFK_STRUCT, np, fieldBase, cls);
	++c.depth;
	for (int j = 0; j < np; ++j)
	{
		int v = 0;
		if (!NewValues(c, 1, &v))
			return false;
		d.fields[fieldBase + j].name = d.members[present[j]].name;
		d.fields[fieldBase + j].value = v;
		if (!ParseField(c, present[j], v))
			return false;
	}
	--c.depth;
	return true;
}

// One present member's column of a struct array, filling slot `slot` of every element's field
// block. An array member is read once per element, each with its own count; a tuple member is
// one column of count * tupleSize values; anything else is one column of count values.
static bool ParseArrayMember(Cursor& c, int member, int count, int fieldBase, int present, int slot)
{
	TfDoc& d = *c.doc;
	const TfMember m = d.members[member];
	for (int k = 0; k < count; ++k)
		d.fields[fieldBase + k * present + slot].name = m.name;
	int basic = m.type & BASIC_MASK;
	int base = 0;
	if (m.type & ARRAY_FLAG)
	{
		for (int k = 0; k < count; ++k)
		{
			int v = 0;
			if (!NewValues(c, 1, &v))
				return false;
			d.fields[fieldBase + k * present + slot].value = v;
			if (!ParseField(c, member, v))
				return false;
		}
		return true;
	}
	if (!NewValues(c, (unsigned __int64)count, &base))
		return false;
	for (int k = 0; k < count; ++k)
		d.fields[fieldBase + k * present + slot].value = base + k;
	if (!(m.type & TUPLE_FLAG))
		return ParseColumn(c, member, count, base);
	unsigned __int64 flatCount = (unsigned __int64)count * (unsigned __int64)m.tupleSize;
	if (basic == T_BYTE)
	{
		const unsigned char* p = 0;
		if (flatCount > (unsigned __int64)(c.size - c.pos))
			return Fail(c, "read past the end");
		ReadBytes(c, (size_t)flatCount, &p);
		for (int k = 0; k < count; ++k)
			SetBytes(c, p + (size_t)k * m.tupleSize, m.tupleSize, base + k);
		return true;
	}
	__int64 prefix = -1;
	int flat = 0;
	if (!ArrayPrefix(c, m.type, &prefix) || !NewValues(c, flatCount, &flat))
		return false;
	for (int k = 0; k < count; ++k)
		SetValue(d, base + k, TFK_ARRAY, m.tupleSize, flat + k * m.tupleSize, -1);
	for (int i = 0; i < (int)flatCount; ++i)
	{
		if (!ParseFieldValue(c, basic, m.className, prefix, flat + i))
			return false;
	}
	return true;
}

// count struct elements, contiguous from base, in the columnar layout: one member bitmap for
// every element, then one column per present member.
static bool ParseStructArray(Cursor& c, int member, int count, int base)
{
	TfDoc& d = *c.doc;
	const TfMember m = d.members[member];
	if (c.depth == MAX_DEPTH)
		return Fail(c, "struct nesting deeper than 64");
	int cls = 0;
	if (m.type == (ARRAY_FLAG | T_STRUCT) && !HasName(d, m.className))
	{
		if (!ReadVarIntIn(c, 1, (__int64)d.types.size() - 1, &cls, "struct array class index out of range"))
			return false;
	}
	else if (!HasName(d, m.className))
		return Fail(c, "class index unknown in struct array");
	else if (!LookupClass(c, m.className, &cls))
		return false;
	std::vector<int> present;
	int fieldBase = 0;
	if (!ReadLayout(c, cls, &present) || !NewFields(c, (unsigned __int64)count * present.size(), &fieldBase))
		return false;
	int np = (int)present.size();
	for (int k = 0; k < count; ++k)
		SetValue(d, base + k, TFK_STRUCT, np, fieldBase + k * np, cls);
	++c.depth;
	for (int j = 0; j < np; ++j)
	{
		if (!ParseArrayMember(c, present[j], count, fieldBase, np, j))
			return false;
	}
	--c.depth;
	return true;
}

static bool ParseFileInfo(Cursor& c)
{
	TfDoc& d = *c.doc;
	__int64 version = 0;
	if (!ReadVarInt(c, &version))
		return false;
	if (version < 3 || version > 5)
	{
		c.badVersion = true;
		return Fail(c, "unsupported tagfile version");
	}
	d.version = (int)version;
	c.poolBase = d.strings.size();
	d.strings.push_back(std::string());
	d.strings.push_back(std::string());
	if (version == 3)
		return true;
	int sdk = 0;
	const unsigned char* p = 0;
	if (!ReadString(c, &sdk))
		return false;
	d.sdk = d.strings[sdk];
	return version == 4 || ReadBytes(c, 6, &p);
}

static bool ParseObject(Cursor& c)
{
	TfDoc& d = *c.doc;
	int id = c.nextObject++;
	if (d.objects.size() <= (size_t)id)
		d.objects.resize((size_t)id + 1, -1);
	d.objects[id] = -1;
	int v = 0;
	if (!NewValues(c, 1, &v) || !ParseStruct(c, 0, v))
		return false;
	d.objects[id] = v;
	return true;
}

// The tag loop: FILE_INFO, METADATA and OBJECT_REMEMBER until FILE_END; any other tag is a
// format error.
static bool ParseBody(Cursor& c)
{
	for (;;)
	{
		__int64 tag = 0;
		if (!ReadVarInt(c, &tag))
			return false;
		bool ok = true;
		switch (tag)
		{
		case TAG_FILE_INFO:       ok = ParseFileInfo(c); break;
		case TAG_METADATA:        ok = ReadTypeInfo(c); break;
		case TAG_OBJECT_REMEMBER: ok = ParseObject(c); break;
		case TAG_FILE_END:        return true;
		default:                  return Fail(c, "unsupported top-level tag");
		}
		if (!ok)
			return false;
	}
}

static void ResetDoc(TfDoc* d)
{
	d->values.clear();
	d->fields.clear();
	d->reals.clear();
	d->bytes.clear();
	d->strings.clear();
	d->types.clear();
	d->members.clear();
	d->objects.clear();
	d->version = 0;
	d->sdk.clear();
	d->stopOffset = 0;
	d->stopReason.clear();
}

static TfResult Refuse(TfDoc* out, TfResult result, const char* reason)
{
	ResetDoc(out);
	out->stopReason = reason;
	return result;
}

static TfResult ParseChecked(const unsigned char* data, size_t size, TfDoc* out)
{
	static const unsigned char kMagic[8] = { 0x1E, 0x0D, 0xB0, 0xCA, 0xCE, 0xFA, 0x11, 0xD0 };
	if (size < 8 || memcmp(data, kMagic, 8) != 0)
		return Refuse(out, TF_BAD_MAGIC, "not a Havok tagfile (bad magic)");
	Cursor c;
	c.body = data + 8;
	c.size = size - 8;
	c.pos = 0;
	c.doc = out;
	c.poolBase = 0;
	c.nextObject = 1;
	c.depth = 0;
	c.nodeCap = NODES_PER_BYTE * size + 4096;
	c.failed = false;
	c.badVersion = false;
	c.failPos = 0;
	c.reason[0] = 0;
	TfType voidType;
	voidType.name = -1;
	voidType.parent = 0;
	voidType.firstMember = 0;
	voidType.memberCount = 0;
	out->types.push_back(voidType);
	if (ParseBody(c))
		return TF_OK;
	bool rootDone = out->objects.size() > 1 && out->objects[1] >= 0;
	if (!rootDone)
		return Refuse(out, c.badVersion ? TF_BAD_VERSION : TF_NO_ROOT, c.reason);
	out->stopOffset = c.failPos;
	out->stopReason = c.reason;
	return TF_TAIL_STOP;
}

TfResult TfParse(const unsigned char* data, size_t size, TfDoc* out)
{
	ResetDoc(out);
	if (data == 0 || size == 0)
		return Refuse(out, TF_EMPTY, "empty input");
	if (size > TF_MAX_FILE_BYTES)
		return Refuse(out, TF_TOO_LARGE, "input over the size cap");
	try
	{
		return ParseChecked(data, size, out);
	}
	catch (const std::bad_alloc&)
	{
		// A typed catch never takes a structured exception, whatever the /EH model, so a fault
		// still ends the process. The tables' storage is released before the reason is set.
		std::vector<TfValue>().swap(out->values);
		std::vector<TfField>().swap(out->fields);
		std::vector<float>().swap(out->reals);
		std::vector<unsigned char>().swap(out->bytes);
		std::vector<std::string>().swap(out->strings);
		std::vector<TfType>().swap(out->types);
		std::vector<TfMember>().swap(out->members);
		std::vector<int>().swap(out->objects);
		return Refuse(out, TF_NO_ROOT, "out of memory");
	}
}

static bool IsKind(const TfDoc& d, int value, int kind)
{
	return value >= 0 && (size_t)value < d.values.size() && d.values[value].kind == kind;
}

int TfRoot(const TfDoc& d)
{
	if (d.objects.size() < 2 || !IsKind(d, d.objects[1], TFK_STRUCT))
		return -1;
	return d.objects[1];
}

// Scanned from the last field, so a name a derived class repeats answers the derived member.
int TfFind(const TfDoc& d, int structValue, const char* name)
{
	if (!IsKind(d, structValue, TFK_STRUCT) || name == 0)
		return -1;
	const TfValue& s = d.values[structValue];
	for (int k = s.count; k-- > 0;)
	{
		const TfField& f = d.fields[s.first + k];
		if (f.name >= 0 && d.strings[f.name] == name)
			return f.value;
	}
	return -1;
}

int TfDeref(const TfDoc& d, int value)
{
	if (!IsKind(d, value, TFK_OBJECT))
		return -1;
	int id = d.values[value].first;
	if (id <= 0 || (size_t)id >= d.objects.size() || !IsKind(d, d.objects[id], TFK_STRUCT))
		return -1;
	return d.objects[id];
}

const char* TfClassName(const TfDoc& d, int structValue)
{
	if (!IsKind(d, structValue, TFK_STRUCT))
		return "";
	int type = d.values[structValue].type;
	if (type <= 0 || (size_t)type >= d.types.size() || d.types[type].name < 0)
		return "";
	return d.strings[d.types[type].name].c_str();
}

int TfArrayCount(const TfDoc& d, int value)
{
	return IsKind(d, value, TFK_ARRAY) ? d.values[value].count : -1;
}

int TfArrayAt(const TfDoc& d, int value, int k)
{
	if (!IsKind(d, value, TFK_ARRAY) || k < 0 || k >= d.values[value].count)
		return -1;
	return d.values[value].first + k;
}

bool TfAsInt(const TfDoc& d, int value, __int64* out)
{
	if (!IsKind(d, value, TFK_INT))
		return false;
	*out = d.values[value].i;
	return true;
}

bool TfAsReal(const TfDoc& d, int value, float* out)
{
	if (IsKind(d, value, TFK_REAL))
	{
		*out = d.values[value].r;
		return true;
	}
	if (IsKind(d, value, TFK_INT))
	{
		*out = (float)d.values[value].i;
		return true;
	}
	return false;
}

int TfAsVec(const TfDoc& d, int value, float* out, int max)
{
	if (!IsKind(d, value, TFK_VEC))
		return -1;
	const TfValue& v = d.values[value];
	int n = v.count < max ? v.count : max;
	for (int k = 0; k < n; ++k)
		out[k] = d.reals[v.first + k];
	return n < 0 ? 0 : n;
}

const char* TfAsString(const TfDoc& d, int value)
{
	if (!IsKind(d, value, TFK_STRING))
		return "";
	return d.strings[d.values[value].first].c_str();
}

} // namespace planner
