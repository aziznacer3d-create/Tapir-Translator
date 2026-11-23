// *****************************************************************************
// Tapir Translator - engine: model of the term corrections (corrections.json)
//
//   ROLE IN THE SYSTEM
//   Online translators know the Archicad vocabulary poorly ("story" becomes
//   "histoire" instead of "étage" in French). corrections.json holds, per output
//   language, rules that fix their translations; the Python layer applies them
//   live (layer/sitecustomize.py, _compile_corrections / apply_corrections).
//   This module is the C++ side used by the corrections window
//   (CorrectionsWindow.cpp): it reads and writes the file, and applies the rules
//   with exactly the layer's semantics so that the window can preview them.
//
//   File format (unchanged, still editable by hand):
//     { "_explanation": "...",
//       "fr": [ {"source": ["story", "stories"], "replace": {"histoire": "étage", ...}},
//               {"source": "layer", "wrong": "couche", "correct": "calque"},
//               {"wrong": "Toutes les histoires", "correct": "Tous les étages"} ],
//       "de": [ ... ] }
//   Semantics (same as the layer):
//     - "source" (optional): the rule applies only if the original text contains
//       one of these words (whole words, case-insensitive);
//     - "wrong" -> "correct": whole-word, case-insensitive replacement in the
//       translation; in fr, es and pt a single word not ending with "s" also
//       covers its -s plural; "replace" = several pairs, no automatic plural;
//     - the replacement keeps the case style of the replaced text (ALL CAPS or
//       initial capital); the longest wrong terms are applied first.
// *****************************************************************************

#ifndef TT_ENGINE_CORRECTIONS_HPP
#define TT_ENGINE_CORRECTIONS_HPP

#include <map>
#include <string>
#include <vector>

namespace Corrections {

// One replacement: 'wrong' (as produced by the online translator) -> 'correct'. UTF-8.
struct Pair {
	std::string wrong;
	std::string correct;
};

// One rule of one output language.
//   sources     : words of the original that trigger the rule (empty = every text)
//   pairs       : the replacements (at least one in a valid rule)
//   replaceForm : true = written as "replace": {...} (no automatic plural);
//                 false = written as "wrong"/"correct" (exactly one pair, automatic -s
//                 plural in fr, es and pt)
struct Rule {
	std::vector<std::string> sources;
	std::vector<Pair> pairs;
	bool replaceForm = false;
};

// The whole file: the explanation text and the rules of each output language.
struct File {
	std::string explanation;
	std::map<std::string, std::vector<Rule>> languages;	// output language code -> rules
};

// Reads 'path' into 'file'. A missing file gives an empty File and true; a file that is not
// valid JSON (or not an object) gives false and a message in 'error' (UTF-8).
bool Load (const std::wstring& path, File& file, std::string& error);

// The JSON text of 'file' (UTF-8, one rule per line, "_explanation" first).
std::string Serialize (const File& file);

// Writes 'file' to 'path' (through a temporary file, so the layer never reads half a file).
bool Save (const std::wstring& path, const File& file, std::string& error);

// Applies 'rules' (of output language 'language') to the online translation 'translated' of
// 'original', exactly like the layer. Returns the corrected translation.
std::string Apply (const std::vector<Rule>& rules, const std::string& language, const std::string& original,
				   const std::string& translated);

// One online translation found in the cache.
struct CachedTranslation {
	std::string original;	// source text (as displayed by the script)
	std::string translated;	// raw online translation (before corrections)
	std::string provider;	// cache sub-folder: deepl, microsoft, google, mymemory, libretranslate...
};

// Every cached online translation into 'language' under <root>\cache (all translators, all
// source languages: cache\<translator>\<source>-<language>.json and the oldest layout
// cache\<source>-<language>.json), sorted by original text, duplicates removed.
std::vector<CachedTranslation> ReadCache (const std::wstring& root, const std::string& language);

// Words of a "source" field typed by the user: split on commas, trimmed, empty parts removed.
std::vector<std::string> SplitSources (const std::string& text);

// The sources joined with ", " (for display and editing).
std::string JoinSources (const std::vector<std::string>& sources);

}	// namespace Corrections

#endif
