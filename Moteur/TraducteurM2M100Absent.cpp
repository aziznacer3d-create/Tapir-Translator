// *****************************************************************************
// Tapir Translator - moteur compile sans CTranslate2 / SentencePiece
//
//   Utilise quand _externe/ct2_install ou SentencePiece (vcpkg x64-windows-static)
//   manquent : le moteur ne sait alors que le mode --essai.
// *****************************************************************************

#include "Traducteur.hpp"

std::unique_ptr<Traducteur> CreerTraducteurM2M100 (const std::string& /*dossier*/, std::string& erreur)
{
	erreur = "moteur compile sans CTranslate2 (voir README, section Moteur)";
	return nullptr;
}
