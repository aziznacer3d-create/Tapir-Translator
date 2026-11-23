// *****************************************************************************
// Tapir Translator - charge embarquee a la fin du .apx (voir TT_Charge.hpp)
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

const char kMagie[8] = { 'T', 'T', 'C', 'H', 'A', 'R', 'G', 'E' };
constexpr size_t kTaillePied = 32;
constexpr DWORD kTampon = 1u << 20;

struct Fichier {
	HANDLE h = INVALID_HANDLE_VALUE;
	~Fichier () { if (h != INVALID_HANDLE_VALUE) CloseHandle (h); }
	bool Ouvert () const { return h != INVALID_HANDLE_VALUE; }
};

void EcrireU64 (unsigned char* p, unsigned long long v)
{
	for (int i = 0; i < 8; ++i)
		p[i] = static_cast<unsigned char> (v >> (8 * i));
}

unsigned long long LireU64 (const unsigned char* p)
{
	unsigned long long v = 0;
	for (int i = 7; i >= 0; --i)
		v = (v << 8) | p[i];
	return v;
}

bool Aller (HANDLE h, unsigned long long position)
{
	LARGE_INTEGER p;
	p.QuadPart = static_cast<LONGLONG> (position);
	return SetFilePointerEx (h, p, nullptr, FILE_BEGIN) != 0;
}

unsigned long long Taille (HANDLE h)
{
	LARGE_INTEGER t = {};
	GetFileSizeEx (h, &t);
	return static_cast<unsigned long long> (t.QuadPart);
}

bool LireExact (HANDLE h, void* donnees, DWORD n)
{
	DWORD lu = 0;
	return ReadFile (h, donnees, n, &lu, nullptr) && lu == n;
}

bool EcrireExact (HANDLE h, const void* donnees, DWORD n)
{
	DWORD ecrit = 0;
	return WriteFile (h, donnees, n, &ecrit, nullptr) && ecrit == n;
}

void Fnv (unsigned long long& empreinte, const unsigned char* p, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		empreinte ^= p[i];
		empreinte *= 1099511628211ull;
	}
}

std::wstring Hex (unsigned long long v)
{
	wchar_t t[17];
	swprintf (t, 17, L"%016llx", v);
	return t;
}

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

struct Entree {
	std::wstring chemin;
	unsigned long long debut = 0;
	unsigned long long taille = 0;
};

// Lit le pied et l'index. debutCharge = 0 si aucune charge.
bool LireIndex (HANDLE h, unsigned long long& debutCharge, std::wstring& empreinte, std::vector<Entree>& entrees)
{
	debutCharge = 0;
	const unsigned long long total = Taille (h);
	if (total < kTaillePied)
		return false;
	unsigned char pied[kTaillePied];
	if (!Aller (h, total - kTaillePied) || !LireExact (h, pied, kTaillePied) || std::memcmp (pied, kMagie, 8) != 0)
		return false;
	const unsigned long long debutIndex = LireU64 (pied + 8);
	const unsigned long long tailleIndex = LireU64 (pied + 16);
	debutCharge = LireU64 (pied + 24);
	if (debutIndex + tailleIndex + kTaillePied != total || tailleIndex > 64u * 1024u * 1024u || debutCharge > debutIndex)
		return false;
	std::string index (static_cast<size_t> (tailleIndex), '\0');
	if (!Aller (h, debutIndex) || (tailleIndex > 0 && !LireExact (h, &index[0], static_cast<DWORD> (tailleIndex))))
		return false;
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
		if (e.debut < debutCharge || e.debut + e.taille > debutIndex || e.chemin.find (L"..") != std::wstring::npos)
			return false;
		entrees.push_back (e);
	}
	return !empreinte.empty ();
}

}	// namespace


bool Empaqueter (const std::wstring& cible, const std::wstring& dossier, std::wstring& empreinte, std::vector<std::wstring>& journal)
{
	Fichier f;
	f.h = CreateFileW (cible.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!f.Ouvert ()) {
		journal.push_back (L"ouverture impossible : " + cible);
		return false;
	}
	// charge precedente retiree
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
	debut = Taille (f.h);

	std::vector<std::wstring> fichiers;
	Lister (dossier, L"", fichiers);
	if (fichiers.empty ()) {
		journal.push_back (L"dossier vide : " + dossier);
		return false;
	}
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
	empreinte = Hex (empreinteNum);
	index = "empreinte\t" + Utf8 (empreinte) + "\n" + index;
	if (!EcrireExact (f.h, index.data (), static_cast<DWORD> (index.size ())))
		return false;
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


bool ExtraireCharge (const std::wstring& fichier, const std::wstring& base, std::wstring& destination, std::vector<std::wstring>& journal)
{
	Fichier f;
	// le .apx est ouvert par ArchiCAD : partage en lecture
	f.h = CreateFileW (fichier.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (!f.Ouvert ()) {
		journal.push_back (L"ouverture impossible : " + fichier);
		return false;
	}
	unsigned long long debut = 0;
	std::wstring empreinte;
	std::vector<Entree> entrees;
	if (!LireIndex (f.h, debut, empreinte, entrees)) {
		journal.push_back (L"aucune charge embarquee dans " + fichier);
		return false;
	}
	destination = Joindre (base, empreinte);
	if (FichierExiste (Joindre (destination, L".complet"))) {
		journal.push_back (L"moteur deja extrait : " + destination);
		return true;
	}

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
	std::wstring erreur;
	EcrireFichier (Joindre (tmp, L".complet"), Utf8 (empreinte) + "\n", erreur);
	if (!MoveFileExW (tmp.c_str (), destination.c_str (), 0)) {
		// une autre instance d'ArchiCAD a pu extraire en meme temps
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
