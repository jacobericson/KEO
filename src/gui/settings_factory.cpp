// settings_factory.cpp — a config module's settings rows and staged copy:
// the widget each key shows as, where that widget writes, and the INI entry
// a change to it becomes.

#include "gui/settings_factory.h"
#include "gui/settings_rows.h"
#include <cmath>
#include <climits>
#include <cstring>
#include <float.h>

namespace settings_factory_detail {
enum StageWidget { SW_NONE, SW_CHECKBOX, SW_SLIDER, SW_INT_SLIDER, SW_DROPBOX };
}
using namespace settings_factory_detail;

// CK_BOOL: a checkbox. CK_FLOAT and CK_DOUBLE: a slider. A row with choices:
// a drop box. CK_INT without choices but with a range: a slider in whole
// steps. Anything else stays INI-only.
static StageWidget WidgetOf(const ConfigKey& k)
{
	switch (k.kind)
	{
	case CK_BOOL:   return SW_CHECKBOX;
	case CK_FLOAT:
	case CK_DOUBLE: return SW_SLIDER;
	case CK_INT:
		if (k.choices)
			return SW_DROPBOX;
		return k.lo <= k.hi ? SW_INT_SLIDER : SW_NONE;
	case CK_CUSTOM: return k.choices ? SW_DROPBOX : SW_NONE;
	default:        return SW_NONE;
	}
}

bool SettingsKeyShown(const ConfigKey& k, bool devBuild)
{
	return k.label && !k.retired && k.kind != CK_TEXT && WidgetOf(k) != SW_NONE && (devBuild || (!k.devOnly && !k.debugOnlyReader));
}

// An offset row's widget writes the state field itself, so only a widget
// whose pointer type is the field's type can bind one.
static bool OffsetWidgetFits(const ConfigKey& k)
{
	return k.kind == CK_BOOL || k.kind == CK_FLOAT || (k.kind == CK_INT && k.choices);
}

// INI-only offset rows keep their state fields; a widget that cannot bind its
// field edits a slot instead.
static bool SlotBoundOffset(const ConfigKey& k)
{
	return k.label && !k.retired && !OffsetWidgetFits(k) && WidgetOf(k) != SW_NONE;
}

// The int the INI would hold for a slider value. Beyond the int range it
// saturates, as the loader's strtol does, so the clamp after it lands on the
// bound the next start would load.
static int RoundSlot(float f)
{
	if (f >= 2147483648.0f)
		return INT_MAX;
	if (f <= -2147483648.0f)
		return INT_MIN;
	return (int)floor(f + 0.5f);
}

// A row's storage, read at its width into every field of the slot: a target
// row's own, or a slot-bound offset row's staged field (StageModule passes it
// as the target).
static ConfigStageValue ReadTarget(const ConfigKey& k)
{
	ConfigStageValue v;
	v.b = false;
	v.i = 0;
	v.f = 0.0f;
	const char* p = (const char*)k.target;
	switch (k.kind)
	{
	case CK_BOOL:
		v.b = *(const bool*)p;
		v.i = v.b ? 1 : 0;
		v.f = (float)v.i;
		break;
	case CK_INT:
		v.i = *(const int*)p;
		v.b = v.i != 0;
		v.f = (float)v.i;
		break;
	case CK_FLOAT:
		v.f = *(const float*)p;
		break;
	case CK_DOUBLE:
		v.f = (float)*(const double*)p;
		break;
	case CK_CUSTOM:
		if (k.size == 1)
			v.i = *(const bool*)p ? 1 : 0;
		else if (k.size == sizeof(int))
			v.i = *(const int*)p;
		v.b = v.i != 0;
		v.f = (float)v.i;
		break;
	default:
		break;
	}
	return v;
}

void StageModule(const ConfigModule& m, ConfigModuleStage* s)
{
	if (m.state && m.stateSize <= sizeof(s->state))
		memcpy(s->state, m.state, m.stateSize);
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		if (!k.retired && (k.target || (m.state && SlotBoundOffset(k))))
		{
			ConfigKey local = k;
			if (!local.target) local.target = (char*)s->state + k.offset;
			s->slots[i] = ReadTarget(local);
		}
	}
}

static SettingsRow NewRow(SettingsRowKind kind, const std::string& label, const char* tooltip)
{
	SettingsRow r;
	r.kind = kind;
	r.label = label;
	r.tooltip = tooltip ? tooltip : "";
	r.boolPtr = NULL;
	r.floatPtr = NULL;
	r.intPtr = NULL;
	r.lo = r.hi = 0.0f;
	r.stepExp = 0;
	r.buttonId = 0;
	r.restart = false;
	return r;
}

// One shown key's row, bound into s: a fitting offset field directly, any
// other row through its staged slot.
static void AddKeyRow(const ConfigKey& k, int i, ConfigModuleStage* s, bool devBuild, std::vector<SettingsRow>* out)
{
	std::string label = k.label;
	std::string tooltip = k.tooltip ? k.tooltip : "";
	if (!k.live)
	{
		label += RESTART_MARK;
		tooltip += tooltip.empty() ? RESTART_NOTE : std::string(" ") + RESTART_NOTE;
	}
	char* field = !k.target && OffsetWidgetFits(k) ? (char*)s->state + k.offset : NULL;
	ConfigStageValue* slot = &s->slots[i];
	SettingsRow r;
	switch (WidgetOf(k))
	{
	case SW_CHECKBOX:
		r = NewRow(SR_CHECKBOX, label, tooltip.c_str());
		r.boolPtr = field ? (bool*)field : &slot->b;
		break;
	case SW_SLIDER:
	case SW_INT_SLIDER:
		r = NewRow(SR_SLIDER, label, tooltip.c_str());
		r.floatPtr = field ? (float*)field : &slot->f;
		r.lo = k.sliderLo;
		r.hi = k.hi;
		r.stepExp = WidgetOf(k) == SW_INT_SLIDER ? 0 : k.stepExp;
		break;
	case SW_DROPBOX:
		r = NewRow(SR_DROPBOX, label, tooltip.c_str());
		r.intPtr = field ? (int*)field : &slot->i;
		for (int c = 0; c < k.choiceCount; ++c)
		{
			const ConfigChoice& choice = k.choices[c];
			if (devBuild || !choice.devOnly || choice.value == *r.intPtr)
				r.choices.push_back(std::make_pair(std::string(choice.label), choice.value));
		}
		break;
	default:
		return;
	}
	r.restart = !k.live;
	out->push_back(r);
}

// A row's place among a section's widget groups: checkboxes, drop boxes, sliders.
static int WidgetGroup(const ConfigKey& k)
{
	switch (WidgetOf(k))
	{
	case SW_CHECKBOX: return 0;
	case SW_DROPBOX:  return 1;
	default:          return 2;
	}
}

void AddSectionRows(const char* title, const std::vector<SettingsKeyRef>& keys, bool devBuild,
                    std::vector<SettingsRow>* out)
{
	bool headerAdded = false;
	// Pass p shows widget group p / 2, its live keys on an even pass.
	for (int pass = 0; pass < 6; ++pass)
	{
		for (size_t j = 0; j < keys.size(); ++j)
		{
			const ConfigKey& k = keys[j].module->keys[keys[j].key];
			if (!SettingsKeyShown(k, devBuild) || WidgetGroup(k) != pass / 2 || k.live != (pass % 2 == 0))
				continue;
			if (!headerAdded)
			{
				out->push_back(NewRow(SR_HEADER, title, NULL));
				headerAdded = true;
			}
			AddKeyRow(k, keys[j].key, keys[j].stage, devBuild, out);
		}
	}
}

void AddModuleRows(const ConfigModule& m, ConfigModuleStage* s, bool devBuild, std::vector<SettingsRow>* out)
{
	std::vector<SettingsKeyRef> keys;
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		SettingsKeyRef ref = { &m, i, s };
		keys.push_back(ref);
	}
	AddSectionRows(m.title, keys, devBuild, out);
}

// Whether a target row's staged slot differs from its saved one, read
// through the field its widget writes.
static bool SlotDiffers(const ConfigKey& k, const ConfigStageValue& a, const ConfigStageValue& b)
{
	switch (k.kind)
	{
	case CK_BOOL:
		return a.b != b.b;
	case CK_INT:
		if (WidgetOf(k) == SW_INT_SLIDER)
			return RoundSlot(a.f) != RoundSlot(b.f);
		return a.i != b.i;
	case CK_FLOAT:
	case CK_DOUBLE:
		return memcmp(&a.f, &b.f, sizeof(float)) != 0;
	case CK_CUSTOM:
		return a.i != b.i;
	default:
		return false;
	}
}

// The slot's value as the INI writes it, formatted by the row's own rule
// from a local copy at the row's width.
static std::string FormatSlot(const ConfigModule& m, const ConfigKey& k, const ConfigStageValue& v)
{
	ConfigKey local = k;
	bool b = v.b;
	int i = v.i;
	float f = v.f;
	double d = (double)v.f;
	switch (k.kind)
	{
	case CK_BOOL:
		local.target = &b;
		break;
	case CK_INT:
		if (WidgetOf(k) == SW_INT_SLIDER)
			i = RoundSlot(v.f);
		local.target = &i;
		break;
	case CK_FLOAT:
		local.target = &f;
		break;
	case CK_DOUBLE:
		local.target = &d;
		break;
	case CK_CUSTOM:
		b = v.i != 0;
		local.target = k.size == 1 ? (void*)&b : (void*)&i;
		break;
	default:
		return "";
	}
	return ConfigFormatValue(m, local, NULL);
}

// A staged target slot held to the loader's rule, or its saved value when
// refused or not finite.
static void ClampSlot(const ConfigKey& k, ConfigStageValue* v, const ConfigStageValue& saved, ConfigLogFn log)
{
	switch (k.kind)
	{
	case CK_INT:
	{
		bool slider = WidgetOf(k) == SW_INT_SLIDER;
		if (slider && !_finite(v->f))
		{
			*v = saved;
			return;
		}
		int x = slider ? RoundSlot(v->f) : v->i;
		if (ConfigClampValue(k, &x, log) == CLAMP_REFUSED)
		{
			*v = saved;
			return;
		}
		if (slider)
			v->f = (float)x;
		else
			v->i = x;
		return;
	}
	case CK_FLOAT:
		if (!_finite(v->f))
			*v = saved;
		else
			ConfigClampValue(k, &v->f, log);
		return;
	case CK_DOUBLE:
	{
		if (!_finite(v->f))
		{
			*v = saved;
			return;
		}
		double d = (double)v->f;
		ConfigClampValue(k, &d, log);
		v->f = (float)d;
		return;
	}
	default:
		return;
	}
}

// A staged offset field held to the loader's rule, or its saved value when
// refused or not finite.
static void ClampField(const ConfigKey& k, char* field, const char* saved, ConfigLogFn log)
{
	size_t width;
	bool finite = true;
	switch (k.kind)
	{
	case CK_INT:    width = sizeof(int); break;
	case CK_FLOAT:  width = sizeof(float); finite = _finite(*(const float*)field) != 0; break;
	case CK_DOUBLE: width = sizeof(double); finite = _finite(*(const double*)field) != 0; break;
	default:        return;
	}
	if (!finite || ConfigClampValue(k, field, log) == CLAMP_REFUSED)
		memcpy(field, saved, width);
}

// A slot-bound offset row is copied at its own width before state comparisons.
static void WriteSlotField(const ConfigKey& k, const ConfigStageValue& slot, void* state)
{
	char* field = (char*)state + k.offset;
	switch (k.kind)
	{
	case CK_INT: *(int*)field = WidgetOf(k) == SW_INT_SLIDER ? RoundSlot(slot.f) : slot.i; break;
	case CK_DOUBLE: *(double*)field = (double)slot.f; break;
	case CK_CUSTOM:
		if (k.size == 1) *(bool*)field = slot.i != 0;
		else if (k.size == sizeof(int)) *(int*)field = slot.i;
		break;
	default: break;
	}
}

void ClampModuleStage(const ConfigModule& m, ConfigModuleStage* s, const ConfigModuleStage& saved, ConfigLogFn log)
{
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		if (k.retired)
			continue;
		if (k.target || (m.state && SlotBoundOffset(k)))
		{
			// A slot that writes the same value as its saved one takes it back
			// exactly, so an integer slider's fraction is never kept.
			if (SlotDiffers(k, s->slots[i], saved.slots[i]))
				ClampSlot(k, &s->slots[i], saved.slots[i], log);
			else
				s->slots[i] = saved.slots[i];
			if (!k.target)
			{
				if (SlotDiffers(k, s->slots[i], saved.slots[i]))
					WriteSlotField(k, s->slots[i], s->state);
				else memcpy((char*)s->state + k.offset, (const char*)saved.state + k.offset, k.size);
			}
		}
		else if (m.state && !ConfigOffsetValueEqual(k, s->state, saved.state))
		{
			ClampField(k, (char*)s->state + k.offset, (const char*)saved.state + k.offset, log);
		}
	}
}

int ModuleStageEntries(const ConfigModule& m, const ConfigModuleStage& staged, const ConfigModuleStage& saved,
                       std::vector<IniEntry>* out)
{
	int n = 0;
	ConfigModuleStage stagedState = staged;
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		if (k.retired)
			continue;
		IniEntry e;
		e.key = k.name;
		e.kind = ConfigIniKind(k.kind);
		if (k.target || (m.state && SlotBoundOffset(k)))
		{
			// A custom row without choices has no INI text for its value.
			if ((k.kind == CK_CUSTOM && !k.choices) || k.kind == CK_TEXT)
				continue;
			if (!SlotDiffers(k, staged.slots[i], saved.slots[i]))
				continue;
			if (!k.target) WriteSlotField(k, staged.slots[i], stagedState.state);
			e.value = FormatSlot(m, k, staged.slots[i]);
		}
		else
		{
			if (!m.state || k.kind == CK_CUSTOM || ConfigOffsetValueEqual(k, staged.state, saved.state))
				continue;
			e.value = ConfigFormatValue(m, k, staged.state);
		}
		e.append = ConfigEntryAppends(k, e.value, stagedState.state, m.defaults);
		out->push_back(e);
		++n;
	}
	return n;
}

static int LiveRows(const ConfigModule& m, const ConfigModuleStage& staged, bool apply,
                    std::vector<std::string>* applied)
{
	if (!m.state)
		return 0;
	int n = 0;
	for (int i = 0; i < CONFIG_STAGE_MAX && m.keys[i].name; ++i)
	{
		const ConfigKey& k = m.keys[i];
		if (!k.live || k.retired || k.target || ConfigOffsetValueEqual(k, staged.state, m.state))
			continue;
		if (apply)
		{
			memcpy((char*)m.state + k.offset, (const char*)staged.state + k.offset, k.size);
			if (applied)
				applied->push_back(std::string(k.name) + "=" + ConfigFormatValue(m, k, m.state));
		}
		++n;
	}
	return n;
}

int LiveModuleRowsDiffering(const ConfigModule& m, const ConfigModuleStage& staged)
{
	return LiveRows(m, staged, false, NULL);
}

int ApplyLiveModuleRows(const ConfigModule& m, const ConfigModuleStage& staged, std::vector<std::string>* applied)
{
	return LiveRows(m, staged, true, applied);
}
