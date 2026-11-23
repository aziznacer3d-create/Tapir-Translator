// *****************************************************************************
// Tapir Translator - moteur de traduction (TapirTranslatorMoteur.exe)
//
//   Petit serveur HTTP local (127.0.0.1 seulement), lance par l'add-on au
//   demarrage d'ArchiCAD. Il relaie vers le traducteur en ligne choisi et sert tous les
//   scripts Tapir : la couche Python lui envoie, en arriere-plan, les textes
//   que les dictionnaires ne connaissent pas.
//
//   GET  /etat      -> {"nom", "version", "langues"}
//   POST /traduire  {"fournisseur": "deepl", "source": "fr" | "", "cible": "en", "textes": [...]}
//                   -> {"traductions": [...]}   (meme ordre, meme nombre)
//   POST /arreter   -> arret du serveur
//
//   Options : --port N (8779)  --cles cles.json  --essai  --parent PID (s'arrete avec ArchiCAD)
//             --journal fichier
// *****************************************************************************

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "Json.hpp"
#include "Traducteur.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#pragma comment (lib, "Ws2_32.lib")

namespace {

const char* const kVersion = "0.5.0";
std::string g_journal;

void Journal (const std::string& ligne)
{
	if (g_journal.empty ())
		return;
	FILE* f = nullptr;
	if (fopen_s (&f, g_journal.c_str (), "ab") != 0 || f == nullptr)
		return;
	char date[32];
	std::time_t t = std::time (nullptr);
	std::tm tm = {};
	localtime_s (&tm, &t);
	std::strftime (date, sizeof (date), "%Y-%m-%d %H:%M:%S", &tm);
	std::fprintf (f, "%s [moteur %lu] %s\n", date, static_cast<unsigned long> (GetCurrentProcessId ()), ligne.c_str ());
	std::fclose (f);
}

bool EnvoyerTout (SOCKET s, const std::string& donnees)
{
	size_t envoye = 0;
	while (envoye < donnees.size ()) {
		const int n = send (s, donnees.data () + envoye, static_cast<int> (donnees.size () - envoye), 0);
		if (n <= 0)
			return false;
		envoye += static_cast<size_t> (n);
	}
	return true;
}

void Repondre (SOCKET s, int code, const std::string& json)
{
	const char* statut = code == 200 ? "OK" : (code == 400 ? "Bad Request" : (code == 404 ? "Not Found" : "Internal Server Error"));
	std::string r = "HTTP/1.1 " + std::to_string (code) + " " + statut + "\r\n";
	r += "Content-Type: application/json; charset=utf-8\r\n";
	r += "Content-Length: " + std::to_string (json.size ()) + "\r\n";
	r += "Connection: close\r\n\r\n";
	r += json;
	EnvoyerTout (s, r);
}

// Lit une requete HTTP complete (en-tetes + corps selon Content-Length)
bool LireRequete (SOCKET s, std::string& methode, std::string& chemin, std::string& corps)
{
	std::string tampon;
	char morceau[8192];
	size_t finEntetes = std::string::npos;
	while (finEntetes == std::string::npos) {
		const int n = recv (s, morceau, sizeof (morceau), 0);
		if (n <= 0)
			return false;
		tampon.append (morceau, static_cast<size_t> (n));
		finEntetes = tampon.find ("\r\n\r\n");
		if (tampon.size () > 65536 && finEntetes == std::string::npos)
			return false;
	}
	const std::string entetes = tampon.substr (0, finEntetes);
	const size_t sp1 = entetes.find (' ');
	const size_t sp2 = sp1 == std::string::npos ? sp1 : entetes.find (' ', sp1 + 1);
	if (sp2 == std::string::npos)
		return false;
	methode = entetes.substr (0, sp1);
	chemin = entetes.substr (sp1 + 1, sp2 - sp1 - 1);

	size_t longueur = 0;
	std::string minuscules = entetes;
	for (char& c : minuscules)
		c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	const size_t pos = minuscules.find ("content-length:");
	if (pos != std::string::npos)
		longueur = static_cast<size_t> (std::strtoul (minuscules.c_str () + pos + 15, nullptr, 10));
	if (longueur > 4 * 1024 * 1024)
		return false;
	corps = tampon.substr (finEntetes + 4);
	while (corps.size () < longueur) {
		const int n = recv (s, morceau, sizeof (morceau), 0);
		if (n <= 0)
			return false;
		corps.append (morceau, static_cast<size_t> (n));
	}
	corps.resize (longueur);
	return true;
}

struct Contexte {
	std::unique_ptr<Traducteur> essai;	// --essai (banc)
	std::unique_ptr<Traducteur> local;	// --modele (option de build TT_MODELE_LOCAL)
	std::string cles;					// --cles : cles.json des traducteurs en ligne
};

Json::Valeur LireCles (const std::string& chemin)
{
	Json::Valeur v;
	if (chemin.empty ())
		return v;
	FILE* f = nullptr;
	if (fopen_s (&f, chemin.c_str (), "rb") != 0 || f == nullptr)
		return v;
	std::string contenu;
	char tampon[4096];
	size_t n = 0;
	while ((n = std::fread (tampon, 1, sizeof (tampon), f)) > 0)
		contenu.append (tampon, n);
	std::fclose (f);
	if (contenu.size () >= 3 && contenu.compare (0, 3, "\xEF\xBB\xBF") == 0)
		contenu.erase (0, 3);
	if (!Json::Lecteur (contenu).Lire (v))
		Journal ("cles.json illisible (JSON invalide) : " + chemin);
	return v;
}

std::string Etat (const Contexte& c)
{
	std::string fournisseurs;
	for (const char* f : FournisseursEnLigne)
		fournisseurs += std::string (fournisseurs.empty () ? "" : ", ") + Json::Chaine (f);
	if (c.local != nullptr)
		fournisseurs += ", \"local\"";
	if (c.essai != nullptr)
		fournisseurs += ", \"essai\"";
	std::string langues;
	for (const char* l : LanguesArchicad)
		langues += std::string (langues.empty () ? "" : ", ") + Json::Chaine (l);
	return "{\"version\": " + Json::Chaine (kVersion) + ", \"fournisseurs\": [" + fournisseurs + "], \"langues\": [" + langues + "]}";
}

// Rend false pour arreter le serveur
bool Traiter (SOCKET s, Contexte& c)
{
	std::string methode, chemin, corps;
	if (!LireRequete (s, methode, chemin, corps)) {
		Repondre (s, 400, "{\"erreur\": \"requete illisible\"}");
		return true;
	}
	if (methode == "GET" && chemin == "/etat") {
		Repondre (s, 200, Etat (c));
		return true;
	}
	if (methode == "POST" && chemin == "/arreter") {
		Repondre (s, 200, "{\"arret\": true}");
		return false;
	}
	if (methode != "POST" || chemin != "/traduire") {
		Repondre (s, 404, "{\"erreur\": \"chemin inconnu\"}");
		return true;
	}

	Json::Valeur requete;
	if (!Json::Lecteur (corps).Lire (requete) || requete.genre != Json::Valeur::Objet) {
		Repondre (s, 400, "{\"erreur\": \"JSON illisible\"}");
		return true;
	}
	const std::string source = requete.Texte ("source");
	const std::string cible = requete.Texte ("cible");
	const std::string fournisseur = requete.Texte ("fournisseur", "mymemory");
	const Json::Valeur* textes = requete.Champ ("textes");
	if (cible.empty () || !LangueArchicad (cible) || (!source.empty () && !LangueArchicad (source)) ||
		textes == nullptr || textes->genre != Json::Valeur::Tableau) {
		Repondre (s, 400, "{\"erreur\": \"langue non prise en charge ou textes absents\"}");
		return true;
	}

	std::unique_ptr<Traducteur> enLigne;
	Traducteur* traducteur = nullptr;
	if (fournisseur == "essai")
		traducteur = c.essai.get ();
	else if (fournisseur == "local")
		traducteur = c.local.get ();
	else {
		enLigne = CreerTraducteurEnLigne (fournisseur, LireCles (c.cles));
		traducteur = enLigne.get ();
	}
	if (traducteur == nullptr) {
		Repondre (s, 400, "{\"erreur\": " + Json::Chaine ("traducteur inconnu ou indisponible : " + fournisseur) + "}");
		return true;
	}

	std::vector<std::string> entree;
	for (const Json::Valeur& v : textes->tableau)
		entree.push_back (v.genre == Json::Valeur::Chaine ? v.chaine : std::string ());

	std::vector<std::string> sortie;
	std::string erreur;
	const DWORD debut = GetTickCount ();
	if (!traducteur->Traduire (source, cible, entree, sortie, erreur) || sortie.size () != entree.size ()) {
		// 503 : la couche garde le texte d'origine et reessaiera (reseau, quota, cle...)
		Journal ("ERREUR " + traducteur->Nom () + " " + (source.empty () ? "auto" : source) + " -> " + cible + " : " + erreur);
		Repondre (s, 503, "{\"erreur\": " + Json::Chaine (traducteur->Nom () + " : " + (erreur.empty () ? "traduction impossible" : erreur)) + "}");
		return true;
	}
	Journal (traducteur->Nom () + " : " + std::to_string (entree.size ()) + " texte(s) " + (source.empty () ? "auto" : source) +
			 " -> " + cible + " en " + std::to_string (GetTickCount () - debut) + " ms");
	std::string json = "{\"traductions\": [";
	for (size_t i = 0; i < sortie.size (); ++i)
		json += (i == 0 ? "" : ", ") + Json::Chaine (sortie[i]);
	json += "]}";
	Repondre (s, 200, json);
	return true;
}

}	// namespace


int main (int argc, char** argv)
{
	int port = 8779;
	Contexte contexte;
	std::string modele;
	DWORD parent = 0;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--port" && i + 1 < argc)
			port = std::atoi (argv[++i]);
		else if (a == "--essai")
			contexte.essai = CreerTraducteurEssai ();
		else if (a == "--modele" && i + 1 < argc)
			modele = argv[++i];
		else if (a == "--cles" && i + 1 < argc)
			contexte.cles = argv[++i];
		else if (a == "--parent" && i + 1 < argc)
			parent = static_cast<DWORD> (std::strtoul (argv[++i], nullptr, 10));
		else if (a == "--journal" && i + 1 < argc)
			g_journal = argv[++i];
	}
	if (!modele.empty ()) {
		std::string erreur;
		contexte.local = CreerTraducteurM2M100 (modele, erreur);
		if (contexte.local == nullptr)
			Journal ("modele local indisponible : " + erreur);
	}
	// S'arrete avec ArchiCAD
	if (parent != 0) {
		HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, parent);
		if (h != nullptr) {
			std::thread ([h] () {
				WaitForSingleObject (h, INFINITE);
				Journal ("ArchiCAD ferme : arret");
				ExitProcess (0);
			}).detach ();
		}
	}

	WSADATA wsa;
	if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0)
		return 4;
	SOCKET ecoute = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in adresse = {};
	adresse.sin_family = AF_INET;
	adresse.sin_port = htons (static_cast<u_short> (port));
	inet_pton (AF_INET, "127.0.0.1", &adresse.sin_addr);
	// pas de SO_REUSEADDR : un second moteur sur le meme port doit echouer (exclusif)
	BOOL exclusif = TRUE;
	setsockopt (ecoute, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*> (&exclusif), sizeof (exclusif));
	if (bind (ecoute, reinterpret_cast<sockaddr*> (&adresse), sizeof (adresse)) != 0 || listen (ecoute, 16) != 0) {
		Journal ("port " + std::to_string (port) + " deja pris : un moteur tourne sans doute deja");
		closesocket (ecoute);
		WSACleanup ();
		return 2;
	}
	Journal ("pret sur 127.0.0.1:" + std::to_string (port) + " (traducteurs en ligne, cles : " + (contexte.cles.empty () ? std::string ("aucune") : contexte.cles) + ")");
	std::printf ("PRET %d\n", port);
	std::fflush (stdout);

	for (;;) {
		SOCKET client = accept (ecoute, nullptr, nullptr);
		if (client == INVALID_SOCKET)
			continue;
		DWORD delai = 10000;
		setsockopt (client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*> (&delai), sizeof (delai));
		const bool continuer = Traiter (client, contexte);
		shutdown (client, SD_BOTH);
		closesocket (client);
		if (!continuer)
			break;
	}
	Journal ("arret demande");
	closesocket (ecoute);
	WSACleanup ();
	return 0;
}
