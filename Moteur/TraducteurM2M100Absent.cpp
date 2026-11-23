// *****************************************************************************
// Tapir Translator - engine built without CTranslate2 / SentencePiece
//
//   Compiled instead of TraducteurM2M100.cpp when the build option
//   TT_MODELE_LOCAL is OFF (the default), or when _externe/ct2_install or
//   SentencePiece (vcpkg x64-windows-static) are missing (see CMakeLists.txt).
//   The engine then has no offline translator: only the online providers and
//   the --essai test translator.
// *****************************************************************************

#include "Traducteur.hpp"

// Stub: always fails with an explanation, so --modele only logs
// "modele local indisponible" and the engine keeps running.
std::unique_ptr<Traducteur> CreerTraducteurM2M100 (const std::string& /*dossier*/, std::string& erreur)
{
	erreur = "moteur compile sans CTranslate2 (voir README, section Moteur)";
	return nullptr;
}
