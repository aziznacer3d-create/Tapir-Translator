// *****************************************************************************
// Tapir Translator - Empaqueter: appends the payload to the end of the .apx
//
//   Usage: Empaqueter.exe <file.apx> <folder>
//   Build tool, run after every link of the add-on (POST_BUILD step in
//   CMakeLists.txt). The folder holds the payload prepared by CMake: the
//   engine TapirTranslatorMoteur.exe, the licence texts and, with
//   TT_MODELE_LOCAL, the offline model. The whole folder is appended to the
//   .apx, so the user installs a single file; the add-on extracts the payload
//   on first start. The payload format and the packing itself are in
//   Src/TT_Charge.hpp / TT_Charge.cpp (TT::Empaqueter).
//
//   Exit codes: 0 success, 1 packing failed, 2 wrong arguments.
// *****************************************************************************

#include "TT_Charge.hpp"
#include "TT_Coeur.hpp"

#include <cstdio>
#include <string>
#include <vector>

// Entry point (wide arguments: paths may contain non-ASCII characters).
// Step 1: check the arguments.
// Step 2: pack the folder into the .apx (TT::Empaqueter also computes the
//         payload fingerprint and fills a log).
// Step 3: print the log lines (UTF-8) for the build output, then the result.
int wmain (int argc, wchar_t** argv)
{
	// Step 1
	if (argc != 3) {
		std::fprintf (stderr, "usage : Empaqueter <fichier.apx> <dossier>\n");
		return 2;
	}
	// Step 2
	std::wstring empreinte;
	std::vector<std::wstring> journal;
	const bool ok = TT::Empaqueter (argv[1], argv[2], empreinte, journal);
	// Step 3
	for (const std::wstring& l : journal)
		std::printf ("Empaqueter : %s\n", TT::Utf8 (l).c_str ());
	if (!ok) {
		std::fprintf (stderr, "Empaqueter : ECHEC\n");
		return 1;
	}
	return 0;
}
