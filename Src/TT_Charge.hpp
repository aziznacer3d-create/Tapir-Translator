// *****************************************************************************
// Tapir Translator - payload appended to the end of the .apx (engine + model)
//
//   ROLE IN THE SYSTEM
//   The user copies a single file: Tapir_Translator.apx. At build time,
//   Tools/Empaqueter.cpp appends, AFTER the normal content of the .apx (an area
//   that Windows ignores when it loads a DLL), the files of a folder (the engine,
//   the optional model, the licence texts), then an index and a 32-byte footer.
//
//   PAYLOAD FORMAT
//
//     [ .apx ][ file 1 ][ file 2 ]...[ UTF-8 index ][ footer ]
//
//     index : "empreinte\t<16 hex digits>\n" (the hash line), then one line per
//             file: "<relative path>\t<start offset>\t<size>\n"
//             (offsets are absolute positions in the .apx, sizes in bytes)
//     footer: "TTCHARGE" (8 bytes) | index start (u64) | index size (u64) |
//             payload start (u64)        - all u64 are little-endian
//
//     The hash is a 64-bit FNV-1a over every relative path and file content.
//
//   EXTRACTION
//   At start-up the add-on extracts the payload only once, into
//   <base>\<hash>\ (completion marker file ".complet"): another version of the
//   .apx has another hash and is extracted next to it.
//
//   No ArchiCAD dependency (Win32 only): shared by the add-on, the Empaqueter
//   tool and the test bench.
// *****************************************************************************

#ifndef TT_CHARGE_HPP
#define TT_CHARGE_HPP

#include <string>
#include <vector>

namespace TT {

// Build-time packing: appends every file of 'dossier' (sub-folders included) to the end of
// 'cible', followed by the index and the footer. A payload already present is removed first,
// so the tool can run again on the same file.
//   empreinte: receives the hash of the payload (16 hex digits)
//   journal:   receives one line per file and a summary (or the error)
bool Empaqueter (const std::wstring& cible, const std::wstring& dossier, std::wstring& empreinte,
				 std::vector<std::wstring>& journal);

// Returns true if 'fichier' carries a valid payload; 'empreinte' then receives its hash and
// 'chemins' the relative paths of the embedded files (nothing is extracted).
bool LireCharge (const std::wstring& fichier, std::wstring& empreinte, std::vector<std::wstring>& chemins);

// Extracts the payload of 'fichier' into <base>\<hash>\ unless that is already done.
//   destination: receives the extracted folder (also when it already existed)
//   journal:     receives what happened (already extracted, extracted, or the error)
// Returns true when the folder is complete and usable.
bool ExtraireCharge (const std::wstring& fichier, const std::wstring& base, std::wstring& destination,
					 std::vector<std::wstring>& journal);

}	// namespace TT

#endif
