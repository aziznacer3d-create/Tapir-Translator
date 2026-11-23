// *****************************************************************************
// Tapir Translator - payload appended to the end of the .apx (implementation)
//
//   ROLE IN THE SYSTEM
//   Packs (build tool Empaqueter), inspects (Session Report) and extracts
//   (add-on start-up) the files appended after the .apx. The exact byte format
//   is described in TT_Charge.hpp. Win32 only, no ArchiCAD dependency.
// *****************************************************************************

#include "TT_Charge.hpp"
#include "TT_Coeur.hpp"

#if !defined (WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace TT {

namespace {

const char kMagie[8] = { 'T', 'T', 'C', 'H', 'A', 'R', 'G', 'E' };	// footer signature
constexpr size_t kTaillePied = 32;		// footer size: 8-byte signature + 3 x u64
constexpr DWORD kTampon = 1u << 20;		// copy buffer size (1 MB)

// Minimal RAII wrapper of a Win32 file handle: closed automatically on every return path.
struct Fichier {
	HANDLE h = INVALID_HANDLE_VALUE;
	~Fichier () { if (h != INVALID_HANDLE_VALUE) CloseHandle (h); }
	bool Ouvert () const { return h != INVALID_HANDLE_VALUE; }
};

// Stores a 64-bit value as 8 little-endian bytes at p (independent of the host byte order).
void EcrireU64 (unsigned char* p, unsigned long long v)
{
	for (int i = 0; i < 8; ++i)
		p[i] = static_cast<unsigned char> (v >> (8 * i));
}

// Reads 8 little-endian bytes at p as a 64-bit value (inverse of EcrireU64).
unsigned long long LireU64 (const unsigned char* p)
{
	unsigned long long v = 0;
	for (int i = 7; i >= 0; --i)
		v = (v << 8) | p[i];
	return v;
}

// Moves the file pointer to an absolute position (64-bit, files may exceed 4 GB in theory).
bool Aller (HANDLE h, unsigned long long position)
{
	LARGE_INTEGER p;
	p.QuadPart = static_cast<LONGLONG> (position);
	return SetFilePointerEx (h, p, nullptr, FILE_BEGIN) != 0;
}

// Size of an open file in bytes (0 if it cannot be read).
unsigned long long Taille (HANDLE h)
{
	LARGE_INTEGER t = {};
	GetFileSizeEx (h, &t);
	return static_cast<unsigned long long> (t.QuadPart);
}

// Reads exactly n bytes; a short read (end of file) counts as a failure.
bool LireExact (HANDLE h, void* donnees, DWORD n)
{
	DWORD lu = 0;
	return ReadFile (h, donnees, n, &lu, nullptr) && lu == n;
}

// Writes exactly n bytes; a partial write counts as a failure.
bool EcrireExact (HANDLE h, const void* donnees, DWORD n)
{
	DWORD ecrit = 0;
	return WriteFile (h, donnees, n, &ecrit, nullptr) && ecrit == n;
}

// Feeds n bytes into a running 64-bit FNV-1a hash (xor the byte, then multiply by the FNV
// prime). The caller starts from the FNV offset basis. Used as a version fingerprint, not
// for security.
void Fnv (unsigned long long& empreinte, const unsigned char* p, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		empreinte ^= p[i];
		empreinte *= 1099511628211ull;
	}
}

// Formats a 64-bit value as 16 lower-case hexadecimal digits (the folder name of an extraction).
std::wstring Hex (unsigned long long v)
{
	wchar_t t[17];
	swprintf (t, 17, L"%016llx", v);
	return t;
}

// Recursively lists the files under <racine>\<relatif> and appends their paths, relative to
// 'racine' (backslash-separated), to 'res'. The files of a folder come before the content of
// its sub-folders. Call with relatif = L"" to list the whole tree.
void Lister (const std::wstring& racine, const std::wstring& relatif, std::vector<std::wstring>& res)
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW (Joindre (Joindre (racine, relatif), L"*").c_str (), &d);
	if (h == INVALID_HANDLE_VALUE)
		return;
	std::vector<std::wstring> dossiers;
	do {
		const std::wstring nom = d.cFileName;
		if (nom == L"." || nom == L"..")
			continue;
		const std::wstring r = relatif.empty () ? nom : relatif + L"\\" + nom;
		if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			dossiers.push_back (r);
		else
			res.push_back (r);
	} while (FindNextFileW (h, &d));
	FindClose (h);
	for (const std::wstring& r : dossiers)
		Lister (racine, r, res);
}

// One embedded file, as described by a line of the index.
struct Entree {
	std::wstring chemin;			// path relative to the extraction folder
	unsigned long long debut = 0;	// absolute offset of its first byte in the .apx
	unsigned long long taille = 0;	// size in bytes
};

// Reads and validates the footer and the index of an open file.
//   debutCharge: receives the offset where the payload starts (0 if there is no payload)
//   empreinte:   receives the hash from the index
//   entrees:     receives one Entree per embedded file
// Returns false if the file has no payload or if anything is inconsistent (the file is then
// treated as having no payload).
bool LireIndex (HANDLE h, unsigned long long& debutCharge, std::wstring& empreinte, std::vector<Entree>& entrees)
{
	// Step 1: the file must be large enough for a footer, and end with the "TTCHARGE" signature.
	debutCharge = 0;
	const unsigned long long total = Taille (h);
	if (total < kTaillePied)
		return false;
	unsigned char pied[kTaillePied];
	if (!Aller (h, total - kTaillePied) || !LireExact (h, pied, kTaillePied) || std::memcmp (pied, kMagie, 8) != 0)
		return false;
	// Step 2: decode the three u64 of the footer and check them: the index must end exactly
	// where the footer starts, be at most 64 MB, and come after the start of the payload.
	const unsigned long long debutIndex = LireU64 (pied + 8);
	const unsigned long long tailleIndex = LireU64 (pied + 16);
	debutCharge = LireU64 (pied + 24);
	if (debutIndex + tailleIndex + kTaillePied != total || tailleIndex > 64u * 1024u * 1024u || debutCharge > debutIndex)
		return false;
	// Step 3: read the whole UTF-8 index.
	std::string index (static_cast<size_t> (tailleIndex), '\0');
	if (!Aller (h, debutIndex) || (tailleIndex > 0 && !LireExact (h, &index[0], static_cast<DWORD> (tailleIndex))))
		return false;
	// Step 4: parse it line by line. Lines without a tab are skipped; the "empreinte" line gives
	// the hash; every other line must be "<path>\t<start>\t<size>".
	size_t pos = 0;
	while (pos < index.size ()) {
		size_t fin = index.find ('\n', pos);
		if (fin == std::string::npos)
			fin = index.size ();
		const std::string ligne = index.substr (pos, fin - pos);
		pos = fin + 1;
		const size_t t1 = ligne.find ('\t');
		if (t1 == std::string::npos)
			continue;
		if (ligne.compare (0, t1, "empreinte") == 0) {
			empreinte = DepuisUtf8 (ligne.substr (t1 + 1));
			continue;
		}
		const size_t t2 = ligne.find ('\t', t1 + 1);
		if (t2 == std::string::npos)
			return false;
		Entree e;
		e.chemin = DepuisUtf8 (ligne.substr (0, t1));
		e.debut = std::strtoull (ligne.c_str () + t1 + 1, nullptr, 10);
		e.taille = std::strtoull (ligne.c_str () + t2 + 1, nullptr, 10);
		// Each file must lie inside the payload area (between its start and the index), and its
		// path must not contain ".." so that extraction can never write outside its folder.
		if (e.debut < debutCharge || e.debut + e.taille > debutIndex || e.chemin.find (L"..") != std::wstring::npos)
			return false;
		entrees.push_back (e);
	}
	// Step 5: an index without a hash line is not valid.
	return !empreinte.empty ();
}

}	// namespace


// Packs the files of 'dossier' at the end of 'cible' (see TT_Charge.hpp for the format).
bool Empaqueter (const std::wstring& cible, const std::wstring& dossier, std::wstring& empreinte, std::vector<std::wstring>& journal)
{
	// Step 1: open the target .apx for reading and writing (exclusive access).
	Fichier f;
	f.h = CreateFileW (cible.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!f.Ouvert ()) {
		journal.push_back (L"ouverture impossible : " + cible);
		return false;
	}
	// Step 2: if a previous payload is present, truncate the file at its start.
	unsigned long long debut = 0;
	{
		std::wstring e;
		std::vector<Entree> anciennes;
		if (LireIndex (f.h, debut, e, anciennes) && debut > 0) {
			Aller (f.h, debut);
			SetEndOfFile (f.h);
			journal.push_back (L"ancienne charge retiree (" + e + L")");
		}
	}
	// The new payload starts at the current end of the file.
	debut = Taille (f.h);

	// Step 3: list the files to embed; an empty folder is an error.
	std::vector<std::wstring> fichiers;
	Lister (dossier, L"", fichiers);
	if (fichiers.empty ()) {
		journal.push_back (L"dossier vide : " + dossier);
		return false;
	}
	// Step 4: append each file in turn, hashing its relative path and its content (FNV-1a,
	// starting from the 64-bit offset basis) and adding its index line.
	std::unique_ptr<unsigned char[]> tampon (new unsigned char[kTampon]);
	unsigned long long empreinteNum = 1469598103934665603ull;
	std::string index;
	Aller (f.h, debut);
	unsigned long long position = debut;
	for (const std::wstring& rel : fichiers) {
		Fichier source;
		source.h = CreateFileW (Joindre (dossier, rel).c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
								FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (!source.Ouvert ()) {
			journal.push_back (L"lecture impossible : " + rel);
			return false;
		}
		const std::string relUtf8 = Utf8 (rel);
		Fnv (empreinteNum, reinterpret_cast<const unsigned char*> (relUtf8.data ()), relUtf8.size ());
		unsigned long long taille = 0;
		for (;;) {
			DWORD lu = 0;
			if (!ReadFile (source.h, tampon.get (), kTampon, &lu, nullptr))
				return false;
			if (lu == 0)
				break;
			Fnv (empreinteNum, tampon.get (), lu);
			if (!EcrireExact (f.h, tampon.get (), lu)) {
				journal.push_back (L"ecriture impossible dans " + cible);
				return false;
			}
			taille += lu;
		}
		index += relUtf8 + "\t" + std::to_string (position) + "\t" + std::to_string (taille) + "\n";
		position += taille;
		journal.push_back (rel + L" : " + std::to_wstring (taille) + L" octets");
	}
	// Step 5: the index, with the hash line first, follows the last file.
	empreinte = Hex (empreinteNum);
	index = "empreinte\t" + Utf8 (empreinte) + "\n" + index;
	if (!EcrireExact (f.h, index.data (), static_cast<DWORD> (index.size ())))
		return false;
	// Step 6: the 32-byte footer: signature, index start (= end of the last file), index size,
	// payload start.
	unsigned char pied[kTaillePied];
	std::memcpy (pied, kMagie, 8);
	EcrireU64 (pied + 8, position);
	EcrireU64 (pied + 16, index.size ());
	EcrireU64 (pied + 24, debut);
	if (!EcrireExact (f.h, pied, kTaillePied))
		return false;
	journal.push_back (std::to_wstring (fichiers.size ()) + L" fichier(s), " + std::to_wstring (position - debut) + L" octets, empreinte " + empreinte);
	return true;
}


// Reads only the index of a payload (used by the status report). The file is opened with full
// sharing because ArchiCAD keeps the loaded .apx open.
bool LireCharge (const std::wstring& fichier, std::wstring& empreinte, std::vector<std::wstring>& chemins)
{
	Fichier f;
	f.h = CreateFileW (fichier.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					   FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!f.Ouvert ())
		return false;
	unsigned long long debut = 0;
	std::vector<Entree> entrees;
	if (!LireIndex (f.h, debut, empreinte, entrees))
		return false;
	for (const Entree& e : entrees)
		chemins.push_back (e.chemin);
	return true;
}


// Extracts the payload into <base>\<hash> (see TT_Charge.hpp). The extraction is made in a
// temporary folder and renamed at the end, so a folder named after the hash is always complete.
bool ExtraireCharge (const std::wstring& fichier, const std::wstring& base, std::wstring& destination, std::vector<std::wstring>& journal)
{
	// Step 1: open the .apx. It is already open in ArchiCAD, hence the full sharing mode.
	Fichier f;
	f.h = CreateFileW (fichier.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (!f.Ouvert ()) {
		journal.push_back (L"ouverture impossible : " + fichier);
		return false;
	}
	// Step 2: read the index; no payload means nothing to extract.
	unsigned long long debut = 0;
	std::wstring empreinte;
	std::vector<Entree> entrees;
	if (!LireIndex (f.h, debut, empreinte, entrees)) {
		journal.push_back (L"aucune charge embarquee dans " + fichier);
		return false;
	}
	// Step 3: already extracted (the ".complet" marker exists): nothing more to do.
	destination = Joindre (base, empreinte);
	if (FichierExiste (Joindre (destination, L".complet"))) {
		journal.push_back (L"moteur deja extrait : " + destination);
		return true;
	}

	// Step 4: copy every file into "<destination>.tmp<process ID>" (unique per ArchiCAD
	// instance, so two instances never write into the same folder), creating sub-folders.
	const std::wstring tmp = destination + L".tmp" + std::to_wstring (GetCurrentProcessId ());
	CreerDossier (tmp);
	std::unique_ptr<unsigned char[]> tampon (new unsigned char[kTampon]);
	for (const Entree& e : entrees) {
		const std::wstring chemin = Joindre (tmp, e.chemin);
		const size_t sep = chemin.find_last_of (L"\\/");
		if (sep != std::wstring::npos)
			CreerDossier (chemin.substr (0, sep));
		Fichier sortie;
		sortie.h = CreateFileW (chemin.c_str (), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (!sortie.Ouvert () || !Aller (f.h, e.debut)) {
			journal.push_back (L"extraction impossible : " + chemin);
			return false;
		}
		unsigned long long reste = e.taille;
		while (reste > 0) {
			const DWORD n = static_cast<DWORD> (reste > kTampon ? kTampon : reste);
			if (!LireExact (f.h, tampon.get (), n) || !EcrireExact (sortie.h, tampon.get (), n)) {
				journal.push_back (L"extraction interrompue : " + chemin);
				return false;
			}
			reste -= n;
		}
	}
	// Step 5: write the ".complet" marker (containing the hash), then rename the temporary
	// folder to its final name.
	std::wstring erreur;
	EcrireFichier (Joindre (tmp, L".complet"), Utf8 (empreinte) + "\n", erreur);
	if (!MoveFileExW (tmp.c_str (), destination.c_str (), 0)) {
		// The rename fails if the destination exists: another ArchiCAD instance may have
		// extracted the same version at the same time, which is fine if it is complete.
		if (FichierExiste (Joindre (destination, L".complet"))) {
			journal.push_back (L"moteur extrait par une autre instance : " + destination);
			return true;
		}
		journal.push_back (L"renommage impossible : " + tmp);
		return false;
	}
	journal.push_back (L"moteur extrait (" + std::to_wstring (entrees.size ()) + L" fichier(s)) : " + destination);
	return true;
}

}	// namespace TT
