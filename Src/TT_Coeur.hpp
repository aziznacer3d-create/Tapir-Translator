// *****************************************************************************
// Tapir Translator - coeur sans ArchiCAD (Win32 + bibliotheque standard)
//
//   Partage par l'add-on (Src/AddOnMain.cpp) et par le banc de test
//   (Tests/BancTraducteur.cpp) : le banc exerce exactement le code qui ecrit
//   la couche, les reglages et l'environnement dans ArchiCAD.
//
//   N'utilise que des en-tetes (std::string / std::wstring) et l'API Win32 :
//   l'add-on est compile avec /Zc:wchar_t- (wchar_t non natif), les fonctions
//   compilees de la STL qui prennent un wchar_t ne se lieraient pas.
// *****************************************************************************

#ifndef TT_COEUR_HPP
#define TT_COEUR_HPP

#include <string>
#include <vector>

namespace TT {

extern const char* const VersionAddOn;

struct Langue {
	const char* code;	// "" = langue d'origine du script
	const char* nom;
};
extern const Langue Langues[];
extern const int NombreLangues;

struct Reglages {
	std::string entree;		// langue des scripts, code de Langues[], "" = detection automatique
	std::string langue;		// langue de sortie, code de Langues[], "" = aucune traduction
	bool collecte = false;	// noter les textes sans traduction
	bool moteur = false;	// traduction en ligne des textes hors dictionnaire
	std::string fournisseur = "mymemory";	// code de Fournisseurs[]
};

// Traducteurs en ligne (meme ordre que le sous-menu Options de RFIX/Tapir_TranslatorFix.grc)
struct Fournisseur {
	const char* code;
	const char* nom;
	const char* besoin;		// ce qu'il faut renseigner dans cles.json
};
extern const Fournisseur Fournisseurs[];
extern const int NombreFournisseurs;
bool FournisseurConnu (const std::string& code);	// + "essai" (banc) et "local" (option de build)

// Port du moteur de traduction (127.0.0.1)
constexpr int PortMoteur = 8779;

// cles.json : cles des traducteurs en ligne (cree vide s'il n'existe pas, jamais ecrase)
std::wstring CheminCles (const std::wstring& racine);
bool CreerModeleCles (const std::wstring& racine, std::wstring& erreur);

// %APPDATA%\Tapir Translator (vide si APPDATA n'existe pas)
std::wstring DossierParDefaut ();

std::wstring Joindre (const std::wstring& dossier, const std::wstring& nom);
bool		 FichierExiste (const std::wstring& chemin);
bool		 DossierExiste (const std::wstring& chemin);
bool		 CreerDossier (const std::wstring& chemin);	// avec les parents
bool		 LireFichier (const std::wstring& chemin, std::string& contenu);
// Ecriture atomique (fichier .tmp puis remplacement) : la couche Python, qui
// relit les fichiers en direct, ne voit jamais un fichier a moitie ecrit.
bool		 EcrireFichier (const std::wstring& chemin, const std::string& contenu, std::wstring& erreur);

std::string	 Utf8 (const std::wstring& texte);
std::wstring DepuisUtf8 (const std::string& texte);

// Ecrit couche\sitecustomize.py (toujours, c'est la version de l'add-on) et
// dictionnaires\_commun.json (seulement s'il n'existe pas : il est a l'usage).
bool ExtraireCouche (const std::wstring& racine, std::vector<std::wstring>& journal);

Reglages LireReglages (const std::wstring& racine);
bool	 EcrireReglages (const std::wstring& racine, const Reglages& reglages, std::wstring& erreur);

// Met <racine>\couche en tete du PYTHONPATH du processus courant (ArchiCAD) et
// pose TAPIR_TRANSLATOR_DIR. Tapir garde ce PYTHONPATH pour chaque script lance
// (il y ajoute ses helpers devant) : Python importe alors la couche tout seul.
bool PreparerEnvironnement (const std::wstring& racine, std::wstring& pythonPath);

std::wstring LireVariable (const wchar_t* nom);

// Noms des .json d'un dossier (sans chemin)
std::vector<std::wstring> ListerJson (const std::wstring& dossier);

// Supprime un dossier et son contenu (sans demander)
void SupprimerDossier (const std::wstring& dossier);

// Sous-dossiers d'un dossier (noms seuls)
std::vector<std::wstring> ListerDossiers (const std::wstring& dossier);

// Nombre de textes encore vides ("texte": "") d'un fichier de manquants
int CompterVides (const std::string& json);

// Dernieres lignes d'un fichier texte
std::vector<std::string> DernieresLignes (const std::wstring& chemin, int nombre);

}	// namespace TT

#endif
