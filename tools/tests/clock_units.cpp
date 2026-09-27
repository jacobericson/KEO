// The QPC clock helpers at their edges: non-positive counts, the sign of a
// millisecond count, the 32-bit clamp and an unset frequency.

#include "base/clock.h"

#include "check.h"

LARGE_INTEGER qpcFrequency;
LARGE_INTEGER pluginStartTime;

int main()
{
	qpcFrequency.QuadPart = 10000000;

	Check(QpcToUs(0) == 0, "QpcToUs(0) == 0");
	Check(QpcToUs(-5) == 0, "QpcToUs(-5) == 0");
	Check(QpcToUs(12345678) == 1234567, "QpcToUs(12345678) == 1234567");

	Check(QpcToMs(-20000000) == -2000.0, "QpcToMs(-20000000) == -2000.0");
	Check(QpcToMs(15000) == 1.5, "QpcToMs(15000) == 1.5");

	Check(QpcDeltaUs(10, 5) == 0, "QpcDeltaUs(10, 5) == 0");
	Check(QpcDeltaUs(0, 10000000) == 1000000, "QpcDeltaUs(0, 10000000) == 1000000");
	Check(QpcDeltaUs(0, 36000000000LL) == 0x7FFFFFFF, "QpcDeltaUs(0, 36000000000) == 0x7FFFFFFF");
	Check(QpcDeltaUs(0, 0x7FFFFFF5LL * 10) == 0x7FFFFFF5, "QpcDeltaUs just below the clamp is not clamped");

	Check(QpcFromUs(2000000) == 20000000, "QpcFromUs(2000000) == 20000000");

	qpcFrequency.QuadPart = 0;
	Check(QpcToUs(12345678) == 0, "QpcToUs with no frequency == 0");
	Check(QpcToMs(15000) == 0.0, "QpcToMs with no frequency == 0");
	Check(QpcDeltaUs(0, 10000000) == 0, "QpcDeltaUs with no frequency == 0");
	Check(QpcFromUs(2000000) == 0, "QpcFromUs with no frequency == 0");

	LONGLONG a = QpcNow();
	LONGLONG b = QpcNow();
	Check(b >= a, "two QpcNow reads are in order");

	return CheckExit("clock_units");
}
