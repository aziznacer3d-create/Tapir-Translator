// *****************************************************************************
// Tapir Translator - engine: the term corrections window
//
//   ROLE IN THE SYSTEM
//   Replaces Notepad for corrections.json (menu Online translator > Term
//   corrections...). The add-on starts the engine executable with
//   --corrections <file>: this window runs in that separate process, so
//   ArchiCAD stays usable while it is open, and only one window exists at a
//   time (a second start brings the first one to the front).
//
//   Layout (texts in the tool's 20 languages, TT_Texts C_*):
//     - top: output language (one set of rules per language) and a search box
//       that filters both lists;
//     - left: the rules, one row per replacement ("if the original contains",
//       "wrong", "correct", number of cached translations it changes), Add /
//       Duplicate / Delete, and the editor of the selected rule;
//     - right: the online translations already received (cache), with their
//       text after corrections; highlighting a wrongly translated word in the
//       selected translation and clicking "Correct this word" creates a rule;
//       "Before / After" preview of the selected rule;
//     - bottom: Cancel / Save. Saving rewrites corrections.json, which the
//       Python layer re-reads at once (open windows and cache follow).
//   The rules are applied exactly like the layer (Corrections.hpp).
// *****************************************************************************

#ifndef TT_ENGINE_CORRECTIONS_WINDOW_HPP
#define TT_ENGINE_CORRECTIONS_WINDOW_HPP

#include <string>

// Shows the window and runs its message loop until it is closed.
//   path       : corrections.json (its folder also holds the cache\ folder)
//   output     : output language shown first ("" = the first language of the file, else "fr")
//   uiLanguage : interface language index (TT_Texts.hpp)
//   autoCloseMs: > 0 = close by itself after that delay without any question (test bench)
// Returns 0 when the window was shown (or an existing one brought to the front), 1 when it
// could not be created.
int ShowCorrectionsWindow (const std::wstring& path, const std::string& output, int uiLanguage, int autoCloseMs);

#endif
