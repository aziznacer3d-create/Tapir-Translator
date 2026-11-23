// *****************************************************************************
// Tapir Translator - engine: interface of a machine translator
//
//   The server (EngineMain.cpp) only knows this interface. Implementations:
//     - TestTranslator.cpp     test translator for the test bench (--test);
//     - OnlineTranslators.cpp  the five online providers (DeepL, Microsoft,
//                               Google, MyMemory, LibreTranslate);
//     - TranslatorM2M100.cpp    optional offline model (build option
//                               TT_LOCAL_MODEL), replaced by
//                               TranslatorM2M100Missing.cpp when it is off.
//   Also declares the list of the 20 ArchiCAD languages the engine accepts.
// *****************************************************************************

#ifndef TT_ENGINE_TRANSLATOR_HPP
#define TT_ENGINE_TRANSLATOR_HPP

#include <memory>
#include <string>
#include <vector>

// Languages of the ArchiCAD 29 localised versions (codes of TT::Languages), all
// supported by M2M100. These are the only codes accepted by POST /translate.
inline const char* const ArchicadLanguages[] = { "fr", "en", "de", "es", "pt", "it", "hu", "cs", "pl", "ru",
											   "uk", "el", "tr", "nl", "sv", "no", "fi", "ja", "ko", "zh" };

// True if language is one of ArchicadLanguages (exact, lower-case code).
inline bool ArchicadLanguage (const std::string& language)
{
	for (const char* l : ArchicadLanguages) {
		if (language == l)
			return true;
	}
	return false;
}

// Abstract machine translator.
class Translator {
public:
	virtual ~Translator () = default;

	// Human-readable name, used in the engine's log messages and error answers
	virtual std::string Nom () const = 0;

	// True if the language is supported (codes of TT::Languages; every current
	// implementation accepts the 20 ArchicadLanguages)
	virtual bool Supports (const std::string& language) const = 0;

	// Translates each text (UTF-8) from source to target.
	//   source    : language code, or "" = unknown (detect automatically)
	//   target     : target language code
	//   texts    : texts to translate
	//   results : output, one translation per text, in the same order
	//   error    : output, the reason when false is returned
	// Returns false and fills error if the whole batch failed; a single text
	// that cannot be translated is returned unchanged.
	virtual bool Translate (const std::string& source, const std::string& target,
						   const std::vector<std::string>& texts, std::vector<std::string>& results,
						   std::string& error) = 0;
};

// --test: returns "[target] text" (no model: tests the whole chain on the test bench)
std::unique_ptr<Translator> CreateTestTranslator ();

// --model <folder>: M2M100 418M in CTranslate2 format (TranslatorM2M100.cpp, build option
// TT_LOCAL_MODEL, off by default). Returns nullptr and fills error if the model cannot be
// loaded, or always when the engine was built without it (TranslatorM2M100Missing.cpp).
std::unique_ptr<Translator> CreateTranslatorM2M100 (const std::string& folder, std::string& error);

// Online translators (OnlineTranslators.cpp): "deepl", "microsoft", "google", "mymemory",
// "libretranslate". keys = parsed content of keys.json. Returns nullptr for an unknown name.
// Json::Value is only forward-declared here so that this header does not pull in Json.hpp.
namespace Json { struct Value; }
extern const char* const OnlineProviders[5];
std::unique_ptr<Translator> CreateOnlineTranslator (const std::string& name, const Json::Value& keys);

#endif
