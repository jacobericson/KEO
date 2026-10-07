#include "fixes/world/paused_skip_policy.h"

#include "check.h"

// The policy never dereferences the character; these stand in for one.
static void* const kNpc = (void*)(uintptr_t)0x2A4F0;
static void* const kPlayer = (void*)(uintptr_t)0x3B7C0;

static int s_playerCalls = 0;
static bool s_playerAnswer = false;

static bool FakeIsPlayer(void* ch)
{
	(void)ch;
	++s_playerCalls;
	return s_playerAnswer;
}

static void ResetPlayer(bool answer)
{
	s_playerCalls = 0;
	s_playerAnswer = answer;
}

// A living, off-screen, uncarried character with an animation that is no
// action slave: every gate lets it through.
static PausedSkipInputs Quiet()
{
	PausedSkipInputs in;
	in.visUpdate = 0;
	in.onScreen = 0;
	in.carried = 0;
	in.haveAnimation = true;
	in.actionSlave = 0;
	in.dead = 0;
	return in;
}

// A frame on which ch is not due, the first of 0..15.
static unsigned long NotDue(void* ch)
{
	for (unsigned long f = 0; f < PSK_PERIOD; ++f)
		if (!PausedSkipDue((uintptr_t)ch, f))
			return f;
	return 0;
}

static void CheckGates()
{
	const unsigned long frame = NotDue(kNpc);
	int calls = 0;
	PausedSkipInputs in = Quiet();
	in.onScreen = 1;
	ResetPlayer(true);
	Check(PausedSkipDecide(in, kNpc, frame, &FakeIsPlayer) == PSK_RUN_VISIBLE,
	      "gate: an on-screen character runs");
	calls += s_playerCalls;

	in = Quiet();
	in.visUpdate = 1;
	ResetPlayer(true);
	Check(PausedSkipDecide(in, kNpc, frame, &FakeIsPlayer) == PSK_RUN_VISIBLE,
	      "gate: a visible-update character runs");
	calls += s_playerCalls;

	in = Quiet();
	in.carried = 1;
	ResetPlayer(true);
	Check(PausedSkipDecide(in, kNpc, frame, &FakeIsPlayer) == PSK_RUN_KEPT, "gate: a carried character is kept");
	calls += s_playerCalls;

	in = Quiet();
	in.actionSlave = 1;
	ResetPlayer(true);
	Check(PausedSkipDecide(in, kNpc, frame, &FakeIsPlayer) == PSK_RUN_KEPT, "gate: an action slave is kept");
	calls += s_playerCalls;

	in = Quiet();
	in.haveAnimation = false;
	ResetPlayer(true);
	Check(PausedSkipDecide(in, kNpc, frame, &FakeIsPlayer) == PSK_RUN_KEPT,
	      "gate: a character with no animation is kept");
	calls += s_playerCalls;

	Check(calls == 0, "lease: the player test is not asked for a visible or kept character");
}

static void CheckLease()
{
	const unsigned long frame = NotDue(kPlayer);
	PausedSkipInputs in = Quiet();
	ResetPlayer(true);
	Check(PausedSkipDecide(in, kPlayer, frame, &FakeIsPlayer) == PSK_RUN_PLAYER && s_playerCalls == 1,
	      "lease: a living player character runs");

	in = Quiet();
	in.dead = 1;
	ResetPlayer(true);
	const PausedSkipVerdict dead = PausedSkipDecide(in, kPlayer, frame, &FakeIsPlayer);
	Check(dead != PSK_RUN_PLAYER && s_playerCalls == 0,
	      "lease: the player test is not asked for a dead character");
	Check(!PausedSkipDue((uintptr_t)kPlayer, frame) && dead == PSK_SKIP,
	      "lease: a dead player character off screen may be skipped");
}

static void CheckSkip()
{
	const unsigned long frame = NotDue(kNpc);
	PausedSkipInputs in = Quiet();
	ResetPlayer(false);
	Check(PausedSkipDecide(in, kNpc, frame, &FakeIsPlayer) == PSK_SKIP,
	      "skip: a quiet off-screen NPC that is not due is skipped");

	int due = 0, skipped = 0;
	for (unsigned long f = 100; f < 100 + PSK_PERIOD; ++f)
	{
		const PausedSkipVerdict v = PausedSkipDecide(in, kNpc, f, &FakeIsPlayer);
		if (v == PSK_RUN_DUE)
			++due;
		else if (v == PSK_SKIP)
			++skipped;
	}
	Check(due == 1 && skipped == (int)PSK_PERIOD - 1, "skip: the same NPC runs on its turn");
}

// Each of 64 characters due exactly once over the sixteen frames starting at first.
static bool OncePerPeriod(unsigned long long first)
{
	for (int i = 0; i < 64; ++i)
	{
		const uintptr_t ch = (uintptr_t)0x20000 + (uintptr_t)i * 0x7B0;
		int n = 0;
		for (unsigned k = 0; k < PSK_PERIOD; ++k)
			if (PausedSkipDue(ch, (unsigned long)(first + k)))
				++n;
		if (n != 1)
			return false;
	}
	return true;
}

static void CheckDue()
{
	Check(PSK_PERIOD == 16, "due: the refresh period is sixteen paused frames");

	Check(OncePerPeriod(0ull) && OncePerPeriod(4294967290ull),
	      "due: every character is due exactly once in sixteen consecutive frames");

	int worst = 0;
	for (unsigned long f = 0; f < PSK_PERIOD; ++f)
	{
		int n = 0;
		for (int i = 0; i < 64; ++i)
			if (PausedSkipDue((uintptr_t)0x10000000 + (uintptr_t)i * 0x1000, f))
				++n;
		if (n > worst)
			worst = n;
	}
	Check(worst <= 8, "due: a 4 KB stride spreads over the period");
}

int main()
{
	CheckGates();
	CheckLease();
	CheckSkip();
	CheckDue();
	return CheckExit("paused_skip_units");
}
