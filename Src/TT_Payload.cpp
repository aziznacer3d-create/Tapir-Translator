// *****************************************************************************
// Tapir Translator - payload appended to the end of the .apx (implementation)
//
//   ROLE IN THE SYSTEM
//   Packs (build tool Packer), inspects (Session Report) and extracts
//   (add-on start-up) the files appended after the .apx. The exact byte format
//   is described in TT_Payload.hpp. Win32 only, no ArchiCAD dependency.
// *****************************************************************************

#include "TT_Payload.hpp"
#include "TT_Core.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace TT {

namespace {

const char kMagic[8] = { 'T', 'T', 'C', 'H', 'A', 'R', 'G', 'E' };	// footer signature
constexpr size_t kFooterSize = 32;		// footer size: 8-byte signature + 3 x u64
constexpr DWORD kBufferSize = 1u << 20;		// copy buffer size (1 MB)

// Minimal RAII wrapper of a Win32 file handle: closed automatically on every return path.
struct FileHandle {
	HANDLE h = INVALID_HANDLE_VALUE;
	~FileHandle () { if (h != INVALID_HANDLE_VALUE) CloseHandle (h); }
	bool IsOpen () const { return h != INVALID_HANDLE_VALUE; }
};

// Stores a 64-bit value as 8 little-endian bytes at p (independent of the host byte order).
void WriteU64 (unsigned char* p, unsigned long long v)
{
	for (int i = 0; i < 8; ++i)
		p[i] = static_cast<unsigned char> (v >> (8 * i));
}

// Reads 8 little-endian bytes at p as a 64-bit value (inverse of WriteU64).
unsigned long long ReadU64 (const unsigned char* p)
{
	unsigned long long v = 0;
	for (int i = 7; i >= 0; --i)
		v = (v << 8) | p[i];
	return v;
}

// Moves the file pointer to an absolute position (64-bit, files may exceed 4 GB in theory).
bool Seek (HANDLE h, unsigned long long position)
{
	LARGE_INTEGER p;
	p.QuadPart = static_cast<LONGLONG> (position);
	return SetFilePointerEx (h, p, nullptr, FILE_BEGIN) != 0;
}

// Size of an open file in bytes (0 if it cannot be read).
unsigned long long FileSize (HANDLE h)
{
	LARGE_INTEGER t = {};
	GetFileSizeEx (h, &t);
	return static_cast<unsigned long long> (t.QuadPart);
}

// Reads exactly n bytes; a short read (end of file) counts as a failure.
bool ReadExact (HANDLE h, void* data, DWORD n)
{
	DWORD read_ = 0;
	return ReadFile (h, data, n, &read_, nullptr) && read_ == n;
}

// Writes exactly n bytes; a partial write counts as a failure.
bool WriteExact (HANDLE h, const void* data, DWORD n)
{
	DWORD written = 0;
	return WriteFile (h, data, n, &written, nullptr) && written == n;
}

// Feeds n bytes into a running 64-bit FNV-1a hash (xor the byte, then multiply by the FNV
// prime). The caller starts from the FNV offset basis. Used as a version fingerprint, not
// for security.
void Fnv (unsigned long long& hash, const unsigned char* p, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		hash ^= p[i];
		hash *= 1099511628211ull;
	}
}

// Formats a 64-bit value as 16 lower-case hexadecimal digits (the folder name of an extraction).
std::wstring Hex (unsigned long long v)
{
	wchar_t t[17];
	swprintf (t, 17, L"%016llx", v);
	return t;
}

// Recursively lists the files under <root>\<relative> and appends their paths, relative to
// 'root' (backslash-separated), to 'res'. The files of a folder come before the content of
// its sub-folders. Call with relative = L"" to list the whole tree.
void ListFiles (const std::wstring& root, const std::wstring& relative, std::vector<std::wstring>& res)
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Join (Join (root, relative), L"*").c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return;
	std::vector<std::wstring> folders;
	do {
		const std::wstring name = d.cFileName;
		if (name == L"." || name == L"..")
			continue;
		const std::wstring r = relative.empty () ? name : relative + L"\\" + name;
		if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			folders.push_back (r);
		else
			res.push_back (r);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	for (const std::wstring& r : folders)
		ListFiles (root, r, res);
}

// One embedded file, as described by a line of the index.
struct IndexEntry {
	std::wstring path;			// path relative to the extraction folder
	unsigned long long start = 0;	// absolute offset of its first byte in the .apx
	unsigned long long size = 0;	// size in bytes
};

// Reads and validates the footer and the index of an open file.
//   payloadStart: receives the offset where the payload starts (0 if there is no payload)
//   hash:   receives the hash from the index
//   inputs:     receives one IndexEntry per embedded file
// Returns false if the file has no payload or if anything is inconsistent (the file is then
// treated as having no payload).
bool ReadIndex (HANDLE h, unsigned long long& payloadStart, std::wstring& hash, std::vector<IndexEntry>& inputs)
{
	// Step 1: the file must be large enough for a footer, and end with the "TTCHARGE" signature.
	payloadStart = 0;
	const unsigned long long total = FileSize (h);
	if (total < kFooterSize)
		return false;
	unsigned char footer[kFooterSize];
	if (!Seek (h, total - kFooterSize) || !ReadExact (h, footer, kFooterSize) || std::memcmp (footer, kMagic, 8) != 0)
		return false;
	// Step 2: decode the three u64 of the footer and check them: the index must end exactly
	// where the footer starts, be at most 64 MB, and come after the start of the payload.
	const unsigned long long indexStart = ReadU64 (footer + 8);
	const unsigned long long indexSize = ReadU64 (footer + 16);
	payloadStart = ReadU64 (footer + 24);
	if (indexStart + indexSize + kFooterSize != total || indexSize > 64u * 1024u * 1024u || payloadStart > indexStart)
		return false;
	// Step 3: read the whole UTF-8 index.
	std::string index (static_cast<size_t> (indexSize), '\0');
	if (!Seek (h, indexStart) || (indexSize > 0 && !ReadExact (h, &index[0], static_cast<DWORD> (indexSize))))
		return false;
	// Step 4: parse it line by line. Lines without a tab are skipped; the "hash" line gives
	// the hash; every other line must be "<path>\t<start>\t<size>".
	size_t pos = 0;
	while (pos < index.size ()) {
		size_t end_ = index.find ('\n', pos);
		if (end_ == std::string::npos)
			end_ = index.size ();
		const std::string line = index.substr (pos, end_ - pos);
		pos = end_ + 1;
		const size_t t1 = line.find ('\t');
		if (t1 == std::string::npos)
			continue;
		if (line.compare (0, t1, "empreinte") == 0) {
			hash = FromUtf8 (line.substr (t1 + 1));
			continue;
		}
		const size_t t2 = line.find ('\t', t1 + 1);
		if (t2 == std::string::npos)
			return false;
		IndexEntry e;
		e.path = FromUtf8 (line.substr (0, t1));
		e.start = std::strtoull (line.c_str () + t1 + 1, nullptr, 10);
		e.size = std::strtoull (line.c_str () + t2 + 1, nullptr, 10);
		// Each file must lie inside the payload area (between its start and the index), and its
		// path must not contain ".." so that extraction can never write outside its folder.
		if (e.start < payloadStart || e.start + e.size > indexStart || e.path.find (L"..") != std::wstring::npos)
			return false;
		inputs.push_back (e);
	}
	// Step 5: an index without a hash line is not valid.
	return !hash.empty ();
}

}	// namespace


// Packs the files of 'folder' at the end of 'target' (see TT_Payload.hpp for the format).
bool Packer (const std::wstring& target, const std::wstring& folder, std::wstring& hash, std::vector<std::wstring>& journal)
{
	// Step 1: open the target .apx for reading and writing (exclusive access).
	FileHandle f;
	f.h = CreateFileW (target.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!f.IsOpen ()) {
		journal.push_back (L"ouverture impossible : " + target);
		return false;
	}
	// Step 2: if a previous payload is present, truncate the file at its start.
	unsigned long long start = 0;
	{
		std::wstring e;
		std::vector<IndexEntry> oldOnes;
		if (ReadIndex (f.h, start, e, oldOnes) && start > 0) {
			Seek (f.h, start);
			SetEndOfFile (f.h);
			journal.push_back (L"ancienne charge retiree (" + e + L")");
		}
	}
	// The new payload starts at the current end of the file.
	start = FileSize (f.h);

	// Step 3: list the files to embed; an empty folder is an error.
	std::vector<std::wstring> files;
	ListFiles (folder, L"", files);
	if (files.empty ()) {
		journal.push_back (L"dossier vide : " + folder);
		return false;
	}
	// Step 4: append each file in turn, hashing its relative path and its content (FNV-1a,
	// starting from the 64-bit offset basis) and adding its index line.
	std::unique_ptr<unsigned char[]> buffer (new unsigned char[kBufferSize]);
	unsigned long long hashNum = 1469598103934665603ull;
	std::string index;
	Seek (f.h, start);
	unsigned long long position = start;
	for (const std::wstring& rel : files) {
		FileHandle source;
		source.h = CreateFileW (Join (folder, rel).c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
								FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (!source.IsOpen ()) {
			journal.push_back (L"lecture impossible : " + rel);
			return false;
		}
		const std::string relUtf8 = Utf8 (rel);
		Fnv (hashNum, reinterpret_cast<const unsigned char*> (relUtf8.data ()), relUtf8.size ());
		unsigned long long size = 0;
		for (;;) {
			DWORD read_ = 0;
			if (!ReadFile (source.h, buffer.get (), kBufferSize, &read_, nullptr))
				return false;
			if (read_ == 0)
				break;
			Fnv (hashNum, buffer.get (), read_);
			if (!WriteExact (f.h, buffer.get (), read_)) {
				journal.push_back (L"ecriture impossible dans " + target);
				return false;
			}
			size += read_;
		}
		index += relUtf8 + "\t" + std::to_string (position) + "\t" + std::to_string (size) + "\n";
		position += size;
		journal.push_back (rel + L" : " + std::to_wstring (size) + L" octets");
	}
	// Step 5: the index, with the hash line first, follows the last file.
	hash = Hex (hashNum);
	index = "empreinte\t" + Utf8 (hash) + "\n" + index;
	if (!WriteExact (f.h, index.data (), static_cast<DWORD> (index.size ())))
		return false;
	// Step 6: the 32-byte footer: signature, index start (= end of the last file), index size,
	// payload start.
	unsigned char footer[kFooterSize];
	std::memcpy (footer, kMagic, 8);
	WriteU64 (footer + 8, position);
	WriteU64 (footer + 16, index.size ());
	WriteU64 (footer + 24, start);
	if (!WriteExact (f.h, footer, kFooterSize))
		return false;
	journal.push_back (std::to_wstring (files.size ()) + L" fichier(s), " + std::to_wstring (position - start) + L" octets, empreinte " + hash);
	return true;
}


// Reads only the index of a payload (used by the status report). The file is opened with full
// sharing because ArchiCAD keeps the loaded .apx open.
bool ReadPayload (const std::wstring& file, std::wstring& hash, std::vector<std::wstring>& paths)
{
	FileHandle f;
	f.h = CreateFileW (file.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					   FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!f.IsOpen ())
		return false;
	unsigned long long start = 0;
	std::vector<IndexEntry> inputs;
	if (!ReadIndex (f.h, start, hash, inputs))
		return false;
	for (const IndexEntry& e : inputs)
		paths.push_back (e.path);
	return true;
}


// Extracts the payload into <base>\<hash> (see TT_Payload.hpp). The extraction is made in a
// temporary folder and renamed at the end, so a folder named after the hash is always complete.
bool ExtractPayload (const std::wstring& file, const std::wstring& base, std::wstring& destination, std::vector<std::wstring>& journal)
{
	// Step 1: open the .apx. It is already open in ArchiCAD, hence the full sharing mode.
	FileHandle f;
	f.h = CreateFileW (file.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (!f.IsOpen ()) {
		journal.push_back (L"ouverture impossible : " + file);
		return false;
	}
	// Step 2: read the index; no payload means nothing to extract.
	unsigned long long start = 0;
	std::wstring hash;
	std::vector<IndexEntry> inputs;
	if (!ReadIndex (f.h, start, hash, inputs)) {
		journal.push_back (L"aucune charge embarquee dans " + file);
		return false;
	}
	// Step 3: already extracted (the ".complete" marker exists): nothing more to do.
	destination = Join (base, hash);
	if (FileExists (Join (destination, L".complet"))) {
		journal.push_back (L"moteur deja extrait : " + destination);
		return true;
	}

	// Step 4: copy every file into "<destination>.tmp<process ID>" (unique per ArchiCAD
	// instance, so two instances never write into the same folder), creating sub-folders.
	const std::wstring tmp = destination + L".tmp" + std::to_wstring (GetCurrentProcessId ());
	CreateFolder (tmp);
	std::unique_ptr<unsigned char[]> buffer (new unsigned char[kBufferSize]);
	for (const IndexEntry& e : inputs) {
		const std::wstring path = Join (tmp, e.path);
		const size_t sep = path.find_last_of (L"\\/");
		if (sep != std::wstring::npos)
			CreateFolder (path.substr (0, sep));
		FileHandle output;
		output.h = CreateFileW (path.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (!output.IsOpen () || !Seek (f.h, e.start)) {
			journal.push_back (L"extraction impossible : " + path);
			return false;
		}
		unsigned long long rest = e.size;
		while (rest > 0) {
			const DWORD n = static_cast<DWORD> (rest > kBufferSize ? kBufferSize : rest);
			if (!ReadExact (f.h, buffer.get (), n) || !WriteExact (output.h, buffer.get (), n)) {
				journal.push_back (L"extraction interrompue : " + path);
				return false;
			}
			rest -= n;
		}
	}
	// Step 5: write the ".complete" marker (containing the hash), then rename the temporary
	// folder to its final name.
	std::wstring error;
	WriteWholeFile (Join (tmp, L".complet"), Utf8 (hash) + "\n", error);
	if (!MoveFileExW (tmp.c_str (), destination.c_str (), 0)) {
		// The rename fails if the destination exists: another ArchiCAD instance may have
		// extracted the same version at the same time, which is fine if it is complete.
		if (FileExists (Join (destination, L".complet"))) {
			journal.push_back (L"moteur extrait par une autre instance : " + destination);
			return true;
		}
		journal.push_back (L"renommage impossible : " + tmp);
		return false;
	}
	journal.push_back (L"moteur extrait (" + std::to_wstring (inputs.size ()) + L" fichier(s)) : " + destination);
	return true;
}

}	// namespace TT
