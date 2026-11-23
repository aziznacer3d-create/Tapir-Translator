// *****************************************************************************
// Tapir Translator - engine: online translators
//
//   The five providers the user can choose from (menu "Traducteur en ligne"),
//   each one implementing the Traducteur interface (Traducteur.hpp). Every
//   call is a synchronous HTTPS request made with Http::Requete (WinHTTP).
//
//   deepl           DeepL API v2 - a key ending in ":fx" is a Free key
//                   (api-free.deepl.com), any other key a Pro key (api.deepl.com)
//                   POST /v2/translate, header "Authorization: DeepL-Auth-Key <key>"
//                   body     {"text": [...], "target_lang": "EN-GB", "source_lang"?: "FR"}
//                   response {"translations": [{"text": "..."}, ...]}
//   microsoft       Microsoft Translator (Azure AI Translator, API v3)
//                   POST /translate?api-version=3.0&to=<l>[&from=<l>]
//                   headers "Ocp-Apim-Subscription-Key" (+ "-Region" if given)
//                   body     [{"Text": "..."}, ...]
//                   response [{"translations": [{"text": "..."}]}, ...]
//   google          Google Cloud Translation (API v2 "Basic")
//                   POST /language/translate/v2?key=<key>
//                   body     {"q": [...], "target": "en", "format": "text", "source"?: "fr"}
//                   response {"data": {"translations": [{"translatedText": "..."}, ...]}}
//   mymemory        MyMemory - no key; an optional e-mail ("de=") raises the
//                   free daily quota. One GET per text:
//                   GET /get?q=<text>&langpair=<src>|<dst>[&de=<email>]
//                   response {"responseData": {"translatedText": "..."}, "responseStatus": 200}
//                   A translatedText starting with "MYMEMORY WARNING:" means the
//                   quota is reached (MoteurMain.cpp then offers to enter an e-mail).
//   libretranslate  LibreTranslate - server URL (+ optional API key)
//                   POST <url>/translate
//                   body     {"q": [...], "source": "auto" | "fr", "target": "en", "format": "text", "api_key"?: "..."}
//                   response {"translatedText": [...]}  (or a single string for one text)
//
//   The keys come from cles.json, read again for every request (MoteurMain.cpp)
//   so they can be changed while the engine runs. Every provider must return
//   exactly one translation per input text, in the same order.
//   Test bench: the environment variable TAPIR_TRANSLATOR_URL_<NAME> (e.g.
//   TAPIR_TRANSLATOR_URL_DEEPL) replaces the service base URL, so a fake local
//   server can answer in each service's format.
// *****************************************************************************

#include "Traducteur.hpp"
#include "Http.hpp"
#include "Json.hpp"

#include <cstdlib>

namespace {

// Returns an ASCII upper-case copy of the string (used for DeepL language
// codes and environment variable names).
std::string Majuscules (std::string s)
{
	for (char& c : s)
		c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
	return s;
}

// Base URL of a service.
//   nom    : provider name ("deepl"...), upper-cased into TAPIR_TRANSLATOR_URL_<NOM>
//   defaut : official address of the service
// Returns the value of that environment variable when it is set and not
// empty (test bench), otherwise defaut.
std::string UrlService (const char* nom, const std::string& defaut)
{
	const std::string variable = std::string ("TAPIR_TRANSLATOR_URL_") + Majuscules (nom);
	char* v = nullptr;
	size_t n = 0;
	if (_dupenv_s (&v, &n, variable.c_str ()) == 0 && v != nullptr) {
		std::string r = v;
		std::free (v);
		if (!r.empty ())
			return r;
	}
	return defaut;
}

// Reads cles.json["<service>"]["<champ>"] as a string; "" when missing.
std::string Champ (const Json::Valeur& cles, const char* service, const char* champ)
{
	const Json::Valeur* s = cles.Champ (service);
	return s != nullptr ? s->Texte (champ) : std::string ();
}

// Serialises a list of texts as a JSON array of strings: ["a", "b"].
std::string TableauJson (const std::vector<std::string>& textes)
{
	std::string r = "[";
	for (size_t i = 0; i < textes.size (); ++i)
		r += (i == 0 ? "" : ", ") + Json::Chaine (textes[i]);
	return r + "]";
}

// Builds the error message of a failed call, written to journal.txt.
//   r : the HTTP response
// No response at all -> the network error from Http::Requete. Otherwise
// "HTTP <code> <meaning>: <detail>", where the detail is taken from the error
// formats of the services: {"message": ...} (DeepL), {"error": "..."}
// (LibreTranslate), {"error": {"message": ...}} (Microsoft, Google), or else
// the first 200 bytes of the body.
std::string MessageService (const Http::Reponse& r)
{
	if (r.code == 0)
		return r.erreur;
	// Step 1: detail extracted from the JSON body, if any
	std::string detail;
	Json::Valeur v;
	if (Json::Lecteur (r.corps).Lire (v)) {
		detail = v.Texte ("message");
		if (detail.empty ())
			detail = v.Texte ("error");
		if (const Json::Valeur* e = v.Champ ("error"))
			if (e->genre == Json::Valeur::Objet)
				detail = e->Texte ("message");
	}
	if (detail.empty ())
		detail = r.corps.substr (0, 200);
	// Step 2: plain meaning of the usual codes (456 = DeepL quota exceeded)
	std::string sens;
	if (r.code == 401 || r.code == 403)
		sens = "cle refusee : ";
	else if (r.code == 429)
		sens = "trop de requetes : ";
	else if (r.code == 456)
		sens = "quota mensuel atteint : ";
	return "HTTP " + std::to_string (r.code) + " " + sens + detail;
}

// Checks a service response and parses its JSON body.
//   r      : HTTP response
//   v      : output, the parsed body
//   erreur : output, filled when false is returned
// Returns true only for HTTP 200 with a valid JSON body.
bool Lire (const Http::Reponse& r, Json::Valeur& v, std::string& erreur)
{
	if (r.code != 200) {
		erreur = MessageService (r);
		return false;
	}
	if (!Json::Lecteur (r.corps).Lire (v)) {
		erreur = "reponse illisible : " + r.corps.substr (0, 200);
		return false;
	}
	return true;
}

// Common base of the online translators: they all accept the 20 ArchiCAD
// languages (each subclass maps the codes to the service's own codes).
class TraducteurEnLigne : public Traducteur {
public:
	bool Prend (const std::string& langue) const override { return LangueArchicad (langue); }
};


// --- DeepL -----------------------------------------------------------------------------
// DeepL API v2: the whole batch in one POST request.
class DeepL : public TraducteurEnLigne {
public:
	// cle : DeepL authentication key from cles.json ("deepl": {"cle": ...})
	explicit DeepL (std::string cle) : cle_ (std::move (cle)) {}
	std::string Nom () const override { return "DeepL"; }

	// DeepL target language code: DeepL requires a variant for English,
	// Portuguese and Chinese (British English, European Portuguese,
	// simplified Chinese); Norwegian is "NB" (Bokmal); other codes upper-cased.
	static std::string Cible (const std::string& l)
	{
		if (l == "en") return "EN-GB";
		if (l == "pt") return "PT-PT";
		if (l == "zh") return "ZH-HANS";
		if (l == "no") return "NB";
		return Majuscules (l);
	}
	// DeepL source language code: no variants for sources, only "NB" for Norwegian.
	static std::string Source (const std::string& l) { return l == "no" ? "NB" : Majuscules (l); }

	// Translates the batch (see Traducteur::Traduire).
	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		if (cle_.empty ()) {
			erreur = "cle DeepL absente (Options > Cles des traducteurs)";
			return false;
		}
		// Step 1: Free keys end with ":fx" and use a different host
		const bool gratuite = cle_.size () > 3 && cle_.compare (cle_.size () - 3, 3, ":fx") == 0;
		const std::string base = UrlService ("deepl", gratuite ? "https://api-free.deepl.com" : "https://api.deepl.com");
		// Step 2: JSON body; no "source_lang" = DeepL detects the language
		std::string corps = "{\"text\": " + TableauJson (textes) + ", \"target_lang\": " + Json::Chaine (Cible (cible));
		if (!source.empty ())
			corps += ", \"source_lang\": " + Json::Chaine (Source (source));
		corps += "}";
		// Step 3: send the request
		const Http::Reponse r = Http::Requete ("POST", base + "/v2/translate",
			{ "Authorization: DeepL-Auth-Key " + cle_, "Content-Type: application/json" }, corps);
		// Step 4: {"translations": [{"text": ...}]}, one entry per text
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		const Json::Valeur* t = v.Champ ("translations");
		if (t == nullptr || t->genre != Json::Valeur::Tableau || t->tableau.size () != textes.size ()) {
			erreur = "reponse DeepL incomplete";
			return false;
		}
		resultats.clear ();
		for (const Json::Valeur& e : t->tableau)
			resultats.push_back (e.Texte ("text"));
		return true;
	}

private:
	std::string cle_;
};


// --- Microsoft Translator ----------------------------------------------------------------
// Azure AI Translator, API v3: the whole batch in one POST request.
class Microsoft : public TraducteurEnLigne {
public:
	// cle    : subscription key ("microsoft": {"cle": ...})
	// region : Azure resource region ("microsoft": {"region": ...}), required
	//          by regional / multi-service resources, may be empty otherwise
	Microsoft (std::string cle, std::string region) : cle_ (std::move (cle)), region_ (std::move (region)) {}
	std::string Nom () const override { return "Microsoft Translator"; }

	// Microsoft language code: simplified Chinese, Norwegian Bokmal,
	// European Portuguese; other codes unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-Hans";
		if (l == "no") return "nb";
		if (l == "pt") return "pt-pt";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		if (cle_.empty ()) {
			erreur = "cle Microsoft absente (Options > Cles des traducteurs)";
			return false;
		}
		// Step 1: languages go in the query string; no "from" = auto-detection
		std::string url = UrlService ("microsoft", "https://api.cognitive.microsofttranslator.com") +
						  "/translate?api-version=3.0&to=" + Code (cible);
		if (!source.empty ())
			url += "&from=" + Code (source);
		// Step 2: body [{"Text": ...}, ...]
		std::string corps = "[";
		for (size_t i = 0; i < textes.size (); ++i)
			corps += std::string (i == 0 ? "" : ", ") + "{\"Text\": " + Json::Chaine (textes[i]) + "}";
		corps += "]";
		// Step 3: key header, plus the region header when a region is configured
		std::vector<std::string> entetes = { "Ocp-Apim-Subscription-Key: " + cle_, "Content-Type: application/json; charset=UTF-8" };
		if (!region_.empty ())
			entetes.push_back ("Ocp-Apim-Subscription-Region: " + region_);
		const Http::Reponse r = Http::Requete ("POST", url, entetes, corps);
		// Step 4: the response is an array with one element per text, each
		// holding "translations": [{"text": ...}] (an entry without it gives "")
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		if (v.genre != Json::Valeur::Tableau || v.tableau.size () != textes.size ()) {
			erreur = "reponse Microsoft incomplete";
			return false;
		}
		resultats.clear ();
		for (const Json::Valeur& e : v.tableau) {
			const Json::Valeur* t = e.Champ ("translations");
			resultats.push_back (t != nullptr && t->genre == Json::Valeur::Tableau && !t->tableau.empty () ? t->tableau[0].Texte ("text")
																												: std::string ());
		}
		return true;
	}

private:
	std::string cle_, region_;
};


// --- Google Cloud Translation (v2) -------------------------------------------------------
// Google Cloud Translation v2 ("Basic") with an API key: one POST request.
class Google : public TraducteurEnLigne {
public:
	// cle : API key ("google": {"cle": ...})
	explicit Google (std::string cle) : cle_ (std::move (cle)) {}
	std::string Nom () const override { return "Google Cloud Translation"; }

	// Google language code: simplified Chinese, European Portuguese; other
	// codes unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-CN";
		if (l == "pt") return "pt-PT";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		if (cle_.empty ()) {
			erreur = "cle Google absente (Options > Cles des traducteurs)";
			return false;
		}
		// Step 1: the key goes in the query string (URL-encoded)
		const std::string url = UrlService ("google", "https://translation.googleapis.com") + "/language/translate/v2?key=" +
								Http::EncoderUrl (cle_);
		// Step 2: body; "format": "text" so the texts are not treated as HTML
		// (no entity escaping); no "source" = auto-detection
		std::string corps = "{\"q\": " + TableauJson (textes) + ", \"target\": " + Json::Chaine (Code (cible)) + ", \"format\": \"text\"";
		if (!source.empty ())
			corps += ", \"source\": " + Json::Chaine (Code (source));
		corps += "}";
		const Http::Reponse r = Http::Requete ("POST", url, { "Content-Type: application/json; charset=utf-8" }, corps);
		// Step 3: {"data": {"translations": [{"translatedText": ...}]}}, one per text
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		const Json::Valeur* d = v.Champ ("data");
		const Json::Valeur* t = d != nullptr ? d->Champ ("translations") : nullptr;
		if (t == nullptr || t->genre != Json::Valeur::Tableau || t->tableau.size () != textes.size ()) {
			erreur = "reponse Google incomplete";
			return false;
		}
		resultats.clear ();
		for (const Json::Valeur& e : t->tableau)
			resultats.push_back (e.Texte ("translatedText"));
		return true;
	}

private:
	std::string cle_;
};


// --- MyMemory (one request per text) ------------------------------------------------------
// MyMemory free API: no key, one GET request per text.
class MyMemory : public TraducteurEnLigne {
public:
	// email : optional address ("mymemory": {"email": ...}) sent as "de=",
	//         which raises the free daily quota
	explicit MyMemory (std::string email) : email_ (std::move (email)) {}
	std::string Nom () const override { return "MyMemory"; }

	// MyMemory language code: simplified Chinese, European Portuguese; other
	// codes unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "zh") return "zh-CN";
		if (l == "pt") return "pt-PT";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		resultats.clear ();
		const std::string base = UrlService ("mymemory", "https://api.mymemory.translated.net");
		// The whole batch fails as soon as one text fails (the layer retries later)
		for (const std::string& texte : textes) {
			// Step 1: GET /get?q=<text>&langpair=<src>|<dst> ("Autodetect" when
			// the source is unknown), plus "&de=<email>" when configured
			std::string url = base + "/get?q=" + Http::EncoderUrl (texte) + "&langpair=" +
							  Http::EncoderUrl ((source.empty () ? std::string ("Autodetect") : Code (source)) + "|" + Code (cible));
			if (!email_.empty ())
				url += "&de=" + Http::EncoderUrl (email_);
			const Http::Reponse r = Http::Requete ("GET", url, {}, std::string ());
			// Step 2: {"responseData": {"translatedText": ...}, "responseStatus": 200}
			Json::Valeur v;
			if (!Lire (r, v, erreur))
				return false;
			const Json::Valeur* statut = v.Champ ("responseStatus");
			const Json::Valeur* donnees = v.Champ ("responseData");
			const std::string traduit = donnees != nullptr ? donnees->Texte ("translatedText") : std::string ();
			// Step 3: responseStatus may be a number or a string; a quota
			// warning arrives with HTTP 200 as the "translation" itself
			// (17 = length of "MYMEMORY WARNING:"). The error text keeps
			// "MYMEMORY WARNING" so MoteurMain.cpp can recognise it.
			const bool ok = statut != nullptr && ((statut->genre == Json::Valeur::Nombre && statut->nombre == 200.0) ||
												  (statut->genre == Json::Valeur::Chaine && statut->chaine == "200"));
			if (!ok || traduit.compare (0, 17, "MYMEMORY WARNING:") == 0) {
				erreur = "MyMemory : " + (traduit.empty () ? v.Texte ("responseDetails") : traduit.substr (0, 160));
				return false;
			}
			resultats.push_back (traduit);
		}
		return true;
	}

private:
	std::string email_;
};


// --- LibreTranslate ----------------------------------------------------------------------
// LibreTranslate (self-hosted or public server): one POST request for the batch.
class LibreTranslate : public TraducteurEnLigne {
public:
	// url : server address ("libretranslate": {"url": ...})
	// cle : API key if the server requires one ("libretranslate": {"cle": ...})
	LibreTranslate (std::string url, std::string cle) : url_ (std::move (url)), cle_ (std::move (cle)) {}
	std::string Nom () const override { return "LibreTranslate"; }

	// LibreTranslate language code: Norwegian Bokmal is "nb"; others unchanged.
	static std::string Code (const std::string& l)
	{
		if (l == "no") return "nb";
		return l;
	}

	bool Traduire (const std::string& source, const std::string& cible, const std::vector<std::string>& textes,
				   std::vector<std::string>& resultats, std::string& erreur) override
	{
		// Step 1: server address (no default: the user must configure one);
		// trailing slashes are removed before appending "/translate"
		std::string base = UrlService ("libretranslate", url_);
		if (base.empty ()) {
			erreur = "adresse du serveur LibreTranslate absente (Options > Cles des traducteurs)";
			return false;
		}
		while (!base.empty () && base.back () == '/')
			base.pop_back ();
		// Step 2: body; source "auto" = detection by the server
		std::string corps = "{\"q\": " + TableauJson (textes) + ", \"source\": " +
							Json::Chaine (source.empty () ? std::string ("auto") : Code (source)) + ", \"target\": " +
							Json::Chaine (Code (cible)) + ", \"format\": \"text\"";
		if (!cle_.empty ())
			corps += ", \"api_key\": " + Json::Chaine (cle_);
		corps += "}";
		const Http::Reponse r = Http::Requete ("POST", base + "/translate", { "Content-Type: application/json" }, corps);
		// Step 3: "translatedText" is an array for an array "q"; some servers
		// answer a single string, accepted when there is only one text
		Json::Valeur v;
		if (!Lire (r, v, erreur))
			return false;
		const Json::Valeur* t = v.Champ ("translatedText");
		resultats.clear ();
		if (t != nullptr && t->genre == Json::Valeur::Tableau && t->tableau.size () == textes.size ()) {
			for (const Json::Valeur& e : t->tableau)
				resultats.push_back (e.genre == Json::Valeur::Chaine ? e.chaine : std::string ());
			return true;
		}
		if (t != nullptr && t->genre == Json::Valeur::Chaine && textes.size () == 1) {
			resultats.push_back (t->chaine);
			return true;
		}
		erreur = "reponse LibreTranslate incomplete";
		return false;
	}

private:
	std::string url_, cle_;
};

}	// namespace


// Names of the online providers, as sent in "fournisseur" and listed by GET /etat.
const char* const FournisseursEnLigne[] = { "deepl", "microsoft", "google", "mymemory", "libretranslate" };

// Creates the online translator called nom, configured from cles.json.
//   nom  : one of FournisseursEnLigne
//   cles : parsed content of cles.json (may be null / empty: missing keys are
//          reported by Traduire, so the error reaches the log)
// Returns nullptr for an unknown name.
std::unique_ptr<Traducteur> CreerTraducteurEnLigne (const std::string& nom, const Json::Valeur& cles)
{
	if (nom == "deepl")
		return std::make_unique<DeepL> (Champ (cles, "deepl", "cle"));
	if (nom == "microsoft")
		return std::make_unique<Microsoft> (Champ (cles, "microsoft", "cle"), Champ (cles, "microsoft", "region"));
	if (nom == "google")
		return std::make_unique<Google> (Champ (cles, "google", "cle"));
	if (nom == "mymemory")
		return std::make_unique<MyMemory> (Champ (cles, "mymemory", "email"));
	if (nom == "libretranslate")
		return std::make_unique<LibreTranslate> (Champ (cles, "libretranslate", "url"), Champ (cles, "libretranslate", "cle"));
	return nullptr;
}
