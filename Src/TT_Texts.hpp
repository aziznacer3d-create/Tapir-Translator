// *****************************************************************************
// Tapir Translator - texts of the tool's own interface, in the 20 ArchiCAD languages
//
//   ROLE IN THE SYSTEM
//   Single source of every user-facing text of the tool itself: menus (turned
//   into resources at build time by Tools/GenerateMenus.cpp), status-bar help
//   strings, the MyMemory quota window of the engine, and Session Report
//   messages. Shared by the add-on, the engine and the menu generator, so it has
//   no dependency.
//
//   Interface language: ArchiCAD's by default (version code FRA, GER, ITA...),
//   or the one chosen in the "Tapir Translator language" menu.
//   Interface index order = TT::Languages[1..20]: fr en de es pt it hu cs pl ru uk
//   el tr nl sv no fi ja ko zh (index 0 = fr ... 19 = zh).
//   Translations still to be reviewed by native speakers.
// *****************************************************************************

#ifndef TT_TEXTS_HPP
#define TT_TEXTS_HPP

#include <initializer_list>
#include <string>
#include <utility>

namespace TT {

// Identifier of each text; also the row index in the table of TT_Texts.cpp (same order).
enum Text : int {
	// menu items (M_)
	M_INPUT, M_AUTO, M_OUTPUT, M_NONE, M_TRANSLATOR, M_NO_KEY, M_KEYS, M_CORRECTIONS,
	M_OPTIONS, M_ONLINE, M_COLLECT, M_FOLDER, M_STATUS, M_INTERFACE, M_INTERFACE_AUTO,
	// status-bar help strings (P_)
	P_AUTO, P_INPUT, P_NONE, P_OUTPUT, P_TRANSLATOR, P_KEYS, P_CORRECTIONS, P_ONLINE,
	P_COLLECT, P_FOLDER, P_STATUS, P_INTERFACE,
	// MyMemory quota dialog shown by the engine (D_)
	D_TITLE, D_ANONYMOUS, D_WITH_EMAIL, D_LABEL, D_SAVE, D_LATER, D_INVALID,
	// Session Report messages (R_)
	R_STARTUP, R_INACTIVE, R_SETTINGS, R_SAME_LANGUAGE, R_YES, R_NO, R_ENGINE_STARTED, R_ENGINE_STOPPED,
	R_ENGINE_NOT_FOUND, R_KEYS_OPENED, R_CORRECTIONS_OPENED, R_INTERFACE, R_KEY_OK, R_KEY_TO_ENTER,
	R_EMAIL_OK, R_NO_KEY, R_ADDRESS_OK, R_ADDRESS_TO_ENTER,
	// term corrections window shown by the engine (C_)
	C_TITLE, C_SEARCH, C_RULES, C_COL_SOURCE, C_COL_WRONG, C_COL_CORRECT, C_COL_COUNT, C_ADD, C_DUPLICATE, C_DELETE,
	C_SELECTED, C_HINT, C_CACHE, C_CACHE_HINT, C_CORRECT_WORD, C_COL_ORIGINAL, C_COL_ONLINE, C_COL_AFTER, C_BEFORE,
	C_AFTER, C_LIVE, C_CANCEL, C_UNSAVED, C_UNREADABLE, C_SAVE_ERROR, C_ALL,
	TextCount	// number of texts (not a text)
};

// Number of interface languages (columns of the table).
constexpr int InterfaceLanguageCount = 20;

// UTF-8 text 't' in the interface language of index 'language' (0 = fr ... 19 = zh). An index out
// of range, or an empty cell, falls back to English; an invalid 't' gives "".
const char* Txt (Text t, int language);

// Same as Txt, then replaces every {key} placeholder by its value:
//   TxtF (R_STARTUP, l, {{"v", "0.7.0"}, {"l", "..."}})
std::string TxtF (Text t, int language, std::initializer_list<std::pair<const char*, std::string>> values);

// Number of empty cells in the table (checked by the test bench: must be 0).
int MissingTexts ();

// Language code (fr, en...) -> interface index; an unknown code (including "") -> English (1).
int InterfaceIndex (const std::string& code);

// Code and name (written in that language) of the interface language of index i
// (out of range -> "en" / "English").
const char* InterfaceCode (int i);
const char* InterfaceName (int i);

// Menu resource IDs: base of the 5 menus of interface language i (+0 input language, +1 output
// language, +2 options, +3 online translator, +4 tool language); help strings are at +300.
// E.g. French menus are 32100..32104 (help 32400..32404), English menus 32110..32114.
// The ArchiCAD API takes resource IDs as 16-bit signed integers (short): every ID must stay
// below 32767, otherwise it wraps to a negative number, menu registration fails and ArchiCAD
// unloads the whole add-on (bug of v0.7.0, whose help strings were at 33500..33694).
constexpr short MenuBase (int i) { return static_cast<short> (32100 + 10 * i); }
constexpr short HelpOffset = 300;
static_assert (32100 + 10 * (InterfaceLanguageCount - 1) + 4 + HelpOffset <= 32767,
			   "menu help resource IDs must fit in a short (ArchiCAD API)");
static_assert (32100 + 10 * (InterfaceLanguageCount - 1) + 4 < 32100 + HelpOffset,
			   "menu IDs and help IDs must not overlap");

// ArchiCAD version code (FRA, GER, INT, JPN...) -> language code (fr, de, en, ja...);
// unknown codes give "en".
std::string LanguageFromArchicad (const std::string& version);

}	// namespace TT

#endif
