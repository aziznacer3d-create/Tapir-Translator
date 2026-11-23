// *****************************************************************************
// Tapir Translator - translation engine (TapirTranslatorMoteur.exe)
//
//   Role
//   ----
//   A small local HTTP server, bound to 127.0.0.1 only (default port 8779),
//   started by the add-on when ArchiCAD starts (and restarted by it if it
//   stops). It serves every Tapir script at once: the Python translation layer
//   (couche/sitecustomize.py) sends it, in the background, the displayed texts
//   that the dictionaries do not know. The engine relays them to the online
//   translator chosen by the user (DeepL, Microsoft, Google, MyMemory,
//   LibreTranslate - see TraducteursEnLigne.cpp), or to the test translator /
//   the optional offline M2M100 model.
//
//   Running in its own process keeps network calls (and, with the offline
//   model, a possible crash) away from ArchiCAD and from the scripts.
//
//   Protocol (one request per connection, "Connection: close", JSON bodies)
//   ------------------------------------------------------------------------
//   GET  /etat      -> {"version": "...", "fournisseurs": [...], "langues": [...]}
//                      (engine version, available providers, supported languages)
//   POST /traduire  {"fournisseur": "deepl", "source": "fr" | "", "cible": "en", "textes": [...]}
//                   -> 200 {"traductions": [...]}   (same order, same count as "textes")
//                   -> 400 bad request (unreadable JSON, unsupported language, unknown provider)
//                   -> 503 the provider failed (network, quota, key...): the layer keeps
//                      the original text and retries later
//                   An empty "source" means "detect automatically"; "fournisseur"
//                   defaults to "mymemory".
//   POST /arreter   -> {"arret": true}, then the server stops
//
//   Command-line options
//   --------------------
//   --port N               listening port (8779)
//   --cles cles.json       providers' keys file, re-read on every request
//   --essai                enable the "essai" test translator (test bench)
//   --modele <folder>      load the offline M2M100 model (build option TT_MODELE_LOCAL)
//   --sans-dialogue        never show the MyMemory quota window (test bench)
//   --interface <code>     language of the engine's own UI (TT_Textes.hpp)
//   --parent PID           exit when that process (ArchiCAD) exits
//   --journal <file>       append log lines to this file (journal.txt)
//   --montrer-quota 0|1    development: only show the quota window and exit
//   --enregistrer-email <cles.json> <email>   test bench: write the e-mail and exit
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
#include "Traducteur.hpp"
#include "Dialogue.hpp"
#include "TT_Textes.hpp"

#include <atomic>

#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#pragma comment (lib, "Ws2_32.lib")

namespace {

// Engine version, reported by GET /etat.
const char* const kVersion = "0.7.0";
// Path of the log file (--journal); empty = no logging.
std::string g_journal;

// Appends one timestamped line to the log file:
//   "YYYY-MM-DD HH:MM:SS [moteur <pid>] <ligne>"
// The file is opened and closed for each line so that the add-on and the
// Python layer, which write to the same journal.txt, can interleave safely.
// Does nothing when no log file was given or it cannot be opened.
void Journal (const std::string& ligne)
{
	if (g_journal.empty ())
		return;
	FILE* f = nullptr;
	if (fopen_s (&f, g_journal.c_str (), "ab") != 0 || f == nullptr)
		return;
	// Local date and time of the event
	char date[32];
	std::time_t t = std::time (nullptr);
	std::tm tm = {};
	localtime_s (&tm, &t);
	std::strftime (date, sizeof (date), "%Y-%m-%d %H:%M:%S", &tm);
	std::fprintf (f, "%s [moteur %lu] %s\n", date, static_cast<unsigned long> (GetCurrentProcessId ()), ligne.c_str ());
	std::fclose (f);
}

// Sends the whole buffer on the socket. send() may write only part of the
// data, so it is called again until everything is sent.
// Returns false as soon as send() fails or the connection is closed.
bool EnvoyerTout (SOCKET s, const std::string& donnees)
{
	size_t envoye = 0;
	while (envoye < donnees.size ()) {
		const int n = send (s, donnees.data () + envoye, static_cast<int> (donnees.size () - envoye), 0);
		if (n <= 0)
			return false;
		envoye += static_cast<size_t> (n);
	}
	return true;
}

// Writes a complete HTTP/1.1 response with a JSON body.
//   code : HTTP status (200, 400, 404; any other code is labelled
//          "Internal Server Error", which includes the 503 used for provider failures)
//   json : response body, already serialised
// The connection is always closed afterwards ("Connection: close").
void Repondre (SOCKET s, int code, const std::string& json)
{
	// Step 1: status line and headers
	const char* statut = code == 200 ? "OK" : (code == 400 ? "Bad Request" : (code == 404 ? "Not Found" : "Internal Server Error"));
	std::string r = "HTTP/1.1 " + std::to_string (code) + " " + statut + "\r\n";
	r += "Content-Type: application/json; charset=utf-8\r\n";
	r += "Content-Length: " + std::to_string (json.size ()) + "\r\n";
	r += "Connection: close\r\n\r\n";
	// Step 2: body, then send everything (errors are ignored: the client is gone)
	r += json;
	EnvoyerTout (s, r);
}

// Reads one complete HTTP request: headers, then the body according to
// Content-Length.
//   methode, chemin : outputs, e.g. "POST" and "/traduire"
//   corps           : output, the request body (exactly Content-Length bytes)
// Returns false if the connection fails or times out, if the request line is
// malformed, if the headers exceed 64 KB or the body exceeds 4 MB.
bool LireRequete (SOCKET s, std::string& methode, std::string& chemin, std::string& corps)
{
	// Step 1: receive until the blank line that ends the headers ("\r\n\r\n")
	std::string tampon;
	char morceau[8192];
	size_t finEntetes = std::string::npos;
	while (finEntetes == std::string::npos) {
		const int n = recv (s, morceau, sizeof (morceau), 0);
		if (n <= 0)
			return false;
		tampon.append (morceau, static_cast<size_t> (n));
		finEntetes = tampon.find ("\r\n\r\n");
		// Guard against endless headers
		if (tampon.size () > 65536 && finEntetes == std::string::npos)
			return false;
	}
	// Step 2: request line "METHOD /path HTTP/1.1" -> method and path
	const std::string entetes = tampon.substr (0, finEntetes);
	const size_t sp1 = entetes.find (' ');
	const size_t sp2 = sp1 == std::string::npos ? sp1 : entetes.find (' ', sp1 + 1);
	if (sp2 == std::string::npos)
		return false;
	methode = entetes.substr (0, sp1);
	chemin = entetes.substr (sp1 + 1, sp2 - sp1 - 1);

	// Step 3: Content-Length, searched case-insensitively in a lowercase copy
	// of the headers (15 = length of "content-length:")
	size_t longueur = 0;
	std::string minuscules = entetes;
	for (char& c : minuscules)
		c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	const size_t pos = minuscules.find ("content-length:");
	if (pos != std::string::npos)
		longueur = static_cast<size_t> (std::strtoul (minuscules.c_str () + pos + 15, nullptr, 10));
	if (longueur > 4 * 1024 * 1024)
		return false;
	// Step 4: the body starts after the blank line; part of it may already be
	// in the buffer, receive the rest, then drop any extra bytes
	corps = tampon.substr (finEntetes + 4);
	while (corps.size () < longueur) {
		const int n = recv (s, morceau, sizeof (morceau), 0);
		if (n <= 0)
			return false;
		corps.append (morceau, static_cast<size_t> (n));
	}
	corps.resize (longueur);
	return true;
}

// Server state shared by all requests, filled from the command line in main().
struct Contexte {
	std::unique_ptr<Traducteur> essai;	// --essai (test bench): "[target] text" translator
	std::unique_ptr<Traducteur> local;	// --modele (build option TT_MODELE_LOCAL): offline M2M100
	std::string cles;					// --cles: path of cles.json (online providers' keys)
	bool sansDialogue = false;			// --sans-dialogue (test bench): no "MyMemory quota" window
	int langueInterface = 1;			// --interface fr / en / ...: UI language index (TT_Textes.hpp)
	std::atomic<bool> dialogueOuvert { false };				// the quota window is currently shown
	std::atomic<unsigned long long> plusTardJusqua { 0 };	// GetTickCount64() before which the quota window must not reappear
};

// Reads and parses cles.json.
//   chemin : path of the file (may be empty)
// Returns the parsed JSON value, or an empty (null) value if the path is
// empty, the file cannot be opened or its JSON is invalid (logged). The file
// is read on every request so that a key added by the user works at once.
Json::Valeur LireCles (const std::string& chemin)
{
	Json::Valeur v;
	if (chemin.empty ())
		return v;
	// Step 1: read the whole file in binary mode
	FILE* f = nullptr;
	if (fopen_s (&f, chemin.c_str (), "rb") != 0 || f == nullptr)
		return v;
	std::string contenu;
	char tampon[4096];
	size_t n = 0;
	while ((n = std::fread (tampon, 1, sizeof (tampon), f)) > 0)
		contenu.append (tampon, n);
	std::fclose (f);
	// Step 2: drop the UTF-8 byte order mark that Notepad may add
	if (contenu.size () >= 3 && contenu.compare (0, 3, "\xEF\xBB\xBF") == 0)
		contenu.erase (0, 3);
	// Step 3: parse
	if (!Json::Lecteur (contenu).Lire (v))
		Journal ("cles.json illisible (JSON invalide) : " + chemin);
	return v;
}

// Writes the MyMemory e-mail address into cles.json ("mymemory": {"email": ...})
// without touching the other keys.
//   chemin : path of cles.json
//   email  : address to store
// Returns true if the file was written. The file is written to "<chemin>.tmp"
// first, then moved over the original, so a failure never leaves a truncated
// cles.json. Note: the file is rewritten by Json::Ecrire (keys sorted).
bool EnregistrerEmail (const std::string& chemin, const std::string& email)
{
	if (chemin.empty ())
		return false;
	// Step 1: current content; start from an empty object if it is not an object
	Json::Valeur v = LireCles (chemin);
	if (v.genre != Json::Valeur::Objet) {
		v = Json::Valeur ();
		v.genre = Json::Valeur::Objet;
	}
	// Step 2: the "mymemory" section (created or reset if missing / not an object)
	Json::Valeur& m = v.objet["mymemory"];
	if (m.genre != Json::Valeur::Objet) {
		m = Json::Valeur ();
		m.genre = Json::Valeur::Objet;
	}
	// Step 3: set "email"
	Json::Valeur e;
	e.genre = Json::Valeur::Chaine;
	e.chaine = email;
	m.objet["email"] = e;
	// Step 4: serialise, write the temporary file, then replace cles.json
	std::string texte;
	Json::Ecrire (v, texte);
	texte += "\n";
	const std::string tmp = chemin + ".tmp";
	FILE* f = nullptr;
	if (fopen_s (&f, tmp.c_str (), "wb") != 0 || f == nullptr)
		return false;
	const bool ecrit = std::fwrite (texte.data (), 1, texte.size (), f) == texte.size ();
	std::fclose (f);
	return ecrit && MoveFileExA (tmp.c_str (), chemin.c_str (), MOVEFILE_REPLACE_EXISTING);
}

// Called when MyMemory answered "MYMEMORY WARNING" or HTTP 429 (daily quota
// reached). Opens the window that asks for an e-mail address (which raises the
// free quota) in its own thread, so the server keeps answering requests.
//   c : server context (keys path, UI language, window state)
// The window is never shown twice at the same time, and not again for one
// hour after the user answered "Later" (or closed it). With --sans-dialogue the
// event is only logged.
void SignalerQuotaMyMemory (Contexte& c)
{
	// Step 1: window already open, or "Later" period still running -> nothing to do
	const unsigned long long maintenant = GetTickCount64 ();
	if (c.dialogueOuvert || maintenant < c.plusTardJusqua)
		return;
	// Step 2: is an e-mail already configured? (then the 50 000 characters/day
	// quota is reached too and the window is only informative)
	const Json::Valeur cles = LireCles (c.cles);
	const Json::Valeur* m = cles.Champ ("mymemory");
	std::string email = m != nullptr ? m->Texte ("email") : std::string ();
	const bool avecEmail = !email.empty ();
	Journal (std::string ("quota MyMemory atteint ") + (avecEmail ? "(meme avec l'e-mail : environ 50 000 caracteres/jour)"
																   : "(anonyme : environ 5 000 caracteres/jour)"));
	// Step 3: test bench mode: no window, but the same one-hour silence
	if (c.sansDialogue) {
		Journal ("fenetre de saisie de l'e-mail desactivee (--sans-dialogue)");
		c.plusTardJusqua = maintenant + 3600000ull;
		return;
	}
	// Step 4: show the window in a detached thread (the context outlives it:
	// it lives in main() until the process exits)
	c.dialogueOuvert = true;
	std::thread ([&c, email, avecEmail] () mutable {
		const ReponseQuota r = DemanderEmailMyMemory (email, avecEmail, c.langueInterface);
		// Step 5: address entered -> store it in cles.json (used from the next
		// request on); otherwise do not ask again for one hour
		if (r == ReponseQuota::Enregistree) {
			if (EnregistrerEmail (c.cles, email))
				Journal ("e-mail MyMemory enregistre dans cles.json : quota porte a environ 50 000 caracteres/jour");
			else
				Journal ("ERREUR enregistrement de l'e-mail dans " + c.cles);
		} else {
			c.plusTardJusqua = GetTickCount64 () + 3600000ull;
			Journal ("fenetre du quota MyMemory fermee sans e-mail : pas de nouvelle question pendant 1 h");
		}
		c.dialogueOuvert = false;
	}).detach ();
}

// Builds the JSON body of GET /etat:
//   {"version": "0.7.0", "fournisseurs": ["deepl", ..., "local"?, "essai"?], "langues": ["fr", ...]}
// "local" and "essai" are listed only when those translators were loaded.
std::string Etat (const Contexte& c)
{
	std::string fournisseurs;
	for (const char* f : FournisseursEnLigne)
		fournisseurs += std::string (fournisseurs.empty () ? "" : ", ") + Json::Chaine (f);
	if (c.local != nullptr)
		fournisseurs += ", \"local\"";
	if (c.essai != nullptr)
		fournisseurs += ", \"essai\"";
	std::string langues;
	for (const char* l : LanguesArchicad)
		langues += std::string (langues.empty () ? "" : ", ") + Json::Chaine (l);
	return "{\"version\": " + Json::Chaine (kVersion) + ", \"fournisseurs\": [" + fournisseurs + "], \"langues\": [" + langues + "]}";
}

// Handles one client connection: reads the request, routes it and answers.
//   s : connected client socket
//   c : server context
// Returns false only for POST /arreter (the server must stop), true otherwise.
bool Traiter (SOCKET s, Contexte& c)
{
	// Step 1: read the request
	std::string methode, chemin, corps;
	if (!LireRequete (s, methode, chemin, corps)) {
		Repondre (s, 400, "{\"erreur\": \"requete illisible\"}");
		return true;
	}
	// Step 2: routing - /etat and /arreter are answered at once
	if (methode == "GET" && chemin == "/etat") {
		Repondre (s, 200, Etat (c));
		return true;
	}
	if (methode == "POST" && chemin == "/arreter") {
		Repondre (s, 200, "{\"arret\": true}");
		return false;
	}
	if (methode != "POST" || chemin != "/traduire") {
		Repondre (s, 404, "{\"erreur\": \"chemin inconnu\"}");
		return true;
	}

	// Step 3: POST /traduire - parse and validate the body (target language
	// required, source language optional, both among the ArchiCAD languages,
	// "textes" must be an array)
	Json::Valeur requete;
	if (!Json::Lecteur (corps).Lire (requete) || requete.genre != Json::Valeur::Objet) {
		Repondre (s, 400, "{\"erreur\": \"JSON illisible\"}");
		return true;
	}
	const std::string source = requete.Texte ("source");
	const std::string cible = requete.Texte ("cible");
	const std::string fournisseur = requete.Texte ("fournisseur", "mymemory");
	const Json::Valeur* textes = requete.Champ ("textes");
	if (cible.empty () || !LangueArchicad (cible) || (!source.empty () && !LangueArchicad (source)) ||
		textes == nullptr || textes->genre != Json::Valeur::Tableau) {
		Repondre (s, 400, "{\"erreur\": \"langue non prise en charge ou textes absents\"}");
		return true;
	}

	// Step 4: choose the translator. Online providers are created for each
	// request from the current cles.json, so key changes apply immediately.
	std::unique_ptr<Traducteur> enLigne;
	Traducteur* traducteur = nullptr;
	if (fournisseur == "essai")
		traducteur = c.essai.get ();
	else if (fournisseur == "local")
		traducteur = c.local.get ();
	else {
		enLigne = CreerTraducteurEnLigne (fournisseur, LireCles (c.cles));
		traducteur = enLigne.get ();
	}
	if (traducteur == nullptr) {
		Repondre (s, 400, "{\"erreur\": " + Json::Chaine ("traducteur inconnu ou indisponible : " + fournisseur) + "}");
		return true;
	}

	// Step 5: input texts (a non-string entry becomes an empty text, so the
	// output keeps the same count and order)
	std::vector<std::string> entree;
	for (const Json::Valeur& v : textes->tableau)
		entree.push_back (v.genre == Json::Valeur::Chaine ? v.chaine : std::string ());

	// Step 6: translate (timed for the log)
	std::vector<std::string> sortie;
	std::string erreur;
	const DWORD debut = GetTickCount ();
	if (!traducteur->Traduire (source, cible, entree, sortie, erreur) || sortie.size () != entree.size ()) {
		// Failure -> 503: the layer keeps the original text and will retry
		// (network, quota, key...). A MyMemory quota error also opens the
		// e-mail window.
		Journal ("ERREUR " + traducteur->Nom () + " " + (source.empty () ? "auto" : source) + " -> " + cible + " : " + erreur);
		if (fournisseur == "mymemory" && (erreur.find ("MYMEMORY WARNING") != std::string::npos || erreur.find ("HTTP 429") != std::string::npos))
			SignalerQuotaMyMemory (c);
		Repondre (s, 503, "{\"erreur\": " + Json::Chaine (traducteur->Nom () + " : " + (erreur.empty () ? "traduction impossible" : erreur)) + "}");
		return true;
	}
	Journal (traducteur->Nom () + " : " + std::to_string (entree.size ()) + " texte(s) " + (source.empty () ? "auto" : source) +
			 " -> " + cible + " en " + std::to_string (GetTickCount () - debut) + " ms");
	// Step 7: answer {"traductions": [...]} in the same order as the request
	std::string json = "{\"traductions\": [";
	for (size_t i = 0; i < sortie.size (); ++i)
		json += (i == 0 ? "" : ", ") + Json::Chaine (sortie[i]);
	json += "]}";
	Repondre (s, 200, json);
	return true;
}

}	// namespace


// Entry point of the engine.
// Step 1: parse the command line (see the file header); --montrer-quota and
//         --enregistrer-email run a one-shot action and return immediately.
// Step 2: load the optional offline model.
// Step 3: watch the parent process (ArchiCAD) and exit with it.
// Step 4: open the listening socket on 127.0.0.1:<port>, exclusively.
// Step 5: serve connections one at a time until POST /arreter.
// Exit codes: 0 normal stop, 2 port already in use, 4 Winsock unavailable
// (the one-shot options return their own codes).
int main (int argc, char** argv)
{
	// Step 1: command line
	int port = 8779;
	Contexte contexte;
	std::string modele;
	DWORD parent = 0;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--port" && i + 1 < argc)
			port = std::atoi (argv[++i]);
		else if (a == "--essai")
			contexte.essai = CreerTraducteurEssai ();
		else if (a == "--modele" && i + 1 < argc)
			modele = argv[++i];
		else if (a == "--cles" && i + 1 < argc)
			contexte.cles = argv[++i];
		else if (a == "--sans-dialogue")
			contexte.sansDialogue = true;
		else if (a == "--interface" && i + 1 < argc)
			contexte.langueInterface = TT::IndiceInterface (argv[++i]);
		else if (a == "--montrer-quota" && i + 1 < argc) {
			// Development: shows the MyMemory quota window (0 = anonymous, 1 = with e-mail)
			// and returns the answer (ReponseQuota) as the exit code
			std::string email = "moi@exemple.fr";
			return static_cast<int> (DemanderEmailMyMemory (email, std::atoi (argv[i + 1]) != 0, contexte.langueInterface));
		}
		else if (a == "--enregistrer-email" && i + 2 < argc) {
			// Utility (test bench): writes the MyMemory e-mail into cles.json and exits
			// (0 = written, 1 = failure)
			const std::string chemin = argv[i + 1];
			return EnregistrerEmail (chemin, argv[i + 2]) ? 0 : 1;
		}
		else if (a == "--parent" && i + 1 < argc)
			parent = static_cast<DWORD> (std::strtoul (argv[++i], nullptr, 10));
		else if (a == "--journal" && i + 1 < argc)
			g_journal = argv[++i];
	}
	// Step 2: offline model (only if --modele was given; a failure is logged
	// and the engine keeps running with the other translators)
	if (!modele.empty ()) {
		std::string erreur;
		contexte.local = CreerTraducteurM2M100 (modele, erreur);
		if (contexte.local == nullptr)
			Journal ("modele local indisponible : " + erreur);
	}
	// Step 3: stop together with ArchiCAD - a detached thread waits for the
	// parent process handle to be signalled, then ends the whole process
	if (parent != 0) {
		HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, parent);
		if (h != nullptr) {
			std::thread ([h] () {
				WaitForSingleObject (h, INFINITE);
				Journal ("ArchiCAD ferme : arret");
				ExitProcess (0);
			}).detach ();
		}
	}

	// Step 4: Winsock 2.2 and a TCP socket listening on the loopback address only
	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0)
		return 4;
	SOCKET ecoute = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in adresse = {};
	adresse.sin_family = AF_INET;
	adresse.sin_port = htons (static_cast<u_short> (port));
	inet_pton (AF_INET, "127.0.0.1", &adresse.sin_addr);
	// No SO_REUSEADDR: a second engine on the same port must fail (exclusive use)
	BOOL exclusif = TRUE;
	setsockopt (ecoute, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*> (&exclusif), sizeof (exclusif));
	if (bind (ecoute, reinterpret_cast<sockaddr*> (&adresse), sizeof (adresse)) != 0 || listen (ecoute, 16) != 0) {
		Journal ("port " + std::to_string (port) + " deja pris : un moteur tourne sans doute deja");
		closesocket (ecoute);
		WSACleanup ();
		return 2;
	}
	// Ready: log it and tell the launcher on stdout ("PRET <port>")
	Journal ("pret sur 127.0.0.1:" + std::to_string (port) + " (traducteurs en ligne, cles : " + (contexte.cles.empty () ? std::string ("aucune") : contexte.cles) + ")");
	std::printf ("PRET %d\n", port);
	std::fflush (stdout);

	// Step 5: accept loop - one request per connection, handled sequentially.
	// A 10 s receive timeout keeps a silent client from blocking the server.
	for (;;) {
		SOCKET client = accept (ecoute, nullptr, nullptr);
		if (client == INVALID_SOCKET)
			continue;
		DWORD delai = 10000;
		setsockopt (client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*> (&delai), sizeof (delai));
		const bool continuer = Traiter (client, contexte);
		shutdown (client, SD_BOTH);
		closesocket (client);
		if (!continuer)
			break;
	}
	// POST /arreter received: clean shutdown
	Journal ("arret demande");
	closesocket (ecoute);
	WSACleanup ();
	return 0;
}
