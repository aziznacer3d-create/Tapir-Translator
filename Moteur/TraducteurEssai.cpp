// *****************************************************************************
// Tapir Translator - moteur : traducteur d'essai (banc de test)
//
//   Ne traduit rien : rend "[cible] texte". Il permet de verifier toute la
//   chaine (couche Python -> serveur -> cache -> reaffichage en direct) sans
//   modele. Une petite attente imite le temps de calcul d'un vrai modele.
// *****************************************************************************

#include "Traducteur.hpp"

#include <chrono>
#include <thread>

namespace {

class TraducteurEssai : public Traducteur {
public:
	std::string Nom () const override { return "essai (aucun modele)"; }

	bool Prend (const std::string& langue) const override
	{
		static const char* const langues[] = { "fr", "en", "de", "es", "pt", "it", "hu", "ar" };
		for (const char* l : langues) {
			if (langue == l)
				return true;
		}
		return false;
	}

	bool Traduire (const std::string& /*source*/, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& /*erreur*/) override
	{
		std::this_thread::sleep_for (std::chrono::milliseconds (200));
		resultats.clear ();
		for (const std::string& t : textes)
			resultats.push_back ("[" + cible + "] " + t);
		return true;
	}
};

}	// namespace


std::unique_ptr<Traducteur> CreerTraducteurEssai ()
{
	return std::make_unique<TraducteurEssai> ();
}
