// *****************************************************************************
// Tapir Translator - engine: the term corrections window (see CorrectionsWindow.hpp)
//
//   A Win32 window built by code (the engine has no resource file), with the
//   Common Controls 6 visual styles (manifest dependency below), the Windows
//   message font, sizes scaled to the display DPI, and a layout recomputed on
//   every resize. Both lists are virtual list views (LVS_OWNERDATA): their texts
//   are produced on demand from the model, so editing a rule only invalidates
//   the lists instead of rebuilding them.
// *****************************************************************************

#include "CorrectionsWindow.hpp"
#include "Corrections.hpp"
#include "TT_Texts.hpp"

#if !defined (NOMINMAX)
	#define NOMINMAX	// std::min / std::max instead of the windows.h macros
#endif
#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <vector>

#pragma comment (lib, "user32.lib")
#pragma comment (lib, "gdi32.lib")
#pragma comment (lib, "comctl32.lib")
// Common Controls 6: modern look of buttons, lists and edit fields (also used by the
// MyMemory quota window of the same executable).
#pragma comment (linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

const wchar_t* const kClass = L"TapirTranslatorCorrections";

// Control identifiers.
enum {
	kLanguage = 200, kSearch, kRules, kAdd, kDuplicate, kDelete, kSources, kWrong, kCorrect, kCache, kSample, kCorrectWord,
	kSave = IDOK, kCancel = IDCANCEL, kAutoClose = 1
};

// UTF-8 -> UTF-16.
std::wstring Wide (const std::string& s)
{
	if (s.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0);
	std::wstring r (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n);
	return r;
}

// UTF-16 -> UTF-8.
std::string Narrow (const std::wstring& s)
{
	if (s.empty ())
		return std::string ();
	const int n = WideCharToMultiByte (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), nullptr, 0, nullptr, nullptr);
	std::string r (static_cast<size_t> (n), '\0');
	WideCharToMultiByte (CP_UTF8, 0, s.data (), static_cast<int> (s.size ()), &r[0], n, nullptr, nullptr);
	return r;
}

// Lower case (invariant locale), for the case-insensitive search.
std::wstring Lower (std::wstring s)
{
	if (!s.empty ())
		LCMapStringEx (LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data (), static_cast<int> (s.size ()), &s[0],
					   static_cast<int> (s.size ()), nullptr, nullptr, 0);
	return s;
}

// Text of a window (edit field...).
std::wstring GetText (HWND h)
{
	const int n = GetWindowTextLengthW (h);
	std::wstring s (static_cast<size_t> (n) + 1, L'\0');
	GetWindowTextW (h, &s[0], n + 1);
	s.resize (static_cast<size_t> (n));
	return s;
}

// One row of the rules list: pair 'pair' of rule 'rule' of the current language.
struct Row {
	size_t rule = 0;
	size_t pair = 0;
	int count = 0;	// cached translations changed by this replacement alone
};

// Window state, attached with GWLP_USERDATA.
struct State {
	// Data
	std::wstring path;						// corrections.json
	std::wstring root;						// its folder (holds cache\)
	int ui = 1;								// interface language index
	Corrections::File file;					// rules of every language
	std::string output;						// current output language code
	std::vector<Row> rows;					// visible rule rows (search filter applied)
	std::vector<Corrections::CachedTranslation> cache;	// cached translations into 'output'
	std::vector<size_t> cacheRows;			// visible cache entries (search filter applied)
	bool dirty = false;						// unsaved changes
	bool updating = false;					// the editor is being filled by code (ignore EN_CHANGE)
	bool autoClose = false;					// test bench: close without questions
	std::wstring scratch;					// text returned to the list views (LVN_GETDISPINFO)
	// Look
	int dpi = 96;
	HFONT font = nullptr, bold = nullptr;
	// Controls
	HWND window = nullptr, outLabel = nullptr, language = nullptr, searchLabel = nullptr, search = nullptr;
	HWND rulesTitle = nullptr, rules = nullptr, add = nullptr, duplicate = nullptr, remove = nullptr;
	HWND selTitle = nullptr, srcLabel = nullptr, sources = nullptr, wrongLabel = nullptr, wrong = nullptr;
	HWND correctLabel = nullptr, correct = nullptr, hint = nullptr;
	HWND cacheTitle = nullptr, cacheHint = nullptr, cacheList = nullptr, sample = nullptr, correctWord = nullptr;
	HWND beforeLabel = nullptr, beforeText = nullptr, afterLabel = nullptr, afterText = nullptr;
	HWND live = nullptr, cancel = nullptr, save = nullptr;

	int E (int v) const { return MulDiv (v, dpi, 96); }
	std::wstring T (TT::Text t) const { return Wide (TT::Txt (t, ui)); }
	std::vector<Corrections::Rule>& Rules () { return file.languages[output]; }
};

// --- model helpers ----------------------------------------------------------------------------

// The single replacement of a row as a one-rule list (for its own count and preview).
std::vector<Corrections::Rule> SingleRule (State& s, const Row& r)
{
	const Corrections::Rule& rule = s.Rules ()[r.rule];
	Corrections::Rule one;
	one.sources = rule.sources;
	one.pairs.push_back (rule.pairs[r.pair]);
	one.replaceForm = rule.replaceForm || rule.pairs.size () > 1;
	return { one };
}

// Number of cached translations changed by the row's replacement alone.
int CountRow (State& s, const Row& r)
{
	const auto one = SingleRule (s, r);
	if (one[0].pairs[0].wrong.empty ())
		return 0;
	int n = 0;
	for (const auto& c : s.cache)
		n += Corrections::Apply (one, s.output, c.original, c.translated) != c.translated ? 1 : 0;
	return n;
}

// Rebuilds the visible rows (search filter on sources, wrong and correct) and their counts.
void BuildRows (State& s)
{
	const std::wstring filter = Lower (GetText (s.search));
	s.rows.clear ();
	auto& rules = s.Rules ();
	for (size_t i = 0; i < rules.size (); ++i) {
		for (size_t j = 0; j < rules[i].pairs.size (); ++j) {
			const auto& p = rules[i].pairs[j];
			const std::wstring all = Lower (Wide (Corrections::JoinSources (rules[i].sources) + " " + p.wrong + " " + p.correct));
			if (filter.empty () || all.find (filter) != std::wstring::npos) {
				Row r;
				r.rule = i;
				r.pair = j;
				r.count = CountRow (s, r);
				s.rows.push_back (r);
			}
		}
	}
	ListView_SetItemCountEx (s.rules, static_cast<int> (s.rows.size ()), LVSICF_NOINVALIDATEALL);
	InvalidateRect (s.rules, nullptr, FALSE);
	const std::wstring title = Wide (TT::TxtF (TT::C_RULES, s.ui, { { "n", std::to_string (s.rows.size ()) } }));
	SetWindowTextW (s.rulesTitle, title.c_str ());
}

// Rebuilds the visible cache entries (search filter on original and translation).
void BuildCacheRows (State& s)
{
	const std::wstring filter = Lower (GetText (s.search));
	s.cacheRows.clear ();
	for (size_t i = 0; i < s.cache.size (); ++i) {
		const std::wstring all = Lower (Wide (s.cache[i].original + " " + s.cache[i].translated));
		if (filter.empty () || all.find (filter) != std::wstring::npos)
			s.cacheRows.push_back (i);
	}
	ListView_SetItemCountEx (s.cacheList, static_cast<int> (s.cacheRows.size ()), LVSICF_NOINVALIDATEALL);
	InvalidateRect (s.cacheList, nullptr, FALSE);
}

// Window title: "Tapir Translator - Term corrections" plus "*" when there are unsaved changes.
void UpdateTitle (State& s)
{
	const std::wstring title = L"Tapir Translator - " + s.T (TT::C_TITLE) + (s.dirty ? L" *" : L"");
	SetWindowTextW (s.window, title.c_str ());
}

// Index of the selected row of a list view, or -1.
int Selected (HWND list)
{
	return ListView_GetNextItem (list, -1, LVNI_SELECTED);
}

// Selects (and shows) row 'i' of a list view.
void Select (HWND list, int i)
{
	ListView_SetItemState (list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	if (i >= 0) {
		ListView_SetItemState (list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible (list, i, FALSE);
	}
}

// Before / After preview of the selected rule: the first cached translation it changes, raw
// and with every rule applied.
void UpdatePreview (State& s)
{
	std::wstring before = L"\x2014", after = L"\x2014";
	const int i = Selected (s.rules);
	if (i >= 0 && static_cast<size_t> (i) < s.rows.size ()) {
		const auto one = SingleRule (s, s.rows[static_cast<size_t> (i)]);
		for (const auto& c : s.cache) {
			if (!one[0].pairs[0].wrong.empty () && Corrections::Apply (one, s.output, c.original, c.translated) != c.translated) {
				before = Wide (c.translated);
				after = Wide (Corrections::Apply (s.Rules (), s.output, c.original, c.translated));
				break;
			}
		}
	}
	SetWindowTextW (s.beforeText, before.c_str ());
	SetWindowTextW (s.afterText, after.c_str ());
}

// Fills the editor with the selected rule (or empties and disables it).
void FillEditor (State& s)
{
	s.updating = true;
	const int i = Selected (s.rules);
	const bool any = i >= 0 && static_cast<size_t> (i) < s.rows.size ();
	if (any) {
		const Row& r = s.rows[static_cast<size_t> (i)];
		const auto& rule = s.Rules ()[r.rule];
		SetWindowTextW (s.sources, Wide (Corrections::JoinSources (rule.sources)).c_str ());
		SetWindowTextW (s.wrong, Wide (rule.pairs[r.pair].wrong).c_str ());
		SetWindowTextW (s.correct, Wide (rule.pairs[r.pair].correct).c_str ());
	} else {
		SetWindowTextW (s.sources, L"");
		SetWindowTextW (s.wrong, L"");
		SetWindowTextW (s.correct, L"");
	}
	for (HWND h : { s.sources, s.wrong, s.correct, s.duplicate, s.remove })
		EnableWindow (h, any);
	s.updating = false;
	UpdatePreview (s);
}

// Shows the rules and the cache of output language 'code'.
void ShowLanguage (State& s, const std::string& code)
{
	s.output = code;
	s.cache = Corrections::ReadCache (s.root, code);
	BuildRows (s);
	BuildCacheRows (s);
	Select (s.rules, s.rows.empty () ? -1 : 0);
	FillEditor (s);
	SetWindowTextW (s.sample, L"");
}

// Something changed in the rules: counts, lists, preview and title follow.
void RulesChanged (State& s)
{
	s.dirty = true;
	const int i = Selected (s.rules);
	BuildRows (s);
	Select (s.rules, std::min (i, static_cast<int> (s.rows.size ()) - 1));
	InvalidateRect (s.cacheList, nullptr, FALSE);	// the "after corrections" column
	UpdatePreview (s);
	UpdateTitle (s);
}

// Adds a new rule (one pair) for the current language and selects it; the search filter is
// cleared so that the new row is visible.
void AddRule (State& s, const std::vector<std::string>& sources, const std::string& wrong, const std::string& correct)
{
	Corrections::Rule r;
	r.sources = sources;
	r.pairs.push_back ({ wrong, correct });
	s.Rules ().push_back (r);
	s.updating = true;
	SetWindowTextW (s.search, L"");
	s.updating = false;
	BuildCacheRows (s);
	RulesChanged (s);
	Select (s.rules, static_cast<int> (s.rows.size ()) - 1);
	FillEditor (s);
}

// Pairs with an empty "wrong" term do nothing: they are dropped before saving.
void Purge (State& s)
{
	for (auto& language : s.file.languages) {
		for (auto& r : language.second)
			r.pairs.erase (std::remove_if (r.pairs.begin (), r.pairs.end (), [] (const Corrections::Pair& p) { return p.wrong.empty (); }),
						   r.pairs.end ());
		language.second.erase (std::remove_if (language.second.begin (), language.second.end (),
											   [] (const Corrections::Rule& r) { return r.pairs.empty (); }),
							   language.second.end ());
	}
}

// Saves corrections.json; on failure shows the reason and returns false.
bool SaveFile (State& s)
{
	Purge (s);
	std::string error;
	if (!Corrections::Save (s.path, s.file, error)) {
		MessageBoxW (s.window, (s.T (TT::C_SAVE_ERROR) + Wide (error)).c_str (), L"Tapir Translator", MB_ICONERROR | MB_OK);
		return false;
	}
	s.dirty = false;
	UpdateTitle (s);
	return true;
}

// Close request (Cancel, Escape, title-bar close): asks about unsaved changes first.
void RequestClose (State& s)
{
	if (s.dirty && !s.autoClose) {
		const int answer = MessageBoxW (s.window, s.T (TT::C_UNSAVED).c_str (), L"Tapir Translator", MB_ICONQUESTION | MB_YESNOCANCEL);
		if (answer == IDCANCEL || (answer == IDYES && !SaveFile (s)))
			return;
	}
	DestroyWindow (s.window);
}

// --- layout -------------------------------------------------------------------------------------

// Places every control for a client area of w x h pixels.
void Layout (State& s, int w, int h)
{
	const int m = s.E (12), line = s.E (24), field = s.E (26), button = s.E (30), gap = s.E (8);
	HDWP d = BeginDeferWindowPos (40);
	const auto Place = [&d] (HWND c, int x, int y, int cw, int ch) {
		d = DeferWindowPos (d, c, nullptr, x, y, std::max (cw, 0), std::max (ch, 0), SWP_NOZORDER | SWP_NOACTIVATE);
	};
	// Top bar: output language and search.
	int y = m;
	Place (s.outLabel, m, y + s.E (4), s.E (120), line);
	Place (s.language, m + s.E (124), y, s.E (200), s.E (300));
	Place (s.searchLabel, m + s.E (340), y + s.E (4), s.E (90), line);
	Place (s.search, m + s.E (434), y, w - m - (m + s.E (434)), field);
	// Bottom bar: status text and buttons.
	const int bottom = h - m - button;
	Place (s.live, m, bottom + s.E (6), w - 3 * m - 2 * s.E (120), line);
	Place (s.cancel, w - m - s.E (120), bottom, s.E (120), button);
	Place (s.save, w - m - 2 * s.E (120) - gap, bottom, s.E (120), button);
	// Body: rules on the left (55 %), cache on the right.
	const int top = y + field + m, bodyBottom = bottom - m;
	const int lw = (w - 3 * m) * 55 / 100, rx = m + lw + m, rw = w - rx - m;
	// Left column, from the bottom up: hint, three fields, editor title, buttons, list.
	int ly = bodyBottom - s.E (40);
	Place (s.hint, m, ly, lw, s.E (40));
	const int labelW = s.E (170);
	ly -= field + gap;
	Place (s.correctLabel, m, ly + s.E (4), labelW, line);
	Place (s.correct, m + labelW, ly, lw - labelW, field);
	ly -= field + gap;
	Place (s.wrongLabel, m, ly + s.E (4), labelW, line);
	Place (s.wrong, m + labelW, ly, lw - labelW, field);
	ly -= field + gap;
	Place (s.srcLabel, m, ly + s.E (4), labelW, line);
	Place (s.sources, m + labelW, ly, lw - labelW, field);
	ly -= line + gap;
	Place (s.selTitle, m, ly, lw, line);
	ly -= button + gap * 2;
	Place (s.add, m, ly, s.E (110), button);
	Place (s.duplicate, m + s.E (110) + gap, ly, s.E (110), button);
	Place (s.remove, m + 2 * (s.E (110) + gap), ly, s.E (110), button);
	Place (s.rulesTitle, m, top, lw, line);
	Place (s.rules, m, top + line, lw, ly - gap - (top + line));
	// Right column, from the bottom up: preview, button, sample, list.
	int ry = bodyBottom - line;
	Place (s.afterLabel, rx, ry, s.E (70), line);
	Place (s.afterText, rx + s.E (74), ry, rw - s.E (74), line);
	ry -= line;
	Place (s.beforeLabel, rx, ry, s.E (70), line);
	Place (s.beforeText, rx + s.E (74), ry, rw - s.E (74), line);
	ry -= button + gap;
	Place (s.correctWord, rx + rw - s.E (200), ry, s.E (200), button);
	ry -= s.E (52) + gap;
	Place (s.sample, rx, ry, rw, s.E (52));
	Place (s.cacheTitle, rx, top, rw, line);
	Place (s.cacheHint, rx, top + line, rw, s.E (36));
	Place (s.cacheList, rx, top + line + s.E (36), rw, ry - gap - (top + line + s.E (36)));
	EndDeferWindowPos (d);
	// List columns follow the widths.
	const int lc = lw - GetSystemMetrics (SM_CXVSCROLL) - s.E (4);
	ListView_SetColumnWidth (s.rules, 0, lc * 32 / 100);
	ListView_SetColumnWidth (s.rules, 1, lc * 26 / 100);
	ListView_SetColumnWidth (s.rules, 2, lc * 26 / 100);
	ListView_SetColumnWidth (s.rules, 3, lc - lc * 84 / 100);
	const int rc = rw - GetSystemMetrics (SM_CXVSCROLL) - s.E (4);
	ListView_SetColumnWidth (s.cacheList, 0, rc / 3);
	ListView_SetColumnWidth (s.cacheList, 1, rc / 3);
	ListView_SetColumnWidth (s.cacheList, 2, rc - 2 * (rc / 3));
}

// --- creation -------------------------------------------------------------------------------------

HWND Control (State& s, const wchar_t* cls, const std::wstring& text, DWORD style, int id, DWORD exStyle = 0, bool bold = false)
{
	HWND h = CreateWindowExW (exStyle, cls, text.c_str (), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, s.window,
							  reinterpret_cast<HMENU> (static_cast<INT_PTR> (id)), GetModuleHandleW (nullptr), nullptr);
	SendMessageW (h, WM_SETFONT, reinterpret_cast<WPARAM> (bold ? s.bold : s.font), TRUE);
	return h;
}

HWND List (State& s, int id, std::initializer_list<TT::Text> columns)
{
	HWND h = Control (s, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA, id,
					  WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle (h, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
	int i = 0;
	for (TT::Text t : columns) {
		const std::wstring title = s.T (t);
		LVCOLUMNW c = {};
		c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		c.pszText = const_cast<wchar_t*> (title.c_str ());
		c.cx = s.E (100);
		c.iSubItem = i;
		ListView_InsertColumn (h, i++, &c);
	}
	return h;
}

void Create (State& s)
{
	const DWORD label = SS_NOPREFIX, edit = WS_TABSTOP | ES_AUTOHSCROLL, button = WS_TABSTOP | BS_PUSHBUTTON;
	s.outLabel = Control (s, L"STATIC", s.T (TT::M_OUTPUT), label, -1);
	s.language = Control (s, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kLanguage);
	for (int i = 0; i < TT::InterfaceLanguageCount; ++i) {
		const std::wstring name = Wide (std::string (TT::InterfaceName (i)) + " (" + TT::InterfaceCode (i) + ")");
		SendMessageW (s.language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM> (name.c_str ()));
	}
	s.searchLabel = Control (s, L"STATIC", s.T (TT::C_SEARCH), label | SS_RIGHT, -1);
	s.search = Control (s, L"EDIT", L"", edit, kSearch, WS_EX_CLIENTEDGE);
	s.rulesTitle = Control (s, L"STATIC", L"", label, -1, 0, true);
	s.rules = List (s, kRules, { TT::C_COL_SOURCE, TT::C_COL_WRONG, TT::C_COL_CORRECT, TT::C_COL_COUNT });
	s.add = Control (s, L"BUTTON", s.T (TT::C_ADD), button, kAdd);
	s.duplicate = Control (s, L"BUTTON", s.T (TT::C_DUPLICATE), button, kDuplicate);
	s.remove = Control (s, L"BUTTON", s.T (TT::C_DELETE), button, kDelete);
	s.selTitle = Control (s, L"STATIC", s.T (TT::C_SELECTED), label, -1, 0, true);
	s.srcLabel = Control (s, L"STATIC", s.T (TT::C_COL_SOURCE), label, -1);
	s.sources = Control (s, L"EDIT", L"", edit, kSources, WS_EX_CLIENTEDGE);
	s.wrongLabel = Control (s, L"STATIC", s.T (TT::C_COL_WRONG), label, -1);
	s.wrong = Control (s, L"EDIT", L"", edit, kWrong, WS_EX_CLIENTEDGE);
	s.correctLabel = Control (s, L"STATIC", s.T (TT::C_COL_CORRECT), label, -1);
	s.correct = Control (s, L"EDIT", L"", edit, kCorrect, WS_EX_CLIENTEDGE);
	s.hint = Control (s, L"STATIC", s.T (TT::C_HINT), label, -1);
	s.cacheTitle = Control (s, L"STATIC", s.T (TT::C_CACHE), label, -1, 0, true);
	s.cacheHint = Control (s, L"STATIC", s.T (TT::C_CACHE_HINT), label, -1);
	s.cacheList = List (s, kCache, { TT::C_COL_ORIGINAL, TT::C_COL_ONLINE, TT::C_COL_AFTER });
	s.sample = Control (s, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kSample, WS_EX_CLIENTEDGE);
	s.correctWord = Control (s, L"BUTTON", s.T (TT::C_CORRECT_WORD), button, kCorrectWord);
	s.beforeLabel = Control (s, L"STATIC", s.T (TT::C_BEFORE), label, -1);
	s.beforeText = Control (s, L"STATIC", L"", label | SS_ENDELLIPSIS, -1);
	s.afterLabel = Control (s, L"STATIC", s.T (TT::C_AFTER), label, -1);
	s.afterText = Control (s, L"STATIC", L"", label | SS_ENDELLIPSIS, -1, 0, true);
	s.live = Control (s, L"STATIC", s.T (TT::C_LIVE), label, -1);
	s.cancel = Control (s, L"BUTTON", s.T (TT::C_CANCEL), button, kCancel);
	s.save = Control (s, L"BUTTON", Wide (TT::Txt (TT::D_SAVE, s.ui)), WS_TABSTOP | BS_DEFPUSHBUTTON, kSave);
}

// --- messages -------------------------------------------------------------------------------------

// Text of one cell of a virtual list (LVN_GETDISPINFO).
void CellText (State& s, NMLVDISPINFOW* info)
{
	if ((info->item.mask & LVIF_TEXT) == 0)
		return;
	const size_t i = static_cast<size_t> (info->item.iItem);
	s.scratch.clear ();
	if (info->hdr.hwndFrom == s.rules && i < s.rows.size ()) {
		const Row& r = s.rows[i];
		const auto& rule = s.Rules ()[r.rule];
		switch (info->item.iSubItem) {
			case 0: s.scratch = rule.sources.empty () ? s.T (TT::C_ALL) : Wide (Corrections::JoinSources (rule.sources)); break;
			case 1: s.scratch = Wide (rule.pairs[r.pair].wrong); break;
			case 2: s.scratch = Wide (rule.pairs[r.pair].correct); break;
			default: s.scratch = std::to_wstring (r.count); break;
		}
	} else if (info->hdr.hwndFrom == s.cacheList && i < s.cacheRows.size ()) {
		const auto& c = s.cache[s.cacheRows[i]];
		switch (info->item.iSubItem) {
			case 0: s.scratch = Wide (c.original); break;
			case 1: s.scratch = Wide (c.translated); break;
			default: {
				// Empty when the corrections change nothing (the online text is kept as is).
				const std::string after = Corrections::Apply (s.Rules (), s.output, c.original, c.translated);
				s.scratch = after == c.translated ? std::wstring () : Wide (after);
				break;
			}
		}
	}
	info->item.pszText = const_cast<wchar_t*> (s.scratch.c_str ());
}

// "Correct this word": the text highlighted in the sample (or the word under the caret)
// becomes the "wrong" term of a new rule; the original's words are proposed as sources only
// when the original is a single word.
void CorrectWord (State& s)
{
	const int i = Selected (s.cacheList);
	if (i < 0 || static_cast<size_t> (i) >= s.cacheRows.size ()) {
		MessageBeep (MB_ICONASTERISK);
		return;
	}
	const auto& c = s.cache[s.cacheRows[static_cast<size_t> (i)]];
	const std::wstring text = GetText (s.sample);
	DWORD start = 0, end = 0;
	SendMessageW (s.sample, EM_GETSEL, reinterpret_cast<WPARAM> (&start), reinterpret_cast<LPARAM> (&end));
	// No selection: extend the caret position to the surrounding word.
	if (start == end) {
		while (start > 0 && IsCharAlphaNumericW (text[start - 1]))
			--start;
		while (end < text.size () && IsCharAlphaNumericW (text[end]))
			++end;
	}
	std::wstring word = text.substr (start, end - start);
	while (!word.empty () && word.back () == L' ')
		word.pop_back ();
	while (!word.empty () && word.front () == L' ')
		word.erase (0, 1);
	if (word.empty ()) {
		MessageBeep (MB_ICONASTERISK);
		return;
	}
	std::vector<std::string> sources;
	if (c.original.find (' ') == std::string::npos)
		sources.push_back (c.original);
	AddRule (s, sources, Narrow (word), Narrow (word));
	SetFocus (s.correct);
	SendMessageW (s.correct, EM_SETSEL, 0, -1);
}

LRESULT CALLBACK Procedure (HWND window, UINT message, WPARAM w, LPARAM l)
{
	State* s = reinterpret_cast<State*> (GetWindowLongPtrW (window, GWLP_USERDATA));
	if (s == nullptr)
		return DefWindowProcW (window, message, w, l);
	switch (message) {
		case WM_SIZE:
			Layout (*s, LOWORD (l), HIWORD (l));
			return 0;
		case WM_GETMINMAXINFO: {
			MINMAXINFO* info = reinterpret_cast<MINMAXINFO*> (l);
			info->ptMinTrackSize.x = s->E (860);
			info->ptMinTrackSize.y = s->E (560);
			return 0;
		}
		case WM_CTLCOLORSTATIC:
			// Read-only sample edit: white like an input field.
			if (reinterpret_cast<HWND> (l) == s->sample) {
				SetBkColor (reinterpret_cast<HDC> (w), GetSysColor (COLOR_WINDOW));
				return reinterpret_cast<LRESULT> (GetSysColorBrush (COLOR_WINDOW));
			}
			break;
		case WM_NOTIFY: {
			NMHDR* h = reinterpret_cast<NMHDR*> (l);
			if (h->code == LVN_GETDISPINFOW) {
				CellText (*s, reinterpret_cast<NMLVDISPINFOW*> (l));
				return 0;
			}
			if (h->code == LVN_ITEMCHANGED) {
				NMLISTVIEW* v = reinterpret_cast<NMLISTVIEW*> (l);
				if ((v->uChanged & LVIF_STATE) != 0 && ((v->uNewState ^ v->uOldState) & LVIS_SELECTED) != 0) {
					if (h->hwndFrom == s->rules && !s->updating)
						FillEditor (*s);
					if (h->hwndFrom == s->cacheList) {
						const int i = Selected (s->cacheList);
						const std::wstring t = i >= 0 && static_cast<size_t> (i) < s->cacheRows.size ()
							? Wide (s->cache[s->cacheRows[static_cast<size_t> (i)]].translated) : std::wstring ();
						SetWindowTextW (s->sample, t.c_str ());
					}
				}
				return 0;
			}
			break;
		}
		case WM_COMMAND: {
			const int id = LOWORD (w), code = HIWORD (w);
			if (id == kLanguage && code == CBN_SELCHANGE) {
				const int i = static_cast<int> (SendMessageW (s->language, CB_GETCURSEL, 0, 0));
				if (i >= 0)
					ShowLanguage (*s, TT::InterfaceCode (i));
			} else if (id == kSearch && code == EN_CHANGE && !s->updating) {
				BuildRows (*s);
				BuildCacheRows (*s);
				Select (s->rules, s->rows.empty () ? -1 : 0);
				FillEditor (*s);
			} else if ((id == kSources || id == kWrong || id == kCorrect) && code == EN_CHANGE && !s->updating) {
				// Editing the selected row updates the model at once.
				const int i = Selected (s->rules);
				if (i >= 0 && static_cast<size_t> (i) < s->rows.size ()) {
					const Row r = s->rows[static_cast<size_t> (i)];
					auto& rule = s->Rules ()[r.rule];
					if (id == kSources)
						rule.sources = Corrections::SplitSources (Narrow (GetText (s->sources)));
					else if (id == kWrong)
						rule.pairs[r.pair].wrong = Narrow (GetText (s->wrong));
					else
						rule.pairs[r.pair].correct = Narrow (GetText (s->correct));
					s->updating = true;
					RulesChanged (*s);
					s->updating = false;
				}
			} else if (id == kAdd) {
				AddRule (*s, {}, std::string (), std::string ());
				SetFocus (s->wrong);
			} else if (id == kDuplicate) {
				const int i = Selected (s->rules);
				if (i >= 0 && static_cast<size_t> (i) < s->rows.size ()) {
					const Row r = s->rows[static_cast<size_t> (i)];
					const auto rule = s->Rules ()[r.rule];
					AddRule (*s, rule.sources, rule.pairs[r.pair].wrong, rule.pairs[r.pair].correct);
					SetFocus (s->wrong);
				}
			} else if (id == kDelete) {
				const int i = Selected (s->rules);
				if (i >= 0 && static_cast<size_t> (i) < s->rows.size ()) {
					const Row r = s->rows[static_cast<size_t> (i)];
					auto& rules = s->Rules ();
					rules[r.rule].pairs.erase (rules[r.rule].pairs.begin () + static_cast<long> (r.pair));
					if (rules[r.rule].pairs.empty ())
						rules.erase (rules.begin () + static_cast<long> (r.rule));
					RulesChanged (*s);
					FillEditor (*s);
				}
			} else if (id == kCorrectWord) {
				CorrectWord (*s);
			} else if (id == kSave) {
				if (SaveFile (*s))
					DestroyWindow (window);
			} else if (id == kCancel) {
				RequestClose (*s);
			}
			return 0;
		}
		case WM_TIMER:
			if (w == kAutoClose)
				DestroyWindow (window);
			return 0;
		case WM_CLOSE:
			RequestClose (*s);
			return 0;
		case WM_DESTROY:
			PostQuitMessage (0);
			return 0;
		default:
			break;
	}
	return DefWindowProcW (window, message, w, l);
}

}	// namespace


int ShowCorrectionsWindow (const std::wstring& path, const std::string& output, int uiLanguage, int autoCloseMs)
{
	// Step 1: only one window: a second start brings the first one to the front.
	if (HWND existing = FindWindowW (kClass, nullptr)) {
		if (IsIconic (existing))
			ShowWindow (existing, SW_RESTORE);
		SetForegroundWindow (existing);
		return 0;
	}
	// Step 2: DPI awareness, common controls, window class.
	SetProcessDPIAware ();
	INITCOMMONCONTROLSEX icc = { sizeof (icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
	InitCommonControlsEx (&icc);
	HINSTANCE instance = GetModuleHandleW (nullptr);
	WNDCLASSW class_ = {};
	class_.lpfnWndProc = Procedure;
	class_.hInstance = instance;
	class_.hCursor = LoadCursor (nullptr, IDC_ARROW);
	class_.hbrBackground = reinterpret_cast<HBRUSH> (COLOR_BTNFACE + 1);
	class_.hIcon = LoadIcon (nullptr, IDI_APPLICATION);
	class_.lpszClassName = kClass;
	RegisterClassW (&class_);

	// Step 3: state and fonts (Windows message font, and its semi-bold variant for titles).
	State s;
	s.path = path;
	s.root = path.substr (0, path.find_last_of (L"\\/"));
	s.ui = uiLanguage;
	s.autoClose = autoCloseMs > 0;
	HDC screen = GetDC (nullptr);
	s.dpi = GetDeviceCaps (screen, LOGPIXELSY);
	ReleaseDC (nullptr, screen);
	NONCLIENTMETRICSW metrics = {};
	metrics.cbSize = sizeof (metrics);
	SystemParametersInfoW (SPI_GETNONCLIENTMETRICS, sizeof (metrics), &metrics, 0);
	s.font = CreateFontIndirectW (&metrics.lfMessageFont);
	LOGFONTW boldFont = metrics.lfMessageFont;
	boldFont.lfWeight = FW_SEMIBOLD;
	s.bold = CreateFontIndirectW (&boldFont);
	std::string error;
	const bool readable = Corrections::Load (path, s.file, error);

	// Step 4: the window, 1000 x 680 (96 DPI) centred on the screen, then its controls.
	const int width = s.E (1000), height = s.E (680);
	RECT zone = { 0, 0, width, height };
	AdjustWindowRectEx (&zone, WS_OVERLAPPEDWINDOW, FALSE, 0);
	const int fw = zone.right - zone.left, fh = zone.bottom - zone.top;
	s.window = CreateWindowExW (0, kClass, L"Tapir Translator", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
								(GetSystemMetrics (SM_CXSCREEN) - fw) / 2, (GetSystemMetrics (SM_CYSCREEN) - fh) / 2, fw, fh,
								nullptr, nullptr, instance, nullptr);
	if (s.window == nullptr) {
		DeleteObject (s.font);
		DeleteObject (s.bold);
		return 1;
	}
	SetWindowLongPtrW (s.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (&s));
	Create (s);
	UpdateTitle (s);

	// Step 5: first output language: the one asked for, else the first of the file, else fr.
	std::string first = output;
	if (first.empty ())
		first = s.file.languages.empty () ? std::string ("fr") : s.file.languages.begin ()->first;
	int index = 0;
	for (int i = 0; i < TT::InterfaceLanguageCount; ++i) {
		if (first == TT::InterfaceCode (i))
			index = i;
	}
	SendMessageW (s.language, CB_SETCURSEL, static_cast<WPARAM> (index), 0);
	RECT client;
	GetClientRect (s.window, &client);
	Layout (s, client.right, client.bottom);
	ShowLanguage (s, TT::InterfaceCode (index));

	// Step 6: show it in front; an unreadable file is reported (and not overwritten unless
	// the user saves).
	ShowWindow (s.window, SW_SHOWNORMAL);
	SetForegroundWindow (s.window);
	SetFocus (s.rules);	// the keyboard works at once (arrows, Tab to the editor)
	if (s.autoClose)
		SetTimer (s.window, kAutoClose, static_cast<UINT> (autoCloseMs), nullptr);
	else if (!readable)
		MessageBoxW (s.window, s.T (TT::C_UNREADABLE).c_str (), L"Tapir Translator", MB_ICONWARNING | MB_OK);

	// Step 7: message loop; IsDialogMessage gives Tab navigation, Enter (Save) and Escape (Cancel).
	MSG msg;
	while (GetMessageW (&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW (s.window, &msg)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
	}
	DeleteObject (s.font);
	DeleteObject (s.bold);
	UnregisterClassW (kClass, instance);
	return 0;
}
