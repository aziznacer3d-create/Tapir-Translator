// *****************************************************************************
// Tapir Translator - textes de l'interface de l'outil, dans les 20 langues d'ArchiCAD
//
//   Menus (generes en ressources par Tools/GenererMenus.cpp), aides de la barre
//   d'etat, fenetre du quota MyMemory, messages du Rapport de session. Partage
//   par l'add-on, le moteur et le generateur de menus (aucune dependance).
//
//   Langue de l'interface : celle d'ArchiCAD par defaut (code de la version :
//   FRA, GER, ITA...), ou celle choisie dans le menu "Langue de Tapir Translator".
//   Ordre des colonnes = TT::Langues[1..20] : fr en de es pt it hu cs pl ru uk el
//   tr nl sv no fi ja ko zh. Traductions a faire relire par des locuteurs natifs.
// *****************************************************************************

#ifndef TT_TEXTES_HPP
#define TT_TEXTES_HPP

#include <initializer_list>
#include <string>
#include <utility>

namespace TT {

enum Texte : int {
	// menus
	M_ENTREE, M_AUTO, M_SORTIE, M_AUCUNE, M_TRADUCTEUR, M_SANS_CLE, M_CLES, M_CORRECTIONS,
	M_OPTIONS, M_EN_LIGNE, M_COLLECTE, M_DOSSIER, M_ETAT, M_INTERFACE, M_INTERFACE_AUTO,
	// aides (barre d'etat)
	P_AUTO, P_ENTREE, P_AUCUNE, P_SORTIE, P_TRADUCTEUR, P_CLES, P_CORRECTIONS, P_EN_LIGNE,
	P_COLLECTE, P_DOSSIER, P_ETAT, P_INTERFACE,
	// fenetre du quota MyMemory
	D_TITRE, D_ANONYME, D_AVEC_EMAIL, D_LIBELLE, D_ENREGISTRER, D_PLUS_TARD, D_INVALIDE,
	// Rapport de session
	R_DEMARRAGE, R_INACTIF, R_REGLAGES, R_MEME_LANGUE, R_OUI, R_NON, R_MOTEUR_LANCE, R_MOTEUR_ARRETE,
	R_MOTEUR_INTROUVABLE, R_CLES_OUVERT, R_CORRECTIONS_OUVERT, R_INTERFACE, R_CLE_OK, R_CLE_A_SAISIR,
	R_EMAIL_OK, R_SANS_CLE, R_ADRESSE_OK, R_ADRESSE_A_SAISIR,
	NombreTextes
};

constexpr int NombreLanguesInterface = 20;

// Texte UTF-8 dans la langue d'indice 'langue' (0 = fr ... 19 = zh ; hors bornes -> anglais)
const char* Txt (Texte t, int langue);

// Remplace {cle} par sa valeur : TxtF (R_DEMARRAGE, l, {{"v", "0.7.0"}, {"l", "..."}})
std::string TxtF (Texte t, int langue, std::initializer_list<std::pair<const char*, std::string>> valeurs);

// Cases vides de la table (banc : doit valoir 0)
int TextesManquants ();

// Code de langue (fr, en...) -> indice d'interface ; inconnu -> anglais (1)
int IndiceInterface (const std::string& code);

// Code et nom (dans sa propre langue) de la langue d'interface d'indice i
const char* CodeInterface (int i);
const char* NomInterface (int i);

// Identifiants des menus : base des 5 menus de la langue i (+0 entree, +1 sortie,
// +2 options, +3 traducteur, +4 langue de l'outil) ; les aides sont a +1000.
constexpr short BaseMenus (int i) { return static_cast<short> (32500 + 10 * i); }
constexpr short DecalageAides = 1000;

// Code de version d'ArchiCAD (FRA, GER, INT, JPN...) -> code de langue (fr, de, en, ja...)
std::string LangueDepuisArchicad (const std::string& version);

}	// namespace TT

#endif
