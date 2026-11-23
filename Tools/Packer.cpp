// *****************************************************************************
// Tapir Translator - Packer: appends the payload to the end of the .apx
//
//   Usage: Packer.exe <file.apx> <folder>
//   Build tool, run after every link of the add-on (POST_BUILD step in
//   CMakeLists.txt). The folder holds the payload prepared by CMake: the
//   engine TapirTranslatorEngine.exe, the licence texts and, with
//   TT_LOCAL_MODEL, the offline model. The whole folder is appended to the
//   .apx, so the user installs a single file; the add-on extracts the payload
//   on first start. The payload format and the packing itself are in
//   Src/TT_Payload.hpp / TT_Payload.cpp (TT::Packer).
//
//   Exit codes: 0 success, 1 packing failed, 2 wrong arguments.
// *****************************************************************************

#include "TT_Payload.hpp"
#include "TT_Core.hpp"

#include <cstdio>
#include <string>
#include <vector>

// Entry point (wide arguments: paths may contain non-ASCII characters).
// Step 1: check the arguments.
// Step 2: pack the folder into the .apx (TT::Packer also computes the
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
	std::wstring hash;
	std::vector<std::wstring> journal;
	const bool ok = TT::Packer (argv[1], argv[2], hash, journal);
	// Step 3
	for (const std::wstring& l : journal)
		std::printf ("Empaqueter : %s\n", TT::Utf8 (l).c_str ());
	if (!ok) {
		std::fprintf (stderr, "Empaqueter : ECHEC\n");
		return 1;
	}
	return 0;
}
