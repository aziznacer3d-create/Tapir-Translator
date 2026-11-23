// *****************************************************************************
// Tapir Translator - ArchiCAD Add-On entry point (API DevKit AC29 / AC30)
//
//   ROLE IN THE SYSTEM
//   This is the only file that talks to the ArchiCAD API. It translates, live,
//   the tkinter interface of the Python scripts launched by the Tapir palette,
//   WITHOUT modifying those scripts. The pieces fit together like this:
//
//   Step 1. At ArchiCAD start-up (the add-on is preloaded), the add-on writes
//           the translation layer couche\sitecustomize.py (embedded in the
//           .apx, see TT_Embarque.hpp) and the common dictionary into
//           %APPDATA%\Tapir Translator (see TT::ExtraireCouche).
//   Step 2. It puts the "couche" folder at the head of ArchiCAD's own
//           PYTHONPATH and sets TAPIR_TRANSLATOR_DIR (TT::PreparerEnvironnement).
//           Tapir keeps this PYTHONPATH for every script it launches, so Python
//           imports the layer by itself before the script runs (one single
//           process, no second script).
//   Step 3. The "Tapir Translator" menu writes the chosen languages and options
//           into reglages.json; the layer re-reads that file live, so windows
//           that are already open switch language too.
//   Step 4. Optionally ("online translation"), the add-on extracts and starts
//           the local translation engine TapirTranslatorMoteur.exe (payload
//           appended to the .apx, see TT_Charge.hpp) and a watchdog thread
//           restarts it if it stops. The layer sends unknown sentences to it.
//
//   Every step is traced in the Session Report ("Tapir Translator : ...").
//   Code that may run outside the main thread (the watchdog) only writes to
//   journal.txt, never to the ArchiCAD API.
// *****************************************************************************

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "TT_Charge.hpp"
#include "TT_Coeur.hpp"
#include "TT_Textes.hpp"
#include "Json.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#include <shellapi.h>

namespace {

// Language of the tool's own interface: ArchiCAD's language (version code FRA, GER...) unless
// another one is chosen in the menu. Both indices below are interface indices (0 = fr, 1 = en ...,
// see TT_Textes.hpp).
// - g_langueMenus: menus are registered only once, at start-up, in the language of that moment
//   (ArchiCAD fixes them until the next start), so this value never changes afterwards.
// - g_langueMessages: Session Report messages follow a change immediately.
// g_versionArchicad holds ArchiCAD's version/language code, read in CheckEnvironment.
std::string g_versionArchicad;
int			g_langueMenus = 1;
int			g_langueMessages = 1;

// Resource IDs of the five menus. The menus are generated at build time
// (RFIX/Tapir_TranslatorMenus.grc, by Tools/GenererMenus.cpp) once per interface language:
// menu ID = BaseMenus (language) + 0..4, and its status-bar help strings are at + DecalageAides.
// These are functions (not constants) because g_langueMenus is only known in RegisterInterface.
// First menu registration error (reported by Initialize, since RegisterInterface must not fail).
GSErrCode g_erreurMenus = NoError;
short	  g_menuEnErreur = 0;

short kMenuEntree	  () { return TT::BaseMenus (g_langueMenus) + 0; }	// input language: item i = TT::Langues[i - 1] (1 = automatic detection)
short kMenuSortie	  () { return TT::BaseMenus (g_langueMenus) + 1; }	// output language: item i = TT::Langues[i - 1] (1 = no translation)
short kMenuCommandes  () { return TT::BaseMenus (g_langueMenus) + 2; }	// "Options" menu (items listed below)
short kMenuTraducteur () { return TT::BaseMenus (g_langueMenus) + 3; }	// online translator: item i = TT::Fournisseurs[i - 1]
short kMenuInterface  () { return TT::BaseMenus (g_langueMenus) + 4; }	// tool language: 1 = same as ArchiCAD, i = TT::Langues[i - 1]
// Item indices (1-based) inside the "Options" menu; item 3 is a separator.
constexpr Int32 kAutomatique   = 1;
constexpr Int32 kCollecte	   = 2;
constexpr Int32 kOuvrirDossier = 4;
constexpr Int32 kEtat		   = 5;
// Item indices inside the "Online translator" menu, after the 5 providers and a separator (item 6).
constexpr Int32 kCles		   = 7;
constexpr Int32 kCorrections   = 8;

// Returns the interface index (0 = fr ... 19 = zh) of the tool's own language.
//   choix: the "interface" setting of reglages.json, a language code; "" means "same as
//          ArchiCAD", in which case the code is derived from ArchiCAD's version code.
// Unknown codes fall back to English (see TT::IndiceInterface).
int LangueOutil (const std::string& choix)
{
	return TT::IndiceInterface (choix.empty () ? TT::LangueDepuisArchicad (g_versionArchicad) : choix);
}

// Global state of the add-on (main thread only, except the engine variables below).
std::wstring g_racine;		// working folder %APPDATA%\Tapir Translator ("" if APPDATA is missing)
TT::Reglages g_reglages;	// in-memory copy of reglages.json, edited by the menu commands
bool		 g_pret = false;	// translation layer and environment are in place

// Engine (TapirTranslatorMoteur.exe) and its watchdog: if the process stops while online
// translation is wanted, a background thread restarts it (that thread never calls the
// ArchiCAD API: it logs to journal.txt). g_verrou protects g_moteur, g_ligneMoteur and
// g_dossierLancement, which are shared between the main thread and the watchdog.
std::mutex			g_verrou;
HANDLE				g_moteur = nullptr;	// process handle of the running engine (nullptr = none)
std::wstring		g_ligneMoteur;		// engine command line (reused for restarts)
std::wstring		g_dossierLancement;	// engine working directory (the folder of the .exe)
std::atomic<bool>	g_moteurVoulu { false };	// true while the engine should be kept running
std::thread			g_veille;			// watchdog thread (runs Veiller)

// Converts a wide (UTF-16) string to an ArchiCAD GS::UniString, going through UTF-8.
GS::UniString Uni (const std::wstring& texte)
{
	return GS::UniString (TT::Utf8 (texte).c_str (), CC_UTF8);
}

// Converts a UTF-8 std::string to a GS::UniString.
GS::UniString Uni (const std::string& utf8)
{
	return GS::UniString (utf8.c_str (), CC_UTF8);
}

// Writes one line to ArchiCAD's Session Report, prefixed with "Tapir Translator : ", so that
// the user can copy the whole trace. Main thread only (ArchiCAD API call).
void Rapport (const GS::UniString& ligne)
{
	// The "%T" format passes the line through unchanged: accented characters are kept and a '%'
	// inside a file path is not interpreted as a printf directive.
	const GS::UniString texte = GS::UniString ("Tapir Translator : ") + ligne;
	ACAPI_WriteReport ("%T", false, texte.ToPrintf ());
}

// Returns the user-facing text 't' (table in TT_Textes.cpp) in the current message language.
GS::UniString T (TT::Texte t)
{
	return Uni (std::string (TT::Txt (t, g_langueMessages)));
}

// Picks the text of a technical line (diagnostics, system errors), which is not in the 20-language
// table: French when the message language is French (index 0), English otherwise.
const char* FE (const char* fr, const char* en)
{
	return g_langueMessages == 0 ? fr : en;
}

// Displayable name of a language code, e.g. "Deutsch (de)".
//   code:   language code from TT::Langues ("" = no specific language)
//   siVide: text returned when 'code' is empty (e.g. "automatic detection" or "no translation")
// An unknown code is returned as is.
GS::UniString NomLangue (const std::string& code, TT::Texte siVide)
{
	if (code.empty ())
		return T (siVide);
	for (int i = 1; i < TT::NombreLangues; ++i) {
		if (code == TT::Langues[i].code)
			return Uni (std::string (TT::Langues[i].nom)) + " (" + Uni (code) + ")";
	}
	return Uni (code);
}

// Summary of the current language pair for the Session Report: "<input> -> <output>".
GS::UniString Langues ()
{
	return NomLangue (g_reglages.entree, TT::M_AUTO) + " -> " + NomLangue (g_reglages.langue, TT::M_AUCUNE);
}

// Sets or clears the check mark of one menu item.
//   menu:  menu resource ID (one of the kMenu... functions)
//   index: 1-based item index inside that menu
//   coche: true = checked
// The current flags are read first so that only the "checked" bit changes.
void Cocher (short menu, Int32 index, bool coche)
{
	API_MenuItemRef ref = {};
	ref.menuResID = menu;
	ref.itemIndex = index;
	GSFlags flags = 0;
	ACAPI_MenuItem_GetMenuItemFlags (&ref, &flags);
	if (coche)
		flags |= API_MenuItemChecked;
	else
		flags &= ~API_MenuItemChecked;
	ACAPI_MenuItem_SetMenuItemFlags (&ref, &flags);
}

// Updates every check mark of the five menus from g_reglages. Called after each settings change.
void RafraichirCoches ()
{
	// Step 1: input / output language menus. Item i + 1 is TT::Langues[i]; i = 0 is the empty code,
	// i.e. "automatic detection" (input) or "no translation" (output).
	for (int i = 0; i < TT::NombreLangues; ++i) {
		Cocher (kMenuEntree (), i + 1, g_reglages.entree == TT::Langues[i].code);
		Cocher (kMenuSortie (), i + 1, g_reglages.langue == TT::Langues[i].code);
	}
	// Step 2: the two toggles of the "Options" menu.
	Cocher (kMenuCommandes (), kAutomatique, g_reglages.moteur);
	Cocher (kMenuCommandes (), kCollecte, g_reglages.collecte);
	// Step 3: the selected online translator.
	for (int i = 0; i < TT::NombreFournisseurs; ++i)
		Cocher (kMenuTraducteur (), i + 1, g_reglages.fournisseur == TT::Fournisseurs[i].code);
	// Step 4: the tool language (item 1, empty code = same as ArchiCAD).
	for (int i = 0; i < TT::NombreLangues; ++i)
		Cocher (kMenuInterface (), i + 1, g_reglages.interface == TT::Langues[i].code);
}

// Appends one UTF-8 line to <g_racine>\journal.txt, as "YYYY-MM-DD hh:mm:ss [add-on] <line>".
// Unlike Rapport, this is safe outside the main thread (pure Win32): the watchdog uses it.
// The layer and the engine write to the same file, hence the shared open mode. Errors are ignored.
void Journal (const std::string& ligne)
{
	if (g_racine.empty ())
		return;
	// Step 1: open in append mode (created if missing), shared with the other writers.
	HANDLE f = CreateFileW (TT::Joindre (g_racine, L"journal.txt").c_str (), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
							nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return;
	// Step 2: local time stamp.
	SYSTEMTIME t;
	GetLocalTime (&t);
	char date[40];
	snprintf (date, sizeof (date), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	// Step 3: write the whole line in one call, then close.
	const std::string texte = std::string (date) + " [add-on] " + ligne + "\n";
	DWORD ecrit = 0;
	WriteFile (f, texte.data (), static_cast<DWORD> (texte.size ()), &ecrit, nullptr);
	CloseHandle (f);
}

// Display name of an online translator code ("deepl" -> "DeepL"); unknown codes are returned as is.
GS::UniString NomFournisseur (const std::string& code)
{
	for (int i = 0; i < TT::NombreFournisseurs; ++i) {
		if (code == TT::Fournisseurs[i].code)
			return GS::UniString (TT::Fournisseurs[i].nom);
	}
	return Uni (code);
}

// Describes the state of the credentials of one translator in cles.json, for the Session Report.
// It never returns the key itself: only whether the expected field is filled in.
//   code: translator code (TT::Fournisseurs)
// What is expected depends on the service: MyMemory needs nothing (an e-mail raises the quota),
// LibreTranslate needs a server URL, Microsoft needs a key and a region, the others a key.
GS::UniString EtatCle (const std::string& code)
{
	// Step 1: read cles.json (it may be missing or edited by hand into invalid JSON).
	std::string contenu;
	if (!TT::LireFichier (TT::CheminCles (g_racine), contenu))
		return FE ("cles.json absent", "cles.json missing");
	// Step 2: parse it, skipping a UTF-8 BOM (Notepad may add one when saving).
	Json::Valeur v;
	if (!Json::Lecteur (contenu.compare (0, 3, "\xEF\xBB\xBF") == 0 ? contenu.substr (3) : contenu).Lire (v))
		return FE ("cles.json illisible (JSON invalide)", "cles.json unreadable (invalid JSON)");
	// Step 3: check the field(s) that this service needs.
	const Json::Valeur* s = v.Champ (code);
	if (code == "mymemory")
		return s != nullptr && !s->Texte ("email").empty () ? T (TT::R_EMAIL_OK) : T (TT::R_SANS_CLE);
	if (code == "libretranslate")
		return s != nullptr && !s->Texte ("url").empty () ? T (TT::R_ADRESSE_OK) : T (TT::R_ADRESSE_A_SAISIR);
	if (s == nullptr || s->Texte ("cle").empty ())
		return T (TT::R_CLE_A_SAISIR);
	if (code == "microsoft" && s->Texte ("region").empty ())
		return T (TT::R_CLE_OK) + FE (", region vide", ", region empty");
	return T (TT::R_CLE_OK);
}

// -----------------------------------------------------------------------------
//  Translation engine (TapirTranslatorMoteur.exe): extraction, launch, watchdog
//  The engine is a separate local process (127.0.0.1:TT::PortMoteur) that sends the
//  sentences unknown to the dictionaries to the chosen online translator (or, with the
//  TT_MODELE_LOCAL build option, to an offline model). A crash there never reaches ArchiCAD.
// -----------------------------------------------------------------------------

// Returns the full path of the .apx itself (it carries the engine, and optionally the model,
// as a payload appended to the file, see TT_Charge.hpp); "" on failure.
// The module is found from the address of this very function, so it works whatever the
// install folder; UNCHANGED_REFCOUNT avoids having to release the module handle.
std::wstring CheminApx ()
{
	HMODULE module = nullptr;
	wchar_t chemin[MAX_PATH] = {};
	if (GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							reinterpret_cast<LPCWSTR> (&CheminApx), &module) &&
		GetModuleFileNameW (module, chemin, MAX_PATH) > 0)
		return chemin;
	return std::wstring ();
}

// Base folder under which the payload is extracted: %LOCALAPPDATA%\Tapir Translator\moteur
// (each version then goes into its own <hash> sub-folder). LOCALAPPDATA keeps the binaries
// out of the roaming profile; if it is missing, <g_racine>\moteur is used instead.
std::wstring BaseExtraction ()
{
	std::wstring local = TT::LireVariable (L"LOCALAPPDATA");
	return local.empty () ? TT::Joindre (g_racine, L"moteur") : TT::Joindre (TT::Joindre (local, L"Tapir Translator"), L"moteur");
}

std::wstring g_dossierMoteur;	// folder of the extracted payload (engine + optional model); "" = not extracted yet

// Lists, in order of preference, the paths where TapirTranslatorMoteur.exe is looked for:
//   1. the engine extracted from the .apx payload (normal case);
//   2. (development) a "TapirTranslatorMoteur" folder next to the .apx;
//   3. %APPDATA%\Tapir Translator\moteur.
// The paths are returned whether or not the files exist (Etat also displays them).
std::vector<std::wstring> EmplacementsMoteur ()
{
	std::vector<std::wstring> res;
	if (!g_dossierMoteur.empty ())
		res.push_back (TT::Joindre (g_dossierMoteur, L"TapirTranslatorMoteur.exe"));
	const std::wstring apx = CheminApx ();
	if (!apx.empty ())
		res.push_back (TT::Joindre (TT::Joindre (apx.substr (0, apx.find_last_of (L"\\/")), L"TapirTranslatorMoteur"), L"TapirTranslatorMoteur.exe"));
	res.push_back (TT::Joindre (TT::Joindre (g_racine, L"moteur"), L"TapirTranslatorMoteur.exe"));
	return res;
}

// Extracts the payload of the .apx into BaseExtraction ()\<hash> and remembers the folder in
// g_dossierMoteur. TT::ExtraireCharge does nothing if this version is already fully extracted,
// so the real copy happens only once per version; within a session this function runs once.
void ExtraireMoteur ()
{
	if (!g_dossierMoteur.empty ())
		return;
	// Step 1: extract (or find the existing extraction) and trace what happened.
	std::vector<std::wstring> journal;
	std::wstring dossier;
	const bool ok = TT::ExtraireCharge (CheminApx (), BaseExtraction (), dossier, journal);
	for (const std::wstring& l : journal)
		Rapport (Uni (l));
	if (!ok)
		return;
	g_dossierMoteur = dossier;
	// Step 2: delete the folders of older extracted versions (including the old ~500 MB offline
	// engine), except the current one. A folder whose engine is still running cannot be deleted
	// (locked file): it stays, and deletion is retried at the next start.
	const std::wstring base = BaseExtraction ();
	const std::wstring garde = dossier.substr (dossier.find_last_of (L"\\/") + 1);
	for (const std::wstring& nom : TT::ListerDossiers (base)) {
		if (nom == garde)
			continue;
		const std::wstring ancien = TT::Joindre (base, nom);
		TT::SupprimerDossier (ancien);
		Rapport (GS::UniString (TT::DossierExiste (ancien) ? FE ("ancienne version non supprimee (en cours d'utilisation) : ", "old version not deleted (in use): ")
														   : FE ("ancienne version supprimee : ", "old version deleted: ")) + Uni (ancien));
	}
}

// Returns true if the engine process we started is still alive (zero-timeout wait on its handle).
bool MoteurEnMarche ()
{
	std::lock_guard<std::mutex> verrou (g_verrou);
	return g_moteur != nullptr && WaitForSingleObject (g_moteur, 0) == WAIT_TIMEOUT;
}

// Starts the engine process with the command line g_ligneMoteur in g_dossierLancement.
// Must be called with g_verrou held (used by both LancerMoteur and the watchdog).
//   erreur: receives GetLastError () when the start fails
// Returns true on success; g_moteur then holds the new process handle (the previous one,
// if any, is closed).
bool DemarrerProcessus (DWORD& erreur)
{
	// Step 1: CreateProcessW may modify the command line, so it gets a writable copy.
	std::vector<wchar_t> tampon (g_ligneMoteur.begin (), g_ligneMoteur.end ());
	tampon.push_back (L'\0');
	// Step 2: start without a console window, at below-normal priority so that ArchiCAD
	// stays responsive.
	STARTUPINFOW si = {};
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
						 nullptr, g_dossierLancement.c_str (), &si, &pi)) {
		erreur = GetLastError ();
		return false;
	}
	// Step 3: only the process handle is kept.
	CloseHandle (pi.hThread);
	// Step 4: the engine's "MyMemory quota" window must be allowed to come to the foreground.
	AllowSetForegroundWindow (pi.dwProcessId);
	// Step 5: replace the handle of the previous (dead) process.
	if (g_moteur != nullptr)
		CloseHandle (g_moteur);
	g_moteur = pi.hProcess;
	return true;
}

// Watchdog thread body: restarts the engine whenever it stops (crash, killed process...)
// for as long as g_moteurVoulu is true. Runs outside the main thread, so it only logs to
// journal.txt (Journal), never to the Session Report.
// Exit code 2 means the port is already taken by the engine of another ArchiCAD instance:
// in that case the restart is retried much later (30 s).
void Veiller ()
{
	int relances = 0;							// restarts in the current one-hour window
	ULONGLONG debutFenetre = GetTickCount64 ();	// start of that window (ms)
	while (g_moteurVoulu) {
		// Step 1: take a snapshot of the process handle under the lock.
		HANDLE h = nullptr;
		{
			std::lock_guard<std::mutex> verrou (g_verrou);
			h = g_moteur;
		}
		// Step 2: wait up to 500 ms for the process to end; still running -> loop again
		// (this also re-checks g_moteurVoulu twice a second).
		if (h != nullptr && WaitForSingleObject (h, 500) == WAIT_TIMEOUT)
			continue;
		if (!g_moteurVoulu)
			break;
		// Step 3: the engine has stopped: read its exit code.
		DWORD code = 0;
		if (h != nullptr)
			GetExitCodeProcess (h, &code);
		// Step 4: restart budget of 20 per hour; the counter is reset when the window expires.
		if (GetTickCount64 () - debutFenetre > 3600000ull) {
			debutFenetre = GetTickCount64 ();
			relances = 0;
		}
		if (relances >= 20) {
			Journal ("moteur arrete 20 fois en une heure : surveillance suspendue (Options > Traduction en ligne pour relancer)");
			return;
		}
		// Step 5: back-off delay: 30 s if the port is taken (code 2), otherwise 1 s, 2 s, 3 s...
		// growing with each restart. The wait is sliced in 250 ms steps so that ArreterMoteur
		// (which clears g_moteurVoulu and joins this thread) is never blocked for long.
		const DWORD attente = code == 2 ? 30000 : static_cast<DWORD> (1000 * (1 + relances));
		Journal ("moteur arrete (code " + std::to_string (code) + ") : relance dans " + std::to_string (attente / 1000) + " s");
		for (DWORD t = 0; t < attente && g_moteurVoulu; t += 250)
			Sleep (250);
		if (!g_moteurVoulu)
			break;
		// Step 6: restart with the same command line, under the lock.
		std::lock_guard<std::mutex> verrou (g_verrou);
		DWORD erreur = 0;
		if (DemarrerProcessus (erreur))
			Journal ("moteur relance");
		else
			Journal ("relance du moteur impossible (erreur Windows " + std::to_string (erreur) + ")");
		++relances;
	}
}

// Stops the engine and its watchdog. Safe to call when nothing is running.
void ArreterMoteur ()
{
	// Step 1: tell the watchdog to stop, then wait for its thread to finish
	// (it must not restart the engine we are about to kill).
	g_moteurVoulu = false;
	if (g_veille.joinable ())
		g_veille.join ();
	// Step 2: kill the process if it is still alive, release its handle, and report it.
	std::lock_guard<std::mutex> verrou (g_verrou);
	if (g_moteur != nullptr) {
		if (WaitForSingleObject (g_moteur, 0) == WAIT_TIMEOUT)
			TerminateProcess (g_moteur, 0);
		CloseHandle (g_moteur);
		g_moteur = nullptr;
		Rapport (T (TT::R_MOTEUR_ARRETE));
	}
}

// Starts the translation engine (if not already running) and its watchdog thread.
// Called at start-up when online translation is enabled, and from the menu commands.
// Every outcome is traced in the Session Report.
void LancerMoteur ()
{
	// Step 1: nothing to do if the engine is already running and supervised; otherwise clean
	// up any half-state (dead process, watchdog still alive) before starting again.
	if (MoteurEnMarche () && g_moteurVoulu)
		return;
	ArreterMoteur ();
	// Step 2: make sure the payload of the .apx is extracted.
	ExtraireMoteur ();
	// Step 3: take the first engine executable that exists (see EmplacementsMoteur);
	// if none is found, report every path that was tried.
	std::wstring exe;
	for (const std::wstring& e : EmplacementsMoteur ()) {
		if (TT::FichierExiste (e)) {
			exe = e;
			break;
		}
	}
	if (exe.empty ()) {
		Rapport (T (TT::R_MOTEUR_INTROUVABLE));
		for (const std::wstring& e : EmplacementsMoteur ())
			Rapport (GS::UniString ("  ") + Uni (e));
		return;
	}
	// Step 4: the engine reads cles.json at each request: create the template if it is missing
	// (an error here is not fatal, the engine then simply finds no key).
	const std::wstring dossier = exe.substr (0, exe.find_last_of (L"\\/"));
	std::wstring erreurCles;
	TT::CreerModeleCles (g_racine, erreurCles);
	// Step 5: build the command line:
	//   --port      local port the layer connects to
	//   --cles      path of cles.json (keys never pass through the scripts)
	//   --parent    ArchiCAD's process ID (the engine exits when ArchiCAD exits)
	//   --journal   shared log file
	//   --interface language code of the tool (for the MyMemory quota window)
	std::wstring ligne = L"\"" + exe + L"\" --port " + std::to_wstring (TT::PortMoteur) + L" --cles \"" + TT::CheminCles (g_racine) +
						 L"\" --parent " + std::to_wstring (GetCurrentProcessId ()) + L" --journal \"" +
						 TT::Joindre (g_racine, L"journal.txt") + L"\" --interface " +
						 TT::DepuisUtf8 (TT::CodeInterface (g_langueMessages));
	// Step 6: pass the offline model only if the .apx carries one (build option TT_MODELE_LOCAL).
	const std::wstring modele = TT::Joindre (dossier, L"modele");
	if (TT::DossierExiste (modele))
		ligne += L" --modele \"" + modele + L"\"";
	// Step 7: remember the command line and folder (the watchdog reuses them) and start the
	// process, all under the lock.
	DWORD erreur = 0;
	{
		std::lock_guard<std::mutex> verrou (g_verrou);
		g_ligneMoteur = ligne;
		g_dossierLancement = dossier;
		if (!DemarrerProcessus (erreur)) {
			Rapport (GS::UniString (FE ("ERREUR lancement du moteur (", "ERROR starting the engine (")) +
					 GS::ValueToUniString (static_cast<Int32> (erreur)) + ") : " + Uni (exe));
			return;
		}
	}
	// Step 8: start the watchdog, then report the chosen translator and the state of its key.
	g_moteurVoulu = true;
	g_veille = std::thread (Veiller);
	Rapport (Uni (TT::TxtF (TT::R_MOTEUR_LANCE, g_langueMessages,
							{ { "f", NomFournisseur (g_reglages.fournisseur).ToCStr (0, MaxUSize, CC_UTF8).Get () },
							  { "e", EtatCle (g_reglages.fournisseur).ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
}

// Menu "Online translator > Term corrections...": opens corrections.json in Notepad, creating
// the pre-filled template first if the file does not exist (an existing file is never
// overwritten). ShellExecuteW returns a value <= 32 on failure.
void OuvrirCorrections ()
{
	std::wstring erreur;
	if (!TT::CreerModeleCorrections (g_racine, erreur)) {
		Rapport (GS::UniString (FE ("ERREUR creation de corrections.json : ", "ERROR creating corrections.json: ")) + Uni (erreur));
		return;
	}
	const std::wstring parametres = L"\"" + TT::CheminCorrections (g_racine) + L"\"";
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", L"notepad.exe", parametres.c_str (), nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Rapport (GS::UniString (FE ("ERREUR ouverture de ", "ERROR opening ")) + Uni (TT::CheminCorrections (g_racine)));
	else
		Rapport (T (TT::R_CORRECTIONS_OUVERT));
}

// Menu "Online translator > Translator keys...": opens cles.json in Notepad, creating the empty
// template first if the file does not exist. The keys stay on this computer: only the engine
// reads them, at each request, so a key saved here is used by the very next translation.
void OuvrirCles ()
{
	std::wstring erreur;
	if (!TT::CreerModeleCles (g_racine, erreur)) {
		Rapport (GS::UniString (FE ("ERREUR creation de cles.json : ", "ERROR creating cles.json: ")) + Uni (erreur));
		return;
	}
	const std::wstring parametres = L"\"" + TT::CheminCles (g_racine) + L"\"";
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", L"notepad.exe", parametres.c_str (), nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Rapport (GS::UniString (FE ("ERREUR ouverture de ", "ERROR opening ")) + Uni (TT::CheminCles (g_racine)));
	else
		Rapport (T (TT::R_CLES_OUVERT) + " (" + Uni (TT::CheminCles (g_racine)) + ")");
}

// Saves g_reglages to reglages.json (the layer picks it up live), reports the new settings
// in the Session Report and refreshes the menu check marks.
void Enregistrer ()
{
	std::wstring erreur;
	// Step 1: warn when input and output languages are the same (nothing will be translated).
	if (g_reglages.entree == g_reglages.langue && !g_reglages.langue.empty ())
		Rapport (T (TT::R_MEME_LANGUE));
	// Step 2: write the file atomically; on success, report languages and online translation
	// state (translator + key state), otherwise report the error.
	if (TT::EcrireReglages (g_racine, g_reglages, erreur)) {
		const GS::UniString enLigne = g_reglages.moteur
			? T (TT::R_OUI) + " (" + NomFournisseur (g_reglages.fournisseur) + " : " + EtatCle (g_reglages.fournisseur) + ")"
			: T (TT::R_NON);
		Rapport (Uni (TT::TxtF (TT::R_REGLAGES, g_langueMessages,
								{ { "l", Langues ().ToCStr (0, MaxUSize, CC_UTF8).Get () },
								  { "o", enLigne.ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
	} else
		Rapport (GS::UniString (FE ("ERREUR ecriture reglages.json : ", "ERROR writing reglages.json: ")) + Uni (erreur));
	// Step 3: menu check marks follow the in-memory settings.
	RafraichirCoches ();
}

// Add-on start-up sequence (called once from Initialize). Puts everything in place so that the
// next Python script launched by Tapir is translated.
void Demarrer ()
{
	// Step 1: working folder %APPDATA%\Tapir Translator; without APPDATA nothing can work.
	g_racine = TT::DossierParDefaut ();
	if (g_racine.empty ()) {
		Rapport (FE ("ERREUR : variable APPDATA absente, la traduction est inactive.", "ERROR: APPDATA variable missing, translation is inactive."));
		return;
	}
	// Step 2: write the translation layer (sitecustomize.py), the common dictionary and the
	// user templates, and trace each file in the Session Report.
	std::vector<std::wstring> journal;
	const bool couche = TT::ExtraireCouche (g_racine, journal);
	for (const std::wstring& ligne : journal)
		Rapport (Uni (ligne));

	// Step 3: prepend the layer folder to ArchiCAD's PYTHONPATH and set TAPIR_TRANSLATOR_DIR;
	// the scripts launched by Tapir inherit this environment.
	std::wstring pythonPath;
	const bool env = TT::PreparerEnvironnement (g_racine, pythonPath);
	Rapport (GS::UniString ("PYTHONPATH = ") + Uni (pythonPath) + (env ? "" : "  (ERROR SetEnvironmentVariable)"));

	// Step 4: load the saved settings and the message language they imply.
	g_reglages = TT::LireReglages (g_racine);
	g_langueMessages = LangueOutil (g_reglages.interface);
	// Step 5: translation is active only if both the layer and the environment are in place;
	// the engine is started only if online translation was left enabled.
	g_pret = couche && env;
	if (g_pret && g_reglages.moteur)
		LancerMoteur ();
	// Step 6: final status line (version and languages, or "inactive").
	Rapport (Uni (TT::TxtF (g_pret ? TT::R_DEMARRAGE : TT::R_INACTIF, g_langueMessages,
							{ { "v", TT::VersionAddOn }, { "l", Langues ().ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
}

// Menu "Options > Tapir Translator status": writes a full diagnostic to the Session Report, so
// the user can paste it instead of describing a problem. Keys are never printed, only their state.
void Etat ()
{
	const GS::UniString oui = T (TT::R_OUI), non = T (TT::R_NON);
	// Step 1: title, working folder, and the two environment variables as ArchiCAD sees them.
	Rapport (GS::UniString ("=== ") + T (TT::M_ETAT) + " " + TT::VersionAddOn + " ===");
	Rapport (GS::UniString (FE ("dossier = ", "folder = ")) + Uni (g_racine) + (g_pret ? "" : FE ("  (couche NON installee)", "  (layer NOT installed)")));
	Rapport (GS::UniString ("PYTHONPATH = ") + Uni (TT::LireVariable (L"PYTHONPATH")));
	Rapport (GS::UniString ("TAPIR_TRANSLATOR_DIR = ") + Uni (TT::LireVariable (L"TAPIR_TRANSLATOR_DIR")));
	// Step 2: languages, collection option and tool language (with ArchiCAD's code when automatic).
	Rapport (GS::UniString (FE ("langues = ", "languages = ")) + Langues () + ", " + T (TT::M_COLLECTE) + " = " + (g_reglages.collecte ? oui : non));
	Rapport (T (TT::M_INTERFACE) + " = " + Uni (std::string (TT::NomInterface (g_langueMessages))) +
			 (g_reglages.interface.empty () ? GS::UniString (" (") + T (TT::M_INTERFACE_AUTO) + " : " + Uni (g_versionArchicad) + ")" : GS::UniString ()));
	// Step 3: every translator with the state of its key; "[x]" marks the selected one.
	for (int i = 0; i < TT::NombreFournisseurs; ++i)
		Rapport (GS::UniString ("  ") + (g_reglages.fournisseur == TT::Fournisseurs[i].code ? "[x] " : "    ") +
				 TT::Fournisseurs[i].nom + " : " + EtatCle (TT::Fournisseurs[i].code));
	// Step 4: online translation wanted or not, and whether the engine process is alive.
	Rapport (T (TT::M_EN_LIGNE) + " = " + (g_reglages.moteur ? oui : non) + ", " + FE ("moteur ", "engine ") +
			 (MoteurEnMarche () ? FE ("en marche", "running") : FE ("arrete", "stopped")) + " (port " +
			 GS::ValueToUniString (static_cast<Int32> (TT::PortMoteur)) + ")");
	// Step 5: payload embedded in the .apx (hash and the first 12 file paths).
	{
		std::wstring empreinte;
		std::vector<std::wstring> chemins;
		if (TT::LireCharge (CheminApx (), empreinte, chemins)) {
			Rapport (GS::UniString (FE ("charge embarquee dans le .apx : empreinte ", "payload embedded in the .apx: hash ")) + Uni (empreinte) + ", " +
					 GS::ValueToUniString (static_cast<Int32> (chemins.size ())) + FE (" fichier(s)", " file(s)"));
			for (size_t i = 0; i < chemins.size () && i < 12; ++i)
				Rapport (GS::UniString ("  ") + Uni (chemins[i]));
		} else
			Rapport (FE ("aucune charge embarquee dans le .apx", "no payload embedded in the .apx"));
	}
	// Step 6: candidate engine paths ("[x]" = file exists) and presence of the layer file.
	for (const std::wstring& e : EmplacementsMoteur ())
		Rapport (GS::UniString ("  ") + (TT::FichierExiste (e) ? "[x] " : "[ ] ") + Uni (e));
	Rapport (GS::UniString ("sitecustomize.py = ") +
			 (TT::FichierExiste (TT::Joindre (TT::Joindre (g_racine, L"couche"), L"sitecustomize.py")) ? oui : non));

	// Step 7: installed dictionaries.
	const std::wstring dictionnaires = TT::Joindre (g_racine, L"dictionnaires");
	const std::vector<std::wstring> dicos = TT::ListerJson (dictionnaires);
	Rapport (GS::UniString (FE ("dictionnaires (", "dictionaries (")) + GS::ValueToUniString (static_cast<Int32> (dicos.size ())) + ") :");
	for (const std::wstring& d : dicos)
		Rapport (GS::UniString ("  ") + Uni (d));

	// Step 8: collected untranslated texts: one file per script, with the number of entries
	// that are still empty (not yet translated by the user).
	const std::wstring manquants = TT::Joindre (g_racine, L"manquants");
	const std::vector<std::wstring> fichiers = TT::ListerJson (manquants);
	Rapport (GS::UniString (FE ("textes manquants (", "missing texts (")) + GS::ValueToUniString (static_cast<Int32> (fichiers.size ())) + ") :");
	for (const std::wstring& f : fichiers) {
		std::string contenu;
		TT::LireFichier (TT::Joindre (manquants, f), contenu);
		Rapport (GS::UniString ("  ") + Uni (f) + " : " + GS::ValueToUniString (static_cast<Int32> (TT::CompterVides (contenu))));
	}

	// Step 9: the last 15 non-empty lines of journal.txt (layer, engine and add-on errors).
	const std::vector<std::string> lignes = TT::DernieresLignes (TT::Joindre (g_racine, L"journal.txt"), 15);
	Rapport (GS::UniString ("journal.txt (") + GS::ValueToUniString (static_cast<Int32> (lignes.size ())) + ") :");
	for (const std::string& l : lignes)
		Rapport (GS::UniString ("  ") + Uni (l));
}

// Menu "Options > Open the dictionaries folder": opens %APPDATA%\Tapir Translator in Explorer
// (created first if needed).
void OuvrirDossier ()
{
	if (g_racine.empty ())
		return;
	TT::CreerDossier (g_racine);
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", g_racine.c_str (), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Rapport (GS::UniString (FE ("ERREUR ouverture du dossier ", "ERROR opening the folder ")) + Uni (g_racine));
}

// Handler of all five menus (installed in Initialize). Dispatches on the menu resource ID and the
// 1-based item index; always returns NoError (errors are traced in the Session Report).
GSErrCode MenuCommandHandler (const API_MenuParams* menuParams)
{
	const short menu = menuParams->menuItemRef.menuResID;
	const Int32 index = menuParams->menuItemRef.itemIndex;
	// The three language menus have one item per entry of TT::Langues.
	const bool langue = index >= 1 && index <= TT::NombreLangues;
	// Input / output language: store the code and save.
	if (menu == kMenuEntree () && langue) {
		g_reglages.entree = TT::Langues[index - 1].code;
		Enregistrer ();
	} else if (menu == kMenuSortie () && langue) {
		g_reglages.langue = TT::Langues[index - 1].code;
		Enregistrer ();
	} else if (menu == kMenuTraducteur ()) {
		// Online translator menu: a provider, or one of the two "open file" commands.
		if (index >= 1 && index <= TT::NombreFournisseurs) {
			g_reglages.fournisseur = TT::Fournisseurs[index - 1].code;
			// Choosing a translator also turns online translation on (and starts the engine).
			if (!g_reglages.moteur) {
				g_reglages.moteur = true;
				LancerMoteur ();
			}
			Enregistrer ();
		} else if (index == kCles) {
			OuvrirCles ();
		} else if (index == kCorrections) {
			OuvrirCorrections ();
		}
	} else if (menu == kMenuInterface () && langue) {
		// Tool language. Item 1 = same as ArchiCAD (empty code). Messages and the engine window
		// change immediately; the menus only at the next ArchiCAD start (g_langueMenus is fixed).
		g_reglages.interface = TT::Langues[index - 1].code;
		g_langueMessages = LangueOutil (g_reglages.interface);
		std::wstring erreur;
		TT::EcrireReglages (g_racine, g_reglages, erreur);
		Rapport (Uni (TT::TxtF (TT::R_INTERFACE, g_langueMessages, { { "l", TT::NomInterface (g_langueMessages) } })));
		RafraichirCoches ();
		// The engine receives the language on its command line: restart it if it is running.
		if (g_moteurVoulu) {
			ArreterMoteur ();
			LancerMoteur ();	// the engine picks up the new language (MyMemory quota window)
		}
	} else if (menu == kMenuCommandes ()) {
		// Options menu: two toggles (saved) and two commands.
		if (index == kAutomatique) {
			g_reglages.moteur = !g_reglages.moteur;
			if (g_reglages.moteur)
				LancerMoteur ();
			else
				ArreterMoteur ();
			Enregistrer ();
		} else if (index == kCollecte) {
			g_reglages.collecte = !g_reglages.collecte;
			Enregistrer ();
		} else if (index == kOuvrirDossier) {
			OuvrirDossier ();
		} else if (index == kEtat) {
			Etat ();
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
	envir->addOnInfo.description += GS::UniString (" (") + TT::VersionAddOn + ")";
	// Step 2: language code of this ArchiCAD version (FRA, GER, INT...): the default tool language.
	g_versionArchicad = envir->serverInfo.language.ToCStr (0, MaxUSize, CC_UTF8).Get ();
	// Step 3: preload, because the environment must be in place before the first Tapir script runs.
	return APIAddon_Preload;
}


// Registers the menus. Called once; ArchiCAD keeps them until it quits, which is why the menu
// language is decided here, once and for all for this session.
GSErrCode RegisterInterface (void)
{
	// Step 1: menu language = the saved choice, otherwise ArchiCAD's language. The settings are
	// read directly here because Initialize (and thus Demarrer) has not run yet.
	const std::wstring racine = TT::DossierParDefaut ();
	const TT::Reglages r = racine.empty () ? TT::Reglages () : TT::LireReglages (racine);
	g_langueMenus = LangueOutil (r.interface);
	g_langueMessages = g_langueMenus;
	// Step 2: register the five menus of that language (help strings at + DecalageAides). They all
	// carry the same title "Tapir Translator", so ArchiCAD merges them into one main menu
	// (MenuFlag_InsertIntoSame); a separator is put before the Options menu.
	const auto Enregistrer = [] (short menu, GSFlags drapeaux) {
		const GSErrCode e = ACAPI_MenuItem_RegisterMenu (menu, static_cast<short> (menu + TT::DecalageAides), MenuCode_UserDef, drapeaux);
		if (e != NoError && g_erreurMenus == NoError) {
			g_erreurMenus = e;
			g_menuEnErreur = menu;
		}
	};
	Enregistrer (kMenuEntree (), MenuFlag_InsertIntoSame);
	Enregistrer (kMenuSortie (), MenuFlag_InsertIntoSame);
	Enregistrer (kMenuTraducteur (), MenuFlag_InsertIntoSame);
	Enregistrer (kMenuCommandes (), MenuFlag_SeparatorBefore | MenuFlag_InsertIntoSame);
	Enregistrer (kMenuInterface (), MenuFlag_InsertIntoSame);
	// Step 3: never return the error: a failing menu must not make ArchiCAD unload the add-on
	// (the translation itself would then stop too). The error is reported by Initialize.
	return NoError;
}


// Called after RegisterInterface, when the add-on is loaded (at start-up, since it is preloaded).
GSErrCode Initialize (void)
{
	// Step 1: install the same handler on the five menus; failures are reported, not fatal.
	GSErrCode err = NoError;
	for (const short menu : { kMenuEntree (), kMenuSortie (), kMenuTraducteur (), kMenuCommandes (), kMenuInterface () }) {
		const GSErrCode e = ACAPI_MenuItem_InstallMenuHandler (menu, MenuCommandHandler);
		if (e != NoError) {
			Rapport (GS::UniString ("ERREUR ACAPI_MenuItem_InstallMenuHandler (") + GS::ValueToUniString (static_cast<Int32> (menu)) +
					 ") = " + GS::ValueToUniString (static_cast<Int32> (e)));
			err = e;
		}
	}
	// Step 2: layer, environment, settings and (optionally) the engine.
	Demarrer ();
	if (g_erreurMenus != NoError)
		Rapport (GS::UniString ("ERROR ACAPI_MenuItem_RegisterMenu (") + GS::ValueToUniString (static_cast<Int32> (g_menuEnErreur)) +
				 ") = " + GS::ValueToUniString (static_cast<Int32> (g_erreurMenus)));
	// Step 3: menu check marks reflect the loaded settings.
	RafraichirCoches ();
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
	ArreterMoteur ();
	return NoError;
}
