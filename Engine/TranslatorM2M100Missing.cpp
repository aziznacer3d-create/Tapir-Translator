// *****************************************************************************
// Tapir Translator - engine built without CTranslate2 / SentencePiece
//
//   Compiled instead of TranslatorM2M100.cpp when the build option
//   TT_LOCAL_MODEL is OFF (the default), or when _external/ct2_install or
//   SentencePiece (vcpkg x64-windows-static) are missing (see CMakeLists.txt).
//   The engine then has no offline translator: only the online providers and
//   the --test test translator.
// *****************************************************************************

#include "Translator.hpp"

// Stub: always fails with an explanation, so --model only logs
// "model local unavailable" and the engine keeps running.
std::unique_ptr<Translator> CreateTranslatorM2M100 (const std::string& /*folder*/, std::string& error)
{
	error = "engine built without CTranslate2 (see README, Engine section)";
	return nullptr;
}
