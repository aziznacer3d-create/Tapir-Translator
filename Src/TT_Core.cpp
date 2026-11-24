// *****************************************************************************
// Tapir Translator - ArchiCAD-independent core (implementation, see TT_Core.hpp)
//
//   ROLE IN THE SYSTEM
//   Implements the language/translator tables, the Win32 file and environment
//   helpers, the installation of the translation layer and its data files, the
//   settings.json reader/writer, the user file templates (keys.json,
//   corrections.json, _user.json) and the PYTHONPATH preparation. Linked both
//   into the add-on and into the test bench, so it never calls ArchiCAD.
// *****************************************************************************

#include "TT_Core.hpp"
#include "TT_Embedded.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <initializer_list>
#include <winsock2.h>	// before windows.h (PortAnswers)
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment (lib, "Ws2_32.lib")
#include <stdlib.h>

namespace TT {

const char* const AddOnVersion = "0.11.0";

// Same order as the items of the input / output language menus (MenuBase + 0 / + 1, generated
// into RFIX/Tapir_TranslatorMenus.grc) and as the columns of dictionaries/_common.json.
// Entry 0 (empty code) is the "automatic detection" / "no translation" / "as ArchiCAD" item.
const Language Languages[] = {
	{ "",	"none" },
	{ "fr",	"Français" },
	{ "en",	"English" },
	{ "de",	"Deutsch" },
	{ "es",	"Español" },
	{ "pt",	"Português" },
	{ "it",	"Italiano" },
	{ "hu",	"Magyar" },
	{ "cs",	"Čeština" },
	{ "pl",	"Polski" },
	{ "ru",	"Русский" },
	{ "uk",	"Українська" },
	{ "el",	"Ελληνικά" },
	{ "tr",	"Türkçe" },
	{ "nl",	"Nederlands" },
	{ "sv",	"Svenska" },
	{ "no",	"Norsk" },
	{ "fi",	"Suomi" },
	{ "ja",	"日本語" },
	{ "ko",	"한국어" },
	{ "zh",	"中文" },
};
const int LanguageCount = static_cast<int> (sizeof (Languages) / sizeof (Languages[0]));

// Online translators: code, display name, what keys.json must contain (in menu order).
const Provider Providers[] = {
	{ "deepl",			"DeepL",					"key (DeepL API Free account)" },
	{ "microsoft",		"Microsoft Translator",		"key + region (Azure, free tier F0)" },
	{ "google",			"Google Cloud Translation",	"key (Google Cloud)" },
	{ "mymemory",		"MyMemory",					"nothing (optional e-mail for a larger quota)" },
	{ "libretranslate",	"LibreTranslate",			"server address (+ key if required)" },
};
const int ProviderCount = static_cast<int> (sizeof (Providers) / sizeof (Providers[0]));

// Validates a translator code read from settings.json. Besides the menu entries, it accepts
// "test" (the fake engine of the test bench) and "local" (offline model, build option).
bool KnownProvider (const std::string& code)
{
	if (code == "test" || code == "local")
		return true;
	for (int i = 0; i < ProviderCount; ++i) {
		if (code == Providers[i].code)
			return true;
	}
	return false;
}


// =============================================================================
//  Text conversions
// =============================================================================

// UTF-16 -> UTF-8. Two-pass Win32 conversion: first ask for the required size, then convert.
// Returns "" for an empty input or on failure.
std::string Utf8 (const std::wstring& text)
{
	if (text.empty ())
		return std::string ();
	const int n = WideCharToMultiByte (CP_UTF8, 0, text.c_str (), static_cast<int> (text.size ()), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return std::string ();
	std::string res (static_cast<size_t> (n), '\0');
	WideCharToMultiByte (CP_UTF8, 0, text.c_str (), static_cast<int> (text.size ()), &res[0], n, nullptr, nullptr);
	return res;
}


// UTF-8 -> UTF-16, same two-pass scheme as Utf8. Returns L"" for an empty input or on failure.
std::wstring FromUtf8 (const std::string& text)
{
	if (text.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, text.c_str (), static_cast<int> (text.size ()), nullptr, 0);
	if (n <= 0)
		return std::wstring ();
	std::wstring res (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, text.c_str (), static_cast<int> (text.size ()), &res[0], n);
	return res;
}


// Builds a readable message for a Win32 error code: "error Windows <code> (<system text>)".
// The system text comes from FormatMessageW (which allocates the buffer, freed here) with its
// trailing line breaks and spaces removed.
static std::wstring ErrorMessage (DWORD code)
{
	wchar_t* buffer = nullptr;
	const DWORD n = FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
									nullptr, code, 0, reinterpret_cast<wchar_t*> (&buffer), 0, nullptr);
	std::wstring res = L"Windows error " + std::to_wstring (static_cast<unsigned long> (code));
	if (n > 0 && buffer != nullptr) {
		std::wstring msg (buffer, n);
		while (!msg.empty () && (msg.back () == L'\n' || msg.back () == L'\r' || msg.back () == L' '))
			msg.pop_back ();
		res += L" (" + msg + L")";
	}
	if (buffer != nullptr)
		LocalFree (buffer);
	return res;
}


// =============================================================================
//  Files and environment variables
// =============================================================================

// Reads an environment variable of the current process. Returns "" if it is not set.
std::wstring GetEnvVar (const wchar_t* name)
{
	// Step 1: ask for the required size (in characters, terminating zero included).
	const DWORD n = GetEnvironmentVariableW (name, nullptr, 0);
	if (n == 0)
		return std::wstring ();
	// Step 2: read it; a result of 0 or larger than the buffer means the variable vanished or
	// grew in between, which is treated as "not set".
	std::vector<wchar_t> buffer (n + 1, L'\0');
	const DWORD read_ = GetEnvironmentVariableW (name, buffer.data (), n + 1);
	if (read_ == 0 || read_ > n)
		return std::wstring ();
	return std::wstring (buffer.data (), read_);
}


// Working folder of the add-on, the layer and the engine: %APPDATA%\Tapir Translator.
// Returns "" when APPDATA is not defined.
std::wstring DefaultFolder ()
{
	const std::wstring appData = GetEnvVar (L"APPDATA");
	if (appData.empty ())
		return std::wstring ();
	return Join (appData, L"Tapir Translator");
}


// Joins a folder and a name. No separator is added if the folder already ends with '\' or '/';
// an empty folder returns the name alone.
std::wstring Join (const std::wstring& folder, const std::wstring& name)
{
	if (folder.empty ())
		return name;
	const wchar_t end_ = folder.back ();
	if (end_ == L'\\' || end_ == L'/')
		return folder + name;
	return folder + L"\\" + name;
}


// True if 'path' exists and is a file (not a folder).
bool FileExists (const std::wstring& path)
{
	const DWORD a = GetFileAttributesW (path.c_str ());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


// True if 'path' exists and is a folder.
bool FolderExists (const std::wstring& path)
{
	const DWORD a = GetFileAttributesW (path.c_str ());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}


// Creates a folder and, recursively, its missing parents (like "mkdir -p").
// Returns true if the folder exists at the end.
bool CreateFolder (const std::wstring& path)
{
	if (path.empty ())
		return false;
	if (FolderExists (path))
		return true;
	// Create the parent first. "sep > 2" stops the recursion at the drive root ("C:\").
	const size_t sep = path.find_last_of (L"\\/");
	if (sep != std::wstring::npos && sep > 2)
		CreateFolder (path.substr (0, sep));
	return CreateDirectoryW (path.c_str (), nullptr) != 0 || GetLastError () == ERROR_ALREADY_EXISTS;
}


// Reads a whole file as raw bytes into 'content' (cleared first). The file is opened with full
// sharing, because the layer, the engine or Notepad may have it open at the same time.
// Returns false if the file cannot be opened or a read fails.
bool ReadWholeFile (const std::wstring& path, std::string& content)
{
	content.clear ();
	HANDLE f = CreateFileW (path.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	char buffer[16384];
	DWORD read_ = 0;
	bool ok = true;
	for (;;) {
		if (!ReadFile (f, buffer, sizeof (buffer), &read_, nullptr)) {
			ok = false;
			break;
		}
		if (read_ == 0)
			break;
		content.append (buffer, read_);
	}
	CloseHandle (f);
	return ok;
}


// Atomic write of 'content' to 'path' (see TT_Core.hpp). On failure 'error' receives a
// readable message and the temporary file is removed.
bool WriteWholeFile (const std::wstring& path, const std::string& content, std::wstring& error)
{
	// Step 1: write everything into "<path>.tmp", in chunks of at most 1 MB.
	const std::wstring tmp = path + L".tmp";
	HANDLE f = CreateFileW (tmp.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		error = L"cannot create " + tmp + L": " + ErrorMessage (GetLastError ());
		return false;
	}
	size_t written = 0;
	bool ok = true;
	while (written < content.size ()) {
		const size_t rest = content.size () - written;
		const DWORD piece = static_cast<DWORD> (rest > 1048576 ? 1048576 : rest);
		DWORD done = 0;
		if (!WriteFile (f, content.data () + written, piece, &done, nullptr) || done == 0) {
			ok = false;
			break;
		}
		written += done;
	}
	// Step 2: keep the error code before CloseHandle can overwrite it, then close.
	const DWORD writeCode = ok ? 0 : GetLastError ();
	CloseHandle (f);
	if (!ok) {
		error = L"cannot write " + tmp + L": " + ErrorMessage (writeCode);
		DeleteFileW (tmp.c_str ());
		return false;
	}
	// Step 3: replace the target with the temporary file. The target may be open for reading
	// for a moment by the Python layer, so the move is retried (50 times, 20 ms apart = ~1 s).
	for (int test = 0; test < 50; ++test) {
		if (MoveFileExW (tmp.c_str (), path.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		Sleep (20);
	}
	error = L"cannot replace " + path + L": " + ErrorMessage (GetLastError ());
	DeleteFileW (tmp.c_str ());
	return false;
}


// Names (without path) of the *.json files of a folder; empty if the folder does not exist.
std::vector<std::wstring> ListJson (const std::wstring& folder)
{
	std::vector<std::wstring> res;
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Join (folder, L"*.json").c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return res;
	do {
		if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			res.push_back (d.cFileName);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	return res;
}


// Deletes a folder recursively: files first (read-only attribute cleared), sub-folders by
// recursion, then the folder itself. Errors are ignored: a file in use (e.g. a running engine)
// simply stays, and the caller checks FolderExists afterwards.
void DeleteFolder (const std::wstring& folder)
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Join (folder, L"*").c_str (), &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = d.cFileName;
			if (name == L"." || name == L"..")
				continue;
			const std::wstring path = Join (folder, name);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				DeleteFolder (path);
			else {
				SetFileAttributesW (path.c_str (), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW (path.c_str ());
			}
		} while (FindNextFileW (h, &d));
		FindClose (h);
	}
	RemoveDirectoryW (folder.c_str ());
}


// Names (without path) of the direct sub-folders of a folder, "." and ".." excluded.
std::vector<std::wstring> ListFolders (const std::wstring& folder)
{
	std::vector<std::wstring> res;
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Join (folder, L"*").c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return res;
	do {
		const std::wstring name = d.cFileName;
		if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..")
			res.push_back (name);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	return res;
}


// Counts the untranslated entries of a "missing" file (see TT_Core.hpp).
int CountEmpty (const std::string& json)
{
	// The missing files are written by the layer (json.dump, indent=2), so an empty entry
	// always reads exactly  "...": ""  and a simple text search is enough.
	int n = 0;
	size_t pos = 0;
	const std::string pattern = "\": \"\"";
	while ((pos = json.find (pattern, pos)) != std::string::npos) {
		++n;
		pos += pattern.size ();
	}
	return n;
}


// Returns the last 'number' non-empty lines of a text file (used to show the end of
// log.txt). Lines are split on '\n'; a trailing '\r' is removed.
std::vector<std::string> LastLines (const std::wstring& path, int number)
{
	std::vector<std::string> lines;
	std::string content;
	if (!ReadWholeFile (path, content))
		return lines;
	// Step 1: split the whole file into non-empty lines.
	size_t start = 0;
	while (start < content.size ()) {
		size_t end_ = content.find ('\n', start);
		if (end_ == std::string::npos)
			end_ = content.size ();
		std::string line = content.substr (start, end_ - start);
		if (!line.empty () && line.back () == '\r')
			line.pop_back ();
		if (!line.empty ())
			lines.push_back (line);
		start = end_ + 1;
	}
	// Step 2: keep only the last 'number' lines.
	if (static_cast<int> (lines.size ()) > number)
		lines.erase (lines.begin (), lines.end () - number);
	return lines;
}


// =============================================================================
//  Translation layer and dictionaries
// =============================================================================

// Installs the translation layer and the data files (see TT_Core.hpp). Called at every
// ArchiCAD start; files are only rewritten when their content differs.
bool ExtractLayer (const std::wstring& root, std::vector<std::wstring>& journal)
{
	// Step 1: create the folder tree: <root>, layer, dictionaries, missing.
	bool ok = true;
	const std::wstring layer = Join (root, L"layer");
	const std::wstring dictionaries = Join (root, L"dictionaries");
	for (const std::wstring& d : { root, layer, dictionaries, Join (root, L"missing") }) {
		if (!CreateFolder (d)) {
			journal.push_back (L"cannot create folder: " + d);
			ok = false;
		}
	}
	if (!ok)
		return false;

	// Step 2: layer\sitecustomize.py, embedded in the add-on (TT_Embedded.hpp). It must always be
	// the add-on's own version, so it is rewritten whenever the file on disk differs.
	std::wstring error;
	const std::wstring sitecustomize = Join (layer, L"sitecustomize.py");
	const std::string source (reinterpret_cast<const char*> (Embedded::Layer), Embedded::LayerSize);
	std::string current;
	if (ReadWholeFile (sitecustomize, current) && current == source) {
		journal.push_back (L"layer up to date: " + sitecustomize);
	} else if (WriteWholeFile (sitecustomize, source, error)) {
		journal.push_back (L"layer written: " + sitecustomize);
	} else {
		journal.push_back (error);
		ok = false;
	}

	// Step 3: dictionaries\_common.json belongs to the add-on version: always replaced when it
	// differs. The user's own additions go into _user.json (never touched, higher priority).
	const std::wstring common = Join (dictionaries, L"_common.json");
	const std::string content (reinterpret_cast<const char*> (Embedded::Common), Embedded::CommonSize);
	std::string existing;
	if (ReadWholeFile (common, existing) && existing == content) {
		journal.push_back (L"common dictionary up to date: " + common);
	} else if (WriteWholeFile (common, content, error)) {
		journal.push_back (L"common dictionary written: " + common);
	} else {
		journal.push_back (error);
		ok = false;
	}
	// Step 4: dictionaries\_user.json, created once with an explanation and one example line
	// (multilingual format 2). A failure here does not make the installation fail.
	const std::wstring user = Join (dictionaries, L"_user.json");
	if (!FileExists (user)) {
		const std::string model =
			"{\n"
			"  \"_explanation\": \"Your translations shared by all scripts. Never modified by the add-on; overrides _common.json. One notion per row, one column per language (codes: fr en de es pt it hu cs pl ru uk el tr nl sv no fi ja ko zh).\",\n"
			"  \"format\": 2,\n"
			"  \"terms\": [\n"
			"    {\"en\": \"Example\", \"fr\": \"Exemple\", \"de\": \"Beispiel\"}\n"
			"  ]\n"
			"}\n";
		if (WriteWholeFile (user, model, error))
			journal.push_back (L"user dictionary created: " + user);
	}
	// Step 5: templates of keys.json and corrections.json, only if missing (also non-fatal).
	if (!FileExists (KeysPath (root)) && CreateKeysTemplate (root, error))
		journal.push_back (L"keys file created: " + KeysPath (root));
	if (!FileExists (CorrectionsPath (root)) && CreateCorrectionsTemplate (root, error))
		journal.push_back (L"corrections file created: " + CorrectionsPath (root));
	return ok;
}


// =============================================================================
//  Settings (settings.json, re-read live by the layer)
//  The file is small and written only by WriteSettings, so it is read with a minimal
//  key search instead of a full JSON parser (this core has no dependency).
// =============================================================================

// True if 'code' is one of the codes of Languages[] (the empty code included).
static bool KnownLanguage (const std::string& code)
{
	for (int i = 0; i < LanguageCount; ++i) {
		if (code == Languages[i].code)
			return true;
	}
	return false;
}


// Finds the value of a key in a flat JSON text: returns the position of the first non-blank
// character after  "key" :  or std::string::npos if the key (or its colon) is not found.
// Only the first occurrence of the key is considered.
static size_t AfterKey (const std::string& json, const char* key)
{
	// Step 1: locate the quoted key, then the following colon.
	const std::string pattern = std::string ("\"") + key + "\"";
	size_t pos = json.find (pattern);
	if (pos == std::string::npos)
		return pos;
	pos = json.find (':', pos + pattern.size ());
	if (pos == std::string::npos)
		return pos;
	// Step 2: skip the colon and any white space.
	++pos;
	while (pos < json.size () && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
		++pos;
	return pos;
}


// Reads <root>\settings.json. Never fails: a missing file, a missing key or an unknown value
// keeps the default of Settings.
Settings ReadSettings (const std::wstring& root)
{
	Settings r;
	std::string json;
	if (!ReadWholeFile (Join (root, L"settings.json"), json))
		return r;
	// Step 1: the three language codes (i = 0 "language", 1 "input", 2 "interface"), kept only
	// when they are string values and known codes.
	for (int i = 0; i < 3; ++i) {
		const size_t pos = AfterKey (json, i == 0 ? "output" : (i == 1 ? "input" : "interface"));
		if (pos != std::string::npos && pos < json.size () && json[pos] == '"') {
			const size_t end_ = json.find ('"', pos + 1);
			if (end_ != std::string::npos) {
				const std::string code = json.substr (pos + 1, end_ - pos - 1);
				if (KnownLanguage (code))
					(i == 0 ? r.language : (i == 1 ? r.input : r.interface)) = code;
			}
		}
	}
	// Step 2: the two booleans (anything other than the literal true counts as false).
	size_t pos = AfterKey (json, "collect");
	if (pos != std::string::npos)
		r.collect = json.compare (pos, 4, "true") == 0;
	pos = AfterKey (json, "online");
	if (pos != std::string::npos)
		r.engine = json.compare (pos, 4, "true") == 0;
	// Step 3: the translator code, kept only if it is known.
	pos = AfterKey (json, "provider");
	if (pos != std::string::npos && pos < json.size () && json[pos] == '"') {
		const size_t end_ = json.find ('"', pos + 1);
		if (end_ != std::string::npos && KnownProvider (json.substr (pos + 1, end_ - pos - 1)))
			r.provider = json.substr (pos + 1, end_ - pos - 1);
	}
	return r;
}


// Path of the translators' credentials file: <root>\keys.json.
std::wstring KeysPath (const std::wstring& root)
{
	return Join (root, L"keys.json");
}


// Creates keys.json with one empty section per translator, each with a "_where" hint telling
// where to get the key. Returns true at once if the file already exists (never overwritten,
// it holds the user's keys).
bool CreateKeysTemplate (const std::wstring& root, std::wstring& error)
{
	const std::wstring path = KeysPath (root);
	if (FileExists (path))
		return true;
	const std::string model =
		"{\n"
		"  \"_explanation\": \"Keys of the Tapir Translator online translators. This file stays on this computer: it is never sent, nor read by the scripts. Save it: the key is used from the next translation on.\",\n"
		"  \"deepl\": {\n"
		"    \"_where\": \"https://www.deepl.com/pro-api: DeepL API Free plan (the key ends with :fx)\",\n"
		"    \"key\": \"\"\n"
		"  },\n"
		"  \"microsoft\": {\n"
		"    \"_where\": \"Azure portal: Translator resource, F0 tier (free); region = the resource's region, e.g. westeurope\",\n"
		"    \"key\": \"\",\n"
		"    \"region\": \"\"\n"
		"  },\n"
		"  \"google\": {\n"
		"    \"_where\": \"Google Cloud console: Cloud Translation API enabled, API key\",\n"
		"    \"key\": \"\"\n"
		"  },\n"
		"  \"mymemory\": {\n"
		"    \"_where\": \"no key; an e-mail address raises the free daily quota\",\n"
		"    \"email\": \"\"\n"
		"  },\n"
		"  \"libretranslate\": {\n"
		"    \"_where\": \"address of a LibreTranslate server (your own or a public instance) and its key if it requires one\",\n"
		"    \"url\": \"\",\n"
		"    \"key\": \"\"\n"
		"  }\n"
		"}\n";
	CreateFolder (root);
	return WriteWholeFile (path, model, error);
}


// Path of the term corrections file: <root>\corrections.json.
std::wstring CorrectionsPath (const std::wstring& root)
{
	return Join (root, L"corrections.json");
}


// Creates corrections.json pre-filled with the common mistakes of online translators on
// ArchiCAD vocabulary, for fr, de, es, it and pt. Returns true at once if the file already
// exists (never overwritten: the user edits it). The template is written as two raw string
// literals (fr, then de/es/it/pt) concatenated into one std::string.
bool CreateCorrectionsTemplate (const std::wstring& root, std::wstring& error)
{
	const std::wstring path = CorrectionsPath (root);
	if (FileExists (path))
		return true;
	const std::string model = std::string (R"JSON({
  "_explanation": "Term corrections applied to ONLINE translations (not to dictionaries), per output language. Save: open windows and the cache follow at once. A rule: source = word(s) of the original that trigger the rule (optional); wrong -> correct (in fr, es, pt the -s plural follows by itself) or replace = { wrong: correct, ... }. Longer expressions are applied first: put agreements there ('toutes les histoires' -> 'tous les etages'). To correct a whole sentence, use dictionaries\\_user.json instead.",
  "fr": [
    {"source": ["story", "stories"], "replace": {
      "toutes les histoires": "tous les étages", "les histoires": "les étages", "une histoire": "un étage",
      "cette histoire": "cet étage", "nouvelle histoire": "nouvel étage", "histoire actuelle": "étage actuel",
      "histoire sélectionnée": "étage sélectionné", "histoires sélectionnées": "étages sélectionnés",
      "histoire de la maison": "étage de référence", "histoire d'origine": "étage de référence",
      "histoires": "étages", "histoire": "étage"}},
    {"source": ["slab", "slabs"], "replace": {"plaques": "dalles", "plaque": "dalle"}},
    {"source": ["beam", "beams"], "replace": {
      "un faisceau": "une poutre", "le faisceau": "la poutre", "ce faisceau": "cette poutre",
      "faisceaux": "poutres", "faisceau": "poutre"}},
    {"source": ["layer", "layers"], "replace": {
      "une couche": "un calque", "la couche": "le calque", "cette couche": "ce calque",
      "toutes les couches": "tous les calques", "couches": "calques", "couche": "calque"}},
    {"source": ["pen", "pens"], "replace": {"plumes": "stylos", "plume": "stylo", "stylets": "stylos", "stylet": "stylo"}},
    {"source": ["fill", "fills"], "replace": {"remplissages": "hachures", "remplissage": "hachure"}},
    {"source": ["railing", "railings"], "replace": {"rampes": "garde-corps", "rampe": "garde-corps"}},
    {"source": ["curtain wall", "curtain walls"], "replace": {
      "murs de rideau": "murs-rideaux", "mur de rideau": "mur-rideau", "murs-rideau": "murs-rideaux"}},
    {"source": ["morph", "morphs"], "replace": {"morphes": "formes", "morphe": "forme", "morphs": "formes", "morph": "forme"}},
    {"source": ["mesh", "meshes"], "replace": {"mailles": "maillages", "maille": "maillage"}},
    {"source": ["layout", "layouts"], "replace": {"dispositions": "mises en page", "disposition": "mise en page"}}
  ],
)JSON") + std::string (R"JSON(  "de": [
    {"source": ["story", "stories"], "replace": {
      "Geschichten": "Geschosse", "Geschichte": "Geschoss", "Stockwerke": "Geschosse", "Stockwerk": "Geschoss"}},
    {"source": ["slab", "slabs"], "replace": {"Platten": "Decken", "Platte": "Decke"}},
    {"source": ["layer", "layers"], "replace": {"Schichten": "Ebenen", "Schicht": "Ebene"}},
    {"source": ["beam", "beams"], "replace": {"Strahlen": "Unterzüge", "Strahl": "Unterzug"}}
  ],
  "es": [
    {"source": ["story", "stories"], "replace": {"historias": "plantas", "historia": "planta"}},
    {"source": ["beam", "beams"], "replace": {"haces": "vigas", "haz": "viga"}}
  ],
  "it": [
    {"source": ["story", "stories"], "replace": {"storie": "piani", "storia": "piano"}},
    {"source": ["slab", "slabs"], "replace": {"lastre": "solai", "lastra": "solaio"}},
    {"source": ["layer", "layers"], "replace": {"strati": "layer", "strato": "layer"}},
    {"source": ["beam", "beams"], "replace": {"fasci": "travi", "fascio": "trave"}}
  ],
  "pt": [
    {"source": ["story", "stories"], "replace": {"histórias": "pisos", "história": "piso"}},
    {"source": ["beam", "beams"], "replace": {"feixes": "vigas", "feixe": "viga"}}
  ]
}
)JSON");
	CreateFolder (root);
	return WriteWholeFile (path, model, error);
}


// Writes <root>\settings.json atomically (the layer re-reads it every 0.3 s).
// Returns false, with 'error' set, if a language code is unknown or the write fails.
bool WriteSettings (const std::wstring& root, const Settings& settings, std::wstring& error)
{
	// Step 1: refuse unknown input / output languages.
	if (!KnownLanguage (settings.language) || !KnownLanguage (settings.input)) {
		error = L"unknown language: " + FromUtf8 (settings.input) + L" -> " + FromUtf8 (settings.language);
		return false;
	}
	// Step 2: build the JSON by hand. An unknown translator falls back to "mymemory" and an
	// unknown tool language to "" (same as ArchiCAD). "port" tells the layer where the engine
	// listens; "written_by" records the writing version.
	CreateFolder (root);
	std::string json = "{\n";
	json += "  \"input\": \"" + settings.input + "\",\n";
	json += "  \"output\": \"" + settings.language + "\",\n";
	json += std::string ("  \"collect\": ") + (settings.collect ? "true" : "false") + ",\n";
	json += std::string ("  \"online\": ") + (settings.engine ? "true" : "false") + ",\n";
	json += "  \"provider\": \"" + (KnownProvider (settings.provider) ? settings.provider : std::string ("mymemory")) + "\",\n";
	json += "  \"interface\": \"" + (KnownLanguage (settings.interface) ? settings.interface : std::string ()) + "\",\n";
	json += "  \"port\": " + std::to_string (EnginePort) + ",\n";
	json += std::string ("  \"written_by\": \"Tapir Translator ") + AddOnVersion + "\"\n";
	json += "}\n";
	// Step 3: atomic write.
	return WriteWholeFile (Join (root, L"settings.json"), json, error);
}


// =============================================================================
//  Process environment (inherited by the scripts that Tapir launches)
// =============================================================================

// ASCII lower-case copy of a string, for case-insensitive path comparison (Windows paths).
static std::wstring Lowercase (std::wstring s)
{
	for (wchar_t& c : s) {
		if (c >= L'A' && c <= L'Z')
			c = static_cast<wchar_t> (c - L'A' + L'a');
	}
	return s;
}


// Sets an environment variable of the current process in both places where it can be read.
// Returns the result of SetEnvironmentVariableW (the one that matters for Tapir).
static bool SetEnvVar (const wchar_t* name, const std::wstring& value)
{
	// SetEnvironmentVariableW: the process environment block (inherited through CreateProcess,
	// which Tapir uses). _wputenv_s: the CRT's own copy, for a launcher that would go through
	// the CRT.
	const bool ok = SetEnvironmentVariableW (name, value.c_str ()) != 0;
	_wputenv_s (name, value.c_str ());
	return ok;
}


// Prepares ArchiCAD's environment so that every Python script launched by Tapir imports the
// layer (see TT_Core.hpp). Returns false if a variable could not be set.
bool PrepareEnvironment (const std::wstring& root, std::wstring& pythonPath)
{
	// Step 1: current PYTHONPATH of ArchiCAD (possibly empty).
	const std::wstring layer = Join (root, L"layer");
	const std::wstring old = GetEnvVar (L"PYTHONPATH");

	// Step 2: is the layer folder already in it? Each ';'-separated entry is compared
	// case-insensitively, ignoring trailing slashes, so the entry is never added twice.
	bool alreadyThere = false;
	size_t start = 0;
	while (start <= old.size ()) {
		size_t end_ = old.find (L';', start);
		if (end_ == std::wstring::npos)
			end_ = old.size ();
		std::wstring piece = old.substr (start, end_ - start);
		while (!piece.empty () && (piece.back () == L'\\' || piece.back () == L'/'))
			piece.pop_back ();
		if (!piece.empty () && Lowercase (piece) == Lowercase (layer))
			alreadyThere = true;
		start = end_ + 1;
	}

	// Step 3: otherwise put it first, ahead of the previous entries (which are kept after it),
	// so that Python's site module imports our sitecustomize.py at interpreter start-up.
	pythonPath = alreadyThere ? old : (old.empty () ? layer : layer + L";" + old);
	// Step 4: set PYTHONPATH and TAPIR_TRANSLATOR_DIR (the layer finds its settings,
	// dictionaries and journal there). Both are attempted even if the first one fails.
	bool ok = SetEnvVar (L"PYTHONPATH", pythonPath);
	ok = SetEnvVar (L"TAPIR_TRANSLATOR_DIR", root) && ok;
	return ok;
}


bool PortAnswers (int port, int timeoutMs)
{
	// Step 1: Winsock is reference-counted: one WSAStartup per call, balanced by WSACleanup.
	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0)
		return false;
	bool answers = false;
	SOCKET s = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s != INVALID_SOCKET) {
		// Step 2: non-blocking connect to 127.0.0.1:<port>, then wait at most timeoutMs for it to
		// complete: success = a server is listening on that port.
		u_long nonBlocking = 1;
		ioctlsocket (s, FIONBIO, &nonBlocking);
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons (static_cast<u_short> (port));
		inet_pton (AF_INET, "127.0.0.1", &address.sin_addr);
		connect (s, reinterpret_cast<sockaddr*> (&address), sizeof (address));
		fd_set written, failed;
		FD_ZERO (&written);
		FD_ZERO (&failed);
		FD_SET (s, &written);
		FD_SET (s, &failed);
		timeval delay = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
		answers = select (0, nullptr, &written, &failed, &delay) > 0 && FD_ISSET (s, &written) && !FD_ISSET (s, &failed);
		closesocket (s);
	}
	WSACleanup ();
	return answers;
}


// Replaces every JSON key "old" (followed by optional blanks and a colon) by "new" in 'json'.
// Returns true if something changed. Values are never touched.
static bool RenameJsonKeys (std::string& json, std::initializer_list<std::pair<const char*, const char*>> keys)
{
	bool changed = false;
	for (const auto& k : keys) {
		const std::string oldKey = std::string ("\"") + k.first + "\"";
		const std::string newKey = std::string ("\"") + k.second + "\"";
		size_t pos = 0;
		while ((pos = json.find (oldKey, pos)) != std::string::npos) {
			// Step 1: it is a key only if the next non-blank character is a colon.
			size_t next = pos + oldKey.size ();
			while (next < json.size () && (json[next] == ' ' || json[next] == '\t' || json[next] == '\r' || json[next] == '\n'))
				++next;
			if (next < json.size () && json[next] == ':') {
				json.replace (pos, oldKey.size (), newKey);
				pos += newKey.size ();
				changed = true;
			} else {
				pos += oldKey.size ();
			}
		}
	}
	return changed;
}

// Rewrites the file 'path' with its keys renamed (see RenameJsonKeys); traced when it changed.
static void MigrateJsonFile (const std::wstring& path, std::initializer_list<std::pair<const char*, const char*>> keys,
							 std::vector<std::wstring>& journal)
{
	std::string json;
	std::wstring error;
	if (ReadWholeFile (path, json) && RenameJsonKeys (json, keys) && WriteWholeFile (path, json, error))
		journal.push_back (L"converted to the current format: " + path);
}

// Renames 'from' to 'to' (file or folder) when 'from' exists and 'to' does not; traced.
static void MigrateName (const std::wstring& from, const std::wstring& to, std::vector<std::wstring>& journal)
{
	const bool exists = FileExists (from) || FolderExists (from);
	if (exists && !FileExists (to) && !FolderExists (to) && MoveFileExW (from.c_str (), to.c_str (), MOVEFILE_COPY_ALLOWED) != 0)
		journal.push_back (L"renamed: " + from + L" -> " + to);
}

void MigrateOldData (const std::wstring& root, std::vector<std::wstring>& journal)
{
	if (root.empty () || !FolderExists (root))
		return;
	// Step 1: file and folder names of versions up to 0.7.5.
	MigrateName (Join (root, L"reglages.json"), Join (root, L"settings.json"), journal);
	MigrateName (Join (root, L"cles.json"), Join (root, L"keys.json"), journal);
	MigrateName (Join (root, L"journal.txt"), Join (root, L"log.txt"), journal);
	MigrateName (Join (root, L"dictionnaires"), Join (root, L"dictionaries"), journal);
	MigrateName (Join (Join (root, L"dictionaries"), L"_perso.json"), Join (Join (root, L"dictionaries"), L"_user.json"), journal);
	MigrateName (Join (root, L"manquants"), Join (root, L"missing"), journal);
	// The old common dictionary belongs to the add-on (rewritten as _common.json at every start).
	const std::wstring oldCommon = Join (Join (root, L"dictionaries"), L"_commun.json");
	if (FileExists (oldCommon) && DeleteFileW (oldCommon.c_str ()) != 0)
		journal.push_back (L"old common dictionary deleted: " + oldCommon);
	if (FolderExists (Join (root, L"couche"))) {
		DeleteFolder (Join (root, L"couche"));	// the layer is rewritten in layer\ at every start
		journal.push_back (L"old layer folder deleted: " + Join (root, L"couche"));
	}
	// Step 2: JSON keys. Settings (the output value "origine" of the oldest versions is read as
	// "no translation" by the layer anyway: only keys are renamed).
	MigrateJsonFile (Join (root, L"settings.json"),
					 { { "entree", "input" }, { "langue", "output" }, { "collecte", "collect" }, { "moteur", "online" },
					   { "fournisseur", "provider" }, { "ecrit_par", "written_by" } }, journal);
	MigrateJsonFile (Join (root, L"keys.json"), { { "cle", "key" }, { "_explication", "_explanation" }, { "_ou", "_where" } }, journal);
	MigrateJsonFile (Join (root, L"corrections.json"),
					 { { "mauvais", "wrong" }, { "remplacer", "replace" }, { "_explication", "_explanation" } }, journal);
	// Dictionaries and missing texts share the dictionary keys (format 1 and format 2).
	for (const wchar_t* folder : { L"dictionaries", L"missing" }) {
		for (const std::wstring& name : ListJson (Join (root, folder))) {
			MigrateJsonFile (Join (Join (root, folder), name),
							 { { "langue_source", "source_language" }, { "traductions", "translations" }, { "termes", "terms" },
							   { "langues", "languages" }, { "_explication", "_explanation" } }, journal);
		}
	}
	// Cache: cache\<translator>\<source>-<target>.json (and the oldest layout, cache\*.json).
	const std::wstring cache = Join (root, L"cache");
	std::vector<std::wstring> cacheFolders { cache };
	for (const std::wstring& name : ListFolders (cache))
		cacheFolders.push_back (Join (cache, name));
	for (const std::wstring& folder : cacheFolders) {
		for (const std::wstring& name : ListJson (folder))
			MigrateJsonFile (Join (folder, name), { { "cible", "target" }, { "fournisseur", "provider" }, { "traductions", "translations" } }, journal);
	}
}

}	// namespace TT
