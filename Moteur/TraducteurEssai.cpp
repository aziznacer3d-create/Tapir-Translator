// *****************************************************************************
// Tapir Translator - engine: test translator (test bench, --essai)
//
//   Translates nothing: returns "[cible] texte" (e.g. "[en] Fermer"). It lets
//   the test bench check the whole chain (Python layer -> server -> cache ->
//   live redisplay) without any model or network access. A short wait mimics
//   the computing time of a real translator.
// *****************************************************************************

#include "Traducteur.hpp"

#include <chrono>
#include <thread>

namespace {

// Test translator: prefixes each text with the target language code.
class TraducteurEssai : public Traducteur {
public:
	std::string Nom () const override { return "essai (aucun modele)"; }

	bool Prend (const std::string& langue) const override { return LangueArchicad (langue); }

	// Never fails; the source language is ignored.
	bool Traduire (const std::string& /*source*/, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& /*erreur*/) override
	{
		// 200 ms per batch, like a (fast) real translation
		std::this_thread::sleep_for (std::chrono::milliseconds (200));
		resultats.clear ();
		for (const std::string& t : textes)
			resultats.push_back ("[" + cible + "] " + t);
		return true;
	}
};

}	// namespace


// Creates the test translator (see Traducteur.hpp).
std::unique_ptr<Traducteur> CreerTraducteurEssai ()
{
	return std::make_unique<TraducteurEssai> ();
}
