// config_rows.h - offset rows into a module's POD config and constant defaults.
#pragma once
#include "base/config_table.h"
#include <cstddef>
#include <climits>

#define DOC true
#define NDOC false
// SHOW: a row on every build's settings page. DEVROW: a developer row
// (diagnostic, internal, guard or tuning constant), on the DEV page only.
#define SHOW false
#define DEVROW true
#define CFG_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define CFG_OROW(n, kind, T, field, lo, hi, minI, posOnly, doc, fn, label, tip, diag, sLo, sExp, ch, chN, dbg) \
	{ n, kind, offsetof(T, field), sizeof(((T*)0)->field), lo, hi, false, label, tip, diag, sLo, sExp, \
	  NULL, minI, posOnly, doc, false, NULL, NULL, fn, ch, chN, dbg }
#define CFG_OBOOL(n, T, field, doc, diag, label, tip) \
	CFG_OROW(n, CK_BOOL, T, field, 1.0f, 0.0f, INT_MIN, false, doc, NULL, label, tip, diag, 0.0f, 0, NULL, 0, false)
#define CFG_OBOOL_DBG(n, T, field, doc, diag, label, tip) \
	CFG_OROW(n, CK_BOOL, T, field, 1.0f, 0.0f, INT_MIN, false, doc, NULL, label, tip, diag, 0.0f, 0, NULL, 0, true)
#define CFG_OINT(n, T, field, lo, hi, minI, doc, diag, label, tip) \
	CFG_OROW(n, CK_INT, T, field, lo, hi, minI, false, doc, NULL, label, tip, diag, lo, 0, NULL, 0, false)
#define CFG_OINT_CHOICES(n, T, field, lo, hi, minI, doc, diag, label, tip, ch) \
	CFG_OROW(n, CK_INT, T, field, lo, hi, minI, false, doc, NULL, label, tip, diag, lo, 0, ch, CFG_COUNT(ch), false)
#define CFG_OFLOAT(n, T, field, lo, hi, posOnly, doc, diag, label, tip, sLo, sExp) \
	CFG_OROW(n, CK_FLOAT, T, field, lo, hi, INT_MIN, posOnly, doc, NULL, label, tip, diag, sLo, sExp, NULL, 0, false)
#define CFG_ODOUBLE(n, T, field, lo, hi, doc, diag, label, tip, sLo, sExp) \
	CFG_OROW(n, CK_DOUBLE, T, field, lo, hi, INT_MIN, false, doc, NULL, label, tip, diag, sLo, sExp, NULL, 0, false)
// A custom row without choices stays INI-only.
#define CFG_OCUSTOM(n, T, field, fn, doc) \
	CFG_OROW(n, CK_CUSTOM, T, field, 1.0f, 0.0f, INT_MIN, false, doc, fn, NULL, NULL, false, 0.0f, 0, NULL, 0, false)
#define CFG_OCUSTOM_CHOICES(n, T, field, fn, doc, diag, label, tip, ch) \
	CFG_OROW(n, CK_CUSTOM, T, field, 1.0f, 0.0f, INT_MIN, false, doc, fn, label, tip, diag, 0.0f, 0, ch, CFG_COUNT(ch), false)
