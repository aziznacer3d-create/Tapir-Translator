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
//   Scenarios (each prints its title, then one OK / FAILED line per check):
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
//   Output: one OK / FAILED line per check, exit code 0 when everything passes.
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
	#error "BENCH_TESTS_DIR must be defined (see CMakeLists.txt)"
#endif

namespace {

// Global counters of passed / failed checks (summary and exit code at the end of main).
int g_ok = 0;
int g_failures = 0;

// Records one check: prints "OK <name>" or "FAILED <name> -> <detail>" and counts it.
// The output is flushed at once so that the progress is visible even if the bench hangs.
void Check (const std::string& name, bool condition, const std::string& detail = std::string ())
{
	if (condition) {
		++g_ok;
		std::printf ("  OK     %s\n", name.c_str ());
	} else {
		++g_failures;
		std::printf ("  FAILED %s%s%s\n", name.c_str (), detail.empty () ? "" : "  -> ", detail.c_str ());
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
		std::printf ("  (cannot write: %s)\n", TT::Utf8 (error).c_str ());
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
//   - a reader thread parses its output: each block of V| lines ended by STATE_END
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
		Command ("end");
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
	//   - "STATE_END"                   -> the state being built becomes the current state
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
				} else if (line == "STATE_END") {
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
	Check (key + " displayed \"" + displayed + "\"", v.displayed == displayed, "displayed \"" + v.displayed + "\"");
	if (seen != nullptr)
		Check (key + " seen by the script \"" + seen + "\"", v.seen == seen, "seen \"" + v.seen + "\"");
}

// Builds a condition for Tool::WaitFor: "the key is displayed with exactly this text".
std::function<bool (const std::map<std::string, Value>&)> Shows (const std::string& key, const std::string& text)
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
	Check (std::string ("WriteSettings input=\"") + input + "\" output=\"" + code + "\"" + (engine ? " engine" : ""),
			  TT::WriteSettings (root, r, error), TT::Utf8 (error));
	const TT::Settings reread = TT::ReadSettings (root);
	Check ("ReadSettings reads the settings back", reread.input == r.input && reread.language == r.language && reread.collect == r.collect &&
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
	Check ("ExtractLayer", TT::ExtractLayer (root, journal));
	Check ("layer\\sitecustomize.py written", TT::FileExists (TT::Join (TT::Join (root, L"layer"), L"sitecustomize.py")));
	Check ("dictionaries\\_common.json written", TT::FileExists (TT::Join (TT::Join (root, L"dictionaries"), L"_common.json")));
	Check ("dictionaries\\_user.json created", TT::FileExists (TT::Join (TT::Join (root, L"dictionaries"), L"_user.json")));
	// An old (modified) _common.json is replaced by the one of this version; _user.json is kept
	Write (TT::Join (TT::Join (root, L"dictionaries"), L"_common.json"), "{\"old\": true}");
	Write (TT::Join (TT::Join (root, L"dictionaries"), L"_user.json"), "{\"format\": 2, \"terms\": []}");
	{
		std::vector<std::wstring> j2;
		TT::ExtractLayer (root, j2);
		std::string c, p;
		TT::ReadWholeFile (TT::Join (TT::Join (root, L"dictionaries"), L"_common.json"), c);
		TT::ReadWholeFile (TT::Join (TT::Join (root, L"dictionaries"), L"_user.json"), p);
		Check ("old _common.json replaced by the one of this version", c.find ("\"terms\"") != std::string::npos);
		Check ("user's _user.json kept", p == "{\"format\": 2, \"terms\": []}");
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
	std::printf ("Tapir Translator %s - test bench\n  folder : %s\n  script : %s\n\n", TT::AddOnVersion,
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
	std::printf ("[0] Engine and model embedded in the .apx\n");
	{
		const std::wstring folder = TT::Join (temp, L"TapirTranslatorPayload_" + std::to_wstring (GetCurrentProcessId ()));
		DeleteTree (folder);
		const std::wstring source = TT::Join (folder, L"source");
		TT::CreateFolder (TT::Join (source, L"model\\sub"));
		std::string big (3 * 1024 * 1024 + 17, '\0');		// larger than the copy buffer (1 MB)
		for (size_t i = 0; i < big.size (); ++i)
			big[i] = static_cast<char> ((i * 31 + 7) & 0xFF);
		Write (TT::Join (source, L"engine.exe"), "MZ fake engine");
		Write (TT::Join (source, L"model\\weights.bin"), big);
		Write (TT::Join (source, L"model\\sub\\vocabulary é.txt"), "mur\nwall\n");
		// Fake .apx: stands for the DLL produced by the linker.
		const std::wstring apx = TT::Join (folder, L"fake.apx");
		const std::string apxContent = "fake DLL content";
		Write (apx, apxContent);

		// Step 2: pack twice; the second pass must replace the first payload, so the file
		// size stays about DLL + payload (one copy of the big file, not two).
		std::wstring hash, hash2;
		std::vector<std::wstring> journal;
		Check ("Pack", TT::Packer (apx, source, hash, journal));
		Check ("Pack a second time (old payload removed)", TT::Packer (apx, source, hash2, journal));
		Check ("same content = same fingerprint", hash == hash2 && !hash.empty ());
		std::string apxRead;
		TT::ReadWholeFile (apx, apxRead);
		Check ("start of the .apx is intact", apxRead.compare (0, apxContent.size (), apxContent) == 0);
		Check ("a single payload after two passes", apxRead.size () < apxContent.size () + big.size () + 2000,
				  std::to_string (apxRead.size ()));
		// Step 3: payload table (3 files, same fingerprint), then extraction into
		// <base>\<fingerprint> and byte-for-byte comparison of the files.
		std::wstring e;
		std::vector<std::wstring> paths;
		Check ("ReadPayload: 3 files", TT::ReadPayload (apx, e, paths) && paths.size () == 3 && e == hash);

		std::wstring extracted;
		const std::wstring base = TT::Join (folder, L"extracted");
		Check ("ExtractPayload", TT::ExtractPayload (apx, base, extracted, journal));
		Check ("extraction folder = fingerprint", extracted == TT::Join (base, hash), TT::Utf8 (extracted));
		std::string read_;
		Check ("large file identical", TT::ReadWholeFile (TT::Join (extracted, L"model\\weights.bin"), read_) && read_ == big);
		Check ("accented file identical", TT::ReadWholeFile (TT::Join (extracted, L"model\\sub\\vocabulary é.txt"), read_) && read_ == "mur\nwall\n");
		// A second extraction of the same version must succeed and end its log with
		// "already extracted" (nothing copied again).
		journal.clear ();
		Check ("second extraction: nothing to redo", TT::ExtractPayload (apx, base, extracted, journal) &&
				  !journal.empty () && journal.back ().find (L"already extracted") != std::wstring::npos);

		// Steps 4 and 5: a modified engine gives a new fingerprint and a new extraction
		// folder; a file without payload (here the fake engine itself) is rejected.
		Write (TT::Join (source, L"engine.exe"), "MZ fake engine v2");
		std::wstring hash3;
		TT::Packer (apx, source, hash3, journal);
		Check ("new version = new fingerprint", hash3 != hash);
		Check ("new version extracted alongside", TT::ExtractPayload (apx, base, extracted, journal) &&
				  TT::ReadWholeFile (TT::Join (extracted, L"engine.exe"), read_) && read_ == "MZ fake engine v2");
		Check ("file without payload rejected", !TT::ReadPayload (TT::Join (source, L"engine.exe"), e, paths));

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
		Check ("Tapir_Translator.apx contains TapirTranslatorEngine.exe", engineInside);
		DeleteTree (folder);
	}

	// --- [0b] conversion of the data of versions <= 0.7.5 (TT::MigrateOldData) -----------------
	//   Old French names and keys must become the current ones, once, without losing the user's
	//   keys, settings, corrections, dictionaries and cache; a second pass changes nothing.
	std::printf ("\n[0b] Conversion of the data of versions <= 0.7.5\n");
	{
		const std::wstring old = TT::Join (temp, L"TapirTranslatorMigration_" + std::to_wstring (GetCurrentProcessId ()));
		DeleteTree (old);
		// Step 1: data as written by version 0.7.5.
		TT::CreateFolder (TT::Join (old, L"dictionnaires"));
		TT::CreateFolder (TT::Join (old, L"manquants"));
		TT::CreateFolder (TT::Join (TT::Join (old, L"cache"), L"mymemory"));
		TT::CreateFolder (TT::Join (old, L"couche"));
		Write (TT::Join (old, L"reglages.json"), "{\n  \"entree\": \"\",\n  \"langue\": \"de\",\n  \"collecte\": true,\n  \"moteur\": true,\n"
												 "  \"fournisseur\": \"deepl\",\n  \"interface\": \"en\",\n  \"ecrit_par\": \"Tapir Translator 0.7.5\"\n}\n");
		Write (TT::Join (old, L"cles.json"), "{\"_explication\": \"x\", \"deepl\": {\"_ou\": \"y\", \"cle\": \"secret:fx\"}}");
		Write (TT::Join (old, L"corrections.json"), "{\"fr\": [{\"source\": \"story\", \"mauvais\": \"histoire\", \"correct\": \"étage\"}]}");
		Write (TT::Join (TT::Join (old, L"dictionnaires"), L"_perso.json"), "{\"format\": 2, \"langues\": [\"fr\"], \"termes\": []}");
		Write (TT::Join (TT::Join (old, L"dictionnaires"), L"_commun.json"), "{\"format\": 2, \"termes\": []}");
		Write (TT::Join (TT::Join (old, L"dictionnaires"), L"outil.json"), "{\"langue_source\": \"fr\", \"traductions\": {\"en\": {\"Fermer\": \"Close\"}}}");
		Write (TT::Join (TT::Join (old, L"manquants"), L"outil.json"), "{\"langue_source\": \"en\", \"traductions\": {\"fr\": {\"Close\": \"\"}}}");
		Write (TT::Join (TT::Join (TT::Join (old, L"cache"), L"mymemory"), L"en-fr.json"),
			   "{\"cible\": \"fr\", \"fournisseur\": \"mymemory\", \"source\": \"en\", \"traductions\": {\"Close\": \"Fermer\"}}");
		Write (TT::Join (old, L"journal.txt"), "old log line\n");
		Write (TT::Join (TT::Join (old, L"couche"), L"sitecustomize.py"), "# old layer\n");
		// Step 2: conversion, then the settings must read back as written by 0.7.5.
		std::vector<std::wstring> journal;
		TT::MigrateOldData (old, journal);
		const TT::Settings s = TT::ReadSettings (old);
		Check ("old settings read back (output de, deepl, online, collect, interface en)",
			   s.language == "de" && s.provider == "deepl" && s.engine && s.collect && s.interface == "en");
		std::string c;
		Check ("cles.json -> keys.json, key kept", !TT::FileExists (TT::Join (old, L"cles.json")) &&
			   TT::ReadWholeFile (TT::Join (old, L"keys.json"), c) && c.find ("\"key\": \"secret:fx\"") != std::string::npos &&
			   c.find ("\"_where\"") != std::string::npos && c.find ("\"_explanation\"") != std::string::npos, c);
		Check ("corrections.json keys converted", TT::ReadWholeFile (TT::Join (old, L"corrections.json"), c) &&
			   c.find ("\"wrong\": \"histoire\"") != std::string::npos, c);
		Check ("dictionnaires -> dictionaries, _perso.json -> _user.json",
			   TT::ReadWholeFile (TT::Join (TT::Join (old, L"dictionaries"), L"_user.json"), c) &&
			   c.find ("\"terms\"") != std::string::npos && c.find ("\"languages\"") != std::string::npos, c);
		Check ("script dictionary converted", TT::ReadWholeFile (TT::Join (TT::Join (old, L"dictionaries"), L"outil.json"), c) &&
			   c.find ("\"source_language\"") != std::string::npos && c.find ("\"translations\"") != std::string::npos, c);
		Check ("manquants -> missing", TT::ReadWholeFile (TT::Join (TT::Join (old, L"missing"), L"outil.json"), c) &&
			   c.find ("\"translations\"") != std::string::npos, c);
		Check ("cache converted, translation kept",
			   TT::ReadWholeFile (TT::Join (TT::Join (TT::Join (old, L"cache"), L"mymemory"), L"en-fr.json"), c) &&
			   c.find ("\"target\": \"fr\"") != std::string::npos && c.find ("\"provider\"") != std::string::npos &&
			   c.find ("\"Close\": \"Fermer\"") != std::string::npos, c);
		Check ("journal.txt -> log.txt", TT::ReadWholeFile (TT::Join (old, L"log.txt"), c) && c == "old log line\n");
		Check ("old layer folder deleted", !TT::FolderExists (TT::Join (old, L"couche")));
		Check ("old _commun.json deleted", !TT::FileExists (TT::Join (TT::Join (old, L"dictionaries"), L"_commun.json")));
		// Step 3: a second pass must find nothing left to convert.
		std::vector<std::wstring> again;
		TT::MigrateOldData (old, again);
		Check ("second pass: nothing to convert", again.empty (), std::to_string (again.size ()));
		DeleteTree (old);
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
	std::printf ("\n[1] Environment prepared by the add-on\n");
	std::string dictionary = Prepare (root);
	// An existing PYTHONPATH (with its own sitecustomize) must be kept and chained
	const std::wstring other = TT::Join (root, L"other_path");
	TT::CreateFolder (other);
	Write (TT::Join (other, L"sitecustomize.py"), "print('NEXT|ok', flush=True)\n");
	SetEnvironmentVariableW (L"PYTHONPATH", other.c_str ());
	std::wstring pythonPath;
	Check ("PrepareEnvironment", TT::PrepareEnvironment (root, pythonPath));
	Check ("PYTHONPATH = layer;previous", pythonPath == TT::Join (root, L"layer") + L";" + other, TT::Utf8 (pythonPath));
	std::wstring again;
	TT::PrepareEnvironment (root, again);
	Check ("PrepareEnvironment idempotent", again == pythonPath, TT::Utf8 (again));
	SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", L"tapir-bench-1");
	SetEnvironmentVariableW (L"PYTHONIOENCODING", L"utf-8");
	SetEnvironmentVariableW (L"BENCH_EXCHANGE", root.c_str ());
	Language (root, "", true);
	Check ("ReadSettings reads back the original language + collection", TT::ReadSettings (root).language.empty () && TT::ReadSettings (root).collect);

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
	std::printf ("\n[2] Script opened untranslated, then translated live\n");
	{
		// Step 1: launch. The first "uv run" may have to prepare its environment, hence 30 s.
		Tool o;
		Check ("launch uv run --script", o.Launch (script, root));
		Check ("layer loaded by Python (sitecustomize)", o.WaitForLine ("LAYER|active", 30000));
		Check ("existing sitecustomize chained", o.WaitForLine ("NEXT|ok", 2000));
		Check ("first state received", o.WaitFor (Shows ("title", "Outil de démonstration"), 10000));
		Text (o, "close", "Fermer", "Fermer");
		Text (o, "status", "Prêt", "Prêt");

		// Step 2: English output (collection still on). Each Text checks the displayed
		// translation AND the original text still returned to the script.
		Language (root, "en", true);
		Check ("live switch to English (< 5 s)", o.WaitFor (Shows ("title", "Demo tool"), 5000));
		Text (o, "title", "Demo tool", "Outil de démonstration");
		Text (o, "status", "Ready", "Prêt");
		Text (o, "close", "Close", "Fermer");
		Text (o, "name", "Name:", "Nom :");
		Text (o, "walls", "Walls", "Murs");
		Text (o, "frame", "Options", "Options");
		// Multi-line text (newlines escaped by the script), "{n}" pattern, and a text found
		// in no dictionary (no engine running: it stays as is).
		Text (o, "multi", "First line\\nSecond line", "Première ligne\\nDeuxième ligne");
		Text (o, "pattern", "3 walls created", "3 murs créés");
		Text (o, "unknown", "Texte inconnu", "Texte inconnu");
		// Menus, notebook tab, Treeview heading, Canvas text.
		Text (o, "menu_file", "File", "Fichier");
		Text (o, "menu_open", "Open...", "Ouvrir...");
		Text (o, "menu_quit", "Quit", "Quitter");
		Text (o, "tab", "General", "Général");
		Text (o, "heading", "Name", "Nom");
		Text (o, "canvas", "Floor plan", "Plan");
		Text (o, "tree_row", "Murs", "Murs");			// data: never translated
		Text (o, "combo", "Murs Dalles", "Murs");			// list values: never translated
		Text (o, "entry", "Murs", "Murs");				// user input: never translated
		// The script's own comparisons with its original texts still succeed.
		Text (o, "logic", "ok");

		// Step 3: the script sets new texts itself while the interface is translated; they
		// must be translated too, and the script must still read its own French values.
		o.Command ("update");
		Check ("script changes its status while translated", o.WaitFor (Shows ("status", "Done"), 3000));
		Text (o, "status", "Done", "Terminé");
		Text (o, "name", "Value", "Valeur");
		Text (o, "logic", "ok");

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
		Check ("untranslated text noted in missing\\bench_tool.json", note);

		// Step 5: dictionary completed while the tool is running: the entry
		// "Text unknown" -> "Unknown text" is inserted after "Plan" in the script's
		// dictionary file, and the open window must pick it up by itself.
		const std::string key = "\"Plan\": \"Floor plan\"";
		const size_t pos = dictionary.find (key);
		Check ("bench dictionary readable", pos != std::string::npos);
		if (pos != std::string::npos) {
			dictionary.insert (pos + key.size (), ",\n      \"Texte inconnu\": \"Unknown text\"");
			Write (TT::Join (TT::Join (root, L"dictionaries"), L"bench_tool.json"), dictionary);
		}
		Check ("modified dictionary applied live", o.WaitFor (Shows ("unknown", "Unknown text"), 5000));
		Text (o, "unknown", "Unknown text", "Texte inconnu");

		// Step 6a: German (collection off). "Text unknown" has no German entry: original.
		Language (root, "de", false);
		Check ("switch to German", o.WaitFor (Shows ("title", "Demo-Werkzeug"), 5000));
		Text (o, "close", "Schließen", "Fermer");
		Text (o, "pattern", "3 Wände erstellt", "3 murs créés");
		Text (o, "walls", "Wände", "Murs");
		Text (o, "status", "Fertig", "Terminé");
		Text (o, "name", "Wert", "Valeur");					// common multilingual dictionary
		Text (o, "unknown", "Texte inconnu", "Texte inconnu");

		// Step 6b: output language "" = no translation: every original text comes back.
		Language (root, "", false);
		Check ("back to the original language", o.WaitFor (Shows ("title", "Outil de démonstration"), 5000));
		Text (o, "close", "Fermer", "Fermer");
		Text (o, "status", "Terminé", "Terminé");
		Text (o, "pattern", "3 murs créés", "3 murs créés");
		Text (o, "menu_open", "Ouvrir...", "Ouvrir...");
		Text (o, "canvas", "Plan", "Plan");

		// Step 6c: output = French, the script's own language: nothing must change (no
		// state change to wait for, hence a fixed delay before the check).
		Language (root, "fr", false);
		Sleep (1500);
		Text (o, "title", "Outil de démonstration");		// fr = source language: nothing to do

		// Step 6d: English again after all these switches.
		Language (root, "en", false);
		Check ("English again after several switches", o.WaitFor (Shows ("title", "Demo tool"), 5000));
		Text (o, "menu_quit", "Quit", "Quitter");
		Text (o, "tab", "General", "Général");
		Text (o, "logic", "ok");

		// Step 7: the script exits by itself with code 0 and printed no Python traceback
		// (the layer must never break the script). On failure, show its output.
		DWORD code = 1;
		Check ("clean exit of the script", o.Finish (code, 10000) && code == 0, "code " + std::to_string (code));
		bool trace = false;
		for (const std::string& l : o.Others ())
			trace = trace || l.find ("Traceback") != std::string::npos;
		Check ("no Python error", !trace);
		if (g_failures > 0)
			ShowOutput (o);
	}

	// --- [3] script launched while a language is already chosen ----------------------------
	// The settings left by [2] say English: widgets must be translated as soon as they are
	// created (no switch involved), and the script logic must still work.
	std::printf ("\n[3] Script launched with English already chosen (translated at creation)\n");
	{
		Tool o;
		Check ("launch", o.Launch (script, root));
		Check ("interface created directly in English", o.WaitFor (Shows ("title", "Demo tool"), 30000));
		Text (o, "close", "Close", "Fermer");
		Text (o, "status", "Ready", "Prêt");
		Text (o, "menu_open", "Open...", "Ouvrir...");
		Text (o, "heading", "Name", "Nom");
		Text (o, "canvas", "Floor plan", "Plan");
		Text (o, "logic", "ok");
		DWORD code = 1;
		Check ("clean exit", o.Finish (code, 10000) && code == 0);
	}

	// --- [3b] common dictionary only (script without a dictionary), several languages -------
	// TAPIR_TRANSLATOR_TOOL overrides the script name used to find <script>.json: with a
	// name that has no dictionary, only _common.json (fr, en, de, es, pt, it, hu) applies.
	//   1. Portuguese and Hungarian come from the common dictionary; the window title,
	//      specific to the script, stays in French;
	//   2. Polish is an ArchiCAD language without a column in the common dictionary: with
	//      the engine disabled the original text must stay;
	//   3. Spanish and Italian, then the script logic is still intact.
	std::printf ("\n[3b] Script without a dictionary: common multilingual dictionary\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_no_dictionary");
		Language (root, "pt", false);
		Tool o;
		Check ("launch", o.Launch (script, root));
		Check ("Portuguese", o.WaitFor (Shows ("close", "Fechar"), 30000));
		Text (o, "walls", "Paredes", "Murs");
		Text (o, "name", "Nome:", "Nom :");
		Text (o, "status", "Pronto", "Prêt");
		Text (o, "menu_file", "Ficheiro", "Fichier");
		Text (o, "title", "Outil de démonstration");		// script-specific: not in the common dictionary
		Language (root, "hu", false);
		Check ("Hungarian", o.WaitFor (Shows ("close", "Bezárás"), 5000));
		Text (o, "walls", "Falak", "Murs");
		Text (o, "menu_quit", "Kilépés", "Quitter");
		Text (o, "tab", "Általános", "Général");
		// Polish: an ArchiCAD language without a column in the common dictionary, engine
		// disabled -> the original text stays (only the engine would translate it)
		Language (root, "pl", false);
		Check ("Polish without a dictionary column: original text", o.WaitFor (Shows ("close", "Fermer"), 5000));
		Text (o, "walls", "Murs", "Murs");
		Language (root, "es", false);
		Check ("Spanish", o.WaitFor (Shows ("close", "Cerrar"), 5000));
		Text (o, "walls", "Muros", "Murs");
		Language (root, "it", false);
		Check ("Italian", o.WaitFor (Shows ("close", "Chiudi"), 5000));
		Text (o, "logic", "ok");
		DWORD code = 1;
		Check ("clean exit", o.Finish (code, 10000) && code == 0);
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
	std::printf ("\n[3c] Script written in English (language not declared)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_english");
		SetEnvironmentVariableW (L"BENCH_UI", L"en");
		Language (root, "fr", false);
		Tool o;
		Check ("launch", o.Launch (script, root));
		Check ("English -> French", o.WaitFor (Shows ("close", "Fermer"), 30000));
		Text (o, "close", "Fermer", "Close");
		Text (o, "name", "Nom :", "Name:");				// French typography (space before ':')
		Text (o, "walls", "Murs", "Walls");
		Text (o, "status", "Prêt", "Ready");
		Text (o, "menu_quit", "Quitter", "Quit");
		Language (root, "en", false);
		Check ("English -> English: nothing changes", o.WaitFor (Shows ("close", "Close"), 5000));
		Text (o, "walls", "Walls", "Walls");
		Language (root, "de", false);
		Check ("English -> German", o.WaitFor (Shows ("close", "Schließen"), 5000));
		Text (o, "name", "Name:", "Name:");
		Text (o, "logic", "ok");

		// Step 2: input language chosen in the menu (4th argument of Language)
		Language (root, "fr", false, "en");
		Check ("input English -> output French", o.WaitFor (Shows ("close", "Fermer"), 5000));
		Text (o, "walls", "Murs", "Walls");
		Text (o, "status", "Prêt", "Ready");
		Language (root, "hu", false, "en");
		Check ("input English -> output Hungarian", o.WaitFor (Shows ("close", "Bezárás"), 5000));
		Text (o, "walls", "Falak", "Walls");
		Language (root, "fr", false, "de");
		Check ("input German on an English script: nothing to translate", o.WaitFor (Shows ("close", "Close"), 5000));
		Text (o, "walls", "Walls", "Walls");
		Language (root, "en", false, "en");
		Sleep (1500);
		Text (o, "close", "Close", "Close");				// input = output
		Text (o, "logic", "ok");
		DWORD code = 1;
		Check ("clean exit", o.Finish (code, 10000) && code == 0);
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
	std::printf ("\n[3d] Automatic offline translation (test engine)\n");
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
			Check ("launch, engine absent", o.Launch (script, root));
			Check ("dictionary applied without engine", o.WaitFor (Shows ("close", "Close"), 30000));
			Sleep (3000);
			Text (o, "unknown", "Texte inconnu", "Texte inconnu");		// engine absent: original text
			Text (o, "logic", "ok");

			// Step 2: start the test engine while the window is open.
			engine = StartEngine (port, journal);
			Check ("test engine started", engine != nullptr);
			Check ("engine back: sentence outside the dictionaries translated live (< 25 s)",
					  o.WaitFor (Shows ("unknown", "[en] Texte inconnu"), 25000));
			Text (o, "unknown", "[en] Texte inconnu", "Texte inconnu");
			// The title may arrive in the engine's next batch
			Check ("title translated automatically", o.WaitFor (Shows ("title", "[en] Outil de démonstration"), 5000));
			Text (o, "title", "[en] Outil de démonstration", "Outil de démonstration");
			Text (o, "close", "Close", "Fermer");						// the dictionary wins
			Text (o, "path", "C:\\Projets\\plan.pln", "C:\\Projets\\plan.pln");	// data: never sent
			Text (o, "code", "MUR-01", "MUR-01");
			Text (o, "tree_row", "Murs", "Murs");
			Text (o, "logic", "ok");

			// Step 3: the cache file of the "test" provider, French -> English, must hold the
			// translation (polled for up to about 3 s).
			bool cached = false;
			for (int i = 0; i < 30 && !cached; ++i) {
				std::string c;
				cached = TT::ReadWholeFile (cacheEn, c) && c.find ("\"Texte inconnu\": \"[en] Texte inconnu\"") != std::string::npos;
				if (!cached)
					Sleep (100);
			}
			Check ("translation kept in cache\\fr-en.json (fr language recognised)", cached);

			// Step 4: Hungarian output -> new request to the engine ("[hu] ..."), the
			// dictionary still gives "Bezárás"; then engine disabled -> original text.
			Language (root, "hu", false, "", true);
			Check ("other output language: new automatic translation", o.WaitFor (Shows ("unknown", "[hu] Texte inconnu"), 10000));
			Text (o, "close", "Bezárás", "Fermer");

			Language (root, "en", false, "", false);
			Check ("engine disabled: original text", o.WaitFor (Shows ("unknown", "Texte inconnu"), 5000));
			Text (o, "close", "Close", "Fermer");
			DWORD code = 1;
			Check ("clean exit", o.Finish (code, 10000) && code == 0);
		}
		StopEngine (engine);

		// Step 5: engine enabled in the settings but stopped: a new run must find its
		// translations in the cache.
		Language (root, "en", false, "", true);
		{
			Tool o;
			Check ("relaunch, engine stopped", o.Launch (script, root));
			Check ("translation taken from the cache, without engine", o.WaitFor (Shows ("unknown", "[en] Texte inconnu"), 30000));
			// The title is created before the script's language is recognised: it follows shortly
			Check ("title taken from the cache", o.WaitFor (Shows ("title", "[en] Outil de démonstration"), 5000));
			Text (o, "title", "[en] Outil de démonstration", "Outil de démonstration");
			Text (o, "frame", "Options", "Options");			// already English: never sent to the engine
			DWORD code = 1;
			Check ("clean exit", o.Finish (code, 10000) && code == 0);
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
	std::printf ("\n[3e] Online translators (fake server, real formats of the 5 services)\n");
	{
		// Fake services on 8798, engine on 8796 (both distinct from the other scenarios).
		const int portServices = 8798;
		const int port = 8796;
		FakeServer fake;
		Check ("fake translation server started", fake.Start (portServices));
		const std::wstring base = L"http://127.0.0.1:" + std::to_wstring (portServices);
		for (const wchar_t* name : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + name).c_str (), base.c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_ENGINE_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_TOOL", L"script_online");
		// Step 1: keys.json template written by the add-on code, then overwritten below.
		const std::wstring keys = TT::KeysPath (root);
		std::wstring error;
		Check ("keys.json template created", TT::CreateKeysTemplate (root, error) && TT::FileExists (keys));
		// The Google key is missing at first (the keys match what FakeServer expects)
		Write (keys, "{\"deepl\": {\"key\": \"cle-deepl:fx\"}, \"microsoft\": {\"key\": \"cle-ms\", \"region\": \"westeurope\"},"
					  " \"google\": {\"key\": \"\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"key\": \"\"}}");
		HANDLE engine = StartEngine (port, TT::Join (root, L"log.txt"), L"--keys \"" + keys + L"\"");
		Check ("engine started with keys.json", engine != nullptr);
		{
			// Input language fixed to French, engine on, provider DeepL to start with.
			Tool o;
			Language (root, "en", false, "fr", true, "deepl");
			Check ("launch", o.Launch (script, root));
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
				Check (std::string (s.code) + ": sentence translated (request and response format of the service)",
						  o.WaitFor (Shows ("unknown", s.expected), 20000), o.Read ("unknown").displayed);
				Text (o, "close", "Close", "Fermer");			// dictionary first
			}
			Text (o, "logic", "ok");

			// Step 3: Google without a key -> original kept (checked again after 2 s, to be
			// sure no translation arrives), then the key is written into keys.json.
			Language (root, "en", false, "fr", true, "google");
			Check ("google without a key: original text kept", o.WaitFor (Shows ("unknown", "Texte inconnu"), 10000));
			Sleep (2000);
			Text (o, "unknown", "Texte inconnu", "Texte inconnu");
			Write (keys, "{\"deepl\": {\"key\": \"cle-deepl:fx\"}, \"microsoft\": {\"key\": \"cle-ms\", \"region\": \"westeurope\"},"
						  " \"google\": {\"key\": \"cle-google\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"key\": \"\"}}");
			Check ("Google key hot-added: translated on the next retry (< 30 s)",
					  o.WaitFor (Shows ("unknown", "GG:en:Texte inconnu"), 30000), o.Read ("unknown").displayed);

			// Step 4: each provider has its own cache folder: DeepL's answer comes back.
			Language (root, "en", false, "fr", true, "deepl");
			Check ("back to DeepL: its cache gives its translation back", o.WaitFor (Shows ("unknown", "DL:EN-GB:Texte inconnu"), 10000));
			Text (o, "logic", "ok");
			DWORD code = 1;
			Check ("clean exit", o.Finish (code, 10000) && code == 0);
		}
		// Step 5: privacy checks on everything the fake service received, then the log
		// line about the missing Google key; finally clean up the engine, the server and
		// the variables.
		Check ("no dictionary text sent online (\"Fermer\")", !fake.Received ("Fermer"));
		Check ("no data sent online (path, code)", !fake.Received ("C:\\Projets\\plan.pln") && !fake.Received ("MUR-01"));
		const std::vector<std::string> journal = TT::LastLines (TT::Join (root, L"log.txt"), 300);
		bool reported = false;
		for (const std::string& l : journal)
			reported = reported || l.find ("Google key missing") != std::string::npos;
		Check ("log: the missing Google key is explained", reported);
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
	std::printf ("\n[3f] Term corrections (story -> étage) and MyMemory quota\n");
	{
		// Fake services on 8799, engine on 8795; only MyMemory and LibreTranslate redirected.
		const int portServices = 8799;
		const int port = 8795;
		FakeServer fake;
		Check ("fake translation server started", fake.Start (portServices));
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
		Check ("engine started", engine != nullptr);
		Check ("corrections.json pre-filled by the add-on", TT::FileExists (TT::CorrectionsPath (root)));
		{
			// Step 1: English -> French through LibreTranslate, default corrections.
			Tool o;
			Language (root, "fr", false, "en", true, "libretranslate");
			Check ("launch (script in English)", o.Launch (script, root));
			Check ("\"All stories\" -> \"Toutes les histoires\" corrected to \"Tous les étages\"",
					  o.WaitFor (Shows ("stories", "Tous les étages"), 20000), o.Read ("stories").displayed);
			Text (o, "stories", "Tous les étages", "All stories");

			// Step 2: corrections modified while the window is open
			Write (TT::CorrectionsPath (root),
					"{\"fr\": [{\"source\": \"story\", \"wrong\": \"histoire\", \"correct\": \"niveau\"},"
					" {\"wrong\": \"Toutes les histoires\", \"correct\": \"Tous les niveaux\"}]}");
			Check ("corrections.json modified: applied live, cache included (automatic -s plural)",
					  o.WaitFor (Shows ("stories", "Tous les niveaux"), 5000), o.Read ("stories").displayed);
			Write (TT::CorrectionsPath (root), "{\"fr\": []}");
			Check ("no correction: raw service translation", o.WaitFor (Shows ("stories", "Toutes les histoires"), 5000),
					  o.Read ("stories").displayed);
			Check ("corrections do not touch the dictionary", o.Read ("close").displayed == "Fermer");

			// Step 3: anonymous MyMemory quota reached (English -> German). The dictionary
			// still gives "Schließen"; 2.5 s later the unknown text must still be original,
			// and log.txt must report the anonymous quota (polled for up to about 4 s).
			fake.anonymousQuota = true;
			Language (root, "de", false, "en", true, "mymemory");
			Check ("MyMemory quota: the original text stays (dictionary applied)", o.WaitFor (Shows ("close", "Schließen"), 10000));
			Sleep (2500);
			Text (o, "unknown", "Texte inconnu", "Texte inconnu");
			bool quota = false;
			for (int i = 0; i < 20 && !quota; ++i) {
				for (const std::string& l : TT::LastLines (TT::Join (root, L"log.txt"), 60))
					quota = quota || l.find ("MyMemory quota reached (anonymous") != std::string::npos;
				if (!quota)
					Sleep (200);
			}
			Check ("log: anonymous MyMemory quota detected (e-mail window requested)", quota);

			// Step 4: what the window's "Save" button does: the e-mail is written into
			// keys.json (engine run once with --save-email, exit code 0 = written);
			// the running engine re-reads the keys and MyMemory answers again.
			wchar_t exe[MAX_PATH] = {};
			GetModuleFileNameW (nullptr, exe, MAX_PATH);
			std::wstring path = exe;
			path = path.substr (0, path.find_last_of (L'\\') + 1) + L"TapirTranslatorEngine.exe";
			std::wstring line = L"\"" + path + L"\" --save-email \"" + keys + L"\" me@example.com";
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
			Check ("e-mail saved in keys.json", code == 0 && c.find ("\"email\": \"me@example.com\"") != std::string::npos);
			Check ("the other keys of keys.json are kept", c.find ("\"key\": \"cle-deepl:fx\"") != std::string::npos &&
																	c.find ("\"_explanation\": \"test\"") != std::string::npos);
			Check ("with the e-mail: MyMemory translates again (< 30 s)", o.WaitFor (Shows ("unknown", "MM:de:Texte inconnu"), 30000),
					  o.Read ("unknown").displayed);
			Text (o, "logic", "ok");
			DWORD end_ = 1;
			Check ("clean exit", o.Finish (end_, 10000) && end_ == 0);
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
	std::printf ("\n[4] Python launched outside Tapir (no TAPIR_SCRIPT_ID)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", nullptr);
		Tool o;
		Check ("launch", o.Launch (script, root));
		Check ("layer inactive", o.WaitForLine ("LAYER|missing", 30000));
		Check ("interface not translated", o.WaitFor (Shows ("title", "Outil de démonstration"), 10000));
		DWORD code = 1;
		Check ("clean exit", o.Finish (code, 10000) && code == 0);
		SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", L"tapir-bench-1");
	}

	// --- [5] layer log -----------------------------------------------------------------------
	// The layer never lets an error reach the script: it writes it to log.txt instead.
	// After all the runs above, the last 200 log lines must exist and contain none of the
	// (French) words of the layer's internal error messages: "interception", "after call",
	// "watchdog", "impossible", "unreadable". Offending lines are printed.
	std::printf ("\n[5] Layer log\n");
	{
		const std::vector<std::string> lines = TT::LastLines (TT::Join (root, L"log.txt"), 200);
		bool error = false;
		for (const std::string& l : lines) {
			if (l.find ("interception") != std::string::npos || l.find ("after call") != std::string::npos ||
				l.find ("watchdog") != std::string::npos || l.find ("failed") != std::string::npos ||
				l.find ("unreadable") != std::string::npos) {
				error = true;
				std::printf ("         | %s\n", l.c_str ());
			}
		}
		Check ("log written", !lines.empty ());
		Check ("no internal error of the layer", !error);
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
	std::printf ("\n[6] Language of Tapir Translator (20 languages, ArchiCAD's by default)\n");
	{
		// Steps 1 to 3
		Check ("texts table complete (51 texts x 20 languages)", TT::MissingTexts () == 0,
				  std::to_string (TT::MissingTexts ()) + " empty cell(s)");
		Check ("ArchiCAD FRA -> French", TT::InterfaceIndex (TT::LanguageFromArchicad ("FRA")) == 0);
		Check ("ArchiCAD GER / AUT -> German", TT::LanguageFromArchicad ("GER") == "de" && TT::LanguageFromArchicad ("AUT") == "de");
		Check ("ArchiCAD INT / USA / UKI -> English", TT::LanguageFromArchicad ("INT") == "en" && TT::LanguageFromArchicad ("UKI") == "en");
		Check ("ArchiCAD BRA -> Portuguese, TAI -> Chinese, JPN -> Japanese",
				  TT::LanguageFromArchicad ("BRA") == "pt" && TT::LanguageFromArchicad ("TAI") == "zh" && TT::LanguageFromArchicad ("JPN") == "ja");
		Check ("unknown version -> English", TT::LanguageFromArchicad ("XYZ") == "en" && TT::InterfaceIndex ("xx") == 1);
		Check ("texts with values (TxtF)", TT::TxtF (TT::R_STARTUP, 2, { { "v", "9" }, { "l", "fr -> de" } }) ==
												"Tapir Translator 9 aktiv. Sprachen: fr -> de.");
		Check ("quota window in Hungarian", std::string (TT::Txt (TT::D_SAVE, TT::InterfaceIndex ("hu"))) == "Mentés");

		// Step 4
		TT::Settings r;
		r.interface = "ja";
		std::wstring error;
		TT::WriteSettings (root, r, error);
		Check ("settings: tool language read back (ja)", TT::ReadSettings (root).interface == "ja");
		r.interface = "";
		TT::WriteSettings (root, r, error);
		Check ("settings: tool language = like ArchiCAD (empty)", TT::ReadSettings (root).interface.empty ());

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
		Check ("menus generated for the 20 languages (5 menus + helps each)", all);

		// Step 6: every resource ID read by the ArchiCAD API (a short) must stay below 32767; above,
		// it wraps to a negative number and ArchiCAD unloads the add-on (bug of v0.7.0).
		bool fitsShort = true;
		for (int l = 0; l < TT::InterfaceLanguageCount; ++l) {
			for (int m = 0; m < 5; ++m)
				fitsShort = fitsShort && TT::MenuBase (l) + m + TT::HelpOffset <= 32767 && TT::MenuBase (l) + m > 32000;
		}
		Check ("menu and help IDs between 32001 and 32767 (ArchiCAD API short)", fitsShort);
		bool noIdTooLarge = true;
		for (size_t p = grc.find ("'STR#' "); p != std::string::npos; p = grc.find ("'STR#' ", p + 7))
			noIdTooLarge = noIdTooLarge && std::atoi (grc.c_str () + p + 7) <= 32767;
		Check ("no generated resource above 32767", noIdTooLarge);

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
		Check ("Tapir_Translator.apx contains the 200 menu and help resources", resourcesInside);
		Check ("generated menus: Japanese present", grc.find (TT::Txt (TT::M_TRANSLATOR, TT::InterfaceIndex ("ja"))) != std::string::npos);
	}

	// Summary; the temporary folder is kept when something failed, for examination.
	std::printf ("\n%d OK, %d FAILED\n", g_ok, g_failures);
	if (g_failures == 0)
		DeleteTree (root);
	else
		std::printf ("folder kept for examination: %s\n", TT::Utf8 (root).c_str ());
	return g_failures == 0 ? 0 : 1;
}
