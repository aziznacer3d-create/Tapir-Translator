// *****************************************************************************
// Tapir Translator - engine: offline translator M2M100 418M (optional)
//
//   Only compiled with the build option TT_LOCAL_MODEL=ON (off by default:
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
//   Model folder (--model): model.bin, config.json,
//   shared_vocabulary.json, sentencepiece.bpe.model
// *****************************************************************************

#include "Translator.hpp"

#include <ctranslate2/translator.h>
#include <sentencepiece_processor.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace {

// M2M100 translator running on the CPU with CTranslate2.
class TranslatorM2M100 : public Translator {
public:
	// Loads the tokeniser and the model from the folder.
	//   folder : model folder (see the file header)
	//   error  : output, the reason when false is returned
	// Step 1: SentencePiece model. Step 2: CTranslate2 model in int8 on the CPU,
	// with at most half of the cores (1 to 8 threads).
	bool Load (const std::string& folder, std::string& error)
	{
		try {
			const auto status = sp_.Load (folder + "/sentencepiece.bpe.model");
			if (!status.ok ()) {
				error = "SentencePiece : " + status.ToString ();
				return false;
			}
			ctranslate2::models::ModelLoader loader (folder);
			loader.device = ctranslate2::Device::CPU;
			loader.compute_type = ctranslate2::ComputeType::INT8;
			ctranslate2::ReplicaPoolConfig config;
			const unsigned cores = std::max (1u, std::thread::hardware_concurrency ());
			// At most half of the cores: ArchiCAD is running next to the engine
			config.num_threads_per_replica = static_cast<size_t> (std::clamp (cores / 2u, 1u, 8u));
			translator_ = std::make_unique<ctranslate2::Translator> (loader, config);
			return true;
		} catch (const std::exception& e) {
			error = std::string ("loading the model: ") + e.what ();
			return false;
		}
	}

	std::string Name () const override { return "M2M100 418M int8 (CTranslate2, offline)"; }

	bool Supports (const std::string& language) const override { return ArchicadLanguage (language); }

	// Translates the batch (see Translator::Translate).
	// Step 1: split every text into lines (one line = one sentence) and turn
	//         each non-blank line into tokens for one batch.
	// Step 2: translate the whole batch at once (beam search).
	// Step 3: rebuild every text line by line, with its original spacing.
	bool Translate (const std::string& source, const std::string& target, const std::vector<std::string>& texts,
				   std::vector<std::string>& results, std::string& error) override
	{
		// Step 1: each text is split into lines: one line = one sentence to translate.
		// A line keeps its index of text, its leading / trailing whitespace
		// (before / after) and its trimmed content (core); "empty_" = nothing to
		// translate (the line is then copied as it is, in "before").
		struct Line { size_t text; std::string before, after, core; bool empty_; };
		std::vector<Line> lines;
		std::vector<std::vector<std::string>> lot;
		std::vector<std::vector<std::string>> prefixes;
		// Unknown source language: guessed from the script (alphabet) of the texts
		const std::string sourceLanguage = source.empty () ? Guess (texts) : source;
		for (size_t t = 0; t < texts.size (); ++t) {
			size_t start = 0;
			const std::string& text = texts[t];
			for (;;) {
				size_t end_ = text.find ('\n', start);
				if (end_ == std::string::npos)
					end_ = text.size ();
				std::string line = text.substr (start, end_ - start);
				Line l { t, "", "", "", true };
				// Separate the surrounding whitespace from the content
				const size_t a = line.find_first_not_of (" \t\r");
				if (a != std::string::npos) {
					const size_t b = line.find_last_not_of (" \t\r");
					l.before = line.substr (0, a);
					l.after = line.substr (b + 1);
					l.core = line.substr (a, b - a + 1);
					// Sub-word pieces, framed by the source language token and
					// the end-of-sentence token; the target language token is the
					// forced first token of the output (prefix)
					std::vector<std::string> pieces;
					sp_.Encode (l.core, &pieces);
					if (!pieces.empty ()) {
						l.empty_ = false;
						std::vector<std::string> tokens;
						tokens.reserve (pieces.size () + 2);
						tokens.push_back ("__" + sourceLanguage + "__");
						tokens.insert (tokens.end (), pieces.begin (), pieces.end ());
						tokens.push_back ("</s>");
						lot.push_back (std::move (tokens));
						prefixes.push_back ({ "__" + target + "__" });
					} else
						l.before = line;
				} else
					l.before = line;
				lines.push_back (l);
				if (end_ >= text.size ())
					break;
				start = end_ + 1;
			}
		}

		// Step 2: translate all the lines in one batch, in the order of "lot"
		std::vector<std::string> translatedTexts;
		if (!lot.empty ()) {
			try {
				// Output length limit: 3 x the longest input + 10 tokens, at most 512
				size_t longest = 0;
				for (const auto& j : lot)
					longest = std::max (longest, j.size ());
				ctranslate2::TranslationOptions options;
				options.beam_size = 5;
				options.max_decoding_length = std::min<size_t> (512, longest * 3 + 10);
				const auto outputs = translator_->translate_batch (lot, prefixes, options);
				for (const auto& s : outputs) {
					std::vector<std::string> tokens = s.output ();
					// The first token is the forced target language ("__xx__"); it is
					// removed with </s>, <s>, <pad>, then the pieces are decoded to text
					std::vector<std::string> pieces;
					for (size_t i = 0; i < tokens.size (); ++i) {
						const std::string& j = tokens[i];
						if (j.size () > 4 && j.compare (0, 2, "__") == 0 && j.compare (j.size () - 2, 2, "__") == 0)
							continue;
						if (j == "</s>" || j == "<s>" || j == "<pad>")
							continue;
						pieces.push_back (j);
					}
					std::string text;
					sp_.Decode (pieces, &text);
					translatedTexts.push_back (text);
				}
			} catch (const std::exception& e) {
				error = std::string ("translation: ") + e.what ();
				return false;
			}
		}

		// Step 3: rebuild each text from its lines, in order: lines joined with
		// '\n', blank lines copied, translated lines put back between their
		// original whitespace after MatchStyle (k walks through "translatedTexts",
		// which has one entry per non-blank line)
		results.assign (texts.size (), std::string ());
		std::vector<bool> first_ (texts.size (), true);
		size_t k = 0;
		for (const Line& l : lines) {
			std::string& r = results[l.text];
			if (!first_[l.text])
				r += '\n';
			first_[l.text] = false;
			if (l.empty_)
				r += l.before;
			else
				r += l.before + MatchStyle (l.core, k < translatedTexts.size () ? translatedTexts[k++] : std::string (), target) + l.after;
		}
		return true;
	}

private:
	// The model readily adds a final full stop and an initial capital: the
	// translation follows the original instead
	// ("Select at least one wall" -> no full stop; "drawings not up to date" -> lower case).
	//   source : original line (trimmed)
	//   t      : its translation
	//   target  : target language (German keeps its capitals: nouns are capitalised)
	// Returns the adjusted translation.
	static std::string MatchStyle (const std::string& source, std::string t, const std::string& target)
	{
		if (source.empty () || t.empty ())
			return t;
		const auto endsWith = [] (const std::string& s, const char* f) {
			const size_t n = std::strlen (f);
			return s.size () >= n && s.compare (s.size () - n, n, f) == 0;
		};
		// Step 1: final full stop. If the original does not end with "." or the
		// CJK full stop, remove a single final "." (or the CJK one) from the
		// translation; an ellipsis "..." is kept.
		const bool sourceDot = endsWith (source, ".") || endsWith (source, "\xE3\x80\x82");	// "." or the CJK full stop U+3002
		if (!sourceDot) {
			if (endsWith (t, "...") && !endsWith (source, "...")) {
			} else if (endsWith (t, ".") && !endsWith (t, "..."))
				t.pop_back ();
			else if (endsWith (t, "\xE3\x80\x82"))
				t.resize (t.size () - 3);
		}
		// Step 2: initial letter (ASCII only) gets the case of the original's
		// first letter; never lowered for German
		const unsigned char s0 = static_cast<unsigned char> (source[0]);
		const unsigned char t0 = static_cast<unsigned char> (t[0]);
		if (s0 >= 'a' && s0 <= 'z' && t0 >= 'A' && t0 <= 'Z' && target != "de")
			t[0] = static_cast<char> (t0 - 'A' + 'a');
		else if (s0 >= 'A' && s0 <= 'Z' && t0 >= 'a' && t0 <= 'z')
			t[0] = static_cast<char> (t0 - 'a' + 'A');
		return t;
	}

	// Unknown source language (the layer could not recognise it): guessed from
	// the first character found in one of the scripts below, otherwise English.
	// Greek -> el, Cyrillic -> ru (Ukrainian cannot be told apart here),
	// Hiragana / Katakana -> ja, Hangul -> ko, CJK ideographs -> zh.
	static std::string Guess (const std::vector<std::string>& texts)
	{
		for (const std::string& t : texts) {
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
	std::unique_ptr<ctranslate2::Translator> translator_;	// loaded model
};

}	// namespace


// Creates and loads the offline translator; nullptr (with error) on failure.
std::unique_ptr<Translator> CreateTranslatorM2M100 (const std::string& folder, std::string& error)
{
	auto t = std::make_unique<TranslatorM2M100> ();
	if (!t->Load (folder, error))
		return nullptr;
	return t;
}
