// *****************************************************************************
// Tapir Translator - ArchiCAD Add-On entry point (API DevKit AC29 / AC30)
//
//   ROLE IN THE SYSTEM
//   This is the only file that talks to the ArchiCAD API. It translates, live,
//   the tkinter interface of the Python scripts launched by the Tapir palette,
//   WITHOUT modifying those scripts. The pieces fit together like this:
//
//   Step 1. At ArchiCAD start-up (the add-on is preloaded), the add-on writes
//           the translation layer layer\sitecustomize.py (embedded in the
//           .apx, see TT_Embedded.hpp) and the common dictionary into
//           %APPDATA%\Tapir Translator (see TT::ExtractLayer).
//   Step 2. It puts the "layer" folder at the head of ArchiCAD's own
//           PYTHONPATH and sets TAPIR_TRANSLATOR_DIR (TT::PrepareEnvironment).
//           Tapir keeps this PYTHONPATH for every script it launches, so Python
//           imports the layer by itself before the script runs (one single
//           process, no second script).
//   Step 3. The "Tapir Translator" menu writes the chosen languages and options
//           into settings.json; the layer re-reads that file live, so windows
//           that are already open switch language too.
//   Step 4. Optionally ("online translation"), the add-on extracts and starts
//           the local translation engine TapirTranslatorEngine.exe (payload
//           appended to the .apx, see TT_Payload.hpp) and a watchdog thread
//           restarts it if it stops. The layer sends unknown sentences to it.
//
//   Every step is traced in the Session Report ("Tapir Translator : ...").
//   Code that may run outside the main thread (the watchdog) only writes to
//   log.txt, never to the ArchiCAD API.
// *****************************************************************************

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "TT_Payload.hpp"
#include "TT_Core.hpp"
#include "TT_Texts.hpp"
#include "Json.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#include <shellapi.h>

namespace {

// Language of the tool's own interface: ArchiCAD's language (version code FRA, GER...) unless
// another one is chosen in the menu. Both indices below are interface indices (0 = fr, 1 = en ...,
// see TT_Texts.hpp).
// - g_menuLanguage: menus are registered only once, at start-up, in the language of that moment
//   (ArchiCAD fixes them until the next start), so this value never changes afterwards.
// - g_messageLanguage: Session Report messages follow a change immediately.
// g_archicadVersion holds ArchiCAD's version/language code, read in CheckEnvironment.
std::string g_archicadVersion;
int			g_menuLanguage = 1;
int			g_messageLanguage = 1;

// Resource IDs of the five menus. The menus are generated at build time
// (RFIX/Tapir_TranslatorMenus.grc, by Tools/GenerateMenus.cpp) once per interface language:
// menu ID = MenuBase (language) + 0..4, and its status-bar help strings are at + HelpOffset.
// These are functions (not constants) because g_menuLanguage is only known in RegisterInterface.
// First menu registration error (reported by Initialize, since RegisterInterface must not fail).
GSErrCode g_menuError = NoError;
short	  g_failedMenu = 0;
// Result of every menu registration ("32100=0 32101=0 ..."), written to log.txt by Initialize.
std::string g_menuTrace;
// Conversions of old data made before the Session Report is usable (reported by Start).
std::vector<std::wstring> g_migration;

short kMenuInput	  () { return TT::MenuBase (g_menuLanguage) + 0; }	// input language: item i = TT::Languages[i - 1] (1 = automatic detection)
short kMenuOutput	  () { return TT::MenuBase (g_menuLanguage) + 1; }	// output language: item i = TT::Languages[i - 1] (1 = no translation)
short kMenuCommands  () { return TT::MenuBase (g_menuLanguage) + 2; }	// "Options" menu (items listed below)
short kMenuTranslator () { return TT::MenuBase (g_menuLanguage) + 3; }	// online translator: item i = TT::Providers[i - 1]
short kMenuInterface  () { return TT::MenuBase (g_menuLanguage) + 4; }	// tool language: 1 = same as ArchiCAD, i = TT::Languages[i - 1]
// Item indices (1-based) inside the "Options" menu; item 3 is a separator.
constexpr Int32 kAutomatic   = 1;
constexpr Int32 kCollect	   = 2;
constexpr Int32 kOpenFolder = 4;
constexpr Int32 kStatus		   = 5;
// Item indices inside the "Online translator" menu, after the 5 providers and a separator (item 6).
constexpr Int32 kKeys		   = 7;
constexpr Int32 kCorrections   = 8;

// Returns the interface index (0 = fr ... 19 = zh) of the tool's own language.
//   choice: the "interface" setting of settings.json, a language code; "" means "same as
//          ArchiCAD", in which case the code is derived from ArchiCAD's version code.
// Unknown codes fall back to English (see TT::InterfaceIndex).
int ToolLanguage (const std::string& choice)
{
	return TT::InterfaceIndex (choice.empty () ? TT::LanguageFromArchicad (g_archicadVersion) : choice);
}

// Global state of the add-on (main thread only, except the engine variables below).
std::wstring g_root;		// working folder %APPDATA%\Tapir Translator ("" if APPDATA is missing)
TT::Settings g_settings;	// in-memory copy of settings.json, edited by the menu commands
bool		 g_ready = false;	// translation layer and environment are in place

// Engine (TapirTranslatorEngine.exe) and its watchdog: if the process stops while online
// translation is wanted, a background thread restarts it (that thread never calls the
// ArchiCAD API: it logs to log.txt). g_lock protects g_engine, g_engineCommandLine and
// g_launchFolder, which are shared between the main thread and the watchdog.
std::mutex			g_lock;
HANDLE				g_engine = nullptr;	// process handle of the running engine (nullptr = none)
std::wstring		g_engineCommandLine;		// engine command line (reused for restarts)
std::wstring		g_launchFolder;	// engine working directory (the folder of the .exe)
std::atomic<bool>	g_engineWanted { false };	// true while the engine should be kept running
std::thread			g_watchdog;			// watchdog thread (runs Watch)

// Converts a wide (UTF-16) string to an ArchiCAD GS::UniString, going through UTF-8.
GS::UniString Uni (const std::wstring& text)
{
	return GS::UniString (TT::Utf8 (text).c_str (), CC_UTF8);
}

// Converts a UTF-8 std::string to a GS::UniString.
GS::UniString Uni (const std::string& utf8)
{
	return GS::UniString (utf8.c_str (), CC_UTF8);
}

void Log (const std::string& line);	// defined below (log.txt)

// Writes one line to ArchiCAD's Session Report, prefixed with "Tapir Translator : ", so that
// the user can copy the whole trace. Main thread only (ArchiCAD API call).
void Report (const GS::UniString& line)
{
	// Step 1: the "%T" format passes the line through unchanged: accented characters are kept and
	// a '%' inside a file path is not interpreted as a printf directive.
	const GS::UniString text = GS::UniString ("Tapir Translator : ") + line;
	ACAPI_WriteReport ("%T", false, text.ToPrintf ());
	// Step 2: same line in log.txt, so that a start-up problem can be diagnosed from the file
	// without asking the user to copy the Session Report (no-op until the working folder is known).
	Log (line.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// Returns the user-facing text 't' (table in TT_Texts.cpp) in the current message language.
GS::UniString T (TT::Text t)
{
	return Uni (std::string (TT::Txt (t, g_messageLanguage)));
}

// Picks the text of a technical line (diagnostics, system errors), which is not in the 20-language
// table: French when the message language is French (index 0), English otherwise.
const char* FE (const char* fr, const char* en)
{
	return g_messageLanguage == 0 ? fr : en;
}

// Displayable name of a language code, e.g. "Deutsch (de)".
//   code:   language code from TT::Languages ("" = no specific language)
//   ifEmpty: text returned when 'code' is empty (e.g. "automatic detection" or "no translation")
// An unknown code is returned as is.
GS::UniString LanguageName (const std::string& code, TT::Text ifEmpty)
{
	if (code.empty ())
		return T (ifEmpty);
	for (int i = 1; i < TT::LanguageCount; ++i) {
		if (code == TT::Languages[i].code)
			return Uni (std::string (TT::Languages[i].name)) + " (" + Uni (code) + ")";
	}
	return Uni (code);
}

// Summary of the current language pair for the Session Report: "<input> -> <output>".
GS::UniString Languages ()
{
	return LanguageName (g_settings.input, TT::M_AUTO) + " -> " + LanguageName (g_settings.language, TT::M_NONE);
}

// Sets or clears the check mark of one menu item.
//   menu:  menu resource ID (one of the kMenu... functions)
//   index: 1-based item index inside that menu
//   checked: true = checked
// The current flags are read first so that only the "checked" bit changes.
void SetChecked (short menu, Int32 index, bool checked)
{
	API_MenuItemRef ref = {};
	ref.menuResID = menu;
	ref.itemIndex = index;
	GSFlags flags = 0;
	ACAPI_MenuItem_GetMenuItemFlags (&ref, &flags);
	if (checked)
		flags |= API_MenuItemChecked;
	else
		flags &= ~API_MenuItemChecked;
	ACAPI_MenuItem_SetMenuItemFlags (&ref, &flags);
}

// Updates every check mark of the five menus from g_settings. Called after each settings change.
void RefreshCheckMarks ()
{
	// Step 1: input / output language menus. Item i + 1 is TT::Languages[i]; i = 0 is the empty code,
	// i.e. "automatic detection" (input) or "no translation" (output).
	for (int i = 0; i < TT::LanguageCount; ++i) {
		SetChecked (kMenuInput (), i + 1, g_settings.input == TT::Languages[i].code);
		SetChecked (kMenuOutput (), i + 1, g_settings.language == TT::Languages[i].code);
	}
	// Step 2: the two toggles of the "Options" menu.
	SetChecked (kMenuCommands (), kAutomatic, g_settings.engine);
	SetChecked (kMenuCommands (), kCollect, g_settings.collect);
	// Step 3: the selected online translator.
	for (int i = 0; i < TT::ProviderCount; ++i)
		SetChecked (kMenuTranslator (), i + 1, g_settings.provider == TT::Providers[i].code);
	// Step 4: the tool language (item 1, empty code = same as ArchiCAD).
	for (int i = 0; i < TT::LanguageCount; ++i)
		SetChecked (kMenuInterface (), i + 1, g_settings.interface == TT::Languages[i].code);
}

// Renames, during the session, the items of the five registered menus into interface language
// 'language' (0 = fr ... 19 = zh), so that changing the tool language does not wait for the next
// ArchiCAD start. The registered menus keep their resource IDs (those of g_menuLanguage); only
// their texts are replaced by those of the STR# resources of the new language, which have the same
// layout: string 1 = main menu title, string 2 = submenu title, string k + 2 = item k.
// Separators ("-") are left alone. The API documents items (index >= 1); index 0 is tried for the
// submenu title and its result traced in log.txt (ArchiCAD may refuse it).
void TranslateMenus (int language)
{
	const short registeredMenus[5] = { kMenuInput (), kMenuOutput (), kMenuCommands (), kMenuTranslator (), kMenuInterface () };
	std::string trace;
	for (short m = 0; m < 5; ++m) {
		const short source = static_cast<short> (TT::MenuBase (language) + m);	// texts of the new language
		// Step 1: the submenu title (string 2), as item 0.
		GS::UniString text;
		API_MenuItemRef ref = {};
		ref.menuResID = registeredMenus[m];
		if (RSGetIndString (&text, source, 2, ACAPI_GetOwnResModule ()) && !text.IsEmpty ()) {
			ref.itemIndex = 0;
			trace += " " + std::to_string (ref.menuResID) + ":0=" +
					 std::to_string (static_cast<long> (ACAPI_MenuItem_SetMenuItemText (&ref, nullptr, &text)));
		}
		// Step 2: every item, until the resource has no more strings.
		long errors = 0;
		for (Int32 k = 1; k < 64; ++k) {
			text.Clear ();
			if (!RSGetIndString (&text, source, static_cast<short> (k + 2), ACAPI_GetOwnResModule ()) || text.IsEmpty ())
				break;
			if (text == "-")
				continue;
			ref.itemIndex = k;
			if (ACAPI_MenuItem_SetMenuItemText (&ref, nullptr, &text) != NoError)
				++errors;
		}
		trace += " " + std::to_string (ref.menuResID) + ":item_errors=" + std::to_string (errors);
	}
	Log (std::string ("menus renamed to ") + TT::InterfaceCode (language) + " :" + trace);
	// Step 3: renaming may reset the item flags: put the check marks back.
	RefreshCheckMarks ();
}

// Appends one UTF-8 line to <g_root>\log.txt, as "YYYY-MM-DD hh:mm:ss [add-on] <line>".
// Unlike Report, this is safe outside the main thread (pure Win32): the watchdog uses it.
// The layer and the engine write to the same file, hence the shared open mode. Errors are ignored.
void Log (const std::string& line)
{
	if (g_root.empty ())
		return;
	// Step 1: open in append mode (created if missing), shared with the other writers.
	HANDLE f = CreateFileW (TT::Join (g_root, L"log.txt").c_str (), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
							nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return;
	// Step 2: local time stamp.
	SYSTEMTIME t;
	GetLocalTime (&t);
	char date[40];
	snprintf (date, sizeof (date), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	// Step 3: write the whole line in one call, then close.
	const std::string text = std::string (date) + " [add-on] " + line + "\n";
	DWORD written = 0;
	WriteFile (f, text.data (), static_cast<DWORD> (text.size ()), &written, nullptr);
	CloseHandle (f);
}

// Display name of an online translator code ("deepl" -> "DeepL"); unknown codes are returned as is.
GS::UniString ProviderName (const std::string& code)
{
	for (int i = 0; i < TT::ProviderCount; ++i) {
		if (code == TT::Providers[i].code)
			return GS::UniString (TT::Providers[i].name);
	}
	return Uni (code);
}

// Describes the state of the credentials of one translator in keys.json, for the Session Report.
// It never returns the key itself: only whether the expected field is filled in.
//   code: translator code (TT::Providers)
// What is expected depends on the service: MyMemory needs nothing (an e-mail raises the quota),
// LibreTranslate needs a server URL, Microsoft needs a key and a region, the others a key.
GS::UniString KeyStatus (const std::string& code)
{
	// Step 1: read keys.json (it may be missing or edited by hand into invalid JSON).
	std::string content;
	if (!TT::ReadWholeFile (TT::KeysPath (g_root), content))
		return FE ("keys.json absent", "keys.json missing");
	// Step 2: parse it, skipping a UTF-8 BOM (Notepad may add one when saving).
	Json::Value v;
	if (!Json::Reader (content.compare (0, 3, "\xEF\xBB\xBF") == 0 ? content.substr (3) : content).Read (v))
		return FE ("keys.json illisible (JSON invalide)", "keys.json unreadable (invalid JSON)");
	// Step 3: check the field(s) that this service needs.
	const Json::Value* s = v.Field (code);
	if (code == "mymemory")
		return s != nullptr && !s->Text ("email").empty () ? T (TT::R_EMAIL_OK) : T (TT::R_NO_KEY);
	if (code == "libretranslate")
		return s != nullptr && !s->Text ("url").empty () ? T (TT::R_ADDRESS_OK) : T (TT::R_ADDRESS_TO_ENTER);
	if (s == nullptr || s->Text ("key").empty ())
		return T (TT::R_KEY_TO_ENTER);
	if (code == "microsoft" && s->Text ("region").empty ())
		return T (TT::R_KEY_OK) + FE (", region vide", ", region empty");
	return T (TT::R_KEY_OK);
}

// -----------------------------------------------------------------------------
//  Translation engine (TapirTranslatorEngine.exe): extraction, launch, watchdog
//  The engine is a separate local process (127.0.0.1:TT::EnginePort) that sends the
//  sentences unknown to the dictionaries to the chosen online translator (or, with the
//  TT_LOCAL_MODEL build option, to an offline model). A crash there never reaches ArchiCAD.
// -----------------------------------------------------------------------------

// Returns the full path of the .apx itself (it carries the engine, and optionally the model,
// as a payload appended to the file, see TT_Payload.hpp); "" on failure.
// The module is found from the address of this very function, so it works whatever the
// install folder; UNCHANGED_REFCOUNT avoids having to release the module handle.
std::wstring ApxPath ()
{
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH] = {};
	if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							reinterpret_cast<LPCWSTR> (&ApxPath), &module) &&
		GetModuleFileNameW (module, path, MAX_PATH) > 0)
		return path;
	return std::wstring ();
}

// Base folder under which the payload is extracted: %LOCALAPPDATA%\Tapir Translator\engine
// (each version then goes into its own <hash> sub-folder). LOCALAPPDATA keeps the binaries
// out of the roaming profile; if it is missing, <g_root>\engine is used instead.
std::wstring ExtractionBase ()
{
	std::wstring local = TT::GetEnvVar (L"LOCALAPPDATA");
	return local.empty () ? TT::Join (g_root, L"engine") : TT::Join (TT::Join (local, L"Tapir Translator"), L"engine");
}

std::wstring g_engineFolder;	// folder of the extracted payload (engine + optional model); "" = not extracted yet

// Lists, in order of preference, the paths where TapirTranslatorEngine.exe is looked for:
//   1. the engine extracted from the .apx payload (normal case);
//   2. (development) a "TapirTranslatorEngine" folder next to the .apx;
//   3. %APPDATA%\Tapir Translator\engine.
// The paths are returned whether or not the files exist (Status also displays them).
std::vector<std::wstring> EngineLocations ()
{
	std::vector<std::wstring> res;
	if (!g_engineFolder.empty ())
		res.push_back (TT::Join (g_engineFolder, L"TapirTranslatorEngine.exe"));
	const std::wstring apx = ApxPath ();
	if (!apx.empty ())
		res.push_back (TT::Join (TT::Join (apx.substr (0, apx.find_last_of (L"\\/")), L"TapirTranslatorEngine"), L"TapirTranslatorEngine.exe"));
	res.push_back (TT::Join (TT::Join (g_root, L"engine"), L"TapirTranslatorEngine.exe"));
	return res;
}

// Extracts the payload of the .apx into ExtractionBase ()\<hash> and remembers the folder in
// g_engineFolder. TT::ExtractPayload does nothing if this version is already fully extracted,
// so the real copy happens only once per version; within a session this function runs once.
void ExtractEngine ()
{
	if (!g_engineFolder.empty ())
		return;
	// Step 1: extract (or find the existing extraction) and trace what happened.
	std::vector<std::wstring> journal;
	std::wstring folder;
	const bool ok = TT::ExtractPayload (ApxPath (), ExtractionBase (), folder, journal);
	for (const std::wstring& l : journal)
		Report (Uni (l));
	if (!ok)
		return;
	g_engineFolder = folder;
	// Step 2: delete the folders of older extracted versions (including the old ~500 MB offline
	// engine), except the current one. A folder whose engine is still running cannot be deleted
	// (locked file): it stays, and deletion is retried at the next start.
	const std::wstring base = ExtractionBase ();
	// (versions <= 0.7.5 extracted into ...\Tapir Translator\moteur: that whole folder goes too)
	const std::wstring oldBase = base.substr (0, base.find_last_of (L"\\/") + 1) + L"moteur";
	if (TT::FolderExists (oldBase)) {
		TT::DeleteFolder (oldBase);
		Report (GS::UniString (TT::FolderExists (oldBase) ? "old engine folder not deleted (in use): " : "old engine folder deleted: ") + Uni (oldBase));
	}
	const std::wstring kept = folder.substr (folder.find_last_of (L"\\/") + 1);
	for (const std::wstring& name : TT::ListFolders (base)) {
		if (name == kept)
			continue;
		const std::wstring old = TT::Join (base, name);
		TT::DeleteFolder (old);
		Report (GS::UniString (TT::FolderExists (old) ? FE ("ancienne version non supprimee (en cours d'utilisation) : ", "old version not deleted (in use): ")
														   : FE ("ancienne version supprimee : ", "old version deleted: ")) + Uni (old));
	}
}

// Returns true if the engine process we started is still alive (zero-timeout wait on its handle).
bool EngineRunning ()
{
	std::lock_guard<std::mutex> lock (g_lock);
	return g_engine != nullptr && WaitForSingleObject (g_engine, 0) == WAIT_TIMEOUT;
}

// Starts the engine process with the command line g_engineCommandLine in g_launchFolder.
// Must be called with g_lock held (used by both StartEngine and the watchdog).
//   error: receives GetLastError () when the start fails
// Returns true on success; g_engine then holds the new process handle (the previous one,
// if any, is closed).
bool StartProcess (DWORD& error)
{
	// Step 1: CreateProcessW may modify the command line, so it gets a writable copy.
	std::vector<wchar_t> buffer (g_engineCommandLine.begin (), g_engineCommandLine.end ());
	buffer.push_back (L'\0');
	// Step 2: start without a console window, at below-normal priority so that ArchiCAD
	// stays responsive.
	STARTUPINFOW si = {};
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW (nullptr, buffer.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
						 nullptr, g_launchFolder.c_str (), &si, &pi)) {
		error = GetLastError ();
		return false;
	}
	// Step 3: only the process handle is kept.
	CloseHandle (pi.hThread);
	// Step 4: the engine's "MyMemory quota" window must be allowed to come to the foreground.
	AllowSetForegroundWindow (pi.dwProcessId);
	// Step 5: replace the handle of the previous (dead) process.
	if (g_engine != nullptr)
		CloseHandle (g_engine);
	g_engine = pi.hProcess;
	return true;
}

// Watchdog thread body: restarts the engine whenever it stops (crash, killed process...)
// for as long as g_engineWanted is true. Runs outside the main thread, so it only logs to
// log.txt (Log), never to the Session Report.
// Exit code 2 means the port is already taken by the engine of another ArchiCAD instance:
// in that case the restart is retried much later (30 s).
void Watch ()
{
	int restarts = 0;							// restarts in the current one-hour window
	ULONGLONG windowStart = GetTickCount64 ();	// start of that window (ms)
	while (g_engineWanted) {
		// Step 1: take a snapshot of the process handle under the lock.
		HANDLE h = nullptr;
		{
			std::lock_guard<std::mutex> lock (g_lock);
			h = g_engine;
		}
		// Step 2: wait up to 500 ms for the process to end; still running -> loop again
		// (this also re-checks g_engineWanted twice a second).
		if (h != nullptr && WaitForSingleObject (h, 500) == WAIT_TIMEOUT)
			continue;
		if (!g_engineWanted)
			break;
		// Step 3: the engine has stopped: read its exit code.
		DWORD code = 0;
		if (h != nullptr)
			GetExitCodeProcess (h, &code);
		// Step 4: restart budget of 20 per hour; the counter is reset when the window expires.
		if (GetTickCount64 () - windowStart > 3600000ull) {
			windowStart = GetTickCount64 ();
			restarts = 0;
		}
		if (restarts >= 20) {
			Log ("engine stopped 20 times in one hour: watchdog suspended (Options > Online translation to restart)");
			return;
		}
		// Step 5: back-off delay: 30 s if the port is taken (code 2), otherwise 1 s, 2 s, 3 s...
		// growing with each restart. The wait is sliced in 250 ms steps so that StopEngine
		// (which clears g_engineWanted and joins this thread) is never blocked for long.
		const DWORD wait = code == 2 ? 30000 : static_cast<DWORD> (1000 * (1 + restarts));
		Log ("engine stopped (code " + std::to_string (code) + "): restart in " + std::to_string (wait / 1000) + " s");
		for (DWORD t = 0; t < wait && g_engineWanted; t += 250)
			Sleep (250);
		if (!g_engineWanted)
			break;
		// Step 6: restart with the same command line, under the lock.
		std::lock_guard<std::mutex> lock (g_lock);
		DWORD error = 0;
		if (StartProcess (error))
			Log ("engine restarted");
		else
			Log ("engine restart failed (Windows error " + std::to_string (error) + ")");
		++restarts;
	}
}

// Stops the engine and its watchdog. Safe to call when nothing is running.
void StopEngine ()
{
	// Step 1: tell the watchdog to stop, then wait for its thread to finish
	// (it must not restart the engine we are about to kill).
	g_engineWanted = false;
	if (g_watchdog.joinable ())
		g_watchdog.join ();
	// Step 2: kill the process if it is still alive, release its handle, and report it.
	std::lock_guard<std::mutex> lock (g_lock);
	if (g_engine != nullptr) {
		if (WaitForSingleObject (g_engine, 0) == WAIT_TIMEOUT)
			TerminateProcess (g_engine, 0);
		CloseHandle (g_engine);
		g_engine = nullptr;
		Report (T (TT::R_ENGINE_STOPPED));
	}
}

// Starts the translation engine (if not already running) and its watchdog thread.
// Called at start-up when online translation is enabled, and from the menu commands.
// Every outcome is traced in the Session Report.
void StartEngine ()
{
	// Step 1: nothing to do if the engine is already running and supervised; otherwise clean
	// up any half-state (dead process, watchdog still alive) before starting again.
	if (EngineRunning () && g_engineWanted)
		return;
	StopEngine ();
	// Step 2: make sure the payload of the .apx is extracted.
	ExtractEngine ();
	// Step 3: take the first engine executable that exists (see EngineLocations);
	// if none is found, report every path that was tried.
	std::wstring exe;
	for (const std::wstring& e : EngineLocations ()) {
		if (TT::FileExists (e)) {
			exe = e;
			break;
		}
	}
	if (exe.empty ()) {
		Report (T (TT::R_ENGINE_NOT_FOUND));
		for (const std::wstring& e : EngineLocations ())
			Report (GS::UniString ("  ") + Uni (e));
		return;
	}
	// Step 4: the engine reads keys.json at each request: create the template if it is missing
	// (an error here is not fatal, the engine then simply finds no key).
	const std::wstring folder = exe.substr (0, exe.find_last_of (L"\\/"));
	std::wstring keysError;
	TT::CreateKeysTemplate (g_root, keysError);
	// Step 5: build the command line:
	//   --port      local port the layer connects to
	//   --keys      path of keys.json (keys never pass through the scripts)
	//   --parent    ArchiCAD's process ID (the engine exits when ArchiCAD exits)
	//   --log   shared log file
	//   --interface language code of the tool (for the MyMemory quota window)
	std::wstring line = L"\"" + exe + L"\" --port " + std::to_wstring (TT::EnginePort) + L" --keys \"" + TT::KeysPath (g_root) +
						 L"\" --parent " + std::to_wstring (GetCurrentProcessId ()) + L" --log \"" +
						 TT::Join (g_root, L"log.txt") + L"\" --interface " +
						 TT::FromUtf8 (TT::InterfaceCode (g_messageLanguage));
	// Step 6: pass the offline model only if the .apx carries one (build option TT_LOCAL_MODEL).
	const std::wstring model = TT::Join (folder, L"model");
	if (TT::FolderExists (model))
		line += L" --model \"" + model + L"\"";
	// Step 7: remember the command line and folder (the watchdog reuses them) and start the
	// process, all under the lock.
	DWORD error = 0;
	{
		std::lock_guard<std::mutex> lock (g_lock);
		g_engineCommandLine = line;
		g_launchFolder = folder;
		if (!StartProcess (error)) {
			Report (GS::UniString (FE ("ERREUR lancement du moteur (", "ERROR starting the engine (")) +
					 GS::ValueToUniString (static_cast<Int32> (error)) + ") : " + Uni (exe));
			return;
		}
	}
	// Step 8: start the watchdog, then report the chosen translator and the state of its key.
	g_engineWanted = true;
	g_watchdog = std::thread (Watch);
	Report (Uni (TT::TxtF (TT::R_ENGINE_STARTED, g_messageLanguage,
							{ { "f", ProviderName (g_settings.provider).ToCStr (0, MaxUSize, CC_UTF8).Get () },
							  { "e", KeyStatus (g_settings.provider).ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
}

// Menu "Online translator > Term corrections...": opens corrections.json in Notepad, creating
// the pre-filled template first if the file does not exist (an existing file is never
// overwritten). ShellExecuteW returns a value <= 32 on failure.
void OpenCorrections ()
{
	std::wstring error;
	if (!TT::CreateCorrectionsTemplate (g_root, error)) {
		Report (GS::UniString (FE ("ERREUR creation de corrections.json : ", "ERROR creating corrections.json: ")) + Uni (error));
		return;
	}
	const std::wstring parameters = L"\"" + TT::CorrectionsPath (g_root) + L"\"";
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", L"notepad.exe", parameters.c_str (), nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Report (GS::UniString (FE ("ERREUR ouverture de ", "ERROR opening ")) + Uni (TT::CorrectionsPath (g_root)));
	else
		Report (T (TT::R_CORRECTIONS_OPENED));
}

// Menu "Online translator > Translator keys...": opens keys.json in Notepad, creating the empty
// template first if the file does not exist. The keys stay on this computer: only the engine
// reads them, at each request, so a key saved here is used by the very next translation.
void OpenKeys ()
{
	std::wstring error;
	if (!TT::CreateKeysTemplate (g_root, error)) {
		Report (GS::UniString (FE ("ERREUR creation de keys.json : ", "ERROR creating keys.json: ")) + Uni (error));
		return;
	}
	const std::wstring parameters = L"\"" + TT::KeysPath (g_root) + L"\"";
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", L"notepad.exe", parameters.c_str (), nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Report (GS::UniString (FE ("ERREUR ouverture de ", "ERROR opening ")) + Uni (TT::KeysPath (g_root)));
	else
		Report (T (TT::R_KEYS_OPENED) + " (" + Uni (TT::KeysPath (g_root)) + ")");
}

// Saves g_settings to settings.json (the layer picks it up live), reports the new settings
// in the Session Report and refreshes the menu check marks.
void Save ()
{
	std::wstring error;
	// Step 1: warn when input and output languages are the same (nothing will be translated).
	if (g_settings.input == g_settings.language && !g_settings.language.empty ())
		Report (T (TT::R_SAME_LANGUAGE));
	// Step 2: write the file atomically; on success, report languages and online translation
	// state (translator + key state), otherwise report the error.
	if (TT::WriteSettings (g_root, g_settings, error)) {
		const GS::UniString online = g_settings.engine
			? T (TT::R_YES) + " (" + ProviderName (g_settings.provider) + " : " + KeyStatus (g_settings.provider) + ")"
			: T (TT::R_NO);
		Report (Uni (TT::TxtF (TT::R_SETTINGS, g_messageLanguage,
								{ { "l", Languages ().ToCStr (0, MaxUSize, CC_UTF8).Get () },
								  { "o", online.ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
	} else
		Report (GS::UniString (FE ("ERREUR ecriture settings.json : ", "ERROR writing settings.json: ")) + Uni (error));
	// Step 3: menu check marks follow the in-memory settings.
	RefreshCheckMarks ();
}

// Add-on start-up sequence (called once from Initialize). Puts everything in place so that the
// next Python script launched by Tapir is translated.
void Start ()
{
	// Step 1: working folder %APPDATA%\Tapir Translator; without APPDATA nothing can work.
	g_root = TT::DefaultFolder ();
	if (g_root.empty ()) {
		Report (FE ("ERREUR : variable APPDATA absente, la traduction est inactive.", "ERROR: APPDATA variable missing, translation is inactive."));
		return;
	}
	// Step 2: write the translation layer (sitecustomize.py), the common dictionary and the
	// user templates, and trace each file in the Session Report.
	std::vector<std::wstring> journal;
	// (data of versions <= 0.7.5 converted first: usually already done by RegisterInterface)
	TT::MigrateOldData (g_root, g_migration);
	for (const std::wstring& line : g_migration)
		Report (Uni (line));
	g_migration.clear ();
	const bool layer = TT::ExtractLayer (g_root, journal);
	for (const std::wstring& line : journal)
		Report (Uni (line));

	// Step 3: prepend the layer folder to ArchiCAD's PYTHONPATH and set TAPIR_TRANSLATOR_DIR;
	// the scripts launched by Tapir inherit this environment.
	std::wstring pythonPath;
	const bool env = TT::PrepareEnvironment (g_root, pythonPath);
	Report (GS::UniString ("PYTHONPATH = ") + Uni (pythonPath) + (env ? "" : "  (ERROR SetEnvironmentVariable)"));

	// Step 4: load the saved settings and the message language they imply.
	g_settings = TT::ReadSettings (g_root);
	g_messageLanguage = ToolLanguage (g_settings.interface);
	// Step 5: translation is active only if both the layer and the environment are in place;
	// the engine is started only if online translation was left enabled.
	g_ready = layer && env;
	if (g_ready && g_settings.engine)
		StartEngine ();
	// Step 6: final status line (version and languages, or "inactive").
	Report (Uni (TT::TxtF (g_ready ? TT::R_STARTUP : TT::R_INACTIVE, g_messageLanguage,
							{ { "v", TT::AddOnVersion }, { "l", Languages ().ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
}

// Menu "Options > Tapir Translator status": writes a full diagnostic to the Session Report, so
// the user can paste it instead of describing a problem. Keys are never printed, only their state.
void Status ()
{
	const GS::UniString yes = T (TT::R_YES), non = T (TT::R_NO);
	// Step 1: title, working folder, and the two environment variables as ArchiCAD sees them.
	Report (GS::UniString ("=== ") + T (TT::M_STATUS) + " " + TT::AddOnVersion + " ===");
	Report (GS::UniString (FE ("dossier = ", "folder = ")) + Uni (g_root) + (g_ready ? "" : FE ("  (couche NON installee)", "  (layer NOT installed)")));
	Report (GS::UniString ("PYTHONPATH = ") + Uni (TT::GetEnvVar (L"PYTHONPATH")));
	Report (GS::UniString ("TAPIR_TRANSLATOR_DIR = ") + Uni (TT::GetEnvVar (L"TAPIR_TRANSLATOR_DIR")));
	// Step 2: languages, collection option and tool language (with ArchiCAD's code when automatic).
	Report (GS::UniString (FE ("langues = ", "languages = ")) + Languages () + ", " + T (TT::M_COLLECT) + " = " + (g_settings.collect ? yes : non));
	Report (T (TT::M_INTERFACE) + " = " + Uni (std::string (TT::InterfaceName (g_messageLanguage))) +
			 (g_settings.interface.empty () ? GS::UniString (" (") + T (TT::M_INTERFACE_AUTO) + " : " + Uni (g_archicadVersion) + ")" : GS::UniString ()));
	// Step 3: every translator with the state of its key; "[x]" marks the selected one.
	for (int i = 0; i < TT::ProviderCount; ++i)
		Report (GS::UniString ("  ") + (g_settings.provider == TT::Providers[i].code ? "[x] " : "    ") +
				 TT::Providers[i].name + " : " + KeyStatus (TT::Providers[i].code));
	// Step 4: online translation wanted or not, and whether the engine process is alive.
	Report (T (TT::M_ONLINE) + " = " + (g_settings.engine ? yes : non) + ", " + FE ("moteur ", "engine ") +
			 (EngineRunning () ? FE ("en marche", "running") : FE ("arrete", "stopped")) + " (port " +
			 GS::ValueToUniString (static_cast<Int32> (TT::EnginePort)) + ")");
	// Step 5: payload embedded in the .apx (hash and the first 12 file paths).
	{
		std::wstring hash;
		std::vector<std::wstring> paths;
		if (TT::ReadPayload (ApxPath (), hash, paths)) {
			Report (GS::UniString (FE ("charge embarquee dans le .apx : empreinte ", "payload embedded in the .apx: hash ")) + Uni (hash) + ", " +
					 GS::ValueToUniString (static_cast<Int32> (paths.size ())) + FE (" fichier(s)", " file(s)"));
			for (size_t i = 0; i < paths.size () && i < 12; ++i)
				Report (GS::UniString ("  ") + Uni (paths[i]));
		} else
			Report (FE ("aucune charge embarquee dans le .apx", "no payload embedded in the .apx"));
	}
	// Step 6: candidate engine paths ("[x]" = file exists) and presence of the layer file.
	for (const std::wstring& e : EngineLocations ())
		Report (GS::UniString ("  ") + (TT::FileExists (e) ? "[x] " : "[ ] ") + Uni (e));
	Report (GS::UniString ("sitecustomize.py = ") +
			 (TT::FileExists (TT::Join (TT::Join (g_root, L"layer"), L"sitecustomize.py")) ? yes : non));

	// Step 7: installed dictionaries.
	const std::wstring dictionaries = TT::Join (g_root, L"dictionaries");
	const std::vector<std::wstring> dictionaryFiles = TT::ListJson (dictionaries);
	Report (GS::UniString (FE ("dictionnaires (", "dictionaries (")) + GS::ValueToUniString (static_cast<Int32> (dictionaryFiles.size ())) + ") :");
	for (const std::wstring& d : dictionaryFiles)
		Report (GS::UniString ("  ") + Uni (d));

	// Step 8: collected untranslated texts: one file per script, with the number of entries
	// that are still empty (not yet translated by the user).
	const std::wstring missing = TT::Join (g_root, L"missing");
	const std::vector<std::wstring> files = TT::ListJson (missing);
	Report (GS::UniString (FE ("textes manquants (", "missing texts (")) + GS::ValueToUniString (static_cast<Int32> (files.size ())) + ") :");
	for (const std::wstring& f : files) {
		std::string content;
		TT::ReadWholeFile (TT::Join (missing, f), content);
		Report (GS::UniString ("  ") + Uni (f) + " : " + GS::ValueToUniString (static_cast<Int32> (TT::CountEmpty (content))));
	}

	// Step 9: the last 15 non-empty lines of log.txt (layer, engine and add-on errors).
	const std::vector<std::string> lines = TT::LastLines (TT::Join (g_root, L"log.txt"), 15);
	Report (GS::UniString ("log.txt (") + GS::ValueToUniString (static_cast<Int32> (lines.size ())) + ") :");
	for (const std::string& l : lines)
		Report (GS::UniString ("  ") + Uni (l));
}

// Menu "Options > Open the dictionaries folder": opens %APPDATA%\Tapir Translator in Explorer
// (created first if needed).
void OpenFolder ()
{
	if (g_root.empty ())
		return;
	TT::CreateFolder (g_root);
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", g_root.c_str (), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Report (GS::UniString (FE ("ERREUR ouverture du dossier ", "ERROR opening the folder ")) + Uni (g_root));
}

// Handler of all five menus (installed in Initialize). Dispatches on the menu resource ID and the
// 1-based item index; always returns NoError (errors are traced in the Session Report).
GSErrCode MenuCommandHandler (const API_MenuParams* menuParams)
{
	// Step 0: stay loaded after this call. The API asks for ACAPI_KeepInMemory (true) "each time
	// your add-on is called": without it ArchiCAD unloads the add-on once the menu command returns,
	// FreeData stops the engine, and the next Tapir script only gets the dictionary translations
	// (bug of v0.7.3 and before, seen after choosing the tool language).
	ACAPI_KeepInMemory (true);
	const short menu = menuParams->menuItemRef.menuResID;
	const Int32 index = menuParams->menuItemRef.itemIndex;
	// The three language menus have one item per entry of TT::Languages.
	const bool language = index >= 1 && index <= TT::LanguageCount;
	// Input / output language: store the code and save.
	if (menu == kMenuInput () && language) {
		g_settings.input = TT::Languages[index - 1].code;
		Save ();
	} else if (menu == kMenuOutput () && language) {
		g_settings.language = TT::Languages[index - 1].code;
		Save ();
	} else if (menu == kMenuTranslator ()) {
		// Online translator menu: a provider, or one of the two "open file" commands.
		if (index >= 1 && index <= TT::ProviderCount) {
			g_settings.provider = TT::Providers[index - 1].code;
			// Choosing a translator also turns online translation on (and starts the engine).
			if (!g_settings.engine) {
				g_settings.engine = true;
				StartEngine ();
			}
			Save ();
		} else if (index == kKeys) {
			OpenKeys ();
		} else if (index == kCorrections) {
			OpenCorrections ();
		}
	} else if (menu == kMenuInterface () && language) {
		// Tool language. Item 1 = same as ArchiCAD (empty code). Messages and the engine window
		// change immediately; the menus only at the next ArchiCAD start (g_menuLanguage is fixed).
		g_settings.interface = TT::Languages[index - 1].code;
		g_messageLanguage = ToolLanguage (g_settings.interface);
		std::wstring error;
		TT::WriteSettings (g_root, g_settings, error);
		Report (Uni (TT::TxtF (TT::R_INTERFACE, g_messageLanguage, { { "l", TT::InterfaceName (g_messageLanguage) } })));
		TranslateMenus (g_messageLanguage);	// menu items follow at once (also restores the check marks)
		// The engine receives the language on its command line: restart it if it is running.
		if (g_engineWanted) {
			StopEngine ();
			StartEngine ();	// the engine picks up the new language (MyMemory quota window)
		}
	} else if (menu == kMenuCommands ()) {
		// Options menu: two toggles (saved) and two commands.
		if (index == kAutomatic) {
			g_settings.engine = !g_settings.engine;
			if (g_settings.engine)
				StartEngine ();
			else
				StopEngine ();
			Save ();
		} else if (index == kCollect) {
			g_settings.collect = !g_settings.collect;
			Save ();
		} else if (index == kOpenFolder) {
			OpenFolder ();
		} else if (index == kStatus) {
			Status ();
		}
	}
	return NoError;
}

}	// namespace


// =============================================================================
//  Functions required by the ArchiCAD API (called by ArchiCAD in this order:
//  CheckEnvironment, RegisterInterface, Initialize ... FreeData)
// =============================================================================

// Called first, when ArchiCAD scans the add-ons. Fills the name and description shown in the
// Add-On Manager and decides how the add-on is loaded.
API_AddonType CheckEnvironment (API_EnvirParams* envir)
{
	// Step 1: name and description from STR# 32000 (RFIX/Tapir_TranslatorFix.grc), plus the version.
	RSGetIndString (&envir->addOnInfo.name, 32000, 1, ACAPI_GetOwnResModule ());
	RSGetIndString (&envir->addOnInfo.description, 32000, 2, ACAPI_GetOwnResModule ());
	envir->addOnInfo.description += GS::UniString (" (") + TT::AddOnVersion + ")";
	// Step 2: language code of this ArchiCAD version (FRA, GER, INT...): the default tool language.
	g_archicadVersion = envir->serverInfo.language.ToCStr (0, MaxUSize, CC_UTF8).Get ();
	// Step 3: preload, because the environment must be in place before the first Tapir script runs.
	return APIAddon_Preload;
}


// Registers the menus. Called once; ArchiCAD keeps them until it quits, which is why the menu
// language is decided here, once and for all for this session.
GSErrCode RegisterInterface (void)
{
	// Step 1: menu language = the saved choice, otherwise ArchiCAD's language. The settings are
	// read directly here because Initialize (and thus Start) has not run yet.
	const std::wstring root = TT::DefaultFolder ();
	if (!root.empty ())
		TT::MigrateOldData (root, g_migration);	// data of versions <= 0.7.5, before reading the settings
	const TT::Settings r = root.empty () ? TT::Settings () : TT::ReadSettings (root);
	g_menuLanguage = ToolLanguage (r.interface);
	g_messageLanguage = g_menuLanguage;
	// Step 2: register the five menus of that language (help strings at + HelpOffset). They all
	// carry the same title "Tapir Translator", so ArchiCAD merges them into one main menu
	// (MenuFlag_InsertIntoSame); a separator is put before the Options menu.
	const auto Save = [] (short menu, GSFlags flags) {
		const GSErrCode e = ACAPI_MenuItem_RegisterMenu (menu, static_cast<short> (menu + TT::HelpOffset), MenuCode_UserDef, flags);
		g_menuTrace += " " + std::to_string (menu) + "=" + std::to_string (static_cast<long> (e));
		if (e != NoError && g_menuError == NoError) {
			g_menuError = e;
			g_failedMenu = menu;
		}
	};
	Save (kMenuInput (), MenuFlag_InsertIntoSame);
	Save (kMenuOutput (), MenuFlag_InsertIntoSame);
	Save (kMenuTranslator (), MenuFlag_InsertIntoSame);
	Save (kMenuCommands (), MenuFlag_SeparatorBefore | MenuFlag_InsertIntoSame);
	Save (kMenuInterface (), MenuFlag_InsertIntoSame);
	// Step 3: never return the error: a failing menu must not make ArchiCAD unload the add-on
	// (the translation itself would then stop too). The error is reported by Initialize.
	return NoError;
}


// Called after RegisterInterface, when the add-on is loaded (at start-up, since it is preloaded).
GSErrCode Initialize (void)
{
	// Step 1: install the same handler on the five menus; failures are reported, not fatal.
	GSErrCode err = NoError;
	for (const short menu : { kMenuInput (), kMenuOutput (), kMenuTranslator (), kMenuCommands (), kMenuInterface () }) {
		const GSErrCode e = ACAPI_MenuItem_InstallMenuHandler (menu, MenuCommandHandler);
		if (e != NoError) {
			Report (GS::UniString ("ERROR ACAPI_MenuItem_InstallMenuHandler (") + GS::ValueToUniString (static_cast<Int32> (menu)) +
					 ") = " + GS::ValueToUniString (static_cast<Int32> (e)));
			err = e;
		}
	}
	// Step 2: layer, environment, settings and (optionally) the engine.
	Start ();
	// Step 2b: trace of the menu registration (log.txt only): ArchiCAD language code, menu
	// language index, and the error code returned for each menu (0 = registered).
	Log ("menus : ArchiCAD " + g_archicadVersion + ", language " + TT::InterfaceCode (g_menuLanguage) + ", RegisterMenu" + g_menuTrace);
	if (g_menuError != NoError)
		Report (GS::UniString ("ERROR ACAPI_MenuItem_RegisterMenu (") + GS::ValueToUniString (static_cast<Int32> (g_failedMenu)) +
				 ") = " + GS::ValueToUniString (static_cast<Int32> (g_menuError)));
	// Step 3: menu check marks reflect the loaded settings.
	RefreshCheckMarks ();
	// Step 4: stay loaded: the environment, the engine process and its watchdog thread belong
	// to this module and must live for the whole session.
	ACAPI_KeepInMemory (true);
	// Step 5: menu errors were reported above; they must not unload the add-on (translation keeps
	// working with the saved settings).
	(void) err;
	return NoError;
}


// Called when the add-on is unloaded (ArchiCAD quits): stop the watchdog and the engine.
GSErrCode FreeData (void)
{
	// Traced in log.txt: an unload outside ArchiCAD's exit means the add-on lost its
	// "keep in memory" state (the engine stops with it).
	Log ("add-on unloaded (FreeData)");
	StopEngine ();
	return NoError;
}
