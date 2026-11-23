// *****************************************************************************
// Tapir Translator - engine: offline translator M2M100 418M (optional)
//
//   Only compiled with the build option TT_MODELE_LOCAL=ON (off by default:
//   the model makes the .apx about 500 MB); provider name "local".
//
//   Model: facebook/m2m100_418M (Meta, MIT licence), converted to the
//   CTranslate2 format in int8 (ct2-transformers-converter --quantization int8).
//   Runtime: CTranslate2 (MIT), Ruy compute backend (Apache 2.0), no MKL nor
//   OpenMP. Sub-word tokenisation: SentencePiece (Apache 2.0).
//
//   A single network translates directly between its 100 languages (the 20
//   ArchiCAD languages included), without going through English. The
//   language is given by special tokens:
//     source = ["__fr__", pieces..., "</s>"]   target prefix = ["__en__"]
//
//   Model folder (--modele): model.bin, config.json,
//   shared_vocabulary.json, sentencepiece.bpe.model
// *****************************************************************************

#include "Traducteur.hpp"

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace {

// M2M100 translator running on the CPU with CTranslate2.
class TraducteurM2M100 : public Traducteur {
public:
	// Loads the tokeniser and the model from the folder.
	//   dossier : model folder (see the file header)
	//   erreur  : output, the reason when false is returned
	// Step 1: SentencePiece model. Step 2: CTranslate2 model in int8 on the CPU,
	// with at most half of the cores (1 to 8 threads).
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
			// At most half of the cores: ArchiCAD is running next to the engine
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

	// Translates the batch (see Traducteur::Traduire).
	// Step 1: split every text into lines (one line = one sentence) and turn
	//         each non-blank line into tokens for one batch.
	// Step 2: translate the whole batch at once (beam search).
	// Step 3: rebuild every text line by line, with its original spacing.
	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		// Step 1: each text is split into lines: one line = one sentence to translate.
		// A line keeps its index of text, its leading / trailing whitespace
		// (avant / apres) and its trimmed content (coeur); "vide" = nothing to
		// translate (the line is then copied as it is, in "avant").
		struct Ligne { size_t texte; std::string avant, apres, coeur; bool vide; };
		std::vector<Ligne> lignes;
		std::vector<std::vector<std::string>> lot;
		std::vector<std::vector<std::string>> prefixes;
		// Unknown source language: guessed from the script (alphabet) of the texts
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
				// Separate the surrounding whitespace from the content
				const size_t a = ligne.find_first_not_of (" \t\r");
				if (a != std::string::npos) {
					const size_t b = ligne.find_last_not_of (" \t\r");
					l.avant = ligne.substr (0, a);
					l.apres = ligne.substr (b + 1);
					l.coeur = ligne.substr (a, b - a + 1);
					// Sub-word pieces, framed by the source language token and
					// the end-of-sentence token; the target language token is the
					// forced first token of the output (prefix)
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

		// Step 2: translate all the lines in one batch, in the order of "lot"
		std::vector<std::string> traduites;
		if (!lot.empty ()) {
			try {
				// Output length limit: 3 x the longest input + 10 tokens, at most 512
				size_t plusLong = 0;
				for (const auto& j : lot)
					plusLong = std::max (plusLong, j.size ());
				ctranslate2::TranslationOptions options;
				options.beam_size = 5;
				options.max_decoding_length = std::min<size_t> (512, plusLong * 3 + 10);
				const auto sorties = traducteur_->translate_batch (lot, prefixes, options);
				for (const auto& s : sorties) {
					std::vector<std::string> jetons = s.output ();
					// The first token is the forced target language ("__xx__"); it is
					// removed with </s>, <s>, <pad>, then the pieces are decoded to text
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

		// Step 3: rebuild each text from its lines, in order: lines joined with
		// '\n', blank lines copied, translated lines put back between their
		// original whitespace after Harmoniser (k walks through "traduites",
		// which has one entry per non-blank line)
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
	// The model readily adds a final full stop and an initial capital: the
	// translation follows the original instead
	// ("Select at least one wall" -> no full stop; "drawings not up to date" -> lower case).
	//   source : original line (trimmed)
	//   t      : its translation
	//   cible  : target language (German keeps its capitals: nouns are capitalised)
	// Returns the adjusted translation.
	static std::string Harmoniser (const std::string& source, std::string t, const std::string& cible)
	{
		if (source.empty () || t.empty ())
			return t;
		const auto finitPar = [] (const std::string& s, const char* f) {
			const size_t n = std::strlen (f);
			return s.size () >= n && s.compare (s.size () - n, n, f) == 0;
		};
		// Step 1: final full stop. If the original does not end with "." or the
		// CJK full stop, remove a single final "." (or the CJK one) from the
		// translation; an ellipsis "..." is kept.
		const bool sourcePoint = finitPar (source, ".") || finitPar (source, "\xE3\x80\x82");	// "." or the CJK full stop U+3002
		if (!sourcePoint) {
			if (finitPar (t, "...") && !finitPar (source, "...")) {
			} else if (finitPar (t, ".") && !finitPar (t, "..."))
				t.pop_back ();
			else if (finitPar (t, "\xE3\x80\x82"))
				t.resize (t.size () - 3);
		}
		// Step 2: initial letter (ASCII only) gets the case of the original's
		// first letter; never lowered for German
		const unsigned char s0 = static_cast<unsigned char> (source[0]);
		const unsigned char t0 = static_cast<unsigned char> (t[0]);
		if (s0 >= 'a' && s0 <= 'z' && t0 >= 'A' && t0 <= 'Z' && cible != "de")
			t[0] = static_cast<char> (t0 - 'A' + 'a');
		else if (s0 >= 'A' && s0 <= 'Z' && t0 >= 'a' && t0 <= 'z')
			t[0] = static_cast<char> (t0 - 'a' + 'A');
		return t;
	}

	// Unknown source language (the layer could not recognise it): guessed from
	// the first character found in one of the scripts below, otherwise English.
	// Greek -> el, Cyrillic -> ru (Ukrainian cannot be told apart here),
	// Hiragana / Katakana -> ja, Hangul -> ko, CJK ideographs -> zh.
	static std::string Deviner (const std::vector<std::string>& textes)
	{
		for (const std::string& t : textes) {
			for (size_t i = 0; i < t.size (); ) {
				// Decode one UTF-8 sequence (4, 3, 2 or 1 byte) into the code point cp
				const unsigned char c = static_cast<unsigned char> (t[i]);
				unsigned long cp = c;
				size_t n = 1;
				if (c >= 0xF0 && i + 3 < t.size ()) { cp = ((c & 0x07ul) << 18) | ((t[i + 1] & 0x3Ful) << 12) | ((t[i + 2] & 0x3Ful) << 6) | (t[i + 3] & 0x3Ful); n = 4; }
				else if (c >= 0xE0 && i + 2 < t.size ()) { cp = ((c & 0x0Ful) << 12) | ((t[i + 1] & 0x3Ful) << 6) | (t[i + 2] & 0x3Ful); n = 3; }
				else if (c >= 0xC0 && i + 1 < t.size ()) { cp = ((c & 0x1Ful) << 6) | (t[i + 1] & 0x3Ful); n = 2; }
				i += n;
				// Unicode blocks of the scripts
				if (cp >= 0x0370 && cp <= 0x03FF) return "el";
				if (cp >= 0x0400 && cp <= 0x04FF) return "ru";
				if (cp >= 0x3040 && cp <= 0x30FF) return "ja";
				if (cp >= 0xAC00 && cp <= 0xD7AF) return "ko";
				if (cp >= 0x4E00 && cp <= 0x9FFF) return "zh";
			}
		}
		return "en";
	}

	sentencepiece::SentencePieceProcessor sp_;				// sub-word tokeniser
	std::unique_ptr<ctranslate2::Translator> traducteur_;	// loaded model
};

}	// namespace


// Creates and loads the offline translator; nullptr (with erreur) on failure.
std::unique_ptr<Traducteur> CreerTraducteurM2M100 (const std::string& dossier, std::string& erreur)
{
	auto t = std::make_unique<TraducteurM2M100> ();
	if (!t->Charger (dossier, erreur))
		return nullptr;
	return t;
}
