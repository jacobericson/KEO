#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gui/settings_panel.h"
#include "gui/settings_rows.h"
#include "gui/bench_buttons.h"
#include "render/render_config.h"
#include "render/render_keys.h"
#include "bench/bench_lever_ab.h"
#include "bench/bench_runner.h"
#include "bench/bench_sweep.h"
#include "base/config.h"
#include "game/game.h"
#include "base/core.h"
#include "zone/zone_config.h"
#include <stddef.h>
#include <string.h>
#include "base/klib_include.h"
#include <kenshi/Globals.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/OptionsWindow.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <mygui/MyGUI_TabControl.h>
#include <mygui/MyGUI_TabItem.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_ComboBox.h>
#include <mygui/MyGUI_Delegate.h>
#include "base/klib_include_end.h"

static_assert(offsetof(OptionsWindow, tabs) == 0x108, "OptionsWindow::tabs");
static_assert(offsetof(OptionsWindow, tooltip) == 0x118, "OptionsWindow::tooltip");
static_assert(offsetof(DatapanelGUI, currentCategory) == 0xB0, "DatapanelGUI::currentCategory");
static_assert(offsetof(DatapanelGUI, basicSpacing) == 0x130, "DatapanelGUI::basicSpacing");
static_assert(offsetof(DataPanelLine, callback) == 0x20, "DataPanelLine::callback");
static_assert(offsetof(DataPanelLine_Button, button) == 0x140, "DataPanelLine_Button::button");
static_assert(offsetof(DataPanelLine_DropBox, listBox) == 0x120, "DataPanelLine_DropBox::listBox");

#ifdef KEO_DEBUG
static const bool DEV_BUILD = true;
#else
static const bool DEV_BUILD = false;
#endif

typedef void (*OptionsFn_t)(OptionsWindow* win);

static OptionsFn_t     s_origCreate = NULL;
static OptionsFn_t     s_origSave   = NULL;
static bool            s_active     = false;   // both hooks installed
static bool            s_staged     = false;   // a tab was built since the last close
static SettingsStaging s_staging;              // what the rows write
static SettingsStaging s_saved;                // what KEO.ini holds
static const char*     s_token      = "off(gate)";   // until InstallSettingsPanel runs
static bool            s_buttonsOk  = false;   // the button calls' addresses match KenshiLib's
static bool            s_groupsOk   = false;   // a checkbox line's toggle invokes its callback (checked at install)

namespace settings_panel_detail {
// A greyed row's line and the checkbox line whose staged value enables it; rebuilt on every
// create and dropped at every close, as the Benchmark buttons are.
struct GatedLine
{
	DataPanelLine* member;
	DataPanelLine* head;
	const bool*    value;
};
} // namespace settings_panel_detail
using namespace settings_panel_detail;

static const int MAX_GATED = 8;
static GatedLine s_gated[MAX_GATED];
static int       s_gatedCount = 0;
static const unsigned char kCheckboxToggleCallback[18] =
	{ 0x48,0x8B,0x4B,0x20,0x48,0x85,0xC9,0x74,0x09,0x48,0x8B,0x01,0x48,0x8B,0xD3,0xFF,0x50,0x10 };

// The Benchmark section's button lines of the tab being shown, rebuilt on
// every create. hide() destroys the lines, so a pointer here is only ever
// compared with the line a press hands in, or used during that press.
struct ButtonLine
{
	DataPanelLine_Button* line;
	int                   id;
};
static const int  MAX_BUTTONS = 2 * BENCH_SLOT_COUNT + 1;
static ButtonLine s_buttons[MAX_BUTTONS];
static int        s_buttonCount = 0;

static const float BUTTON_WIDTH = 0.4f;

static const int PREFERRED_CATEGORY = 48;

// A category no other tab's panel uses.
static int PickCategory(MyGUI::TabControl* tabs, size_t count)
{
	int maxCat = 0;
	bool clash = false;
	for (size_t i = 0; i < count; ++i)
	{
		DatapanelGUI** p = tabs->getItemDataAt<DatapanelGUI*>(i, false);
		if (!p || !*p)
			continue;
		int cat = (*p)->currentCategory;
		if (cat == PREFERRED_CATEGORY)
			clash = true;
		if (cat > maxCat)
			maxCat = cat;
	}
	return clash ? maxCat + 1 : PREFERRED_CATEGORY;
}

// Each Run button reads Stop while its slot's run is active; the sweep
// button shows the sweep's progress.
static void UpdateRunCaptions(int activeSlot)
{
	for (int i = 0; i < s_buttonCount; ++i)
	{
		int id = s_buttons[i].id;
		MyGUI::Button* b = s_buttons[i].line->button;
		if (!b)
			continue;
		if (id == BENCH_BUTTON_SWEEP)
			b->setCaption(SweepCaption(BenchSweepLegNumber(), BenchSweepLegTotal()));
		else if (id >= BENCH_BUTTON_RUN && id < BENCH_BUTTON_RUN + BENCH_SLOT_COUNT)
			b->setCaption(id - BENCH_BUTTON_RUN == activeSlot ? "Stop" : "Run");
	}
}

// The line's callback (DataPanelLine_Button::pressCallback), from MyGUI's
// click dispatch on the main thread; nothing may throw back into the game.
static void OnButtonPress(DataPanelLine* line)
{
	try
	{
		for (int i = 0; i < s_buttonCount; ++i)
		{
			if (s_buttons[i].line != line)
				continue;
			int id = s_buttons[i].id;
			int slot = id % 100;
			int speed = id != BENCH_BUTTON_SWEEP && slot >= 0 && slot < BENCH_SLOT_COUNT ? s_staging.benchSpeed[slot] : 1;
			UpdateRunCaptions(BenchButtonPressed(id, speed));
			return;
		}
	}
	catch (...)
	{
		LogMsg("Settings panel: a Benchmark button failed");
	}
}

// A grouped checkbox's callback (DataPanelLine_CheckBox::notifyToggleCheck, after it wrote the
// staged value), from MyGUI's click dispatch on the main thread: its members follow its value.
static void OnGroupToggle(DataPanelLine* line)
{
	try
	{
		for (int i = 0; i < s_gatedCount; ++i)
		{
			if (s_gated[i].head == line)
				s_gated[i].member->setEnabled(*s_gated[i].value);
		}
	}
	catch (...)
	{
		LogMsg("Settings panel: a grouped row failed to follow its checkbox");
	}
}

// The game's own Options tabs draw a heading as "[title]" in this colour,
// after a full line of space.
static const char* const HEADING_COLOUR = "#afa68b";
static const char* const NOTE_COLOUR    = "#a0a0a0";

// The game's slider lines end their value box, and its checkbox lines their
// box, at this fraction of the panel's width; the value box starts this far
// before it.
static const float VALUE_COLUMN_RIGHT = 0.98f;
static const float VALUE_COLUMN_WIDTH = 0.16f;

// A drop box line places its box at a fixed left offset; this moves it into
// the value column, keeping its row and its width.
static void AlignToValueColumn(MyGUI::Widget* box)
{
	if (!box || !box->getParent())
		return;
	int panelWidth = box->getParent()->getWidth();
	box->setPosition((int)(panelWidth * (VALUE_COLUMN_RIGHT - VALUE_COLUMN_WIDTH)), box->getTop());
}

static void AddRows(DatapanelGUI* panel, const std::vector<SettingsRow>& rows, int cat, ToolTip* tooltip)
{
	const std::string buttonSkin = "Kenshi_Button2";
	std::vector<DataPanelLine*> lines(rows.size(), (DataPanelLine*)NULL);
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const SettingsRow& r = rows[i];
		switch (r.kind)
		{
		case SR_HEADER:
			// A note already brings its own space.
			if (i && rows[i - 1].kind != SR_NOTE)
				panel->addSpace(cat, 1.0f);
			panel->setLine(std::string(HEADING_COLOUR) + "[" + r.label + "]", std::string(), cat, false, true);
			break;
		case SR_TEXT:
			panel->setLine(r.label, std::string(), cat, false, true);
			break;
		case SR_NOTE:
			panel->addSpace(cat, 1.0f);
			panel->setLine(NOTE_COLOUR + r.label, std::string(), cat, false, true);
			break;
		case SR_BUTTON:
		{
			// NULL when a line with this key exists and is not a button.
			DataPanelLine_Button* b = panel->setLineTextButton(r.label, r.caption, cat, BUTTON_WIDTH, buttonSkin);
			if (!b || s_buttonCount >= MAX_BUTTONS)
				break;
			b->setToolTip(r.tooltip, tooltip);
			b->callback = MyGUI::newDelegate(&OnButtonPress);
			s_buttons[s_buttonCount].line = b;
			s_buttons[s_buttonCount].id = r.buttonId;
			++s_buttonCount;
			break;
		}
		case SR_CHECKBOX:
		{
			DataPanelLine_CheckBox* c = panel->setLineCheckbox(r.label, r.boolPtr, cat);
			c->setToolTip(r.tooltip, tooltip);
			lines[i] = c;
			break;
		}
		case SR_SLIDER:
		{
			DataPanelLine_SliderEditable* s = panel->setLineSliderEditable(r.label, cat, true, r.lo, r.hi, r.floatPtr);
			s->setPrecision(r.stepExp);
			s->setToolTip(r.tooltip, tooltip);
			lines[i] = s;
			break;
		}
		case SR_DROPBOX:
		{
			DataPanelLine_DropBox* d = panel->setLineDropBox(r.label, cat, r.intPtr, false, VALUE_COLUMN_WIDTH);
			AlignToValueColumn(d->listBox);
			for (size_t c = 0; c < r.choices.size(); ++c)
				d->addAValue(r.choices[c].first, r.choices[c].second);
			d->refresh();
			d->setToolTip(r.tooltip, tooltip);
			lines[i] = d;
			break;
		}
		}
	}
	if (!s_groupsOk)
		return;
	int refused = 0;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const int head = rows[i].enabledByRow;
		if (head < 0 || (size_t)head >= rows.size() || !lines[i] || !lines[head] || !rows[head].boolPtr)
			continue;
		if (s_gatedCount >= MAX_GATED)
		{
			++refused;
			continue;
		}
		lines[i]->setEnabled(*rows[head].boolPtr);
		GatedLine g = { lines[i], lines[head], rows[head].boolPtr };
		s_gated[s_gatedCount++] = g;
		lines[head]->callback = MyGUI::newDelegate(&OnGroupToggle);
	}
	if (refused)
	{
		std::ostringstream ss;
		ss << "Settings panel: " << refused << " grouped row(s) past the " << MAX_GATED
		   << "-row gated list stay enabled";
		LogMsg(ss.str());
	}
}

// The squad radius row's floor from the game's Fast zone hopping option as
// it reads now. Its checkbox writes the option when clicked, so the close
// sees a change made during the same visit to the Options window.
static ConfigFloor SquadRadiusFloor()
{
	return zone::ZoneSquadRadiusFloor(GameFastZoneHopping());
}

static void LogFloorRaised(const ConfigFloor& f, const char* when)
{
	std::ostringstream ss;
	ss << "Settings panel: " << f.key << " below its floor, raised to " << f.floor << " " << when;
	LogMsg(ss.str());
}

// Inserts the tab before the last one (Mods, where RE_Kenshi puts its
// button). hide() casts every tab's item data to DatapanelGUI* and throws on
// anything else, then destroys the panel and removes the tab itself, so the
// tab carries its panel from the moment the panel exists and nothing here
// outlives this call.
static void BuildTab(OptionsWindow* win)
{
	MyGUI::TabControl* tabs = win->tabs;
	if (!tabs || !gui)
		return;
	size_t count = tabs->getItemCount();
	if (count == 0)
		return;

	int renderModule = RenderModuleIndex();
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		s_staging.module[m] = s_saved.module[m];
		if (m != renderModule)
			StageLiveModuleRows(kConfigModules[m], &s_staging.module[m]);
	}
	if (renderModule >= 0)
	{
		RenderConfig& render = StagedRender(&s_staging);
		render = g_renderCfg;
		BenchLeverUserRenderConfig(&render);
		render.renderLevers = StagedRender(&s_saved).renderLevers;
	}
	StageBenchSpeeds(&s_staging, g_benchSlots);
	s_buttonCount = 0;
	s_gatedCount = 0;

	SettingsBench bench;
	bench.available = s_buttonsOk && BenchAvailable();
	bench.reason = !s_buttonsOk ? "button calls" : BenchUnavailableReason();
	bench.slots = g_benchSlots;
	bench.activeSlot = BenchRunnerSlot();
	bench.sweepLeg = BenchSweepLegNumber();
	bench.sweepLegs = BenchSweepLegTotal();
	bench.runSec = BenchLeverRunSeconds(BenchLeversCombined());
	bench.runSecCombined = BenchLeverRunSeconds(true);
	bench.sweepLegCount = BenchSweepLegCount();

	MyGUI::TabItem* tab = NULL;
	bool owned = false;
	try
	{
		std::vector<SettingsRow> rows;
		BuildSettingsRows(&s_staging, DEV_BUILD, &bench, &rows);
		ConfigFloor floor = SquadRadiusFloor();
		if (ApplySettingsFloor(&s_staging, floor, &rows))
			LogFloorRaised(floor, "on the tab");
		int cat = PickCategory(tabs, count);
		tab = tabs->insertItemAt(count - 1, "KEO");
		DatapanelGUI* panel = gui->createDatapanel("keo_options", tab, true);
		if (!panel)
		{
			tabs->removeItem(tab);
			return;
		}
		tabs->setItemData(tab, (DatapanelGUI*)panel);
		owned = true;
		panel->changeCategory(cat);
		panel->setLineSpacing(25.0f);
		// Until its first control line, a panel puts each line in a fixed slot
		// a full line apart; packing from the start keeps the first heading as
		// close to its rows as the others.
		panel->basicSpacing = false;
		AddRows(panel, rows, cat, win->tooltip);
		tab->setVisible(false);
		s_staged = true;
	}
	catch (...)
	{
		try
		{
			if (owned)
				tab->setVisible(false);
			else if (tab)
				tabs->removeItem(tab);
		}
		catch (...) {}
		LogMsg("Settings panel: building the tab failed, left out");
	}
}

static void CommitStaging()
{
	s_staged = false;
	SettingsStaging staged = s_staging;
	ConfigFloor floor = SquadRadiusFloor();
	if (ApplySettingsFloor(&staged, floor, NULL))
		LogFloorRaised(floor, "at close");
	int renderModule = RenderModuleIndex();
	RenderConfig& render = StagedRender(&staged);
	std::vector<std::string> notes;
	ClampSettings(&staged, s_saved, g_renderCfg, &LogMsg, &notes);
	for (size_t i = 0; i < notes.size(); ++i)
		LogMsg("Settings panel: " + notes[i]);

	SettingsDiff d = DiffSettings(staged, g_renderCfg, s_saved);
	if (!d.applied && !d.saved)
		return;

	if (renderModule >= 0)
	{
		// renderLevers is startup-only: pass it on only when it changed, so
		// ApplyRenderConfig says once that it waits for a restart.
		RenderConfig next = render;
		if (next.renderLevers == StagedRender(&s_saved).renderLevers)
			next.renderLevers = g_renderCfg.renderLevers;
		ApplyRenderConfig(next);
	}
	// The other modules' live rows go straight into the running config, one aligned store each;
	// this runs on the main thread, and a reader elsewhere loads each field once per use.
	std::vector<std::string> live;
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
	{
		if (m != renderModule)
			ApplyLiveModuleRows(kConfigModules[m], staged.module[m], &live);
	}
	for (size_t i = 0; i < live.size(); ++i)
		LogMsg("Settings panel: live " + live[i]);

	int savedKeys = 0;
	if (d.saved)
	{
		// The render module's keys are SaveRenderConfig's; every other module's
		// changed keys go in as extra entries.
		std::vector<IniEntry> extra;
		for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
		{
			if (m != renderModule)
				ModuleStageEntries(kConfigModules[m], staged.module[m], s_saved.module[m], &extra);
		}
		bool written = renderModule >= 0 ? SaveRenderConfig(render, extra)
		                                 : SaveIniEntries(extra, NULL, "Settings panel");
		if (written)
		{
			s_saved = staged;
			savedKeys = d.saved;
		}
	}
	std::ostringstream ss;
	// A lever's on/off path runs on the in-game render tick, so a change made
	// at the title screen acts once a game is loaded.
	ss << "Settings panel: applied " << d.applied << " live key(s) (levers act from the next in-game frame), saved "
	   << savedKeys << " key(s)";
	LogDebug(ss.str());
}

static void hook_OptionsCreate(OptionsWindow* win)
{
	s_origCreate(win);
	if (s_active)
		BuildTab(win);
}

// Runs once per close, from hide(), before the panels are destroyed.
static void hook_OptionsSaveOptions(OptionsWindow* win)
{
	s_origSave(win);
	s_buttonCount = 0;
	s_gatedCount = 0;
	if (s_active && s_staged)
		CommitStaging();
}

// KenshiLib's address for the function must be the one the prologue row checks.
static bool HookSite(size_t rva, void* klibAddr, void* detour, void** orig, const char* name)
{
	void* ours = GameAddr(rva);
	if (klibAddr != ours)
	{
		LogMsg(std::string("Settings panel: ") + name + " address differs from KenshiLib's");
		return false;
	}
	if (!VerifyPrologueByRva(rva))
		return false;
	if (KenshiLib::AddHook(ours, detour, orig) != KenshiLib::SUCCESS)
	{
		LogMsg(std::string("Settings panel: ") + name + " hook failed");
		return false;
	}
	return true;
}

void InstallSettingsPanel()
{
	if (!keo_gui::g_guiCfg.settingsPanelEnabled)
	{
		s_token = "off(ini)";
		return;
	}
	for (int m = 0; m < kConfigModuleCount && m < CONFIG_MODULE_MAX; ++m)
		StageModule(kConfigModules[m], &s_saved.module[m]);

	// The create hook does nothing until both are in, so a tab is never
	// built whose changes could not be applied.
	bool ok = HookSite(RVA_OPTIONS_CREATE, (void*)KlibRealAddress(&OptionsWindow::create),
	                   (void*)hook_OptionsCreate, (void**)&s_origCreate, "OptionsWindow::create")
	       && HookSite(RVA_OPTIONS_SAVE_OPTIONS, (void*)KlibRealAddress(&OptionsWindow::saveOptions),
	                   (void*)hook_OptionsSaveOptions, (void**)&s_origSave, "OptionsWindow::saveOptions");
	s_active = ok;
	s_token = ok ? "ok" : "off(hook)";
	if (!ok)
		LogMsg("Settings panel: off, the Options window keeps its own tabs only");

	// The Benchmark buttons (DEV only) call setLineTextButton and rely on
	// pressCallback invoking the line's callback; a mismatch leaves only their
	// section out.
	s_buttonsOk = (void*)KlibRealAddress(&DatapanelGUI::setLineTextButton) == GameAddr(RVA_DP_SET_LINE_TEXT_BUTTON)
	           && (void*)KlibRealAddress(&DataPanelLine_Button::pressCallback) == GameAddr(RVA_DP_BUTTON_PRESS);
	if (ok && !s_buttonsOk && DEV_BUILD)
		LogMsg("Bench: button call addresses differ from KenshiLib's, Benchmark section left out");

	// Grouped rows grey while their checkbox is off; that relies on the checkbox's toggle invoking
	// the line's callback after it writes the value. A mismatch leaves every row enabled.
	s_groupsOk = memcmp((const void*)GameAddr(RVA_DP_CHECKBOX_TOGGLE_CALLBACK), kCheckboxToggleCallback, sizeof(kCheckboxToggleCallback)) == 0;
	if (ok && !s_groupsOk)
		LogMsg("Settings panel: the checkbox toggle differs; grouped rows stay enabled");
}

const char* SettingsPanelToken()
{
	if (!keo_gui::g_guiCfg.settingsPanelEnabled)
		return "off(ini)";
	return s_token;
}
