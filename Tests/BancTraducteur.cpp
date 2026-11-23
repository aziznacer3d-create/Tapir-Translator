// *****************************************************************************
// Tapir Translator - test bench outside ArchiCAD
//
//   Reproduces what ArchiCAD + the add-on + the Tapir palette do, using the
//   add-on's own code (Src/TT_Coeur.cpp, Src/TT_Charge.cpp, Src/TT_Textes.cpp)
//   instead of a copy of it:
//     1. Add-on environment preparation: ExtraireCouche / EcrireReglages /
//        PreparerEnvironnement are run on a temporary folder (instead of
//        %APPDATA%\Tapir Translator), so the translation layer
//        (couche\sitecustomize.py), the dictionaries and the settings are
//        exactly what the add-on would write.
//     2. Launch of the fake script Tests/banc_outil.py through
//        "uv run --script <script> --port N", like TapirPalette::ExecuteScript,
//        with TAPIR_SCRIPT_ID set. Python then imports sitecustomize by itself.
//     3. Reading of the script's interface state: for every widget, the text
//        DISPLAYED by Tk (read in the real Tcl interpreter) and the text SEEN by
//        the script (read through tkinter, i.e. through the layer), while the
//        bench changes the languages, the dictionaries, the provider and the
//        keys on the fly.
//     4. A fake online translation server (ServeurFactice) that speaks the real
//        request/response formats of DeepL, Microsoft Translator, Google Cloud
//        Translation, MyMemory and LibreTranslate, so that the real translation
//        engine (TapirTranslatorMoteur.exe) can be tested end to end without
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
//     [3d] automatic translation with the offline test engine (--essai), cache;
//     [3e] the 5 online translators through the fake server and the real engine
//          (keys, hot-added key, per-provider cache, nothing sensitive sent);
//     [3f] term corrections (corrections.json) and the MyMemory quota / e-mail;
//     [4]  Python launched outside Tapir: the layer must do nothing;
//     [5]  layer log: no internal error;
//     [6]  language of Tapir Translator itself (texts table, ArchiCAD language
//          codes, settings, generated menus).
//
//   Output: one OK / ECHEC line per check, exit code 0 when everything passes.
//   Usage : BancTraducteur.exe   (uv must be in the PATH)
// *****************************************************************************

#include "TT_Charge.hpp"
#include "TT_Coeur.hpp"
#include "TT_Textes.hpp"

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

// BANC_DOSSIER_TESTS (UTF-8 path of the Tests folder) is defined by CMakeLists.txt.
#if !defined (BANC_DOSSIER_TESTS)
	#error "BANC_DOSSIER_TESTS doit etre defini (voir CMakeLists.txt)"
#endif

namespace {

// Global counters of passed / failed checks (summary and exit code at the end of main).
int g_ok = 0;
int g_echecs = 0;

// Records one check: prints "OK <name>" or "ECHEC <name> -> <detail>" and counts it.
// The output is flushed at once so that the progress is visible even if the bench hangs.
void Verifier (const std::string& nom, bool condition, const std::string& detail = std::string ())
{
	if (condition) {
		++g_ok;
		std::printf ("  OK     %s\n", nom.c_str ());
	} else {
		++g_echecs;
		std::printf ("  ECHEC  %s%s%s\n", nom.c_str (), detail.empty () ? "" : "  -> ", detail.c_str ());
	}
	std::fflush (stdout);
}

// Tests folder (where banc_outil.py and donnees\banc_outil.json live), as a Windows path:
// the compile-time UTF-8 path is converted to UTF-16 and its '/' turned into '\'.
std::wstring DossierTests ()
{
	std::wstring d = TT::DepuisUtf8 (BANC_DOSSIER_TESTS);
	for (wchar_t& c : d) {
		if (c == L'/')
			c = L'\\';
	}
	return d;
}

// Writes a file with the add-on's own helper; a failure is only printed (the check that
// depends on the file will then fail with a clearer message).
void Ecrire (const std::wstring& chemin, const std::string& contenu)
{
	std::wstring erreur;
	if (!TT::EcrireFichier (chemin, contenu, erreur))
		std::printf ("  (ecriture impossible : %s)\n", TT::Utf8 (erreur).c_str ());
}

// Recursively deletes a folder and everything in it (used to start each run from a clean
// temporary folder and to clean up afterwards).
void SupprimerArbre (const std::wstring& dossier)
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (TT::Joindre (dossier, L"*").c_str (), &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring nom = d.cFileName;
			if (nom == L"." || nom == L"..")
				continue;
			const std::wstring chemin = TT::Joindre (dossier, nom);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				SupprimerArbre (chemin);
			else
				DeleteFileW (chemin.c_str ());
		} while (FindNextFileW (h, &d));
		FindClose (h);
	}
	RemoveDirectoryW (dossier.c_str ());
}


// =============================================================================
//  The running fake script
// =============================================================================

// One widget text as reported by banc_outil.py on a "V|<key>|<displayed>|<seen>" line.
struct Valeur {
	std::string affiche;	// read in the Tcl interpreter (what the user sees)
	std::string vu;			// read through the tkinter API (what the script logic receives)
};

// Drives one run of banc_outil.py:
//   - starts it like the Tapir palette (uv run --script), stdout/stderr on a pipe;
//   - a reader thread parses its output: each block of V| lines ended by FIN_ETAT
//     becomes the current state (key -> Valeur), any other line is kept in autres_;
//   - commands are sent through <exchange folder>\commande.txt ("maj", "fin").
// The destructor kills the process if it is still running.
class Outil {
public:
	~Outil () { Arreter (); }

	// Starts the script; 'echange' is the folder holding commande.txt (BANC_ECHANGE).
	// Steps: 1. empty commande.txt; 2. create the output pipe (write end inherited by the
	// child, read end not); 3. CreateProcess of the uv command line, without a console
	// window, with the current environment (PYTHONPATH, TAPIR_SCRIPT_ID...); 4. start the
	// reader thread.
	bool Lancer (const std::wstring& script, const std::wstring& echange)
	{
		echange_ = echange;
		Ecrire (TT::Joindre (echange_, L"commande.txt"), "");
		SECURITY_ATTRIBUTES sa = { sizeof (sa), nullptr, TRUE };
		HANDLE ecriture = nullptr;
		if (!CreatePipe (&lecture_, &ecriture, &sa, 0))
			return false;
		SetHandleInformation (lecture_, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si = {};
		si.cb = sizeof (si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = ecriture;
		si.hStdError = ecriture;
		si.hStdInput = GetStdHandle (STD_INPUT_HANDLE);
		PROCESS_INFORMATION pi = {};
		// Same as TapirPalette::ExecuteScript: uv run --script <file> --port <n>
		std::wstring ligne = L"uv run --script \"" + script + L"\" --port 19723";
		std::vector<wchar_t> tampon (ligne.begin (), ligne.end ());
		tampon.push_back (L'\0');
		const BOOL ok = CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		// The child has its own copy of the write end: close ours so that ReadFile ends
		// when the child exits.
		CloseHandle (ecriture);
		if (!ok)
			return false;
		CloseHandle (pi.hThread);
		processus_ = pi.hProcess;
		lecteur_ = std::thread ([this] () { Lire (); });
		return true;
	}

	// Sends a command to the script by writing commande.txt (polled by the script every tick).
	void Commande (const char* c) { Ecrire (TT::Joindre (echange_, L"commande.txt"), c); }

	// Waits for a state that satisfies the condition (at most 'ms' milliseconds), polling
	// every 50 ms. Used because translation is asynchronous: the layer reloads settings and
	// dictionaries every 0.3 s and online translations arrive later.
	bool Attendre (const std::function<bool (const std::map<std::string, Valeur>&)>& condition, int ms)
	{
		const auto fin = std::chrono::steady_clock::now () + std::chrono::milliseconds (ms);
		while (std::chrono::steady_clock::now () < fin) {
			{
				std::lock_guard<std::mutex> verrou (m_);
				if (!etat_.empty () && condition (etat_))
					return true;
			}
			std::this_thread::sleep_for (std::chrono::milliseconds (50));
		}
		return false;
	}

	// Waits (at most 'ms' milliseconds) for a non-state output line starting with 'debut'
	// (e.g. "COUCHE|active", "SUIVANT|ok").
	bool AttendreLigne (const std::string& debut, int ms)
	{
		const auto fin = std::chrono::steady_clock::now () + std::chrono::milliseconds (ms);
		while (std::chrono::steady_clock::now () < fin) {
			if (AVuLigne (debut))
				return true;
			std::this_thread::sleep_for (std::chrono::milliseconds (50));
		}
		return false;
	}

	// True if a non-state output line starting with 'debut' has already been received.
	bool AVuLigne (const std::string& debut)
	{
		std::lock_guard<std::mutex> verrou (m_);
		for (const std::string& l : autres_) {
			if (l.compare (0, debut.size (), debut) == 0)
				return true;
		}
		return false;
	}

	// Current value of one key of the last complete state ("<absent>" if never reported).
	Valeur Lire (const std::string& cle)
	{
		std::lock_guard<std::mutex> verrou (m_);
		auto it = etat_.find (cle);
		return it == etat_.end () ? Valeur { "<absent>", "<absent>" } : it->second;
	}

	// Asks the script to close ("fin") and waits up to 'ms' milliseconds for the process to
	// exit. Returns true and its exit code if it exited by itself; otherwise kills it and
	// returns false.
	bool Finir (DWORD& code, int ms)
	{
		Commande ("fin");
		if (processus_ == nullptr || WaitForSingleObject (processus_, static_cast<DWORD> (ms)) != WAIT_OBJECT_0) {
			Arreter ();
			return false;
		}
		GetExitCodeProcess (processus_, &code);
		Arreter ();
		return true;
	}

	// Copy of every non-state output line (COUCHE|..., SUIVANT|..., Python tracebacks, ...).
	std::vector<std::string> Autres ()
	{
		std::lock_guard<std::mutex> verrou (m_);
		return autres_;
	}

private:
	// Stops the run: kills the process if it is still alive, waits for the reader thread
	// and closes the handles. Safe to call several times.
	void Arreter ()
	{
		if (processus_ != nullptr) {
			if (WaitForSingleObject (processus_, 0) != WAIT_OBJECT_0) {
				TerminateProcess (processus_, 99);
				// uv's Python child may keep the pipe open: unblock the pending read
				CancelIoEx (lecture_, nullptr);
			}
			CloseHandle (processus_);
			processus_ = nullptr;
		}
		if (lecteur_.joinable ())
			lecteur_.join ();
		if (lecture_ != nullptr) {
			CloseHandle (lecture_);
			lecture_ = nullptr;
		}
	}

	// Reader thread: splits the script output into lines (CR removed) and
	//   - "V|<key>|<displayed>|<seen>" -> stored in a state being built (the seen part is
	//     everything after the second '|');
	//   - "FIN_ETAT"                   -> the state being built becomes the current state
	//     in one go, so a check never sees a half-written state;
	//   - anything else                -> appended to autres_.
	// Ends when the pipe is closed (or the read is cancelled by Arreter).
	void Lire ()
	{
		std::string reste;
		std::map<std::string, Valeur> enCours;
		char tampon[4096];
		DWORD lu = 0;
		while (ReadFile (lecture_, tampon, sizeof (tampon), &lu, nullptr) && lu > 0) {
			reste.append (tampon, lu);
			size_t pos;
			while ((pos = reste.find ('\n')) != std::string::npos) {
				std::string ligne = reste.substr (0, pos);
				reste.erase (0, pos + 1);
				if (!ligne.empty () && ligne.back () == '\r')
					ligne.pop_back ();
				if (ligne.compare (0, 2, "V|") == 0) {
					const size_t a = ligne.find ('|', 2);
					const size_t b = a == std::string::npos ? a : ligne.find ('|', a + 1);
					if (b != std::string::npos)
						enCours[ligne.substr (2, a - 2)] = Valeur { ligne.substr (a + 1, b - a - 1), ligne.substr (b + 1) };
				} else if (ligne == "FIN_ETAT") {
					std::lock_guard<std::mutex> verrou (m_);
					etat_ = enCours;
					enCours.clear ();
				} else {
					std::lock_guard<std::mutex> verrou (m_);
					autres_.push_back (ligne);
				}
			}
		}
	}

	// echange_: folder of commande.txt; processus_: the uv process; lecture_: read end of
	// the output pipe; lecteur_: reader thread; m_ protects etat_ (last complete state) and
	// autres_ (non-state lines), shared between the reader thread and the checks.
	std::wstring echange_;
	HANDLE processus_ = nullptr;
	HANDLE lecture_ = nullptr;
	std::thread lecteur_;
	std::mutex m_;
	std::map<std::string, Valeur> etat_;
	std::vector<std::string> autres_;
};


// Checks the displayed text of one key (and, if given, the text seen by the script) in
// the current state. One or two checks, labelled "<key> affiche ..." / "<key> vu ...".
void Texte (Outil& o, const std::string& cle, const std::string& affiche, const char* vu = nullptr)
{
	const Valeur v = o.Lire (cle);
	Verifier (cle + " affiche \"" + affiche + "\"", v.affiche == affiche, "affiche \"" + v.affiche + "\"");
	if (vu != nullptr)
		Verifier (cle + " vu par le script \"" + vu + "\"", v.vu == vu, "vu \"" + v.vu + "\"");
}

// Builds a condition for Outil::Attendre: "the key is displayed with exactly this text".
std::function<bool (const std::map<std::string, Valeur>&)> Affiche (const std::string& cle, const std::string& texte)
{
	return [cle, texte] (const std::map<std::string, Valeur>& e) {
		auto it = e.find (cle);
		return it != e.end () && it->second.affiche == texte;
	};
}

// Does what the add-on menu does: writes reglages.json with
//   code        output language ("" = no translation),
//   collecte    collect untranslated texts into manquants\,
//   entree      input language ("" = automatic detection),
//   moteur      online / automatic translation enabled,
//   fournisseur translation provider ("essai" = offline test engine, "deepl", ...).
// Two checks: the write succeeds, and LireReglages reads back the same values. The running
// script's layer picks the new settings up by itself (reloaded every 0.3 s).
void Langue (const std::wstring& racine, const char* code, bool collecte, const char* entree = "", bool moteur = false,
			 const char* fournisseur = "essai")
{
	TT::Reglages r;
	r.entree = entree;
	r.langue = code;
	r.collecte = collecte;
	r.moteur = moteur;
	r.fournisseur = fournisseur;
	std::wstring erreur;
	Verifier (std::string ("EcrireReglages entree=\"") + entree + "\" sortie=\"" + code + "\"" + (moteur ? " moteur" : ""),
			  TT::EcrireReglages (racine, r, erreur), TT::Utf8 (erreur));
	const TT::Reglages relu = TT::LireReglages (racine);
	Verifier ("LireReglages relit les reglages", relu.entree == r.entree && relu.langue == r.langue && relu.collecte == r.collecte &&
			  relu.moteur == r.moteur && relu.fournisseur == r.fournisseur);
}

// Starts the translation engine TapirTranslatorMoteur.exe found next to the bench, like the
// add-on does: <options> --port <port> --parent <bench pid> --journal "<log>".
// Options used here: L"--essai" (offline test engine, translation = "[<lang>] <text>"),
// L"--cles \"<cles.json>\"" (online providers), optionally with L"--sans-dialogue".
// Waits 500 ms for it to start listening and returns its process handle (nullptr on failure).
HANDLE LancerMoteur (int port, const std::wstring& journal, const std::wstring& options = L"--essai")
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW (nullptr, exe, MAX_PATH);
	std::wstring chemin = exe;
	chemin = chemin.substr (0, chemin.find_last_of (L'\\') + 1) + L"TapirTranslatorMoteur.exe";
	std::wstring ligne = L"\"" + chemin + L"\" " + options + L" --port " + std::to_wstring (port) + L" --parent " +
						 std::to_wstring (GetCurrentProcessId ()) + L" --journal \"" + journal + L"\"";
	std::vector<wchar_t> tampon (ligne.begin (), ligne.end ());
	tampon.push_back (L'\0');
	STARTUPINFOW si = {};
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
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
// records every text it receives (ARecu, to prove that dictionary texts and data are never
// sent online), can simulate the anonymous MyMemory quota (quotaAnonyme), and mistranslates
// "All stories" into French like a real online translator would (for the term corrections).
// The engine is pointed at it through the TAPIR_TRANSLATOR_URL_<SERVICE> variables.
class ServeurFactice {
public:
	~ServeurFactice () { Arreter (); }

	// Opens the listening socket on 127.0.0.1:<port> and starts the accept thread.
	bool Demarrer (int port)
	{
		WSADATA wsa;
		WSAStartup (MAKEWORD (2, 2), &wsa);
		ecoute_ = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons (static_cast<u_short> (port));
		inet_pton (AF_INET, "127.0.0.1", &a.sin_addr);
		if (bind (ecoute_, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0 || listen (ecoute_, 16) != 0)
			return false;
		fil_ = std::thread ([this] () { Boucle (); });
		return true;
	}

	// Closes the listening socket (which makes accept fail and ends the thread), then joins it.
	void Arreter ()
	{
		if (ecoute_ != INVALID_SOCKET) {
			closesocket (ecoute_);
			ecoute_ = INVALID_SOCKET;
		}
		if (fil_.joinable ())
			fil_.join ();
	}

	// True if this exact text was ever sent to the fake service (by any provider route).
	bool ARecu (const std::string& texte)
	{
		std::lock_guard<std::mutex> v (m_);
		return recus_.count (texte) > 0;
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

	// Decoded value of the query-string parameter 'nom' ("" if absent). The name must start
	// the query or follow '&' / '?', so that a short name (e.g. "to") never matches the end
	// of a longer parameter name.
	static std::string Parametre (const std::string& requete, const std::string& nom)
	{
		size_t p = 0;
		while ((p = requete.find (nom + "=", p)) != std::string::npos) {
			if (p == 0 || requete[p - 1] == '&' || requete[p - 1] == '?') {
				const size_t debut = p + nom.size () + 1;
				const size_t fin = requete.find ('&', debut);
				return Decoder (requete.substr (debut, fin == std::string::npos ? std::string::npos : fin - debut));
			}
			++p;
		}
		return std::string ();
	}

	// Records a received text (thread-safe), for ARecu.
	void Noter (const std::string& t)
	{
		std::lock_guard<std::mutex> v (m_);
		recus_.insert (t);
	}

	// Sends a complete HTTP/1.1 response with a JSON body and "Connection: close".
	static void Repondre (SOCKET s, int code, const std::string& json)
	{
		const std::string r = "HTTP/1.1 " + std::to_string (code) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
							  std::to_string (json.size ()) + "\r\nConnection: close\r\n\r\n" + json;
		send (s, r.data (), static_cast<int> (r.size ()), 0);
	}

	// Accept loop: one connection at a time, one request per connection, then close.
	// Ends when the listening socket is closed by Arreter.
	void Boucle ()
	{
		for (;;) {
			SOCKET c = accept (ecoute_, nullptr, nullptr);
			if (c == INVALID_SOCKET)
				return;
			Traiter (c);
			shutdown (c, SD_BOTH);
			closesocket (c);
		}
	}

	// Handles one HTTP request:
	//   1. read until the end of the headers, then the body up to Content-Length;
	//   2. extract the path and the query string of the request target, parse the JSON body;
	//   3. dispatch on the path to the emulated provider and answer in its format.
	void Traiter (SOCKET c)
	{
		std::string t;
		char b[8192];
		size_t fin = std::string::npos;
		while (fin == std::string::npos) {
			const int n = recv (c, b, sizeof (b), 0);
			if (n <= 0)
				return;
			t.append (b, static_cast<size_t> (n));
			fin = t.find ("\r\n\r\n");
		}
		// Header names are case-insensitive: they are searched in a lower-case copy.
		std::string entetes = t.substr (0, fin);
		std::string corps = t.substr (fin + 4);
		std::string minus = entetes;
		for (char& ch : minus)
			ch = static_cast<char> (std::tolower (static_cast<unsigned char> (ch)));
		const size_t cl = minus.find ("content-length:");
		const size_t longueur = cl == std::string::npos ? 0 : static_cast<size_t> (std::strtoul (minus.c_str () + cl + 15, nullptr, 10));
		while (corps.size () < longueur) {
			const int n = recv (c, b, sizeof (b), 0);
			if (n <= 0)
				break;
			corps.append (b, static_cast<size_t> (n));
		}
		// Value of a header (name given in lower case), taken from the original headers so
		// that its case is kept; "" if the header is absent.
		const auto entete = [&] (const char* nom) {
			const size_t p = minus.find (std::string ("\r\n") + nom + ":");
			if (p == std::string::npos)
				return std::string ();
			size_t d = p + 3 + std::strlen (nom);
			while (d < entetes.size () && entetes[d] == ' ')
				++d;
			return entetes.substr (d, entetes.find ("\r\n", d) - d);
		};
		// Request line "METHOD <target> HTTP/1.1": target = path [? query].
		const size_t sp1 = entetes.find (' ');
		const std::string cibleUrl = entetes.substr (sp1 + 1, entetes.find (' ', sp1 + 1) - sp1 - 1);
		const size_t q = cibleUrl.find ('?');
		const std::string chemin = cibleUrl.substr (0, q);
		const std::string requete = q == std::string::npos ? std::string () : cibleUrl.substr (q + 1);
		Json::Valeur v;
		Json::Lecteur (corps).Lire (v);

		// Translates a JSON array of texts (strings, or objects with a "Text" field) into a
		// comma-separated list of "<prefix>:<target>:<text>" JSON strings, each one wrapped in
		// {"<champ>": ...} when 'champ' is given. Every text is recorded (Noter).
		const auto liste = [&] (const Json::Valeur* tab, const char* prefixe, const std::string& cible, const char* champ) {
			std::string r;
			if (tab != nullptr && tab->genre == Json::Valeur::Tableau) {
				for (const Json::Valeur& e : tab->tableau) {
					const std::string texte = e.genre == Json::Valeur::Chaine ? e.chaine : e.Texte ("Text");
					Noter (texte);
					const std::string trad = Json::Chaine (std::string (prefixe) + ":" + cible + ":" + texte);
					r += std::string (r.empty () ? "" : ", ") + (champ != nullptr ? "{\"" + std::string (champ) + "\": " + trad + "}" : trad);
				}
			}
			return r;
		};

		// DeepL: /v2/translate, header "Authorization: DeepL-Auth-Key <key>",
		// body {"text": [...], "target_lang": ...} -> {"translations": [{"text": ...}]}.
		// Wrong key -> 403, like DeepL.
		if (chemin == "/v2/translate") {						// DeepL
			if (entete ("authorization") != "DeepL-Auth-Key cle-deepl:fx")
				return Repondre (c, 403, "{\"message\": \"Authorization failed\"}");
			return Repondre (c, 200, "{\"translations\": [" + liste (v.Champ ("text"), "DL", v.Texte ("target_lang"), "text") + "]}");
		}
		// Microsoft Translator: /translate?api-version=...&to=<lang>, header
		// "Ocp-Apim-Subscription-Key", body [{"Text": ...}, ...] ->
		// [{"translations": [{"text": ..., "to": ...}]}, ...]. Wrong key -> 401.
		// (Told apart from LibreTranslate, which uses the same path, by "api-version".)
		if (chemin == "/translate" && !Parametre (requete, "api-version").empty ()) {	// Microsoft
			if (entete ("ocp-apim-subscription-key") != "cle-ms")
				return Repondre (c, 401, "{\"error\": {\"message\": \"invalid key\"}}");
			const std::string to = Parametre (requete, "to");
			std::string r;
			for (const Json::Valeur& e : v.tableau) {
				Noter (e.Texte ("Text"));
				r += std::string (r.empty () ? "" : ", ") + "{\"translations\": [{\"text\": " + Json::Chaine ("MS:" + to + ":" + e.Texte ("Text")) +
					 ", \"to\": " + Json::Chaine (to) + "}]}";
			}
			return Repondre (c, 200, "[" + r + "]");
		}
		// Google Cloud Translation v2: /language/translate/v2?key=<key>, body {"q": [...],
		// "target": ...} -> {"data": {"translations": [{"translatedText": ...}]}}.
		// Wrong or missing key -> 400 "API key not valid".
		if (chemin == "/language/translate/v2") {				// Google
			if (Parametre (requete, "key") != "cle-google")
				return Repondre (c, 400, "{\"error\": {\"message\": \"API key not valid\"}}");
			return Repondre (c, 200, "{\"data\": {\"translations\": [" + liste (v.Champ ("q"), "GG", v.Texte ("target"), "translatedText") + "]}}");
		}
		// MyMemory: /get?q=<text>&langpair=<src>|<dst>[&de=<e-mail>], one text per request
		// -> {"responseData": {"translatedText": ...}, "responseStatus": 200}.
		// With quotaAnonyme set and no e-mail ("de"), it answers like the real service once
		// the anonymous daily quota is used up: "MYMEMORY WARNING ..." with status 429.
		if (chemin == "/get") {									// MyMemory
			const std::string texte = Parametre (requete, "q");
			const std::string paire = Parametre (requete, "langpair");
			Noter (texte);
			if (quotaAnonyme && Parametre (requete, "de").empty ())
				return Repondre (c, 200, "{\"responseData\": {\"translatedText\": \"MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE "
										 "TRANSLATIONS FOR TODAY. NEXT AVAILABLE IN 10 HOURS\"}, \"responseStatus\": 429, \"quotaFinished\": true}");
			return Repondre (c, 200, "{\"responseData\": {\"translatedText\": " + Json::Chaine ("MM:" + paire.substr (paire.find ('|') + 1) + ":" + texte) +
										 "}, \"responseStatus\": 200}");
		}
		// LibreTranslate: /translate (without api-version), body {"q": [...], "target": ...} ->
		// {"translatedText": [...]}. No key check (none is configured in the bench).
		if (chemin == "/translate") {							// LibreTranslate
			// Towards French, a word-for-word translation typical of online translators:
			// "All stories" -> "Toutes les histoires" ("story" -> "histoire" instead of
			// "étage"), used by scenario [3f] to test the term corrections.
			const Json::Valeur* q = v.Champ ("q");
			if (v.Texte ("target") == "fr" && q != nullptr && q->genre == Json::Valeur::Tableau) {
				std::string r;
				for (const Json::Valeur& e : q->tableau) {
					Noter (e.chaine);
					r += std::string (r.empty () ? "" : ", ") +
						 Json::Chaine (e.chaine == "All stories" ? std::string ("Toutes les histoires") : "LT:fr:" + e.chaine);
				}
				return Repondre (c, 200, "{\"translatedText\": [" + r + "]}");
			}
			return Repondre (c, 200, "{\"translatedText\": [" + liste (v.Champ ("q"), "LT", v.Texte ("target"), nullptr) + "]}");
		}
		// Unknown route.
		Repondre (c, 404, "{}");
	}

public:
	std::atomic<bool> quotaAnonyme { false };	// MyMemory without e-mail: "MYMEMORY WARNING"

private:
	// Listening socket, accept thread, and the set of received texts (protected by m_).
	SOCKET ecoute_ = INVALID_SOCKET;
	std::thread fil_;
	std::mutex m_;
	std::set<std::string> recus_;
};

// Kills the translation engine started by LancerMoteur (if any) and waits for it to exit.
void ArreterMoteur (HANDLE& moteur)
{
	if (moteur != nullptr) {
		TerminateProcess (moteur, 0);
		WaitForSingleObject (moteur, 5000);
		CloseHandle (moteur);
		moteur = nullptr;
	}
}

// Prints the script's non-state output (e.g. a Python traceback), to help diagnose a failure.
void MontrerSortie (Outil& o)
{
	for (const std::string& l : o.Autres ())
		std::printf ("         | %s\n", l.c_str ());
}

// Prepares the add-on folder 'racine' as the add-on does at ArchiCAD start-up, and checks it:
//   1. start from an empty folder, run ExtraireCouche (layer, common dictionary, templates
//      of _perso.json, cles.json and corrections.json) and check the expected files exist;
//   2. simulate an old installation (outdated _commun.json, user-edited _perso.json), run
//      ExtraireCouche again and check that _commun.json is replaced but _perso.json is kept;
//   3. install the fake script's own dictionary (Tests\donnees\banc_outil.json) as
//      dictionnaires\banc_outil.json (named after the .py launched by Tapir).
// Returns the content of that dictionary (scenario [2] edits it live).
std::string Preparer (const std::wstring& racine)
{
	SupprimerArbre (racine);
	std::vector<std::wstring> journal;
	Verifier ("ExtraireCouche", TT::ExtraireCouche (racine, journal));
	Verifier ("couche\\sitecustomize.py ecrit", TT::FichierExiste (TT::Joindre (TT::Joindre (racine, L"couche"), L"sitecustomize.py")));
	Verifier ("dictionnaires\\_commun.json ecrit", TT::FichierExiste (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_commun.json")));
	Verifier ("dictionnaires\\_perso.json cree", TT::FichierExiste (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_perso.json")));
	// An old (modified) _commun.json is replaced by the one of this version; _perso.json is kept
	Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_commun.json"), "{\"ancien\": true}");
	Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_perso.json"), "{\"format\": 2, \"termes\": []}");
	{
		std::vector<std::wstring> j2;
		TT::ExtraireCouche (racine, j2);
		std::string c, p;
		TT::LireFichier (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_commun.json"), c);
		TT::LireFichier (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_perso.json"), p);
		Verifier ("ancien _commun.json remplace par celui de la version", c.find ("\"termes\"") != std::string::npos);
		Verifier ("_perso.json de l'utilisateur garde", p == "{\"format\": 2, \"termes\": []}");
	}
	std::string dico;
	TT::LireFichier (TT::Joindre (TT::Joindre (DossierTests (), L"donnees"), L"banc_outil.json"), dico);
	Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"banc_outil.json"), dico);
	return dico;
}

}	// namespace


// Runs every scenario in order. Each one leaves the settings (and environment variables)
// in a known state for the next one. Scenarios [1] to [6] share one temporary folder,
//   %TEMP%\TapirTranslatorBanc_<pid>, which plays the add-on folder (%APPDATA%\Tapir
//   Translator) and is also the exchange folder of the fake script (BANC_ECHANGE).
// It is deleted at the end if every check passed, and kept for examination otherwise.
int main ()
{
	// UTF-8 console (the check labels contain accented texts).
	SetConsoleOutputCP (CP_UTF8);
	wchar_t temp[MAX_PATH] = {};
	GetTempPathW (MAX_PATH, temp);
	const std::wstring racine = TT::Joindre (temp, L"TapirTranslatorBanc_" + std::to_wstring (GetCurrentProcessId ()));
	const std::wstring script = TT::Joindre (DossierTests (), L"banc_outil.py");
	std::printf ("Tapir Translator %s - banc de test\n  dossier : %s\n  script  : %s\n\n", TT::VersionAddOn,
				 TT::Utf8 (racine).c_str (), TT::Utf8 (script).c_str ());

	// --- [0] payload embedded at the end of the .apx (TT_Charge) ---------------------------
	// The build appends the engine, the model and the licences to the .apx (Empaqueter); the
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
		const std::wstring dossier = TT::Joindre (temp, L"TapirTranslatorCharge_" + std::to_wstring (GetCurrentProcessId ()));
		SupprimerArbre (dossier);
		const std::wstring source = TT::Joindre (dossier, L"source");
		TT::CreerDossier (TT::Joindre (source, L"modele\\sous"));
		std::string gros (3 * 1024 * 1024 + 17, '\0');		// larger than the copy buffer (1 MB)
		for (size_t i = 0; i < gros.size (); ++i)
			gros[i] = static_cast<char> ((i * 31 + 7) & 0xFF);
		Ecrire (TT::Joindre (source, L"moteur.exe"), "MZ faux moteur");
		Ecrire (TT::Joindre (source, L"modele\\poids.bin"), gros);
		Ecrire (TT::Joindre (source, L"modele\\sous\\vocabulaire é.txt"), "mur\nwall\n");
		// Fake .apx: stands for the DLL produced by the linker.
		const std::wstring apx = TT::Joindre (dossier, L"faux.apx");
		const std::string contenuApx = "faux contenu de DLL";
		Ecrire (apx, contenuApx);

		// Step 2: pack twice; the second pass must replace the first payload, so the file
		// size stays about DLL + payload (one copy of the big file, not two).
		std::wstring empreinte, empreinte2;
		std::vector<std::wstring> journal;
		Verifier ("Empaqueter", TT::Empaqueter (apx, source, empreinte, journal));
		Verifier ("Empaqueter une seconde fois (ancienne charge retiree)", TT::Empaqueter (apx, source, empreinte2, journal));
		Verifier ("meme contenu = meme empreinte", empreinte == empreinte2 && !empreinte.empty ());
		std::string apxLu;
		TT::LireFichier (apx, apxLu);
		Verifier ("le debut du .apx est intact", apxLu.compare (0, contenuApx.size (), contenuApx) == 0);
		Verifier ("une seule charge apres deux passages", apxLu.size () < contenuApx.size () + gros.size () + 2000,
				  std::to_string (apxLu.size ()));
		// Step 3: payload table (3 files, same fingerprint), then extraction into
		// <base>\<fingerprint> and byte-for-byte comparison of the files.
		std::wstring e;
		std::vector<std::wstring> chemins;
		Verifier ("LireCharge : 3 fichiers", TT::LireCharge (apx, e, chemins) && chemins.size () == 3 && e == empreinte);

		std::wstring extrait;
		const std::wstring base = TT::Joindre (dossier, L"extrait");
		Verifier ("ExtraireCharge", TT::ExtraireCharge (apx, base, extrait, journal));
		Verifier ("dossier d'extraction = empreinte", extrait == TT::Joindre (base, empreinte), TT::Utf8 (extrait));
		std::string lu;
		Verifier ("gros fichier identique", TT::LireFichier (TT::Joindre (extrait, L"modele\\poids.bin"), lu) && lu == gros);
		Verifier ("fichier a accent identique", TT::LireFichier (TT::Joindre (extrait, L"modele\\sous\\vocabulaire é.txt"), lu) && lu == "mur\nwall\n");
		// A second extraction of the same version must succeed and end its log with
		// "deja extrait" (nothing copied again).
		journal.clear ();
		Verifier ("deuxieme extraction : rien a refaire", TT::ExtraireCharge (apx, base, extrait, journal) &&
				  !journal.empty () && journal.back ().find (L"deja extrait") != std::wstring::npos);

		// Steps 4 and 5: a modified engine gives a new fingerprint and a new extraction
		// folder; a file without payload (here the fake engine itself) is rejected.
		Ecrire (TT::Joindre (source, L"moteur.exe"), "MZ faux moteur v2");
		std::wstring empreinte3;
		TT::Empaqueter (apx, source, empreinte3, journal);
		Verifier ("nouvelle version = nouvelle empreinte", empreinte3 != empreinte);
		Verifier ("nouvelle version extraite a cote", TT::ExtraireCharge (apx, base, extrait, journal) &&
				  TT::LireFichier (TT::Joindre (extrait, L"moteur.exe"), lu) && lu == "MZ faux moteur v2");
		Verifier ("fichier sans charge refuse", !TT::LireCharge (TT::Joindre (source, L"moteur.exe"), e, chemins));

		// Step 6: the real .apx produced by the build (same folder as the bench) must carry
		// TapirTranslatorMoteur.exe in its payload.
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW (nullptr, exe, MAX_PATH);
		std::wstring vrai = exe;
		vrai = vrai.substr (0, vrai.find_last_of (L'\\') + 1) + L"Tapir_Translator.apx";
		chemins.clear ();
		bool moteurDedans = false;
		if (TT::LireCharge (vrai, e, chemins)) {
			for (const std::wstring& c : chemins)
				moteurDedans = moteurDedans || c == L"TapirTranslatorMoteur.exe";
		}
		Verifier ("Tapir_Translator.apx contient TapirTranslatorMoteur.exe", moteurDedans);
		SupprimerArbre (dossier);
	}

	// --- [1] environment, like the add-on's Initialize () --------------------------------
	//   1. Preparer: layer and dictionaries extracted into the temporary add-on folder;
	//   2. an existing PYTHONPATH (here with its own sitecustomize.py, which prints
	//      "SUIVANT|ok") must be kept behind the layer folder, and the layer must chain to
	//      that other sitecustomize (checked in [2]);
	//   3. PreparerEnvironnement must be idempotent (no duplicate entry when called again);
	//   4. the variables Tapir / the bench give the script: TAPIR_SCRIPT_ID (marks a Tapir
	//      script, without it the layer does nothing), UTF-8 output, BANC_ECHANGE;
	//   5. initial settings: no translation (original language), collection enabled.
	std::printf ("\n[1] Environnement prepare par l'add-on\n");
	std::string dico = Preparer (racine);
	// An existing PYTHONPATH (with its own sitecustomize) must be kept and chained
	const std::wstring autre = TT::Joindre (racine, L"autre_chemin");
	TT::CreerDossier (autre);
	Ecrire (TT::Joindre (autre, L"sitecustomize.py"), "print('SUIVANT|ok', flush=True)\n");
	SetEnvironmentVariableW (L"PYTHONPATH", autre.c_str ());
	std::wstring pythonPath;
	Verifier ("PreparerEnvironnement", TT::PreparerEnvironnement (racine, pythonPath));
	Verifier ("PYTHONPATH = couche;ancien", pythonPath == TT::Joindre (racine, L"couche") + L";" + autre, TT::Utf8 (pythonPath));
	std::wstring encore;
	TT::PreparerEnvironnement (racine, encore);
	Verifier ("PreparerEnvironnement idempotent", encore == pythonPath, TT::Utf8 (encore));
	SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", L"tapir-banc-1");
	SetEnvironmentVariableW (L"PYTHONIOENCODING", L"utf-8");
	SetEnvironmentVariableW (L"BANC_ECHANGE", racine.c_str ());
	Langue (racine, "", true);
	Verifier ("LireReglages relit la langue d'origine + collecte", TT::LireReglages (racine).langue.empty () && TT::LireReglages (racine).collecte);

	// --- [2] main scenario: opened in its original language, then translated live ---------
	// The script uses its own dictionary (dictionnaires\banc_outil.json) plus the common one.
	//   1. launch through uv; the layer must be loaded by Python (COUCHE|active) and must
	//      chain the pre-existing sitecustomize (SUIVANT|ok); first state in French;
	//   2. switch to English while the window is open: every kind of displayed text is
	//      translated, the script still sees its French texts, data is left alone;
	//   3. the script changes its own texts while translated ("maj");
	//   4. collection: the untranslated text is noted in manquants\banc_outil.json;
	//   5. the dictionary is completed while the script runs: applied live;
	//   6. German, back to the original language, "fr" (= source, nothing to do), English
	//      again: several live switches in a row;
	//   7. clean exit, no Python traceback.
	std::printf ("\n[2] Script ouvert sans traduction puis traduit en direct\n");
	{
		// Step 1: launch. The first "uv run" may have to prepare its environment, hence 30 s.
		Outil o;
		Verifier ("lancement uv run --script", o.Lancer (script, racine));
		Verifier ("couche chargee par Python (sitecustomize)", o.AttendreLigne ("COUCHE|active", 30000));
		Verifier ("sitecustomize existant chaine", o.AttendreLigne ("SUIVANT|ok", 2000));
		Verifier ("premier etat recu", o.Attendre (Affiche ("titre", "Outil de démonstration"), 10000));
		Texte (o, "fermer", "Fermer", "Fermer");
		Texte (o, "statut", "Prêt", "Prêt");

		// Step 2: English output (collection still on). Each Texte checks the displayed
		// translation AND the original text still returned to the script.
		Langue (racine, "en", true);
		Verifier ("bascule en anglais en direct (< 5 s)", o.Attendre (Affiche ("titre", "Demo tool"), 5000));
		Texte (o, "titre", "Demo tool", "Outil de démonstration");
		Texte (o, "statut", "Ready", "Prêt");
		Texte (o, "fermer", "Close", "Fermer");
		Texte (o, "nom", "Name:", "Nom :");
		Texte (o, "murs", "Walls", "Murs");
		Texte (o, "cadre", "Options", "Options");
		// Multi-line text (newlines escaped by the script), "{n}" pattern, and a text found
		// in no dictionary (no engine running: it stays as is).
		Texte (o, "multi", "First line\\nSecond line", "Première ligne\\nDeuxième ligne");
		Texte (o, "modele", "3 walls created", "3 murs créés");
		Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");
		// Menus, notebook tab, Treeview heading, Canvas text.
		Texte (o, "menu_fichier", "File", "Fichier");
		Texte (o, "menu_ouvrir", "Open...", "Ouvrir...");
		Texte (o, "menu_quitter", "Quit", "Quitter");
		Texte (o, "onglet", "General", "Général");
		Texte (o, "entete", "Name", "Nom");
		Texte (o, "canevas", "Floor plan", "Plan");
		Texte (o, "ligne_arbre", "Murs", "Murs");			// data: never translated
		Texte (o, "combo", "Murs Dalles", "Murs");			// list values: never translated
		Texte (o, "saisie", "Murs", "Murs");				// user input: never translated
		// The script's own comparisons with its original texts still succeed.
		Texte (o, "logique", "ok");

		// Step 3: the script sets new texts itself while the interface is translated; they
		// must be translated too, and the script must still read its own French values.
		o.Commande ("maj");
		Verifier ("le script change son statut pendant la traduction", o.Attendre (Affiche ("statut", "Done"), 3000));
		Texte (o, "statut", "Done", "Terminé");
		Texte (o, "nom", "Value", "Valeur");
		Texte (o, "logique", "ok");

		// Step 4: with collection on, "Texte inconnu" must be written (with an empty
		// translation) into manquants\banc_outil.json; polled for up to about 4 s.
		const std::wstring manquants = TT::Joindre (TT::Joindre (racine, L"manquants"), L"banc_outil.json");
		bool note = false;
		for (int i = 0; i < 40 && !note; ++i) {
			std::string m;
			note = TT::LireFichier (manquants, m) && m.find ("\"Texte inconnu\": \"\"") != std::string::npos;
			if (!note)
				Sleep (100);
		}
		Verifier ("texte non traduit note dans manquants\\banc_outil.json", note);

		// Step 5: dictionary completed while the tool is running: the entry
		// "Texte inconnu" -> "Unknown text" is inserted after "Plan" in the script's
		// dictionary file, and the open window must pick it up by itself.
		const std::string cle = "\"Plan\": \"Floor plan\"";
		const size_t pos = dico.find (cle);
		Verifier ("dictionnaire du banc lisible", pos != std::string::npos);
		if (pos != std::string::npos) {
			dico.insert (pos + cle.size (), ",\n      \"Texte inconnu\": \"Unknown text\"");
			Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"banc_outil.json"), dico);
		}
		Verifier ("dictionnaire modifie applique en direct", o.Attendre (Affiche ("inconnu", "Unknown text"), 5000));
		Texte (o, "inconnu", "Unknown text", "Texte inconnu");

		// Step 6a: German (collection off). "Texte inconnu" has no German entry: original.
		Langue (racine, "de", false);
		Verifier ("bascule en allemand", o.Attendre (Affiche ("titre", "Demo-Werkzeug"), 5000));
		Texte (o, "fermer", "Schließen", "Fermer");
		Texte (o, "modele", "3 Wände erstellt", "3 murs créés");
		Texte (o, "murs", "Wände", "Murs");
		Texte (o, "statut", "Fertig", "Terminé");
		Texte (o, "nom", "Wert", "Valeur");					// common multilingual dictionary
		Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");

		// Step 6b: output language "" = no translation: every original text comes back.
		Langue (racine, "", false);
		Verifier ("retour a la langue d'origine", o.Attendre (Affiche ("titre", "Outil de démonstration"), 5000));
		Texte (o, "fermer", "Fermer", "Fermer");
		Texte (o, "statut", "Terminé", "Terminé");
		Texte (o, "modele", "3 murs créés", "3 murs créés");
		Texte (o, "menu_ouvrir", "Ouvrir...", "Ouvrir...");
		Texte (o, "canevas", "Plan", "Plan");

		// Step 6c: output = French, the script's own language: nothing must change (no
		// state change to wait for, hence a fixed delay before the check).
		Langue (racine, "fr", false);
		Sleep (1500);
		Texte (o, "titre", "Outil de démonstration");		// fr = source language: nothing to do

		// Step 6d: English again after all these switches.
		Langue (racine, "en", false);
		Verifier ("de nouveau en anglais apres plusieurs bascules", o.Attendre (Affiche ("titre", "Demo tool"), 5000));
		Texte (o, "menu_quitter", "Quit", "Quitter");
		Texte (o, "onglet", "General", "Général");
		Texte (o, "logique", "ok");

		// Step 7: the script exits by itself with code 0 and printed no Python traceback
		// (the layer must never break the script). On failure, show its output.
		DWORD code = 1;
		Verifier ("fermeture propre du script", o.Finir (code, 10000) && code == 0, "code " + std::to_string (code));
		bool trace = false;
		for (const std::string& l : o.Autres ())
			trace = trace || l.find ("Traceback") != std::string::npos;
		Verifier ("aucune erreur Python", !trace);
		if (g_echecs > 0)
			MontrerSortie (o);
	}

	// --- [3] script launched while a language is already chosen ----------------------------
	// The settings left by [2] say English: widgets must be translated as soon as they are
	// created (no switch involved), and the script logic must still work.
	std::printf ("\n[3] Script lance avec l'anglais deja choisi (traduction a la creation)\n");
	{
		Outil o;
		Verifier ("lancement", o.Lancer (script, racine));
		Verifier ("interface creee directement en anglais", o.Attendre (Affiche ("titre", "Demo tool"), 30000));
		Texte (o, "fermer", "Close", "Fermer");
		Texte (o, "statut", "Ready", "Prêt");
		Texte (o, "menu_ouvrir", "Open...", "Ouvrir...");
		Texte (o, "entete", "Name", "Nom");
		Texte (o, "canevas", "Floor plan", "Plan");
		Texte (o, "logique", "ok");
		DWORD code = 1;
		Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
	}

	// --- [3b] common dictionary only (script without a dictionary), several languages -------
	// TAPIR_TRANSLATOR_OUTIL overrides the script name used to find <script>.json: with a
	// name that has no dictionary, only _commun.json (fr, en, de, es, pt, it, hu) applies.
	//   1. Portuguese and Hungarian come from the common dictionary; the window title,
	//      specific to the script, stays in French;
	//   2. Polish is an ArchiCAD language without a column in the common dictionary: with
	//      the engine disabled the original text must stay;
	//   3. Spanish and Italian, then the script logic is still intact.
	std::printf ("\n[3b] Script sans dictionnaire : dictionnaire commun multilingue\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_sans_dictionnaire");
		Langue (racine, "pt", false);
		Outil o;
		Verifier ("lancement", o.Lancer (script, racine));
		Verifier ("portugais", o.Attendre (Affiche ("fermer", "Fechar"), 30000));
		Texte (o, "murs", "Paredes", "Murs");
		Texte (o, "nom", "Nome:", "Nom :");
		Texte (o, "statut", "Pronto", "Prêt");
		Texte (o, "menu_fichier", "Ficheiro", "Fichier");
		Texte (o, "titre", "Outil de démonstration");		// script-specific: not in the common dictionary
		Langue (racine, "hu", false);
		Verifier ("hongrois", o.Attendre (Affiche ("fermer", "Bezárás"), 5000));
		Texte (o, "murs", "Falak", "Murs");
		Texte (o, "menu_quitter", "Kilépés", "Quitter");
		Texte (o, "onglet", "Általános", "Général");
		// Polish: an ArchiCAD language without a column in the common dictionary, engine
		// disabled -> the original text stays (only the engine would translate it)
		Langue (racine, "pl", false);
		Verifier ("polonais sans colonne de dictionnaire : texte d'origine", o.Attendre (Affiche ("fermer", "Fermer"), 5000));
		Texte (o, "murs", "Murs", "Murs");
		Langue (racine, "es", false);
		Verifier ("espagnol", o.Attendre (Affiche ("fermer", "Cerrar"), 5000));
		Texte (o, "murs", "Muros", "Murs");
		Langue (racine, "it", false);
		Verifier ("italien", o.Attendre (Affiche ("fermer", "Chiudi"), 5000));
		Texte (o, "logique", "ok");
		DWORD code = 1;
		Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
	}

	// --- [3c] script written in English, source language not declared ---------------------
	// BANC_UI=en makes banc_outil.py build its interface in English; the script name has no
	// dictionary, so only the multilingual common dictionary is used.
	//   1. automatic input detection (input ""): English -> French (with the French space
	//      before ':'), English -> English (nothing changes), English -> German;
	//   2. input language chosen in the menu: en -> fr, en -> hu; a wrong input language
	//      (de) on an English script finds nothing to translate; input = output (en -> en)
	//      translates nothing;
	//   3. clean up the variables and go back to English output for the next scenarios.
	std::printf ("\n[3c] Script ecrit en anglais (langue non declaree)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_anglais");
		SetEnvironmentVariableW (L"BANC_UI", L"en");
		Langue (racine, "fr", false);
		Outil o;
		Verifier ("lancement", o.Lancer (script, racine));
		Verifier ("anglais -> francais", o.Attendre (Affiche ("fermer", "Fermer"), 30000));
		Texte (o, "fermer", "Fermer", "Close");
		Texte (o, "nom", "Nom :", "Name:");				// French typography (space before ':')
		Texte (o, "murs", "Murs", "Walls");
		Texte (o, "statut", "Prêt", "Ready");
		Texte (o, "menu_quitter", "Quitter", "Quit");
		Langue (racine, "en", false);
		Verifier ("anglais -> anglais : rien ne change", o.Attendre (Affiche ("fermer", "Close"), 5000));
		Texte (o, "murs", "Walls", "Walls");
		Langue (racine, "de", false);
		Verifier ("anglais -> allemand", o.Attendre (Affiche ("fermer", "Schließen"), 5000));
		Texte (o, "nom", "Name:", "Name:");
		Texte (o, "logique", "ok");

		// Step 2: input language chosen in the menu (4th argument of Langue)
		Langue (racine, "fr", false, "en");
		Verifier ("entree anglais -> sortie francais", o.Attendre (Affiche ("fermer", "Fermer"), 5000));
		Texte (o, "murs", "Murs", "Walls");
		Texte (o, "statut", "Prêt", "Ready");
		Langue (racine, "hu", false, "en");
		Verifier ("entree anglais -> sortie hongrois", o.Attendre (Affiche ("fermer", "Bezárás"), 5000));
		Texte (o, "murs", "Falak", "Walls");
		Langue (racine, "fr", false, "de");
		Verifier ("entree allemand sur un script anglais : rien a traduire", o.Attendre (Affiche ("fermer", "Close"), 5000));
		Texte (o, "murs", "Walls", "Walls");
		Langue (racine, "en", false, "en");
		Sleep (1500);
		Texte (o, "fermer", "Close", "Close");				// input = output
		Texte (o, "logique", "ok");
		DWORD code = 1;
		Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		// Step 3: back to the French script and English output.
		SetEnvironmentVariableW (L"BANC_UI", nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", nullptr);
		Langue (racine, "en", false);
	}

	// --- [3d] automatic offline translation (test engine) ----------------------------------
	// Texts found in no dictionary are sent by the layer to the local engine; here the engine
	// runs with --essai, whose "translation" is "[<lang>] <text>". The layer is told the
	// engine port through TAPIR_TRANSLATOR_MOTEUR_PORT (a port no real engine uses).
	//   1. engine enabled in the settings but not running: the dictionary still applies and
	//      the unknown text keeps its original value;
	//   2. the engine starts: the unknown sentence and the title get translated live (the
	//      layer retries by itself); the dictionary still wins, data stays untouched;
	//   3. the translation is stored in cache\essai\fr-en.json (French source recognised);
	//   4. another output language triggers a new automatic translation; disabling the
	//      engine brings the original text back;
	//   5. engine stopped, script relaunched: the translations come back from the cache
	//      alone; an already-English text ("Options") is never sent to the engine.
	std::printf ("\n[3d] Traduction automatique hors ligne (moteur d'essai)\n");
	{
		const int port = 8792;	// not the port of a real engine (8779)
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_MOTEUR_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_moteur");	// no dictionary of its own
		const std::wstring journal = TT::Joindre (racine, L"journal.txt");
		const std::wstring cacheEn = TT::Joindre (TT::Joindre (TT::Joindre (racine, L"cache"), L"essai"), L"fr-en.json");
		HANDLE moteur = nullptr;
		// English output, automatic input, engine enabled, provider "essai" (default).
		Langue (racine, "en", false, "", true);
		{
			// Step 1: no engine yet; wait 3 s to be sure nothing arrives for the unknown text.
			Outil o;
			Verifier ("lancement, moteur absent", o.Lancer (script, racine));
			Verifier ("dictionnaire applique sans moteur", o.Attendre (Affiche ("fermer", "Close"), 30000));
			Sleep (3000);
			Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");		// engine absent: original text
			Texte (o, "logique", "ok");

			// Step 2: start the test engine while the window is open.
			moteur = LancerMoteur (port, journal);
			Verifier ("moteur d'essai lance", moteur != nullptr);
			Verifier ("moteur revenu : phrase hors dictionnaire traduite en direct (< 25 s)",
					  o.Attendre (Affiche ("inconnu", "[en] Texte inconnu"), 25000));
			Texte (o, "inconnu", "[en] Texte inconnu", "Texte inconnu");
			// The title may arrive in the engine's next batch
			Verifier ("titre traduit automatiquement", o.Attendre (Affiche ("titre", "[en] Outil de démonstration"), 5000));
			Texte (o, "titre", "[en] Outil de démonstration", "Outil de démonstration");
			Texte (o, "fermer", "Close", "Fermer");						// the dictionary wins
			Texte (o, "chemin", "C:\\Projets\\plan.pln", "C:\\Projets\\plan.pln");	// data: never sent
			Texte (o, "code", "MUR-01", "MUR-01");
			Texte (o, "ligne_arbre", "Murs", "Murs");
			Texte (o, "logique", "ok");

			// Step 3: the cache file of the "essai" provider, French -> English, must hold the
			// translation (polled for up to about 3 s).
			bool enCache = false;
			for (int i = 0; i < 30 && !enCache; ++i) {
				std::string c;
				enCache = TT::LireFichier (cacheEn, c) && c.find ("\"Texte inconnu\": \"[en] Texte inconnu\"") != std::string::npos;
				if (!enCache)
					Sleep (100);
			}
			Verifier ("traduction gardee dans cache\\fr-en.json (langue fr reconnue)", enCache);

			// Step 4: Hungarian output -> new request to the engine ("[hu] ..."), the
			// dictionary still gives "Bezárás"; then engine disabled -> original text.
			Langue (racine, "hu", false, "", true);
			Verifier ("autre langue de sortie : nouvelle traduction automatique", o.Attendre (Affiche ("inconnu", "[hu] Texte inconnu"), 10000));
			Texte (o, "fermer", "Bezárás", "Fermer");

			Langue (racine, "en", false, "", false);
			Verifier ("moteur desactive : texte d'origine", o.Attendre (Affiche ("inconnu", "Texte inconnu"), 5000));
			Texte (o, "fermer", "Close", "Fermer");
			DWORD code = 1;
			Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		}
		ArreterMoteur (moteur);

		// Step 5: engine enabled in the settings but stopped: a new run must find its
		// translations in the cache.
		Langue (racine, "en", false, "", true);
		{
			Outil o;
			Verifier ("relance, moteur arrete", o.Lancer (script, racine));
			Verifier ("traduction reprise du cache, sans moteur", o.Attendre (Affiche ("inconnu", "[en] Texte inconnu"), 30000));
			// The title is created before the script's language is recognised: it follows shortly
			Verifier ("titre repris du cache", o.Attendre (Affiche ("titre", "[en] Outil de démonstration"), 5000));
			Texte (o, "titre", "[en] Outil de démonstration", "Outil de démonstration");
			Texte (o, "cadre", "Options", "Options");			// already English: never sent to the engine
			DWORD code = 1;
			Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		}
		// Clean up: default script name, engine disabled, English output.
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", nullptr);
		Langue (racine, "en", false);
	}

	// --- [3e] online translators (fake server with the DeepL, Microsoft, Google, MyMemory
	//     and LibreTranslate formats), through the real engine ------------------------------
	// Chain under test: layer -> engine (--cles cles.json) -> fake service -> engine ->
	// cache -> layer. The TAPIR_TRANSLATOR_URL_<SERVICE> variables send every provider to
	// the fake server instead of the real one.
	//   1. cles.json: the add-on's template is created, then replaced by keys for every
	//      provider except Google;
	//   2. for DeepL, Microsoft, MyMemory and LibreTranslate in turn: the unknown sentence
	//      comes back in that provider's format with that provider's prefix, while the
	//      dictionary still translates "Fermer" first;
	//   3. Google without a key: the original text stays; the key is then added to
	//      cles.json while everything runs: the engine re-reads it and the next retry
	//      succeeds;
	//   4. back to DeepL: its own cache gives its translation back;
	//   5. after the run: no dictionary text and no data (path, code) ever reached the
	//      service, and the missing Google key is explained in journal.txt.
	std::printf ("\n[3e] Traducteurs en ligne (faux serveur, formats reels des 5 services)\n");
	{
		// Fake services on 8798, engine on 8796 (both distinct from the other scenarios).
		const int portServices = 8798;
		const int port = 8796;
		ServeurFactice faux;
		Verifier ("faux serveur de traduction demarre", faux.Demarrer (portServices));
		const std::wstring base = L"http://127.0.0.1:" + std::to_wstring (portServices);
		for (const wchar_t* nom : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + nom).c_str (), base.c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_MOTEUR_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_en_ligne");
		// Step 1: cles.json template written by the add-on code, then overwritten below.
		const std::wstring cles = TT::CheminCles (racine);
		std::wstring erreur;
		Verifier ("modele de cles.json cree", TT::CreerModeleCles (racine, erreur) && TT::FichierExiste (cles));
		// The Google key is missing at first (the keys match what ServeurFactice expects)
		Ecrire (cles, "{\"deepl\": {\"cle\": \"cle-deepl:fx\"}, \"microsoft\": {\"cle\": \"cle-ms\", \"region\": \"westeurope\"},"
					  " \"google\": {\"cle\": \"\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"cle\": \"\"}}");
		HANDLE moteur = LancerMoteur (port, TT::Joindre (racine, L"journal.txt"), L"--cles \"" + cles + L"\"");
		Verifier ("moteur lance avec cles.json", moteur != nullptr);
		{
			// Input language fixed to French, engine on, provider DeepL to start with.
			Outil o;
			Langue (racine, "en", false, "fr", true, "deepl");
			Verifier ("lancement", o.Lancer (script, racine));
			// Step 2: expected answer of each provider for "Texte inconnu" (DeepL asks for
			// EN-GB as target, the others for "en").
			const struct { const char* code; const char* attendu; } services[] = {
				{ "deepl", "DL:EN-GB:Texte inconnu" },
				{ "microsoft", "MS:en:Texte inconnu" },
				{ "mymemory", "MM:en:Texte inconnu" },
				{ "libretranslate", "LT:en:Texte inconnu" },
			};
			for (const auto& s : services) {
				Langue (racine, "en", false, "fr", true, s.code);
				Verifier (std::string (s.code) + " : phrase traduite (format de requete et de reponse du service)",
						  o.Attendre (Affiche ("inconnu", s.attendu), 20000), o.Lire ("inconnu").affiche);
				Texte (o, "fermer", "Close", "Fermer");			// dictionary first
			}
			Texte (o, "logique", "ok");

			// Step 3: Google without a key -> original kept (checked again after 2 s, to be
			// sure no translation arrives), then the key is written into cles.json.
			Langue (racine, "en", false, "fr", true, "google");
			Verifier ("google sans cle : texte d'origine garde", o.Attendre (Affiche ("inconnu", "Texte inconnu"), 10000));
			Sleep (2000);
			Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");
			Ecrire (cles, "{\"deepl\": {\"cle\": \"cle-deepl:fx\"}, \"microsoft\": {\"cle\": \"cle-ms\", \"region\": \"westeurope\"},"
						  " \"google\": {\"cle\": \"cle-google\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"cle\": \"\"}}");
			Verifier ("cle Google ajoutee a chaud : traduit au nouvel essai (< 30 s)",
					  o.Attendre (Affiche ("inconnu", "GG:en:Texte inconnu"), 30000), o.Lire ("inconnu").affiche);

			// Step 4: each provider has its own cache folder: DeepL's answer comes back.
			Langue (racine, "en", false, "fr", true, "deepl");
			Verifier ("retour a DeepL : son cache redonne sa traduction", o.Attendre (Affiche ("inconnu", "DL:EN-GB:Texte inconnu"), 10000));
			Texte (o, "logique", "ok");
			DWORD code = 1;
			Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		}
		// Step 5: privacy checks on everything the fake service received, then the log
		// line about the missing Google key; finally clean up the engine, the server and
		// the variables.
		Verifier ("aucun texte du dictionnaire envoye en ligne (\"Fermer\")", !faux.ARecu ("Fermer"));
		Verifier ("aucune donnee envoyee en ligne (chemin, code)", !faux.ARecu ("C:\\Projets\\plan.pln") && !faux.ARecu ("MUR-01"));
		const std::vector<std::string> journal = TT::DernieresLignes (TT::Joindre (racine, L"journal.txt"), 300);
		bool signale = false;
		for (const std::string& l : journal)
			signale = signale || l.find ("cle Google absente") != std::string::npos;
		Verifier ("journal : la cle Google manquante est expliquee", signale);
		ArreterMoteur (moteur);
		faux.Arreter ();
		for (const wchar_t* nom : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + nom).c_str (), nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", nullptr);
		Langue (racine, "en", false);
	}

	// --- [3f] term corrections and MyMemory quota --------------------------------------------
	// The English interface (BANC_UI=en) is translated into French by the fake
	// LibreTranslate, which turns "All stories" into "Toutes les histoires".
	//   1. the corrections.json pre-filled by the add-on must fix it into "Tous les étages";
	//   2. corrections.json edited while the window is open (rules "histoire" -> "niveau",
	//      for originals containing "story", whose -s plural follows automatically, and the
	//      whole phrase "Toutes les histoires" -> "Tous les niveaux"): applied live to the
	//      cached translation; with no correction, the raw service translation shows;
	//      corrections never touch dictionary translations;
	//   3. MyMemory anonymous quota reached: the original text stays (dictionary still
	//      applied) and the engine logs the quota (the e-mail window is disabled here by
	//      --sans-dialogue);
	//   4. what the "Enregistrer" button of that window does: the engine writes the e-mail
	//      into cles.json, keeping the other entries; MyMemory then translates again (the
	//      fake server only blocks requests without an e-mail).
	std::printf ("\n[3f] Corrections de termes (story -> étage) et quota MyMemory\n");
	{
		// Fake services on 8799, engine on 8795; only MyMemory and LibreTranslate redirected.
		const int portServices = 8799;
		const int port = 8795;
		ServeurFactice faux;
		Verifier ("faux serveur de traduction demarre", faux.Demarrer (portServices));
		const std::wstring base = L"http://127.0.0.1:" + std::to_wstring (portServices);
		for (const wchar_t* nom : { L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + nom).c_str (), base.c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_MOTEUR_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_corrections");
		SetEnvironmentVariableW (L"BANC_UI", L"en");
		// cles.json with an extra "_explication" entry and a DeepL key: both must survive
		// when the engine writes the MyMemory e-mail (step 4). No e-mail at first.
		const std::wstring cles = TT::CheminCles (racine);
		Ecrire (cles, "{\"_explication\": \"test\", \"deepl\": {\"cle\": \"cle-deepl:fx\"}, \"mymemory\": {\"email\": \"\"}}");
		HANDLE moteur = LancerMoteur (port, TT::Joindre (racine, L"journal.txt"), L"--sans-dialogue --cles \"" + cles + L"\"");
		Verifier ("moteur lance", moteur != nullptr);
		Verifier ("corrections.json pre-rempli par l'add-on", TT::FichierExiste (TT::CheminCorrections (racine)));
		{
			// Step 1: English -> French through LibreTranslate, default corrections.
			Outil o;
			Langue (racine, "fr", false, "en", true, "libretranslate");
			Verifier ("lancement (script en anglais)", o.Lancer (script, racine));
			Verifier ("\"All stories\" -> \"Toutes les histoires\" corrige en \"Tous les étages\"",
					  o.Attendre (Affiche ("etages", "Tous les étages"), 20000), o.Lire ("etages").affiche);
			Texte (o, "etages", "Tous les étages", "All stories");

			// Step 2: corrections modified while the window is open
			Ecrire (TT::CheminCorrections (racine),
					"{\"fr\": [{\"source\": \"story\", \"mauvais\": \"histoire\", \"correct\": \"niveau\"},"
					" {\"mauvais\": \"Toutes les histoires\", \"correct\": \"Tous les niveaux\"}]}");
			Verifier ("corrections.json modifie : applique en direct, cache compris (pluriel -s automatique)",
					  o.Attendre (Affiche ("etages", "Tous les niveaux"), 5000), o.Lire ("etages").affiche);
			Ecrire (TT::CheminCorrections (racine), "{\"fr\": []}");
			Verifier ("sans correction : traduction brute du service", o.Attendre (Affiche ("etages", "Toutes les histoires"), 5000),
					  o.Lire ("etages").affiche);
			Verifier ("la correction ne touche pas le dictionnaire", o.Lire ("fermer").affiche == "Fermer");

			// Step 3: anonymous MyMemory quota reached (English -> German). The dictionary
			// still gives "Schließen"; 2.5 s later the unknown text must still be original,
			// and journal.txt must report the anonymous quota (polled for up to about 4 s).
			faux.quotaAnonyme = true;
			Langue (racine, "de", false, "en", true, "mymemory");
			Verifier ("quota MyMemory : le texte d'origine reste (dictionnaire applique)", o.Attendre (Affiche ("fermer", "Schließen"), 10000));
			Sleep (2500);
			Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");
			bool quota = false;
			for (int i = 0; i < 20 && !quota; ++i) {
				for (const std::string& l : TT::DernieresLignes (TT::Joindre (racine, L"journal.txt"), 60))
					quota = quota || l.find ("quota MyMemory atteint (anonyme") != std::string::npos;
				if (!quota)
					Sleep (200);
			}
			Verifier ("journal : quota MyMemory anonyme detecte (fenetre e-mail demandee)", quota);

			// Step 4: what the window's "Enregistrer" button does: the e-mail is written into
			// cles.json (engine run once with --enregistrer-email, exit code 0 = written);
			// the running engine re-reads the keys and MyMemory answers again.
			wchar_t exe[MAX_PATH] = {};
			GetModuleFileNameW (nullptr, exe, MAX_PATH);
			std::wstring chemin = exe;
			chemin = chemin.substr (0, chemin.find_last_of (L'\\') + 1) + L"TapirTranslatorMoteur.exe";
			std::wstring ligne = L"\"" + chemin + L"\" --enregistrer-email \"" + cles + L"\" moi@exemple.fr";
			std::vector<wchar_t> tampon (ligne.begin (), ligne.end ());
			tampon.push_back (L'\0');
			STARTUPINFOW si = {};
			si.cb = sizeof (si);
			PROCESS_INFORMATION pi = {};
			DWORD code = 1;
			if (CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
				WaitForSingleObject (pi.hProcess, 10000);
				GetExitCodeProcess (pi.hProcess, &code);
				CloseHandle (pi.hThread);
				CloseHandle (pi.hProcess);
			}
			std::string c;
			TT::LireFichier (cles, c);
			Verifier ("e-mail enregistre dans cles.json", code == 0 && c.find ("\"email\": \"moi@exemple.fr\"") != std::string::npos);
			Verifier ("les autres cles de cles.json sont gardees", c.find ("\"cle\": \"cle-deepl:fx\"") != std::string::npos &&
																	c.find ("\"_explication\": \"test\"") != std::string::npos);
			Verifier ("avec l'e-mail : MyMemory traduit de nouveau (< 30 s)", o.Attendre (Affiche ("inconnu", "MM:de:Texte inconnu"), 30000),
					  o.Lire ("inconnu").affiche);
			Texte (o, "logique", "ok");
			DWORD fin = 1;
			Verifier ("fermeture propre", o.Finir (fin, 10000) && fin == 0);
		}
		// Clean up: engine, fake server, redirections and script variables.
		ArreterMoteur (moteur);
		faux.Arreter ();
		for (const wchar_t* nom : { L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + nom).c_str (), nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", nullptr);
		SetEnvironmentVariableW (L"BANC_UI", nullptr);
		Langue (racine, "en", false);
	}

	// --- [4] outside Tapir: the layer must do nothing ---------------------------------------
	// The layer is still on the PYTHONPATH (so it is imported by every Python started from
	// ArchiCAD's environment), but without TAPIR_SCRIPT_ID it must not install itself
	// (COUCHE|absente) and the interface must stay untranslated although English is the
	// chosen output. TAPIR_SCRIPT_ID is restored afterwards.
	std::printf ("\n[4] Python lance hors Tapir (pas de TAPIR_SCRIPT_ID)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", nullptr);
		Outil o;
		Verifier ("lancement", o.Lancer (script, racine));
		Verifier ("couche inactive", o.AttendreLigne ("COUCHE|absente", 30000));
		Verifier ("interface non traduite", o.Attendre (Affiche ("titre", "Outil de démonstration"), 10000));
		DWORD code = 1;
		Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		SetEnvironmentVariableW (L"TAPIR_SCRIPT_ID", L"tapir-banc-1");
	}

	// --- [5] layer log -----------------------------------------------------------------------
	// The layer never lets an error reach the script: it writes it to journal.txt instead.
	// After all the runs above, the last 200 log lines must exist and contain none of the
	// (French) words of the layer's internal error messages: "interception", "apres call",
	// "surveillance", "impossible", "illisible". Offending lines are printed.
	std::printf ("\n[5] Journal de la couche\n");
	{
		const std::vector<std::string> lignes = TT::DernieresLignes (TT::Joindre (racine, L"journal.txt"), 200);
		bool erreur = false;
		for (const std::string& l : lignes) {
			if (l.find ("interception") != std::string::npos || l.find ("apres call") != std::string::npos ||
				l.find ("surveillance") != std::string::npos || l.find ("impossible") != std::string::npos ||
				l.find ("illisible") != std::string::npos) {
				erreur = true;
				std::printf ("         | %s\n", l.c_str ());
			}
		}
		Verifier ("journal ecrit", !lignes.empty ());
		Verifier ("aucune erreur interne de la couche", !erreur);
	}

	// --- [6] language of the tool itself (menus, messages, window) ---------------------------
	//   1. the texts table (Src/TT_Textes.cpp) has no empty cell;
	//   2. ArchiCAD language codes map to the right interface language, unknown -> English
	//      (index 0 = French, index 1 = English);
	//   3. formatted texts (TxtF with named values) and a text of the quota window;
	//   4. the "interface" setting is written and read back (a code, or "" = like ArchiCAD);
	//   5. the menus generated at build time (RFIX\Tapir_TranslatorMenus.grc, next to the
	//      Tests folder) contain, for each of the 20 languages, its 5 menu STR# resources
	//      and their status-bar help resources (+ DecalageAides), and the Japanese text.
	std::printf ("\n[6] Langue de Tapir Translator (20 langues, celle d'ArchiCAD par defaut)\n");
	{
		// Steps 1 to 3
		Verifier ("table des textes complete (51 textes x 20 langues)", TT::TextesManquants () == 0,
				  std::to_string (TT::TextesManquants ()) + " case(s) vide(s)");
		Verifier ("ArchiCAD FRA -> francais", TT::IndiceInterface (TT::LangueDepuisArchicad ("FRA")) == 0);
		Verifier ("ArchiCAD GER / AUT -> allemand", TT::LangueDepuisArchicad ("GER") == "de" && TT::LangueDepuisArchicad ("AUT") == "de");
		Verifier ("ArchiCAD INT / USA / UKI -> anglais", TT::LangueDepuisArchicad ("INT") == "en" && TT::LangueDepuisArchicad ("UKI") == "en");
		Verifier ("ArchiCAD BRA -> portugais, TAI -> chinois, JPN -> japonais",
				  TT::LangueDepuisArchicad ("BRA") == "pt" && TT::LangueDepuisArchicad ("TAI") == "zh" && TT::LangueDepuisArchicad ("JPN") == "ja");
		Verifier ("version inconnue -> anglais", TT::LangueDepuisArchicad ("XYZ") == "en" && TT::IndiceInterface ("xx") == 1);
		Verifier ("textes avec valeurs (TxtF)", TT::TxtF (TT::R_DEMARRAGE, 2, { { "v", "9" }, { "l", "fr -> de" } }) ==
												"Tapir Translator 9 aktiv. Sprachen: fr -> de.");
		Verifier ("fenetre du quota en hongrois", std::string (TT::Txt (TT::D_ENREGISTRER, TT::IndiceInterface ("hu"))) == "Mentés");

		// Step 4
		TT::Reglages r;
		r.interface = "ja";
		std::wstring erreur;
		TT::EcrireReglages (racine, r, erreur);
		Verifier ("reglages : langue de l'outil relue (ja)", TT::LireReglages (racine).interface == "ja");
		r.interface = "";
		TT::EcrireReglages (racine, r, erreur);
		Verifier ("reglages : langue de l'outil = comme ArchiCAD (vide)", TT::LireReglages (racine).interface.empty ());

		// Step 5: for language l, menu m has the id BaseMenus (l) + m, its help + DecalageAides.
		std::string grc;
		const std::wstring menus = TT::Joindre (TT::Joindre (DossierTests ().substr (0, DossierTests ().find_last_of (L'\\')), L"RFIX"),
												L"Tapir_TranslatorMenus.grc");
		bool tous = TT::LireFichier (menus, grc);
		for (int l = 0; l < TT::NombreLanguesInterface && tous; ++l) {
			for (int m = 0; m < 5 && tous; ++m) {
				const int id = TT::BaseMenus (l) + m;
				tous = grc.find ("'STR#' " + std::to_string (id) + " ") != std::string::npos &&
					   grc.find ("'STR#' " + std::to_string (id + TT::DecalageAides) + " ") != std::string::npos;
			}
		}
		Verifier ("menus generes pour les 20 langues (5 menus + aides chacune)", tous);

		// Step 6: every resource ID read by the ArchiCAD API (a short) must stay below 32767; above,
		// it wraps to a negative number and ArchiCAD unloads the add-on (bug of v0.7.0).
		bool dansShort = true;
		for (int l = 0; l < TT::NombreLanguesInterface; ++l) {
			for (int m = 0; m < 5; ++m)
				dansShort = dansShort && TT::BaseMenus (l) + m + TT::DecalageAides <= 32767 && TT::BaseMenus (l) + m > 32000;
		}
		Verifier ("identifiants de menus et d'aides entre 32001 et 32767 (short de l'API ArchiCAD)", dansShort);
		bool sansIdTropGrand = true;
		for (size_t p = grc.find ("'STR#' "); p != std::string::npos; p = grc.find ("'STR#' ", p + 7))
			sansIdTropGrand = sansIdTropGrand && std::atoi (grc.c_str () + p + 7) <= 32767;
		Verifier ("aucune ressource generee au-dessus de 32767", sansIdTropGrand);
		Verifier ("menus generes : japonais present", grc.find (TT::Txt (TT::M_TRADUCTEUR, TT::IndiceInterface ("ja"))) != std::string::npos);
	}

	// Summary; the temporary folder is kept when something failed, for examination.
	std::printf ("\n%d OK, %d ECHEC(S)\n", g_ok, g_echecs);
	if (g_echecs == 0)
		SupprimerArbre (racine);
	else
		std::printf ("dossier garde pour examen : %s\n", TT::Utf8 (racine).c_str ());
	return g_echecs == 0 ? 0 : 1;
}
