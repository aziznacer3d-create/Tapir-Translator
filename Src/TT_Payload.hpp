// *****************************************************************************
// Tapir Translator - payload appended to the end of the .apx (engine + model)
//
//   ROLE IN THE SYSTEM
//   The user copies a single file: Tapir_Translator.apx. At build time,
//   Tools/Packer.cpp appends, AFTER the normal content of the .apx (an area
//   that Windows ignores when it loads a DLL), the files of a folder (the engine,
//   the optional model, the licence texts), then an index and a 32-byte footer.
//
//   PAYLOAD FORMAT
//
//     [ .apx ][ file 1 ][ file 2 ]...[ UTF-8 index ][ footer ]
//
//     index : "hash\t<16 hex digits>\n" (the hash line), then one line per
//             file: "<relative path>\t<start offset>\t<size>\n"
//             (offsets are absolute positions in the .apx, sizes in bytes)
//     footer: "TTPAYLOD" (8 bytes) | index start (u64) | index size (u64) |
//             payload start (u64)        - all u64 are little-endian
//
//     The hash is a 64-bit FNV-1a over every relative path and file content.
//
//   EXTRACTION
//   At start-up the add-on extracts the payload only once, into
//   <base>\<hash>\ (completion marker file ".complete"): another version of the
//   .apx has another hash and is extracted next to it.
//
//   No ArchiCAD dependency (Win32 only): shared by the add-on, the Packer
//   tool and the test bench.
// *****************************************************************************

#ifndef TT_PAYLOAD_HPP
#define TT_PAYLOAD_HPP

#include <string>
#include <vector>

namespace TT {

// Build-time packing: appends every file of 'folder' (sub-folders included) to the end of
// 'target', followed by the index and the footer. A payload already present is removed first,
// so the tool can run again on the same file.
//   hash: receives the hash of the payload (16 hex digits)
//   journal:   receives one line per file and a summary (or the error)
bool Packer (const std::wstring& target, const std::wstring& folder, std::wstring& hash,
				 std::vector<std::wstring>& journal);

// Returns true if 'file' carries a valid payload; 'hash' then receives its hash and
// 'paths' the relative paths of the embedded files (nothing is extracted).
bool ReadPayload (const std::wstring& file, std::wstring& hash, std::vector<std::wstring>& paths);

// Extracts the payload of 'file' into <base>\<hash>\ unless that is already done.
//   destination: receives the extracted folder (also when it already existed)
//   journal:     receives what happened (already extracted, extracted, or the error)
// Returns true when the folder is complete and usable.
bool ExtractPayload (const std::wstring& file, const std::wstring& base, std::wstring& destination,
					 std::vector<std::wstring>& journal);

}	// namespace TT

#endif
