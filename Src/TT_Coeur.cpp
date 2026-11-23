// *****************************************************************************
// Tapir Translator - ArchiCAD-independent core (implementation, see TT_Coeur.hpp)
//
//   ROLE IN THE SYSTEM
//   Implements the language/translator tables, the Win32 file and environment
//   helpers, the installation of the translation layer and its data files, the
//   reglages.json reader/writer, the user file templates (cles.json,
//   corrections.json, _perso.json) and the PYTHONPATH preparation. Linked both
//   into the add-on and into the test bench, so it never calls ArchiCAD.
// *****************************************************************************

#include "TT_Coeur.hpp"
#include "TT_Embarque.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

namespace TT {

const char* const VersionAddOn = "0.7.4";

// Same order as the items of the input / output language menus (BaseMenus + 0 / + 1, generated
// into RFIX/Tapir_TranslatorMenus.grc) and as the columns of dictionnaires/_commun.json.
// Entry 0 (empty code) is the "automatic detection" / "no translation" / "as ArchiCAD" item.
const Langue Langues[] = {
	{ "",	"aucune" },
	{ "fr",	"Français" },
	{ "en",	"English" },
	{ "de",	"Deutsch" },
	{ "es",	"Español" },
	{ "pt",	"Português" },
	{ "it",	"Italiano" },
	{ "hu",	"Magyar" },
	{ "cs",	"Čeština" },
	{ "pl",	"Polski" },
	{ "ru",	"Русский" },
	{ "uk",	"Українська" },
	{ "el",	"Ελληνικά" },
	{ "tr",	"Türkçe" },
	{ "nl",	"Nederlands" },
	{ "sv",	"Svenska" },
	{ "no",	"Norsk" },
	{ "fi",	"Suomi" },
	{ "ja",	"日本語" },
	{ "ko",	"한국어" },
	{ "zh",	"中文" },
};
const int NombreLangues = static_cast<int> (sizeof (Langues) / sizeof (Langues[0]));

// Online translators: code, display name, what cles.json must contain (in menu order).
const Fournisseur Fournisseurs[] = {
	{ "deepl",			"DeepL",					"cle (compte DeepL API Free)" },
	{ "microsoft",		"Microsoft Translator",		"cle + region (Azure, niveau gratuit F0)" },
	{ "google",			"Google Cloud Translation",	"cle (Google Cloud)" },
	{ "mymemory",		"MyMemory",					"rien (e-mail facultatif pour un quota plus grand)" },
	{ "libretranslate",	"LibreTranslate",			"adresse du serveur (+ cle si demandee)" },
};
const int NombreFournisseurs = static_cast<int> (sizeof (Fournisseurs) / sizeof (Fournisseurs[0]));

// Validates a translator code read from reglages.json. Besides the menu entries, it accepts
// "essai" (the fake engine of the test bench) and "local" (offline model, build option).
bool FournisseurConnu (const std::string& code)
{
	if (code == "essai" || code == "local")
		return true;
	for (int i = 0; i < NombreFournisseurs; ++i) {
		if (code == Fournisseurs[i].code)
			return true;
	}
	return false;
}


// =============================================================================
//  Text conversions
// =============================================================================

// UTF-16 -> UTF-8. Two-pass Win32 conversion: first ask for the required size, then convert.
// Returns "" for an empty input or on failure.
std::string Utf8 (const std::wstring& texte)
{
	if (texte.empty ())
		return std::string ();
	const int n = WideCharToMultiByte (CP_UTF8, 0, texte.c_str (), static_cast<int> (texte.size ()), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return std::string ();
	std::string res (static_cast<size_t> (n), '\0');
	WideCharToMultiByte (CP_UTF8, 0, texte.c_str (), static_cast<int> (texte.size ()), &res[0], n, nullptr, nullptr);
	return res;
}


// UTF-8 -> UTF-16, same two-pass scheme as Utf8. Returns L"" for an empty input or on failure.
std::wstring DepuisUtf8 (const std::string& texte)
{
	if (texte.empty ())
		return std::wstring ();
	const int n = MultiByteToWideChar (CP_UTF8, 0, texte.c_str (), static_cast<int> (texte.size ()), nullptr, 0);
	if (n <= 0)
		return std::wstring ();
	std::wstring res (static_cast<size_t> (n), L'\0');
	MultiByteToWideChar (CP_UTF8, 0, texte.c_str (), static_cast<int> (texte.size ()), &res[0], n);
	return res;
}


// Builds a readable message for a Win32 error code: "erreur Windows <code> (<system text>)".
// The system text comes from FormatMessageW (which allocates the buffer, freed here) with its
// trailing line breaks and spaces removed.
static std::wstring MessageErreur (DWORD code)
{
	wchar_t* tampon = nullptr;
	const DWORD n = FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
									nullptr, code, 0, reinterpret_cast<wchar_t*> (&tampon), 0, nullptr);
	std::wstring res = L"erreur Windows " + std::to_wstring (static_cast<unsigned long> (code));
	if (n > 0 && tampon != nullptr) {
		std::wstring msg (tampon, n);
		while (!msg.empty () && (msg.back () == L'\n' || msg.back () == L'\r' || msg.back () == L' '))
			msg.pop_back ();
		res += L" (" + msg + L")";
	}
	if (tampon != nullptr)
		LocalFree (tampon);
	return res;
}


// =============================================================================
//  Files and environment variables
// =============================================================================

// Reads an environment variable of the current process. Returns "" if it is not set.
std::wstring LireVariable (const wchar_t* nom)
{
	// Step 1: ask for the required size (in characters, terminating zero included).
	const DWORD n = GetEnvironmentVariableW (nom, nullptr, 0);
	if (n == 0)
		return std::wstring ();
	// Step 2: read it; a result of 0 or larger than the buffer means the variable vanished or
	// grew in between, which is treated as "not set".
	std::vector<wchar_t> tampon (n + 1, L'\0');
	const DWORD lu = GetEnvironmentVariableW (nom, tampon.data (), n + 1);
	if (lu == 0 || lu > n)
		return std::wstring ();
	return std::wstring (tampon.data (), lu);
}


// Working folder of the add-on, the layer and the engine: %APPDATA%\Tapir Translator.
// Returns "" when APPDATA is not defined.
std::wstring DossierParDefaut ()
{
	const std::wstring appData = LireVariable (L"APPDATA");
	if (appData.empty ())
		return std::wstring ();
	return Joindre (appData, L"Tapir Translator");
}


// Joins a folder and a name. No separator is added if the folder already ends with '\' or '/';
// an empty folder returns the name alone.
std::wstring Joindre (const std::wstring& dossier, const std::wstring& nom)
{
	if (dossier.empty ())
		return nom;
	const wchar_t fin = dossier.back ();
	if (fin == L'\\' || fin == L'/')
		return dossier + nom;
	return dossier + L"\\" + nom;
}


// True if 'chemin' exists and is a file (not a folder).
bool FichierExiste (const std::wstring& chemin)
{
	const DWORD a = GetFileAttributesW (chemin.c_str ());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


// True if 'chemin' exists and is a folder.
bool DossierExiste (const std::wstring& chemin)
{
	const DWORD a = GetFileAttributesW (chemin.c_str ());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}


// Creates a folder and, recursively, its missing parents (like "mkdir -p").
// Returns true if the folder exists at the end.
bool CreerDossier (const std::wstring& chemin)
{
	if (chemin.empty ())
		return false;
	if (DossierExiste (chemin))
		return true;
	// Create the parent first. "sep > 2" stops the recursion at the drive root ("C:\").
	const size_t sep = chemin.find_last_of (L"\\/");
	if (sep != std::wstring::npos && sep > 2)
		CreerDossier (chemin.substr (0, sep));
	return CreateDirectoryW (chemin.c_str (), nullptr) != 0 || GetLastError () == ERROR_ALREADY_EXISTS;
}


// Reads a whole file as raw bytes into 'contenu' (cleared first). The file is opened with full
// sharing, because the layer, the engine or Notepad may have it open at the same time.
// Returns false if the file cannot be opened or a read fails.
bool LireFichier (const std::wstring& chemin, std::string& contenu)
{
	contenu.clear ();
	HANDLE f = CreateFileW (chemin.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	char tampon[16384];
	DWORD lu = 0;
	bool ok = true;
	for (;;) {
		if (!ReadFile (f, tampon, sizeof (tampon), &lu, nullptr)) {
			ok = false;
			break;
		}
		if (lu == 0)
			break;
		contenu.append (tampon, lu);
	}
	CloseHandle (f);
	return ok;
}


// Atomic write of 'contenu' to 'chemin' (see TT_Coeur.hpp). On failure 'erreur' receives a
// readable message and the temporary file is removed.
bool EcrireFichier (const std::wstring& chemin, const std::string& contenu, std::wstring& erreur)
{
	// Step 1: write everything into "<chemin>.tmp", in chunks of at most 1 MB.
	const std::wstring tmp = chemin + L".tmp";
	HANDLE f = CreateFileW (tmp.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		erreur = L"creation de " + tmp + L" impossible : " + MessageErreur (GetLastError ());
		return false;
	}
	size_t ecrit = 0;
	bool ok = true;
	while (ecrit < contenu.size ()) {
		const size_t reste = contenu.size () - ecrit;
		const DWORD morceau = static_cast<DWORD> (reste > 1048576 ? 1048576 : reste);
		DWORD fait = 0;
		if (!WriteFile (f, contenu.data () + ecrit, morceau, &fait, nullptr) || fait == 0) {
			ok = false;
			break;
		}
		ecrit += fait;
	}
	// Step 2: keep the error code before CloseHandle can overwrite it, then close.
	const DWORD codeEcriture = ok ? 0 : GetLastError ();
	CloseHandle (f);
	if (!ok) {
		erreur = L"ecriture de " + tmp + L" impossible : " + MessageErreur (codeEcriture);
		DeleteFileW (tmp.c_str ());
		return false;
	}
	// Step 3: replace the target with the temporary file. The target may be open for reading
	// for a moment by the Python layer, so the move is retried (50 times, 20 ms apart = ~1 s).
	for (int essai = 0; essai < 50; ++essai) {
		if (MoveFileExW (tmp.c_str (), chemin.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		Sleep (20);
	}
	erreur = L"remplacement de " + chemin + L" impossible : " + MessageErreur (GetLastError ());
	DeleteFileW (tmp.c_str ());
	return false;
}


// Names (without path) of the *.json files of a folder; empty if the folder does not exist.
std::vector<std::wstring> ListerJson (const std::wstring& dossier)
{
	std::vector<std::wstring> res;
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Joindre (dossier, L"*.json").c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return res;
	do {
		if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			res.push_back (d.cFileName);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	return res;
}


// Deletes a folder recursively: files first (read-only attribute cleared), sub-folders by
// recursion, then the folder itself. Errors are ignored: a file in use (e.g. a running engine)
// simply stays, and the caller checks DossierExiste afterwards.
void SupprimerDossier (const std::wstring& dossier)
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Joindre (dossier, L"*").c_str (), &d);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring nom = d.cFileName;
			if (nom == L"." || nom == L"..")
				continue;
			const std::wstring chemin = Joindre (dossier, nom);
			if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				SupprimerDossier (chemin);
			else {
				SetFileAttributesW (chemin.c_str (), FILE_ATTRIBUTE_NORMAL);
				DeleteFileW (chemin.c_str ());
			}
		} while (FindNextFileW (h, &d));
		FindClose (h);
	}
	RemoveDirectoryW (dossier.c_str ());
}


// Names (without path) of the direct sub-folders of a folder, "." and ".." excluded.
std::vector<std::wstring> ListerDossiers (const std::wstring& dossier)
{
	std::vector<std::wstring> res;
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Joindre (dossier, L"*").c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return res;
	do {
		const std::wstring nom = d.cFileName;
		if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nom != L"." && nom != L"..")
			res.push_back (nom);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	return res;
}


// Counts the untranslated entries of a "manquants" file (see TT_Coeur.hpp).
int CompterVides (const std::string& json)
{
	// The manquants files are written by the layer (json.dump, indent=2), so an empty entry
	// always reads exactly  "...": ""  and a simple text search is enough.
	int n = 0;
	size_t pos = 0;
	const std::string motif = "\": \"\"";
	while ((pos = json.find (motif, pos)) != std::string::npos) {
		++n;
		pos += motif.size ();
	}
	return n;
}


// Returns the last 'nombre' non-empty lines of a text file (used to show the end of
// journal.txt). Lines are split on '\n'; a trailing '\r' is removed.
std::vector<std::string> DernieresLignes (const std::wstring& chemin, int nombre)
{
	std::vector<std::string> lignes;
	std::string contenu;
	if (!LireFichier (chemin, contenu))
		return lignes;
	// Step 1: split the whole file into non-empty lines.
	size_t debut = 0;
	while (debut < contenu.size ()) {
		size_t fin = contenu.find ('\n', debut);
		if (fin == std::string::npos)
			fin = contenu.size ();
		std::string ligne = contenu.substr (debut, fin - debut);
		if (!ligne.empty () && ligne.back () == '\r')
			ligne.pop_back ();
		if (!ligne.empty ())
			lignes.push_back (ligne);
		debut = fin + 1;
	}
	// Step 2: keep only the last 'nombre' lines.
	if (static_cast<int> (lignes.size ()) > nombre)
		lignes.erase (lignes.begin (), lignes.end () - nombre);
	return lignes;
}


// =============================================================================
//  Translation layer and dictionaries
// =============================================================================

// Installs the translation layer and the data files (see TT_Coeur.hpp). Called at every
// ArchiCAD start; files are only rewritten when their content differs.
bool ExtraireCouche (const std::wstring& racine, std::vector<std::wstring>& journal)
{
	// Step 1: create the folder tree: <racine>, couche, dictionnaires, manquants.
	bool ok = true;
	const std::wstring couche = Joindre (racine, L"couche");
	const std::wstring dictionnaires = Joindre (racine, L"dictionnaires");
	for (const std::wstring& d : { racine, couche, dictionnaires, Joindre (racine, L"manquants") }) {
		if (!CreerDossier (d)) {
			journal.push_back (L"dossier impossible a creer : " + d);
			ok = false;
		}
	}
	if (!ok)
		return false;

	// Step 2: couche\sitecustomize.py, embedded in the add-on (TT_Embarque.hpp). It must always be
	// the add-on's own version, so it is rewritten whenever the file on disk differs.
	std::wstring erreur;
	const std::wstring sitecustomize = Joindre (couche, L"sitecustomize.py");
	const std::string source (reinterpret_cast<const char*> (Embarque::Couche), Embarque::CoucheTaille);
	std::string actuel;
	if (LireFichier (sitecustomize, actuel) && actuel == source) {
		journal.push_back (L"couche a jour : " + sitecustomize);
	} else if (EcrireFichier (sitecustomize, source, erreur)) {
		journal.push_back (L"couche ecrite : " + sitecustomize);
	} else {
		journal.push_back (erreur);
		ok = false;
	}

	// Step 3: dictionnaires\_commun.json belongs to the add-on version: always replaced when it
	// differs. The user's own additions go into _perso.json (never touched, higher priority).
	const std::wstring commun = Joindre (dictionnaires, L"_commun.json");
	const std::string contenu (reinterpret_cast<const char*> (Embarque::Commun), Embarque::CommunTaille);
	std::string existant;
	if (LireFichier (commun, existant) && existant == contenu) {
		journal.push_back (L"dictionnaire commun a jour : " + commun);
	} else if (EcrireFichier (commun, contenu, erreur)) {
		journal.push_back (L"dictionnaire commun ecrit : " + commun);
	} else {
		journal.push_back (erreur);
		ok = false;
	}
	// Step 4: dictionnaires\_perso.json, created once with an explanation and one example line
	// (multilingual format 2). A failure here does not make the installation fail.
	const std::wstring perso = Joindre (dictionnaires, L"_perso.json");
	if (!FichierExiste (perso)) {
		const std::string modele =
			"{\n"
			"  \"_explication\": \"Vos traductions communes a tous les scripts. Jamais modifie par l'add-on ; l'emporte sur _commun.json. Une notion par ligne, une colonne par langue (codes : fr en de es pt it hu cs pl ru uk el tr nl sv no fi ja ko zh).\",\n"
			"  \"format\": 2,\n"
			"  \"termes\": [\n"
			"    {\"en\": \"Example\", \"fr\": \"Exemple\", \"de\": \"Beispiel\"}\n"
			"  ]\n"
			"}\n";
		if (EcrireFichier (perso, modele, erreur))
			journal.push_back (L"dictionnaire personnel cree : " + perso);
	}
	// Step 5: templates of cles.json and corrections.json, only if missing (also non-fatal).
	if (!FichierExiste (CheminCles (racine)) && CreerModeleCles (racine, erreur))
		journal.push_back (L"fichier des cles cree : " + CheminCles (racine));
	if (!FichierExiste (CheminCorrections (racine)) && CreerModeleCorrections (racine, erreur))
		journal.push_back (L"fichier des corrections cree : " + CheminCorrections (racine));
	return ok;
}


// =============================================================================
//  Settings (reglages.json, re-read live by the layer)
//  The file is small and written only by EcrireReglages, so it is read with a minimal
//  key search instead of a full JSON parser (this core has no dependency).
// =============================================================================

// True if 'code' is one of the codes of Langues[] (the empty code included).
static bool LangueConnue (const std::string& code)
{
	for (int i = 0; i < NombreLangues; ++i) {
		if (code == Langues[i].code)
			return true;
	}
	return false;
}


// Finds the value of a key in a flat JSON text: returns the position of the first non-blank
// character after  "cle" :  or std::string::npos if the key (or its colon) is not found.
// Only the first occurrence of the key is considered.
static size_t ApresCle (const std::string& json, const char* cle)
{
	// Step 1: locate the quoted key, then the following colon.
	const std::string motif = std::string ("\"") + cle + "\"";
	size_t pos = json.find (motif);
	if (pos == std::string::npos)
		return pos;
	pos = json.find (':', pos + motif.size ());
	if (pos == std::string::npos)
		return pos;
	// Step 2: skip the colon and any white space.
	++pos;
	while (pos < json.size () && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
		++pos;
	return pos;
}


// Reads <racine>\reglages.json. Never fails: a missing file, a missing key or an unknown value
// keeps the default of Reglages.
Reglages LireReglages (const std::wstring& racine)
{
	Reglages r;
	std::string json;
	if (!LireFichier (Joindre (racine, L"reglages.json"), json))
		return r;
	// Step 1: the three language codes (i = 0 "langue", 1 "entree", 2 "interface"), kept only
	// when they are string values and known codes.
	for (int i = 0; i < 3; ++i) {
		const size_t pos = ApresCle (json, i == 0 ? "langue" : (i == 1 ? "entree" : "interface"));
		if (pos != std::string::npos && pos < json.size () && json[pos] == '"') {
			const size_t fin = json.find ('"', pos + 1);
			if (fin != std::string::npos) {
				const std::string code = json.substr (pos + 1, fin - pos - 1);
				if (LangueConnue (code))
					(i == 0 ? r.langue : (i == 1 ? r.entree : r.interface)) = code;
			}
		}
	}
	// Step 2: the two booleans (anything other than the literal true counts as false).
	size_t pos = ApresCle (json, "collecte");
	if (pos != std::string::npos)
		r.collecte = json.compare (pos, 4, "true") == 0;
	pos = ApresCle (json, "moteur");
	if (pos != std::string::npos)
		r.moteur = json.compare (pos, 4, "true") == 0;
	// Step 3: the translator code, kept only if it is known.
	pos = ApresCle (json, "fournisseur");
	if (pos != std::string::npos && pos < json.size () && json[pos] == '"') {
		const size_t fin = json.find ('"', pos + 1);
		if (fin != std::string::npos && FournisseurConnu (json.substr (pos + 1, fin - pos - 1)))
			r.fournisseur = json.substr (pos + 1, fin - pos - 1);
	}
	return r;
}


// Path of the translators' credentials file: <racine>\cles.json.
std::wstring CheminCles (const std::wstring& racine)
{
	return Joindre (racine, L"cles.json");
}


// Creates cles.json with one empty section per translator, each with a "_ou" hint telling
// where to get the key. Returns true at once if the file already exists (never overwritten,
// it holds the user's keys).
bool CreerModeleCles (const std::wstring& racine, std::wstring& erreur)
{
	const std::wstring chemin = CheminCles (racine);
	if (FichierExiste (chemin))
		return true;
	const std::string modele =
		"{\n"
		"  \"_explication\": \"Cles des traducteurs en ligne de Tapir Translator. Ce fichier reste sur ce poste : il n'est jamais envoye ni lu par les scripts. Enregistrez-le, la cle sert des la traduction suivante.\",\n"
		"  \"deepl\": {\n"
		"    \"_ou\": \"https://www.deepl.com/pro-api : offre DeepL API Free (une cle se termine par :fx)\",\n"
		"    \"cle\": \"\"\n"
		"  },\n"
		"  \"microsoft\": {\n"
		"    \"_ou\": \"portail Azure : ressource Translator, niveau F0 (gratuit) ; region = celle de la ressource, ex. westeurope\",\n"
		"    \"cle\": \"\",\n"
		"    \"region\": \"\"\n"
		"  },\n"
		"  \"google\": {\n"
		"    \"_ou\": \"console Google Cloud : API Cloud Translation activee, cle API\",\n"
		"    \"cle\": \"\"\n"
		"  },\n"
		"  \"mymemory\": {\n"
		"    \"_ou\": \"aucune cle ; une adresse e-mail augmente le quota quotidien gratuit\",\n"
		"    \"email\": \"\"\n"
		"  },\n"
		"  \"libretranslate\": {\n"
		"    \"_ou\": \"adresse d'un serveur LibreTranslate (le votre ou une instance publique) et sa cle si elle en demande une\",\n"
		"    \"url\": \"\",\n"
		"    \"cle\": \"\"\n"
		"  }\n"
		"}\n";
	CreerDossier (racine);
	return EcrireFichier (chemin, modele, erreur);
}


// Path of the term corrections file: <racine>\corrections.json.
std::wstring CheminCorrections (const std::wstring& racine)
{
	return Joindre (racine, L"corrections.json");
}


// Creates corrections.json pre-filled with the common mistakes of online translators on
// ArchiCAD vocabulary, for fr, de, es, it and pt. Returns true at once if the file already
// exists (never overwritten: the user edits it). The template is written as two raw string
// literals (fr, then de/es/it/pt) concatenated into one std::string.
bool CreerModeleCorrections (const std::wstring& racine, std::wstring& erreur)
{
	const std::wstring chemin = CheminCorrections (racine);
	if (FichierExiste (chemin))
		return true;
	const std::string modele = std::string (R"JSON({
  "_explication": "Corrections de termes appliquees aux traductions EN LIGNE (pas aux dictionnaires), langue de sortie par langue de sortie. Enregistrez : les fenetres ouvertes et le cache suivent aussitot. Une regle : source = mot(s) de l'original qui declenchent la regle (facultatif) ; mauvais -> correct (en fr, es, pt le pluriel en -s suit tout seul) ou remplacer = { mauvais: correct, ... }. Les expressions les plus longues passent d'abord : mettez-y les accords ('toutes les histoires' -> 'tous les etages'). Pour corriger une phrase entiere, utilisez plutot dictionnaires\\_perso.json.",
  "fr": [
    {"source": ["story", "stories"], "remplacer": {
      "toutes les histoires": "tous les étages", "les histoires": "les étages", "une histoire": "un étage",
      "cette histoire": "cet étage", "nouvelle histoire": "nouvel étage", "histoire actuelle": "étage actuel",
      "histoire sélectionnée": "étage sélectionné", "histoires sélectionnées": "étages sélectionnés",
      "histoire de la maison": "étage de référence", "histoire d'origine": "étage de référence",
      "histoires": "étages", "histoire": "étage"}},
    {"source": ["slab", "slabs"], "remplacer": {"plaques": "dalles", "plaque": "dalle"}},
    {"source": ["beam", "beams"], "remplacer": {
      "un faisceau": "une poutre", "le faisceau": "la poutre", "ce faisceau": "cette poutre",
      "faisceaux": "poutres", "faisceau": "poutre"}},
    {"source": ["layer", "layers"], "remplacer": {
      "une couche": "un calque", "la couche": "le calque", "cette couche": "ce calque",
      "toutes les couches": "tous les calques", "couches": "calques", "couche": "calque"}},
    {"source": ["pen", "pens"], "remplacer": {"plumes": "stylos", "plume": "stylo", "stylets": "stylos", "stylet": "stylo"}},
    {"source": ["fill", "fills"], "remplacer": {"remplissages": "hachures", "remplissage": "hachure"}},
    {"source": ["railing", "railings"], "remplacer": {"rampes": "garde-corps", "rampe": "garde-corps"}},
    {"source": ["curtain wall", "curtain walls"], "remplacer": {
      "murs de rideau": "murs-rideaux", "mur de rideau": "mur-rideau", "murs-rideau": "murs-rideaux"}},
    {"source": ["morph", "morphs"], "remplacer": {"morphes": "formes", "morphe": "forme", "morphs": "formes", "morph": "forme"}},
    {"source": ["mesh", "meshes"], "remplacer": {"mailles": "maillages", "maille": "maillage"}},
    {"source": ["layout", "layouts"], "remplacer": {"dispositions": "mises en page", "disposition": "mise en page"}}
  ],
)JSON") + std::string (R"JSON(  "de": [
    {"source": ["story", "stories"], "remplacer": {
      "Geschichten": "Geschosse", "Geschichte": "Geschoss", "Stockwerke": "Geschosse", "Stockwerk": "Geschoss"}},
    {"source": ["slab", "slabs"], "remplacer": {"Platten": "Decken", "Platte": "Decke"}},
    {"source": ["layer", "layers"], "remplacer": {"Schichten": "Ebenen", "Schicht": "Ebene"}},
    {"source": ["beam", "beams"], "remplacer": {"Strahlen": "Unterzüge", "Strahl": "Unterzug"}}
  ],
  "es": [
    {"source": ["story", "stories"], "remplacer": {"historias": "plantas", "historia": "planta"}},
    {"source": ["beam", "beams"], "remplacer": {"haces": "vigas", "haz": "viga"}}
  ],
  "it": [
    {"source": ["story", "stories"], "remplacer": {"storie": "piani", "storia": "piano"}},
    {"source": ["slab", "slabs"], "remplacer": {"lastre": "solai", "lastra": "solaio"}},
    {"source": ["layer", "layers"], "remplacer": {"strati": "layer", "strato": "layer"}},
    {"source": ["beam", "beams"], "remplacer": {"fasci": "travi", "fascio": "trave"}}
  ],
  "pt": [
    {"source": ["story", "stories"], "remplacer": {"histórias": "pisos", "história": "piso"}},
    {"source": ["beam", "beams"], "remplacer": {"feixes": "vigas", "feixe": "viga"}}
  ]
}
)JSON");
	CreerDossier (racine);
	return EcrireFichier (chemin, modele, erreur);
}


// Writes <racine>\reglages.json atomically (the layer re-reads it every 0.3 s).
// Returns false, with 'erreur' set, if a language code is unknown or the write fails.
bool EcrireReglages (const std::wstring& racine, const Reglages& reglages, std::wstring& erreur)
{
	// Step 1: refuse unknown input / output languages.
	if (!LangueConnue (reglages.langue) || !LangueConnue (reglages.entree)) {
		erreur = L"langue inconnue : " + DepuisUtf8 (reglages.entree) + L" -> " + DepuisUtf8 (reglages.langue);
		return false;
	}
	// Step 2: build the JSON by hand. An unknown translator falls back to "mymemory" and an
	// unknown tool language to "" (same as ArchiCAD). "port" tells the layer where the engine
	// listens; "ecrit_par" records the writing version.
	CreerDossier (racine);
	std::string json = "{\n";
	json += "  \"entree\": \"" + reglages.entree + "\",\n";
	json += "  \"langue\": \"" + reglages.langue + "\",\n";
	json += std::string ("  \"collecte\": ") + (reglages.collecte ? "true" : "false") + ",\n";
	json += std::string ("  \"moteur\": ") + (reglages.moteur ? "true" : "false") + ",\n";
	json += "  \"fournisseur\": \"" + (FournisseurConnu (reglages.fournisseur) ? reglages.fournisseur : std::string ("mymemory")) + "\",\n";
	json += "  \"interface\": \"" + (LangueConnue (reglages.interface) ? reglages.interface : std::string ()) + "\",\n";
	json += "  \"port\": " + std::to_string (PortMoteur) + ",\n";
	json += std::string ("  \"ecrit_par\": \"Tapir Translator ") + VersionAddOn + "\"\n";
	json += "}\n";
	// Step 3: atomic write.
	return EcrireFichier (Joindre (racine, L"reglages.json"), json, erreur);
}


// =============================================================================
//  Process environment (inherited by the scripts that Tapir launches)
// =============================================================================

// ASCII lower-case copy of a string, for case-insensitive path comparison (Windows paths).
static std::wstring Minuscules (std::wstring s)
{
	for (wchar_t& c : s) {
		if (c >= L'A' && c <= L'Z')
			c = static_cast<wchar_t> (c - L'A' + L'a');
	}
	return s;
}


// Sets an environment variable of the current process in both places where it can be read.
// Returns the result of SetEnvironmentVariableW (the one that matters for Tapir).
static bool PoserVariable (const wchar_t* nom, const std::wstring& valeur)
{
	// SetEnvironmentVariableW: the process environment block (inherited through CreateProcess,
	// which Tapir uses). _wputenv_s: the CRT's own copy, for a launcher that would go through
	// the CRT.
	const bool ok = SetEnvironmentVariableW (nom, valeur.c_str ()) != 0;
	_wputenv_s (nom, valeur.c_str ());
	return ok;
}


// Prepares ArchiCAD's environment so that every Python script launched by Tapir imports the
// layer (see TT_Coeur.hpp). Returns false if a variable could not be set.
bool PreparerEnvironnement (const std::wstring& racine, std::wstring& pythonPath)
{
	// Step 1: current PYTHONPATH of ArchiCAD (possibly empty).
	const std::wstring couche = Joindre (racine, L"couche");
	const std::wstring ancien = LireVariable (L"PYTHONPATH");

	// Step 2: is the layer folder already in it? Each ';'-separated entry is compared
	// case-insensitively, ignoring trailing slashes, so the entry is never added twice.
	bool dejaLa = false;
	size_t debut = 0;
	while (debut <= ancien.size ()) {
		size_t fin = ancien.find (L';', debut);
		if (fin == std::wstring::npos)
			fin = ancien.size ();
		std::wstring morceau = ancien.substr (debut, fin - debut);
		while (!morceau.empty () && (morceau.back () == L'\\' || morceau.back () == L'/'))
			morceau.pop_back ();
		if (!morceau.empty () && Minuscules (morceau) == Minuscules (couche))
			dejaLa = true;
		debut = fin + 1;
	}

	// Step 3: otherwise put it first, ahead of the previous entries (which are kept after it),
	// so that Python's site module imports our sitecustomize.py at interpreter start-up.
	pythonPath = dejaLa ? ancien : (ancien.empty () ? couche : couche + L";" + ancien);
	// Step 4: set PYTHONPATH and TAPIR_TRANSLATOR_DIR (the layer finds its settings,
	// dictionaries and journal there). Both are attempted even if the first one fails.
	bool ok = PoserVariable (L"PYTHONPATH", pythonPath);
	ok = PoserVariable (L"TAPIR_TRANSLATOR_DIR", racine) && ok;
	return ok;
}

}	// namespace TT
