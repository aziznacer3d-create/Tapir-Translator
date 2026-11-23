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

const char* const VersionAddOn = "0.3.0";

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
	{ "ar",	"العربية" },
};
const int NombreLangues = static_cast<int> (sizeof (Langues) / sizeof (Langues[0]));


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

	const std::wstring commun = Joindre (dictionnaires, L"_commun.json");
	if (FichierExiste (commun)) {
		journal.push_back (L"dictionnaire commun garde (deja present) : " + commun);
	} else {
		const std::string contenu (reinterpret_cast<const char*> (Embarque::Commun), Embarque::CommunTaille);
		if (EcrireFichier (commun, contenu, erreur))
			journal.push_back (L"dictionnaire commun ecrit : " + commun);
		else {
			journal.push_back (erreur);
			ok = false;
		}
	}
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
	return r;
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
