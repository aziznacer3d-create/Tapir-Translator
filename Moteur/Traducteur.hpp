// *****************************************************************************
// Tapir Translator - moteur : interface d'un traducteur automatique
//
//   Le serveur (MoteurMain.cpp) ne connait que cette interface. Le traducteur
//   d'essai (TraducteurEssai.cpp) sert au banc ; le traducteur reel (modele hors
//   ligne, licence libre de droit) se branche ici.
// *****************************************************************************

#ifndef TT_MOTEUR_TRADUCTEUR_HPP
#define TT_MOTEUR_TRADUCTEUR_HPP

#include <memory>
#include <string>
#include <vector>

class Traducteur {
public:
	virtual ~Traducteur () = default;

	// Nom affiche dans /etat et dans le Rapport de session
	virtual std::string Nom () const = 0;

	// Langues prises en charge (codes de TT::Langues : fr en de es pt it hu ar)
	virtual bool Prend (const std::string& langue) const = 0;

	// Traduit chaque texte (UTF-8) de source vers cible. Rend false et remplit
	// erreur si le lot entier a echoue ; un texte intraduisible est rendu tel quel.
	virtual bool Traduire (const std::string& source, const std::string& cible,
						   const std::vector<std::string>& textes, std::vector<std::string>& resultats,
						   std::string& erreur) = 0;
};

// --essai : rend "[cible] texte" (aucun modele : banc de test de la chaine complete)
std::unique_ptr<Traducteur> CreerTraducteurEssai ();

#endif
