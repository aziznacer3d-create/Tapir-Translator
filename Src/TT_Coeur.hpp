// *****************************************************************************
// Tapir Translator - ArchiCAD-independent core (Win32 + standard library only)
//
//   ROLE IN THE SYSTEM
//   Everything the add-on needs that does not involve the ArchiCAD API:
//   language and translator tables, settings file (reglages.json), user files
//   (cles.json, corrections.json, _perso.json), extraction of the translation
//   layer and of the common dictionary, preparation of the PYTHONPATH, and
//   small file helpers.
//
//   It is shared by the add-on (Src/AddOnMain.cpp) and by the test bench
//   (Tests/BancTraducteur.cpp): the bench exercises exactly the code that writes
//   the layer, the settings and the environment inside ArchiCAD.
//
//   It only relies on header-only parts of the STL (std::string / std::wstring)
//   and on the Win32 API: the add-on is compiled with /Zc:wchar_t- (wchar_t is
//   not a native type), so compiled STL functions taking a wchar_t would not link.
// *****************************************************************************

#ifndef TT_COEUR_HPP
#define TT_COEUR_HPP

#include <string>
#include <vector>

namespace TT {

// Version of the add-on (shown in the Add-On Manager, the Session Report and reglages.json).
extern const char* const VersionAddOn;

// One language of the menus: its code (as used in dictionaries and reglages.json) and its
// name written in that language.
struct Langue {
	const char* code;	// "" = the script's own language (automatic / no translation / as ArchiCAD)
	const char* nom;
};
// The 21 entries: "" first, then the 20 ArchiCAD 29 languages (see TT_Coeur.cpp).
extern const Langue Langues[];
extern const int NombreLangues;

// In-memory image of reglages.json, the file shared by the add-on (writer) and the Python layer
// (reader, live). Default values are those used when the file or a key is missing.
struct Reglages {
	std::string entree;		// language the scripts are written in, code of Langues[], "" = automatic detection
	std::string langue;		// output language, code of Langues[], "" = no translation
	bool collecte = false;	// record the texts that have no translation (manquants folder)
	bool moteur = false;	// online translation of the texts missing from the dictionaries
	std::string fournisseur = "mymemory";	// code of Fournisseurs[]
	std::string interface;	// language of the tool itself (code of Langues[]); "" = ArchiCAD's language
};

// Online translators (same order as the "Online translator" menu, BaseMenus + 3, generated
// into RFIX/Tapir_TranslatorMenus.grc).
struct Fournisseur {
	const char* code;		// identifier in reglages.json and cles.json
	const char* nom;		// display name
	const char* besoin;		// what has to be filled in cles.json
};
extern const Fournisseur Fournisseurs[];
extern const int NombreFournisseurs;
// True for the codes of Fournisseurs[], plus "essai" (test bench) and "local" (offline build option).
bool FournisseurConnu (const std::string& code);

// TCP port of the local translation engine (127.0.0.1).
constexpr int PortMoteur = 8779;

// cles.json: credentials of the online translators. CheminCles returns <racine>\cles.json;
// CreerModeleCles writes an empty template if the file does not exist (never overwritten).
std::wstring CheminCles (const std::wstring& racine);
bool CreerModeleCles (const std::wstring& racine, std::wstring& erreur);

// corrections.json: terms to correct in online translations ("story" -> "étage", not
// "histoire"). CreerModeleCorrections writes a pre-filled file if none exists (never overwritten).
std::wstring CheminCorrections (const std::wstring& racine);
bool CreerModeleCorrections (const std::wstring& racine, std::wstring& erreur);

// Working folder %APPDATA%\Tapir Translator ("" if APPDATA does not exist).
std::wstring DossierParDefaut ();

// Small file helpers (wide paths, Win32).
// Joindre: joins a folder and a name with a backslash (unless the folder already ends with one).
std::wstring Joindre (const std::wstring& dossier, const std::wstring& nom);
bool		 FichierExiste (const std::wstring& chemin);	// existing file (not a folder)
bool		 DossierExiste (const std::wstring& chemin);	// existing folder
bool		 CreerDossier (const std::wstring& chemin);	// creates the parents too
bool		 LireFichier (const std::wstring& chemin, std::string& contenu);	// whole file, raw bytes
// Atomic write (temporary .tmp file, then replacement): the Python layer, which re-reads the
// files live, never sees a half-written file. 'erreur' receives a readable message on failure.
bool		 EcrireFichier (const std::wstring& chemin, const std::string& contenu, std::wstring& erreur);

// UTF-16 <-> UTF-8 conversions (empty string on failure).
std::string	 Utf8 (const std::wstring& texte);
std::wstring DepuisUtf8 (const std::string& texte);

// Installs the translation layer and the data files under 'racine':
//   - couche\sitecustomize.py: always the add-on's version (rewritten when it differs);
//   - dictionnaires\_commun.json: the add-on's version too (rewritten when it differs);
//   - dictionnaires\_perso.json, cles.json, corrections.json: created only if missing.
// 'journal' receives one line per action. Returns false if a folder or a mandatory file failed.
bool ExtraireCouche (const std::wstring& racine, std::vector<std::wstring>& journal);

// Reads / writes <racine>\reglages.json. LireReglages never fails: missing or invalid values
// keep their defaults. EcrireReglages refuses unknown language codes.
Reglages LireReglages (const std::wstring& racine);
bool	 EcrireReglages (const std::wstring& racine, const Reglages& reglages, std::wstring& erreur);

// Puts <racine>\couche at the head of the PYTHONPATH of the current process (ArchiCAD) and sets
// TAPIR_TRANSLATOR_DIR. Tapir keeps this PYTHONPATH for every script it launches (it prepends
// its own helpers), so Python imports the layer by itself. 'pythonPath' receives the new value.
bool PreparerEnvironnement (const std::wstring& racine, std::wstring& pythonPath);

// Value of an environment variable of the current process ("" if it is not set).
std::wstring LireVariable (const wchar_t* nom);

// Names of the .json files of a folder (no path).
std::vector<std::wstring> ListerJson (const std::wstring& dossier);

// Deletes a folder and all its content, recursively (no confirmation).
void SupprimerDossier (const std::wstring& dossier);

// Names of the sub-folders of a folder (no path).
std::vector<std::wstring> ListerDossiers (const std::wstring& dossier);

// Number of texts still empty ("text": "") in a file of the manquants folder.
int CompterVides (const std::string& json);

// The last 'nombre' non-empty lines of a text file.
std::vector<std::string> DernieresLignes (const std::wstring& chemin, int nombre);

}	// namespace TT

#endif
