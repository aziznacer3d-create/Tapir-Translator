// *****************************************************************************
// Tapir Translator - charge embarquee a la fin du .apx (moteur + modele)
//
//   L'utilisateur ne copie qu'un fichier : Tapir_Translator.apx. Au build,
//   Tools/Empaqueter.cpp ajoute APRES le contenu du .apx (zone que Windows
//   ignore au chargement d'une DLL) les fichiers d'un dossier, puis un index et
//   un pied de 32 octets :
//
//     [ .apx ][ fichier 1 ][ fichier 2 ]...[ index UTF-8 ][ pied ]
//
//     index : "empreinte\t<16 hex>\n" puis "<chemin relatif>\t<debut>\t<taille>\n"
//     pied  : "TTCHARGE" | debut de l'index (u64) | taille de l'index (u64) |
//             debut de la charge (u64)
//
//   Au demarrage, l'add-on extrait la charge une seule fois dans
//   <base>\<empreinte>\ (marque ".complet") : une autre version du .apx a une
//   autre empreinte et s'extrait a cote.
//
//   Sans ArchiCAD (Win32) : partage par l'add-on, l'outil Empaqueter et le banc.
// *****************************************************************************

#ifndef TT_CHARGE_HPP
#define TT_CHARGE_HPP

#include <string>
#include <vector>

namespace TT {

// Ajoute tous les fichiers de 'dossier' (sous-dossiers compris) a la fin de 'cible'.
// Une charge deja presente est d'abord retiree.
bool Empaqueter (const std::wstring& cible, const std::wstring& dossier, std::wstring& empreinte,
				 std::vector<std::wstring>& journal);

// Rend true et l'empreinte si 'fichier' porte une charge
bool LireCharge (const std::wstring& fichier, std::wstring& empreinte, std::vector<std::wstring>& chemins);

// Extrait la charge de 'fichier' dans <base>\<empreinte>\ si ce n'est pas deja fait.
// 'destination' recoit le dossier extrait.
bool ExtraireCharge (const std::wstring& fichier, const std::wstring& base, std::wstring& destination,
					 std::vector<std::wstring>& journal);

}	// namespace TT

#endif
