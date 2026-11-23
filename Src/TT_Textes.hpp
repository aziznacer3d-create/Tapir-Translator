// *****************************************************************************
// Tapir Translator - texts of the tool's own interface, in the 20 ArchiCAD languages
//
//   ROLE IN THE SYSTEM
//   Single source of every user-facing text of the tool itself: menus (turned
//   into resources at build time by Tools/GenererMenus.cpp), status-bar help
//   strings, the MyMemory quota window of the engine, and Session Report
//   messages. Shared by the add-on, the engine and the menu generator, so it has
//   no dependency.
//
//   Interface language: ArchiCAD's by default (version code FRA, GER, ITA...),
//   or the one chosen in the "Tapir Translator language" menu.
//   Interface index order = TT::Langues[1..20]: fr en de es pt it hu cs pl ru uk
//   el tr nl sv no fi ja ko zh (index 0 = fr ... 19 = zh).
//   Translations still to be reviewed by native speakers.
// *****************************************************************************

#ifndef TT_TEXTES_HPP
#define TT_TEXTES_HPP

#include <initializer_list>
#include <string>
#include <utility>

namespace TT {

// Identifier of each text; also the row index in the table of TT_Textes.cpp (same order).
enum Texte : int {
	// menu items (M_)
	M_ENTREE, M_AUTO, M_SORTIE, M_AUCUNE, M_TRADUCTEUR, M_SANS_CLE, M_CLES, M_CORRECTIONS,
	M_OPTIONS, M_EN_LIGNE, M_COLLECTE, M_DOSSIER, M_ETAT, M_INTERFACE, M_INTERFACE_AUTO,
	// status-bar help strings (P_)
	P_AUTO, P_ENTREE, P_AUCUNE, P_SORTIE, P_TRADUCTEUR, P_CLES, P_CORRECTIONS, P_EN_LIGNE,
	P_COLLECTE, P_DOSSIER, P_ETAT, P_INTERFACE,
	// MyMemory quota dialog shown by the engine (D_)
	D_TITRE, D_ANONYME, D_AVEC_EMAIL, D_LIBELLE, D_ENREGISTRER, D_PLUS_TARD, D_INVALIDE,
	// Session Report messages (R_)
	R_DEMARRAGE, R_INACTIF, R_REGLAGES, R_MEME_LANGUE, R_OUI, R_NON, R_MOTEUR_LANCE, R_MOTEUR_ARRETE,
	R_MOTEUR_INTROUVABLE, R_CLES_OUVERT, R_CORRECTIONS_OUVERT, R_INTERFACE, R_CLE_OK, R_CLE_A_SAISIR,
	R_EMAIL_OK, R_SANS_CLE, R_ADRESSE_OK, R_ADRESSE_A_SAISIR,
	NombreTextes	// number of texts (not a text)
};

// Number of interface languages (columns of the table).
constexpr int NombreLanguesInterface = 20;

// UTF-8 text 't' in the interface language of index 'langue' (0 = fr ... 19 = zh). An index out
// of range, or an empty cell, falls back to English; an invalid 't' gives "".
const char* Txt (Texte t, int langue);

// Same as Txt, then replaces every {key} placeholder by its value:
//   TxtF (R_DEMARRAGE, l, {{"v", "0.7.0"}, {"l", "..."}})
std::string TxtF (Texte t, int langue, std::initializer_list<std::pair<const char*, std::string>> valeurs);

// Number of empty cells in the table (checked by the test bench: must be 0).
int TextesManquants ();

// Language code (fr, en...) -> interface index; an unknown code (including "") -> English (1).
int IndiceInterface (const std::string& code);

// Code and name (written in that language) of the interface language of index i
// (out of range -> "en" / "English").
const char* CodeInterface (int i);
const char* NomInterface (int i);

// Menu resource IDs: base of the 5 menus of interface language i (+0 input language, +1 output
// language, +2 options, +3 online translator, +4 tool language); help strings are at +300.
// E.g. French menus are 32100..32104 (help 32400..32404), English menus 32110..32114.
// The ArchiCAD API takes resource IDs as 16-bit signed integers (short): every ID must stay
// below 32767, otherwise it wraps to a negative number, menu registration fails and ArchiCAD
// unloads the whole add-on (bug of v0.7.0, whose help strings were at 33500..33694).
constexpr short BaseMenus (int i) { return static_cast<short> (32100 + 10 * i); }
constexpr short DecalageAides = 300;
static_assert (32100 + 10 * (NombreLanguesInterface - 1) + 4 + DecalageAides <= 32767,
			   "menu help resource IDs must fit in a short (ArchiCAD API)");
static_assert (32100 + 10 * (NombreLanguesInterface - 1) + 4 < 32100 + DecalageAides,
			   "menu IDs and help IDs must not overlap");

// ArchiCAD version code (FRA, GER, INT, JPN...) -> language code (fr, de, en, ja...);
// unknown codes give "en".
std::string LangueDepuisArchicad (const std::string& version);

}	// namespace TT

#endif
