// *****************************************************************************
// Tapir Translator - coeur sans ArchiCAD (voir TT_Coeur.hpp)
// *****************************************************************************

#include "TT_Coeur.hpp"
#include "TT_Embarque.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

namespace TT {

const char* const VersionAddOn = "0.6.0";

// Meme ordre que les menus 32500 / 32501 (RFIX/Tapir_TranslatorFix.grc) et que les
// colonnes de dictionnaires/_commun.json
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

const Fournisseur Fournisseurs[] = {
	{ "deepl",			"DeepL",					"cle (compte DeepL API Free)" },
	{ "microsoft",		"Microsoft Translator",		"cle + region (Azure, niveau gratuit F0)" },
	{ "google",			"Google Cloud Translation",	"cle (Google Cloud)" },
	{ "mymemory",		"MyMemory",					"rien (e-mail facultatif pour un quota plus grand)" },
	{ "libretranslate",	"LibreTranslate",			"adresse du serveur (+ cle si demandee)" },
};
const int NombreFournisseurs = static_cast<int> (sizeof (Fournisseurs) / sizeof (Fournisseurs[0]));

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
//  Textes
// =============================================================================

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
//  Fichiers
// =============================================================================

std::wstring LireVariable (const wchar_t* nom)
{
	const DWORD n = GetEnvironmentVariableW (nom, nullptr, 0);
	if (n == 0)
		return std::wstring ();
	std::vector<wchar_t> tampon (n + 1, L'\0');
	const DWORD lu = GetEnvironmentVariableW (nom, tampon.data (), n + 1);
	if (lu == 0 || lu > n)
		return std::wstring ();
	return std::wstring (tampon.data (), lu);
}


std::wstring DossierParDefaut ()
{
	const std::wstring appData = LireVariable (L"APPDATA");
	if (appData.empty ())
		return std::wstring ();
	return Joindre (appData, L"Tapir Translator");
}


std::wstring Joindre (const std::wstring& dossier, const std::wstring& nom)
{
	if (dossier.empty ())
		return nom;
	const wchar_t fin = dossier.back ();
	if (fin == L'\\' || fin == L'/')
		return dossier + nom;
	return dossier + L"\\" + nom;
}


bool FichierExiste (const std::wstring& chemin)
{
	const DWORD a = GetFileAttributesW (chemin.c_str ());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


bool DossierExiste (const std::wstring& chemin)
{
	const DWORD a = GetFileAttributesW (chemin.c_str ());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}


bool CreerDossier (const std::wstring& chemin)
{
	if (chemin.empty ())
		return false;
	if (DossierExiste (chemin))
		return true;
	const size_t sep = chemin.find_last_of (L"\\/");
	if (sep != std::wstring::npos && sep > 2)
		CreerDossier (chemin.substr (0, sep));
	return CreateDirectoryW (chemin.c_str (), nullptr) != 0 || GetLastError () == ERROR_ALREADY_EXISTS;
}


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


bool EcrireFichier (const std::wstring& chemin, const std::string& contenu, std::wstring& erreur)
{
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
	const DWORD codeEcriture = ok ? 0 : GetLastError ();
	CloseHandle (f);
	if (!ok) {
		erreur = L"ecriture de " + tmp + L" impossible : " + MessageErreur (codeEcriture);
		DeleteFileW (tmp.c_str ());
		return false;
	}
	// Le fichier peut etre ouvert un instant en lecture par la couche Python : on reessaie.
	for (int essai = 0; essai < 50; ++essai) {
		if (MoveFileExW (tmp.c_str (), chemin.c_str (), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		Sleep (20);
	}
	erreur = L"remplacement de " + chemin + L" impossible : " + MessageErreur (GetLastError ());
	DeleteFileW (tmp.c_str ());
	return false;
}


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


int CompterVides (const std::string& json)
{
	// Les fichiers de manquants sont ecrits par la couche (json.dump, indent=2) :
	// une entree vide s'y lit toujours  "...": ""
	int n = 0;
	size_t pos = 0;
	const std::string motif = "\": \"\"";
	while ((pos = json.find (motif, pos)) != std::string::npos) {
		++n;
		pos += motif.size ();
	}
	return n;
}


std::vector<std::string> DernieresLignes (const std::wstring& chemin, int nombre)
{
	std::vector<std::string> lignes;
	std::string contenu;
	if (!LireFichier (chemin, contenu))
		return lignes;
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
	if (static_cast<int> (lignes.size ()) > nombre)
		lignes.erase (lignes.begin (), lignes.end () - nombre);
	return lignes;
}


// =============================================================================
//  Couche et dictionnaires
// =============================================================================

bool ExtraireCouche (const std::wstring& racine, std::vector<std::wstring>& journal)
{
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

	// _commun.json est celui de la version de l'add-on : toujours remplace.
	// Les ajouts de l'utilisateur vont dans _perso.json (jamais touche, prioritaire).
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
	if (!FichierExiste (CheminCles (racine)) && CreerModeleCles (racine, erreur))
		journal.push_back (L"fichier des cles cree : " + CheminCles (racine));
	if (!FichierExiste (CheminCorrections (racine)) && CreerModeleCorrections (racine, erreur))
		journal.push_back (L"fichier des corrections cree : " + CheminCorrections (racine));
	return ok;
}


// =============================================================================
//  Reglages (reglages.json, relu en direct par la couche)
// =============================================================================

static bool LangueConnue (const std::string& code)
{
	for (int i = 0; i < NombreLangues; ++i) {
		if (code == Langues[i].code)
			return true;
	}
	return false;
}


static size_t ApresCle (const std::string& json, const char* cle)
{
	const std::string motif = std::string ("\"") + cle + "\"";
	size_t pos = json.find (motif);
	if (pos == std::string::npos)
		return pos;
	pos = json.find (':', pos + motif.size ());
	if (pos == std::string::npos)
		return pos;
	++pos;
	while (pos < json.size () && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
		++pos;
	return pos;
}


Reglages LireReglages (const std::wstring& racine)
{
	Reglages r;
	std::string json;
	if (!LireFichier (Joindre (racine, L"reglages.json"), json))
		return r;
	for (int i = 0; i < 2; ++i) {
		const size_t pos = ApresCle (json, i == 0 ? "langue" : "entree");
		if (pos != std::string::npos && pos < json.size () && json[pos] == '"') {
			const size_t fin = json.find ('"', pos + 1);
			if (fin != std::string::npos) {
				const std::string code = json.substr (pos + 1, fin - pos - 1);
				if (LangueConnue (code))
					(i == 0 ? r.langue : r.entree) = code;
			}
		}
	}
	size_t pos = ApresCle (json, "collecte");
	if (pos != std::string::npos)
		r.collecte = json.compare (pos, 4, "true") == 0;
	pos = ApresCle (json, "moteur");
	if (pos != std::string::npos)
		r.moteur = json.compare (pos, 4, "true") == 0;
	pos = ApresCle (json, "fournisseur");
	if (pos != std::string::npos && pos < json.size () && json[pos] == '"') {
		const size_t fin = json.find ('"', pos + 1);
		if (fin != std::string::npos && FournisseurConnu (json.substr (pos + 1, fin - pos - 1)))
			r.fournisseur = json.substr (pos + 1, fin - pos - 1);
	}
	return r;
}


std::wstring CheminCles (const std::wstring& racine)
{
	return Joindre (racine, L"cles.json");
}


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


std::wstring CheminCorrections (const std::wstring& racine)
{
	return Joindre (racine, L"corrections.json");
}


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


bool EcrireReglages (const std::wstring& racine, const Reglages& reglages, std::wstring& erreur)
{
	if (!LangueConnue (reglages.langue) || !LangueConnue (reglages.entree)) {
		erreur = L"langue inconnue : " + DepuisUtf8 (reglages.entree) + L" -> " + DepuisUtf8 (reglages.langue);
		return false;
	}
	CreerDossier (racine);
	std::string json = "{\n";
	json += "  \"entree\": \"" + reglages.entree + "\",\n";
	json += "  \"langue\": \"" + reglages.langue + "\",\n";
	json += std::string ("  \"collecte\": ") + (reglages.collecte ? "true" : "false") + ",\n";
	json += std::string ("  \"moteur\": ") + (reglages.moteur ? "true" : "false") + ",\n";
	json += "  \"fournisseur\": \"" + (FournisseurConnu (reglages.fournisseur) ? reglages.fournisseur : std::string ("mymemory")) + "\",\n";
	json += "  \"port\": " + std::to_string (PortMoteur) + ",\n";
	json += std::string ("  \"ecrit_par\": \"Tapir Translator ") + VersionAddOn + "\"\n";
	json += "}\n";
	return EcrireFichier (Joindre (racine, L"reglages.json"), json, erreur);
}


// =============================================================================
//  Environnement du processus (herite par les scripts que Tapir lance)
// =============================================================================

static std::wstring Minuscules (std::wstring s)
{
	for (wchar_t& c : s) {
		if (c >= L'A' && c <= L'Z')
			c = static_cast<wchar_t> (c - L'A' + L'a');
	}
	return s;
}


static bool PoserVariable (const wchar_t* nom, const std::wstring& valeur)
{
	// SetEnvironmentVariableW : bloc d'environnement du processus (CreateProcess, que
	// Tapir utilise). _wputenv_s : copie du CRT, pour un lanceur qui passerait par le CRT.
	const bool ok = SetEnvironmentVariableW (nom, valeur.c_str ()) != 0;
	_wputenv_s (nom, valeur.c_str ());
	return ok;
}


bool PreparerEnvironnement (const std::wstring& racine, std::wstring& pythonPath)
{
	const std::wstring couche = Joindre (racine, L"couche");
	const std::wstring ancien = LireVariable (L"PYTHONPATH");

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

	pythonPath = dejaLa ? ancien : (ancien.empty () ? couche : couche + L";" + ancien);
	bool ok = PoserVariable (L"PYTHONPATH", pythonPath);
	ok = PoserVariable (L"TAPIR_TRANSLATOR_DIR", racine) && ok;
	return ok;
}

}	// namespace TT
