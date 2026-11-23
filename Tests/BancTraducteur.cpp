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

#include "TT_Charge.hpp"
#include "TT_Coeur.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment (lib, "Ws2_32.lib")

#include "Json.hpp"

#include <cstring>
#include <set>

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

void Langue (const std::wstring& racine, const char* code, bool collecte, const char* entree = "", bool moteur = false,
			 const char* fournisseur = "essai")
{
	TT::Reglages r;
	r.entree = entree;
	r.langue = code;
	r.collecte = collecte;
	r.moteur = moteur;
	r.fournisseur = fournisseur;
	std::wstring erreur;
	Verifier (std::string ("EcrireReglages entree=\"") + entree + "\" sortie=\"" + code + "\"" + (moteur ? " moteur" : ""),
			  TT::EcrireReglages (racine, r, erreur), TT::Utf8 (erreur));
	const TT::Reglages relu = TT::LireReglages (racine);
	Verifier ("LireReglages relit les reglages", relu.entree == r.entree && relu.langue == r.langue && relu.collecte == r.collecte &&
			  relu.moteur == r.moteur && relu.fournisseur == r.fournisseur);
}

// Moteur de traduction, a cote du banc ; options : L"--essai" ou L"--cles \"...\""
HANDLE LancerMoteur (int port, const std::wstring& journal, const std::wstring& options = L"--essai")
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW (nullptr, exe, MAX_PATH);
	std::wstring chemin = exe;
	chemin = chemin.substr (0, chemin.find_last_of (L'\\') + 1) + L"TapirTranslatorMoteur.exe";
	std::wstring ligne = L"\"" + chemin + L"\" " + options + L" --port " + std::to_wstring (port) + L" --parent " +
						 std::to_wstring (GetCurrentProcessId ()) + L" --journal \"" + journal + L"\"";
	std::vector<wchar_t> tampon (ligne.begin (), ligne.end ());
	tampon.push_back (L'\0');
	STARTUPINFOW si = {};
	si.cb = sizeof (si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW (nullptr, tampon.data (), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return nullptr;
	CloseHandle (pi.hThread);
	Sleep (500);
	return pi.hProcess;
}

// Faux service de traduction : repond aux formats de requete et de reponse de DeepL, Microsoft,
// Google, MyMemory et LibreTranslate, avec "<SERVICE>:<langue cible>:<texte>". Verifie les cles.
class ServeurFactice {
public:
	~ServeurFactice () { Arreter (); }

	bool Demarrer (int port)
	{
		WSADATA wsa;
		WSAStartup (MAKEWORD (2, 2), &wsa);
		ecoute_ = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons (static_cast<u_short> (port));
		inet_pton (AF_INET, "127.0.0.1", &a.sin_addr);
		if (bind (ecoute_, reinterpret_cast<sockaddr*> (&a), sizeof (a)) != 0 || listen (ecoute_, 16) != 0)
			return false;
		fil_ = std::thread ([this] () { Boucle (); });
		return true;
	}

	void Arreter ()
	{
		if (ecoute_ != INVALID_SOCKET) {
			closesocket (ecoute_);
			ecoute_ = INVALID_SOCKET;
		}
		if (fil_.joinable ())
			fil_.join ();
	}

	bool ARecu (const std::string& texte)
	{
		std::lock_guard<std::mutex> v (m_);
		return recus_.count (texte) > 0;
	}

private:
	static std::string Decoder (const std::string& s)
	{
		std::string r;
		for (size_t i = 0; i < s.size (); ++i) {
			if (s[i] == '%' && i + 2 < s.size ()) {
				r += static_cast<char> (std::strtol (s.substr (i + 1, 2).c_str (), nullptr, 16));
				i += 2;
			} else
				r += s[i] == '+' ? ' ' : s[i];
		}
		return r;
	}

	static std::string Parametre (const std::string& requete, const std::string& nom)
	{
		size_t p = 0;
		while ((p = requete.find (nom + "=", p)) != std::string::npos) {
			if (p == 0 || requete[p - 1] == '&' || requete[p - 1] == '?') {
				const size_t debut = p + nom.size () + 1;
				const size_t fin = requete.find ('&', debut);
				return Decoder (requete.substr (debut, fin == std::string::npos ? std::string::npos : fin - debut));
			}
			++p;
		}
		return std::string ();
	}

	void Noter (const std::string& t)
	{
		std::lock_guard<std::mutex> v (m_);
		recus_.insert (t);
	}

	static void Repondre (SOCKET s, int code, const std::string& json)
	{
		const std::string r = "HTTP/1.1 " + std::to_string (code) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
							  std::to_string (json.size ()) + "\r\nConnection: close\r\n\r\n" + json;
		send (s, r.data (), static_cast<int> (r.size ()), 0);
	}

	void Boucle ()
	{
		for (;;) {
			SOCKET c = accept (ecoute_, nullptr, nullptr);
			if (c == INVALID_SOCKET)
				return;
			Traiter (c);
			shutdown (c, SD_BOTH);
			closesocket (c);
		}
	}

	void Traiter (SOCKET c)
	{
		std::string t;
		char b[8192];
		size_t fin = std::string::npos;
		while (fin == std::string::npos) {
			const int n = recv (c, b, sizeof (b), 0);
			if (n <= 0)
				return;
			t.append (b, static_cast<size_t> (n));
			fin = t.find ("\r\n\r\n");
		}
		std::string entetes = t.substr (0, fin);
		std::string corps = t.substr (fin + 4);
		std::string minus = entetes;
		for (char& ch : minus)
			ch = static_cast<char> (std::tolower (static_cast<unsigned char> (ch)));
		const size_t cl = minus.find ("content-length:");
		const size_t longueur = cl == std::string::npos ? 0 : static_cast<size_t> (std::strtoul (minus.c_str () + cl + 15, nullptr, 10));
		while (corps.size () < longueur) {
			const int n = recv (c, b, sizeof (b), 0);
			if (n <= 0)
				break;
			corps.append (b, static_cast<size_t> (n));
		}
		const auto entete = [&] (const char* nom) {
			const size_t p = minus.find (std::string ("\r\n") + nom + ":");
			if (p == std::string::npos)
				return std::string ();
			size_t d = p + 3 + std::strlen (nom);
			while (d < entetes.size () && entetes[d] == ' ')
				++d;
			return entetes.substr (d, entetes.find ("\r\n", d) - d);
		};
		const size_t sp1 = entetes.find (' ');
		const std::string cibleUrl = entetes.substr (sp1 + 1, entetes.find (' ', sp1 + 1) - sp1 - 1);
		const size_t q = cibleUrl.find ('?');
		const std::string chemin = cibleUrl.substr (0, q);
		const std::string requete = q == std::string::npos ? std::string () : cibleUrl.substr (q + 1);
		Json::Valeur v;
		Json::Lecteur (corps).Lire (v);

		const auto liste = [&] (const Json::Valeur* tab, const char* prefixe, const std::string& cible, const char* champ) {
			std::string r;
			if (tab != nullptr && tab->genre == Json::Valeur::Tableau) {
				for (const Json::Valeur& e : tab->tableau) {
					const std::string texte = e.genre == Json::Valeur::Chaine ? e.chaine : e.Texte ("Text");
					Noter (texte);
					const std::string trad = Json::Chaine (std::string (prefixe) + ":" + cible + ":" + texte);
					r += std::string (r.empty () ? "" : ", ") + (champ != nullptr ? "{\"" + std::string (champ) + "\": " + trad + "}" : trad);
				}
			}
			return r;
		};

		if (chemin == "/v2/translate") {						// DeepL
			if (entete ("authorization") != "DeepL-Auth-Key cle-deepl:fx")
				return Repondre (c, 403, "{\"message\": \"Authorization failed\"}");
			return Repondre (c, 200, "{\"translations\": [" + liste (v.Champ ("text"), "DL", v.Texte ("target_lang"), "text") + "]}");
		}
		if (chemin == "/translate" && !Parametre (requete, "api-version").empty ()) {	// Microsoft
			if (entete ("ocp-apim-subscription-key") != "cle-ms")
				return Repondre (c, 401, "{\"error\": {\"message\": \"invalid key\"}}");
			const std::string to = Parametre (requete, "to");
			std::string r;
			for (const Json::Valeur& e : v.tableau) {
				Noter (e.Texte ("Text"));
				r += std::string (r.empty () ? "" : ", ") + "{\"translations\": [{\"text\": " + Json::Chaine ("MS:" + to + ":" + e.Texte ("Text")) +
					 ", \"to\": " + Json::Chaine (to) + "}]}";
			}
			return Repondre (c, 200, "[" + r + "]");
		}
		if (chemin == "/language/translate/v2") {				// Google
			if (Parametre (requete, "key") != "cle-google")
				return Repondre (c, 400, "{\"error\": {\"message\": \"API key not valid\"}}");
			return Repondre (c, 200, "{\"data\": {\"translations\": [" + liste (v.Champ ("q"), "GG", v.Texte ("target"), "translatedText") + "]}}");
		}
		if (chemin == "/get") {									// MyMemory
			const std::string texte = Parametre (requete, "q");
			const std::string paire = Parametre (requete, "langpair");
			Noter (texte);
			return Repondre (c, 200, "{\"responseData\": {\"translatedText\": " + Json::Chaine ("MM:" + paire.substr (paire.find ('|') + 1) + ":" + texte) +
										 "}, \"responseStatus\": 200}");
		}
		if (chemin == "/translate") {							// LibreTranslate
			return Repondre (c, 200, "{\"translatedText\": [" + liste (v.Champ ("q"), "LT", v.Texte ("target"), nullptr) + "]}");
		}
		Repondre (c, 404, "{}");
	}

	SOCKET ecoute_ = INVALID_SOCKET;
	std::thread fil_;
	std::mutex m_;
	std::set<std::string> recus_;
};

void ArreterMoteur (HANDLE& moteur)
{
	if (moteur != nullptr) {
		TerminateProcess (moteur, 0);
		WaitForSingleObject (moteur, 5000);
		CloseHandle (moteur);
		moteur = nullptr;
	}
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
	Verifier ("dictionnaires\\_perso.json cree", TT::FichierExiste (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_perso.json")));
	// un _commun.json ancien (modifie) est remplace par celui de la version ; _perso.json est garde
	Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_commun.json"), "{\"ancien\": true}");
	Ecrire (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_perso.json"), "{\"format\": 2, \"termes\": []}");
	{
		std::vector<std::wstring> j2;
		TT::ExtraireCouche (racine, j2);
		std::string c, p;
		TT::LireFichier (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_commun.json"), c);
		TT::LireFichier (TT::Joindre (TT::Joindre (racine, L"dictionnaires"), L"_perso.json"), p);
		Verifier ("ancien _commun.json remplace par celui de la version", c.find ("\"termes\"") != std::string::npos);
		Verifier ("_perso.json de l'utilisateur garde", p == "{\"format\": 2, \"termes\": []}");
	}
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

	// --- charge embarquee a la fin du .apx (TT_Charge) ------------------------------------
	std::printf ("[0] Moteur et modele embarques dans le .apx\n");
	{
		const std::wstring dossier = TT::Joindre (temp, L"TapirTranslatorCharge_" + std::to_wstring (GetCurrentProcessId ()));
		SupprimerArbre (dossier);
		const std::wstring source = TT::Joindre (dossier, L"source");
		TT::CreerDossier (TT::Joindre (source, L"modele\\sous"));
		std::string gros (3 * 1024 * 1024 + 17, '\0');		// plus grand que le tampon de copie (1 Mo)
		for (size_t i = 0; i < gros.size (); ++i)
			gros[i] = static_cast<char> ((i * 31 + 7) & 0xFF);
		Ecrire (TT::Joindre (source, L"moteur.exe"), "MZ faux moteur");
		Ecrire (TT::Joindre (source, L"modele\\poids.bin"), gros);
		Ecrire (TT::Joindre (source, L"modele\\sous\\vocabulaire é.txt"), "mur\nwall\n");
		const std::wstring apx = TT::Joindre (dossier, L"faux.apx");
		const std::string contenuApx = "faux contenu de DLL";
		Ecrire (apx, contenuApx);

		std::wstring empreinte, empreinte2;
		std::vector<std::wstring> journal;
		Verifier ("Empaqueter", TT::Empaqueter (apx, source, empreinte, journal));
		Verifier ("Empaqueter une seconde fois (ancienne charge retiree)", TT::Empaqueter (apx, source, empreinte2, journal));
		Verifier ("meme contenu = meme empreinte", empreinte == empreinte2 && !empreinte.empty ());
		std::string apxLu;
		TT::LireFichier (apx, apxLu);
		Verifier ("le debut du .apx est intact", apxLu.compare (0, contenuApx.size (), contenuApx) == 0);
		Verifier ("une seule charge apres deux passages", apxLu.size () < contenuApx.size () + gros.size () + 2000,
				  std::to_string (apxLu.size ()));
		std::wstring e;
		std::vector<std::wstring> chemins;
		Verifier ("LireCharge : 3 fichiers", TT::LireCharge (apx, e, chemins) && chemins.size () == 3 && e == empreinte);

		std::wstring extrait;
		const std::wstring base = TT::Joindre (dossier, L"extrait");
		Verifier ("ExtraireCharge", TT::ExtraireCharge (apx, base, extrait, journal));
		Verifier ("dossier d'extraction = empreinte", extrait == TT::Joindre (base, empreinte), TT::Utf8 (extrait));
		std::string lu;
		Verifier ("gros fichier identique", TT::LireFichier (TT::Joindre (extrait, L"modele\\poids.bin"), lu) && lu == gros);
		Verifier ("fichier a accent identique", TT::LireFichier (TT::Joindre (extrait, L"modele\\sous\\vocabulaire é.txt"), lu) && lu == "mur\nwall\n");
		journal.clear ();
		Verifier ("deuxieme extraction : rien a refaire", TT::ExtraireCharge (apx, base, extrait, journal) &&
				  !journal.empty () && journal.back ().find (L"deja extrait") != std::wstring::npos);

		Ecrire (TT::Joindre (source, L"moteur.exe"), "MZ faux moteur v2");
		std::wstring empreinte3;
		TT::Empaqueter (apx, source, empreinte3, journal);
		Verifier ("nouvelle version = nouvelle empreinte", empreinte3 != empreinte);
		Verifier ("nouvelle version extraite a cote", TT::ExtraireCharge (apx, base, extrait, journal) &&
				  TT::LireFichier (TT::Joindre (extrait, L"moteur.exe"), lu) && lu == "MZ faux moteur v2");
		Verifier ("fichier sans charge refuse", !TT::LireCharge (TT::Joindre (source, L"moteur.exe"), e, chemins));

		// le vrai .apx produit par le build (meme dossier que le banc)
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW (nullptr, exe, MAX_PATH);
		std::wstring vrai = exe;
		vrai = vrai.substr (0, vrai.find_last_of (L'\\') + 1) + L"Tapir_Translator.apx";
		chemins.clear ();
		bool moteurDedans = false;
		if (TT::LireCharge (vrai, e, chemins)) {
			for (const std::wstring& c : chemins)
				moteurDedans = moteurDedans || c == L"TapirTranslatorMoteur.exe";
		}
		Verifier ("Tapir_Translator.apx contient TapirTranslatorMoteur.exe", moteurDedans);
		SupprimerArbre (dossier);
	}

	// --- environnement, comme Initialize () de l'add-on -------------------------------
	std::printf ("\n[1] Environnement prepare par l'add-on\n");
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
		// polonais : langue d'ArchiCAD sans colonne dans le dictionnaire commun, moteur
		// desactive -> le texte d'origine reste (seul le moteur la traduira)
		Langue (racine, "pl", false);
		Verifier ("polonais sans colonne de dictionnaire : texte d'origine", o.Attendre (Affiche ("fermer", "Fermer"), 5000));
		Texte (o, "murs", "Murs", "Murs");
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

	// --- traduction automatique hors ligne (moteur d'essai) ----------------------------------
	std::printf ("\n[3d] Traduction automatique hors ligne (moteur d'essai)\n");
	{
		const int port = 8792;	// pas celui d'un vrai moteur (8779)
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_MOTEUR_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_moteur");	// aucun dictionnaire propre
		const std::wstring journal = TT::Joindre (racine, L"journal.txt");
		const std::wstring cacheEn = TT::Joindre (TT::Joindre (TT::Joindre (racine, L"cache"), L"essai"), L"fr-en.json");
		HANDLE moteur = nullptr;
		Langue (racine, "en", false, "", true);
		{
			Outil o;
			Verifier ("lancement, moteur absent", o.Lancer (script, racine));
			Verifier ("dictionnaire applique sans moteur", o.Attendre (Affiche ("fermer", "Close"), 30000));
			Sleep (3000);
			Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");		// moteur absent : texte d'origine
			Texte (o, "logique", "ok");

			moteur = LancerMoteur (port, journal);
			Verifier ("moteur d'essai lance", moteur != nullptr);
			Verifier ("moteur revenu : phrase hors dictionnaire traduite en direct (< 25 s)",
					  o.Attendre (Affiche ("inconnu", "[en] Texte inconnu"), 25000));
			Texte (o, "inconnu", "[en] Texte inconnu", "Texte inconnu");
			// le titre peut arriver dans le lot suivant du moteur
			Verifier ("titre traduit automatiquement", o.Attendre (Affiche ("titre", "[en] Outil de démonstration"), 5000));
			Texte (o, "titre", "[en] Outil de démonstration", "Outil de démonstration");
			Texte (o, "fermer", "Close", "Fermer");						// le dictionnaire l'emporte
			Texte (o, "chemin", "C:\\Projets\\plan.pln", "C:\\Projets\\plan.pln");	// donnee : jamais envoyee
			Texte (o, "code", "MUR-01", "MUR-01");
			Texte (o, "ligne_arbre", "Murs", "Murs");
			Texte (o, "logique", "ok");

			bool enCache = false;
			for (int i = 0; i < 30 && !enCache; ++i) {
				std::string c;
				enCache = TT::LireFichier (cacheEn, c) && c.find ("\"Texte inconnu\": \"[en] Texte inconnu\"") != std::string::npos;
				if (!enCache)
					Sleep (100);
			}
			Verifier ("traduction gardee dans cache\\fr-en.json (langue fr reconnue)", enCache);

			Langue (racine, "hu", false, "", true);
			Verifier ("autre langue de sortie : nouvelle traduction automatique", o.Attendre (Affiche ("inconnu", "[hu] Texte inconnu"), 10000));
			Texte (o, "fermer", "Bezárás", "Fermer");

			Langue (racine, "en", false, "", false);
			Verifier ("moteur desactive : texte d'origine", o.Attendre (Affiche ("inconnu", "Texte inconnu"), 5000));
			Texte (o, "fermer", "Close", "Fermer");
			DWORD code = 1;
			Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		}
		ArreterMoteur (moteur);

		Langue (racine, "en", false, "", true);
		{
			Outil o;
			Verifier ("relance, moteur arrete", o.Lancer (script, racine));
			Verifier ("traduction reprise du cache, sans moteur", o.Attendre (Affiche ("inconnu", "[en] Texte inconnu"), 30000));
			// le titre est cree avant que la langue du script soit reconnue : il suit de peu
			Verifier ("titre repris du cache", o.Attendre (Affiche ("titre", "[en] Outil de démonstration"), 5000));
			Texte (o, "titre", "[en] Outil de démonstration", "Outil de démonstration");
			Texte (o, "cadre", "Options", "Options");			// deja anglais : jamais envoye au moteur
			DWORD code = 1;
			Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		}
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", nullptr);
		Langue (racine, "en", false);
	}

	// --- traducteurs en ligne (faux serveur aux formats DeepL, Microsoft, Google, MyMemory,
	//     LibreTranslate), en passant par le vrai moteur -------------------------------------
	std::printf ("\n[3e] Traducteurs en ligne (faux serveur, formats reels des 5 services)\n");
	{
		const int portServices = 8798;
		const int port = 8796;
		ServeurFactice faux;
		Verifier ("faux serveur de traduction demarre", faux.Demarrer (portServices));
		const std::wstring base = L"http://127.0.0.1:" + std::to_wstring (portServices);
		for (const wchar_t* nom : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + nom).c_str (), base.c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_MOTEUR_PORT", std::to_wstring (port).c_str ());
		SetEnvironmentVariableW (L"TAPIR_TRANSLATOR_OUTIL", L"script_en_ligne");
		const std::wstring cles = TT::CheminCles (racine);
		std::wstring erreur;
		Verifier ("modele de cles.json cree", TT::CreerModeleCles (racine, erreur) && TT::FichierExiste (cles));
		// la cle Google manque au depart
		Ecrire (cles, "{\"deepl\": {\"cle\": \"cle-deepl:fx\"}, \"microsoft\": {\"cle\": \"cle-ms\", \"region\": \"westeurope\"},"
					  " \"google\": {\"cle\": \"\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"cle\": \"\"}}");
		HANDLE moteur = LancerMoteur (port, TT::Joindre (racine, L"journal.txt"), L"--cles \"" + cles + L"\"");
		Verifier ("moteur lance avec cles.json", moteur != nullptr);
		{
			Outil o;
			Langue (racine, "en", false, "fr", true, "deepl");
			Verifier ("lancement", o.Lancer (script, racine));
			const struct { const char* code; const char* attendu; } services[] = {
				{ "deepl", "DL:EN-GB:Texte inconnu" },
				{ "microsoft", "MS:en:Texte inconnu" },
				{ "mymemory", "MM:en:Texte inconnu" },
				{ "libretranslate", "LT:en:Texte inconnu" },
			};
			for (const auto& s : services) {
				Langue (racine, "en", false, "fr", true, s.code);
				Verifier (std::string (s.code) + " : phrase traduite (format de requete et de reponse du service)",
						  o.Attendre (Affiche ("inconnu", s.attendu), 20000), o.Lire ("inconnu").affiche);
				Texte (o, "fermer", "Close", "Fermer");			// dictionnaire d'abord
			}
			Texte (o, "logique", "ok");

			Langue (racine, "en", false, "fr", true, "google");
			Verifier ("google sans cle : texte d'origine garde", o.Attendre (Affiche ("inconnu", "Texte inconnu"), 10000));
			Sleep (2000);
			Texte (o, "inconnu", "Texte inconnu", "Texte inconnu");
			Ecrire (cles, "{\"deepl\": {\"cle\": \"cle-deepl:fx\"}, \"microsoft\": {\"cle\": \"cle-ms\", \"region\": \"westeurope\"},"
						  " \"google\": {\"cle\": \"cle-google\"}, \"mymemory\": {\"email\": \"\"}, \"libretranslate\": {\"url\": \"\", \"cle\": \"\"}}");
			Verifier ("cle Google ajoutee a chaud : traduit au nouvel essai (< 30 s)",
					  o.Attendre (Affiche ("inconnu", "GG:en:Texte inconnu"), 30000), o.Lire ("inconnu").affiche);

			Langue (racine, "en", false, "fr", true, "deepl");
			Verifier ("retour a DeepL : son cache redonne sa traduction", o.Attendre (Affiche ("inconnu", "DL:EN-GB:Texte inconnu"), 10000));
			Texte (o, "logique", "ok");
			DWORD code = 1;
			Verifier ("fermeture propre", o.Finir (code, 10000) && code == 0);
		}
		Verifier ("aucun texte du dictionnaire envoye en ligne (\"Fermer\")", !faux.ARecu ("Fermer"));
		Verifier ("aucune donnee envoyee en ligne (chemin, code)", !faux.ARecu ("C:\\Projets\\plan.pln") && !faux.ARecu ("MUR-01"));
		const std::vector<std::string> journal = TT::DernieresLignes (TT::Joindre (racine, L"journal.txt"), 300);
		bool signale = false;
		for (const std::string& l : journal)
			signale = signale || l.find ("cle Google absente") != std::string::npos;
		Verifier ("journal : la cle Google manquante est expliquee", signale);
		ArreterMoteur (moteur);
		faux.Arreter ();
		for (const wchar_t* nom : { L"DEEPL", L"MICROSOFT", L"GOOGLE", L"MYMEMORY", L"LIBRETRANSLATE" })
			SetEnvironmentVariableW ((std::wstring (L"TAPIR_TRANSLATOR_URL_") + nom).c_str (), nullptr);
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
