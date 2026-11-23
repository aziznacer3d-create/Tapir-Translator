// *****************************************************************************
// Tapir Translator - ArchiCAD-independent core (Win32 + standard library only)
//
//   ROLE IN THE SYSTEM
//   Everything the add-on needs that does not involve the ArchiCAD API:
//   language and translator tables, settings file (settings.json), user files
//   (keys.json, corrections.json, _user.json), extraction of the translation
//   layer and of the common dictionary, preparation of the PYTHONPATH, and
//   small file helpers.
//
//   It is shared by the add-on (Src/AddOnMain.cpp) and by the test bench
//   (Tests/TranslatorBench.cpp): the bench exercises exactly the code that writes
//   the layer, the settings and the environment inside ArchiCAD.
//
//   It only relies on header-only parts of the STL (std::string / std::wstring)
//   and on the Win32 API: the add-on is compiled with /Zc:wchar_t- (wchar_t is
//   not a native type), so compiled STL functions taking a wchar_t would not link.
// *****************************************************************************

#ifndef TT_CORE_HPP
#define TT_CORE_HPP

#include <string>
#include <vector>

namespace TT {

// Version of the add-on (shown in the Add-On Manager, the Session Report and settings.json).
extern const char* const AddOnVersion;

// One language of the menus: its code (as used in dictionaries and settings.json) and its
// name written in that language.
struct Language {
	const char* code;	// "" = the script's own language (automatic / no translation / as ArchiCAD)
	const char* name;
};
// The 21 entries: "" first, then the 20 ArchiCAD 29 languages (see TT_Core.cpp).
extern const Language Languages[];
extern const int LanguageCount;

// In-memory image of settings.json, the file shared by the add-on (writer) and the Python layer
// (reader, live). Default values are those used when the file or a key is missing.
struct Settings {
	std::string input;		// language the scripts are written in, code of Languages[], "" = automatic detection
	std::string language;		// output language, code of Languages[], "" = no translation
	bool collect = false;	// record the texts that have no translation (missing folder)
	bool engine = false;	// online translation of the texts missing from the dictionaries
	std::string provider = "mymemory";	// code of Providers[]
	std::string interface;	// language of the tool itself (code of Languages[]); "" = ArchiCAD's language
};

// Online translators (same order as the "Online translator" menu, MenuBase + 3, generated
// into RFIX/Tapir_TranslatorMenus.grc).
struct Provider {
	const char* code;		// identifier in settings.json and keys.json
	const char* name;		// display name
	const char* need;		// what has to be filled in keys.json
};
extern const Provider Providers[];
extern const int ProviderCount;
// True for the codes of Providers[], plus "test" (test bench) and "local" (offline build option).
bool KnownProvider (const std::string& code);

// TCP port of the local translation engine (127.0.0.1).
constexpr int EnginePort = 8779;

// True if a server accepts TCP connections on 127.0.0.1:<port> within timeoutMs. Used by the
// add-on's watchdog: when another ArchiCAD instance already runs an engine on EnginePort, that
// engine is used instead of restarting ours in a loop.
bool PortAnswers (int port, int timeoutMs);

// keys.json: credentials of the online translators. KeysPath returns <root>\keys.json;
// CreateKeysTemplate writes an empty template if the file does not exist (never overwritten).
std::wstring KeysPath (const std::wstring& root);
bool CreateKeysTemplate (const std::wstring& root, std::wstring& error);

// corrections.json: terms to correct in online translations ("story" -> "étage", not
// "histoire"). CreateCorrectionsTemplate writes a pre-filled file if none exists (never overwritten).
std::wstring CorrectionsPath (const std::wstring& root);
bool CreateCorrectionsTemplate (const std::wstring& root, std::wstring& error);

// Working folder %APPDATA%\Tapir Translator ("" if APPDATA does not exist).
std::wstring DefaultFolder ();

// Small file helpers (wide paths, Win32).
// Join: joins a folder and a name with a backslash (unless the folder already ends with one).
std::wstring Join (const std::wstring& folder, const std::wstring& name);
bool		 FileExists (const std::wstring& path);	// existing file (not a folder)
bool		 FolderExists (const std::wstring& path);	// existing folder
bool		 CreateFolder (const std::wstring& path);	// creates the parents too
bool		 ReadWholeFile (const std::wstring& path, std::string& content);	// whole file, raw bytes
// Atomic write (temporary .tmp file, then replacement): the Python layer, which re-reads the
// files live, never sees a half-written file. 'error' receives a readable message on failure.
bool		 WriteWholeFile (const std::wstring& path, const std::string& content, std::wstring& error);

// UTF-16 <-> UTF-8 conversions (empty string on failure).
std::string	 Utf8 (const std::wstring& text);
std::wstring FromUtf8 (const std::string& text);

// Installs the translation layer and the data files under 'root':
//   - layer\sitecustomize.py: always the add-on's version (rewritten when it differs);
//   - dictionaries\_common.json: the add-on's version too (rewritten when it differs);
//   - dictionaries\_user.json, keys.json, corrections.json: created only if missing.
// 'journal' receives one line per action. Returns false if a folder or a mandatory file failed.
bool ExtractLayer (const std::wstring& root, std::vector<std::wstring>& journal);

// Converts, once, the data written by versions up to 0.7.5 (French file names and JSON keys) to
// the current names: reglages.json -> settings.json, cles.json -> keys.json, journal.txt ->
// log.txt, dictionnaires\ -> dictionaries\ (_perso.json -> _user.json), manquants\ -> missing\,
// and the old keys inside settings, keys, corrections, dictionaries, missing texts and cache.
// The old layer folder (couche\) is deleted. Does nothing when nothing old is left; an item
// whose new name already exists is left alone. 'journal' receives one line per conversion.
void MigrateOldData (const std::wstring& root, std::vector<std::wstring>& journal);

// Reads / writes <root>\settings.json. ReadSettings never fails: missing or invalid values
// keep their defaults. WriteSettings refuses unknown language codes.
Settings ReadSettings (const std::wstring& root);
bool	 WriteSettings (const std::wstring& root, const Settings& settings, std::wstring& error);

// Puts <root>\layer at the head of the PYTHONPATH of the current process (ArchiCAD) and sets
// TAPIR_TRANSLATOR_DIR. Tapir keeps this PYTHONPATH for every script it launches (it prepends
// its own helpers), so Python imports the layer by itself. 'pythonPath' receives the new value.
bool PrepareEnvironment (const std::wstring& root, std::wstring& pythonPath);

// Value of an environment variable of the current process ("" if it is not set).
std::wstring GetEnvVar (const wchar_t* name);

// Names of the .json files of a folder (no path).
std::vector<std::wstring> ListJson (const std::wstring& folder);

// Deletes a folder and all its content, recursively (no confirmation).
void DeleteFolder (const std::wstring& folder);

// Names of the sub-folders of a folder (no path).
std::vector<std::wstring> ListFolders (const std::wstring& folder);

// Number of texts still empty ("text": "") in a file of the missing folder.
int CountEmpty (const std::string& json);

// The last 'number' non-empty lines of a text file.
std::vector<std::string> LastLines (const std::wstring& path, int number);

}	// namespace TT

#endif
