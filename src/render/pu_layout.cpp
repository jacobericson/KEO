#include "render/pu_layout.h"
#include <cstring>

// _update's non-visible test: cmp byte [rcx+27Ch],0 at +0x2B, then
// mov ecx,[rax+190h]; sub ecx,[rsi+274h] (Root frame minus last-visible frame) at +0x3A.
static const size_t        UPDATE_NONVIS_AT = 0x2B;
static const unsigned char UPDATE_NONVIS[7] = { 0x80,0xB9,0x7C,0x02,0x00,0x00,0x00 };
static const size_t        UPDATE_VISTEST_AT = 0x3A;
static const unsigned char UPDATE_VISTEST[12] = { 0x8B,0x88,0x90,0x01,0x00,0x00,0x2B,0x8E,0x74,0x02,0x00,0x00 };

// getTemplateName: lea rax,[rcx+380h]; ret
static const unsigned char TEMPLATE_NAME[8] = { 0x48,0x8D,0x81,0x80,0x03,0x00,0x00,0xC3 };

// setNonVisibleUpdateTimeout(float t):
//   comiss xmm1,[rip+c] (c = 0.0f); jbe clear
//   movss [rcx+278h],xmm1; mov byte [rcx+27Ch],1; ret
//   clear: mov dword [rcx+278h],0; mov byte [rcx+27Ch],0; ret
// So a timeout <= 0 clears the flag.
static const unsigned char SET_NONVIS_HEAD[3] = { 0x0F,0x2F,0x0D };
static const size_t        SET_NONVIS_BODY_AT = 7;
static const unsigned char SET_NONVIS_BODY[36] =
{
	0x76,0x10,
	0xF3,0x0F,0x11,0x89,0x78,0x02,0x00,0x00,
	0xC6,0x81,0x7C,0x02,0x00,0x00,0x01,
	0xC3,
	0xC7,0x81,0x78,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
	0xC6,0x81,0x7C,0x02,0x00,0x00,0x00,
	0xC3
};

// Root::getNextFrameNumber: mov eax,[rcx+190h]; ret
static const unsigned char ROOT_FRAME[7] = { 0x8B,0x81,0x90,0x01,0x00,0x00,0xC3 };

static bool InImage(HMODULE module, const void* p, size_t len)
{
	if (!module || !p)
		return false;
	uintptr_t base = (uintptr_t)module;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	uintptr_t a = (uintptr_t)p;
	return a >= base && a + len <= base + nt->OptionalHeader.SizeOfImage;
}

static bool BytesAt(HMODULE module, const unsigned char* fn, size_t at, const unsigned char* want, size_t len)
{
	return fn && InImage(module, fn + at, len) && memcmp(fn + at, want, len) == 0;
}

bool VerifyParticleLayout(HMODULE pu, HMODULE ogre)
{
	const unsigned char* upd  = (const unsigned char*)GetProcAddress(pu, SYM_PU_UPDATE);
	const unsigned char* name = (const unsigned char*)GetProcAddress(pu, SYM_PU_TEMPLATE);
	const unsigned char* set  = (const unsigned char*)GetProcAddress(pu, SYM_PU_SET_NONVISIBLE);
	const unsigned char* next = ogre ? (const unsigned char*)GetProcAddress(ogre, SYM_ROOT_NEXT_FRAME) : NULL;

	if (!BytesAt(pu, upd, UPDATE_NONVIS_AT, UPDATE_NONVIS, sizeof(UPDATE_NONVIS)) ||
	    !BytesAt(pu, upd, UPDATE_VISTEST_AT, UPDATE_VISTEST, sizeof(UPDATE_VISTEST)) ||
	    !BytesAt(pu, name, 0, TEMPLATE_NAME, sizeof(TEMPLATE_NAME)) ||
	    !BytesAt(ogre, next, 0, ROOT_FRAME, sizeof(ROOT_FRAME)) ||
	    !BytesAt(pu, set, 0, SET_NONVIS_HEAD, sizeof(SET_NONVIS_HEAD)) ||
	    !BytesAt(pu, set, SET_NONVIS_BODY_AT, SET_NONVIS_BODY, sizeof(SET_NONVIS_BODY)))
		return false;

	// The comparand must be 0.0f, else "clear" is not what a 0 argument does.
	int rel = *(const int*)(set + 3);
	const unsigned char* zero = set + SET_NONVIS_BODY_AT + rel;
	return InImage(pu, zero, 4) && *(const unsigned int*)zero == 0;
}

void ReadStdString(const void* s, char* out, size_t cap)
{
	if (!out || cap == 0)
		return;
	out[0] = 0;
	if ((uintptr_t)s < 0x10000 || cap < 2)
		return;
	size_t size = *(const size_t*)((const char*)s + STR_SIZE);
	size_t res  = *(const size_t*)((const char*)s + STR_CAPACITY);
	if (size > res || res > (1u << 20) || res < STR_SSO - 1)
		return;
	const char* p = res >= STR_SSO ? *(const char* const*)s : (const char*)s;
	if ((uintptr_t)p < 0x10000 || (uintptr_t)p >= 0x00007FFFFFFFFFFFULL)
		return;
	size_t n = size < cap - 1 ? size : cap - 1;
	memcpy(out, p, n);
	out[n] = 0;
}
