// *****************************************************************************
// Tapir Translator - engine: test translator (test bench, --test)
//
//   Translates nothing: returns "[target] text" (e.g. "[en] Fermer"). It lets
//   the test bench check the whole chain (Python layer -> server -> cache ->
//   live redisplay) without any model or network access. A short wait mimics
//   the computing time of a real translator.
// *****************************************************************************

#include "Translator.hpp"

#include <chrono>
#include <thread>

namespace {

// Test translator: prefixes each text with the target language code.
class TestTranslator : public Translator {
public:
	std::string Name () const override { return "test (no model)"; }

	bool Supports (const std::string& language) const override { return ArchicadLanguage (language); }

	// Never fails; the source language is ignored.
	bool Translate (const std::string& /*source*/, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& /*error*/) override
	{
		// 200 ms per batch, like a (fast) real translation
		std::this_thread::sleep_for (std::chrono::milliseconds (200));
		results.clear ();
		for (const std::string& t : texts)
			results.push_back ("[" + target + "] " + t);
		return true;
	}
};

}	// namespace


// Creates the test translator (see Translator.hpp).
std::unique_ptr<Translator> CreateTestTranslator ()
{
	return std::make_unique<TestTranslator> ();
}
