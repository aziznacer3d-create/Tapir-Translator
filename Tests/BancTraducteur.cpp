// *****************************************************************************
// Tapir Translator - banc de test hors ArchiCAD
//
//   Reproduit ce que font ArchiCAD + l'add-on + Tapir, avec le code de l'add-on
//   lui-meme (Src/TT_Coeur.cpp) :
//     1. ExtraireCouche / EcrireReglages / PreparerEnvironnement dans un dossier
//        temporaire (au lieu de %APPDATA%) ;
//     2. lancement du faux script Tests/banc_outil.py par
//        "uv run --script <script> --port N", comme la palette Tapir, avec
//        TAPIR_SCRIPT_ID pose ;
//     3. lecture de l'etat de son interface (texte affiche par Tk / texte vu par
//        le script) pendant que le banc change la langue et les dictionnaires.
//
//   Sortie : une ligne OK / ECHEC par verification, code de retour 0 si tout passe.
//   Usage : BancTraducteur.exe   (uv doit etre dans le PATH)
// *****************************************************************************

#include "TT_Coeur.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined (BANC_DOSSIER_TESTS)
	#error "BANC_DOSSIER_TESTS doit etre defini (voir CMakeLists.txt)"
#endif

namespace {

int g_ok = 0;
int g_echecs = 0;

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

std::wstring DossierTests ()
{
	std::wstring d = TT::DepuisUtf8 (BANC_DOSSIER_TESTS);
	for (wchar_t& c : d) {
		if (c == L'/')
			c = L'\\';
	}
	return d;
}

void Ecrire (const std::wstring& chemin, const std::string& contenu)
{
	std::wstring erreur;
	if (!TT::EcrireFichier (chemin, contenu, erreur))
		std::printf ("  (ecriture impossible : %s)\n", TT::Utf8 (erreur).c_str ());
}

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
//  Le faux script en cours d'execution
// =============================================================================

struct Valeur {
	std::string affiche;	// lu dans l'interpreteur Tcl (ce que voit l'utilisateur)
	std::string vu;			// lu par l'API tkinter (ce que recoit la logique du script)
};

class Outil {
public:
	~Outil () { Arreter (); }

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
		// Comme TapirPalette::ExecuteScript : uv run --script <fichier> --port <n>
		std::wstring ligne = L"uv run --script \"" + script + L"\" --port 19723";
		std::vector<wchar_t> tampon (ligne.begin (), ligne.end ());
		tampon.push_back (L'\0');
		const BOOL ok = CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		CloseHandle (ecriture);
		if (!ok)
			return false;
		CloseHandle (pi.hThread);
		processus_ = pi.hProcess;
		lecteur_ = std::thread ([this] () { Lire (); });
		return true;
	}

	void Commande (const char* c) { Ecrire (TT::Joindre (echange_, L"commande.txt"), c); }

	// Attend un etat qui satisfait la condition (au plus 'ms' millisecondes)
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

	bool AVuLigne (const std::string& debut)
	{
		std::lock_guard<std::mutex> verrou (m_);
		for (const std::string& l : autres_) {
			if (l.compare (0, debut.size (), debut) == 0)
				return true;
		}
		return false;
	}

	Valeur Lire (const std::string& cle)
	{
		std::lock_guard<std::mutex> verrou (m_);
		auto it = etat_.find (cle);
		return it == etat_.end () ? Valeur { "<absent>", "<absent>" } : it->second;
	}

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

	std::vector<std::string> Autres ()
	{
		std::lock_guard<std::mutex> verrou (m_);
		return autres_;
	}

private:
	void Arreter ()
	{
		if (processus_ != nullptr) {
			if (WaitForSingleObject (processus_, 0) != WAIT_OBJECT_0) {
				TerminateProcess (processus_, 99);
				// le Python enfant de uv peut garder le tube ouvert : debloquer la lecture
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

	std::wstring echange_;
	HANDLE processus_ = nullptr;
	HANDLE lecture_ = nullptr;
	std::thread lecteur_;
	std::mutex m_;
	std::map<std::string, Valeur> etat_;
	std::vector<std::string> autres_;
};


// Verifie le texte affiche (et, si donne, le texte vu par le script)
void Texte (Outil& o, const std::string& cle, const std::string& affiche, const char* vu = nullptr)
{
	const Valeur v = o.Lire (cle);
	Verifier (cle + " affiche \"" + affiche + "\"", v.affiche == affiche, "affiche \"" + v.affiche + "\"");
	if (vu != nullptr)
		Verifier (cle + " vu par le script \"" + vu + "\"", v.vu == vu, "vu \"" + v.vu + "\"");
}

std::function<bool (const std::map<std::string, Valeur>&)> Affiche (const std::string& cle, const std::string& texte)
{
	return [cle, texte] (const std::map<std::string, Valeur>& e) {
		auto it = e.find (cle);
		return it != e.end () && it->second.affiche == texte;
	};
}

void Langue (const std::wstring& racine, const char* code, bool collecte, const char* entree = "")
{
	TT::Reglages r;
	r.entree = entree;
	r.langue = code;
	r.collecte = collecte;
	std::wstring erreur;
	Verifier (std::string ("EcrireReglages entree=\"") + entree + "\" sortie=\"" + code + "\"", TT::EcrireReglages (racine, r, erreur), TT::Utf8 (erreur));
	const TT::Reglages relu = TT::LireReglages (racine);
	Verifier ("LireReglages relit entree et sortie", relu.entree == r.entree && relu.langue == r.langue && relu.collecte == r.collecte);
}

void MontrerSortie (Outil& o)
{
	for (const std::string& l : o.Autres ())
		std::printf ("         | %s\n", l.c_str ());
}

std::string Preparer (const std::wstring& racine)
{
	SupprimerArbre (racine);
	std::vector<std::wstring> journal;
	Verifier ("ExtraireCouche", TT::ExtraireCouche (racine, journal));
	Verifier ("couche\\sitecustomize.py ecrit", TT::FichierExiste (TT::Joindre (TT::Joindre (racine, L"couche"), L"sitecustomize.py")));
	Verifier ("dictionnaires\\_commun.json ecrit", TT::FichierExiste (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_commun.json")));
	std::string dico;
	TT::LireFichier (TT::Joindre (TT::Joindre (DossierTests (), L"donnees"), L"banc_outil.json"), dico);
	Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"banc_outil.json"), dico);
	return dico;
}

}	// namespace


int main ()
{
	SetConsoleOutputCP (CP_UTF8);
	wchar_t temp[MAX_PATH] = {};
	GetTempPathW (MAX_PATH, temp);
	const std::wstring racine = TT::Joindre (temp, L"TapirTranslatorBanc_" + std::to_wstring (GetCurrentProcessId ()));
	const std::wstring script = TT::Joindre (DossierTests (), L"banc_outil.py");
	std::printf ("Tapir Translator %s - banc de test\n  dossier : %s\n  script  : %s\n\n", TT::VersionAddOn,
				 TT::Utf8 (racine).c_str (), TT::Utf8 (script).c_str ());

	// --- environnement, comme Initialize () de l'add-on -------------------------------
	std::printf ("[1] Environnement prepare par l'add-on\n");
	std::string dico = Preparer (racine);
	// un PYTHONPATH deja present (avec son propre sitecustomize) doit etre garde et chaine
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

	// --- scenario principal : ouvert en langue d'origine, traduit en direct ---------------
	std::printf ("\n[2] Script ouvert sans traduction puis traduit en direct\n");
	{
		Outil o;
		Verifier ("lancement uv run --script", o.Lancer (script, racine));
		Verifier ("couche chargee par Python (sitecustomize)", o.AttendreLigne ("COUCHE|active", 30000));
		Verifier ("sitecustomize existant chaine", o.AttendreLigne ("SUIVANT|ok", 2000));
		Verifier ("premier etat recu", o.Attendre (Affiche ("titre", "Outil de démonstration"), 10000));
		Texte (o, "fermer", "Fermer", "Fermer");
		Texte (o, "statut", "Prêt", "Prêt");

		Langue (racine, "en", true);
		Verifier ("bascule en anglais en direct (< 5 s)", o.Attendre (Affiche ("titre", "Demo tool"), 5000));
		Texte (o, "titre", "Demo tool", "Outil de démonstration");
		Texte (o, "statut", "Ready", "Prêt");
		Texte (o, "fermer", "Close", "Fermer");
		Texte (o, "nom", "Name:", "Nom :");
		Texte (o, "murs", "Walls", "Murs");
		Texte (o, "cadre", "Options", "Options");
		Texte (o, "multi", "First line\\nSecond line", "Première ligne\\nDeuxième ligne");
		Texte (o, "modele", "3 walls created", "3 murs créés");
		Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");
		Texte (o, "menu_fichier", "File", "Fichier");
		Texte (o, "menu_ouvrir", "Open...", "Ouvrir...");
		Texte (o, "menu_quitter", "Quit", "Quitter");
		Texte (o, "onglet", "General", "Général");
		Texte (o, "entete", "Name", "Nom");
		Texte (o, "canevas", "Floor plan", "Plan");
		Texte (o, "ligne_arbre", "Murs", "Murs");			// donnee : jamais traduite
		Texte (o, "combo", "Murs Dalles", "Murs");			// valeurs de liste : jamais traduites
		Texte (o, "saisie", "Murs", "Murs");				// saisie : jamais traduite
		Texte (o, "logique", "ok");

		o.Commande ("maj");
		Verifier ("le script change son statut pendant la traduction", o.Attendre (Affiche ("statut", "Done"), 3000));
		Texte (o, "statut", "Done", "Terminé");
		Texte (o, "nom", "Value", "Valeur");
		Texte (o, "logique", "ok");

		const std::wstring manquants = TT::Joindre (TT::Joindre (racine, L"manquants"), L"banc_outil.json");
		bool note = false;
		for (int i = 0; i < 40 && !note; ++i) {
			std::string m;
			note = TT::LireFichier (manquants, m) && m.find ("\"Texte inconnu\": \"\"") != std::string::npos;
			if (!note)
				Sleep (100);
		}
		Verifier ("texte non traduit note dans manquants\\banc_outil.json", note);

		// dictionnaire complete pendant que l'outil tourne
		const std::string cle = "\"Plan\": \"Floor plan\"";
		const size_t pos = dico.find (cle);
		Verifier ("dictionnaire du banc lisible", pos != std::string::npos);
		if (pos != std::string::npos) {
			dico.insert (pos + cle.size (), ",\n      \"Texte inconnu\": \"Unknown text\"");
			Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"banc_outil.json"), dico);
		}
		Verifier ("dictionnaire modifie applique en direct", o.Attendre (Affiche ("inconnu", "Unknown text"), 5000));
		Texte (o, "inconnu", "Unknown text", "Texte inconnu");

		Langue (racine, "de", false);
		Verifier ("bascule en allemand", o.Attendre (Affiche ("titre", "Demo-Werkzeug"), 5000));
		Texte (o, "fermer", "Schließen", "Fermer");
		Texte (o, "modele", "3 Wände erstellt", "3 murs créés");
		Texte (o, "murs", "Wände", "Murs");
		Texte (o, "statut", "Fertig", "Terminé");
		Texte (o, "nom", "Wert", "Valeur");					// dictionnaire commun multilingue
		Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");

		Langue (racine, "", false);
		Verifier ("retour a la langue d'origine", o.Attendre (Affiche ("titre", "Outil de démonstration"), 5000));
		Texte (o, "fermer", "Fermer", "Fermer");
		Texte (o, "statut", "Terminé", "Terminé");
		Texte (o, "modele", "3 murs créés", "3 murs créés");
		Texte (o, "menu_ouvrir", "Ouvrir...", "Ouvrir...");
		Texte (o, "canevas", "Plan", "Plan");

		Langue (racine, "fr", false);
		Sleep (1500);
		Texte (o, "titre", "Outil de démonstration");		// fr = langue source : rien a faire

		Langue (racine, "en", false);
		Verifier ("de nouveau en anglais apres plusieurs bascules", o.Attendre (Affiche ("titre", "Demo tool"), 5000));
		Texte (o, "menu_quitter", "Quit", "Quitter");
		Texte (o, "onglet", "General", "Général");
		Texte (o, "logique", "ok");

		DWORD code = 1;
		Verifier ("fermeture propre du script", o.Finir (code, 10000) && code == 0, "code " + std::to_string (code));
		bool trace = false;
		for (const std::string& l : o.Autres ())
			trace = trace || l.find ("Traceback") != std::string::npos;
		Verifier ("aucune erreur Python", !trace);
		if (g_echecs > 0)
			MontrerSortie (o);
	}

	// --- script lance alors que la langue est deja choisie ----------------------------------
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

	// --- dictionnaire commun seul (script sans dictionnaire), toutes les langues ------------
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
		Texte (o, "titre", "Outil de démonstration");		// propre au script : pas dans le commun
		Langue (racine, "hu", false);
		Verifier ("hongrois", o.Attendre (Affiche ("fermer", "Bezárás"), 5000));
		Texte (o, "murs", "Falak", "Murs");
		Texte (o, "menu_quitter", "Kilépés", "Quitter");
		Texte (o, "onglet", "Általános", "Général");
		Langue (racine, "ar", false);
		Verifier ("arabe", o.Attendre (Affiche ("fermer", "إغلاق"), 5000));
		Texte (o, "murs", "الجدران", "Murs");
		Texte (o, "entete", "الاسم", "Nom");
		Langue (racine, "es", false);
		Verifier ("espagnol", o.Attendre (Affiche ("fermer", "Cerrar"), 5000));
		Texte (o, "murs", "Muros", "Murs");
		Langue (racine, "it", false);
		Verifier ("italien", o.Attendre (Affiche ("fermer", "Chiudi"), 5000));
		Texte (o, "logique", "ok");
		DWORD code = 1;
		Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
	}

	// --- script ecrit en anglais, sans langue declaree --------------------------------------
	std::printf ("\n[3c] Script ecrit en anglais (langue non declaree)\n");
	{
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_anglais");
		SetEnvironmentVariableW (L"BANC_UI", L"en");
		Langue (racine, "fr", false);
		Outil o;
		Verifier ("lancement", o.Lancer (script, racine));
		Verifier ("anglais -> francais", o.Attendre (Affiche ("fermer", "Fermer"), 30000));
		Texte (o, "fermer", "Fermer", "Close");
		Texte (o, "nom", "Nom :", "Name:");				// typographie francaise
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

		// langue d'entree choisie dans le menu
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
		Texte (o, "fermer", "Close", "Close");				// entree = sortie
		Texte (o, "logique", "ok");
		DWORD code = 1;
		Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		SetEnvironmentVariableW (L"BANC_UI", nullptr);
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", nullptr);
		Langue (racine, "en", false);
	}

	// --- hors Tapir : la couche ne doit rien faire ---------------------------------------
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

	// --- journal de la couche -----------------------------------------------------------
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

	std::printf ("\n%d OK, %d ECHEC(S)\n", g_ok, g_echecs);
	if (g_echecs == 0)
		SupprimerArbre (racine);
	else
		std::printf ("dossier garde pour examen : %s\n", TT::Utf8 (racine).c_str ());
	return g_echecs == 0 ? 0 : 1;
}
