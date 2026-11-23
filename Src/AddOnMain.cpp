// *****************************************************************************
// Tapir Translator - Add-On ArchiCAD (API DevKit AC29 / AC30)
//
//   Traduit en direct l'interface tkinter des scripts Python lances par la
//   palette Tapir, SANS modifier les scripts :
//
//   1. Au demarrage d'ArchiCAD, l'add-on ecrit la couche de traduction
//      (couche\sitecustomize.py, embarquee dans le .apx) dans
//      %APPDATA%\Tapir Translator.
//   2. Il met ce dossier en tete du PYTHONPATH d'ArchiCAD et pose
//      TAPIR_TRANSLATOR_DIR. Tapir garde ce PYTHONPATH pour chaque script :
//      Python importe la couche tout seul avant le script (un seul processus).
//   3. Le menu "Tapir Translator" ecrit la langue dans reglages.json ; la couche
//      le relit en direct, fenetres deja ouvertes comprises.
//
//   Chaque etape est tracee dans le Rapport de session ("Tapir Translator : ...").
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

// Langue de l'outil : celle d'ArchiCAD (code de version FRA, GER...) sauf choix dans le menu.
// Les menus sont enregistres une fois au demarrage dans la langue de ce moment (g_langueMenus) ;
// les messages suivent tout de suite un changement (g_langueMessages).
std::string g_versionArchicad;
int			g_langueMenus = 1;
int			g_langueMessages = 1;

// Menus generes (RFIX/Tapir_TranslatorMenus.grc) : BaseMenus (langue) + 0..4, aides a + DecalageAides
short kMenuEntree	  () { return TT::BaseMenus (g_langueMenus) + 0; }	// item i = TT::Langues[i - 1] (1 = detection automatique)
short kMenuSortie	  () { return TT::BaseMenus (g_langueMenus) + 1; }	// item i = TT::Langues[i - 1] (1 = aucune traduction)
short kMenuCommandes  () { return TT::BaseMenus (g_langueMenus) + 2; }
short kMenuTraducteur () { return TT::BaseMenus (g_langueMenus) + 3; }	// item i = TT::Fournisseurs[i - 1]
short kMenuInterface  () { return TT::BaseMenus (g_langueMenus) + 4; }	// 1 = comme ArchiCAD, i = TT::Langues[i - 1]
constexpr Int32 kAutomatique   = 1;
constexpr Int32 kCollecte	   = 2;
constexpr Int32 kOuvrirDossier = 4;
constexpr Int32 kEtat		   = 5;
constexpr Int32 kCles		   = 7;
constexpr Int32 kCorrections   = 8;

// Langue de l'outil d'apres les reglages ("" = celle d'ArchiCAD)
int LangueOutil (const std::string& choix)
{
	return TT::IndiceInterface (choix.empty () ? TT::LangueDepuisArchicad (g_versionArchicad) : choix);
}

std::wstring g_racine;
TT::Reglages g_reglages;
bool		 g_pret = false;	// couche et environnement en place

// Moteur (TapirTranslatorMoteur.exe) et sa surveillance : si le processus s'arrete alors que la
// traduction en ligne est voulue, un fil le relance (sans appel a l'API ArchiCAD : journal.txt).
std::mutex			g_verrou;
HANDLE				g_moteur = nullptr;
std::wstring		g_ligneMoteur;		// ligne de commande du moteur
std::wstring		g_dossierLancement;
std::atomic<bool>	g_moteurVoulu { false };
std::thread			g_veille;

GS::UniString Uni (const std::wstring& texte)
{
	return GS::UniString (TT::Utf8 (texte).c_str (), CC_UTF8);
}

GS::UniString Uni (const std::string& utf8)
{
	return GS::UniString (utf8.c_str (), CC_UTF8);
}

void Rapport (const GS::UniString& ligne)
{
	// %T : la ligne est passee telle quelle (accents, et '%' d'un chemin non interprete)
	const GS::UniString texte = GS::UniString ("Tapir Translator : ") + ligne;
	ACAPI_WriteReport ("%T", false, texte.ToPrintf ());
}

// Texte de l'interface dans la langue de l'outil
GS::UniString T (TT::Texte t)
{
	return Uni (std::string (TT::Txt (t, g_langueMessages)));
}

// Lignes techniques (diagnostic, erreurs systeme) : en francais, sinon en anglais
const char* FE (const char* fr, const char* en)
{
	return g_langueMessages == 0 ? fr : en;
}

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

GS::UniString Langues ()
{
	return NomLangue (g_reglages.entree, TT::M_AUTO) + " -> " + NomLangue (g_reglages.langue, TT::M_AUCUNE);
}

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

void RafraichirCoches ()
{
	for (int i = 0; i < TT::NombreLangues; ++i) {
		Cocher (kMenuEntree (), i + 1, g_reglages.entree == TT::Langues[i].code);
		Cocher (kMenuSortie (), i + 1, g_reglages.langue == TT::Langues[i].code);
	}
	Cocher (kMenuCommandes (), kAutomatique, g_reglages.moteur);
	Cocher (kMenuCommandes (), kCollecte, g_reglages.collecte);
	for (int i = 0; i < TT::NombreFournisseurs; ++i)
		Cocher (kMenuTraducteur (), i + 1, g_reglages.fournisseur == TT::Fournisseurs[i].code);
	for (int i = 0; i < TT::NombreLangues; ++i)
		Cocher (kMenuInterface (), i + 1, g_reglages.interface == TT::Langues[i].code);
}

// Ligne ajoutee a journal.txt (utilisable hors du fil principal, contrairement au Rapport de session)
void Journal (const std::string& ligne)
{
	if (g_racine.empty ())
		return;
	HANDLE f = CreateFileW (TT::Joindre (g_racine, L"journal.txt").c_str (), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
							nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return;
	SYSTEMTIME t;
	GetLocalTime (&t);
	char date[40];
	snprintf (date, sizeof (date), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	const std::string texte = std::string (date) + " [add-on] " + ligne + "\n";
	DWORD ecrit = 0;
	WriteFile (f, texte.data (), static_cast<DWORD> (texte.size ()), &ecrit, nullptr);
	CloseHandle (f);
}

GS::UniString NomFournisseur (const std::string& code)
{
	for (int i = 0; i < TT::NombreFournisseurs; ++i) {
		if (code == TT::Fournisseurs[i].code)
			return GS::UniString (TT::Fournisseurs[i].nom);
	}
	return Uni (code);
}

// Etat des cles (jamais la cle elle-meme) : "renseignee" si le champ attendu n'est pas vide
GS::UniString EtatCle (const std::string& code)
{
	std::string contenu;
	if (!TT::LireFichier (TT::CheminCles (g_racine), contenu))
		return FE ("cles.json absent", "cles.json missing");
	Json::Valeur v;
	if (!Json::Lecteur (contenu.compare (0, 3, "\xEF\xBB\xBF") == 0 ? contenu.substr (3) : contenu).Lire (v))
		return FE ("cles.json illisible (JSON invalide)", "cles.json unreadable (invalid JSON)");
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
//  Moteur de traduction hors ligne
// -----------------------------------------------------------------------------

// Chemin du .apx lui-meme (il porte le moteur et le modele, voir TT_Charge.hpp)
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

// Dossier d'extraction : %LOCALAPPDATA%\Tapir Translator\moteur\<empreinte>
std::wstring BaseExtraction ()
{
	std::wstring local = TT::LireVariable (L"LOCALAPPDATA");
	return local.empty () ? TT::Joindre (g_racine, L"moteur") : TT::Joindre (TT::Joindre (local, L"Tapir Translator"), L"moteur");
}

std::wstring g_dossierMoteur;	// dossier extrait (moteur + modele)

// Moteur embarque d'abord ; sinon (developpement) dossier "TapirTranslatorMoteur" a cote du .apx,
// puis %APPDATA%\Tapir Translator\moteur
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

// Extrait la charge du .apx (une seule fois par version)
void ExtraireMoteur ()
{
	if (!g_dossierMoteur.empty ())
		return;
	std::vector<std::wstring> journal;
	std::wstring dossier;
	const bool ok = TT::ExtraireCharge (CheminApx (), BaseExtraction (), dossier, journal);
	for (const std::wstring& l : journal)
		Rapport (Uni (l));
	if (!ok)
		return;
	g_dossierMoteur = dossier;
	// anciennes versions extraites (dont l'ancien moteur hors ligne de ~500 Mo) : supprimees,
	// sauf si un moteur y tourne encore (fichier verrouille : on reessaiera au prochain demarrage)
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

bool MoteurEnMarche ()
{
	std::lock_guard<std::mutex> verrou (g_verrou);
	return g_moteur != nullptr && WaitForSingleObject (g_moteur, 0) == WAIT_TIMEOUT;
}

// Demarre le processus avec g_ligneMoteur (appele sous g_verrou)
bool DemarrerProcessus (DWORD& erreur)
{
	std::vector<wchar_t> tampon (g_ligneMoteur.begin (), g_ligneMoteur.end ());
	tampon.push_back (L'\0');
	STARTUPINFOW si = {};
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
						 nullptr, g_dossierLancement.c_str (), &si, &pi)) {
		erreur = GetLastError ();
		return false;
	}
	CloseHandle (pi.hThread);
	// la fenetre "quota MyMemory" du moteur doit pouvoir passer au premier plan
	AllowSetForegroundWindow (pi.dwProcessId);
	if (g_moteur != nullptr)
		CloseHandle (g_moteur);
	g_moteur = pi.hProcess;
	return true;
}

// Fil de surveillance : relance le moteur s'il s'arrete (plantage, arret force...).
// Code 2 = port deja pris par le moteur d'un autre ArchiCAD : on reessaie plus tard.
void Veiller ()
{
	int relances = 0;
	ULONGLONG debutFenetre = GetTickCount64 ();
	while (g_moteurVoulu) {
		HANDLE h = nullptr;
		{
			std::lock_guard<std::mutex> verrou (g_verrou);
			h = g_moteur;
		}
		if (h != nullptr && WaitForSingleObject (h, 500) == WAIT_TIMEOUT)
			continue;
		if (!g_moteurVoulu)
			break;
		DWORD code = 0;
		if (h != nullptr)
			GetExitCodeProcess (h, &code);
		if (GetTickCount64 () - debutFenetre > 3600000ull) {
			debutFenetre = GetTickCount64 ();
			relances = 0;
		}
		if (relances >= 20) {
			Journal ("moteur arrete 20 fois en une heure : surveillance suspendue (Options > Traduction en ligne pour relancer)");
			return;
		}
		const DWORD attente = code == 2 ? 30000 : static_cast<DWORD> (1000 * (1 + relances));
		Journal ("moteur arrete (code " + std::to_string (code) + ") : relance dans " + std::to_string (attente / 1000) + " s");
		for (DWORD t = 0; t < attente && g_moteurVoulu; t += 250)
			Sleep (250);
		if (!g_moteurVoulu)
			break;
		std::lock_guard<std::mutex> verrou (g_verrou);
		DWORD erreur = 0;
		if (DemarrerProcessus (erreur))
			Journal ("moteur relance");
		else
			Journal ("relance du moteur impossible (erreur Windows " + std::to_string (erreur) + ")");
		++relances;
	}
}

void ArreterMoteur ()
{
	g_moteurVoulu = false;
	if (g_veille.joinable ())
		g_veille.join ();
	std::lock_guard<std::mutex> verrou (g_verrou);
	if (g_moteur != nullptr) {
		if (WaitForSingleObject (g_moteur, 0) == WAIT_TIMEOUT)
			TerminateProcess (g_moteur, 0);
		CloseHandle (g_moteur);
		g_moteur = nullptr;
		Rapport (T (TT::R_MOTEUR_ARRETE));
	}
}

void LancerMoteur ()
{
	if (MoteurEnMarche () && g_moteurVoulu)
		return;
	ArreterMoteur ();
	ExtraireMoteur ();
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
	const std::wstring dossier = exe.substr (0, exe.find_last_of (L"\\/"));
	std::wstring erreurCles;
	TT::CreerModeleCles (g_racine, erreurCles);
	std::wstring ligne = L"\"" + exe + L"\" --port " + std::to_wstring (TT::PortMoteur) + L" --cles \"" + TT::CheminCles (g_racine) +
						 L"\" --parent " + std::to_wstring (GetCurrentProcessId ()) + L" --journal \"" +
						 TT::Joindre (g_racine, L"journal.txt") + L"\" --interface " +
						 TT::DepuisUtf8 (TT::CodeInterface (g_langueMessages));
	// modele hors ligne seulement si le .apx en porte un (option de build TT_MODELE_LOCAL)
	const std::wstring modele = TT::Joindre (dossier, L"modele");
	if (TT::DossierExiste (modele))
		ligne += L" --modele \"" + modele + L"\"";
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
	g_moteurVoulu = true;
	g_veille = std::thread (Veiller);
	Rapport (Uni (TT::TxtF (TT::R_MOTEUR_LANCE, g_langueMessages,
							{ { "f", NomFournisseur (g_reglages.fournisseur).ToCStr (0, MaxUSize, CC_UTF8).Get () },
							  { "e", EtatCle (g_reglages.fournisseur).ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
}

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

void Enregistrer ()
{
	std::wstring erreur;
	if (g_reglages.entree == g_reglages.langue && !g_reglages.langue.empty ())
		Rapport (T (TT::R_MEME_LANGUE));
	if (TT::EcrireReglages (g_racine, g_reglages, erreur)) {
		const GS::UniString enLigne = g_reglages.moteur
			? T (TT::R_OUI) + " (" + NomFournisseur (g_reglages.fournisseur) + " : " + EtatCle (g_reglages.fournisseur) + ")"
			: T (TT::R_NON);
		Rapport (Uni (TT::TxtF (TT::R_REGLAGES, g_langueMessages,
								{ { "l", Langues ().ToCStr (0, MaxUSize, CC_UTF8).Get () },
								  { "o", enLigne.ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
	} else
		Rapport (GS::UniString (FE ("ERREUR ecriture reglages.json : ", "ERROR writing reglages.json: ")) + Uni (erreur));
	RafraichirCoches ();
}

void Demarrer ()
{
	g_racine = TT::DossierParDefaut ();
	if (g_racine.empty ()) {
		Rapport (FE ("ERREUR : variable APPDATA absente, la traduction est inactive.", "ERROR: APPDATA variable missing, translation is inactive."));
		return;
	}
	std::vector<std::wstring> journal;
	const bool couche = TT::ExtraireCouche (g_racine, journal);
	for (const std::wstring& ligne : journal)
		Rapport (Uni (ligne));

	std::wstring pythonPath;
	const bool env = TT::PreparerEnvironnement (g_racine, pythonPath);
	Rapport (GS::UniString ("PYTHONPATH = ") + Uni (pythonPath) + (env ? "" : "  (ERROR SetEnvironmentVariable)"));

	g_reglages = TT::LireReglages (g_racine);
	g_langueMessages = LangueOutil (g_reglages.interface);
	g_pret = couche && env;
	if (g_pret && g_reglages.moteur)
		LancerMoteur ();
	Rapport (Uni (TT::TxtF (g_pret ? TT::R_DEMARRAGE : TT::R_INACTIF, g_langueMessages,
							{ { "v", TT::VersionAddOn }, { "l", Langues ().ToCStr (0, MaxUSize, CC_UTF8).Get () } })));
}

void Etat ()
{
	const GS::UniString oui = T (TT::R_OUI), non = T (TT::R_NON);
	Rapport (GS::UniString ("=== ") + T (TT::M_ETAT) + " " + TT::VersionAddOn + " ===");
	Rapport (GS::UniString (FE ("dossier = ", "folder = ")) + Uni (g_racine) + (g_pret ? "" : FE ("  (couche NON installee)", "  (layer NOT installed)")));
	Rapport (GS::UniString ("PYTHONPATH = ") + Uni (TT::LireVariable (L"PYTHONPATH")));
	Rapport (GS::UniString ("TAPIR_TRANSLATOR_DIR = ") + Uni (TT::LireVariable (L"TAPIR_TRANSLATOR_DIR")));
	Rapport (GS::UniString (FE ("langues = ", "languages = ")) + Langues () + ", " + T (TT::M_COLLECTE) + " = " + (g_reglages.collecte ? oui : non));
	Rapport (T (TT::M_INTERFACE) + " = " + Uni (std::string (TT::NomInterface (g_langueMessages))) +
			 (g_reglages.interface.empty () ? GS::UniString (" (") + T (TT::M_INTERFACE_AUTO) + " : " + Uni (g_versionArchicad) + ")" : GS::UniString ()));
	for (int i = 0; i < TT::NombreFournisseurs; ++i)
		Rapport (GS::UniString ("  ") + (g_reglages.fournisseur == TT::Fournisseurs[i].code ? "[x] " : "    ") +
				 TT::Fournisseurs[i].nom + " : " + EtatCle (TT::Fournisseurs[i].code));
	Rapport (T (TT::M_EN_LIGNE) + " = " + (g_reglages.moteur ? oui : non) + ", " + FE ("moteur ", "engine ") +
			 (MoteurEnMarche () ? FE ("en marche", "running") : FE ("arrete", "stopped")) + " (port " +
			 GS::ValueToUniString (static_cast<Int32> (TT::PortMoteur)) + ")");
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
	for (const std::wstring& e : EmplacementsMoteur ())
		Rapport (GS::UniString ("  ") + (TT::FichierExiste (e) ? "[x] " : "[ ] ") + Uni (e));
	Rapport (GS::UniString ("sitecustomize.py = ") +
			 (TT::FichierExiste (TT::Joindre (TT::Joindre (g_racine, L"couche"), L"sitecustomize.py")) ? oui : non));

	const std::wstring dictionnaires = TT::Joindre (g_racine, L"dictionnaires");
	const std::vector<std::wstring> dicos = TT::ListerJson (dictionnaires);
	Rapport (GS::UniString (FE ("dictionnaires (", "dictionaries (")) + GS::ValueToUniString (static_cast<Int32> (dicos.size ())) + ") :");
	for (const std::wstring& d : dicos)
		Rapport (GS::UniString ("  ") + Uni (d));

	const std::wstring manquants = TT::Joindre (g_racine, L"manquants");
	const std::vector<std::wstring> fichiers = TT::ListerJson (manquants);
	Rapport (GS::UniString (FE ("textes manquants (", "missing texts (")) + GS::ValueToUniString (static_cast<Int32> (fichiers.size ())) + ") :");
	for (const std::wstring& f : fichiers) {
		std::string contenu;
		TT::LireFichier (TT::Joindre (manquants, f), contenu);
		Rapport (GS::UniString ("  ") + Uni (f) + " : " + GS::ValueToUniString (static_cast<Int32> (TT::CompterVides (contenu))));
	}

	const std::vector<std::string> lignes = TT::DernieresLignes (TT::Joindre (g_racine, L"journal.txt"), 15);
	Rapport (GS::UniString ("journal.txt (") + GS::ValueToUniString (static_cast<Int32> (lignes.size ())) + ") :");
	for (const std::string& l : lignes)
		Rapport (GS::UniString ("  ") + Uni (l));
}

void OuvrirDossier ()
{
	if (g_racine.empty ())
		return;
	TT::CreerDossier (g_racine);
	const HINSTANCE r = ShellExecuteW (nullptr, L"open", g_racine.c_str (), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR> (r) <= 32)
		Rapport (GS::UniString (FE ("ERREUR ouverture du dossier ", "ERROR opening the folder ")) + Uni (g_racine));
}

GSErrCode MenuCommandHandler (const API_MenuParams* menuParams)
{
	const short menu = menuParams->menuItemRef.menuResID;
	const Int32 index = menuParams->menuItemRef.itemIndex;
	const bool langue = index >= 1 && index <= TT::NombreLangues;
	if (menu == kMenuEntree () && langue) {
		g_reglages.entree = TT::Langues[index - 1].code;
		Enregistrer ();
	} else if (menu == kMenuSortie () && langue) {
		g_reglages.langue = TT::Langues[index - 1].code;
		Enregistrer ();
	} else if (menu == kMenuTraducteur ()) {
		if (index >= 1 && index <= TT::NombreFournisseurs) {
			g_reglages.fournisseur = TT::Fournisseurs[index - 1].code;
			// choisir un traducteur active la traduction en ligne
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
		// 1 = comme ArchiCAD ("") ; messages et fenetre du moteur tout de suite, menus au prochain demarrage
		g_reglages.interface = TT::Langues[index - 1].code;
		g_langueMessages = LangueOutil (g_reglages.interface);
		std::wstring erreur;
		TT::EcrireReglages (g_racine, g_reglages, erreur);
		Rapport (Uni (TT::TxtF (TT::R_INTERFACE, g_langueMessages, { { "l", TT::NomInterface (g_langueMessages) } })));
		RafraichirCoches ();
		if (g_moteurVoulu) {
			ArreterMoteur ();
			LancerMoteur ();	// le moteur reprend la langue (fenetre du quota MyMemory)
		}
	} else if (menu == kMenuCommandes ()) {
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
//  Fonctions requises par l'API
// =============================================================================

API_AddonType CheckEnvironment (API_EnvirParams* envir)
{
	RSGetIndString (&envir->addOnInfo.name, 32000, 1, ACAPI_GetOwnResModule ());
	RSGetIndString (&envir->addOnInfo.description, 32000, 2, ACAPI_GetOwnResModule ());
	envir->addOnInfo.description += GS::UniString (" (") + TT::VersionAddOn + ")";
	// langue de la version d'ArchiCAD (FRA, GER, INT...) : langue de l'outil par defaut
	g_versionArchicad = envir->serverInfo.language.ToCStr (0, MaxUSize, CC_UTF8).Get ();
	// Precharge : l'environnement doit etre en place avant le premier script Tapir.
	return APIAddon_Preload;
}


GSErrCode RegisterInterface (void)
{
	// langue des menus : choix enregistre, sinon celle d'ArchiCAD (fixee jusqu'au prochain demarrage)
	const std::wstring racine = TT::DossierParDefaut ();
	const TT::Reglages r = racine.empty () ? TT::Reglages () : TT::LireReglages (racine);
	g_langueMenus = LangueOutil (r.interface);
	g_langueMessages = g_langueMenus;
	// Cinq menus sous le meme titre "Tapir Translator" : ArchiCAD les reunit en un menu principal
	GSErrCode err = NoError;
	err |= ACAPI_MenuItem_RegisterMenu (kMenuEntree (), kMenuEntree () + TT::DecalageAides, MenuCode_UserDef, MenuFlag_InsertIntoSame);
	err |= ACAPI_MenuItem_RegisterMenu (kMenuSortie (), kMenuSortie () + TT::DecalageAides, MenuCode_UserDef, MenuFlag_InsertIntoSame);
	err |= ACAPI_MenuItem_RegisterMenu (kMenuTraducteur (), kMenuTraducteur () + TT::DecalageAides, MenuCode_UserDef, MenuFlag_InsertIntoSame);
	err |= ACAPI_MenuItem_RegisterMenu (kMenuCommandes (), kMenuCommandes () + TT::DecalageAides, MenuCode_UserDef,
										MenuFlag_SeparatorBefore | MenuFlag_InsertIntoSame);
	err |= ACAPI_MenuItem_RegisterMenu (kMenuInterface (), kMenuInterface () + TT::DecalageAides, MenuCode_UserDef, MenuFlag_InsertIntoSame);
	return err;
}


GSErrCode Initialize (void)
{
	GSErrCode err = NoError;
	for (const short menu : { kMenuEntree (), kMenuSortie (), kMenuTraducteur (), kMenuCommandes (), kMenuInterface () }) {
		const GSErrCode e = ACAPI_MenuItem_InstallMenuHandler (menu, MenuCommandHandler);
		if (e != NoError) {
			Rapport (GS::UniString ("ERREUR ACAPI_MenuItem_InstallMenuHandler (") + GS::ValueToUniString (static_cast<Int32> (menu)) +
					 ") = " + GS::ValueToUniString (static_cast<Int32> (e)));
			err = e;
		}
	}
	Demarrer ();
	RafraichirCoches ();
	ACAPI_KeepInMemory (true);
	return err;
}


GSErrCode FreeData (void)
{
	ArreterMoteur ();
	return NoError;
}
