// *****************************************************************************
// Tapir Translator - test bench outside ArchiCAD
//
//   Reproduces what ArchiCAD + the add-on + the Tapir palette do, using the
//   add-on's own code (Src/TT_Core.cpp, Src/TT_Payload.cpp, Src/TT_Texts.cpp)
//   instead of a copy of it:
//     1. Add-on environment preparation: ExtractLayer / WriteSettings /
//        PrepareEnvironment are run on a temporary folder (instead of
//        %APPDATA%\Tapir Translator), so the translation layer
//        (layer\sitecustomize.py), the dictionaries and the settings are
//        exactly what the add-on would write.
//     2. Launch of the fake script Tests/bench_tool.py through
//        "uv run --script <script> --port N", like TapirPalette::ExecuteScript,
//        with TAPIR_SCRIPT_ID set. Python then imports sitecustomize by itself.
//     3. Reading of the script's interface state: for every widget, the text
//        DISPLAYED by Tk (read in the real Tcl interpreter) and the text SEEN by
//        the script (read through tkinter, i.e. through the layer), while the
//        bench changes the languages, the dictionaries, the provider and the
//        keys on the fly.
//     4. A fake online translation server (FakeServer) that speaks the real
//        request/response formats of DeepL, Microsoft Translator, Google Cloud
//        Translation, MyMemory and LibreTranslate, so that the real translation
//        engine (TapirTranslatorEngine.exe) can be tested end to end without
//        network or real keys.
//
//   Scenarios (each prints its title, then one OK / ECHEC line per check):
//     [0]  payload (engine + model) appended at the end of the .apx: packing,
//          reading, extraction, versioning by fingerprint;
//     [1]  environment prepared by the add-on (layer, dictionaries, PYTHONPATH
//          chaining, settings);
//     [2]  script opened untranslated, then translated live (languages,
//          dictionaries, missing-text collection, script logic unaffected);
//     [3]  script launched with English already chosen (translated at creation);
//     [3b] script without its own dictionary: common multilingual dictionary;
//     [3c] script written in English (source language not declared), then with
//          an explicit input language;
//     [3d] automatic translation with the offline test engine (--test), cache;
//     [3e] the 5 online translators through the fake server and the real engine
//          (keys, hot-added key, per-provider cache, nothing sensitive sent);
//     [3f] term corrections (corrections.json) and the MyMemory quota / e-mail;
//     [4]  Python launched outside Tapir: the layer must do nothing;
//     [5]  layer log: no internal error;
//     [6]  language of Tapir Translator itself (texts table, ArchiCAD language
//          codes, settings, generated menus).
//
//   Output: one OK / ECHEC line per check, exit code 0 when everything passes.
//   Usage : TranslatorBench.exe   (uv must be in the PATH)
// *****************************************************************************

#include "TT_Payload.hpp"
#include "TT_Core.hpp"
#include "TT_Texts.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")

#include "Json.hpp"

#include <cstring>
#include <set>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// BENCH_TESTS_DIR (UTF-8 path of the Tests folder) is defined by CMakeLists.txt.
#if !defined (BENCH_TESTS_DIR)
	#error "BANC_DOSSIER_TESTS doit etre defini (voir CMakeLists.txt)"
#endif

namespace {

// Global counters of passed / failed checks (summary and exit code at the end of main).
int g_ok = 0;
int g_failures = 0;

// Records one check: prints "OK <name>" or "ECHEC <name> -> <detail>" and counts it.
// The output is flushed at once so that the progress is visible even if the bench hangs.
void Check (const std::string& name, bool condition, const std::string& detail = std::string ())
{
	if (condition) {
		++g_ok;
		std::printf ("  OK     %s\n", name.c_str ());
	} else {
		++g_failures;
		std::printf ("  ECHEC  %s%s%s\n", name.c_str (), detail.empty () ? "" : "  -> ", detail.c_str ());
	}
	std::fflush (stdout);
}

// Tests folder (where bench_tool.py and data\bench_tool.json live), as a Windows path:
// the compile-time UTF-8 path is converted to UTF-16 and its '/' turned into '\'.
std::wstring TestsFolder ()
{
	std::wstring d = TT::FromUtf8 (BENCH_TESTS_DIR);
	for (wchar_t& c : d) {
		if (c == L'/')
			c = L'\\';
	}
	return d;
}

// Writes a file with the add-on's own helper; a failure is only printed (the check that
// depends on the file will then fail with a clearer message).
void Write (const std::wstring& path, const std::string& content)
{
	std::wstring error;
	if (!TT::WriteWholeFile (path, content, error))
		std::printf ("  (ecriture impossible : %s)\n", TT::Utf8 (error).c_str ());
}

// Recursively deletes a folder and everything in it (used to start each run from a clean
// temporary folder and to clean up afterwards).
void DeleteTree (const std::wstring& folder)
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (TT::Join (folder, L"*").c_str (), &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = d.cFileName;
			if (name == L"." || name == L"..")
				continue;
			const std::wstring path = TT::Join (folder, name);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				DeleteTree (path);
			else
				DeleteFileW (path.c_str ());
		} while (FindNextFileW (h, &d));
		FindClose (h);
	}
	RemoveDirectoryW (folder.c_str ());
}


// =============================================================================
//  The running fake script
// =============================================================================

// One widget text as reported by bench_tool.py on a "V|<key>|<displayed>|<seen>" line.
struct Value {
	std::string displayed;	// read in the Tcl interpreter (what the user sees)
	std::string seen;			// read through the tkinter API (what the script logic receives)
};

// Drives one run of bench_tool.py:
//   - starts it like the Tapir palette (uv run --script), stdout/stderr on a pipe;
//   - a reader thread parses its output: each block of V| lines ended by FIN_ETAT
//     becomes the current state (key -> Value), any other line is kept in others_;
//   - commands are sent through <exchange folder>\command.txt ("upper", "end_").
// The destructor kills the process if it is still running.
class Tool {
public:
	~Tool () { Stop (); }

	// Starts the script; 'exchange' is the folder holding command.txt (BENCH_EXCHANGE).
	// Steps: 1. empty command.txt; 2. create the output pipe (write end inherited by the
	// child, read end not); 3. CreateProcess of the uv command line, without a console
	// window, with the current environment (PYTHONPATH, TAPIR_SCRIPT_ID...); 4. start the
	// reader thread.
	bool Launch (const std::wstring& script, const std::wstring& exchange)
	{
		exchange_ = exchange;
		Write (TT::Join (exchange_, L"command.txt"), "");
		SECURITY_ATTRIBUTES sa = { sizeof (sa), nullptr, TRUE };
		HANDLE writing = nullptr;
		if (!CreatePipe (&reading_, &writing, &sa, 0))
			return false;
		SetHandleInformation (reading_, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si = {};
		si.cb = sizeof (si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = writing;
		si.hStdError = writing;
		si.hStdInput = GetStdHandle (STD_INPUT_HANDLE);
		PROCESS_INFORMATION pi = {};
		// Same as TapirPalette::ExecuteScript: uv run --script <file> --port <n>
		std::wstring line = L"uv run --script \"" + script + L"\" --port 19723";
		std::vector<wchar_t> buffer (line.begin (), line.end ());
		buffer.push_back (L'\0');
		const BOOL ok = CreateProcessW (nullptr, buffer.data (), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		// The child has its own copy of the write end: close ours so that ReadFile ends
		// when the child exits.
		CloseHandle (writing);
		if (!ok)
			return false;
		CloseHandle (pi.hThread);
		process_ = pi.hProcess;
		reader_ = std::thread ([this] () { Read (); });
		return true;
	}

	// Sends a command to the script by writing command.txt (polled by the script every tick).
	void Command (const char* c) { Write (TT::Join (exchange_, L"command.txt"), c); }

	// Waits for a state that satisfies the condition (at most 'ms' milliseconds), polling
	// every 50 ms. Used because translation is asynchronous: the layer reloads settings and
	// dictionaries every 0.3 s and online translations arrive later.
	bool WaitFor (const std::function<bool (const std::map<std::string, Value>&)>& condition, int ms)
	{
		const auto end_ = std::chrono::steady_clock::now () + std::chrono::milliseconds (ms);
		while (std::chrono::steady_clock::now () < end_) {
			{
				std::lock_guard<std::mutex> lock (m_);
				if (!status_.empty () && condition (status_))
					return true;
			}
			std::this_thread::sleep_for (std::chrono::milliseconds (50));
		}
		return false;
	}

	// Waits (at most 'ms' milliseconds) for a non-state output line starting with 'start'
	// (e.g. "LAYER|active", "NEXT|ok").
	bool WaitForLine (const std::string& start, int ms)
	{
		const auto end_ = std::chrono::steady_clock::now () + std::chrono::milliseconds (ms);
		while (std::chrono::steady_clock::now () < end_) {
			if (SawLine (start))
				return true;
			std::this_thread::sleep_for (std::chrono::milliseconds (50));
		}
		return false;
	}

	// True if a non-state output line starting with 'start' has already been received.
	bool SawLine (const std::string& start)
	{
		std::lock_guard<std::mutex> lock (m_);
		for (const std::string& l : others_) {
			if (l.compare (0, start.size (), start) == 0)
				return true;
		}
		return false;
	}

	// Current value of one key of the last complete state ("<absent>" if never reported).
	Value Read (const std::string& key)
	{
		std::lock_guard<std::mutex> lock (m_);
		auto it = status_.find (key);
		return it == status_.end () ? Value { "<absent>", "<absent>" } : it->second;
	}

	// Asks the script to close ("end_") and waits up to 'ms' milliseconds for the process to
	// exit. Returns true and its exit code if it exited by itself; otherwise kills it and
	// returns false.
	bool Finish (DWORD& code, int ms)
	{
		Command ("fin");
		if (process_ == nullptr || WaitForSingleObject (process_, static_cast<DWORD> (ms)) != WAIT_OBJECT_0) {
			Stop ();
			return false;
		}
		GetExitCodeProcess (process_, &code);
		Stop ();
		return true;
	}

	// Copy of every non-state output line (LAYER|..., SUIVANT|..., Python tracebacks, ...).
	std::vector<std::string> Others ()
	{
		std::lock_guard<std::mutex> lock (m_);
		return others_;
	}

private:
	// Stops the run: kills the process if it is still alive, waits for the reader thread
	// and closes the handles. Safe to call several times.
	void Stop ()
	{
		if (process_ != nullptr) {
			if (WaitForSingleObject (process_, 0) != WAIT_OBJECT_0) {
				TerminateProcess (process_, 99);
				// uv's Python child may keep the pipe open: unblock the pending read
				CancelIoEx (reading_, nullptr);
			}
			CloseHandle (process_);
			process_ = nullptr;
		}
		if (reader_.joinable ())
			reader_.join ();
		if (reading_ != nullptr) {
			CloseHandle (reading_);
			reading_ = nullptr;
		}
	}

	// Reader thread: splits the script output into lines (CR removed) and
	//   - "V|<key>|<displayed>|<seen>" -> stored in a state being built (the seen part is
	//     everything after the second '|');
	//   - "FIN_ETAT"                   -> the state being built becomes the current state
	//     in one go, so a check never sees a half-written state;
	//   - anything else                -> appended to others_.
	// Ends when the pipe is closed (or the read is cancelled by Stop).
	void Read ()
	{
		std::string rest;
		std::map<std::string, Value> inProgress;
		char buffer[4096];
		DWORD read_ = 0;
		while (ReadFile (reading_, buffer, sizeof (buffer), &read_, nullptr) && read_ > 0) {
			rest.append (buffer, read_);
			size_t pos;
			while ((pos = rest.find ('\n')) != std::string::npos) {
				std::string line = rest.substr (0, pos);
				rest.erase (0, pos + 1);
				if (!line.empty () && line.back () == '\r')
					line.pop_back ();
				if (line.compare (0, 2, "V|") == 0) {
					const size_t a = line.find ('|', 2);
					const size_t b = a == std::string::npos ? a : line.find ('|', a + 1);
					if (b != std::string::npos)
						inProgress[line.substr (2, a - 2)] = Value { line.substr (a + 1, b - a - 1), line.substr (b + 1) };
				} else if (line == "FIN_ETAT") {
					std::lock_guard<std::mutex> lock (m_);
					status_ = inProgress;
					inProgress.clear ();
				} else {
					std::lock_guard<std::mutex> lock (m_);
					others_.push_back (line);
				}
			}
		}
	}

	// exchange_: folder of command.txt; process_: the uv process; reading_: read end of
	// the output pipe; reader_: reader thread; m_ protects status_ (last complete state) and
	// others_ (non-state lines), shared between the reader thread and the checks.
	std::wstring exchange_;
	HANDLE process_ = nullptr;
	HANDLE reading_ = nullptr;
	std::thread reader_;
	std::mutex m_;
	std::map<std::string, Value> status_;
	std::vector<std::string> others_;
};


// Checks the displayed text of one key (and, if given, the text seen by the script) in
// the current state. One or two checks, labelled "<key> displayed ..." / "<key> seen ...".
void Text (Tool& o, const std::string& key, const std::string& displayed, const char* seen = nullptr)
{
	const Value v = o.Read (key);
	Check (key + " affiche \"" + displayed + "\"", v.displayed == displayed, "affiche \"" + v.displayed + "\"");
	if (seen != nullptr)
		Check (key + " vu par le script \"" + seen + "\"", v.seen == seen, "vu \"" + v.seen + "\"");
}

// Builds a condition for Tool::WaitFor: "the key is displayed with exactly this text".
std::function<bool (const std::map<std::string, Value>&)> Affiche (const std::string& key, const std::string& text)
{
	return [key, text] (const std::map<std::string, Value>& e) {
		auto it = e.find (key);
		return it != e.end () && it->second.displayed == text;
	};
}

// Does what the add-on menu does: writes settings.json with
//   code        output language ("" = no translation),
//   collect    collect untranslated texts into missing\,
//   input      input language ("" = automatic detection),
//   engine      online / automatic translation enabled,
//   provider translation provider ("test" = offline test engine, "deepl", ...).
// Two checks: the write succeeds, and ReadSettings reads back the same values. The running
// script's layer picks the new settings up by itself (reloaded every 0.3 s).
void Language (const std::wstring& root, const char* code, bool collect, const char* input = "", bool engine = false,
			 const char* provider = "test")
{
	TT::Settings r;
	r.input = input;
	r.language = code;
	r.collect = collect;
	r.engine = engine;
	r.provider = provider;
	std::wstring error;
	Check (std::string ("EcrireReglages entree=\"") + input + "\" sortie=\"" + code + "\"" + (engine ? " moteur" : ""),
			  TT::WriteSettings (root, r, error), TT::Utf8 (error));
	const TT::Settings reread = TT::ReadSettings (root);
	Check ("LireReglages relit les reglages", reread.input == r.input && reread.language == r.language && reread.collect == r.collect &&
			  reread.engine == r.engine && reread.provider == r.provider);
}

// Starts the translation engine TapirTranslatorEngine.exe found next to the bench, like the
// add-on does: <options> --port <port> --parent <bench pid> --log "<log>".
// Options used here: L"--test" (offline test engine, translation = "[<lang>] <text>"),
// L"--keys \"<keys.json>\"" (online providers), optionally with L"--no-dialog".
// Waits 500 ms for it to start listening and returns its process handle (nullptr on failure).
HANDLE StartEngine (int port, const std::wstring& journal, const std::wstring& options = L"--test")
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW (nullptr, exe, MAX_PATH);
	std::wstring path = exe;
	path = path.substr (0, path.find_last_of (L'\\') + 1) + L"TapirTranslatorEngine.exe";
	std::wstring line = L"\"" + path + L"\" " + options + L" --port " + std::to_wstring (port) + L" --parent " +
						 std::to_wstring (GetCurrentProcessId ()) + L" --log \"" + journal + L"\"";
	std::vector<wchar_t> buffer (line.begin (), line.end ());
	buffer.push_back (L'\0');
	STARTUPINFOW si = {};
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW (nullptr, buffer.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return nullptr;
	CloseHandle (pi.hThread);
	Sleep (500);
	return pi.hProcess;
}

// Fake online translation service (HTTP on 127.0.0.1, one request per connection).
// It emulates the request and response formats of DeepL, Microsoft Translator, Google Cloud
// Translation (v2), MyMemory and LibreTranslate, and answers "<PREFIX>:<target language>:<text>"
// (DL, MS, GG, MM, LT), so each check can tell which provider produced a translation.
// It also checks the keys the way the real services do (wrong or missing key -> HTTP error),
// records every text it receives (Received, to prove that dictionary texts and data are never
// sent online), can simulate the anonymous MyMemory quota (anonymousQuota), and mistranslates
// "All stories" into French like a real online translator would (for the term corrections).
// The engine is pointed at it through the TAPIR_TRANSLATOR_URL_<SERVICE> variables.
class FakeServer {
public:
	~FakeServer () { Stop (); }

	// Opens the listening socket on 127.0.0.1:<port> and starts the accept thread.
	bool Start (int port)
	{
		WSADATA wsa;
		WSAStartup (MAKEWORD (2, 2), &wsa);
		listener_ = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons (static_cast<u_short> (port));
		inet_pton (AF_INET, "127.0.0.1", &a.sin_addr);
		if (bind (listener_, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0 || listen (listener_, 16) != 0)
			return false;
		thread_ = std::thread ([this] () { Loop (); });
		return true;
	}

	// Closes the listening socket (which makes accept fail and ends the thread), then joins it.
	void Stop ()
	{
		if (listener_ != INVALID_SOCKET) {
			closesocket (listener_);
			listener_ = INVALID_SOCKET;
		}
		if (thread_.joinable ())
			thread_.join ();
	}

	// True if this exact text was ever sent to the fake service (by any provider route).
	bool Received (const std::string& text)
	{
		std::lock_guard<std::mutex> v (m_);
		return received_.count (text) > 0;
	}

private:
	// URL decoding of a query-string value: "%xx" -> byte, '+' -> space.
	static std::string Decoder (const std::string& s)
	{
		std::string r;
		for (size_t i = 0; i < s.size (); ++i) {
			if (s[i] == '%' && i + 2 < s.size ()) {
				r += static_cast<char> (std::strtol (s.substr (i + 1, 2).c_str (), nullptr, 16));
				i += 2;
			} else
				r += s[i] == '+' ? ' ' : s[i];
		}
		return r;
	}

	// Decoded value of the query-string parameter 'name' ("" if absent). The name must start
	// the query or follow '&' / '?', so that a short name (e.g. "to") never matches the end
	// of a longer parameter name.
	static std::string Parameter (const std::string& request, const std::string& name)
	{
		size_t p = 0;
		while ((p = request.find (name + "=", p)) != std::string::npos) {
			if (p == 0 || request[p - 1] == '&' || request[p - 1] == '?') {
				const size_t start = p + name.size () + 1;
				const size_t end_ = request.find ('&', start);
				return Decoder (request.substr (start, end_ == std::string::npos ? std::string::npos : end_ - start));
			}
			++p;
		}
		return std::string ();
	}

	// Records a received text (thread-safe), for Received.
	void Note (const std::string& t)
	{
		std::lock_guard<std::mutex> v (m_);
		received_.insert (t);
	}

	// Sends a complete HTTP/1.1 response with a JSON body and "Connection: close".
	static void Respond (SOCKET s, int code, const std::string& json)
	{
		const std::string r = "HTTP/1.1 " + std::to_string (code) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
							  std::to_string (json.size ()) + "\r\nConnection: close\r\n\r\n" + json;
		send (s, r.data (), static_cast<int> (r.size ()), 0);
	}

	// Accept loop: one connection at a time, one request per connection, then close.
	// Ends when the listening socket is closed by Stop.
	void Loop ()
	{
		for (;;) {
			SOCKET c = accept (listener_, nullptr, nullptr);
			if (c == INVALID_SOCKET)
				return;
			HandleRequest (c);
			shutdown (c, SD_BOTH);
			closesocket (c);
		}
	}

	// Handles one HTTP request:
	//   1. read until the end of the headers, then the body up to Content-Length;
	//   2. extract the path and the query string of the request target, parse the JSON body;
	//   3. dispatch on the path to the emulated provider and answer in its format.
	void HandleRequest (SOCKET c)
	{
		std::string t;
		char b[8192];
		size_t end_ = std::string::npos;
		while (end_ == std::string::npos) {
			const int n = recv (c, b, sizeof (b), 0);
			if (n <= 0)
				return;
			t.append (b, static_cast<size_t> (n));
			end_ = t.find ("\r\n\r\n");
		}
		// Header names are case-insensitive: they are searched in a lower-case copy.
		std::string headers = t.substr (0, end_);
		std::string body = t.substr (end_ + 4);
		std::string minus = headers;
		for (char& ch : minus)
			ch = static_cast<char> (std::tolower (static_cast<unsigned char> (ch)));
		const size_t cl = minus.find ("content-length:");
		const size_t length = cl == std::string::npos ? 0 : static_cast<size_t> (std::strtoul (minus.c_str () + cl + 15, nullptr, 10));
		while (body.size () < length) {
			const int n = recv (c, b, sizeof (b), 0);
			if (n <= 0)
				break;
			body.append (b, static_cast<size_t> (n));
		}
		// Value of a header (name given in lower case), taken from the original headers so
		// that its case is kept; "" if the header is absent.
		const auto header = [&] (const char* name) {
			const size_t p = minus.find (std::string ("\r\n") + name + ":");
			if (p == std::string::npos)
				return std::string ();
			size_t d = p + 3 + std::strlen (name);
			while (d < headers.size () && headers[d] == ' ')
				++d;
			return headers.substr (d, headers.find ("\r\n", d) - d);
		};
		// Request line "METHOD <target> HTTP/1.1": target = path [? query].
		const size_t sp1 = headers.find (' ');
		const std::string targetUrl = headers.substr (sp1 + 1, headers.find (' ', sp1 + 1) - sp1 - 1);
		const size_t q = targetUrl.find ('?');
		const std::string path = targetUrl.substr (0, q);
		const std::string request = q == std::string::npos ? std::string () : targetUrl.substr (q + 1);
		Json::Value v;
		Json::Reader (body).Read (v);

		// Translates a JSON array of texts (strings, or objects with a "Text" field) into a
		// comma-separated list of "<prefix>:<target>:<text>" JSON strings, each one wrapped in
		// {"<field>": ...} when 'field' is given. Every text is recorded (Note).
		const auto list = [&] (const Json::Value* tab, const char* prefix, const std::string& target, const char* field) {
			std::string r;
			if (tab != nullptr && tab->kind == Json::Value::Array) {
				for (const Json::Value& e : tab->array) {
					const std::string text = e.kind == Json::Value::String ? e.str : e.Text ("Text");
					Note (text);
					const std::string trans = Json::String (std::string (prefix) + ":" + target + ":" + text);
					r += std::string (r.empty () ? "" : ", ") + (field != nullptr ? "{\"" + std::string (field) + "\": " + trans + "}" : trans);
				}
			}
			return r;
		};

		// DeepL: /v2/translate, header "Authorization: DeepL-Auth-Key <key>",
		// body {"text": [...], "target_lang": ...} -> {"translations": [{"text": ...}]}.
		// Wrong key -> 403, like DeepL.
		if (path == "/v2/translate") {						// DeepL
			if (header ("authorization") != "DeepL-Auth-Key cle-deepl:fx")
				return Respond (c, 403, "{\"message\": \"Authorization failed\"}");
			return Respond (c, 200, "{\"translations\": [" + list (v.Field ("text"), "DL", v.Text ("target_lang"), "text") + "]}");
		}
		// Microsoft Translator: /translate?api-version=...&to=<lang>, header
		// "Ocp-Apim-Subscription-Key", body [{"Text": ...}, ...] ->
		// [{"translations": [{"text": ..., "to": ...}]}, ...]. Wrong key -> 401.
		// (Told apart from LibreTranslate, which uses the same path, by "api-version".)
		if (path == "/translate" && !Parameter (request, "api-version").empty ()) {	// Microsoft
			if (header ("ocp-apim-subscription-key") != "cle-ms")
				return Respond (c, 401, "{\"error\": {\"message\": \"invalid key\"}}");
			const std::string to = Parameter (request, "to");
			std::string r;
			for (const Json::Value& e : v.array) {
				Note (e.Text ("Text"));
				r += std::string (r.empty () ? "" : ", ") + "{\"translations\": [{\"text\": " + Json::String ("MS:" + to + ":" + e.Text ("Text")) +
					 ", \"to\": " + Json::String (to) + "}]}";
			}
			return Respond (c, 200, "[" + r + "]");
		}
		// Google Cloud Translation v2: /language/translate/v2?key=<key>, body {"q": [...],
		// "target": ...} -> {"data": {"translations": [{"translatedText": ...}]}}.
		// Wrong or missing key -> 400 "API key not valid".
		if (path == "/language/translate/v2") {				// Google
			if (Parameter (request, "key") != "cle-google")
				return Respond (c, 400, "{\"error\": {\"message\": \"API key not valid\"}}");
			return Respond (c, 200, "{\"data\": {\"translations\": [" + list (v.Field ("q"), "GG", v.Text ("target"), "translatedText") + "]}}");
		}
		// MyMemory: /get?q=<text>&langpair=<src>|<dst>[&de=<e-mail>], one text per request
		// -> {"responseData": {"translatedText": ...}, "responseStatus": 200}.
		// With anonymousQuota set and no e-mail ("de"), it answers like the real service once
		// the anonymous daily quota is used up: "MYMEMORY WARNING ..." with status 429.
		if (path == "/get") {									// MyMemory
			const std::string text = Parameter (request, "q");
			const std::string pair = Parameter (request, "langpair");
			Note (text);
			if (anonymousQuota && Parameter (request, "de").empty ())
				return Respond (c, 200, "{\"responseData\": {\"translatedText\": \"MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE "
										 "TRANSLATIONS FOR TODAY. NEXT AVAILABLE IN 10 HOURS\"}, \"responseStatus\": 429, \"quotaFinished\": true}");
			return Respond (c, 200, "{\"responseData\": {\"translatedText\": " + Json::String ("MM:" + pair.substr (pair.find ('|') + 1) + ":" + text) +
										 "}, \"responseStatus\": 200}");
		}
		// LibreTranslate: /translate (without api-version), body {"q": [...], "target": ...} ->
		// {"translatedText": [...]}. No key check (none is configured in the bench).
		if (path == "/translate") {							// LibreTranslate
			// Towards French, a word-for-word translation typical of online translators:
			// "All stories" -> "Toutes les histoires" ("story" -> "histoire" instead of
			// "étage"), used by scenario [3f] to test the term corrections.
			const Json::Value* q = v.Field ("q");
			if (v.Text ("target") == "fr" && q != nullptr && q->kind == Json::Value::Array) {
				std::string r;
				for (const Json::Value& e : q->array) {
					Note (e.str);
					r += std::string (r.empty () ? "" : ", ") +
						 Json::String (e.str == "All stories" ? std::string ("Toutes les histoires") : "LT:fr:" + e.str);
				}
				return Respond (c, 200, "{\"translatedText\": [" + r + "]}");
			}
			return Respond (c, 200, "{\"translatedText\": [" + list (v.Field ("q"), "LT", v.Text ("target"), nullptr) + "]}");
		}
		// Unknown route.
		Respond (c, 404, "{}");
	}

public:
	std::atomic<bool> anonymousQuota { false };	// MyMemory without e-mail: "MYMEMORY WARNING"

private:
	// Listening socket, accept thread, and the set of received texts (protected by m_).
	SOCKET listener_ = INVALID_SOCKET;
	std::thread thread_;
	std::mutex m_;
	std::set<std::string> received_;
};

// Kills the translation engine started by StartEngine (if any) and waits for it to exit.
void StopEngine (HANDLE& engine)
{
	if (engine != nullptr) {
		TerminateProcess (engine, 0);
		WaitForSingleObject (engine, 5000);
		CloseHandle (engine);
		engine = nullptr;
	}
}

// Prints the script's non-state output (e.g. a Python traceback), to help diagnose a failure.
void ShowOutput (Tool& o)
{
	for (const std::string& l : o.Others ())
		std::printf ("         | %s\n", l.c_str ());
}

// Prepares the add-on folder 'root' as the add-on does at ArchiCAD start-up, and checks it:
//   1. start from an empty folder, run ExtractLayer (layer, common dictionary, templates
//      of _user.json, keys.json and corrections.json) and check the expected files exist;
//   2. simulate an old installation (outdated _common.json, user-edited _user.json), run
//      ExtractLayer again and check that _common.json is replaced but _user.json is kept;
//   3. install the fake script's own dictionary (Tests\data\bench_tool.json) as
//      dictionaries\bench_tool.json (named after the .py launched by Tapir).
// Returns the content of that dictionary (scenario [2] edits it live).
std::string Prepare (const std::wstring& root)
{
	DeleteTree (root);
	std::vector<std::wstring> journal;
	Check ("ExtraireCouche", TT::ExtractLayer (root, journal));
	Check ("couche\\sitecustomize.py ecrit", TT::FileExists (TT::Join (TT::Join (root, L"layer"), L"sitecustomize.py")));
	Check ("dictionnaires\\_common.json ecrit", TT::FileExists (TT::Join (TT::Join (root, L"dictionaries"), L"_common.json")));
	Check ("dictionnaires\\_user.json cree", TT::FileExists (TT::Join (TT::Join (root, L"dictionaries"), L"_user.json")));
	// An old (modified) _common.json is replaced by the one of this version; _user.json is kept
	Write (TT::Join (TT::Join (root, L"dictionaries"), L"_common.json"), "{\"ancien\": true}");
	Write (TT::Join (TT::Join (root, L"dictionaries"), L"_user.json"), "{\"format\": 2, \"terms\": []}");
	{
		std::vector<std::wstring> j2;
		TT::ExtractLayer (root, j2);
		std::string c, p;
		TT::ReadWholeFile (TT::Join (TT::Join (root, L"dictionaries"), L"_common.json"), c);
		TT::ReadWholeFile (TT::Join (TT::Join (root, L"dictionaries"), L"_user.json"), p);
		Check ("ancien _common.json remplace par celui de la version", c.find ("\"terms\"") != std::string::npos);
		Check ("_user.json de l'utilisateur garde", p == "{\"format\": 2, \"terms\": []}");
	}
	std::string dictionary;
	TT::ReadWholeFile (TT::Join (TT::Join (TestsFolder (), L"data"), L"bench_tool.json"), dictionary);
	Write (TT::Join (TT::Join (root, L"dictionaries"), L"bench_tool.json"), dictionary);
	return dictionary;
}

}	// namespace


// Runs every scenario in order. Each one leaves the settings (and environment variables)
// in a known state for the next one. Scenarios [1] to [6] share one temporary folder,
//   %TEMP%\TapirTranslatorBench_<pid>, which plays the add-on folder (%APPDATA%\Tapir
//   Translator) and is also the exchange folder of the fake script (BENCH_EXCHANGE).
// It is deleted at the end if every check passed, and kept for examination otherwise.
int main ()
{
	// UTF-8 console (the check labels contain accented texts).
	SetConsoleOutputCP (CP_UTF8);
	wchar_t temp[MAX_PATH] = {};
	GetTempPathW (MAX_PATH, temp);
	const std::wstring root = TT::Join (temp, L"TapirTranslatorBench_" + std::to_wstring (GetCurrentProcessId ()));
	const std::wstring script = TT::Join (TestsFolder (), L"bench_tool.py");
	std::printf ("Tapir Translator %s - banc de test\n  dossier : %s\n  script  : %s\n\n", TT::AddOnVersion,
				 TT::Utf8 (root).c_str (), TT::Utf8 (script).c_str ());

	// --- [0] payload embedded at the end of the .apx (TT_Payload) ---------------------------
	// The build appends the engine, the model and the licenses to the .apx (Packer); the
	// add-on reads and extracts them once into a folder named after their fingerprint.
	// This block works on a fake .apx and a fake source folder:
	//   1. build the source: a fake engine, a file larger than the 1 MB copy buffer, and a
	//      file with an accented name in a sub-folder;
	//   2. pack twice: the old payload must be replaced (not appended again), same content
	//      must give the same fingerprint, and the original DLL content must stay intact;
	//   3. read the payload table and extract it; check the files byte for byte, and that a
	//      second extraction does nothing;
	//   4. change a file: new fingerprint, extracted next to the old version;
	//   5. a file without payload is rejected;
	//   6. finally, the real Tapir_Translator.apx built next to the bench must contain the
	//      engine.
	std::printf ("[0] Moteur et modele embarques dans le .apx\n");
	{
		const std::wstring folder = TT::Join (temp, L"TapirTranslatorCharge_" + std::to_wstring (GetCurrentProcessId ()));
		DeleteTree (folder);
		const std::wstring source = TT::Join (folder, L"source");
		TT::CreateFolder (TT::Join (source, L"modele\\sous"));
		std::string big (3 * 1024 * 1024 + 17, '\0');		// larger than the copy buffer (1 MB)
		for (size_t i = 0; i < big.size (); ++i)
			big[i] = static_cast<char> ((i * 31 + 7) & 0xFF);
		Write (TT::Join (source, L"engine.exe"), "MZ faux moteur");
		Write (TT::Join (source, L"modele\\poids.bin"), big);
		Write (TT::Join (source, L"modele\\sous\\vocabulaire é.txt"), "mur\nwall\n");
		// Fake .apx: stands for the DLL produced by the linker.
		const std::wstring apx = TT::Join (folder, L"faux.apx");
		const std::string apxContent = "faux contenu de DLL";
		Write (apx, apxContent);

		// Step 2: pack twice; the second pass must replace the first payload, so the file
		// size stays about DLL + payload (one copy of the big file, not two).
		std::wstring hash, hash2;
		std::vector<std::wstring> journal;
		Check ("Empaqueter", TT::Packer (apx, source, hash, journal));
		Check ("Empaqueter une seconde fois (ancienne charge retiree)", TT::Packer (apx, source, hash2, journal));
		Check ("meme contenu = meme empreinte", hash == hash2 && !hash.empty ());
		std::string apxRead;
		TT::ReadWholeFile (apx, apxRead);
		Check ("le debut du .apx est intact", apxRead.compare (0, apxContent.size (), apxContent) == 0);
		Check ("une seule charge apres deux passages", apxRead.size () < apxContent.size () + big.size () + 2000,
				  std::to_string (apxRead.size ()));
		// Step 3: payload table (3 files, same fingerprint), then extraction into
		// <base>\<fingerprint> and byte-for-byte comparison of the files.
		std::wstring e;
		std::vector<std::wstring> paths;
		Check ("LireCharge : 3 fichiers", TT::ReadPayload (apx, e, paths) && paths.size () == 3 && e == hash);

		std::wstring extracted;
		const std::wstring base = TT::Join (folder, L"extracted");
		Check ("ExtraireCharge", TT::ExtractPayload (apx, base, extracted, journal));
		Check ("dossier d'extraction = empreinte", extracted == TT::Join (base, hash), TT::Utf8 (extracted));
		std::string read_;
		Check ("gros fichier identique", TT::ReadWholeFile (TT::Join (extracted, L"modele\\poids.bin"), read_) && read_ == big);
		Check ("fichier a accent identique", TT::ReadWholeFile (TT::Join (extracted, L"modele\\sous\\vocabulaire é.txt"), read_) && read_ == "mur\nwall\n");
		// A second extraction of the same version must succeed and end its log with
		// "already extracted" (nothing copied again).
		journal.clear ();
		Check ("deuxieme extraction : rien a refaire", TT::ExtractPayload (apx, base, extracted, journal) &&
				  !journal.empty () && journal.back ().find (L"deja extrait") != std::wstring::npos);

		// Steps 4 and 5: a modified engine gives a new fingerprint and a new extraction
		// folder; a file without payload (here the fake engine itself) is rejected.
		Write (TT::Join (source, L"engine.exe"), "MZ faux moteur v2");
		std::wstring hash3;
		TT::Packer (apx, source, hash3, journal);
		Check ("nouvelle version = nouvelle empreinte", hash3 != hash);
		Check ("nouvelle version extraite a cote", TT::ExtractPayload (apx, base, extracted, journal) &&
				  TT::ReadWholeFile (TT::Join (extracted, L"engine.exe"), read_) && read_ == "MZ faux moteur v2");
		Check ("fichier sans charge refuse", !TT::ReadPayload (TT::Join (source, L"engine.exe"), e, paths));

		// Step 6: the real .apx produced by the build (same folder as the bench) must carry
		// TapirTranslatorEngine.exe in its payload.
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW (nullptr, exe, MAX_PATH);
		std::wstring realPath = exe;
		realPath = realPath.substr (0, realPath.find_last_of (L'\\') + 1) + L"Tapir_Translator.apx";
		paths.clear ();
		bool engineInside = false;
		if (TT::ReadPayload (realPath, e, paths)) {
			for (const std::wstring& c : paths)
				engineInside = engineInside || c == L"TapirTranslatorEngine.exe";
		}
		Check ("Tapir_Translator.apx contient TapirTranslatorEngine.exe", engineInside);
		DeleteTree (folder);
	}

	// --- [1] environment, like the add-on's Initialize () --------------------------------
	//   1. Prepare: layer and dictionaries extracted into the temporary add-on folder;
	//   2. an existing PYTHONPATH (here with its own sitecustomize.py, which prints
	//      "NEXT|ok") must be kept behind the layer folder, and the layer must chain to
	//      that other sitecustomize (checked in [2]);
	//   3. PrepareEnvironment must be idempotent (no duplicate entry when called again);
	//   4. the variables Tapir / the bench give the script: TAPIR_SCRIPT_ID (marks a Tapir
	//      script, without it the layer does nothing), UTF-8 output, BENCH_EXCHANGE;
	//   5. initial settings: no translation (original language), collection enabled.
	std::printf ("\n[1] Environnement prepare par l'add-on\n");
	std::string dictionary = Prepare (root);
	// An existing PYTHONPATH (with its own sitecustomize) must be kept and chained
	const std::wstring other = TT::Join (root, L"other_path");
	TT::CreateFolder (other);
	Write (TT::Join (other, L"sitecustomize.py"), "print('NEXT|ok', flush=True)\n");
	SetEnvironmentVariableW (L"PYTHONPATH", other.c_str ());
	std::wstring pythonPath;
	Check ("PreparerEnvironnement", TT::PrepareEnvironment (root, pythonPath));
	Check ("PYTHONPATH = couche;ancien", pythonPath == TT::Join (root, L"layer") + L";" + other, TT::Utf8 (pythonPath));
	std::wstring again;
	TT::PrepareEnvironment (root, again);
	Check ("PreparerEnvironnement idempotent", again == pythonPath, TT::Utf8 (again));
	SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", L"tapir-banc-1");
	SetEnvironmentVariableW (L"PYTHONIOENCODING", L"utf-8");
	SetEnvironmentVariableW (L"BENCH_EXCHANGE", root.c_str ());
	Language (root, "", true);
	Check ("LireReglages relit la langue d'origine + collecte", TT::ReadSettings (root).language.empty () && TT::ReadSettings (root).collect);

	// --- [2] main scenario: opened in its original language, then translated live ---------
	// The script uses its own dictionary (dictionaries\bench_tool.json) plus the common one.
	//   1. launch through uv; the layer must be loaded by Python (LAYER|active) and must
	//      chain the pre-existing sitecustomize (NEXT|ok); first state in French;
	//   2. switch to English while the window is open: every kind of displayed text is
	//      translated, the script still sees its French texts, data is left alone;
	//   3. the script changes its own texts while translated ("upper");
	//   4. collection: the untranslated text is noted in missing\bench_tool.json;
	//   5. the dictionary is completed while the script runs: applied live;
	//   6. German, back to the original language, "fr" (= source, nothing to do), English
	//      again: several live switches in a row;
	//   7. clean exit, no Python traceback.
	std::printf ("\n[2] Script ouvert sans traduction puis traduit en direct\n");
	{
		// Step 1: launch. The first "uv run" may have to prepare its environment, hence 30 s.
		Tool o;
		Check ("lancement uv run --script", o.Launch (script, root));
		Check ("couche chargee par Python (sitecustomize)", o.WaitForLine ("LAYER|active", 30000));
		Check ("sitecustomize existant chaine", o.WaitForLine ("NEXT|ok", 2000));
		Check ("premier etat recu", o.WaitFor (Affiche ("titre", "Outil de démonstration"), 10000));
		Text (o, "fermer", "Fermer", "Fermer");
		Text (o, "statut", "Prêt", "Prêt");

		// Step 2: English output (collection still on). Each Text checks the displayed
		// translation AND the original text still returned to the script.
		Language (root, "en", true);
		Check ("bascule en anglais en direct (< 5 s)", o.WaitFor (Affiche ("titre", "Demo tool"), 5000));
		Text (o, "titre", "Demo tool", "Outil de démonstration");
		Text (o, "statut", "Ready", "Prêt");
		Text (o, "fermer", "Close", "Fermer");
		Text (o, "nom", "Name:", "Nom :");
		Text (o, "murs", "Walls", "Murs");
		Text (o, "cadre", "Options", "Options");
		// Multi-line text (newlines escaped by the script), "{n}" pattern, and a text found
		// in no dictionary (no engine running: it stays as is).
		Text (o, "multi", "First line\\nSecond line", "Première ligne\\nDeuxième ligne");
		Text (o, "modele", "3 walls created", "3 murs créés");
		Text (o, "inconnu", "Texte inconnu", "Texte inconnu");
		// Menus, notebook tab, Treeview heading, Canvas text.
		Text (o, "menu_fichier", "File", "Fichier");
		Text (o, "menu_ouvrir", "Open...", "Ouvrir...");
		Text (o, "menu_quitter", "Quit", "Quitter");
		Text (o, "onglet", "General", "Général");
		Text (o, "entete", "Name", "Nom");
		Text (o, "canevas", "Floor plan", "Plan");
		Text (o, "ligne_arbre", "Murs", "Murs");			// data: never translated
		Text (o, "combo", "Murs Dalles", "Murs");			// list values: never translated
		Text (o, "saisie", "Murs", "Murs");				// user input: never translated
		// The script's own comparisons with its original texts still succeed.
		Text (o, "logique", "ok");

		// Step 3: the script sets new texts itself while the interface is translated; they
		// must be translated too, and the script must still read its own French values.
		o.Command ("maj");
		Check ("le script change son statut pendant la traduction", o.WaitFor (Affiche ("statut", "Done"), 3000));
		Text (o, "statut", "Done", "Terminé");
		Text (o, "nom", "Value", "Valeur");
		Text (o, "logique", "ok");

		// Step 4: with collection on, "Text unknown" must be written (with an empty
		// translation) into missing\bench_tool.json; polled for up to about 4 s.
		const std::wstring missing = TT::Join (TT::Join (root, L"missing"), L"bench_tool.json");
		bool note = false;
		for (int i = 0; i < 40 && !note; ++i) {
			std::string m;
			note = TT::ReadWholeFile (missing, m) && m.find ("\"Texte inconnu\": \"\"") != std::string::npos;
			if (!note)
				Sleep (100);
		}
		Check ("texte non traduit note dans manquants\\bench_tool.json", note);

		// Step 5: dictionary completed while the tool is running: the entry
		// "Text unknown" -> "Unknown text" is inserted after "Plan" in the script's
		// dictionary file, and the open window must pick it up by itself.
		const std::string key = "\"Plan\": \"Floor plan\"";
		const size_t pos = dictionary.find (key);
		Check ("dictionnaire du banc lisible", pos != std::string::npos);
		if (pos != std::string::npos) {
			dictionary.insert (pos + key.size (), ",\n      \"Texte inconnu\": \"Unknown text\"");
			Write (TT::Join (TT::Join (root, L"dictionaries"), L"bench_tool.json"), dictionary);
		}
		Check ("dictionnaire modifie applique en direct", o.WaitFor (Affiche ("inconnu", "Unknown text"), 5000));
		Text (o, "inconnu", "Unknown text", "Texte inconnu");

		// Step 6a: German (collection off). "Text unknown" has no German entry: original.
		Language (root, "de", false);
		Check ("bascule en allemand", o.WaitFor (Affiche ("titre", "Demo-Werkzeug"), 5000));
		Text (o, "fermer", "Schließen", "Fermer");
		Text (o, "modele", "3 Wände erstellt", "3 murs créés");
		Text (o, "murs", "Wände", "Murs");
		Text (o, "statut", "Fertig", "Terminé");
		Text (o, "nom", "Wert", "Valeur");					// common multilingual dictionary
		Text (o, "inconnu", "Texte inconnu", "Texte inconnu");

		// Step 6b: output language "" = no translation: every original text comes back.
		Language (root, "", false);
		Check ("retour a la langue d'original", o.WaitFor (Affiche ("titre", "Outil de démonstration"), 5000));
		Text (o, "fermer", "Fermer", "Fermer");
		Text (o, "statut", "Terminé", "Terminé");
		Text (o, "modele", "3 murs créés", "3 murs créés");
		Text (o, "menu_ouvrir", "Ouvrir...", "Ouvrir...");
		Text (o, "canevas", "Plan", "Plan");

		// Step 6c: output = French, the script's own language: nothing must change (no
		// state change to wait for, hence a fixed delay before the check).
		Language (root, "fr", false);
		Sleep (1500);
		Text (o, "titre", "Outil de démonstration");		// fr = source language: nothing to do

		// Step 6d: English again after all these switches.
		Language (root, "en", false);
		Check ("de nouveau en anglais apres plusieurs bascules", o.WaitFor (Affiche ("titre", "Demo tool"), 5000));
		Text (o, "menu_quitter", "Quit", "Quitter");
		Text (o, "onglet", "General", "Général");
		Text (o, "logique", "ok");

		// Step 7: the script exits by itself with code 0 and printed no Python traceback
		// (the layer must never break the script). On failure, show its output.
		DWORD code = 1;
		Check ("fermeture propre du script", o.Finish (code, 10000) && code == 0, "code " + std::to_string (code));
		bool trace = false;
		for (const std::string& l : o.Others ())
			trace = trace || l.find ("Traceback") != std::string::npos;
		Check ("aucune erreur Python", !trace);
		if (g_failures > 0)
			ShowOutput (o);
	}

	// --- [3] script launched while a language is already chosen ----------------------------
	// The settings left by [2] say English: widgets must be translated as soon as they are
	// created (no switch involved), and the script logic must still work.
	std::printf ("\n[3] Script lance avec l'anglais deja choisi (traduction a la creation)\n");
	{
		Tool o;
		Check ("lancement", o.Launch (script, root));
		Check ("interface creee directement en anglais", o.WaitFor (Affiche ("titre", "Demo tool"), 30000));
		Text (o, "fermer", "Close", "Fermer");
		Text (o, "statut", "Ready", "Prêt");
		Text (o, "menu_ouvrir", "Open...", "Ouvrir...");
		Text (o, "entete", "Name", "Nom");
		Text (o, "canevas", "Floor plan", "Plan");
		Text (o, "logique", "ok");
		DWORD code = 1;
		Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
	}

	// --- [3b] common dictionary only (script without a dictionary), several languages -------
	// TAPIR_TRANSLATOR_TOOL overrides the script name used to find <script>.json: with a
	// name that has no dictionary, only _common.json (fr, en, de, es, pt, it, hu) applies.
	//   1. Portuguese and Hungarian come from the common dictionary; the window title,
	//      specific to the script, stays in French;
	//   2. Polish is an ArchiCAD language without a column in the common dictionary: with
	//      the engine disabled the original text must stay;
	//   3. Spanish and Italian, then the script logic is still intact.
	std::printf ("\n[3b] Script sans dictionnaire : dictionnaire commun multilingue\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_no_dictionary");
		Language (root, "pt", false);
		Tool o;
		Check ("lancement", o.Launch (script, root));
		Check ("portugais", o.WaitFor (Affiche ("fermer", "Fechar"), 30000));
		Text (o, "murs", "Paredes", "Murs");
		Text (o, "nom", "Nome:", "Nom :");
		Text (o, "statut", "Pronto", "Prêt");
		Text (o, "menu_fichier", "Ficheiro", "Fichier");
		Text (o, "titre", "Outil de démonstration");		// script-specific: not in the common dictionary
		Language (root, "hu", false);
		Check ("hongrois", o.WaitFor (Affiche ("fermer", "Bezárás"), 5000));
		Text (o, "murs", "Falak", "Murs");
		Text (o, "menu_quitter", "Kilépés", "Quitter");
		Text (o, "onglet", "Általános", "Général");
		// Polish: an ArchiCAD language without a column in the common dictionary, engine
		// disabled -> the original text stays (only the engine would translate it)
		Language (root, "pl", false);
		Check ("polonais sans colonne de dictionnaire : texte d'original", o.WaitFor (Affiche ("fermer", "Fermer"), 5000));
		Text (o, "murs", "Murs", "Murs");
		Language (root, "es", false);
		Check ("espagnol", o.WaitFor (Affiche ("fermer", "Cerrar"), 5000));
		Text (o, "murs", "Muros", "Murs");
		Language (root, "it", false);
		Check ("italien", o.WaitFor (Affiche ("fermer", "Chiudi"), 5000));
		Text (o, "logique", "ok");
		DWORD code = 1;
		Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
	}

	// --- [3c] script written in English, source language not declared ---------------------
	// BENCH_UI=en makes bench_tool.py build its interface in English; the script name has no
	// dictionary, so only the multilingual common dictionary is used.
	//   1. automatic input detection (input ""): English -> French (with the French space
	//      before ':'), English -> English (nothing changes), English -> German;
	//   2. input language chosen in the menu: en -> fr, en -> hu; a wrong input language
	//      (de) on an English script finds nothing to translate; input = output (en -> en)
	//      translates nothing;
	//   3. clean up the variables and go back to English output for the next scenarios.
	std::printf ("\n[3c] Script ecrit en anglais (langue non declaree)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_english");
		SetEnvironmentVariableW (L"BENCH_UI", L"en");
		Language (root, "fr", false);
		Tool o;
		Check ("lancement", o.Launch (script, root));
		Check ("anglais -> francais", o.WaitFor (Affiche ("fermer", "Fermer"), 30000));
		Text (o, "fermer", "Fermer", "Close");
		Text (o, "nom", "Nom :", "Name:");				// French typography (space before ':')
		Text (o, "murs", "Murs", "Walls");
		Text (o, "statut", "Prêt", "Ready");
		Text (o, "menu_quitter", "Quitter", "Quit");
		Language (root, "en", false);
		Check ("anglais -> anglais : rien ne change", o.WaitFor (Affiche ("fermer", "Close"), 5000));
		Text (o, "murs", "Walls", "Walls");
		Language (root, "de", false);
		Check ("anglais -> allemand", o.WaitFor (Affiche ("fermer", "Schließen"), 5000));
		Text (o, "nom", "Name:", "Name:");
		Text (o, "logique", "ok");

		// Step 2: input language chosen in the menu (4th argument of Language)
		Language (root, "fr", false, "en");
		Check ("entree anglais -> sortie francais", o.WaitFor (Affiche ("fermer", "Fermer"), 5000));
		Text (o, "murs", "Murs", "Walls");
		Text (o, "statut", "Prêt", "Ready");
		Language (root, "hu", false, "en");
		Check ("entree anglais -> sortie hongrois", o.WaitFor (Affiche ("fermer", "Bezárás"), 5000));
		Text (o, "murs", "Falak", "Walls");
		Language (root, "fr", false, "de");
		Check ("entree allemand sur un script anglais : rien a traduire", o.WaitFor (Affiche ("fermer", "Close"), 5000));
		Text (o, "murs", "Walls", "Walls");
		Language (root, "en", false, "en");
		Sleep (1500);
		Text (o, "fermer", "Close", "Close");				// input = output
		Text (o, "logique", "ok");
		DWORD code = 1;
		Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
		// Step 3: back to the French script and English output.
		SetEnvironmentVariableW (L"BENCH_UI", nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", nullptr);
		Language (root, "en", false);
	}

	// --- [3d] automatic offline translation (test engine) ----------------------------------
	// Texts found in no dictionary are sent by the layer to the local engine; here the engine
	// runs with --test, whose "translation" is "[<lang>] <text>". The layer is told the
	// engine port through TAPIR_TRANSLATOR_ENGINE_PORT (a port no real engine uses).
	//   1. engine enabled in the settings but not running: the dictionary still applies and
	//      the unknown text keeps its original value;
	//   2. the engine starts: the unknown sentence and the title get translated live (the
	//      layer retries by itself); the dictionary still wins, data stays untouched;
	//   3. the translation is stored in cache\test\fr-en.json (French source recognised);
	//   4. another output language triggers a new automatic translation; disabling the
	//      engine brings the original text back;
	//   5. engine stopped, script relaunched: the translations come back from the cache
	//      alone; an already-English text ("Options") is never sent to the engine.
	std::printf ("\n[3d] Traduction automatique hors ligne (moteur d'essai)\n");
	{
		const int port = 8792;	// not the port of a real engine (8779)
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_ENGINE_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_engine");	// no dictionary of its own
		const std::wstring journal = TT::Join (root, L"log.txt");
		const std::wstring cacheEn = TT::Join (TT::Join (TT::Join (root, L"cache"), L"test"), L"fr-en.json");
		HANDLE engine = nullptr;
		// English output, automatic input, engine enabled, provider "test" (default).
		Language (root, "en", false, "", true);
		{
			// Step 1: no engine yet; wait 3 s to be sure nothing arrives for the unknown text.
			Tool o;
			Check ("lancement, moteur absent", o.Launch (script, root));
			Check ("dictionnaire applique sans moteur", o.WaitFor (Affiche ("fermer", "Close"), 30000));
			Sleep (3000);
			Text (o, "inconnu", "Texte inconnu", "Texte inconnu");		// engine absent: original text
			Text (o, "logique", "ok");

			// Step 2: start the test engine while the window is open.
			engine = StartEngine (port, journal);
			Check ("moteur d'essai lance", engine != nullptr);
			Check ("moteur revenu : phrase hors dictionnaire traduite en direct (< 25 s)",
					  o.WaitFor (Affiche ("inconnu", "[en] Texte inconnu"), 25000));
			Text (o, "inconnu", "[en] Texte inconnu", "Texte inconnu");
			// The title may arrive in the engine's next batch
			Check ("titre traduit automatiquement", o.WaitFor (Affiche ("titre", "[en] Outil de démonstration"), 5000));
			Text (o, "titre", "[en] Outil de démonstration", "Outil de démonstration");
			Text (o, "fermer", "Close", "Fermer");						// the dictionary wins
			Text (o, "chemin", "C:\\Projets\\plan.pln", "C:\\Projets\\plan.pln");	// data: never sent
			Text (o, "code", "MUR-01", "MUR-01");
			Text (o, "ligne_arbre", "Murs", "Murs");
			Text (o, "logique", "ok");

			// Step 3: the cache file of the "test" provider, French -> English, must hold the
			// translation (polled for up to about 3 s).
			bool cached = false;
			for (int i = 0; i < 30 && !cached; ++i) {
				std::string c;
				cached = TT::ReadWholeFile (cacheEn, c) && c.find ("\"Texte inconnu\": \"[en] Texte inconnu\"") != std::string::npos;
				if (!cached)
					Sleep (100);
			}
			Check ("traduction gardee dans cache\\fr-en.json (langue fr reconnue)", cached);

			// Step 4: Hungarian output -> new request to the engine ("[hu] ..."), the
			// dictionary still gives "Bezárás"; then engine disabled -> original text.
			Language (root, "hu", false, "", true);
			Check ("autre langue de sortie : nouvelle traduction automatique", o.WaitFor (Affiche ("inconnu", "[hu] Texte inconnu"), 10000));
			Text (o, "fermer", "Bezárás", "Fermer");

			Language (root, "en", false, "", false);
			Check ("moteur desactive : texte d'original", o.WaitFor (Affiche ("inconnu", "Texte inconnu"), 5000));
			Text (o, "fermer", "Close", "Fermer");
			DWORD code = 1;
			Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
		}
		StopEngine (engine);

		// Step 5: engine enabled in the settings but stopped: a new run must find its
		// translations in the cache.
		Language (root, "en", false, "", true);
		{
			Tool o;
			Check ("relance, moteur arrete", o.Launch (script, root));
			Check ("traduction reprise du cache, sans moteur", o.WaitFor (Affiche ("inconnu", "[en] Texte inconnu"), 30000));
			// The title is created before the script's language is recognised: it follows shortly
			Check ("titre repris du cache", o.WaitFor (Affiche ("titre", "[en] Outil de démonstration"), 5000));
			Text (o, "titre", "[en] Outil de démonstration", "Outil de démonstration");
			Text (o, "cadre", "Options", "Options");			// already English: never sent to the engine
			DWORD code = 1;
			Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
		}
		// Clean up: default script name, engine disabled, English output.
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", nullptr);
		Language (root, "en", false);
	}

	// --- [3e] online translators (fake server with the DeepL, Microsoft, Google, MyMemory
	//     and LibreTranslate formats), through the real engine ------------------------------
	// Chain under test: layer -> engine (--keys keys.json) -> fake service -> engine ->
	// cache -> layer. The TAPIR_TRANSLATOR_URL_<SERVICE> variables send every provider to
	// the fake server instead of the real one.
	//   1. keys.json: the add-on's template is created, then replaced by keys for every
	//      provider except Google;
	//   2. for DeepL, Microsoft, MyMemory and LibreTranslate in turn: the unknown sentence
	//      comes back in that provider's format with that provider's prefix, while the
	//      dictionary still translates "Fermer" first;
	//   3. Google without a key: the original text stays; the key is then added to
	//      keys.json while everything runs: the engine re-reads it and the next retry
	//      succeeds;
	//   4. back to DeepL: its own cache gives its translation back;
	//   5. after the run: no dictionary text and no data (path, code) ever reached the
	//      service, and the missing Google key is explained in log.txt.
	std::printf ("\n[3e] Traducteurs en ligne (faux serveur, formats reels des 5 services)\n");
	{
		// Fake services on 8798, engine on 8796 (both distinct from the other scenarios).
		const int portServices = 8798;
		const int port = 8796;
		FakeServer fake;
		Check ("faux serveur de traduction demarre", fake.Start (portServices));
		const std::wstring base = L"http://127.0.0.1:" + std::to_wstring (portServices);
		for (const wchar_t* name : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + name).c_str (), base.c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_ENGINE_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_online");
		// Step 1: keys.json template written by the add-on code, then overwritten below.
		const std::wstring keys = TT::KeysPath (root);
		std::wstring error;
		Check ("modele de keys.json cree", TT::CreateKeysTemplate (root, error) && TT::FileExists (keys));
		// The Google key is missing at first (the keys match what FakeServer expects)
		Write (keys, "{\"deepl\": {\"key\": \"cle-deepl:fx\"}, \"microsoft\": {\"key\": \"cle-ms\", \"region\": \"westeurope\"},"
					  " \"google\": {\"key\": \"\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"key\": \"\"}}");
		HANDLE engine = StartEngine (port, TT::Join (root, L"log.txt"), L"--keys \"" + keys + L"\"");
		Check ("moteur lance avec keys.json", engine != nullptr);
		{
			// Input language fixed to French, engine on, provider DeepL to start with.
			Tool o;
			Language (root, "en", false, "fr", true, "deepl");
			Check ("lancement", o.Launch (script, root));
			// Step 2: expected answer of each provider for "Text unknown" (DeepL asks for
			// EN-GB as target, the others for "en").
			const struct { const char* code; const char* expected; } services[] = {
				{ "deepl", "DL:EN-GB:Texte inconnu" },
				{ "microsoft", "MS:en:Texte inconnu" },
				{ "mymemory", "MM:en:Texte inconnu" },
				{ "libretranslate", "LT:en:Texte inconnu" },
			};
			for (const auto& s : services) {
				Language (root, "en", false, "fr", true, s.code);
				Check (std::string (s.code) + " : phrase traduite (format de requete et de reponse du service)",
						  o.WaitFor (Affiche ("inconnu", s.expected), 20000), o.Read ("inconnu").displayed);
				Text (o, "fermer", "Close", "Fermer");			// dictionary first
			}
			Text (o, "logique", "ok");

			// Step 3: Google without a key -> original kept (checked again after 2 s, to be
			// sure no translation arrives), then the key is written into keys.json.
			Language (root, "en", false, "fr", true, "google");
			Check ("google sans cle : texte d'origine garde", o.WaitFor (Affiche ("inconnu", "Texte inconnu"), 10000));
			Sleep (2000);
			Text (o, "inconnu", "Texte inconnu", "Texte inconnu");
			Write (keys, "{\"deepl\": {\"key\": \"cle-deepl:fx\"}, \"microsoft\": {\"key\": \"cle-ms\", \"region\": \"westeurope\"},"
						  " \"google\": {\"key\": \"cle-google\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"key\": \"\"}}");
			Check ("cle Google ajoutee a chaud : traduit au nouvel essai (< 30 s)",
					  o.WaitFor (Affiche ("inconnu", "GG:en:Texte inconnu"), 30000), o.Read ("inconnu").displayed);

			// Step 4: each provider has its own cache folder: DeepL's answer comes back.
			Language (root, "en", false, "fr", true, "deepl");
			Check ("retour a DeepL : son cache redonne sa traduction", o.WaitFor (Affiche ("inconnu", "DL:EN-GB:Texte inconnu"), 10000));
			Text (o, "logique", "ok");
			DWORD code = 1;
			Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
		}
		// Step 5: privacy checks on everything the fake service received, then the log
		// line about the missing Google key; finally clean up the engine, the server and
		// the variables.
		Check ("aucun texte du dictionnaire envoye en ligne (\"Fermer\")", !fake.Received ("Fermer"));
		Check ("aucune donnee envoyee en ligne (chemin, code)", !fake.Received ("C:\\Projets\\plan.pln") && !fake.Received ("MUR-01"));
		const std::vector<std::string> journal = TT::LastLines (TT::Join (root, L"log.txt"), 300);
		bool reported = false;
		for (const std::string& l : journal)
			reported = reported || l.find ("cle Google absente") != std::string::npos;
		Check ("journal : la cle Google manquante est expliquee", reported);
		StopEngine (engine);
		fake.Stop ();
		for (const wchar_t* name : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + name).c_str (), nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", nullptr);
		Language (root, "en", false);
	}

	// --- [3f] term corrections and MyMemory quota --------------------------------------------
	// The English interface (BENCH_UI=en) is translated into French by the fake
	// LibreTranslate, which turns "All stories" into "Toutes les histoires".
	//   1. the corrections.json pre-filled by the add-on must fix it into "Tous les étages";
	//   2. corrections.json edited while the window is open (rules "histoire" -> "level",
	//      for originals containing "story", whose -s plural follows automatically, and the
	//      whole phrase "Toutes les histoires" -> "Tous les levels"): applied live to the
	//      cached translation; with no correction, the raw service translation shows;
	//      corrections never touch dictionary translations;
	//   3. MyMemory anonymous quota reached: the original text stays (dictionary still
	//      applied) and the engine logs the quota (the e-mail window is disabled here by
	//      --no-dialog);
	//   4. what the "Save" button of that window does: the engine writes the e-mail
	//      into keys.json, keeping the other entries; MyMemory then translates again (the
	//      fake server only blocks requests without an e-mail).
	std::printf ("\n[3f] Corrections de termes (story -> étage) et quota MyMemory\n");
	{
		// Fake services on 8799, engine on 8795; only MyMemory and LibreTranslate redirected.
		const int portServices = 8799;
		const int port = 8795;
		FakeServer fake;
		Check ("faux serveur de traduction demarre", fake.Start (portServices));
		const std::wstring base = L"http://127.0.0.1:" + std::to_wstring (portServices);
		for (const wchar_t* name : { L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + name).c_str (), base.c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_ENGINE_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_corrections");
		SetEnvironmentVariableW (L"BENCH_UI", L"en");
		// keys.json with an extra "_explanation" entry and a DeepL key: both must survive
		// when the engine writes the MyMemory e-mail (step 4). No e-mail at first.
		const std::wstring keys = TT::KeysPath (root);
		Write (keys, "{\"_explanation\": \"test\", \"deepl\": {\"key\": \"cle-deepl:fx\"}, \"mymemory\": {\"email\": \"\"}}");
		HANDLE engine = StartEngine (port, TT::Join (root, L"log.txt"), L"--no-dialog --keys \"" + keys + L"\"");
		Check ("moteur lance", engine != nullptr);
		Check ("corrections.json pre-rempli par l'add-on", TT::FileExists (TT::CorrectionsPath (root)));
		{
			// Step 1: English -> French through LibreTranslate, default corrections.
			Tool o;
			Language (root, "fr", false, "en", true, "libretranslate");
			Check ("lancement (script en anglais)", o.Launch (script, root));
			Check ("\"All stories\" -> \"Toutes les histoires\" corrige en \"Tous les étages\"",
					  o.WaitFor (Affiche ("etages", "Tous les étages"), 20000), o.Read ("etages").displayed);
			Text (o, "etages", "Tous les étages", "All stories");

			// Step 2: corrections modified while the window is open
			Write (TT::CorrectionsPath (root),
					"{\"fr\": [{\"source\": \"story\", \"wrong\": \"histoire\", \"correct\": \"niveau\"},"
					" {\"wrong\": \"Toutes les histoires\", \"correct\": \"Tous les niveaux\"}]}");
			Check ("corrections.json modifie : applique en direct, cache compris (pluriel -s automatique)",
					  o.WaitFor (Affiche ("etages", "Tous les niveaux"), 5000), o.Read ("etages").displayed);
			Write (TT::CorrectionsPath (root), "{\"fr\": []}");
			Check ("sans correction : traduction brute du service", o.WaitFor (Affiche ("etages", "Toutes les histoires"), 5000),
					  o.Read ("etages").displayed);
			Check ("la correction ne touche pas le dictionnaire", o.Read ("fermer").displayed == "Fermer");

			// Step 3: anonymous MyMemory quota reached (English -> German). The dictionary
			// still gives "Schließen"; 2.5 s later the unknown text must still be original,
			// and log.txt must report the anonymous quota (polled for up to about 4 s).
			fake.anonymousQuota = true;
			Language (root, "de", false, "en", true, "mymemory");
			Check ("quota MyMemory : le texte d'origine reste (dictionnaire applique)", o.WaitFor (Affiche ("fermer", "Schließen"), 10000));
			Sleep (2500);
			Text (o, "inconnu", "Texte inconnu", "Texte inconnu");
			bool quota = false;
			for (int i = 0; i < 20 && !quota; ++i) {
				for (const std::string& l : TT::LastLines (TT::Join (root, L"log.txt"), 60))
					quota = quota || l.find ("quota MyMemory atteint (anonyme") != std::string::npos;
				if (!quota)
					Sleep (200);
			}
			Check ("journal : quota MyMemory anonyme detecte (fenetre e-mail demandee)", quota);

			// Step 4: what the window's "Save" button does: the e-mail is written into
			// keys.json (engine run once with --save-email, exit code 0 = written);
			// the running engine re-reads the keys and MyMemory answers again.
			wchar_t exe[MAX_PATH] = {};
			GetModuleFileNameW (nullptr, exe, MAX_PATH);
			std::wstring path = exe;
			path = path.substr (0, path.find_last_of (L'\\') + 1) + L"TapirTranslatorEngine.exe";
			std::wstring line = L"\"" + path + L"\" --save-email \"" + keys + L"\" moi@exemple.fr";
			std::vector<wchar_t> buffer (line.begin (), line.end ());
			buffer.push_back (L'\0');
			STARTUPINFOW si = {};
			si.cb = sizeof (si);
			PROCESS_INFORMATION pi = {};
			DWORD code = 1;
			if (CreateProcessW (nullptr, buffer.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
				WaitForSingleObject (pi.hProcess, 10000);
				GetExitCodeProcess (pi.hProcess, &code);
				CloseHandle (pi.hThread);
				CloseHandle (pi.hProcess);
			}
			std::string c;
			TT::ReadWholeFile (keys, c);
			Check ("e-mail enregistre dans keys.json", code == 0 && c.find ("\"email\": \"moi@exemple.fr\"") != std::string::npos);
			Check ("les autres cles de keys.json sont gardees", c.find ("\"key\": \"cle-deepl:fx\"") != std::string::npos &&
																	c.find ("\"_explanation\": \"test\"") != std::string::npos);
			Check ("avec l'e-mail : MyMemory traduit de nouveau (< 30 s)", o.WaitFor (Affiche ("inconnu", "MM:de:Texte inconnu"), 30000),
					  o.Read ("inconnu").displayed);
			Text (o, "logique", "ok");
			DWORD end_ = 1;
			Check ("fermeture propre", o.Finish (end_, 10000) && end_ == 0);
		}
		// Clean up: engine, fake server, redirections and script variables.
		StopEngine (engine);
		fake.Stop ();
		for (const wchar_t* name : { L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + name).c_str (), nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", nullptr);
		SetEnvironmentVariableW (L"BENCH_UI", nullptr);
		Language (root, "en", false);
	}

	// --- [4] outside Tapir: the layer must do nothing ---------------------------------------
	// The layer is still on the PYTHONPATH (so it is imported by every Python started from
	// ArchiCAD's environment), but without TAPIR_SCRIPT_ID it must not install itself
	// (LAYER|missing) and the interface must stay untranslated although English is the
	// chosen output. TAPIR_SCRIPT_ID is restored afterwards.
	std::printf ("\n[4] Python lance hors Tapir (pas de TAPIR_SCRIPT_ID)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", nullptr);
		Tool o;
		Check ("lancement", o.Launch (script, root));
		Check ("couche inactive", o.WaitForLine ("LAYER|missing", 30000));
		Check ("interface non traduite", o.WaitFor (Affiche ("titre", "Outil de démonstration"), 10000));
		DWORD code = 1;
		Check ("fermeture propre", o.Finish (code, 10000) && code == 0);
		SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", L"tapir-banc-1");
	}

	// --- [5] layer log -----------------------------------------------------------------------
	// The layer never lets an error reach the script: it writes it to log.txt instead.
	// After all the runs above, the last 200 log lines must exist and contain none of the
	// (French) words of the layer's internal error messages: "interception", "after call",
	// "watchdog", "impossible", "unreadable". Offending lines are printed.
	std::printf ("\n[5] Journal de la couche\n");
	{
		const std::vector<std::string> lines = TT::LastLines (TT::Join (root, L"log.txt"), 200);
		bool error = false;
		for (const std::string& l : lines) {
			if (l.find ("interception") != std::string::npos || l.find ("apres call") != std::string::npos ||
				l.find ("surveillance") != std::string::npos || l.find ("impossible") != std::string::npos ||
				l.find ("illisible") != std::string::npos) {
				error = true;
				std::printf ("         | %s\n", l.c_str ());
			}
		}
		Check ("journal ecrit", !lines.empty ());
		Check ("aucune erreur interne de la couche", !error);
	}

	// --- [6] language of the tool itself (menus, messages, window) ---------------------------
	//   1. the texts table (Src/TT_Texts.cpp) has no empty cell;
	//   2. ArchiCAD language codes map to the right interface language, unknown -> English
	//      (index 0 = French, index 1 = English);
	//   3. formatted texts (TxtF with named values) and a text of the quota window;
	//   4. the "interface" setting is written and read back (a code, or "" = like ArchiCAD);
	//   5. the menus generated at build time (RFIX\Tapir_TranslatorMenus.grc, next to the
	//      Tests folder) contain, for each of the 20 languages, its 5 menu STR# resources
	//      and their status-bar help resources (+ HelpOffset), and the Japanese text.
	std::printf ("\n[6] Langue de Tapir Translator (20 langues, celle d'ArchiCAD par defaut)\n");
	{
		// Steps 1 to 3
		Check ("table des textes complete (51 textes x 20 langues)", TT::MissingTexts () == 0,
				  std::to_string (TT::MissingTexts ()) + " case(s) vide(s)");
		Check ("ArchiCAD FRA -> francais", TT::InterfaceIndex (TT::LanguageFromArchicad ("FRA")) == 0);
		Check ("ArchiCAD GER / AUT -> allemand", TT::LanguageFromArchicad ("GER") == "de" && TT::LanguageFromArchicad ("AUT") == "de");
		Check ("ArchiCAD INT / USA / UKI -> anglais", TT::LanguageFromArchicad ("INT") == "en" && TT::LanguageFromArchicad ("UKI") == "en");
		Check ("ArchiCAD BRA -> portugais, TAI -> chinois, JPN -> japonais",
				  TT::LanguageFromArchicad ("BRA") == "pt" && TT::LanguageFromArchicad ("TAI") == "zh" && TT::LanguageFromArchicad ("JPN") == "ja");
		Check ("version inconnue -> anglais", TT::LanguageFromArchicad ("XYZ") == "en" && TT::InterfaceIndex ("xx") == 1);
		Check ("textes avec valeurs (TxtF)", TT::TxtF (TT::R_STARTUP, 2, { { "v", "9" }, { "l", "fr -> de" } }) ==
												"Tapir Translator 9 aktiv. Sprachen: fr -> de.");
		Check ("fenetre du quota en hongrois", std::string (TT::Txt (TT::D_SAVE, TT::InterfaceIndex ("hu"))) == "Mentés");

		// Step 4
		TT::Settings r;
		r.interface = "ja";
		std::wstring error;
		TT::WriteSettings (root, r, error);
		Check ("reglages : langue de l'outil relue (ja)", TT::ReadSettings (root).interface == "ja");
		r.interface = "";
		TT::WriteSettings (root, r, error);
		Check ("reglages : langue de l'outil = comme ArchiCAD (vide)", TT::ReadSettings (root).interface.empty ());

		// Step 5: for language l, menu m has the id MenuBase (l) + m, its help + HelpOffset.
		std::string grc;
		const std::wstring menus = TT::Join (TT::Join (TestsFolder ().substr (0, TestsFolder ().find_last_of (L'\\')), L"RFIX"),
												L"Tapir_TranslatorMenus.grc");
		bool all = TT::ReadWholeFile (menus, grc);
		for (int l = 0; l < TT::InterfaceLanguageCount && all; ++l) {
			for (int m = 0; m < 5 && all; ++m) {
				const int id = TT::MenuBase (l) + m;
				all = grc.find ("'STR#' " + std::to_string (id) + " ") != std::string::npos &&
					   grc.find ("'STR#' " + std::to_string (id + TT::HelpOffset) + " ") != std::string::npos;
			}
		}
		Check ("menus generes pour les 20 langues (5 menus + aides chacune)", all);

		// Step 6: every resource ID read by the ArchiCAD API (a short) must stay below 32767; above,
		// it wraps to a negative number and ArchiCAD unloads the add-on (bug of v0.7.0).
		bool fitsShort = true;
		for (int l = 0; l < TT::InterfaceLanguageCount; ++l) {
			for (int m = 0; m < 5; ++m)
				fitsShort = fitsShort && TT::MenuBase (l) + m + TT::HelpOffset <= 32767 && TT::MenuBase (l) + m > 32000;
		}
		Check ("identifiants de menus et d'aides entre 32001 et 32767 (short de l'API ArchiCAD)", fitsShort);
		bool noIdTooLarge = true;
		for (size_t p = grc.find ("'STR#' "); p != std::string::npos; p = grc.find ("'STR#' ", p + 7))
			noIdTooLarge = noIdTooLarge && std::atoi (grc.c_str () + p + 7) <= 32767;
		Check ("aucune ressource generee au-dessus de 32767", noIdTooLarge);

		// Step 7: the generated menus must really be inside the built .apx (bug of v0.7.0 / v0.7.1:
		// RFIX.win/Tapir_Translator.rc2 did not include them, ArchiCAD answered -7003 and showed no
		// menu). The .apx is opened as a data file (no code runs) and every menu and help STR#
		// resource (type "STRS" after ResConv) is looked up.
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW (nullptr, exe, MAX_PATH);
		std::wstring realApx = exe;
		realApx = realApx.substr (0, realApx.find_last_of (L'\\') + 1) + L"Tapir_Translator.apx";
		HMODULE module = LoadLibraryExW (realApx.c_str (), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		bool resourcesInside = module != nullptr;
		for (int l = 0; l < TT::InterfaceLanguageCount && resourcesInside; ++l) {
			for (int m = 0; m < 5 && resourcesInside; ++m) {
				const int id = TT::MenuBase (l) + m;
				resourcesInside = FindResourceW (module, MAKEINTRESOURCEW (id), L"STRS") != nullptr &&
								   FindResourceW (module, MAKEINTRESOURCEW (id + TT::HelpOffset), L"STRS") != nullptr;
			}
		}
		if (module != nullptr)
			FreeLibrary (module);
		Check ("Tapir_Translator.apx contient les 200 ressources de menus et d'aides", resourcesInside);
		Check ("menus generes : japonais present", grc.find (TT::Txt (TT::M_TRANSLATOR, TT::InterfaceIndex ("ja"))) != std::string::npos);
	}

	// Summary; the temporary folder is kept when something failed, for examination.
	std::printf ("\n%d OK, %d ECHEC(S)\n", g_ok, g_failures);
	if (g_failures == 0)
		DeleteTree (root);
	else
		std::printf ("dossier garde pour examen : %s\n", TT::Utf8 (root).c_str ());
	return g_failures == 0 ? 0 : 1;
}
