// *****************************************************************************
// Tapir Translator - engine: model of the term corrections (see Corrections.hpp)
//
//   Text matching works on UTF-16 (Win32): lower / upper case through
//   LCMapStringEx (invariant locale, handles accented letters), word characters
//   through IsCharAlphaNumericW plus "_" - the same notion of word as Python's
//   \w used by the layer.
// *****************************************************************************

#include "Corrections.hpp"
#include "Json.hpp"

#if !defined (NOMINMAX)
	#define NOMINMAX	// std::min / std::max instead of the windows.h macros
#endif
#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <set>

namespace Corrections {

namespace {

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

// Lower or upper case of a UTF-16 string (same length: LCMAP_LOWERCASE / UPPERCASE map
// character by character).
std::wstring MapCase (const std::wstring& s, DWORD flag)
{
	if (s.empty ())
		return s;
	std::wstring r (s.size (), L'\0');
	LCMapStringEx (LOCALE_NAME_INVARIANT, flag, s.data (), static_cast<int> (s.size ()), &r[0], static_cast<int> (r.size ()),
				   nullptr, nullptr, 0);
	return r;
}

// Word character, like Python's \w: letter, digit (any script) or underscore.
bool WordChar (wchar_t c)
{
	return c == L'_' || IsCharAlphaNumericW (c) != FALSE;
}

// Position of the first whole-word occurrence of 'word' in 'text' at or after 'from' (both
// already in lower case), or npos.
size_t FindWord (const std::wstring& text, const std::wstring& word, size_t from)
{
	if (word.empty ())
		return std::wstring::npos;
	for (size_t p = text.find (word, from); p != std::wstring::npos; p = text.find (word, p + 1)) {
		const bool before = p == 0 || !WordChar (text[p - 1]);
		const bool after = p + word.size () >= text.size () || !WordChar (text[p + word.size ()]);
		if (before && after)
			return p;
	}
	return std::wstring::npos;
}

// Case style of the layer's _same_case: 'matched' ALL CAPS (2+ characters) -> 'correct' in
// upper case; 'matched' with an initial capital -> initial capital; otherwise unchanged.
std::wstring SameCase (const std::wstring& matched, const std::wstring& correct)
{
	bool hasUpper = false, hasLower = false;
	for (wchar_t c : matched) {
		hasUpper = hasUpper || IsCharUpperW (c) != FALSE;
		hasLower = hasLower || IsCharLowerW (c) != FALSE;
	}
	if (hasUpper && !hasLower && matched.size () > 1)
		return MapCase (correct, LCMAP_UPPERCASE);
	if (!matched.empty () && IsCharUpperW (matched[0]) && !correct.empty ())
		return MapCase (correct.substr (0, 1), LCMAP_UPPERCASE) + correct.substr (1);
	return correct;
}

// Length of re.escape (wrong) in Python 3.7+ (special characters get a backslash): the layer
// sorts its patterns by that length, so the order of application is the same here.
size_t EscapedLength (const std::wstring& s)
{
	static const std::wstring special = L"()[]{}?*+-|^$\\.&~# \t\n\r\v\f";
	size_t n = s.size ();
	for (wchar_t c : s)
		n += special.find (c) != std::wstring::npos ? 1 : 0;
	return n;
}

// One compiled replacement (a pair of a rule, with the rule's sources), UTF-16 lower case.
struct Compiled {
	std::vector<std::wstring> sources;	// lower case; empty = no condition
	std::wstring wrong;					// lower case
	std::wstring correct;				// as typed
	size_t order = 0;					// EscapedLength (wrong): longest first
};

// The layer's _compile_corrections: every pair of every rule (+ automatic -s plural for a
// single "wrong"/"correct" word in fr, es and pt), longest wrong term first (stable sort).
std::vector<Compiled> Compile (const std::vector<Rule>& rules, const std::string& language)
{
	std::vector<Compiled> res;
	const bool plural = language == "fr" || language == "es" || language == "pt";
	for (const Rule& r : rules) {
		std::vector<std::wstring> sources;
		for (const std::string& s : r.sources) {
			if (!s.empty ())
				sources.push_back (MapCase (Wide (s), LCMAP_LOWERCASE));
		}
		std::vector<Pair> pairs = r.pairs;
		if (!r.replaceForm && pairs.size () == 1 && plural) {
			const Pair& p = pairs[0];
			if (!p.wrong.empty () && p.wrong.find (' ') == std::string::npos && p.correct.find (' ') == std::string::npos &&
				p.wrong.back () != 's')
				pairs.push_back ({ p.wrong + "s", p.correct + "s" });
		}
		for (const Pair& p : pairs) {
			if (p.wrong.empty ())
				continue;
			Compiled c;
			c.sources = sources;
			c.wrong = MapCase (Wide (p.wrong), LCMAP_LOWERCASE);
			c.correct = Wide (p.correct);
			c.order = EscapedLength (Wide (p.wrong));
			res.push_back (c);
		}
	}
	std::stable_sort (res.begin (), res.end (), [] (const Compiled& a, const Compiled& b) { return a.order > b.order; });
	return res;
}

// Reads a whole file (raw bytes).
bool ReadAll (const std::wstring& path, std::string& content)
{
	HANDLE h = CreateFileW (path.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
							FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	content.clear ();
	char buffer[65536];
	DWORD read = 0;
	while (ReadFile (h, buffer, sizeof (buffer), &read, nullptr) && read > 0)
		content.append (buffer, read);
	CloseHandle (h);
	if (content.compare (0, 3, "\xEF\xBB\xBF") == 0)
		content.erase (0, 3);
	return true;
}

// Names of the entries of a folder matching 'pattern' (FindFirstFileW), folders or files.
std::vector<std::wstring> List (const std::wstring& folder, const wchar_t* pattern, bool folders)
{
	std::vector<std::wstring> res;
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW ((folder + L"\\" + pattern).c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return res;
	do {
		const bool isFolder = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		const std::wstring name = d.cFileName;
		if (isFolder == folders && name != L"." && name != L"..")
			res.push_back (name);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	return res;
}

}	// namespace


std::vector<std::string> SplitSources (const std::string& text)
{
	std::vector<std::string> res;
	size_t start = 0;
	while (start <= text.size ()) {
		size_t end = text.find (',', start);
		if (end == std::string::npos)
			end = text.size ();
		std::string word = text.substr (start, end - start);
		while (!word.empty () && (word.front () == ' ' || word.front () == '\t'))
			word.erase (0, 1);
		while (!word.empty () && (word.back () == ' ' || word.back () == '\t'))
			word.pop_back ();
		if (!word.empty ())
			res.push_back (word);
		start = end + 1;
	}
	return res;
}


std::string JoinSources (const std::vector<std::string>& sources)
{
	std::string res;
	for (const std::string& s : sources)
		res += (res.empty () ? "" : ", ") + s;
	return res;
}


bool Load (const std::wstring& path, File& file, std::string& error)
{
	file = File ();
	// Step 1: a missing file is an empty set of rules (the add-on normally creates it).
	std::string content;
	if (!ReadAll (path, content))
		return true;
	Json::Value root;
	if (!Json::Reader (content).Read (root) || root.kind != Json::Value::Object) {
		error = "invalid JSON";
		return false;
	}
	// Step 2: the explanation, then one array of rules per output language.
	file.explanation = root.Text ("_explanation");
	for (const auto& entry : root.object) {
		if (entry.first.empty () || entry.first[0] == '_' || entry.second.kind != Json::Value::Array)
			continue;
		std::vector<Rule>& rules = file.languages[entry.first];
		for (const Json::Value& r : entry.second.array) {
			if (r.kind != Json::Value::Object)
				continue;
			// Step 3: "source" = one word or a list of words.
			Rule rule;
			if (const Json::Value* s = r.Field ("source")) {
				if (s->kind == Json::Value::String && !s->str.empty ())
					rule.sources.push_back (s->str);
				for (const Json::Value& w : s->array) {
					if (w.kind == Json::Value::String && !w.str.empty ())
						rule.sources.push_back (w.str);
				}
			}
			// Step 4: "replace" mapping, else the "wrong" / "correct" pair.
			const Json::Value* replace = r.Field ("replace");
			if (replace != nullptr && replace->kind == Json::Value::Object) {
				rule.replaceForm = true;
				for (const auto& p : replace->object) {
					if (!p.first.empty () && p.second.kind == Json::Value::String)
						rule.pairs.push_back ({ p.first, p.second.str });
				}
			} else if (!r.Text ("wrong").empty ()) {
				rule.pairs.push_back ({ r.Text ("wrong"), r.Text ("correct") });
			}
			if (!rule.pairs.empty ())
				rules.push_back (rule);
		}
	}
	return true;
}


std::string Serialize (const File& file)
{
	std::string json = "{\n  \"_explanation\": " + Json::String (file.explanation);
	for (const auto& language : file.languages) {
		if (language.second.empty ())
			continue;
		json += ",\n  " + Json::String (language.first) + ": [";
		bool first = true;
		for (const Rule& r : language.second) {
			if (r.pairs.empty ())
				continue;
			json += std::string (first ? "" : ",") + "\n    {";
			first = false;
			// "source": a string for one word, a list for several, nothing for none.
			std::string fields;
			if (r.sources.size () == 1) {
				fields = "\"source\": " + Json::String (r.sources[0]);
			} else if (r.sources.size () > 1) {
				fields = "\"source\": [";
				for (size_t i = 0; i < r.sources.size (); ++i)
					fields += (i == 0 ? "" : ", ") + Json::String (r.sources[i]);
				fields += "]";
			}
			if (r.replaceForm || r.pairs.size () > 1) {
				fields += std::string (fields.empty () ? "" : ", ") + "\"replace\": {";
				for (size_t i = 0; i < r.pairs.size (); ++i)
					fields += (i == 0 ? "" : ", ") + Json::String (r.pairs[i].wrong) + ": " + Json::String (r.pairs[i].correct);
				fields += "}";
			} else {
				fields += std::string (fields.empty () ? "" : ", ") + "\"wrong\": " + Json::String (r.pairs[0].wrong) +
						  ", \"correct\": " + Json::String (r.pairs[0].correct);
			}
			json += fields + "}";
		}
		json += "\n  ]";
	}
	return json + "\n}\n";
}


bool Save (const std::wstring& path, const File& file, std::string& error)
{
	// Step 1: write everything into <path>.tmp ...
	const std::string json = Serialize (file);
	const std::wstring tmp = path + L".tmp";
	HANDLE h = CreateFileW (tmp.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		error = "cannot create " + Narrow (tmp) + " (Windows error " + std::to_string (GetLastError ()) + ")";
		return false;
	}
	DWORD written = 0;
	const bool ok = WriteFile (h, json.data (), static_cast<DWORD> (json.size ()), &written, nullptr) && written == json.size ();
	CloseHandle (h);
	// Step 2: ... then replace the real file in one move.
	if (!ok || !MoveFileExW (tmp.c_str (), path.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		error = "cannot write " + Narrow (path) + " (Windows error " + std::to_string (GetLastError ()) + ")";
		DeleteFileW (tmp.c_str ());
		return false;
	}
	return true;
}


std::string Apply (const std::vector<Rule>& rules, const std::string& language, const std::string& original,
				   const std::string& translated)
{
	if (translated.empty () || rules.empty ())
		return translated;
	const std::wstring originalLower = MapCase (Wide (original), LCMAP_LOWERCASE);
	std::wstring text = Wide (translated);
	for (const Compiled& c : Compile (rules, language)) {
		// Step 1: the rule's "source" condition on the original text.
		if (!c.sources.empty ()) {
			bool found = false;
			for (const std::wstring& s : c.sources)
				found = found || FindWord (originalLower, s, 0) != std::wstring::npos;
			if (!found)
				continue;
		}
		// Step 2: replace every whole-word occurrence, left to right, keeping the case style.
		std::wstring lower = MapCase (text, LCMAP_LOWERCASE);
		std::wstring result;
		size_t from = 0;
		for (size_t p = FindWord (lower, c.wrong, 0); p != std::wstring::npos; p = FindWord (lower, c.wrong, from)) {
			result += text.substr (from, p - from) + SameCase (text.substr (p, c.wrong.size ()), c.correct);
			from = p + c.wrong.size ();
		}
		if (from > 0)
			text = result + text.substr (from);
	}
	return Narrow (text);
}


std::vector<CachedTranslation> ReadCache (const std::wstring& root, const std::string& language)
{
	std::vector<CachedTranslation> res;
	if (language.empty ())
		return res;
	// Step 1: the folders to scan: cache\<translator> and cache itself (oldest layout).
	const std::wstring cache = root + L"\\cache";
	std::vector<std::pair<std::wstring, std::string>> folders { { cache, std::string () } };
	for (const std::wstring& name : List (cache, L"*", true))
		folders.push_back ({ cache + L"\\" + name, Narrow (name) });
	// Step 2: every <source>-<language>.json file: its "translations" table.
	const std::wstring suffix = L"-" + Wide (language) + L".json";
	std::set<std::string> seen;
	for (const auto& folder : folders) {
		for (const std::wstring& name : List (folder.first, (L"*" + suffix).c_str (), false)) {
			std::string content;
			Json::Value v;
			if (!ReadAll (folder.first + L"\\" + name, content) || !Json::Reader (content).Read (v))
				continue;
			const Json::Value* table = v.Field ("translations");
			if (table == nullptr)
				table = v.Field ("traductions");	// cache written by versions <= 0.7.5
			if (table == nullptr || table->kind != Json::Value::Object)
				continue;
			for (const auto& t : table->object) {
				if (t.second.kind == Json::Value::String && !t.second.str.empty () && seen.insert (t.first).second)
					res.push_back ({ t.first, t.second.str, folder.second });
			}
		}
	}
	// Step 3: sorted by original text, case-insensitively.
	std::sort (res.begin (), res.end (), [] (const CachedTranslation& a, const CachedTranslation& b) {
		return MapCase (Wide (a.original), LCMAP_LOWERCASE) < MapCase (Wide (b.original), LCMAP_LOWERCASE);
	});
	return res;
}

}	// namespace Corrections
