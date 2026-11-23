// *****************************************************************************
// Tapir Translator - moteur : client HTTP(S) WinHTTP
//
//   Passe par la pile reseau de Windows : proxy du systeme (WPAD / reglages
//   Windows) et certificats du magasin Windows, ce qui marche sur un reseau
//   d'entreprise qui inspecte le TLS.
// *****************************************************************************

#ifndef TT_MOTEUR_HTTP_HPP
#define TT_MOTEUR_HTTP_HPP

#include <string>
#include <vector>

namespace Http {

struct Reponse {
	int code = 0;			// code HTTP, 0 = pas de reponse (reseau, nom, delai)
	std::string corps;
	std::string erreur;		// explication si code == 0
};

// url : http://... ou https://... ; entetes : "Nom: valeur"
Reponse Requete (const std::string& methode, const std::string& url, const std::vector<std::string>& entetes,
				 const std::string& corps, int delaiMs = 20000);

// Encodage d'un parametre d'URL (UTF-8, RFC 3986)
std::string EncoderUrl (const std::string& texte);

}	// namespace Http

#endif
