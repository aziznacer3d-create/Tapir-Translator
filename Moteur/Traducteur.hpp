// *****************************************************************************
// Tapir Translator - engine: interface of a machine translator
//
//   The server (MoteurMain.cpp) only knows this interface. Implementations:
//     - TraducteurEssai.cpp     test translator for the test bench (--essai);
//     - TraducteursEnLigne.cpp  the five online providers (DeepL, Microsoft,
//                               Google, MyMemory, LibreTranslate);
//     - TraducteurM2M100.cpp    optional offline model (build option
//                               TT_MODELE_LOCAL), replaced by
//                               TraducteurM2M100Absent.cpp when it is off.
//   Also declares the list of the 20 ArchiCAD languages the engine accepts.
// *****************************************************************************

#ifndef TT_MOTEUR_TRADUCTEUR_HPP
#define TT_MOTEUR_TRADUCTEUR_HPP

#include <memory>
#include <string>
#include <vector>

// Languages of the ArchiCAD 29 localised versions (codes of TT::Langues), all
// supported by M2M100. These are the only codes accepted by POST /traduire.
inline const char* const LanguesArchicad[] = { "fr", "en", "de", "es", "pt", "it", "hu", "cs", "pl", "ru",
											   "uk", "el", "tr", "nl", "sv", "no", "fi", "ja", "ko", "zh" };

// True if langue is one of LanguesArchicad (exact, lower-case code).
inline bool LangueArchicad (const std::string& langue)
{
	for (const char* l : LanguesArchicad) {
		if (langue == l)
			return true;
	}
	return false;
}

// Abstract machine translator.
class Traducteur {
public:
	virtual ~Traducteur () = default;

	// Human-readable name, used in the engine's log messages and error answers
	virtual std::string Nom () const = 0;

	// True if the language is supported (codes of TT::Langues; every current
	// implementation accepts the 20 LanguesArchicad)
	virtual bool Prend (const std::string& langue) const = 0;

	// Translates each text (UTF-8) from source to cible.
	//   source    : language code, or "" = unknown (detect automatically)
	//   cible     : target language code
	//   textes    : texts to translate
	//   resultats : output, one translation per text, in the same order
	//   erreur    : output, the reason when false is returned
	// Returns false and fills erreur if the whole batch failed; a single text
	// that cannot be translated is returned unchanged.
	virtual bool Traduire (const std::string& source, const std::string& cible,
						   const std::vector<std::string>& textes, std::vector<std::string>& resultats,
						   std::string& erreur) = 0;
};

// --essai: returns "[cible] texte" (no model: tests the whole chain on the test bench)
std::unique_ptr<Traducteur> CreerTraducteurEssai ();

// --modele <dossier>: M2M100 418M in CTranslate2 format (TraducteurM2M100.cpp, build option
// TT_MODELE_LOCAL, off by default). Returns nullptr and fills erreur if the model cannot be
// loaded, or always when the engine was built without it (TraducteurM2M100Absent.cpp).
std::unique_ptr<Traducteur> CreerTraducteurM2M100 (const std::string& dossier, std::string& erreur);

// Online translators (TraducteursEnLigne.cpp): "deepl", "microsoft", "google", "mymemory",
// "libretranslate". cles = parsed content of cles.json. Returns nullptr for an unknown name.
// Json::Valeur is only forward-declared here so that this header does not pull in Json.hpp.
namespace Json { struct Valeur; }
extern const char* const FournisseursEnLigne[5];
std::unique_ptr<Traducteur> CreerTraducteurEnLigne (const std::string& nom, const Json::Valeur& cles);

#endif
