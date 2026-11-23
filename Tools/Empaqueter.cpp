// *****************************************************************************
// Tapir Translator - Empaqueter : ajoute le moteur et le modele a la fin du .apx
//
//   Usage : Empaqueter.exe <fichier.apx> <dossier>
//   Lance apres chaque edition de liens de l'add-on (CMakeLists.txt). Voir
//   Src/TT_Charge.hpp pour le format.
// *****************************************************************************

#include "TT_Charge.hpp"
#include "TT_Coeur.hpp"

#include <cstdio>
#include <string>
#include <vector>

int wmain (int argc, wchar_t** argv)
{
	if (argc != 3) {
		std::fprintf (stderr, "usage : Empaqueter <fichier.apx> <dossier>\n");
		return 2;
	}
	std::wstring empreinte;
	std::vector<std::wstring> journal;
	const bool ok = TT::Empaqueter (argv[1], argv[2], empreinte, journal);
	for (const std::wstring& l : journal)
		std::printf ("Empaqueter : %s\n", TT::Utf8 (l).c_str ());
	if (!ok) {
		std::fprintf (stderr, "Empaqueter : ECHEC\n");
		return 1;
	}
	return 0;
}
