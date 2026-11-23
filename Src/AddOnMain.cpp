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

#include "TT_Coeur.hpp"

#include <shellapi.h>

namespace {

constexpr short kMenuResID   = 32500;
constexpr short kPromptResID = 32600;

// Index du STR# 32500 (RFIX/Tapir_TranslatorFix.grc) : garder alignes
constexpr Int32 kPremiereLangue	= 1;	// 1..6 = TT::Langues[0..5]
constexpr Int32 kCollecte		= 8;
constexpr Int32 kOuvrirDossier	= 10;
constexpr Int32 kEtat			= 11;

std::wstring g_racine;
TT::Reglages g_reglages;
bool		 g_pret = false;	// couche et environnement en place

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

GS::UniString NomLangue (const std::string& code)
{
	for (int i = 0; i < TT::NombreLangues; ++i) {
		if (code == TT::Langues[i].code)
			return i == 0 ? GS::UniString ("langue d'origine (aucune traduction)") : Uni (std::string (TT::Langues[i].nom)) + " (" + Uni (code) + ")";
	}
	return Uni (code);
}

void Cocher (Int32 index, bool coche)
{
	API_MenuItemRef ref = {};
	ref.menuResID = kMenuResID;
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
	for (int i = 0; i < TT::NombreLangues; ++i)
		Cocher (kPremiereLangue + i, g_reglages.langue == TT::Langues[i].code);
	Cocher (kCollecte, g_reglages.collecte);
}

void Enregistrer ()
{
	std::wstring erreur;
	if (TT::EcrireReglages (g_racine, g_reglages, erreur))
		Rapport (GS::UniString ("langue = ") + NomLangue (g_reglages.langue) + ", collecte des textes non traduits = " +
				 (g_reglages.collecte ? "oui" : "non") + " (reglages.json ecrit ; les scripts ouverts suivent en moins d'une seconde)");
	else
		Rapport (GS::UniString ("ERREUR ecriture reglages.json : ") + Uni (erreur));
	RafraichirCoches ();
}

void Demarrer ()
{
	g_racine = TT::DossierParDefaut ();
	if (g_racine.empty ()) {
		Rapport ("ERREUR : variable APPDATA absente, la traduction est inactive.");
		return;
	}
	std::vector<std::wstring> journal;
	const bool couche = TT::ExtraireCouche (g_racine, journal);
	for (const std::wstring& ligne : journal)
		Rapport (Uni (ligne));

	std::wstring pythonPath;
	const bool env = TT::PreparerEnvironnement (g_racine, pythonPath);
	Rapport (GS::UniString ("PYTHONPATH d'ArchiCAD = ") + Uni (pythonPath) + (env ? "" : "  (ERREUR SetEnvironmentVariable)"));

	g_reglages = TT::LireReglages (g_racine);
	g_pret = couche && env;
	Rapport (GS::UniString ("version ") + TT::VersionAddOn + (g_pret ? ", active" : ", INACTIVE (voir les erreurs ci-dessus)") +
			 ", langue = " + NomLangue (g_reglages.langue));
}

void Etat ()
{
	Rapport (GS::UniString ("=== etat, version ") + TT::VersionAddOn + " ===");
	Rapport (GS::UniString ("dossier = ") + Uni (g_racine) + (g_pret ? "" : "  (couche NON installee)"));
	Rapport (GS::UniString ("PYTHONPATH actuel = ") + Uni (TT::LireVariable (L"PYTHONPATH")));
	Rapport (GS::UniString ("TAPIR_TRANSLATOR_DIR = ") + Uni (TT::LireVariable (L"TAPIR_TRANSLATOR_DIR")));
	Rapport (GS::UniString ("langue = ") + NomLangue (g_reglages.langue) + ", collecte = " + (g_reglages.collecte ? "oui" : "non"));
	Rapport (GS::UniString ("couche presente = ") +
			 (TT::FichierExiste (TT::Joindre (TT::Joindre (g_racine, L"couche"), L"sitecustomize.py")) ? "oui" : "NON"));

	const std::wstring dictionnaires = TT::Joindre (g_racine, L"dictionnaires");
	const std::vector<std::wstring> dicos = TT::ListerJson (dictionnaires);
	Rapport (GS::UniString ("dictionnaires (") + GS::ValueToUniString (static_cast<Int32> (dicos.size ())) + ") :");
	for (const std::wstring& d : dicos)
		Rapport (GS::UniString ("  ") + Uni (d));

	const std::wstring manquants = TT::Joindre (g_racine, L"manquants");
	const std::vector<std::wstring> fichiers = TT::ListerJson (manquants);
	Rapport (GS::UniString ("textes manquants (") + GS::ValueToUniString (static_cast<Int32> (fichiers.size ())) + " fichier(s)) :");
	for (const std::wstring& f : fichiers) {
		std::string contenu;
		TT::LireFichier (TT::Joindre (manquants, f), contenu);
		Rapport (GS::UniString ("  ") + Uni (f) + " : " + GS::ValueToUniString (static_cast<Int32> (TT::CompterVides (contenu))) + " texte(s) a traduire");
	}

	const std::vector<std::string> lignes = TT::DernieresLignes (TT::Joindre (g_racine, L"journal.txt"), 15);
	Rapport (GS::UniString ("journal de la couche (") + GS::ValueToUniString (static_cast<Int32> (lignes.size ())) + " dernieres lignes) :");
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
		Rapport (GS::UniString ("ERREUR ouverture du dossier ") + Uni (g_racine));
}

GSErrCode MenuCommandHandler (const API_MenuParams* menuParams)
{
	if (menuParams->menuItemRef.menuResID != kMenuResID)
		return NoError;
	const Int32 index = menuParams->menuItemRef.itemIndex;
	if (index >= kPremiereLangue && index < kPremiereLangue + TT::NombreLangues) {
		g_reglages.langue = TT::Langues[index - kPremiereLangue].code;
		Enregistrer ();
	} else if (index == kCollecte) {
		g_reglages.collecte = !g_reglages.collecte;
		Enregistrer ();
	} else if (index == kOuvrirDossier) {
		OuvrirDossier ();
	} else if (index == kEtat) {
		Etat ();
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
	// Precharge : l'environnement doit etre en place avant le premier script Tapir.
	return APIAddon_Preload;
}


GSErrCode RegisterInterface (void)
{
	return ACAPI_MenuItem_RegisterMenu (kMenuResID, kPromptResID, MenuCode_UserDef, MenuFlag_Default);
}


GSErrCode Initialize (void)
{
	const GSErrCode err = ACAPI_MenuItem_InstallMenuHandler (kMenuResID, MenuCommandHandler);
	if (err != NoError)
		Rapport (GS::UniString ("ERREUR ACAPI_MenuItem_InstallMenuHandler = ") + GS::ValueToUniString (static_cast<Int32> (err)));
	Demarrer ();
	RafraichirCoches ();
	ACAPI_KeepInMemory (true);
	return err;
}


GSErrCode FreeData (void)
{
	return NoError;
}
