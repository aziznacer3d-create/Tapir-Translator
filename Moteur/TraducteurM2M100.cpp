// *****************************************************************************
// Tapir Translator - moteur : traducteur reel M2M100 418M (hors ligne)
//
//   Modele : facebook/m2m100_418M (Meta, licence MIT), converti au format
//   CTranslate2 en int8 (ct2-transformers-converter --quantization int8).
//   Execution : CTranslate2 (MIT), calcul Ruy (Apache 2.0), sans MKL ni OpenMP.
//   Decoupage en morceaux de mots : SentencePiece (Apache 2.0).
//
//   Un seul reseau traduit directement entre ses 100 langues (fr en de es pt
//   it hu ar comprises), sans passer par l'anglais :
//     source = ["__fr__", morceaux..., "</s>"]   prefixe cible = ["__en__"]
//
//   Dossier du modele (--modele) : model.bin, config.json,
//   shared_vocabulary.json, sentencepiece.bpe.model
// *****************************************************************************

#include "Traducteur.hpp"

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace {

class TraducteurM2M100 : public Traducteur {
public:
	bool Charger (const std::string& dossier, std::string& erreur)
	{
		try {
			const auto etat = sp_.Load (dossier + "/sentencepiece.bpe.model");
			if (!etat.ok ()) {
				erreur = "SentencePiece : " + etat.ToString ();
				return false;
			}
			ctranslate2::models::ModelLoader chargeur (dossier);
			chargeur.device = ctranslate2::Device::CPU;
			chargeur.compute_type = ctranslate2::ComputeType::INT8;
			ctranslate2::ReplicaPoolConfig config;
			const unsigned coeurs = std::max (1u, std::thread::hardware_concurrency ());
			// la moitie des coeurs au plus : ArchiCAD tourne a cote
			config.num_threads_per_replica = static_cast<size_t> (std::clamp (coeurs / 2u, 1u, 8u));
			traducteur_ = std::make_unique<ctranslate2::Translator> (chargeur, config);
			return true;
		} catch (const std::exception& e) {
			erreur = std::string ("chargement du modele : ") + e.what ();
			return false;
		}
	}

	std::string Nom () const override { return "M2M100 418M int8 (CTranslate2, hors ligne)"; }

	bool Prend (const std::string& langue) const override { return LangueArchicad (langue); }

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		// Chaque texte est coupe en lignes : une ligne = une phrase a traduire.
		struct Ligne { size_t texte; std::string avant, apres, coeur; bool vide; };
		std::vector<Ligne> lignes;
		std::vector<std::vector<std::string>> lot;
		std::vector<std::vector<std::string>> prefixes;
		const std::string langueSource = source.empty () ? Deviner (textes) : source;
		for (size_t t = 0; t < textes.size (); ++t) {
			size_t debut = 0;
			const std::string& texte = textes[t];
			for (;;) {
				size_t fin = texte.find ('\n', debut);
				if (fin == std::string::npos)
					fin = texte.size ();
				std::string ligne = texte.substr (debut, fin - debut);
				Ligne l { t, "", "", "", true };
				const size_t a = ligne.find_first_not_of (" \t\r");
				if (a != std::string::npos) {
					const size_t b = ligne.find_last_not_of (" \t\r");
					l.avant = ligne.substr (0, a);
					l.apres = ligne.substr (b + 1);
					l.coeur = ligne.substr (a, b - a + 1);
					std::vector<std::string> morceaux;
					sp_.Encode (l.coeur, &morceaux);
					if (!morceaux.empty ()) {
						l.vide = false;
						std::vector<std::string> jetons;
						jetons.reserve (morceaux.size () + 2);
						jetons.push_back ("__" + langueSource + "__");
						jetons.insert (jetons.end (), morceaux.begin (), morceaux.end ());
						jetons.push_back ("</s>");
						lot.push_back (std::move (jetons));
						prefixes.push_back ({ "__" + cible + "__" });
					} else
						l.avant = ligne;
				} else
					l.avant = ligne;
				lignes.push_back (l);
				if (fin >= texte.size ())
					break;
				debut = fin + 1;
			}
		}

		std::vector<std::string> traduites;
		if (!lot.empty ()) {
			try {
				size_t plusLong = 0;
				for (const auto& j : lot)
					plusLong = std::max (plusLong, j.size ());
				ctranslate2::TranslationOptions options;
				options.beam_size = 5;
				options.max_decoding_length = std::min<size_t> (512, plusLong * 3 + 10);
				const auto sorties = traducteur_->translate_batch (lot, prefixes, options);
				for (const auto& s : sorties) {
					std::vector<std::string> jetons = s.output ();
					// le premier jeton est la langue cible imposee ; </s> et jetons speciaux retires
					std::vector<std::string> morceaux;
					for (size_t i = 0; i < jetons.size (); ++i) {
						const std::string& j = jetons[i];
						if (j.size () > 4 && j.compare (0, 2, "__") == 0 && j.compare (j.size () - 2, 2, "__") == 0)
							continue;
						if (j == "</s>" || j == "<s>" || j == "<pad>")
							continue;
						morceaux.push_back (j);
					}
					std::string texte;
					sp_.Decode (morceaux, &texte);
					traduites.push_back (texte);
				}
			} catch (const std::exception& e) {
				erreur = std::string ("traduction : ") + e.what ();
				return false;
			}
		}

		resultats.assign (textes.size (), std::string ());
		std::vector<bool> premiere (textes.size (), true);
		size_t k = 0;
		for (const Ligne& l : lignes) {
			std::string& r = resultats[l.texte];
			if (!premiere[l.texte])
				r += '\n';
			premiere[l.texte] = false;
			if (l.vide)
				r += l.avant;
			else
				r += l.avant + Harmoniser (l.coeur, k < traduites.size () ? traduites[k++] : std::string (), cible) + l.apres;
		}
		return true;
	}

private:
	// Le modele ajoute volontiers un point final et une majuscule : on suit l'original
	// ("Select at least one wall" -> pas de point ; "drawings not up to date" -> minuscule).
	static std::string Harmoniser (const std::string& source, std::string t, const std::string& cible)
	{
		if (source.empty () || t.empty ())
			return t;
		const auto finitPar = [] (const std::string& s, const char* f) {
			const size_t n = std::strlen (f);
			return s.size () >= n && s.compare (s.size () - n, n, f) == 0;
		};
		const bool sourcePoint = finitPar (source, ".") || finitPar (source, "\xE3\x80\x82");	// . ou 。
		if (!sourcePoint) {
			if (finitPar (t, "...") && !finitPar (source, "...")) {
			} else if (finitPar (t, ".") && !finitPar (t, "..."))
				t.pop_back ();
			else if (finitPar (t, "\xE3\x80\x82"))
				t.resize (t.size () - 3);
		}
		const unsigned char s0 = static_cast<unsigned char> (source[0]);
		const unsigned char t0 = static_cast<unsigned char> (t[0]);
		if (s0 >= 'a' && s0 <= 'z' && t0 >= 'A' && t0 <= 'Z' && cible != "de")
			t[0] = static_cast<char> (t0 - 'A' + 'a');
		else if (s0 >= 'A' && s0 <= 'Z' && t0 >= 'a' && t0 <= 'z')
			t[0] = static_cast<char> (t0 - 'a' + 'A');
		return t;
	}

	// Langue source inconnue (la couche n'a pas pu la reconnaitre) : d'apres l'alphabet,
	// sinon anglais. Cyrillique -> russe (l'ukrainien ne se distingue pas ici).
	static std::string Deviner (const std::vector<std::string>& textes)
	{
		for (const std::string& t : textes) {
			for (size_t i = 0; i < t.size (); ) {
				const unsigned char c = static_cast<unsigned char> (t[i]);
				unsigned long cp = c;
				size_t n = 1;
				if (c >= 0xF0 && i + 3 < t.size ()) { cp = ((c & 0x07ul) << 18) | ((t[i + 1] & 0x3Ful) << 12) | ((t[i + 2] & 0x3Ful) << 6) | (t[i + 3] & 0x3Ful); n = 4; }
				else if (c >= 0xE0 && i + 2 < t.size ()) { cp = ((c & 0x0Ful) << 12) | ((t[i + 1] & 0x3Ful) << 6) | (t[i + 2] & 0x3Ful); n = 3; }
				else if (c >= 0xC0 && i + 1 < t.size ()) { cp = ((c & 0x1Ful) << 6) | (t[i + 1] & 0x3Ful); n = 2; }
				i += n;
				if (cp >= 0x0370 && cp <= 0x03FF) return "el";
				if (cp >= 0x0400 && cp <= 0x04FF) return "ru";
				if (cp >= 0x3040 && cp <= 0x30FF) return "ja";
				if (cp >= 0xAC00 && cp <= 0xD7AF) return "ko";
				if (cp >= 0x4E00 && cp <= 0x9FFF) return "zh";
			}
		}
		return "en";
	}

	sentencepiece::SentencePieceProcessor sp_;
	std::unique_ptr<ctranslate2::Translator> traducteur_;
};

}	// namespace


std::unique_ptr<Traducteur> CreerTraducteurM2M100 (const std::string& dossier, std::string& erreur)
{
	auto t = std::make_unique<TraducteurM2M100> ();
	if (!t->Charger (dossier, erreur))
		return nullptr;
	return t;
}
