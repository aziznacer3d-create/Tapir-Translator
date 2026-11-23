// *****************************************************************************
// Tapir Translator - GenererMenus : menus de l'add-on dans les 20 langues
//
//   Usage : GenererMenus.exe <RFIX/Tapir_TranslatorMenus.grc>
//   Ecrit, pour chaque langue d'interface i, les 5 menus (STR# BaseMenus (i) + 0..4)
//   et leurs aides (+ DecalageAides) a partir de Src/TT_Textes.cpp. Lance au build
//   avant la compilation des ressources ; le fichier n'est reecrit que s'il change.
//   L'add-on n'enregistre que les menus de la langue choisie (au demarrage).
// *****************************************************************************

#include "TT_Textes.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string Chaine (const std::string& s)
{
	std::string r = "\"";
	for (const char c : s) {
		if (c == '"')
			r += "'";		// pas de guillemet droit dans une chaine GRC
		else if (c == '\\')
			r += "/";
		else
			r += c;
	}
	return r + "\"";
}

void Menu (std::string& grc, short id, const char* titre, const std::string& sousMenu, const std::vector<std::string>& elements,
		   const std::vector<std::string>& aides)
{
	grc += "'STR#' " + std::to_string (id) + " \"" + titre + "\" {\n";
	grc += "/* [   ] */\t\t\"Tapir Translator\"\n";
	grc += "/* [   ] */\t\t" + Chaine (sousMenu) + "\n";
	for (size_t i = 0; i < elements.size (); ++i)
		grc += "/* [" + std::to_string (i + 1) + "] */\t\t\t" + Chaine (elements[i]) + "\n";
	grc += "}\n\n";
	grc += "'STR#' " + std::to_string (id + TT::DecalageAides) + " \"" + titre + " (aides)\" {\n";
	grc += "/* [   ] */\t\t\"\"\n/* [   ] */\t\t\"\"\n";
	for (size_t i = 0; i < aides.size (); ++i)
		grc += "/* [" + std::to_string (i + 1) + "] */\t\t" + Chaine (aides[i]) + "\n";
	grc += "}\n\n";
}

}	// namespace


int main (int argc, char** argv)
{
	if (argc != 2) {
		std::fprintf (stderr, "usage : GenererMenus <fichier.grc>\n");
		return 2;
	}
	std::string grc = "\xEF\xBB\xBF/* Genere par Tools/GenererMenus.cpp a partir de Src/TT_Textes.cpp : ne pas modifier a la main. */\n\n";
	for (int l = 0; l < TT::NombreLanguesInterface; ++l) {
		const auto T = [l] (TT::Texte t) { return std::string (TT::Txt (t, l)); };
		const short base = TT::BaseMenus (l);
		grc += "/* ===== " + std::string (TT::CodeInterface (l)) + " ===== */\n\n";

		std::vector<std::string> langues, aidesEntree, aidesSortie, aidesInterface;
		for (int i = 0; i < TT::NombreLanguesInterface; ++i)
			langues.push_back (TT::NomInterface (i));

		std::vector<std::string> e = { T (TT::M_AUTO) };
		e.insert (e.end (), langues.begin (), langues.end ());
		std::vector<std::string> a (1, T (TT::P_AUTO));
		a.insert (a.end (), langues.size (), T (TT::P_ENTREE));
		Menu (grc, base + 0, "Langue d'entree", T (TT::M_ENTREE), e, a);

		e = { T (TT::M_AUCUNE) };
		e.insert (e.end (), langues.begin (), langues.end ());
		a = { T (TT::P_AUCUNE) };
		a.insert (a.end (), langues.size (), T (TT::P_SORTIE));
		Menu (grc, base + 1, "Langue de sortie", T (TT::M_SORTIE), e, a);

		Menu (grc, base + 2, "Options", T (TT::M_OPTIONS),
			  { T (TT::M_EN_LIGNE), T (TT::M_COLLECTE), "-", T (TT::M_DOSSIER), T (TT::M_ETAT) },
			  { T (TT::P_EN_LIGNE), T (TT::P_COLLECTE), "", T (TT::P_DOSSIER), T (TT::P_ETAT) });

		Menu (grc, base + 3, "Traducteur en ligne", T (TT::M_TRADUCTEUR),
			  { "DeepL", "Microsoft Translator", "Google Cloud Translation", "MyMemory (" + T (TT::M_SANS_CLE) + ")", "LibreTranslate",
				"-", T (TT::M_CLES), T (TT::M_CORRECTIONS) },
			  { T (TT::P_TRADUCTEUR), T (TT::P_TRADUCTEUR), T (TT::P_TRADUCTEUR), T (TT::P_TRADUCTEUR), T (TT::P_TRADUCTEUR), "",
				T (TT::P_CLES), T (TT::P_CORRECTIONS) });

		e = { T (TT::M_INTERFACE_AUTO) };
		e.insert (e.end (), langues.begin (), langues.end ());
		Menu (grc, base + 4, "Langue de l'outil", T (TT::M_INTERFACE), e, std::vector<std::string> (e.size (), T (TT::P_INTERFACE)));
	}

	// n'ecrit que si le contenu change (pas de recompilation des ressources inutile)
	std::string ancien;
	FILE* f = nullptr;
	if (fopen_s (&f, argv[1], "rb") == 0 && f != nullptr) {
		char tampon[65536];
		size_t n = 0;
		while ((n = std::fread (tampon, 1, sizeof (tampon), f)) > 0)
			ancien.append (tampon, n);
		std::fclose (f);
	}
	if (ancien == grc)
		return 0;
	if (fopen_s (&f, argv[1], "wb") != 0 || f == nullptr) {
		std::fprintf (stderr, "GenererMenus : ecriture impossible : %s\n", argv[1]);
		return 1;
	}
	std::fwrite (grc.data (), 1, grc.size (), f);
	std::fclose (f);
	std::printf ("GenererMenus : %s ecrit (%d langues)\n", argv[1], TT::NombreLanguesInterface);
	return 0;
}
