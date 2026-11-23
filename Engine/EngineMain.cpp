// *****************************************************************************
// Tapir Translator - translation engine (TapirTranslatorEngine.exe)
//
//   Role
//   ----
//   A small local HTTP server, bound to 127.0.0.1 only (default port 8779),
//   started by the add-on when ArchiCAD starts (and restarted by it if it
//   stops). It serves every Tapir script at once: the Python translation layer
//   (layer/sitecustomize.py) sends it, in the background, the displayed texts
//   that the dictionaries do not know. The engine relays them to the online
//   translator chosen by the user (DeepL, Microsoft, Google, MyMemory,
//   LibreTranslate - see OnlineTranslators.cpp), or to the test translator /
//   the optional offline M2M100 model.
//
//   Running in its own process keeps network calls (and, with the offline
//   model, a possible crash) away from ArchiCAD and from the scripts.
//
//   Protocol (one request per connection, "Connection: close", JSON bodies)
//   ------------------------------------------------------------------------
//   GET  /status      -> {"version": "...", "providers": [...], "languages": [...]}
//                      (engine version, available providers, supported languages)
//   POST /translate  {"provider": "deepl", "source": "fr" | "", "target": "en", "texts": [...]}
//                   -> 200 {"translations": [...]}   (same order, same count as "texts")
//                   -> 400 bad request (unreadable JSON, unsupported language, unknown provider)
//                   -> 503 the provider failed (network, quota, key...): the layer keeps
//                      the original text and retries later
//                   An empty "source" means "detect automatically"; "provider"
//                   defaults to "mymemory".
//   POST /stop_   -> {"stop": true}, then the server stops
//
//   Command-line options
//   --------------------
//   --port N               listening port (8779)
//   --keys keys.json       providers' keys file, re-read on every request
//   --test                enable the "test" test translator (test bench)
//   --model <folder>      load the offline M2M100 model (build option TT_LOCAL_MODEL)
//   --no-dialog        never show the MyMemory quota window (test bench)
//   --interface <code>     language of the engine's own UI (TT_Texts.hpp)
//   --parent PID           exit when that process (ArchiCAD) exits
//   --log <file>       append log lines to this file (log.txt)
//   --show-quota 0|1    development: only show the quota window and exit
//   --save-email <keys.json> <email>   test bench: write the e-mail and exit
//
//   On success the engine prints "PRET <port>" on stdout once it listens.
// *****************************************************************************

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "Json.hpp"
#include "Translator.hpp"
#include "Dialogue.hpp"
#include "TT_Texts.hpp"

#include <atomic>

#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#pragma comment (lib, "Ws2_32.lib")

namespace {

// Engine version, reported by GET /status.
const char* const kVersion = "0.7.5";
// Path of the log file (--log); empty = no logging.
std::string g_log;

// Appends one timestamped line to the log file:
//   "YYYY-MM-DD HH:MM:SS [engine <pid>] <line>"
// The file is opened and closed for each line so that the add-on and the
// Python layer, which write to the same log.txt, can interleave safely.
// Does nothing when no log file was given or it cannot be opened.
void Log (const std::string& line)
{
	if (g_log.empty ())
		return;
	FILE* f = nullptr;
	if (fopen_s (&f, g_log.c_str (), "ab") != 0 || f == nullptr)
		return;
	// Local date and time of the event
	char date[32];
	std::time_t t = std::time (nullptr);
	std::tm tm = {};
	localtime_s (&tm, &t);
	std::strftime (date, sizeof (date), "%Y-%m-%d %H:%M:%S", &tm);
	std::fprintf (f, "%s [moteur %lu] %s\n", date, static_cast<unsigned long> (GetCurrentProcessId ()), line.c_str ());
	std::fclose (f);
}

// Sends the whole buffer on the socket. send() may write only part of the
// data, so it is called again until everything is sent.
// Returns false as soon as send() fails or the connection is closed.
bool SendAll (SOCKET s, const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size ()) {
		const int n = send (s, data.data () + sent, static_cast<int> (data.size () - sent), 0);
		if (n <= 0)
			return false;
		sent += static_cast<size_t> (n);
	}
	return true;
}

// Writes a complete HTTP/1.1 response with a JSON body.
//   code : HTTP status (200, 400, 404; any other code is labelled
//          "Internal Server Error", which includes the 503 used for provider failures)
//   json : response body, already serialised
// The connection is always closed afterwards ("Connection: close").
void Respond (SOCKET s, int code, const std::string& json)
{
	// Step 1: status line and headers
	const char* status_code = code == 200 ? "OK" : (code == 400 ? "Bad Request" : (code == 404 ? "Not Found" : "Internal Server Error"));
	std::string r = "HTTP/1.1 " + std::to_string (code) + " " + status_code + "\r\n";
	r += "Content-Type: application/json; charset=utf-8\r\n";
	r += "Content-Length: " + std::to_string (json.size ()) + "\r\n";
	r += "Connection: close\r\n\r\n";
	// Step 2: body, then send everything (errors are ignored: the client is gone)
	r += json;
	SendAll (s, r);
}

// Reads one complete HTTP request: headers, then the body according to
// Content-Length.
//   method, path : outputs, e.g. "POST" and "/translate"
//   body           : output, the request body (exactly Content-Length bytes)
// Returns false if the connection fails or times out, if the request line is
// malformed, if the headers exceed 64 KB or the body exceeds 4 MB.
bool ReadRequest (SOCKET s, std::string& method, std::string& path, std::string& body)
{
	// Step 1: receive until the blank line that ends the headers ("\r\n\r\n")
	std::string buffer;
	char piece[8192];
	size_t headersEnd = std::string::npos;
	while (headersEnd == std::string::npos) {
		const int n = recv (s, piece, sizeof (piece), 0);
		if (n <= 0)
			return false;
		buffer.append (piece, static_cast<size_t> (n));
		headersEnd = buffer.find ("\r\n\r\n");
		// Guard against endless headers
		if (buffer.size () > 65536 && headersEnd == std::string::npos)
			return false;
	}
	// Step 2: request line "METHOD /path HTTP/1.1" -> method and path
	const std::string headers = buffer.substr (0, headersEnd);
	const size_t sp1 = headers.find (' ');
	const size_t sp2 = sp1 == std::string::npos ? sp1 : headers.find (' ', sp1 + 1);
	if (sp2 == std::string::npos)
		return false;
	method = headers.substr (0, sp1);
	path = headers.substr (sp1 + 1, sp2 - sp1 - 1);

	// Step 3: Content-Length, searched case-insensitively in a lowercase copy
	// of the headers (15 = length of "content-length:")
	size_t length = 0;
	std::string lowercase = headers;
	for (char& c : lowercase)
		c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	const size_t pos = lowercase.find ("content-length:");
	if (pos != std::string::npos)
		length = static_cast<size_t> (std::strtoul (lowercase.c_str () + pos + 15, nullptr, 10));
	if (length > 4 * 1024 * 1024)
		return false;
	// Step 4: the body starts after the blank line; part of it may already be
	// in the buffer, receive the rest, then drop any extra bytes
	body = buffer.substr (headersEnd + 4);
	while (body.size () < length) {
		const int n = recv (s, piece, sizeof (piece), 0);
		if (n <= 0)
			return false;
		body.append (piece, static_cast<size_t> (n));
	}
	body.resize (length);
	return true;
}

// Server state shared by all requests, filled from the command line in main().
struct Context {
	std::unique_ptr<Translator> test;	// --test (test bench): "[target] text" translator
	std::unique_ptr<Translator> local;	// --model (build option TT_LOCAL_MODEL): offline M2M100
	std::string keys;					// --keys: path of keys.json (online providers' keys)
	bool noDialog = false;			// --no-dialog (test bench): no "MyMemory quota" window
	int interfaceLanguage = 1;			// --interface fr / en / ...: UI language index (TT_Texts.hpp)
	std::atomic<bool> dialogOpen { false };				// the quota window is currently shown
	std::atomic<unsigned long long> laterUntil { 0 };	// GetTickCount64() before which the quota window must not reappear
};

// Reads and parses keys.json.
//   path : path of the file (may be empty)
// Returns the parsed JSON value, or an empty (null) value if the path is
// empty, the file cannot be opened or its JSON is invalid (logged). The file
// is read on every request so that a key added by the user works at once.
Json::Value ReadKeys (const std::string& path)
{
	Json::Value v;
	if (path.empty ())
		return v;
	// Step 1: read the whole file in binary mode
	FILE* f = nullptr;
	if (fopen_s (&f, path.c_str (), "rb") != 0 || f == nullptr)
		return v;
	std::string content;
	char buffer[4096];
	size_t n = 0;
	while ((n = std::fread (buffer, 1, sizeof (buffer), f)) > 0)
		content.append (buffer, n);
	std::fclose (f);
	// Step 2: drop the UTF-8 byte order mark that Notepad may add
	if (content.size () >= 3 && content.compare (0, 3, "\xEF\xBB\xBF") == 0)
		content.erase (0, 3);
	// Step 3: parse
	if (!Json::Reader (content).Read (v))
		Log ("keys.json illisible (JSON invalide) : " + path);
	return v;
}

// Writes the MyMemory e-mail address into keys.json ("mymemory": {"email": ...})
// without touching the other keys.
//   path : path of keys.json
//   email  : address to store
// Returns true if the file was written. The file is written to "<path>.tmp"
// first, then moved over the original, so a failure never leaves a truncated
// keys.json. Note: the file is rewritten by Json::Write (keys sorted).
bool SaveEmail (const std::string& path, const std::string& email)
{
	if (path.empty ())
		return false;
	// Step 1: current content; start from an empty object if it is not an object
	Json::Value v = ReadKeys (path);
	if (v.kind != Json::Value::Object) {
		v = Json::Value ();
		v.kind = Json::Value::Object;
	}
	// Step 2: the "mymemory" section (created or reset if missing / not an object)
	Json::Value& m = v.object["mymemory"];
	if (m.kind != Json::Value::Object) {
		m = Json::Value ();
		m.kind = Json::Value::Object;
	}
	// Step 3: set "email"
	Json::Value e;
	e.kind = Json::Value::String;
	e.str = email;
	m.object["email"] = e;
	// Step 4: serialise, write the temporary file, then replace keys.json
	std::string text;
	Json::Write (v, text);
	text += "\n";
	const std::string tmp = path + ".tmp";
	FILE* f = nullptr;
	if (fopen_s (&f, tmp.c_str (), "wb") != 0 || f == nullptr)
		return false;
	const bool written = std::fwrite (text.data (), 1, text.size (), f) == text.size ();
	std::fclose (f);
	return written && MoveFileExA (tmp.c_str (), path.c_str (), MOVEFILE_REPLACE_EXISTING);
}

// Called when MyMemory answered "MYMEMORY WARNING" or HTTP 429 (daily quota
// reached). Opens the window that asks for an e-mail address (which raises the
// free quota) in its own thread, so the server keeps answering requests.
//   c : server context (keys path, UI language, window state)
// The window is never shown twice at the same time, and not again for one
// hour after the user answered "Later" (or closed it). With --no-dialog the
// event is only logged.
void ReportMyMemoryQuota (Context& c)
{
	// Step 1: window already open, or "Later" period still running -> nothing to do
	const unsigned long long now = GetTickCount64 ();
	if (c.dialogOpen || now < c.laterUntil)
		return;
	// Step 2: is an e-mail already configured? (then the 50 000 characters/day
	// quota is reached too and the window is only informative)
	const Json::Value keys = ReadKeys (c.keys);
	const Json::Value* m = keys.Field ("mymemory");
	std::string email = m != nullptr ? m->Text ("email") : std::string ();
	const bool withEmail = !email.empty ();
	Log (std::string ("quota MyMemory atteint ") + (withEmail ? "(meme avec l'e-mail : environ 50 000 caracteres/jour)"
																   : "(anonyme : environ 5 000 caracteres/jour)"));
	// Step 3: test bench mode: no window, but the same one-hour silence
	if (c.noDialog) {
		Log ("fenetre de saisie de l'e-mail desactivee (--no-dialog)");
		c.laterUntil = now + 3600000ull;
		return;
	}
	// Step 4: show the window in a detached thread (the context outlives it:
	// it lives in main() until the process exits)
	c.dialogOpen = true;
	std::thread ([&c, email, withEmail] () mutable {
		const QuotaAnswer r = AskMyMemoryEmail (email, withEmail, c.interfaceLanguage);
		// Step 5: address entered -> store it in keys.json (used from the next
		// request on); otherwise do not ask again for one hour
		if (r == QuotaAnswer::Saved) {
			if (SaveEmail (c.keys, email))
				Log ("e-mail MyMemory enregistre dans keys.json : quota porte a environ 50 000 caracteres/jour");
			else
				Log ("ERREUR enregistrement de l'e-mail dans " + c.keys);
		} else {
			c.laterUntil = GetTickCount64 () + 3600000ull;
			Log ("fenetre du quota MyMemory fermee sans e-mail : pas de nouvelle question pendant 1 h");
		}
		c.dialogOpen = false;
	}).detach ();
}

// Builds the JSON body of GET /status:
//   {"version": "0.7.0", "providers": ["deepl", ..., "local"?, "test"?], "languages": ["fr", ...]}
// "local" and "test" are listed only when those translators were loaded.
std::string Status (const Context& c)
{
	std::string providers;
	for (const char* f : OnlineProviders)
		providers += std::string (providers.empty () ? "" : ", ") + Json::String (f);
	if (c.local != nullptr)
		providers += ", \"local\"";
	if (c.test != nullptr)
		providers += ", \"test\"";
	std::string languages;
	for (const char* l : ArchicadLanguages)
		languages += std::string (languages.empty () ? "" : ", ") + Json::String (l);
	return "{\"version\": " + Json::String (kVersion) + ", \"providers\": [" + providers + "], \"languages\": [" + languages + "]}";
}

// Handles one client connection: reads the request, routes it and answers.
//   s : connected client socket
//   c : server context
// Returns false only for POST /stop_ (the server must stop), true otherwise.
bool HandleRequest (SOCKET s, Context& c)
{
	// Step 1: read the request
	std::string method, path, body;
	if (!ReadRequest (s, method, path, body)) {
		Respond (s, 400, "{\"error\": \"requete illisible\"}");
		return true;
	}
	// Step 2: routing - /status and /stop_ are answered at once
	if (method == "GET" && path == "/status") {
		Respond (s, 200, Status (c));
		return true;
	}
	if (method == "POST" && path == "/stop") {
		Respond (s, 200, "{\"stop\": true}");
		return false;
	}
	if (method != "POST" || path != "/translate") {
		Respond (s, 404, "{\"error\": \"chemin inconnu\"}");
		return true;
	}

	// Step 3: POST /translate - parse and validate the body (target language
	// required, source language optional, both among the ArchiCAD languages,
	// "texts" must be an array)
	Json::Value request;
	if (!Json::Reader (body).Read (request) || request.kind != Json::Value::Object) {
		Respond (s, 400, "{\"error\": \"JSON illisible\"}");
		return true;
	}
	const std::string source = request.Text ("source");
	const std::string target = request.Text ("target");
	const std::string provider = request.Text ("provider", "mymemory");
	const Json::Value* texts = request.Field ("texts");
	if (target.empty () || !ArchicadLanguage (target) || (!source.empty () && !ArchicadLanguage (source)) ||
		texts == nullptr || texts->kind != Json::Value::Array) {
		Respond (s, 400, "{\"error\": \"langue non prise en charge ou textes absents\"}");
		return true;
	}

	// Step 4: choose the translator. Online providers are created for each
	// request from the current keys.json, so key changes apply immediately.
	std::unique_ptr<Translator> online;
	Translator* translator = nullptr;
	if (provider == "test")
		translator = c.test.get ();
	else if (provider == "local")
		translator = c.local.get ();
	else {
		online = CreateOnlineTranslator (provider, ReadKeys (c.keys));
		translator = online.get ();
	}
	if (translator == nullptr) {
		Respond (s, 400, "{\"error\": " + Json::String ("traducteur inconnu ou indisponible : " + provider) + "}");
		return true;
	}

	// Step 5: input texts (a non-string entry becomes an empty text, so the
	// output keeps the same count and order)
	std::vector<std::string> input;
	for (const Json::Value& v : texts->array)
		input.push_back (v.kind == Json::Value::String ? v.str : std::string ());

	// Step 6: translate (timed for the log)
	std::vector<std::string> output;
	std::string error;
	const DWORD start = GetTickCount ();
	if (!translator->Translate (source, target, input, output, error) || output.size () != input.size ()) {
		// Failure -> 503: the layer keeps the original text and will retry
		// (network, quota, key...). A MyMemory quota error also opens the
		// e-mail window.
		Log ("ERREUR " + translator->Nom () + " " + (source.empty () ? "auto" : source) + " -> " + target + " : " + error);
		if (provider == "mymemory" && (error.find ("MYMEMORY WARNING") != std::string::npos || error.find ("HTTP 429") != std::string::npos))
			ReportMyMemoryQuota (c);
		Respond (s, 503, "{\"error\": " + Json::String (translator->Nom () + " : " + (error.empty () ? "traduction impossible" : error)) + "}");
		return true;
	}
	Log (translator->Nom () + " : " + std::to_string (input.size ()) + " texte(s) " + (source.empty () ? "auto" : source) +
			 " -> " + target + " en " + std::to_string (GetTickCount () - start) + " ms");
	// Step 7: answer {"translations": [...]} in the same order as the request
	std::string json = "{\"translations\": [";
	for (size_t i = 0; i < output.size (); ++i)
		json += (i == 0 ? "" : ", ") + Json::String (output[i]);
	json += "]}";
	Respond (s, 200, json);
	return true;
}

}	// namespace


// Entry point of the engine.
// Step 1: parse the command line (see the file header); --show-quota and
//         --save-email run a one-shot action and return immediately.
// Step 2: load the optional offline model.
// Step 3: watch the parent process (ArchiCAD) and exit with it.
// Step 4: open the listening socket on 127.0.0.1:<port>, exclusively.
// Step 5: serve connections one at a time until POST /stop_.
// Exit codes: 0 normal stop, 2 port already in use, 4 Winsock unavailable
// (the one-shot options return their own codes).
int main (int argc, char** argv)
{
	// Step 1: command line
	int port = 8779;
	Context context;
	std::string model;
	DWORD parent = 0;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--port" && i + 1 < argc)
			port = std::atoi (argv[++i]);
		else if (a == "--test")
			context.test = CreateTestTranslator ();
		else if (a == "--model" && i + 1 < argc)
			model = argv[++i];
		else if (a == "--keys" && i + 1 < argc)
			context.keys = argv[++i];
		else if (a == "--no-dialog")
			context.noDialog = true;
		else if (a == "--interface" && i + 1 < argc)
			context.interfaceLanguage = TT::InterfaceIndex (argv[++i]);
		else if (a == "--show-quota" && i + 1 < argc) {
			// Development: shows the MyMemory quota window (0 = anonymous, 1 = with e-mail)
			// and returns the answer (QuotaAnswer) as the exit code
			std::string email = "moi@exemple.fr";
			return static_cast<int> (AskMyMemoryEmail (email, std::atoi (argv[i + 1]) != 0, context.interfaceLanguage));
		}
		else if (a == "--save-email" && i + 2 < argc) {
			// Utility (test bench): writes the MyMemory e-mail into keys.json and exits
			// (0 = written, 1 = failure)
			const std::string path = argv[i + 1];
			return SaveEmail (path, argv[i + 2]) ? 0 : 1;
		}
		else if (a == "--parent" && i + 1 < argc)
			parent = static_cast<DWORD> (std::strtoul (argv[++i], nullptr, 10));
		else if (a == "--log" && i + 1 < argc)
			g_log = argv[++i];
	}
	// Step 2: offline model (only if --model was given; a failure is logged
	// and the engine keeps running with the other translators)
	if (!model.empty ()) {
		std::string error;
		context.local = CreateTranslatorM2M100 (model, error);
		if (context.local == nullptr)
			Log ("modele local indisponible : " + error);
	}
	// Step 3: stop together with ArchiCAD - a detached thread waits for the
	// parent process handle to be signalled, then ends the whole process
	if (parent != 0) {
		HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, parent);
		if (h != nullptr) {
			std::thread ([h] () {
				WaitForSingleObject (h, INFINITE);
				Log ("ArchiCAD ferme : arret");
				ExitProcess (0);
			}).detach ();
		}
	}

	// Step 4: Winsock 2.2 and a TCP socket listening on the loopback address only
	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0)
		return 4;
	SOCKET listener = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons (static_cast<u_short> (port));
	inet_pton (AF_INET, "127.0.0.1", &address.sin_addr);
	// No SO_REUSEADDR: a second engine on the same port must fail (exclusive use)
	BOOL exclusive = TRUE;
	setsockopt (listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*> (&exclusive), sizeof (exclusive));
	if (bind (listener, reinterpret_cast<sockaddr*> (&address), sizeof (address)) != 0 || listen (listener, 16) != 0) {
		Log ("port " + std::to_string (port) + " deja pris : un moteur tourne sans doute deja");
		closesocket (listener);
		WSACleanup ();
		return 2;
	}
	// Ready: log it and tell the launcher on stdout ("PRET <port>")
	Log ("pret sur 127.0.0.1:" + std::to_string (port) + " (traducteurs en ligne, cles : " + (context.keys.empty () ? std::string ("aucune") : context.keys) + ")");
	std::printf ("READY %d\n", port);
	std::fflush (stdout);

	// Step 5: accept loop - one request per connection, handled sequentially.
	// A 10 s receive timeout keeps a silent client from blocking the server.
	for (;;) {
		SOCKET client = accept (listener, nullptr, nullptr);
		if (client == INVALID_SOCKET)
			continue;
		DWORD delay = 10000;
		setsockopt (client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*> (&delay), sizeof (delay));
		const bool continue_ = HandleRequest (client, context);
		shutdown (client, SD_BOTH);
		closesocket (client);
		if (!continue_)
			break;
	}
	// POST /stop_ received: clean shutdown
	Log ("arret demande");
	closesocket (listener);
	WSACleanup ();
	return 0;
}
